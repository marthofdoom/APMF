#include "PCH.h"
#include "core/Sources.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <shared_mutex>

// Win32, declared by hand (the PCH does not pull in <Windows.h>; the same one-line
// imports core/EquipSink.cpp uses). REX::W32 (the fork's KERNEL32.h) has no
// GetModuleHandleEx / WritePrivateProfileString / GetPrivateProfileInt.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);
extern "C" __declspec(dllimport) int __stdcall WritePrivateProfileStringA(
    const char* a_appName, const char* a_keyName, const char* a_string, const char* a_fileName);
extern "C" __declspec(dllimport) int __stdcall GetModuleHandleExA(
    unsigned long a_flags, const char* a_moduleName, void** a_outModule);

// See core/Sources.h for the model, the degrade rule, the census and the threading.

namespace apmf::sources {

    namespace {

        using apmf::log::Hex;

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF_Sources.ini";
        constexpr std::size_t kMaxSources = 256;
        constexpr std::size_t kNameMax    = 64;
        constexpr std::uint64_t kCensusPeriodMs = 60000;   // plan: "every 60 s on the main-thread pump"
        constexpr std::uint64_t kUnattribLogMs  = 5000;
        constexpr std::size_t   kAlertRing      = 32;
        constexpr std::uint32_t kIdleLoads      = 3;       // Q14: "3 loads"

        // 0x6 = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | _UNCHANGED_REFCOUNT (EquipSink.cpp precedent)
        constexpr unsigned long kFromAddress = 0x6;

        struct Source {
            char          name[kNameMax]{};
            Kind          kind = Kind::kUnknown;
            std::atomic<int>           prio[kFacetCount]{};
            std::atomic<std::uint32_t> hits[kFacetCount]{};
            std::atomic<std::uint32_t> overruled[kFacetCount]{};
            std::atomic<std::uint32_t> stood[kFacetCount]{};
            std::atomic<std::uint64_t> lastSeenMs[kFacetCount]{};
            std::atomic<std::uint32_t> involved{ 0 };          // facet bitmask
            std::atomic<std::uint32_t> lastLoadSeq{ 0 };       // the load sequence it was last seen in
        };

        Source                     g_src[kMaxSources];
        std::atomic<std::uint32_t> g_count{ 0 };
        std::mutex                 g_addMx;
        std::atomic<bool>          g_fullLogged{ false };
        std::atomic<bool>          g_inited{ false };
        std::atomic<std::uint32_t> g_loadSeq{ 1 };

        // INI, read once at Init (main thread) and kept for Register(): per facet,
        // lower-case key -> value; plus the [Defaults] per facet.
        std::unordered_map<std::string, int> g_iniPrio[kFacetCount];
        // Filled with the built-in defaults so a Register() that runs before Init() (a client
        // calling the API early) never reads 0 (= off); Init() then applies the INI.
        template <int V>
        constexpr std::array<int, kFacetCount> Filled() {
            std::array<int, kFacetCount> a{};
            for (auto& x : a) x = V;
            return a;
        }
        std::array<int, kFacetCount> g_defUnreg  = Filled<kDefaultUnregistered>();
        std::array<int, kFacetCount> g_defClient = Filled<kDefaultClient>();

        // clients: handle -> {source, issued}
        struct HandleRec { SourceId src; std::uint64_t issuedMs; };
        std::shared_mutex                                 g_handleMx;
        std::unordered_map<APMF_API::Handle, HandleRec>   g_handles;
        constexpr std::size_t kHandleCap = 1u << 16;
        // module base -> source, so a client request does not resolve its module name every time
        std::mutex                                         g_modCacheMx;
        std::unordered_map<std::uintptr_t, SourceId>       g_modCache;

        // alerts
        std::mutex                 g_alertMx;
        Alert                      g_alerts[kAlertRing];
        std::size_t                g_alertNext = 0;
        std::atomic<std::uint64_t> g_alertTotal{ 0 };
        std::atomic<std::uint64_t> g_unattribLastLog[kFacetCount]{};
        std::atomic<std::uint32_t> g_unattribSuppressed[kFacetCount]{};

        // census (MAIN thread only, except the published snapshot)
        struct Seat {
            std::string   seat;
            Facet         facet = Facet::kCore;
            bool          callSite = false;
            std::uintptr_t where = 0;    // vtable entry address, or the E8 address
            std::uintptr_t ours  = 0;    // the value Harbinger wrote (thunk, or the E8's target)
            SourceId      prev = kNoSource;
            SourceId      late = kNoSource;
            std::uintptr_t lastSeen = 0;
            bool          lostOutermost = false;
            int           rehooks = 0;
            std::function<std::uintptr_t(std::uintptr_t)> rehook;
        };
        std::vector<Seat>                               g_seats;   // main thread
        std::mutex                                      g_seatPubMx;
        std::shared_ptr<const std::vector<SeatView>>    g_seatPub = std::make_shared<std::vector<SeatView>>();
        std::atomic<std::uint32_t>                      g_seatCount{ 0 };
        std::uint64_t                                   g_lastCensusMs = 0;   // main thread

        // the menu's write path
        struct PrioOp { SourceId id; Facet f; int prio; };
        std::mutex          g_opMx;
        std::vector<PrioOp> g_ops;
        std::atomic<bool>   g_opsPending{ false };

        bool Valid(SourceId id) {
            return id >= 0 && static_cast<std::uint32_t>(id) < g_count.load(std::memory_order_acquire);
        }

        std::string Lower(std::string_view s) {
            std::string r(s);
            for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return r;
        }

        bool IEquals(std::string_view a, const char* b) {
            const std::size_t n = std::strlen(b);
            if (a.size() != n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        int Clamp(int v) { return std::clamp(v, kPrioMin, kPrioMax); }

        int DefaultFor(Kind k, Facet f) {
            const int i = static_cast<int>(f);
            switch (k) {
            case Kind::kClient: return g_defClient[i];
            case Kind::kDirect: return kDefaultDirect;
            default:            return g_defUnreg[i];
            }
        }

        // The INI value for (facet, name), else the kind's default.
        int InitialPriority(std::string_view name, Kind k, Facet f) {
            const auto& m  = g_iniPrio[static_cast<int>(f)];
            const auto  it = m.find(Lower(name));
            return it != m.end() ? Clamp(it->second) : DefaultFor(k, f);
        }

        void ReadIni() {
            for (int i = 0; i < kFacetCount; ++i) {
                const Facet f    = static_cast<Facet>(i);
                const auto  sect = FacetName(f);
                g_defUnreg[i]  = Clamp(static_cast<int>(GetPrivateProfileIntA(
                    "Defaults", fmt::format("iUnregistered{}", sect).c_str(), kDefaultUnregistered, kIni)));
                g_defClient[i] = Clamp(static_cast<int>(GetPrivateProfileIntA(
                    "Defaults", fmt::format("iClient{}", sect).c_str(), kDefaultClient, kIni)));
                g_iniPrio[i].clear();
                // A null key enumerates the section's key names, NUL-separated, double-NUL-terminated
                // (REX::W32::GetPrivateProfileStringA, the fork's KERNEL32.h).
                std::vector<char> buf(16384, '\0');
                const auto n = REX::W32::GetPrivateProfileStringA(sect, nullptr, "", buf.data(),
                                                                   static_cast<std::uint32_t>(buf.size()), kIni);
                for (std::size_t p = 0; p < n && buf[p];) {
                    const std::string key(buf.data() + p);
                    p += key.size() + 1;
                    const long v = static_cast<long>(GetPrivateProfileIntA(sect, key.c_str(), -1, kIni));
                    if (v < 0) continue;   // not a number, or negative: ignored (the default applies)
                    g_iniPrio[i][Lower(key)] = static_cast<int>(v);
                }
            }
        }

        SourceId AddLocked(std::string_view name, Kind kind) {
            const auto n = g_count.load(std::memory_order_relaxed);
            for (std::uint32_t i = 0; i < n; ++i)
                if (IEquals(name, g_src[i].name)) return static_cast<SourceId>(i);
            if (n >= kMaxSources) {
                if (!g_fullLogged.exchange(true))
                    spdlog::error("[sources] the source table is FULL ({} entries) -- '{}' and every later source "
                                  "are not tracked (their writes count as Unattributed and are never denied).",
                                  kMaxSources, name);
                return kNoSource;
            }
            Source& s = g_src[n];
            const auto len = std::min(name.size(), kNameMax - 1);
            std::memcpy(s.name, name.data(), len);
            s.name[len] = '\0';
            s.kind = kind;
            for (int f = 0; f < kFacetCount; ++f)
                s.prio[f].store(InitialPriority(s.name, kind, static_cast<Facet>(f)), std::memory_order_relaxed);
            g_count.store(n + 1, std::memory_order_release);   // publish: name/kind/prio written above
            spdlog::info("[sources] new source '{}' ({}). Priorities: Target {}, Detection {}, Package {}, Equip {} "
                         "(0-100, APMF_Sources.ini).",
                         s.name, KindName(kind), s.prio[0].load(), s.prio[1].load(), s.prio[2].load(), s.prio[3].load());
            return static_cast<SourceId>(n);
        }

        const char* BaseName(const char* path) {
            const char* b = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') b = p + 1;
            return b;
        }

        void* ModuleOf(std::uintptr_t addr) {
            void* mod = nullptr;
            if (!addr || !GetModuleHandleExA(kFromAddress, reinterpret_cast<const char*>(addr), &mod)) return nullptr;
            return mod;
        }

        void* SelfModule() {
            static void* s = ModuleOf(reinterpret_cast<std::uintptr_t>(&SelfModule));
            return s;
        }

        void PublishSeats() {
            auto v = std::make_shared<std::vector<SeatView>>();
            for (const auto& s : g_seats) {
                if (s.prev == kNoSource && s.late == kNoSource && !s.lostOutermost) continue;
                v->push_back(SeatView{ s.seat, s.facet, s.callSite, s.prev, s.late, s.lostOutermost, s.rehooks });
            }
            std::scoped_lock lk(g_seatPubMx);
            g_seatPub = std::move(v);
        }

        std::uintptr_t ReadSeat(const Seat& s) {
            if (!s.callSite) return *reinterpret_cast<const std::uintptr_t*>(s.where);
            std::uint8_t b[5]{};
            std::memcpy(b, reinterpret_cast<const void*>(s.where), sizeof(b));
            if (b[0] != 0xE8) return 0;
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, 4);
            return s.where + 5 + static_cast<std::intptr_t>(rel);
        }

        // The source a displaced / displacing value belongs to: a foreign module, never the
        // engine (the engine is "no hook"), never ourselves.
        SourceId ForeignSourceOf(std::uintptr_t value, Kind kind) {
            if (!value) return kNoSource;
            const SourceId id = ForAddress(value, kind);
            return (id == kEngineSource) ? kNoSource : id;
        }

    }

    const char* FacetName(Facet f) {
        switch (f) {
        case Facet::kTarget:       return "Target";
        case Facet::kDetection:    return "Detection";
        case Facet::kPackage:      return "Package";
        case Facet::kEquip:        return "Equip";
        case Facet::kCombatAction: return "CombatAction";
        case Facet::kCast:         return "Cast";
        case Facet::kCombat:       return "Combat";
        case Facet::kCore:         return "Core";
        default:                   return "?";
        }
    }

    Facet FacetForTag(std::string_view t) {
        const auto starts = [&](std::string_view p) { return t.substr(0, p.size()) == p; };
        if (starts("ch.7") || starts("ch.23")) return Facet::kCombatAction;
        if (starts("t2a")) return Facet::kEquip;
        if (starts("nonaliasprobe")) return Facet::kPackage;
        if (starts("t2c") || starts("ch.8") || starts("aicastseats") || starts("ctcensus")) return Facet::kCast;
        return Facet::kCore;
    }

    const char* KindName(Kind k) {
        switch (k) {
        case Kind::kClient:     return "client";
        case Kind::kHook:       return "hook";
        case Kind::kInterposer: return "interposer";
        case Kind::kPlugin:     return "plugin";
        case Kind::kScript:     return "script";
        case Kind::kEngine:     return "engine";
        case Kind::kDirect:     return "direct calls";
        case Kind::kUnknown:    return "unknown";
        default:                return "?";
        }
    }

    void Init() {
        if (g_inited.exchange(true)) return;
        ReadIni();
        {
            std::scoped_lock lk(g_addMx);
            // A source registered before Init (an early client request) got the built-in
            // defaults: give it the INI's values now.
            const auto n = g_count.load(std::memory_order_relaxed);
            for (std::uint32_t i = 0; i < n; ++i)
                for (int f = 0; f < kFacetCount; ++f)
                    g_src[i].prio[f].store(InitialPriority(g_src[i].name, g_src[i].kind, static_cast<Facet>(f)),
                                           std::memory_order_relaxed);
            AddLocked("Engine", Kind::kEngine);              // kEngineSource
            AddLocked("DirectCalls", Kind::kDirect);         // kDirectSource
            AddLocked("Unattributed", Kind::kUnknown);       // kUnattributedSource
        }
        std::size_t keys = 0;
        for (const auto& m : g_iniPrio) keys += m.size();
        spdlog::info("[sources] Harbinger source registry up. {} -- {} priority override(s) read; defaults: unregistered "
                     "{}, client {}, direct calls {} (0 = off). An outside write Harbinger cannot attribute is ALLOWED "
                     "and alerted, never denied.",
                     keys ? "APMF_Sources.ini found" : "APMF_Sources.ini absent or empty (all defaults)", keys,
                     g_defUnreg[0], g_defClient[0], kDefaultDirect);
    }

    SourceId Find(std::string_view name) {
        const auto n = g_count.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; ++i)
            if (IEquals(name, g_src[i].name)) return static_cast<SourceId>(i);
        return kNoSource;
    }

    SourceId Register(std::string_view name, Kind kind) {
        if (name.empty()) return kNoSource;
        if (const auto id = Find(name); id != kNoSource) return id;
        std::scoped_lock lk(g_addMx);
        return AddLocked(name, kind);
    }

    std::uintptr_t FollowStubs(std::uintptr_t addr) {
        for (int hop = 0; hop < 3 && addr; ++hop) {
            if (ModuleOf(addr)) return addr;   // inside a module: that is the code
            std::uint8_t e[14]{};
            std::memcpy(e, reinterpret_cast<const void*>(addr), sizeof(e));
            std::uintptr_t next = 0;
            if (e[0] == 0xE9) {
                std::int32_t rel = 0; std::memcpy(&rel, e + 1, 4);
                next = addr + 5 + static_cast<std::intptr_t>(rel);
            } else if (e[0] == 0xFF && e[1] == 0x25) {
                std::int32_t disp = 0; std::memcpy(&disp, e + 2, 4);
                std::memcpy(&next, reinterpret_cast<const void*>(addr + 6 + static_cast<std::intptr_t>(disp)), sizeof(next));
            } else if (e[0] == 0x48 && e[1] == 0xB8 && e[10] == 0xFF && e[11] == 0xE0) {
                std::memcpy(&next, e + 2, sizeof(next));
            } else {
                return addr;
            }
            addr = next;
        }
        return addr;
    }

    const char* ModuleNameOf(std::uintptr_t addr, char* out, std::size_t size) {
        if (!out || size == 0) return "";
        out[0] = '\0';
        void* mod = ModuleOf(FollowStubs(addr));
        if (!mod) return out;
        char path[260]{};
        const auto n = REX::W32::GetModuleFileNameA(reinterpret_cast<REX::W32::HMODULE>(mod), path, sizeof(path));
        if (n == 0 || n >= sizeof(path)) return out;
        std::strncpy(out, BaseName(path), size - 1);
        out[size - 1] = '\0';
        return out;
    }

    SourceId ForAddress(std::uintptr_t addr, Kind kind) {
        const std::uintptr_t code = FollowStubs(addr);
        void* mod = ModuleOf(code);
        if (!mod) return kNoSource;
        if (mod == SelfModule()) return kNoSource;
        if (mod == reinterpret_cast<void*>(REL::Module::get().base())) return kEngineSource;
        const auto key = reinterpret_cast<std::uintptr_t>(mod);
        {
            std::scoped_lock lk(g_modCacheMx);
            if (const auto it = g_modCache.find(key); it != g_modCache.end()) return it->second;
        }
        char name[kNameMax]{};
        ModuleNameOf(code, name, sizeof(name));
        SourceId id = kNoSource;
        if (name[0] == '\0') id = kNoSource;
        else if (std::strncmp(name, "skse64", 6) == 0) id = kDirectSource;   // Papyrus natives / the console bridge
        else id = Register(name, kind);
        std::scoped_lock lk(g_modCacheMx);
        g_modCache[key] = id;
        return id;
    }

    std::string_view Name(SourceId id) { return Valid(id) ? std::string_view(g_src[id].name) : std::string_view(); }
    Kind KindOf(SourceId id) { return Valid(id) ? g_src[id].kind : Kind::kUnknown; }

    int Priority(SourceId id, Facet f) {
        if (!Valid(id) || f >= Facet::kCount) return kDefaultUnregistered;
        return g_src[id].prio[static_cast<int>(f)].load(std::memory_order_relaxed);
    }

    void NoteInvolved(SourceId id, Facet f) {
        if (!Valid(id) || f >= Facet::kCount) return;
        g_src[id].involved.fetch_or(1u << static_cast<int>(f), std::memory_order_relaxed);
    }

    void NoteHit(SourceId id, Facet f) {
        if (!Valid(id) || f >= Facet::kCount) return;
        auto& s = g_src[id];
        const int i = static_cast<int>(f);
        s.involved.fetch_or(1u << i, std::memory_order_relaxed);
        s.hits[i].fetch_add(1, std::memory_order_relaxed);
        s.lastSeenMs[i].store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
        s.lastLoadSeq.store(g_loadSeq.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void NoteOverruled(SourceId id, Facet f) {
        if (!Valid(id) || f >= Facet::kCount) return;
        g_src[id].overruled[static_cast<int>(f)].fetch_add(1, std::memory_order_relaxed);
    }

    void NoteStood(SourceId id, Facet f) {
        if (!Valid(id) || f >= Facet::kCount) return;
        g_src[id].stood[static_cast<int>(f)].fetch_add(1, std::memory_order_relaxed);
    }

    // ---- clients ----------------------------------------------------------------------

    Facet FacetForIntent(APMF_API::Intent i) {
        using namespace APMF_API;
        switch (i) {
        case kIntent_CombatTarget:
        case kIntent_TargetPin:         return Facet::kTarget;
        case kIntent_OfferPackage:
        case kIntent_Travel:            return Facet::kPackage;
        case kIntent_Equipment:
        case kIntent_EquipAuthority:    return Facet::kEquip;
        case kIntent_SelectSpell:
        case kIntent_Cast:              return Facet::kCast;
        case kIntent_CombatAction:
        case kIntent_PursuitLeash:      return Facet::kCombatAction;
        case kIntent_CombatEntry:
        case kIntent_CombatReentryDeny:
        case kIntent_CombatApproach:    return Facet::kCombat;
        case kIntent_Detection:         return Facet::kDetection;
        default:                        return Facet::kCore;
        }
    }

    void NoteClientRequest(APMF_API::Handle h, const void* returnAddress, APMF_API::Intent intent) {
        if (h == APMF_API::kInvalidHandle) return;
        SourceId id = ForAddress(reinterpret_cast<std::uintptr_t>(returnAddress), Kind::kClient);
        // A client request from inside APMF itself or the engine is not a client (never
        // happens through ClientAPI, which only clients call). Keep the handle unmapped.
        if (id == kEngineSource || id == kDirectSource) id = kNoSource;
        if (id == kNoSource) return;
        NoteInvolved(id, FacetForIntent(intent));
        std::unique_lock lk(g_handleMx);
        if (g_handles.size() >= kHandleCap) {
            // Never unbounded: drop the oldest half by issue time (Pump also purges dead ones).
            std::vector<std::pair<std::uint64_t, APMF_API::Handle>> v;
            v.reserve(g_handles.size());
            for (const auto& [k, r] : g_handles) v.emplace_back(r.issuedMs, k);
            std::sort(v.begin(), v.end());
            for (std::size_t i = 0; i < v.size() / 2; ++i) g_handles.erase(v[i].second);
        }
        g_handles[h] = HandleRec{ id, apmf::clock::MonotonicMs() };
    }

    void ForgetHandle(APMF_API::Handle h) {
        std::unique_lock lk(g_handleMx);
        g_handles.erase(h);
    }

    SourceId ClientForHandle(APMF_API::Handle h) {
        std::shared_lock lk(g_handleMx);
        const auto it = g_handles.find(h);
        return it == g_handles.end() ? kNoSource : it->second.src;
    }

    int ClaimPriority(APMF_API::Handle h, Facet f, SourceId* outClient) {
        const SourceId c = ClientForHandle(h);
        if (outClient) *outClient = c;
        if (c == kNoSource) return f < Facet::kCount ? g_defClient[static_cast<int>(f)] : kDefaultClient;
        return Priority(c, f);
    }

    // ---- the degrade rule ------------------------------------------------------------

    void Unattributed(Facet f, RE::FormID actor, const char* detail) {
        if (f >= Facet::kCount) return;
        NoteHit(kUnattributedSource, f);
        g_alertTotal.fetch_add(1, std::memory_order_relaxed);
        const auto now = apmf::clock::MonotonicMs();
        {
            std::scoped_lock lk(g_alertMx);
            Alert& a = g_alerts[g_alertNext];
            g_alertNext = (g_alertNext + 1) % kAlertRing;
            a.ms     = now;
            a.facet  = f;
            a.actor  = actor;
            std::strncpy(a.detail, detail ? detail : "", sizeof(a.detail) - 1);
            a.detail[sizeof(a.detail) - 1] = '\0';
        }
        const int i    = static_cast<int>(f);
        auto      prev = g_unattribLastLog[i].load(std::memory_order_relaxed);
        if (now - prev >= kUnattribLogMs && g_unattribLastLog[i].compare_exchange_strong(prev, now)) {
            const auto suppressed = g_unattribSuppressed[i].exchange(0, std::memory_order_relaxed);
            spdlog::warn("[sources] UNATTRIBUTED {} write on actor 0x{}: {} -- ALLOWED (Harbinger never denies a writer "
                         "it cannot name; see the Harbinger page alerts). {} more in the last {} s not logged.",
                         FacetName(f), Hex(actor), detail ? detail : "", suppressed, kUnattribLogMs / 1000);
        } else {
            g_unattribSuppressed[i].fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::vector<Alert> RecentAlerts() {
        std::vector<Alert> v;
        std::scoped_lock   lk(g_alertMx);
        for (std::size_t k = 0; k < kAlertRing; ++k) {
            const Alert& a = g_alerts[(g_alertNext + kAlertRing - 1 - k) % kAlertRing];
            if (a.ms == 0) break;
            v.push_back(a);
        }
        return v;
    }

    std::uint64_t AlertCount() { return g_alertTotal.load(std::memory_order_relaxed); }

    // ---- the census --------------------------------------------------------------------

    void RegisterVtableSeat(Facet f, std::string seat, std::uintptr_t entry, std::uintptr_t ours,
                            std::uintptr_t prev, std::function<std::uintptr_t(std::uintptr_t)> rehook) {
        try {
            Seat s;
            s.seat     = std::move(seat);
            s.facet    = f;
            s.callSite = false;
            s.where    = entry;
            s.ours     = ours;
            s.lastSeen = ours;
            s.prev     = ForeignSourceOf(prev, Kind::kHook);
            s.rehook   = std::move(rehook);
            if (s.prev != kNoSource) {
                NoteInvolved(s.prev, f);
                spdlog::warn("[sources] census: seat {} ({}) was ALREADY hooked by {} when Harbinger installed -- that "
                             "hook runs INSIDE Harbinger's seat and is a {} source.",
                             s.seat, FacetName(f), Name(s.prev), FacetName(f));
            }
            g_seats.push_back(std::move(s));
            g_seatCount.store(static_cast<std::uint32_t>(g_seats.size()), std::memory_order_relaxed);
            PublishSeats();
        } catch (...) {
        }
    }

    void RegisterCallSeat(Facet f, std::string seat, std::uintptr_t site, std::uintptr_t prevTarget) {
        try {
            Seat s;
            s.seat     = std::move(seat);
            s.facet    = f;
            s.callSite = true;
            s.where    = site;
            s.ours     = ReadSeat(s);
            s.lastSeen = s.ours;
            s.prev     = ForeignSourceOf(prevTarget, Kind::kInterposer);
            if (s.prev != kNoSource) {
                NoteInvolved(s.prev, f);
                spdlog::warn("[sources] census: call site {} ({}) was ALREADY patched by {} when Harbinger installed -- "
                             "that patch runs INSIDE Harbinger's seat and is a {} source.",
                             s.seat, FacetName(f), Name(s.prev), FacetName(f));
            }
            g_seats.push_back(std::move(s));
            g_seatCount.store(static_cast<std::uint32_t>(g_seats.size()), std::memory_order_relaxed);
            PublishSeats();
        } catch (...) {
        }
    }

    SourceId PrevSourceOf(std::string_view seat) {
        auto snap = SeatSnapshot();
        for (const auto& s : *snap)
            if (s.seat == seat) return s.prev;
        return kNoSource;
    }

    void Census(const char* why) {
        try {
            std::uint32_t displaced = 0;
            bool          changed   = false;
            for (auto& s : g_seats) {
                const std::uintptr_t cur = ReadSeat(s);
                if (cur == s.lastSeen) {
                    if (cur != s.ours) ++displaced;
                    continue;
                }
                s.lastSeen = cur;
                changed    = true;
                if (cur == s.ours) {
                    spdlog::info("[sources] census ({}): seat {} holds Harbinger's own pointer again.", why, s.seat);
                    s.lostOutermost = false;
                    continue;
                }
                if (ModuleOf(FollowStubs(cur)) == SelfModule()) {
                    // Another Harbinger seat installed later on the same entry (e.g. ch.24's selector
                    // seat over ch.20's): still ours, still outermost.
                    s.ours = cur;
                    s.lostOutermost = false;
                    continue;
                }
                const SourceId late = ForeignSourceOf(cur, s.callSite ? Kind::kInterposer : Kind::kHook);
                s.late = late;
                if (late != kNoSource) NoteInvolved(late, s.facet);
                char mod[kNameMax]{};
                ModuleNameOf(cur, mod, sizeof(mod));
                if (s.rehook) {
                    const std::uintptr_t mine = s.rehook(cur);
                    if (mine) {
                        s.ours     = mine;
                        s.lastSeen = mine;
                        s.lostOutermost = false;
                        ++s.rehooks;
                        spdlog::warn("[sources] census ({}): seat {} ({}) was hooked over AFTER Harbinger by {} -- "
                                     "RE-HOOKED ON TOP (Q13). {} now runs INSIDE Harbinger's seat as a {} source.",
                                     why, s.seat, FacetName(s.facet), mod[0] ? mod : "an unnamed trampoline",
                                     late != kNoSource ? Name(late) : std::string_view("it"), FacetName(s.facet));
                        continue;
                    }
                }
                s.lostOutermost = true;
                ++displaced;
                spdlog::error("[sources] census ({}): seat {} ({}) was hooked over AFTER Harbinger by {} -- Harbinger is "
                              "NOT outermost there. Its writes are captured only where another seat sees them; "
                              "{}",
                              why, s.seat, FacetName(s.facet), mod[0] ? mod : "an unnamed trampoline",
                              s.callSite ? "a call-site seat stays the innermost patch, so its deny still holds."
                                         : "no re-hook exists for this seat in Phase 1a (it is a census row).");
            }
            if (changed) PublishSeats();
            spdlog::info("[sources] census ({}): {} seat(s) checked, {} not holding Harbinger's pointer, {} source(s) "
                         "known.",
                         why, g_seats.size(), displaced, g_count.load());
        } catch (...) {
        }
    }

    std::shared_ptr<const std::vector<SeatView>> SeatSnapshot() {
        std::scoped_lock lk(g_seatPubMx);
        return g_seatPub;
    }

    std::uint32_t SeatCount() { return g_seatCount.load(std::memory_order_relaxed); }

    // ---- pump --------------------------------------------------------------------------

    void Pump() {
        if (!g_inited.load(std::memory_order_relaxed)) return;
        if (g_opsPending.exchange(false, std::memory_order_acq_rel)) {
            std::vector<PrioOp> ops;
            {
                std::scoped_lock lk(g_opMx);
                ops.swap(g_ops);
            }
            for (const auto& op : ops) {
                if (!Valid(op.id) || op.f >= Facet::kCount) continue;
                auto& s    = g_src[op.id];
                const int i = static_cast<int>(op.f);
                const int v = Clamp(op.prio);
                if (s.prio[i].exchange(v, std::memory_order_relaxed) == v) continue;
                const auto val = std::to_string(v);
                const bool ok  = WritePrivateProfileStringA(FacetName(op.f), s.name, val.c_str(), kIni) != 0;
                spdlog::info("[sources] priority {} / {} = {} (menu){}.", FacetName(op.f), s.name, v,
                             ok ? ", saved to APMF_Sources.ini" : " -- the INI write FAILED, it lasts this session only");
            }
        }
        const auto now = apmf::clock::MonotonicMs();
        if (g_lastCensusMs == 0) g_lastCensusMs = now;
        if (now - g_lastCensusMs >= kCensusPeriodMs) {
            g_lastCensusMs = now;
            Census("60 s");
            // Purge handles whose claim is gone (released internally, ended by Harbinger,
            // unloaded). A just-filed handle may not be published yet: keep anything < 60 s old.
            std::vector<APMF_API::Handle> dead;
            {
                std::shared_lock lk(g_handleMx);
                for (const auto& [h, r] : g_handles)
                    if (now - r.issuedMs >= kCensusPeriodMs && !apmf::ControlMap::Get().IsClaimLive(h)) dead.push_back(h);
            }
            if (!dead.empty()) {
                std::unique_lock lk(g_handleMx);
                for (const auto h : dead) g_handles.erase(h);
            }
        }
    }

    void OnPostLoad(const char* why) {
        g_loadSeq.fetch_add(1, std::memory_order_relaxed);
        Census(why);
    }

    void EnqueueSetPriority(SourceId id, Facet f, int prio) {
        try {
            std::scoped_lock lk(g_opMx);
            g_ops.push_back(PrioOp{ id, f, prio });
            g_opsPending.store(true, std::memory_order_release);
        } catch (...) {
        }
    }

    std::uint32_t Count() { return g_count.load(std::memory_order_acquire); }

    bool Involved(SourceId id, Facet f) {
        return Valid(id) && f < Facet::kCount &&
               (g_src[id].involved.load(std::memory_order_relaxed) & (1u << static_cast<int>(f))) != 0;
    }
    std::uint32_t Hits(SourceId id, Facet f) {
        return Valid(id) && f < Facet::kCount ? g_src[id].hits[static_cast<int>(f)].load(std::memory_order_relaxed) : 0;
    }
    std::uint32_t Overruled(SourceId id, Facet f) {
        return Valid(id) && f < Facet::kCount ? g_src[id].overruled[static_cast<int>(f)].load(std::memory_order_relaxed) : 0;
    }
    std::uint32_t Stood(SourceId id, Facet f) {
        return Valid(id) && f < Facet::kCount ? g_src[id].stood[static_cast<int>(f)].load(std::memory_order_relaxed) : 0;
    }
    std::uint64_t LastSeenMs(SourceId id, Facet f) {
        return Valid(id) && f < Facet::kCount ? g_src[id].lastSeenMs[static_cast<int>(f)].load(std::memory_order_relaxed) : 0;
    }
    bool Idle(SourceId id) {
        if (!Valid(id)) return true;
        const auto seen = g_src[id].lastLoadSeq.load(std::memory_order_relaxed);
        const auto now  = g_loadSeq.load(std::memory_order_relaxed);
        return seen == 0 || now - seen >= kIdleLoads;
    }

}
