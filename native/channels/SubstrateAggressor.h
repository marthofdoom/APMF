#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace RE {
    using FormID = std::uint32_t;   // identical to CommonLib's alias (APMF_API.h does the same)
    class AIProcess;
}

// ============================================================================
// THE SUBSTRATE AGGRESSOR CLEAR (substrate round R5, fix F-3 of _evidence/2026-10-18-designs/SUBSTRATE-R2-RESEARCH.md
// section 1; INVARIANTS #0 (j) condition 2 (g); marth 2026-10-19: "clear the aggressor bit on held holders after
// research names its readers"). channels/SubstrateAggressor.cpp has the evidence block.
//
// StartCombat(anchor) sets the holder's aggressor bit (AIProcess+0x136 bit 2): the holder attacked a non-hostile
// target. A held holder attacked no one. Harbinger sets it back to false through the engine's own setter (the call
// StopCombat makes) right after the entry, and again only when something sets it again while the holder is HELD (a
// script's Actor.StartCombat or a join). Never once the holder is REAL: in a real fight the engine's value stands.
//
// EVERYTHING HERE IS GAME THREAD (combatsubstrate's Poll seat and what it runs).
// ============================================================================

namespace apmf::substrateaggressor {

    // combatsubstrate::Install (kDataLoaded), after the substrate's own rows. Reads [CombatSubstrate]
    // bSubstrateAggressorClear (default 1). nullptr = the clear stands (ON) or was switched off in the INI (OFF, the
    // reason kept); otherwise the reason it cannot stand (the runtime, a row refused): the caller REFUSES the
    // substrate (principle 2).
    const char* Install();
    bool        On();
    const char* OffReason();   // never null ("" when on)

    // Enter, right after a successful StartCombat(anchor): the bit set to false (logged with its value).
    void AfterEntry(RE::FormID a_holder, RE::AIProcess* a_proc);

    // Serve, every Poll while the substrate is open and its REAL bit was clear at Serve's top. `a_slotState` /
    // `a_realBit`: the holder's slot state, re-read just before the setter, so a real fight that started on another
    // thread since Serve's top keeps the engine's value. Re-clears only a bit that reads 1 (counted, logged with a cap).
    void WhileHeld(RE::FormID a_holder, RE::AIProcess* a_proc, std::uint64_t a_nowMs,
                   const std::atomic<std::uint32_t>& a_slotState, std::uint32_t a_realBit);

    // CloseSub / DropSub: the clear's summary for the close line ("" when nothing is recorded), then forget.
    std::string Closing(RE::FormID a_holder);

    // combatsubstrate::ResetAll (revert / kPreLoadGame).
    void Reset();

}
