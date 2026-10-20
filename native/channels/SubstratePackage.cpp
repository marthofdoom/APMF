#include "PCH.h"
#include "channels/SubstratePackage.h"

#include "channels/CombatSubstrate.h"   // StateOf / AnyLive (lock-free)
#include "core/Allowance.h"             // SeatVerified, RuntimeSupported, IsRuntime1_7_104
#include "core/Log.h"
#include "core/Sources.h"               // RegisterCallSeat (the seat census)

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

// ============================================================================
// F-6: THE HELD HOLDER'S PACKAGE STAYS IN CHARGE. Research: _evidence/2026-10-18-designs/SUBSTRATE-R2-RESEARCH.md
// item 6 (the 10-19 log: 12 of 12 re-picked windows on Sven ran his combat-override package 0x00109124
// PlayerFollowerCombatOverridePackageExterior, and each one reset the ignoring bit; 0 of the windows without the
// swap did), and this round's agent log apmf-substrate-f6.md.
//
// THE SWAP (disassembly of the unpacked 1.6.1170 and 1.5.97 images and the 1.7.104 image, 2026-10-20). The package
// selector (AE 37172 0x65DE90 / SE 36193 0x5CD930 / 1.7.104 0x670880; one caller, the package evaluation) is
//   rbx = Character slot 0x49 CheckForCurrentAliasPackage(actor)      AE +0x3E / SE +0x37 / 1.7.104 +0x3E
//   if !rbx: the NPC's AI-form packages, then its default package list, then the process's fallback
//   if (process && Actor+0x160 combat controller (SE +0x158) && !(rbx && rbx PKDT bit 20 IgnoreCombat) &&
//       (process+0x137 level != 0 || vfunc 0x70 Get3D2)):
//       r = GetEnterCombatOverridePackage(npc, actor)                  AE +0xF3 / SE +0xEA / 1.7.104 +0xF3
//       if r: rbx = r                                                  (`test rax,rax; cmovne rbx,rax`)
//   rbx's resolve (AE 29620), return rbx.
// GetEnterCombatOverridePackage (AE 24673 0x3B79C0 / SE 24169 0x35EE70 / 1.7.104 0x3BEA80) answers the actor's
// quest-alias ECOR package first (Actor+0x70 extra list -> the alias instance array -> AE 0x1828F0: Sven's
// DialogueFollower alias gives 0x00109124), then its NPC's BGSOverridePackCollection::enterCombatOverRidePackList
// (TESNPC+0x180 = +0x160 + 0x20, the fork's BGSOverridePackCollection.h) through the selector's own list picker.
// Nothing on this path is virtual, and slot 0x49 is asked BEFORE the swap: Harbinger's 0x49 seat (ch.9,
// core/PackageGate.cpp) cannot express this deny -- whatever it answers is replaced. So the seat is the selector's
// own call (write_call<5>); a null answer is the engine's own "no override" case, and the selector keeps rbx.
//
// WHO IS DENIED. Only an actor combatsubstrate::StateOf reports HELD (a live holder whose real-combat trigger has
// not fired). REAL holders, the anchor and everyone else get the engine's answer. Only the selector's call is
// seated, so only a package the engine picks BECAUSE the actor has a combat controller is ever withheld (every
// package this function returns is an enter-combat override, alias or NPC). A client's 0x49 offer is rbx: denying
// the override keeps it. Both sources (alias and NPC list) are denied: both are the combat package swap the
// ruling blocks.
//
// THE OTHER TWO CALLERS of GetEnterCombatOverridePackage (not seated, by reasoning):
//   AE 40079 0x71C020 (SE 39012 0x688270) runs the override only when the ignoring-bit getter (AE 39129 0x6D6780) reads
//     0: a held holder's bit is 1 (S2a's write, S2b's re-assert), so it skips the override.
//   AE 39360 0x6E2B80 (SE 38376 0x64FE60) is the flee answer: the override package if there is one, else Character
//     slot 0xDD InitiateFlee. Its callers are StartCombat's coward branch (AE 0x6B6C45: race flag, Aggression 0
//     AND Confidence 0) and two flee routes (AE 39637 / 39667). A held holder reaching it is a flee, not the
//     package swap this deny is for; listed for the reviewer (uncertain spot) and the deny audit.
//
// S2b (marth's flag-hold approval of record 2026-10-12, INVARIANTS #0 (j) 3; built in this round because the
// research shows writers of AIProcess+0x139 REMAIN after the deny): UpdateIgnoringCombat (AE 39181 0x6D7E80 /
// SE 38221 0x645470) writes the running package's IgnoreCombat into the bit from the package start (AE 39434, a
// package (re)start: seen once on Auri's own package in the 10-19 log) and from the procedure-tree node begin visitor
// (AE 29290, every Sequence / Stacked / Simultaneous / Random node entering a child, for ANY package's tree). The
// held package lacks IgnoreCombat, so each such event writes 0. Reassert writes 1 back, once per reset Serve sees,
// logs each one, and stops 15 s after the open (the substrate's TTL cap is 15 s too). DEFAULT OFF
// (bSubstrateBitReassert=0, F-6 review SEV-3): it switches on only after P6 and marth's go.
// ============================================================================

namespace apmf::substratepackage {

    namespace {

        using apmf::log::Hex;
        namespace subs = apmf::combatsubstrate;

        constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
        constexpr std::uint64_t kReassertCapMs = 15000;   // marth 2026-10-12: "capped at 15 s"
        constexpr std::uint32_t kReassertLines = 30;      // per window; the CLOSE summary carries the full count

        // The selector's call offsets (AE 0x65DF83 / SE 0x5CDA1A / 1.7.104 0x670973) and the 1.7.104 raw RVAs
        // (tools/verified_addresses idmap-1.7.104-apmf.csv rva:0x65DE90 -> 0x670880 and rva:0x3B79C0 -> 0x3BEA80;
        // VerifiedAddresses rows Substrate.PackageSelector.1_7_104 / Substrate.Npc.GetEnterCombatOverridePackage.1_7_104).
        constexpr std::uintptr_t kSiteOffAE   = 0xF3;
        constexpr std::uintptr_t kSiteOffSE   = 0xEA;
        constexpr std::uintptr_t kSiteOff17   = 0xF3;
        constexpr std::uintptr_t kSelector17  = 0x670880;
        constexpr std::uintptr_t kOverride17  = 0x3BEA80;

        static_assert(offsetof(RE::AIProcess, ignoringCombat) == 0x139,
                      "AIProcess+0x139 is the ignoring-combat bit UpdateIgnoringCombat writes (AE 0x6D7EBB / 0x6D7ED8)");
        static_assert(offsetof(RE::AIProcess, currentPackage) == 0x18 && offsetof(RE::ActorPackage, package) == 0x08,
                      "AIProcess+0x18 ActorPackage, +0x08 the running TESPackage (CombatSubstrate.cpp asserts the same)");

        using OverrideFn = RE::TESPackage* (*)(RE::TESNPC*, RE::Actor*);

        std::atomic<std::uintptr_t> g_orig{ 0 };          // the VERIFIED GetEnterCombatOverridePackage
        std::atomic<bool>           g_seatOn{ false };
        std::atomic<const char*>    g_seatState{ "not installed" };
        std::atomic<bool>           g_reassertOn{ true };
        bool                        g_installedOnce = false;   // game thread (kDataLoaded)
        const char*                 g_refusal       = nullptr; // Install's answer, repeated on a second call
        bool                        g_iniOff        = false;   // bSubstrateOverrideDeny=0: the one by-name hole (logged per OPEN)

        struct Window {
            RE::FormID    prePkg       = 0;       // running at Opening (just before StartCombat)
            bool          opened       = false;   // Opening ran (false: a denial for a holder Opening never saw)
            std::uint32_t denied       = 0;
            RE::FormID    loggedPkg    = 0;       // the override package last logged (one line per package per window)
            RE::FormID    lastPkg      = 0;
            std::uint32_t reasserts    = 0;
            std::uint32_t byRestart    = 0;
            std::uint32_t lines        = 0;
            bool          capLogged    = false;
        };
        std::mutex                             g_mx;   // LEAF: no engine call and no spdlog under it
        std::unordered_map<RE::FormID, Window> g_win;

        void NoteDenied(RE::FormID a_actor, RE::FormID a_pkg) {
            RE::FormID prev   = 0;
            bool       known  = false;
            bool       log    = false;
            {
                std::scoped_lock lk(g_mx);
                Window& w = g_win[a_actor];
                ++w.denied;
                w.lastPkg = a_pkg;
                if (w.loggedPkg != a_pkg) {
                    w.loggedPkg = a_pkg;
                    log         = true;
                }
                prev  = w.prePkg;
                known = w.opened;
            }
            if (log)
                spdlog::info("[substrate] 0x{} combat-override package 0x{} DENIED -- pre-substrate package was 0x{} (the "
                             "package selector keeps its own pick; held holder, F-6){}",
                             Hex(a_actor), Hex(a_pkg), Hex(prev),
                             known ? "" : " [no pre-substrate record: the window opened before this seat saw it]");
        }

        // The selector's call. ANY THREAD (wherever the engine evaluates packages).
        RE::TESPackage* OverrideThunk(RE::TESNPC* a_npc, RE::Actor* a_actor) {
            const auto      orig = reinterpret_cast<OverrideFn>(g_orig.load(std::memory_order_acquire));
            RE::TESPackage* pkg  = orig(a_npc, a_actor);
            if (!pkg || !a_actor || !g_seatOn.load(std::memory_order_relaxed) || !subs::AnyLive()) return pkg;
            const RE::FormID id = a_actor->GetFormID();
            if (subs::StateOf(id) != APMF_API::kSubstrateState_Held) return pkg;
            NoteDenied(id, pkg->GetFormID());
            return nullptr;   // the engine's own "no override": `test rax,rax; cmovne` keeps the selector's pick
        }

        bool IniOn(const char* a_key, const char* a_default) {
            char b[16]{};
            REX::W32::GetPrivateProfileStringA("CombatSubstrate", a_key, a_default, b, sizeof(b), kIni);
            return std::atoi(b) != 0;
        }

        RE::FormID Running(const RE::AIProcess* a_proc) {
            const auto* pkg = a_proc ? a_proc->currentPackage.package : nullptr;
            return pkg ? pkg->GetFormID() : 0;
        }

    }

    const char* Install() {
        if (g_installedOnce) return g_refusal;
        g_installedOnce = true;
        // S2b defaults OFF (F-6 review SEV-3): it waits for P6 and marth's go (INVARIANTS #0 (j) 3).
        g_reassertOn.store(IniOn("bSubstrateBitReassert", "0"), std::memory_order_relaxed);
        spdlog::info("[substrate] S2b ignoring-bit re-assert {} (capped at {} s from the open; every undone reset "
                     "logged).",
                     g_reassertOn.load() ? "ON" : "OFF ([CombatSubstrate] bSubstrateBitReassert=0)", kReassertCapMs / 1000);

        const char*    why  = nullptr;
        const bool     on17 = apmf::allowance::IsRuntime1_7_104();
        const bool     ae   = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
        std::uintptr_t site = 0, fn = 0;
        if (!apmf::allowance::RuntimeSupported()) {
            why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (!IniOn("bSubstrateOverrideDeny", "1")) {
            g_iniOff = true;
            why      = "[CombatSubstrate] bSubstrateOverrideDeny=0";
        } else if (on17) {
            const std::uintptr_t sel = REL::Offset(kSelector17).address();
            fn                       = REL::Offset(kOverride17).address();
            bool ok = apmf::allowance::SeatVerified(sel, "Substrate.PackageSelector.1_7_104");
            ok      = apmf::allowance::SeatVerified(fn, "Substrate.Npc.GetEnterCombatOverridePackage.1_7_104") && ok;
            if (!ok) why = "the self-check refused a 1.7.104 raw row (listed above)";
            site = sel + kSiteOff17;
        } else {
            // RELOCATION_ID(SE, AE): evaluated only on exactly 1.6.1170 / 1.5.97 (both ids are in those libraries).
            site = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(36193, 37172) }.address() + (ae ? kSiteOffAE : kSiteOffSE);
            fn   = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(24169, 24673) }.address();
            bool ok = apmf::allowance::SeatVerified(site, "Substrate.PackageSelector.CombatOverrideCall");
            ok      = apmf::allowance::SeatVerified(fn, "Substrate.Npc.GetEnterCombatOverridePackage") && ok;
            if (!ok) why = "the self-check refused a row (listed above)";
        }
        if (!why) {
            // The bytes NOW (another DLL may have patched the site since the self-check): E8 -> the function.
            // On 1.7.104 this is also the call-site proof the raw rows do not carry.
            const auto*  b   = reinterpret_cast<const std::uint8_t*>(site);
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            if (b[0] != 0xE8 || site + 5 + static_cast<std::intptr_t>(rel) != fn)
                why = "the selector's call no longer calls GetEnterCombatOverridePackage (another patcher got there first)";
        }
        if (why && g_iniOff) {
            // The one by-name hole (F-6 review SEV-3): switched off in the INI, the substrate still opens and every OPEN
            // logs the hole (Opening), as the music deny's switch does.
            g_seatState.store(why, std::memory_order_release);
            spdlog::warn("[substrate] combat-override deny OFF -- {}. A held holder whose quest alias or NPC carries an "
                         "enter-combat override package (every vanilla follower) runs THAT package while held; each OPEN "
                         "logs the hole.",
                         why);
            return nullptr;
        }
        if (why) {
            // A refused row refuses the SUBSTRATE (principle 2: a facet it switches on without its deny), as the music,
            // bark and aggressor rows do: combatsubstrate::Install folds this reason into its own.
            g_seatState.store(why, std::memory_order_release);
            g_refusal = why;
            spdlog::warn("[substrate] combat-override deny seat NOT installed -- {}. The combat substrate is refused with "
                         "it.",
                         why);
            return why;
        }
        static auto* s_tr = new SKSE::Trampoline("APMF.SubstratePackage");   // never freed
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(32);
        if (mem) s_tr->set_trampoline(mem, 32);
        else s_tr->create(32);
        g_orig.store(fn, std::memory_order_release);   // before the E8 is rewritten: the thunk may run at once
        const std::uintptr_t prev = s_tr->write_call<5>(site, &OverrideThunk);
        if (prev != fn)
            spdlog::error("[substrate] combat-override seat patch: the previous call target changed between the check "
                          "and the patch (0x{:X}, expected 0x{:X}); the verified function is called.",
                          prev, fn);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kPackage, "Substrate.PackageSelector.CombatOverrideCall",
                                        site, fn);
        g_seatState.store("ON", std::memory_order_release);
        g_seatOn.store(true, std::memory_order_release);
        spdlog::info("[substrate] combat-override deny seat ON: the package selector's GetEnterCombatOverridePackage "
                     "call (RVA 0x{:X}) answers \"none\" for a HELD substrate holder, so the package it ran before the "
                     "substrate stays in charge; real fights and non-holders are untouched.",
                     site - REL::Module::get().base());
        return nullptr;
    }

    const char* SeatState() {
        const char* s = g_seatState.load(std::memory_order_acquire);
        return s ? s : "unknown";
    }

    void Reset() {
        std::scoped_lock lk(g_mx);
        g_win.clear();
    }

    void Opening(RE::FormID a_actor, const RE::AIProcess* a_proc) {
        const RE::FormID pre = Running(a_proc);
        if (g_iniOff)
            spdlog::warn("[substrate] 0x{} known hole: the combat-override deny is OFF ([CombatSubstrate] "
                         "bSubstrateOverrideDeny=0): a combat-override package may replace 0x{} while the substrate is open.",
                         Hex(a_actor), Hex(pre));
        std::scoped_lock lk(g_mx);
        Window& w = g_win[a_actor];
        w        = Window{};
        w.prePkg = pre;
        w.opened = true;
    }

    bool Reassert(RE::FormID a_actor, RE::AIProcess* a_proc, bool a_restart, std::uint64_t a_sinceOpenMs) {
        if (!a_proc || !g_reassertOn.load(std::memory_order_relaxed)) return false;
        const bool capped = a_sinceOpenMs > kReassertCapMs;
        const RE::FormID running = Running(a_proc);
        if (!capped) a_proc->ignoringCombat = true;   // AIProcess+0x139 (static_assert above)
        std::uint32_t n = 0;
        bool          line = false, capLine = false;
        {
            std::scoped_lock lk(g_mx);
            Window& w = g_win[a_actor];
            if (capped) {
                capLine     = !w.capLogged;
                w.capLogged = true;
            } else {
                n = ++w.reasserts;
                if (a_restart) ++w.byRestart;
                line = w.lines < kReassertLines;
                if (line) ++w.lines;
            }
        }
        if (capLine)
            spdlog::warn("[substrate] 0x{} ignoring-bit reset at +{} ms NOT re-asserted: S2b stops {} s after the open.",
                         Hex(a_actor), a_sinceOpenMs, kReassertCapMs / 1000);
        if (line)
            spdlog::info("[substrate] 0x{} S2b re-asserted AIProcess+0x139 = 1 at +{} ms (undone reset #{}, cause: {}; "
                         "running package 0x{}).",
                         Hex(a_actor), a_sinceOpenMs, n,
                         a_restart ? "a package (re)start" : "OTHER: the same package, the same start time", Hex(running));
        return !capped;
    }

    std::uint32_t Closing(RE::FormID a_actor, RE::Actor* a_ref, bool a_dropped) {
        Window w{};
        bool   had = false;
        {
            std::scoped_lock lk(g_mx);
            if (auto it = g_win.find(a_actor); it != g_win.end()) {
                w   = it->second;
                had = true;
                g_win.erase(it);
            }
        }
        if (!had) return 0;
        if (!w.opened) return w.denied;   // closed before the entry: no summary
        const RE::FormID now = Running(a_ref ? a_ref->GetActorRuntimeData().currentProcess : nullptr);
        spdlog::info("[substrate] 0x{} package hold ({}): pre-substrate package 0x{} {}; combat-override swaps denied {}{}; "
                     "S2b re-asserts {} (by a package restart {}){}; override seat {}.",
                     Hex(a_actor), a_dropped ? "dropped on real combat" : "close", Hex(w.prePkg),
                     now == w.prePkg ? "STAYED IN CHARGE (still running)" : fmt::format("is NOT running now (0x{})", Hex(now)),
                     w.denied, w.denied ? fmt::format(" (last 0x{})", Hex(w.lastPkg)) : std::string(), w.reasserts,
                     w.byRestart, w.capLogged ? ", stopped at the 15 s cap" : "", SeatState());
        return w.denied;
    }

}
