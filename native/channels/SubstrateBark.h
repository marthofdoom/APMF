#pragma once

#include <cstdint>

namespace RE {
    using FormID = std::uint32_t;   // identical to CommonLib's alias (APMF_API.h does the same)
    class Actor;
    class CombatGroup;
}

// ============================================================================
// THE SUBSTRATE BARK DENY (substrate round R5, fix F-2 of _evidence/2026-10-18-designs/SUBSTRATE-R2-RESEARCH.md;
// INVARIANTS #0 (j) condition 2 (f)). channels/SubstrateBark.cpp has the evidence block.
//
// The combat-start bark of a substrate holder is requested on its controller's FIRST update (AE 33231 / SE 32484)
// as NormalToCombat (59) or NormalToAlert (57). The engine's own gate in the combat-dialogue core (AE 44673 / SE
// 43467) skips that line while the holder GROUP's detection-dialogue timer for it is running. Harbinger pre-arms
// the two timers (CombatGroup::detectionDialogueTimers[2] and [4]) to {the AI clock, the TTL + 2 s} in the
// holder's own Actor::UpdateCombat (Character slot 0xE4, TargetPin's layer 0) BEFORE the engine's update runs, on
// the update's own job thread, where the engine itself reads and writes those timers unlocked. Nothing else is
// touched.
//
// Records are indexed by the substrate's slot (combatsubstrate's g_slots index, < kMaxSlots).
// ============================================================================

namespace apmf::substratebark {

    inline constexpr int kMaxSlots = 16;

    // combatsubstrate::Install (kDataLoaded, game thread), after the substrate's own rows verified. Reads
    // [CombatSubstrate] bSubstrateBarkDeny (default 1). Returns nullptr when the deny stands (ON) or was switched
    // off in the INI (OFF, the reason kept for every OPEN line), else the reason it cannot stand (a row refused,
    // the runtime, the 0xE4 layer 0 not installed): the caller then REFUSES the substrate (principle 2).
    const char* Install();
    bool        On();          // ANY THREAD
    const char* OffReason();   // ANY THREAD, never null ("" when on)

    // GAME THREAD. Enter, just BEFORE the holder's StartCombat(anchor): the record becomes PENDING, so the holder's
    // next UpdateCombat (on whatever job thread) arms the timers before the engine's update reads them, even when
    // that update races the end of the StartCombat call. `a_holdSec` is the duration written (the TTL + 2 s).
    void Arm(int a_slot, RE::FormID a_holder, std::uint32_t a_holderH, float a_holdSec);
    // GAME THREAD. The entry failed: the record is forgotten (no line).
    void Disarm(int a_slot);

    // ANY THREAD (an UpdateCombat job). channels/TargetPin.cpp's layer-0 thunk, BEFORE the chain below it. One
    // relaxed load while nothing is pending.
    void BeforeUpdateCombat(RE::Actor* a_actor);

    // The held engagement's detection seat (channels/CombatEntry.cpp DetectThunk), via combatsubstrate:
    //   SeatAnchorPass: after the engine's update of the ANCHOR in `a_group` (AnchorKnowledge);
    //   SeatRealTarget: a target that is NOT the anchor in `a_group` (NoteGroupTarget), before the drop can
    //                   clear any slot: Harbinger's two values are put back to what they were (only while they
    //                   still read exactly Harbinger's values), so a real fight's bark is never suppressed.
    // Both act ONLY when `a_writeLockedHere` (the caller's WriteLockedHere: inside the engine's own write-locked
    // update of that live group). COMBAT-MANAGER JOB thread. No log.
    void SeatAnchorPass(RE::CombatGroup* a_group, bool a_writeLockedHere);
    void SeatRealTarget(RE::CombatGroup* a_group, bool a_writeLockedHere);

    // GAME THREAD. CloseSub / DropSub (only for an opened substrate): the one BARK result line, then the record
    // is forgotten. `a_dropped` = the real-combat drop.
    void Closing(int a_slot, RE::FormID a_holder, bool a_dropped, std::uint64_t a_nowMs);

    // GAME THREAD. combatsubstrate::ResetAll (revert / kPreLoadGame): forget every record.
    void Reset();

}
