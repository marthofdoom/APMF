#pragma once

#include <cstdint>

namespace RE {
    using FormID = std::uint32_t;   // identical to CommonLib's alias (APMF_API.h does the same)
    class Actor;
    class AIProcess;
}

// ============================================================================
// THE HELD HOLDER'S PACKAGE STAYS IN CHARGE (substrate fix F-6, marth 2026-10-20: "The combat packages
// movement is blocked and the regular ooc package that they had before remains in charge via harbingers
// allow deny."; INVARIANTS #0 (j) condition 3). Two mechanisms, one concern (see the .cpp for the RE):
//
//  1. THE COMBAT-OVERRIDE DENY (a new engine seat). The engine's package selector (AE 37172 / SE 36193)
//     swaps its pick for the actor's ENTER-COMBAT OVERRIDE package (its quest alias's, then its NPC's)
//     whenever the actor has a combat controller. Character slot 0x49 (core/PackageGate.cpp) is asked
//     BEFORE that swap and cannot stop it, so the seat is the selector's own call of
//     GetEnterCombatOverridePackage. For a HELD holder (combatsubstrate::StateOf == Held) the call answers
//     "none": the selector keeps its own pick (the package that ran before the substrate, or a client's
//     0x49 offer). Never for a REAL holder, never for anyone else. [CombatSubstrate] bSubstrateOverrideDeny.
//
//  2. S2b, THE IGNORING-BIT RE-ASSERT. combatsubstrate's Serve detects each engine reset of AIProcess+0x139
//     while held; after it counted and attributed one, Reassert writes the bit back to 1 and logs it,
//     capped at 15 s from the open. [CombatSubstrate] bSubstrateBitReassert.
//
// THREADS. The seat's thunk runs wherever the engine evaluates packages (engine AI threads): it reads
// combatsubstrate::StateOf (lock-free) and the two forms' FormIDs, and takes this file's leaf mutex only on
// a denial (no engine call under it; spdlog runs after it is released). Everything else is GAME THREAD:
// channels/CombatSubstrate.cpp calls it from Install, Enter, Serve, CloseSub, DropSub and ResetAll.
// ============================================================================

namespace apmf::substratepackage {

    // combatsubstrate::Install, after the substrate installed. Reads the two INI keys, verifies the seat's
    // rows and installs the call-site seat (refused by name on a failed row). Idempotent.
    void Install();
    // "ON" or the reason the seat is not installed (never null). ANY THREAD.
    const char* SeatState();

    // combatsubstrate::ResetAll (revert / kPreLoadGame): forget every window.
    void Reset();

    // Enter, just BEFORE StartCombat(anchor): records the package running now (the pre-substrate package)
    // and opens the holder's window.
    void Opening(RE::FormID a_actor, const RE::AIProcess* a_proc);

    // Serve, inside its reset block, AFTER it counted and attributed an engine reset of the ignoring bit
    // (`a_restart`: the package or its start time changed since the bit was written). S2b: writes
    // AIProcess+0x139 back to 1 and logs the undone reset, while `a_sinceOpenMs` <= 15 s and the switch is on.
    // True = the bit was written back.
    bool Reassert(RE::FormID a_actor, RE::AIProcess* a_proc, bool a_restart, std::uint64_t a_sinceOpenMs);

    // CloseSub / DropSub, at the top (before StopCombat): the window's summary line (the pre-substrate
    // package, whether it is still the running one, swaps denied, bits re-asserted); forgets the window.
    void Closing(RE::FormID a_actor, RE::Actor* a_ref, bool a_dropped);

}
