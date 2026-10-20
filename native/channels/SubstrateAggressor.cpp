#include "PCH.h"
#include "channels/SubstrateAggressor.h"

#include "core/Allowance.h"   // SeatVerified, the exact-runtime gates
#include "core/Log.h"

#include <cstdlib>
#include <unordered_map>

// ============================================================================
// THE SUBSTRATE AGGRESSOR CLEAR (substrate round R5, fix F-3; SUBSTRATE-R2-RESEARCH.md section 1; INVARIANTS #0 (j)
// condition 2 (g)).
//
// THE BIT (PROVEN on 1.6.1170 and 1.5.97): AIProcess+0x136 bit 2, touched only through two leaf accessors (a scan of
// every [reg+0x136] access found no other read or write of bit 2 in AIProcess code):
//   get AE 39144 0x6D6C90 / SE 38184 0x6442E0  [Substrate.AIProcess.GetAggressor]
//   set AE 39145 0x6D6CB0 / SE 38185 0x644300  [Substrate.AIProcess.SetAggressor]  (dl = the new value)
// Writers: StartCombat (1 on the attacker of a non-hostile target; the JOIN copies the group's first member's
// bit), StopCombat (0, the call this file makes), AI message 0x2A (1; posted only by the Papyrus native
// Actor.StartCombat), and the process update (0 for an actor with no controller). Readers: GetShouldHelp (a moral
// helper does not help an aggressor), KillImpl (INFERRED: murder versus a justified kill), StartCombat (an attacker
// of an aggressor is not the aggressor), the attack reaction, and the group join. NONE of them raises an alarm, a
// flee or a fight by itself; while it is set on a held holder they read a wrong fact (the follower "attacked
// someone"), which matters at the real-fight transition. So: after the entry it is set to false through the
// engine's own setter, on the game thread; while HELD it is checked every Poll and cleared again only if something
// set it again (a script's StartCombat, a join). Never once REAL (the engine's value is the truth of a real fight).
// The close's StopCombat clears it anyway (AE 0x6B7403).
//
// THREADING: game thread only. The setter is the engine's non-atomic read-modify-write of the byte +0x136; the
// engine calls it from StartCombat / StopCombat on whatever thread runs them, so a concurrent engine write of
// another bit of that byte is the same exposure the engine's own StopCombat from the main thread has.
//
// SCOPE: exactly 1.6.1170 and 1.5.97 (1.7.104: the two rows have no 1.7.104 entry and a RELOCATION_ID miss is
// fatal there: refused by name). [CombatSubstrate] bSubstrateAggressorClear=0 switches it off (logged per OPEN).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
    constexpr std::uint32_t kMaxLines     = 4;   // re-clear lines per substrate (the rest are counted)

    using Get_t = bool (*)(const RE::AIProcess*);
    using Set_t = void (*)(RE::AIProcess*, bool);
    Get_t g_get = nullptr;
    Set_t g_set = nullptr;

    bool        g_on  = false;
    const char* g_off = "not installed";

    struct Rec {
        int           atEntry = -1;   // the bit right after StartCombat returned (before the clear)
        int           afterClear = -1;
        std::uint32_t reclears = 0, lines = 0;
        std::uint64_t firstReclearMs = 0;
    };
    std::unordered_map<RE::FormID, Rec> g_recs;   // GAME THREAD

}

namespace apmf::substrateaggressor {

    const char* Install() {
        char b[8]{};
        REX::W32::GetPrivateProfileStringA("CombatSubstrate", "bSubstrateAggressorClear", "", b, sizeof(b), kIni);
        if (b[0] && std::atoi(b) == 0) {
            g_on  = false;
            g_off = "[CombatSubstrate] bSubstrateAggressorClear=0 in APMF.ini";
            return nullptr;
        }
        if (apmf::allowance::IsRuntime1_7_104() ||
            !(REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) || REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)))
            return "the aggressor clear's rows are verified on exactly 1.6.1170 and 1.5.97 only (1.7.104: "
                   "Substrate.AIProcess.SetAggressor / GetAggressor are not in its id table)";
        const auto get = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38184, 39144) }.address();
        const auto set = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38185, 39145) }.address();
        bool ok = apmf::allowance::SeatVerified(get, "Substrate.AIProcess.GetAggressor");
        ok = apmf::allowance::SeatVerified(set, "Substrate.AIProcess.SetAggressor") && ok;
        if (!ok)
            return "the address self-check refused a row the aggressor clear calls (listed above): without it a held "
                   "holder would carry the aggressor bit StartCombat set";
        g_get = reinterpret_cast<Get_t>(get);
        g_set = reinterpret_cast<Set_t>(set);
        g_on  = true;
        g_off = "";
        return nullptr;
    }

    bool        On() { return g_on; }
    const char* OffReason() { return g_off; }

    void AfterEntry(RE::FormID a_holder, RE::AIProcess* a_proc) {
        Rec& r = g_recs[a_holder];
        r      = Rec{};
        if (!g_on) {
            spdlog::warn("[substrate] 0x{} known hole: the aggressor clear is OFF ({}): the holder keeps the aggressor "
                         "bit StartCombat set (AIProcess+0x136 bit 2 = {}).",
                         Hex(a_holder), g_off,
                         a_proc ? ((a_proc->lowProcessFlags.underlying() & 0x04) ? "1" : "0") : "no process");
            return;
        }
        if (!a_proc) {
            spdlog::warn("[substrate] 0x{} aggressor clear: no AI process after the entry; nothing written.", Hex(a_holder));
            return;
        }
        r.atEntry = g_get(a_proc) ? 1 : 0;
        g_set(a_proc, false);   // the engine's own setter, as StopCombat calls it
        r.afterClear = g_get(a_proc) ? 1 : 0;
        spdlog::info("[substrate] 0x{} aggressor bit (AIProcess+0x136 bit 2) after the entry {} -> {} (the engine's setter, "
                     "AE 39145 / SE 38185, set false: a held holder attacked no one){}",
                     Hex(a_holder), r.atEntry, r.afterClear,
                     r.afterClear ? " -- STILL SET after the setter (a concurrent StartCombat set it again?)" : "");
    }

    void WhileHeld(RE::FormID a_holder, RE::AIProcess* a_proc, std::uint64_t a_nowMs,
                   const std::atomic<std::uint32_t>& a_slotState, std::uint32_t a_realBit) {
        if (!g_on || !a_proc) return;
        if (!g_get(a_proc)) return;
        if (a_slotState.load(std::memory_order_acquire) & a_realBit) return;   // REAL since Serve's top: the engine's value
        const auto it = g_recs.find(a_holder);
        if (it == g_recs.end()) return;   // AfterEntry did not run for this holder (no entry yet)
        Rec& r = it->second;
        g_set(a_proc, false);
        ++r.reclears;
        if (!r.firstReclearMs) r.firstReclearMs = a_nowMs;
        if (r.lines < kMaxLines) {
            ++r.lines;
            spdlog::info("[substrate] 0x{} aggressor bit SET AGAIN while held (a script's Actor.StartCombat or a join): "
                         "cleared again (#{}{}).",
                         Hex(a_holder), r.reclears, r.lines == kMaxLines ? "; further re-clears are only counted" : "");
        }
    }

    std::string Closing(RE::FormID a_holder) {
        const auto it = g_recs.find(a_holder);
        if (it == g_recs.end()) return {};
        const Rec r = it->second;
        g_recs.erase(it);
        if (!g_on) return fmt::format("aggressor clear OFF ({})", g_off);
        if (r.atEntry < 0) return "aggressor clear: nothing written (no process at the entry)";
        return fmt::format("aggressor bit at the entry {}, after the clear {}, re-cleared while held {}", r.atEntry,
                           r.afterClear, r.reclears);
    }

    void Reset() { g_recs.clear(); }

}
