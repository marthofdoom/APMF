#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"

#include <intrin.h>      // _ReturnAddress / _AddressOfReturnAddress (core/EquipGate.cpp precedent)
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>

// Win32 INI read for the three switches below. Declared by hand, exactly like
// core/EquipGate.cpp / core/CastClassify.cpp do -- PCH does not pull in
// <Windows.h>. The two module queries name the DLL an EXTERNAL equip caller
// lives in (diagnosis only; the verdict never depends on the name).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);
extern "C" __declspec(dllimport) int __stdcall GetModuleHandleExA(
    unsigned long a_flags, const char* a_moduleName, void** a_outModule);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(
    void* a_module, char* a_buf, unsigned long a_size);

// ============================================================================
// See core/EquipSink.h for the design and Docs/INVARIANTS.md #17a for the
// licence. This TU is the single sink thunk plus its install-time byte-verify.
// ============================================================================

namespace apmf::equipsink {

    namespace {

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // ---- The worker's 4th argument, as the two call sites build it on the
        // stack (AE EquipObject +0x113..+0x15d at rsp+0x28; SE +0x87..+0xd3; the
        // list sibling at rsp+0x20 with the dword 0x00010001 = queue/force/
        // sounds/applyNow). Natural layout reproduces the measured offsets; the
        // static_asserts pin them. Read-only here: the thunk never writes it. ----
        struct EquipData {
            RE::ExtraDataList*      extra;      // +0x00  EquipObject's a_extraData
            std::uint32_t           count;      // +0x08  a_count
            const RE::BGSEquipSlot* slot;       // +0x10  a_slot, or the default slot resolved by 38911/37955 (0x6cab20/0x638c30)
            std::uint64_t           zero;       // +0x18  always 0 at both sites
            bool                    queue;      // +0x20  a_queueEquip
            bool                    force;      // +0x21  a_forceEquip
            bool                    sounds;     // +0x22  a_playSounds
            bool                    applyNow;   // +0x23  a_applyNow
            bool                    tail;       // +0x24  always 0 at both sites
        };
        static_assert(offsetof(EquipData, extra)    == 0x00);
        static_assert(offsetof(EquipData, count)    == 0x08);
        static_assert(offsetof(EquipData, slot)     == 0x10);
        static_assert(offsetof(EquipData, zero)     == 0x18);
        static_assert(offsetof(EquipData, queue)    == 0x20);
        static_assert(offsetof(EquipData, force)    == 0x21);
        static_assert(offsetof(EquipData, sounds)   == 0x22);
        static_assert(offsetof(EquipData, applyNow) == 0x23);
        static_assert(offsetof(EquipData, tail)     == 0x24);

        // The worker: (ActorEquipManager*, Actor*, TESBoundObject*, EquipData*).
        // Four register args, no stack args read (prologue verified on both
        // binaries), return value UNUSED by both callers (AE +0x175 nop/lfence
        // then a call that overwrites eax; the sibling restores registers and
        // returns) -- so returning without calling it is ABI-clean.
        using Worker_t = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*, EquipData*);

        // ---- Per-runtime site table (Docs/INVARIANTS.md #17a condition 3: from
        // the disassembly of THAT runtime, never a CommonLib declaration).
        // `depth` = byte offset from THIS thunk's own return-address slot to
        // EquipObject's CALLER's return-address slot, measured from the bytes:
        //   AE 38894 EquipObject : push rdi,r12,r13,r14,r15 + sub rsp,0x50 -> 8 + 0x50 + 5*8 = 0x80
        //   AE 38893 list sibling: push r12,r14,r15          + sub rsp,0x50 -> 8 + 0x50 + 3*8 = 0x70
        //   SE 37938 EquipObject : push rdi                  + sub rsp,0x50 -> 8 + 0x50 + 1*8 = 0x60
        //   SE 37937 list sibling: push r12,r14,r15          + sub rsp,0x50 -> 8 + 0x50 + 3*8 = 0x70
        // The 8 is this thunk's own return slot (the E8 at the site pushed it).
        struct SiteSpec {
            std::uint64_t id;      // Address-Library id of the function holding the site
            std::uint32_t off;     // byte offset of the E8 inside that function
            std::uint32_t depth;   // see above
            const char*   name;
        };
        constexpr SiteSpec kSitesAE[] = {
            { 38894, 0x170, 0x80, "EquipObject" },
            { 38893, 0x0BC, 0x70, "EquipObjectList" },
        };
        constexpr SiteSpec kSitesSE[] = {
            { 37938, 0x0E5, 0x60, "EquipObject" },
            { 37937, 0x0BC, 0x70, "EquipObjectList" },
        };
        constexpr std::uint64_t kWorkerAE = 38929;   // 1.6.1170 RVA 0x6CBE30
        constexpr std::uint64_t kWorkerSE = 37974;   // 1.5.97   RVA 0x639E20

        // ---- Caller path table (per runtime). An engine caller of EquipObject /
        // the list sibling whose id is not here logs as Unknown(<id>); a return
        // address outside SkyrimSE.exe logs as External(<module>). ----
        enum class PathKind : std::uint8_t { kEngine, kScript, kConsole };
        struct PathSpec {
            std::uint64_t id;
            const char*   name;
            PathKind      kind;
        };
        constexpr PathSpec kPathsAE[] = {
            { 418622, "OutfitApply",       PathKind::kEngine },
            { 19692,  "AddWornOutfit",     PathKind::kEngine },
            { 39649,  "AiCommand",         PathKind::kEngine },
            { 39650,  "AiCommand",         PathKind::kEngine },
            { 37797,  "RemoveItemReequip", PathKind::kEngine },
            { 19689,  "RemoveItemReequip", PathKind::kEngine },
            { 16059,  "RemoveItemReequip", PathKind::kEngine },
            { 48126,  "CombatNode",        PathKind::kEngine },
            { 48124,  "CombatNode",        PathKind::kEngine },
            { 54661,  "Script",            PathKind::kScript },
            { 22351,  "Console",           PathKind::kConsole },
            { 29383,  "PlayerMenu",        PathKind::kEngine },
            { 29346,  "PlayerMenu",        PathKind::kEngine },
            { 38907,  "PlayerMenu",        PathKind::kEngine },
            { 38913,  "WorkerReentry",     PathKind::kEngine },   // the worker family's own re-entry (2nd copy / dual wield)
        };
        constexpr PathSpec kPathsSE[] = {
            { 24234,  "OutfitApply",       PathKind::kEngine },
            { 19266,  "AddWornOutfit",     PathKind::kEngine },
            { 38618,  "AiCommand",         PathKind::kEngine },
            { 38619,  "AiCommand",         PathKind::kEngine },
            { 36781,  "RemoveItemReequip", PathKind::kEngine },
            { 19263,  "RemoveItemReequip", PathKind::kEngine },
            { 15821,  "RemoveItemReequip", PathKind::kEngine },
            { 46957,  "CombatNode",        PathKind::kEngine },
            { 46955,  "CombatNode",        PathKind::kEngine },
            { 53861,  "Script",            PathKind::kScript },
            { 21869,  "Console",           PathKind::kConsole },
            { 28629,  "PlayerMenu",        PathKind::kEngine },
            { 28593,  "PlayerMenu",        PathKind::kEngine },
            { 37951,  "PlayerMenu",        PathKind::kEngine },
            { 37957,  "WorkerReentry",     PathKind::kEngine },
        };

        // ---- runtime state (written at Install on the main thread, read from any
        // thread afterwards; the atomics publish the install) ----
        struct InstalledSite {
            std::uintptr_t addr  = 0;   // absolute address of the E8
            std::uint32_t  depth = 0;
            std::uint64_t  id    = 0;
            std::uint32_t  off   = 0;
            const char*    name  = "";
        };
        std::atomic<bool>  g_installed{ false };
        std::atomic<bool>  g_observeOnly{ true };
        std::atomic<bool>  g_denyScript{ false };
        Worker_t           g_worker = nullptr;
        InstalledSite      g_sites[2]{};
        std::uintptr_t     g_base = 0, g_textBegin = 0, g_textEnd = 0;
        const PathSpec*    g_paths     = nullptr;
        std::size_t        g_pathCount = 0;
        // Sorted (offset -> id) copy of the Address Library, built once at
        // Install and immutable afterwards (concurrent readers are fine). NEVER
        // call its operator(): that is lower_bound + report_and_fail on a miss;
        // classification uses the public iterators with upper_bound - 1 instead.
        std::unique_ptr<REL::IDDatabase::Offset2ID> g_offset2id;

        thread_local int t_apmfDepth = 0;

        // ---- logging discipline: dedupe per (actor,item,path) for 2 s, global
        // cap 100 lines/s with a per-minute drop count (CLAUDE.md principle 8:
        // volume is not importance -- one dropped-count line beats a flood) ----
        constexpr std::uint64_t kDedupeMs   = 2000;
        constexpr std::uint32_t kCapPerSec  = 100;
        constexpr std::size_t   kDedupeMax  = 2048;
        std::mutex                                       g_logMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;   // key -> ms
        std::uint64_t g_secStart = 0, g_minStart = 0;
        std::uint32_t g_secCount = 0, g_minDropped = 0;

        // Returns true if this line may be logged now; also emits the per-minute
        // drop report when one is due. `pending` receives the drop count to report
        // (0 == none) so the report itself is logged OUTSIDE the lock.
        bool AdmitLog(std::uint64_t key, std::uint64_t nowMs, std::uint32_t& pendingDrops) {
            pendingDrops = 0;
            std::scoped_lock lock(g_logMx);
            if (g_minStart == 0) g_minStart = nowMs;
            if (nowMs - g_minStart >= 60000) {
                pendingDrops = g_minDropped;
                g_minDropped = 0;
                g_minStart   = nowMs;
            }
            if (auto it = g_lastLogMs.find(key); it != g_lastLogMs.end() && nowMs - it->second < kDedupeMs)
                return false;   // dedupe: same (actor,item,path) inside the window -- not a drop
            if (nowMs - g_secStart >= 1000) { g_secStart = nowMs; g_secCount = 0; }
            if (g_secCount >= kCapPerSec) { ++g_minDropped; return false; }
            ++g_secCount;
            if (g_lastLogMs.size() >= kDedupeMax) {
                for (auto it = g_lastLogMs.begin(); it != g_lastLogMs.end();)
                    it = (nowMs - it->second >= kDedupeMs) ? g_lastLogMs.erase(it) : std::next(it);
                if (g_lastLogMs.size() >= kDedupeMax) g_lastLogMs.clear();   // still full: reset, never grow unbounded
            }
            g_lastLogMs[key] = nowMs;
            return true;
        }

        // ---- who asked? ----
        struct Caller {
            const char*    name  = "Unknown";
            PathKind       kind  = PathKind::kEngine;
            std::uint64_t  id    = 0;
            std::uintptr_t rva   = 0;      // inside the exe: the return address' RVA
            bool           known = false;  // true iff `name` came from the path table
            bool           external = false;
            char           ext[64]{};      // External: the module's base name
        };

        Caller Classify(std::uintptr_t ret) {
            Caller c;
            if (ret >= g_textBegin && ret < g_textEnd) {
                c.rva = ret - g_base;
                if (g_offset2id) {
                    // Greatest library offset <= rva == the function containing the
                    // return address. upper_bound on the sorted-by-offset table, then
                    // step back one (never operator(): it report_and_fails on a miss).
                    auto it = std::upper_bound(g_offset2id->begin(), g_offset2id->end(), c.rva,
                                               [](std::uintptr_t a, const auto& m) { return a < m.offset; });
                    if (it != g_offset2id->begin()) c.id = std::prev(it)->id;
                }
                for (std::size_t i = 0; i < g_pathCount; ++i) {
                    if (g_paths[i].id == c.id) { c.name = g_paths[i].name; c.kind = g_paths[i].kind; c.known = true; return c; }
                }
                return c;   // Unknown(<id>)
            }
            c.external = true;
            c.name     = "External";
            void* mod  = nullptr;
            // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x4) | UNCHANGED_REFCOUNT (0x2)
            if (GetModuleHandleExA(0x6, reinterpret_cast<const char*>(ret), &mod) && mod) {
                char path[260]{};
                const auto n = GetModuleFileNameA(mod, path, sizeof(path));
                if (n > 0 && n < sizeof(path)) {
                    const char* baseName = path;
                    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') baseName = p + 1;
                    std::strncpy(c.ext, baseName, sizeof(c.ext) - 1);
                    // SKSE's own Papyrus natives (EquipItemEx/EquipItemById) live in
                    // skse64_*.dll: a script path in every sense that matters.
                    if (std::strncmp(c.ext, "skse64", 6) == 0) c.kind = PathKind::kScript;
                }
            }
            if (c.ext[0] == '\0') std::strncpy(c.ext, "?", sizeof(c.ext) - 1);
            return c;
        }

        // ---- THE THUNK. Same signature as the worker; the two patched E8s land
        // here through one shared trampoline stub. ----
        void SinkThunk(RE::ActorEquipManager* mgr, RE::Actor* actor, RE::TESBoundObject* obj, EquipData* data) {
            // Captured FIRST, before anything else (core/EquipGate.cpp precedent):
            // the genuine site (the E8 is 5 bytes before where it returns to) and
            // this frame's own return slot, from which the caller's slot is a fixed
            // per-site distance.
            const std::uintptr_t site      = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - 5;
            const auto           myRetSlot = reinterpret_cast<std::uint8_t*>(_AddressOfReturnAddress());

            // #17a condition 5: the player and any unclaimed actor pass through
            // untouched and unlogged. Cheapest checks first.
            if (!actor || !obj || !data || actor->IsPlayerRef()) { g_worker(mgr, actor, obj, data); return; }
            const RE::FormID actorId = actor->GetFormID();
            EquipSetView set;
            if (!ControlMap::Get().TryGetEquipSet(actorId, set)) { g_worker(mgr, actor, obj, data); return; }

            // A claimed actor. Everything from here is logged (deduped/capped).
            const RE::FormID itemId = obj->GetFormID();
            const int        tls    = t_apmfDepth;

            std::uint32_t depth = 0;
            const InstalledSite* siteSpec = nullptr;
            for (const auto& s : g_sites) if (s.addr == site) { siteSpec = &s; depth = s.depth; break; }
            Caller caller;
            if (depth) {
                std::uintptr_t ret = 0;
                std::memcpy(&ret, myRetSlot + depth, sizeof(ret));
                caller = Classify(ret);
            }
            // (depth == 0 cannot happen -- only our two E8s reach this thunk -- but a
            // classification that cannot be made is logged as Unknown, never guessed.)

            bool inSet = false;
            for (std::uint32_t i = 0; i < set.count; ++i) if (set.forms[i] == itemId) { inSet = true; break; }

            const bool observe    = g_observeOnly.load(std::memory_order_relaxed) ||
                                    (set.flags & APMF_API::kEquipAuth_ObserveOnly) != 0;
            const bool denyScript = g_denyScript.load(std::memory_order_relaxed) ||
                                    (set.flags & APMF_API::kEquipAuth_DenyScript) != 0;
            const bool scriptPath = (caller.kind == PathKind::kScript || caller.kind == PathKind::kConsole);

            // The verdict. Order matters and is the whole policy:
            //   APMF's own equip (and the engine's re-entry beneath it)  -> allow
            //   no declaration yet                                      -> allow (declare->enforce)
            //   a declared item                                         -> allow
            //   a script/console equip without DenyScript               -> allow
            //   anything else                                           -> deny (or would-deny)
            const char* verdict = "allow";
            bool        callWorker = true;
            if (tls > 0 || set.count == 0 || inSet || (scriptPath && !denyScript)) {
                verdict = "allow";
            } else if (observe) {
                verdict = "would-deny";
            } else {
                verdict    = "deny";
                callWorker = false;
            }

            // Log line (Docs/INTEGRATION.md's probe criteria parse these fields).
            // Wrapped: nothing in the logging path may unwind into the engine's
            // equip frame (a bad_alloc here must cost a log line, never a CTD).
            try {
                const auto     nowMs = apmf::clock::MonotonicMs();
                const std::uint64_t pathKey = caller.external
                    ? (0x8000000000000000ull ^ std::hash<std::string_view>{}(caller.ext))
                    : caller.id;
                const std::uint64_t key = ((static_cast<std::uint64_t>(actorId) << 32) | itemId) ^ (pathKey * 0x9E3779B97F4A7C15ull);
                std::uint32_t drops = 0;
                const bool admit = AdmitLog(key, nowMs, drops);
                if (drops)
                    spdlog::warn("[apmf][equip-obs] dropped {} line(s) in the last minute (cap {}/s).", drops, kCapPerSec);
                if (admit) {
                    std::string path;
                    if (caller.external)     path = std::string("External(") + caller.ext + ")";
                    else if (!caller.known)  path = "Unknown(" + std::to_string(caller.id) + ")";
                    else                     path = caller.name;
                    const char* itemName = obj->GetName();
                    spdlog::info("[apmf][equip-obs] actor={} item={} op=equip path={} site={}+0x{} ret={} q={} f={} s={} a={} tls={} verdict={} name='{}'",
                                 apmf::log::Hex(actorId), apmf::log::Hex(itemId), path,
                                 siteSpec ? siteSpec->id : 0, apmf::log::Hex(siteSpec ? siteSpec->off : 0, 0),
                                 caller.external ? std::string("ext") : apmf::log::Hex(caller.rva, 0),
                                 data->queue ? 1 : 0, data->force ? 1 : 0, data->sounds ? 1 : 0, data->applyNow ? 1 : 0,
                                 tls, verdict, itemName ? itemName : "");
                }
            } catch (...) {
            }

            if (callWorker) g_worker(mgr, actor, obj, data);
            // deny: return without calling -- nothing queued, no re-entry, and the
            // caller's own lock/epilogue runs exactly as if the worker had returned.
        }

    }   // namespace

    ApmfEquipScope::ApmfEquipScope()  { ++t_apmfDepth; }
    ApmfEquipScope::~ApmfEquipScope() { --t_apmfDepth; }
    int  ApmfDepth()   { return t_apmfDepth; }
    bool Installed()   { return g_installed.load(std::memory_order_acquire); }
    bool ObserveOnly() { return g_observeOnly.load(std::memory_order_relaxed); }
    bool DenyScript()  { return g_denyScript.load(std::memory_order_relaxed); }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[apmf][equip-sink] VR runtime -- sites are SE/AE-only verified; seat NOT installed.");
            return;
        }
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;

        const bool enabled = GetPrivateProfileIntA("EquipAuthority", "bEquipAuthority", 1, kIni) != 0;
        g_observeOnly.store(GetPrivateProfileIntA("EquipAuthority", "bEquipObserveOnly", 1, kIni) != 0,
                            std::memory_order_relaxed);
        g_denyScript.store(GetPrivateProfileIntA("EquipAuthority", "bEquipDenyScript", 0, kIni) != 0,
                           std::memory_order_relaxed);
        if (!enabled) {
            spdlog::warn("[apmf][equip-sink] [EquipAuthority] bEquipAuthority=0 -- seat NOT installed; "
                         "kIntent_EquipAuthority claims will be accepted but enforce nothing.");
            return;
        }

        const bool ae = REL::Module::IsAE();
        const SiteSpec* sites = ae ? kSitesAE : kSitesSE;
        g_paths     = ae ? kPathsAE : kPathsSE;
        g_pathCount = ae ? std::size(kPathsAE) : std::size(kPathsSE);

        const auto& mod  = REL::Module::get();
        g_base           = mod.base();
        const auto text  = mod.segment(REL::Segment::textx);
        g_textBegin      = text.address();
        g_textEnd        = text.address() + text.size();

        const std::uintptr_t worker = REL::ID(ae ? kWorkerAE : kWorkerSE).address();
        g_worker = reinterpret_cast<Worker_t>(worker);

        // #17a condition 2: byte-verify EVERY site before writing ANY. Any mismatch
        // refuses the whole seat -- a SCAR-class collision becomes a refusal.
        bool ok = true;
        for (int i = 0; i < 2; ++i) {
            const auto& s = sites[i];
            const std::uintptr_t addr = REL::ID(s.id).address() + s.off;
            std::uint8_t b[5]{};
            std::memcpy(b, reinterpret_cast<const void*>(addr), sizeof(b));
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            const std::uintptr_t target = addr + 5 + static_cast<std::intptr_t>(rel);
            if (b[0] != 0xE8 || target != worker) {
                spdlog::error("[apmf][equip-sink] site-verify FAILED {}+0x{} ({}) got {} {} {} {} {} (target 0x{}, expected E8->0x{}) -- seat NOT installed",
                              s.id, apmf::log::Hex(s.off, 0), s.name,
                              apmf::log::Hex(b[0], 2), apmf::log::Hex(b[1], 2), apmf::log::Hex(b[2], 2),
                              apmf::log::Hex(b[3], 2), apmf::log::Hex(b[4], 2),
                              apmf::log::Hex(target - g_base, 0), apmf::log::Hex(worker - g_base, 0));
                ok = false;
                continue;
            }
            g_sites[i] = InstalledSite{ addr, s.depth, s.id, s.off, s.name };
            spdlog::info("[apmf][equip-sink] site {}+0x{} ({}) verified E8->0x{} (caller-ret depth 0x{})",
                         s.id, apmf::log::Hex(s.off, 0), s.name, apmf::log::Hex(worker - g_base, 0),
                         apmf::log::Hex(s.depth, 0));
        }
        if (!ok) {
            g_sites[0] = g_sites[1] = InstalledSite{};
            return;
        }

        // The caller classifier's table: a sorted copy of the Address Library
        // (one-time, ~0.4M AE / ~0.8M SE entries). Built BEFORE the patch so the
        // thunk never runs against a half-built table.
        {
            const auto t0 = apmf::clock::MonotonicMs();
            g_offset2id = std::make_unique<REL::IDDatabase::Offset2ID>();
            spdlog::info("[apmf][equip-sink] caller table: {} address-library entries sorted in {} ms.",
                         g_offset2id->size(), apmf::clock::MonotonicMs() - t0);
        }

        // Both sites share one 14-byte stub (write_5branch keys the stub by
        // destination). 64 leaves headroom; nothing else in APMF uses the trampoline.
        SKSE::AllocTrampoline(64);
        auto& tr = SKSE::GetTrampoline();
        for (const auto& s : g_sites) tr.write_call<5>(s.addr, &SinkThunk);

        g_installed.store(true, std::memory_order_release);
        spdlog::info("[apmf][equip-sink] INSTALLED on {} ({} sites -> worker {}); observe-only={} deny-script={}. "
                     "Deny = the worker is not called; the player and unclaimed actors pass untouched (INVARIANTS #17a).",
                     ae ? "AE" : "SE", 2, ae ? kWorkerAE : kWorkerSE,
                     g_observeOnly.load(std::memory_order_relaxed) ? 1 : 0,
                     g_denyScript.load(std::memory_order_relaxed) ? 1 : 0);
    }

}
