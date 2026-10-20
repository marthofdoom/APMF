#include "PCH.h"
#include "channels/SubstrateProbe.h"

// Win32, declared by hand (the PCH does not pull in <Windows.h>; the same one-line import core/Sources.cpp uses).
extern "C" __declspec(dllimport) int __stdcall GetModuleHandleExA(unsigned long a_flags, const char* a_moduleName,
                                                                   void** a_outModule);

#include "core/Clock.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// THE SUBSTRATE PROBES (substrate round R1, SUBSTRATE-TO-OOC-PLAN.md "R1 detail"). PASSIVE.
//
// What each probe answers (the 10-19 field run could not tell these apart, plan section 0):
//   MOVE-BASELINE  speed and distance to the player per holder, from 1 s before a dry-run REQUEST (the
//                  pick is known one round early: DryRunSchedule hands the probe its due time) or from
//                  the entry of a client substrate, through the window, to 3 s after the close. One
//                  summary line per window: "idle inside the follow radius" vs "frozen".
//   re-pick        at "the re-pick did NOT run within 2000 ms": the context the actor's AI update needs.
//   other reset    each ignoring-bit reset Serve attributes to OTHER: its frame and package.
//   aggressor      AIProcess+0x136 bit 2 before the entry, at the close, +1 s and +5 s after.
//   hands          the weapon state after the close: ms to "not drawn" and to "sheathed", or 10 s.
// R5 additions (F-7 of SUBSTRATE-R2-RESEARCH.md; F-5 / F-4):
//   procedure      at each OTHER reset, the running ActorPackage's procedure fields this Poll and the previous one:
//                  +0x1C currentProcedureIndex, +0x10 data, and the three fields every procedure-tree node BEGIN
//                  rewrites (AE 38970 / SE 38015, from the "begin" visitor AE 29290 / SE 28536 that also calls
//                  UpdateIgnoringCombat): +0x24 the flags (the package's PKDT flags, then the node's own and-not /
//                  or overrides), +0x28 the interrupt flags, +0x2B the preferred speed. A changed field in the same
//                  frame as the reset says a node began (R2 Q6: the reset is a node transition, INFERRED there).
//   package swap   every change of the running package while a window is open (at most 4 lines): from / to, and
//                  the procedure type (18 = a package run from a template's procedure tree: R2 Q6's combat-override
//                  swap, e.g. 0x00109124 PlayerFollowerCombatOverridePackageExterior).
//   pause / menus  at a stalled re-pick: UI::GameIsPaused (numPausesGame), the open menus (those that pause the
//                  game marked *), and the wait split into run / paused / stalled time (F-5).
//   anchor flag    the anchor's form flag 0x10 at the open (the engine's own TCL toggle writes exactly that bit,
//                  AE 22825 -> 14634; Harbinger never writes it: F-8 is not built).
//   sink order     once at kDataLoaded: the TESCombatEvent source's sink array in index order (the send loop walks
//                  it in that order and stops at the first sink that returns non-zero, AE 33305 0x55CDE3), each
//                  sink's module and, for the engine's, its RTTI class; whether Harbinger's sink precedes the
//                  Papyrus VM's (F-4 needs that answer first; nothing is filtered).
//   (The two dialogue timers of F-2 are read by channels/SubstrateBark.cpp inside the engine's locked group update,
//   because this file never dereferences a CombatGroup; its BARK DENY line carries them.)
//
// RULES (INVARIANTS #17, CLAUDE.md "Probes must be FULLY PASSIVE"): plain member reads of CommonLib members
// and calls the substrate's own code already makes at this seat (GetPosition, GetCurrentScene,
// GetOccupiedFurniture, MenuTopicManager::speaker behind its verified rows, UI::IsMenuOpen). No engine
// write, no hook, no new address id, no CombatGroup dereference (the controller pointer is only tested
// for null, as HasController does). Summary or change-only lines, capped per window. Game thread only:
// every entry point runs from combatsubstrate's Poll seat (APMF-B26's quiescence assumption), so the
// state below has no lock.
//
// Every engine field, verified (fork 57be9d67, the registry baseline's pin; disassembly where noted):
//   AIProcess+0x136 lowProcessFlags (bit 2: the aggressor bit) -- CombatSubstrate.cpp's static_assert + read.
//   AIProcess+0x137 processLevel -- Actor::GetProcessLevel AE 0x69C6D0 / SE 0x608DE0 `movzx eax, byte [rax+0x137]`.
//   AIProcess+0x18 ActorPackage: +0x08 package, +0x20 packageStartTime (CombatSubstrate.cpp's static_assert),
//     +0x24 modifiedPackageFlag -- UpdateIgnoringCombat AE 0x6D7E80: `mov eax, [rax+0x24]` (the ActorPackage's
//     flags), else `mov eax, [r8+0x20]` (the TESPackage's PKDT flags), then `shr eax, 0x14; test al, 1` (bit 20,
//     kIgnoreCombat) decides +0x139.
//   TESPackage+0x20 packData: +0 packFlags (above), +4 packType (CombatSubstrate.cpp PackageOf reads it).
//   ActorPackage +0x10 data, +0x1C currentProcedureIndex (written by the ActorPackage methods AE 38957..38959 and
//     clamped to the package's procedure count by AE 38967 0x6CE110), +0x24 / +0x28 / +0x2B (AE 38970 0x6CE2D7 /
//     0x6CE2DE / 0x6CE2E7, SE 38015 0x63BD57 / 0x63BD5E / 0x63BD67) -- CommonLib's ActorPackage, static_asserts below.
//   TESForm+0x10 formFlags bit 4 (the anchor's collision): the TCL setter AE 14634 / SE 14477 writes only that bit.
//   UI::numPausesGame (CommonLib's inline GameIsPaused), UI::menuMap entries, IMenu::OnStack / PausesGame.
//   BSTEventSource<TESCombatEvent>: CommonLib's inline sinks array read under its own lock (the AddEventSink path).
//   Actor boolBits (GetActorRuntimeData, per-runtime): kEvpBuffered bit 16 and kResetAI bit 17 (EvaluatePackage's
//     immediate path clears both: AE 0x66D117 `and [rdi+0xE8], 0xFFFCFFFF` / SE 0x5DB713 on +0xE0); kProcessMe
//     bit 1 (Actor::IsAIEnabled in the fork; the anchor's AI-off write in CombatSubstrate.cpp).
//   ActorState (AsActorState: SE Actor+0xB8, AE +0xC0) actorState2 at +0x0C, weaponState bits 5..7 -- SE
//     0x1C7BD9 `mov eax, [rbx+0xC4]; shr eax, 5; and eax, 7; cmp al, 3`, AE 0x21431B `mov eax, [rbx+0xCC];
//     and eax, 0xE0; cmp eax, 0x60` (3 = kDrawn; the fork's IsWeaponDrawn = kDrawn / kWantToSheathe / kSheathing).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char*   kIni           = "Data/SKSE/Plugins/APMF.ini";
    constexpr std::uint64_t kSampleMs      = 250;     // the MOVE cadence of CombatSubstrate.cpp's P6 trace
    constexpr std::uint64_t kPreMs         = 1000;    // baseline before a dry-run request
    constexpr std::uint64_t kAfterMoveMs   = 3000;    // baseline after the close
    constexpr std::uint64_t kAggr1Ms       = 1000;
    constexpr std::uint64_t kAggr5Ms       = 5000;
    constexpr std::uint64_t kSheatheMaxMs  = 10000;
    constexpr std::uint64_t kUnopenedMs    = 5000;    // a requested window that never opened (Begin refused it)
    constexpr std::uint64_t kNoCloseMs     = 60000;   // an open window with no close seen (TTL is clamped to 15 s)
    constexpr std::size_t   kMaxWindows    = 32;
    constexpr std::size_t   kMaxPreActors  = 16;
    constexpr std::size_t   kMaxPreSamples = 8;
    constexpr std::uint32_t kMaxResetLines = 8;       // per window; the count is in the window's last line
    constexpr std::uint8_t  kAggressorBit  = 0x04;    // AIProcess+0x136 bit 2
    constexpr std::uint32_t kIgnoreCombat  = 1u << 20;
    constexpr std::uint32_t kFormCollisionOff = 1u << 4;   // TESForm flag the console's TCL toggles (R2 Q8)

    bool g_enabled    = false;
    bool g_dialogueOk = false;

    struct Stats {
        std::uint32_t n = 0, dn = 0;
        double        sum = 0.0;
        float         mn = std::numeric_limits<float>::infinity(), mx = 0.0f;
        float         d0 = 0.0f, d1 = 0.0f, dmn = std::numeric_limits<float>::infinity(), dmx = 0.0f;
        void          Speed(float v) {
            ++n;
            sum += v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        void Dist(float d) {
            if (d < 0.0f) return;
            if (dn++ == 0) d0 = d;
            d1  = d;
            dmn = std::min(dmn, d);
            dmx = std::max(dmx, d);
        }
        std::string Text() const {
            std::string sp = n ? fmt::format("speed {:.0f}/{:.0f}/{:.0f} (min/avg/max, {} samples)", static_cast<double>(mn),
                                             sum / n, static_cast<double>(mx), n)
                               : std::string("speed n/a");
            std::string di = dn ? fmt::format("dist-to-player {:.0f}->{:.0f} [{:.0f}..{:.0f}]", static_cast<double>(d0),
                                              static_cast<double>(d1), static_cast<double>(dmn), static_cast<double>(dmx))
                                : std::string("dist n/a");
            return sp + ", " + di;
        }
    };

    enum Stage : int { kPre = 0, kOpen = 1, kPost = 2 };

    struct Window {
        Stage         stage       = kPre;
        bool          dryRun      = false;
        std::uint64_t requestedMs = 0, openedMs = 0, closedMs = 0;
        // movement
        bool          havePos = false;
        RE::NiPoint3  prevPos{};
        std::uint64_t prevPosMs = 0, lastSampleMs = 0;
        Stats         st[3]{};
        bool          moveLogged = false;
        // the running package, every frame (the "changed in the same frame" test of a reset)
        bool          havePkg = false, havePrevPkg = false;
        RE::FormID    curPkg = 0, prevPkg = 0;
        float         curStart = 0.0f, prevStart = 0.0f;
        // aggressor bit: -1 not read yet, -2 no AI process at that moment
        int           aggrPre = -1, aggrClose = -1, aggr1 = -1, aggr5 = -1;
        // hands after the close
        bool          dropped = false, sheatheDone = false;
        int           weaponAtClose = -1;
        std::uint64_t notDrawnMs = std::numeric_limits<std::uint64_t>::max();   // ms after the close; max = not seen
        std::uint64_t sheathedMs = std::numeric_limits<std::uint64_t>::max();
        // resets / re-pick
        std::uint32_t otherResets = 0, resetLines = 0;
        bool          repickLogged = false;
        // R5 F-7: the ActorPackage's procedure fields, this Poll and the previous one
        struct Proc {
            std::int32_t  index = 0;
            std::uint32_t flags = 0;
            std::uint16_t interrupt = 0;
            std::int8_t   speed = 0;
            std::uintptr_t data = 0;
            bool operator==(const Proc&) const = default;
        };
        Proc          curProc{}, prevProc{};
        // R5 F-7: package swaps while open (the package at the entry, the last one seen open), and the anchor's flag
        RE::FormID    entryPkg = 0, openPkg = 0;
        bool          openPkgSet = false;
        std::uint32_t swaps = 0, swapLines = 0;
        int           anchorCollisionOff = -1;   // form flag 0x10 at the open; -1 not read
    };
    std::unordered_map<RE::FormID, Window> g_win;
    std::uint64_t                          g_capLogMs = 0, g_capSkipped = 0;

    struct PreSample {
        std::uint64_t ms;
        RE::NiPoint3  pos;
        float         dist;
    };
    std::unordered_map<RE::FormID, std::vector<PreSample>> g_pre;
    std::uint64_t                                          g_preDue = 0, g_preLastMs = 0;
    std::uint64_t                                          g_repickLastMs = 0;

    int IniInt(const char* a_sec, const char* a_key, int a_def) {
        char b[32]{};
        REX::W32::GetPrivateProfileStringA(a_sec, a_key, "", b, sizeof(b), kIni);
        if (!b[0]) return a_def;
        return std::atoi(b);
    }

    const char* WeaponName(int w) {
        switch (w) {
        case 0:  return "sheathed";
        case 1:  return "want-to-draw";
        case 2:  return "drawing";
        case 3:  return "drawn";
        case 4:  return "want-to-sheathe";
        case 5:  return "sheathing";
        default: return "?";
        }
    }

    const char* LevelName(std::uint8_t l) {
        switch (l) {
        case 0:  return "high";
        case 1:  return "middle-high";
        case 2:  return "middle-low";
        case 3:  return "low";
        default: return "none";
        }
    }

    constexpr std::uint32_t kMaxSwapLines = 4;

    Window::Proc ProcOf(const RE::AIProcess* p) {
        Window::Proc r{};
        if (!p) return r;
        const auto& ap = p->currentPackage;
        r.index     = ap.currentProcedureIndex;
        r.flags     = ap.modifiedPackageFlag;
        r.interrupt = ap.modifiedInterruptFlag;
        r.speed     = ap.preferredSpeed;
        r.data      = reinterpret_cast<std::uintptr_t>(ap.data);
        return r;
    }

    std::string ProcText(const Window::Proc& a) {
        return fmt::format("proc-index={} data=0x{:X} flags=0x{:08X} interrupt=0x{:04X} speed={}", a.index, a.data, a.flags,
                           a.interrupt, static_cast<int>(a.speed));
    }

    std::string PkgName(RE::FormID a_id) {
        auto* pkg = a_id ? RE::TESForm::LookupByID<RE::TESPackage>(a_id) : nullptr;
        if (!pkg) return fmt::format("0x{}", Hex(a_id));
        const auto t = static_cast<std::uint32_t>(pkg->packData.packType.underlying());
        return fmt::format("0x{} (type {}{})", Hex(a_id), t, t == 18 ? ": runs a template's procedure tree" : "");
    }

    // ---- R5 F-4: the TESCombatEvent sink order (passive, once) --------------------------------------------------
    // The module image owning a DATA address (a vtable): GetModuleHandleEx FROM_ADDRESS | UNCHANGED_REFCOUNT, the size
    // from the PE header (e_lfanew +0x3C, SizeOfImage at NT +0x50). Not sources::ModuleRange (it follows jmp stubs,
    // which is for code). False = in no module.
    bool ImageOf(std::uintptr_t a_addr, std::uintptr_t& a_lo, std::uintptr_t& a_hi, std::string& a_name) {
        void* mod = nullptr;
        if (!GetModuleHandleExA(0x6, reinterpret_cast<const char*>(a_addr), &mod) || !mod) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mod);
        const auto nt   = base + *reinterpret_cast<const std::uint32_t*>(base + 0x3C);
        a_lo            = base;
        a_hi            = base + *reinterpret_cast<const std::uint32_t*>(nt + 0x50);
        char path[260]{};
        const auto n = REX::W32::GetModuleFileNameA(reinterpret_cast<REX::W32::HMODULE>(mod), path, sizeof(path));
        std::string_view pv(path, n < sizeof(path) ? n : 0);
        if (const auto cut = pv.find_last_of("\\/"); cut != std::string_view::npos) pv.remove_prefix(cut + 1);
        a_name = pv.empty() ? std::string("?") : std::string(pv);
        return a_hi > a_lo;
    }

    // The COMPLETE object's RTTI name of the object whose vtable is `a_vt` (MSVC x64: vtable[-1] = the
    // CompleteObjectLocator {sig 1, offset, cdOffset, TypeDescriptor rva +0x0C, hierarchy rva, self rva +0x14}; the
    // TypeDescriptor's name at +0x10). Every read is bounds-checked against the owning image; "" when anything fails.
    std::string RttiOf(std::uintptr_t a_vt) {
        std::uintptr_t lo = 0, hi = 0;
        std::string    mod;
        if (!ImageOf(a_vt, lo, hi, mod) || a_vt < lo + 8 || a_vt >= hi) return {};
        const auto col = *reinterpret_cast<const std::uintptr_t*>(a_vt - 8);
        if (col < lo || col + 0x18 > hi) return {};
        if (*reinterpret_cast<const std::uint32_t*>(col) != 1) return {};
        if (col - *reinterpret_cast<const std::uint32_t*>(col + 0x14) != lo) return {};
        const auto td = lo + *reinterpret_cast<const std::uint32_t*>(col + 0x0C);
        if (td + 0x10 >= hi) return {};
        const char* nm = reinterpret_cast<const char*>(td + 0x10);
        std::string out;
        for (std::size_t i = 0; i < 160 && td + 0x10 + i < hi && nm[i]; ++i) {
            if (nm[i] < 0x20 || nm[i] > 0x7E) return {};
            out.push_back(nm[i]);
        }
        return out;
    }

    std::string AggrText(int v) { return v == -1 ? std::string("not read") : v == -2 ? std::string("no process") : std::to_string(v); }

    int AggrOf(const RE::AIProcess* p) { return p ? ((p->lowProcessFlags.underlying() & kAggressorBit) ? 1 : 0) : -2; }

    int WeaponOf(RE::Actor* a) {
        const auto* as = a ? a->AsActorState() : nullptr;
        return as ? static_cast<int>(static_cast<std::uint32_t>(as->GetWeaponState())) : -1;
    }

    float DistToPlayer(const RE::NiPoint3& a_pos) {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc ? a_pos.GetDistance(pc->GetPosition()) : -1.0f;
    }

    void AddSample(Window& w, const RE::NiPoint3& a_pos, float a_dist, std::uint64_t a_ms) {
        Stats& s = w.st[w.stage];
        if (w.havePos && a_ms > w.prevPosMs) {
            const float dt = static_cast<float>(a_ms - w.prevPosMs) / 1000.0f;
            s.Speed(a_pos.GetDistance(w.prevPos) / dt);
        }
        s.Dist(a_dist);
        w.havePos      = true;
        w.prevPos      = a_pos;
        w.prevPosMs    = a_ms;
        w.lastSampleMs = a_ms;
    }

    // The package fields of one line: id, procedure type, start time, both flag words and their IgnoreCombat bit.
    std::string PackageText(const RE::AIProcess* p) {
        if (!p) return "no AI process";
        const auto* pkg = p->currentPackage.package;
        if (!pkg) return fmt::format("pkg=none actor-pkg-flags=0x{:08X}", p->currentPackage.modifiedPackageFlag);
        const std::uint32_t af = p->currentPackage.modifiedPackageFlag;
        const std::uint32_t pf = pkg->packData.packFlags.underlying();
        return fmt::format("pkg=0x{} type={} start={:.2f} actor-pkg-flags=0x{:08X} (IgnoreCombat {}) pkg-flags=0x{:08X} "
                           "(IgnoreCombat {})",
                           Hex(pkg->GetFormID()), static_cast<std::uint32_t>(pkg->packData.packType.underlying()),
                           static_cast<double>(p->currentPackage.packageStartTime), af, (af & kIgnoreCombat) ? 1 : 0, pf,
                           (pf & kIgnoreCombat) ? 1 : 0);
    }

    void LogMove(RE::FormID a_id, Window& w, const char* a_note) {
        if (w.moveLogged) return;
        w.moveLogged = true;
        spdlog::info("[substrate] 0x{} MOVE-BASELINE ({}{}) before=[{}] open=[{} over {} ms] after-3s=[{}]{}{}", Hex(a_id),
                     w.dryRun ? "dry run" : "client", w.dryRun && w.st[kPre].n == 0 ? ", no pre-request samples" : "",
                     w.st[kPre].Text(), w.st[kOpen].Text(), w.closedMs > w.openedMs ? w.closedMs - w.openedMs : 0,
                     w.st[kPost].Text(), a_note ? " -- " : "", a_note ? a_note : "");
    }

    void LogEnd(RE::FormID a_id, Window& w, const char* a_note) {
        LogMove(a_id, w, a_note);
        std::string hands;
        if (w.dropped)
            hands = "n/a (dropped for real combat: the fight keeps the hands)";
        else if (w.weaponAtClose < 0)
            hands = "not read";
        else {
            hands = fmt::format("{} at the close", WeaponName(w.weaponAtClose));
            constexpr auto kNever = std::numeric_limits<std::uint64_t>::max();
            hands += w.notDrawnMs == kNever ? fmt::format(", still drawn after {} ms", kSheatheMaxMs)
                                            : fmt::format(", not drawn at +{} ms", w.notDrawnMs);
            hands += w.sheathedMs == kNever ? ", not sheathed in the window" : fmt::format(", sheathed at +{} ms", w.sheathedMs);
        }
        spdlog::info("[substrate] 0x{} PROBE after-close: aggressor-bit (AIProcess+0x136 bit 2) before-entry={} close={} "
                     "+1s={} +5s={}; hands: {}; other ignoring-bit resets {} ({} logged); package swaps while open {} "
                     "(entry package {}); anchor form flag 0x10 (collision off) at the open {}{}{}",
                     Hex(a_id), AggrText(w.aggrPre), AggrText(w.aggrClose), AggrText(w.aggr1), AggrText(w.aggr5), hands,
                     w.otherResets, w.resetLines, w.swaps, PkgName(w.entryPkg),
                     w.anchorCollisionOff < 0 ? std::string("not read") : std::to_string(w.anchorCollisionOff),
                     a_note ? " -- " : "", a_note ? a_note : "");
    }

    // A new window for `a_id`; an old one still in its post-close timers is finished first. Null = the cap.
    Window* NewWindow(RE::FormID a_id, bool a_dryRun, std::uint64_t a_now) {
        if (auto it = g_win.find(a_id); it != g_win.end()) {
            if (it->second.stage == kPost) LogEnd(a_id, it->second, "cut short: a new substrate opened on this actor");
            g_win.erase(it);
        }
        if (g_win.size() >= kMaxWindows) {
            ++g_capSkipped;
            if (a_now - g_capLogMs >= 60000) {
                g_capLogMs = a_now;
                spdlog::info("[substrate] PROBE window cap ({}) reached: {} window(s) not tracked so far.", kMaxWindows,
                             g_capSkipped);
            }
            return nullptr;
        }
        Window& w     = g_win[a_id];
        w.dryRun      = a_dryRun;
        w.requestedMs = a_now;
        return &w;
    }

    static_assert(offsetof(RE::AIProcess, processLevel) == 0x137,
                  "AIProcess+0x137 processLevel (GetProcessLevel AE 0x69C6D0 / SE 0x608DE0 read it): re-verify");
    static_assert(offsetof(RE::ActorPackage, modifiedPackageFlag) == 0x24,
                  "ActorPackage+0x24 (UpdateIgnoringCombat AE 0x6D7EA1 reads it): re-verify");
    static_assert(offsetof(RE::TESPackage, packData) == 0x20 && offsetof(RE::PACKAGE_DATA, packFlags) == 0 &&
                      offsetof(RE::PACKAGE_DATA, packType) == 4,
                  "TESPackage+0x20 PKDT flags (UpdateIgnoringCombat AE 0x6D7EAB reads it), +0x24 the procedure type: re-verify");
    static_assert(static_cast<std::uint32_t>(RE::PACKAGE_DATA::GeneralFlag::kIgnoreCombat) == kIgnoreCombat,
                  "IgnoreCombat is bit 20 (UpdateIgnoringCombat `shr eax, 0x14; test al, 1`): re-verify");
    static_assert(offsetof(RE::ActorState, actorState2) == 0x0C,
                  "ActorState+0x0C actorState2 (weaponState bits 5..7: SE [actor+0xC4], AE [actor+0xCC]): re-verify");
    static_assert(static_cast<std::uint32_t>(RE::Actor::BOOL_BITS::kEvpBuffered) == (1u << 16) &&
                      static_cast<std::uint32_t>(RE::Actor::BOOL_BITS::kResetAI) == (1u << 17) &&
                      static_cast<std::uint32_t>(RE::Actor::BOOL_BITS::kProcessMe) == (1u << 1),
                  "EvaluatePackage's immediate path clears boolBits 16-17 (AE 0x66D117 / SE 0x5DB713); kProcessMe is bit 1 "
                  "(Actor::IsAIEnabled): re-verify");
    static_assert(static_cast<std::uint32_t>(RE::WEAPON_STATE::kSheathed) == 0 &&
                      static_cast<std::uint32_t>(RE::WEAPON_STATE::kDrawn) == 3,
                  "weaponState 0 = sheathed, 3 = drawn (the engine's `cmp 3` / `cmp 0x60`): re-verify");

}

namespace apmf::substrateprobe {

    void Install(bool a_dialogueOk) {
        g_dialogueOk = a_dialogueOk;
        g_enabled    = IniInt("CombatSubstrate", "bSubstrateProbes", 1) != 0;
        spdlog::info("[substrate] R1 probes {} ([CombatSubstrate] bSubstrateProbes): MOVE-BASELINE per window (1 s before a "
                     "dry-run request, 3 s after the close), the re-pick context, each OTHER ignoring-bit reset, the aggressor "
                     "bit around the window, the hands after the close. Passive: reads only.{}",
                     g_enabled ? "ON" : "off",
                     g_enabled && !a_dialogueOk ? " Dialogue-with-player is logged as unknown (MenuTopicManager row unverified)."
                                                : "");
    }

    bool Enabled() { return g_enabled; }

    void Reset() {
        g_win.clear();
        g_pre.clear();
        g_preDue = g_preLastMs = 0;
    }

    void BeforeDryRound(std::uint64_t a_now, std::uint64_t a_dueMs) {
        if (!g_enabled || a_dueMs <= a_now || a_dueMs - a_now > kPreMs) return;
        if (g_preDue != a_dueMs) {   // a new round: forget the last round's leftovers (non-picks)
            g_pre.clear();
            g_preDue    = a_dueMs;
            g_preLastMs = 0;
        }
        if (g_preLastMs && a_now - g_preLastMs < kSampleMs) return;
        g_preLastMs = a_now;
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;
        pl->ForEachHighActor([&](RE::Actor* a) {
            if (!a || !a->IsPlayerTeammate() || a->IsDead() || !a->Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
            const RE::FormID id = a->GetFormID();
            auto             it = g_pre.find(id);
            if (it == g_pre.end()) {
                if (g_pre.size() >= kMaxPreActors) return RE::BSContainer::ForEachResult::kContinue;
                it = g_pre.emplace(id, std::vector<PreSample>{}).first;
            }
            if (it->second.size() < kMaxPreSamples) {
                const RE::NiPoint3 pos = a->GetPosition();
                it->second.push_back(PreSample{ a_now, pos, DistToPlayer(pos) });
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    void Requested(RE::FormID a_actor, std::uint64_t a_now) {
        if (!g_enabled) return;
        Window* w = NewWindow(a_actor, true, a_now);
        if (!w) return;
        if (auto it = g_pre.find(a_actor); it != g_pre.end()) {
            for (const auto& s : it->second) AddSample(*w, s.pos, s.dist, s.ms);
            g_pre.erase(it);
        }
    }

    void BeforeEntry(RE::FormID a_actor, const RE::AIProcess* a_proc, std::uint64_t a_now) {
        if (!g_enabled) return;
        auto    it = g_win.find(a_actor);
        Window* w  = (it != g_win.end() && it->second.stage == kPre) ? &it->second : NewWindow(a_actor, false, a_now);
        if (w) {
            w->aggrPre  = AggrOf(a_proc);
            w->entryPkg = (a_proc && a_proc->currentPackage.package) ? a_proc->currentPackage.package->GetFormID() : 0;
        }
    }

    void Opened(RE::FormID a_actor, std::uint64_t a_now, const RE::TESObjectREFR* a_anchor) {
        if (!g_enabled) return;
        auto it = g_win.find(a_actor);
        if (it == g_win.end() || it->second.stage != kPre) return;
        it->second.stage    = kOpen;
        it->second.openedMs = a_now;
        if (a_anchor) it->second.anchorCollisionOff = (a_anchor->GetFormFlags() & kFormCollisionOff) ? 1 : 0;   // F-7
    }

    void RepickNotRun(RE::Actor* a_actor, const RE::AIProcess* a_proc, std::uint64_t a_waitedMs, std::uint64_t a_runMs,
                      std::uint64_t a_pausedMs, std::uint64_t a_stalledMs) {
        if (!g_enabled || !a_actor) return;
        const RE::FormID id = a_actor->GetFormID();
        auto             it = g_win.find(id);
        if (it != g_win.end()) {
            if (it->second.repickLogged) return;
            it->second.repickLogged = true;
        }
        const auto now = apmf::clock::MonotonicMs();
        if (it == g_win.end() && g_repickLastMs && now - g_repickLastMs < 1000) return;   // untracked: at most 1 / s
        g_repickLastMs = now;
        const auto&         rd   = a_actor->GetActorRuntimeData();
        const std::uint32_t bits = rd.boolBits.underlying();
        const std::uint8_t  lvl  = a_proc ? static_cast<std::uint8_t>(a_proc->processLevel.underlying()) : std::uint8_t{ 0xFF };
        const char*         dlg  = "unknown (MenuTopicManager row unverified)";
        if (g_dialogueOk) {
            const auto* mtm = RE::MenuTopicManager::GetSingleton();
            dlg = !mtm ? "unknown (no MenuTopicManager)"
                : (mtm->speaker.native_handle() != 0 && mtm->speaker.native_handle() == a_actor->GetHandle().native_handle())
                    ? "y"
                    : "n";
        }
        auto*       ui      = RE::UI::GetSingleton();
        const bool  dlgMenu = ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        const auto* as      = a_actor->AsActorState();
        // R5 F-5 / F-7: the pause and the open menus (a pausing menu stops the actor's AI update, the re-pick's only road)
        std::string menus;
        if (ui) {
            for (const auto& kv : ui->menuMap) {
                const auto* m = kv.second.menu.get();
                if (!m || !m->OnStack()) continue;
                if (menus.size() > 240) {
                    menus += ",...";
                    break;
                }
                menus += fmt::format("{}{}{}", menus.empty() ? "" : ",", kv.first.c_str(), m->PausesGame() ? "*" : "");
            }
        }
        spdlog::info("[substrate] 0x{} PROBE re-pick NOT run after {} ms ({} ms of it unpaused, {} ms paused, {} ms stalled "
                     "between Polls); game-paused={} (numPausesGame {}), open menus [{}] (* pauses the game); {}; "
                     "boolBits=0x{:08X} (kEvpBuffered {}, kResetAI {}, "
                     "kProcessMe/AI-on {}); process level {} ({}), lowProcessFlags=0x{:02X}; furniture={} sit-sleep-state={}; "
                     "dialogue-with-player={} dialogue-menu={} scene={}; combat-controller={}",
                     Hex(id), a_waitedMs, a_runMs, a_pausedMs, a_stalledMs, ui && ui->GameIsPaused() ? "y" : "n",
                     ui ? ui->numPausesGame : 0u, menus.empty() ? std::string("none") : menus, PackageText(a_proc), bits,
                     (bits >> 16) & 1u, (bits >> 17) & 1u, (bits >> 1) & 1u,
                     static_cast<std::uint32_t>(lvl), LevelName(lvl), a_proc ? static_cast<std::uint32_t>(a_proc->lowProcessFlags.underlying()) : 0u,
                     a_actor->GetOccupiedFurniture() ? "y" : "n",
                     as ? static_cast<std::uint32_t>(as->GetSitSleepState()) : 0xFFu, dlg, dlgMenu ? "open" : "closed",
                     a_actor->GetCurrentScene() ? "y" : "n", rd.combatController ? "y" : "n");
    }

    void OtherReset(RE::FormID a_actor, const RE::AIProcess* a_proc, std::uint64_t a_now) {
        if (!g_enabled) return;
        auto it = g_win.find(a_actor);
        if (it == g_win.end()) return;
        Window& w = it->second;
        ++w.otherResets;
        if (w.resetLines >= kMaxResetLines) return;
        ++w.resetLines;
        // "this frame" = since the probe's previous Poll sample of the package (the reset was seen between Serve's
        // previous pass and this one, the same interval).
        const auto*      pkg   = a_proc ? a_proc->currentPackage.package : nullptr;
        const RE::FormID nowId = pkg ? pkg->GetFormID() : 0;
        const float      nowSt = a_proc ? a_proc->currentPackage.packageStartTime : 0.0f;
        const std::string changed =
            !w.havePrevPkg ? std::string("unknown (no previous frame sampled)")
            : (w.prevPkg != nowId || w.prevStart != nowSt)
                ? fmt::format("YES (previous frame: pkg 0x{} start {:.2f})", Hex(w.prevPkg), static_cast<double>(w.prevStart))
                : std::string("no");
        // R5 F-7: the procedure node. Every tree-node BEGIN rewrites +0x24 / +0x28 / +0x2B (AE 38970); a change since
        // the previous Poll in the frame of the reset says a node began.
        const Window::Proc now = ProcOf(a_proc);
        const std::string  node =
            !w.havePrevPkg ? fmt::format("now [{}], previous Poll not sampled", ProcText(now))
            : now == w.prevProc
                ? fmt::format("[{}], unchanged since the previous Poll (no node begin seen)", ProcText(now))
                : fmt::format("[{}] <- previous Poll [{}]: CHANGED (a procedure node began)", ProcText(now),
                              ProcText(w.prevProc));
        spdlog::info("[substrate] 0x{} PROBE other ignoring-bit reset #{} at +{} ms after the open: {}; package change in "
                     "this frame: {}; procedure {}{}",
                     Hex(a_actor), w.otherResets, w.openedMs && a_now >= w.openedMs ? a_now - w.openedMs : 0,
                     PackageText(a_proc), changed, node,
                     w.resetLines == kMaxResetLines ? fmt::format(" (line cap {} for this window: the rest are counted)",
                                                                  kMaxResetLines)
                                                    : std::string());
    }

    void CombatEventSinkOrder(RE::ScriptEventSourceHolder* a_holder, const void* a_harbinger) {
        if (!g_enabled || !a_holder) return;
        auto* src = a_holder->GetEventSource<RE::TESCombatEvent>();
        if (!src) return;
        std::vector<const void*> sinks;
        std::size_t              pendingAdd = 0;
        {
            RE::BSSpinLockGuard lk(src->lock);   // the source's own lock (CommonLib's inline AddEventSink takes it too)
            for (auto* sk : src->sinks) sinks.push_back(sk);
            pendingAdd = src->pendingRegisters.size();
        }
        std::string list;
        int         mine = -1, vm = -1;
        for (std::size_t i = 0; i < sinks.size(); ++i) {
            const auto*       sk = sinks[i];
            const auto        vt = sk ? *reinterpret_cast<const std::uintptr_t*>(sk) : 0;
            std::uintptr_t    lo = 0, hi = 0;
            std::string       mod;
            const bool        inMod = vt && ImageOf(vt, lo, hi, mod);
            const std::string cls   = inMod ? RttiOf(vt) : std::string();
            if (sk == a_harbinger) mine = static_cast<int>(i);
            if (vm < 0 && cls == ".?AVSkyrimVM@@") vm = static_cast<int>(i);
            list += fmt::format("{}#{} {}{}{}{}", list.empty() ? "" : "; ", i, inMod ? mod : std::string("no module"),
                                cls.empty() ? "" : " ", cls, sk == a_harbinger ? " (HARBINGER)" : "");
        }
        spdlog::info("[substrate] PROBE TESCombatEvent sink order at kDataLoaded ({} sink(s), {} pending registration(s); "
                     "the send loop walks them in this order and stops at the first that returns non-zero, AE 33305): {}. "
                     "Harbinger's sink #{}, the Papyrus VM (SkyrimVM) #{} -> {}",
                     sinks.size(), pendingAdd, list.empty() ? std::string("none") : list, mine, vm,
                     mine < 0 || vm < 0 ? std::string("UNKNOWN (one of them is not in the array)")
                     : mine < vm        ? std::string("Harbinger is AHEAD of the VM (an F-4 filter could be a plain sink)")
                                        : std::string("Harbinger is BEHIND the VM (an F-4 filter would need the sink "
                                                      "array reordered: a question for marth, not a code change)"));
    }

    void Closing(RE::FormID a_actor, RE::Actor* a_ref, bool a_opened, bool a_dropped, std::uint64_t a_now) {
        if (!g_enabled) return;
        auto it = g_win.find(a_actor);
        if (it == g_win.end()) return;
        if (!a_opened) {   // closed before the entry: CloseSub logged it; no window to summarise
            g_win.erase(it);
            return;
        }
        Window& w   = it->second;
        w.stage     = kPost;
        w.closedMs  = a_now;
        w.dropped   = a_dropped;
        auto* proc  = a_ref ? a_ref->GetActorRuntimeData().currentProcess : nullptr;
        w.aggrClose = AggrOf(proc);
        if (a_dropped) {
            w.sheatheDone = true;
            return;
        }
        w.weaponAtClose = WeaponOf(a_ref);
        if (w.weaponAtClose >= 0 && w.weaponAtClose < 3) w.notDrawnMs = 0;   // not drawn at the close (IsWeaponDrawn)
        if (w.weaponAtClose == 0) {
            w.sheathedMs  = 0;
            w.sheatheDone = true;
        }
        if (w.weaponAtClose < 0) w.sheatheDone = true;
    }

    void Poll(std::uint64_t a_now) {
        if (!g_enabled) return;
        if (!g_pre.empty() && a_now > g_preDue + kPreMs) g_pre.clear();   // the round passed; leftovers are non-picks
        if (g_win.empty()) return;
        for (auto it = g_win.begin(); it != g_win.end();) {
            const RE::FormID id = it->first;
            Window&          w  = it->second;
            auto*            a  = RE::TESForm::LookupByID<RE::Actor>(id);
            const bool       ok = a && a->Is3DLoaded() && !a->IsDead();
            if (w.stage == kPre && a_now - w.requestedMs > kUnopenedMs) {   // Begin / Enter refused it (logged there)
                it = g_win.erase(it);
                continue;
            }
            if (w.stage == kOpen && a_now - w.openedMs > kNoCloseMs) {
                LogEnd(id, w, "no close seen within 60 s (the probe stops)");
                it = g_win.erase(it);
                continue;
            }
            if (!ok) {
                if (w.stage == kPost) {
                    LogEnd(id, w, "the actor is gone, unloaded or dead (post-close timers cut)");
                    it = g_win.erase(it);
                    continue;
                }
                ++it;   // open or before: CloseSub follows (Serve closes an unloaded / dead owner)
                continue;
            }
            auto* proc = a->GetActorRuntimeData().currentProcess;
            // the running package, every frame
            if (proc) {
                w.havePrevPkg = w.havePkg;
                w.prevPkg     = w.curPkg;
                w.prevStart   = w.curStart;
                w.curPkg      = proc->currentPackage.package ? proc->currentPackage.package->GetFormID() : 0;
                w.curStart    = proc->currentPackage.packageStartTime;
                w.havePkg     = true;
                w.prevProc    = w.curProc;
                w.curProc     = ProcOf(proc);
                // R5 F-7: the package selector swapping a HELD body onto another package (R2 Q6: the combat override)
                if (w.stage == kOpen) {
                    const RE::FormID from = w.openPkgSet ? w.openPkg : w.entryPkg;
                    if (w.curPkg != from) {
                        ++w.swaps;
                        if (w.swapLines < kMaxSwapLines) {
                            ++w.swapLines;
                            spdlog::info("[substrate] 0x{} PROBE package SWAP while held at +{} ms: {} -> {}{}", Hex(id),
                                         a_now >= w.openedMs ? a_now - w.openedMs : 0, PkgName(from), PkgName(w.curPkg),
                                         w.swapLines == kMaxSwapLines ? " (line cap for this window: the rest are counted)"
                                                                      : "");
                        }
                    }
                    w.openPkg    = w.curPkg;
                    w.openPkgSet = true;
                }
            }
            const std::uint64_t since = w.stage == kPost ? a_now - w.closedMs : 0;
            // movement, every kSampleMs, until 3 s after the close
            if ((w.stage != kPost || since <= kAfterMoveMs) && (!w.havePos || a_now - w.lastSampleMs >= kSampleMs)) {
                const RE::NiPoint3 pos = a->GetPosition();
                AddSample(w, pos, DistToPlayer(pos), a_now);
            }
            if (w.stage != kPost) {
                ++it;
                continue;
            }
            if (since > kAfterMoveMs) LogMove(id, w, nullptr);
            if (w.aggr1 == -1 && since >= kAggr1Ms) w.aggr1 = AggrOf(proc);
            if (w.aggr5 == -1 && since >= kAggr5Ms) w.aggr5 = AggrOf(proc);
            if (!w.sheatheDone) {
                const int ws = WeaponOf(a);
                if (ws >= 0 && ws < 3 && w.notDrawnMs == std::numeric_limits<std::uint64_t>::max()) w.notDrawnMs = since;
                if (ws == 0) {
                    w.sheathedMs  = since;
                    w.sheatheDone = true;
                } else if (since >= kSheatheMaxMs) {
                    w.sheatheDone = true;
                }
            }
            if (w.moveLogged && w.aggr5 != -1 && w.sheatheDone) {
                LogEnd(id, w, nullptr);
                it = g_win.erase(it);
                continue;
            }
            ++it;
        }
    }

}
