#pragma once
#include "APMF_API.h"

#include <cstdint>
#include <vector>

namespace RE {
    class Actor;
    class CombatGroup;
}

// ============================================================================
// THE ANCHOR GUARD (2026-10-19, SEV-1 fix/apmf-anchor-leak; INVARIANTS #0 (j); channels/AnchorGuard.cpp has
// the full block). Keeps every actor that is NOT a live substrate holder out of combat with the substrate's
// anchor. Two halves:
//   * THE SEAT (any thread): ch.22's slot-0x99 thunk, at StartCombat's own SELF check, hands this file
//     StartCombat's target (rsi) and group (rbp) through the widened stub. A NULL-target StartCombat with a
//     group (a "join this group, take its targets" entry, AE caller 0x71C2DD / SE 0x68848E) is refused when the
//     actor is not a live holder and the group is a SUBSTRATE group (published by the sweep below, compared by
//     ADDRESS only: the seat never reads through the group).
//   * THE SWEEP (game thread, combatsubstrate::Poll): while any substrate is live and kTailMs after the last
//     close, every high-process actor that is not a live holder and whose controller targets the anchor, or
//     whose group lists it, is logged as an ERROR (a leak, never masked) and cleaned: the anchor out of its
//     group (CombatGroup::RemoveTarget) when no live holder shares that group, then the held engagement's
//     ranked StopCombat when nothing else is left to fight. CLOSE runs the same for every non-holder in the
//     closing holder's group.
// ============================================================================
namespace apmf::anchorguard {

    // kDataLoaded, from reentrydeny::Install BEFORE it builds the slot-0x99 stub. `a_startCombat` is
    // Actor::StartCombat's verified address. True = verified (row AnchorGuard.StartCombat.TargetGroupRegs on
    // exactly 1.6.1170 / 1.5.97 / 1.7.104; 1.7.104 at the AE offset +0x23) and enabled: ch.22 then builds the WIDENED stub
    // (self site: r8 = rsi, r9 = rbp) and calls AnswerJoin at the self site. Reads [CombatSubstrate]
    // bSubstrateJoinGuard / bSubstrateEntryTrace. GAME THREAD, once.
    bool InstallSeat(std::uintptr_t a_startCombat);

    // ch.22 sets this once the slot holds the widened stub (after the vtable write). Any thread.
    void SeatArmed();

    // Why the seat is off (for the install line). Never null. Any thread.
    const char* SeatState();

    // THE SEAT. Called by ch.22's thunk at StartCombat's self-check return site only, after ch.22's own answer
    // (which did not refuse). `a_target` / `a_group` are StartCombat's rsi / rbp there. True = refuse (IsDead
    // answers "dead": StartCombat returns false before it does anything). Logs the passive entry-route line
    // ([CombatSubstrate] bSubstrateEntryTrace) while any substrate is live. Reads no engine state beyond the two
    // actors' FormIDs; dereferences nothing of the group; takes no engine lock. ANY THREAD.
    bool AnswerJoin(RE::Actor* a_self, RE::Actor* a_target, RE::CombatGroup* a_group, std::uintptr_t a_siteRet);

    // THE SWEEP, from combatsubstrate::Poll every frame (both of its branches). GAME THREAD.
    void Sweep(std::uint64_t a_nowMs);

    // CLOSE (combatsubstrate CloseSub): the member handles of the holder's group, read under the group's read
    // lock BEFORE the holder's StopCombat. GAME THREAD.
    std::vector<RE::ActorHandle> GroupMembers(RE::Actor* a_holder);
    // ... and after it: every member that is not a live holder gets the sweep's treatment. GAME THREAD.
    void AfterClose(RE::FormID a_holder, const std::vector<RE::ActorHandle>& a_members);

    // revert / kPreLoadGame (from combatsubstrate::ResetAll). GAME THREAD.
    void ResetAll();

}
