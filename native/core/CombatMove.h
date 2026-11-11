#pragma once
#include "APMF_API.h"
#include "core/Sources.h"

namespace apmf { struct ServeCheck; }   // core/ControlMap.h (ABI v35 CanServe)

// ============================================================================
// ABI v38 COMBAT MOVE (kIntent_CombatMove, ch.31): make an actor do ONE bash or power bash, by the
// engine's own start-of-attack action, ONCE, on the game thread. INVARIANTS #0 (l). Docs/FACETS.md
// "Combat move", MAP.md core/CombatMove.
//
// THE ENGINE ROAD (disassembly of the unpacked 1.6.1170 executable; the 1.5.97 twins in brackets):
//   * The combat tree's Bash leaf attempt (AE 47841 0x86B4A0) tests the actor (AE 0x6B90F0: the attack
//     state at actor+0xC8 is None, the graph bool "IsAttackReady" is true, BOOL_FLAGS bit 20
//     kAttackingDisabled is clear) and calls StartAttack (AE 49170 0x8A28B0 [SE 48139]) on the bash
//     context.
//   * StartAttack draws an entry of the ACTOR BASE's attack data map (actor+0x40 -> TESNPC+0x130; the
//     gather AE 0x8A3960 keeps flags bit 1 Bash for the bash pool) and builds a TESActionData on its
//     stack: ActionInput's constructor (AE 14993 0x1EE360) with priority 2, source = the actor, no action
//     form and no target; the TESActionData vtable (AE 188603 0x178C478); animEvent (+0x28) = the entry's
//     event. Then TESActionData::Process (vtable slot 5, AE 41557 0x77AD30 [SE 40551 0x6E6120]).
//   * Process hands the data to the action mediator (AE 38949 0x6CD0D0 [SE 0x63AF10]): its pre-checks,
//     the animation event on the actor's graph (AE 0x7C2FB0: the holder at actor+0x38, slot 1), and on
//     success its post-action step (AE 39004 0x6CF760 [SE 38048 0x63D0F0]), which calls the ONE function
//     that sets an actor's attack data (AE 39541 0x6EE0C0 [SE 0x65B330]; exactly one caller on each
//     build): a lookup of the event in the actor base's map into process->high->attackData, and then
//     the attack state (flags bit 1 -> 6 = Bash).
//   So the attack data and the attack state the hit reads are set ONLY by this action road. A bare
//   NotifyAnimationGraph("bashStart") plays the swing without either.
//
// WHAT HARBINGER DOES: the same construction (the default constructor RELOCATION_ID(15916, 41558), the
// TESActionData vtable, source, priority 2, the entry's event), ONE Process() call, the destructor
// (vtable slot 0 with flags 0: no free, the object is on our stack as it is on the engine's), then the
// read-back. The entry is the client's declared KIND (bash / power bash); among several of that kind
// Harbinger takes the highest attack chance (ties: the lower event string), never a random draw.
//
// NEVER A CLAIM (the kIntent_Place shape): ClientAPI diverts the request here before the control map.
// The handle comes from ControlMap::NextHandle(). The outcome is a per-handle record read by
// GetCombatMoveState (APMF_API_v38); a TESHitEvent sink marks the record when the bash hits.
//
// THE PASSIVE PROBE ([Probe] bCombatMove, default 0): one line per MELEE hit caused by or landing on a
// player teammate: the hit's flags (power / bash / blocked / sneak), the attacker's attack data (event,
// flags) and attack state at the hit, and the target's blocking state. It changes nothing. It is how
// the first field run shows what the engine's own bashes and blocks look like next to a sent one.
// ============================================================================

namespace apmf::combatmove {

    // kDataLoaded: the INI switch, the runtime gate (exactly 1.6.1170 / 1.5.97, VR refused), the two
    // SeatVerified rows, the hit sink. No hook.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // ANY THREAD (ClientAPI). The synchronous checks (installed, param present, a move this ABI builds,
    // actor non-zero and not the player), then a Queued record and ONE apmf::mainthread::Post of the
    // move. NO form lookup here. false = refused (one WARN line; the caller returns kInvalidHandle and
    // keeps no record). `a_dry` non-null = CHECK ONLY (ABI v35 CanServe): the same call-time checks, each
    // refusal recorded there instead of logged, and true returned (pending OneShot | CombatMoveRoom)
    // before the record table is touched.
    bool Enqueue(APMF_API::Handle a_handle, RE::FormID a_actor, const APMF_API::APMF_Param* a_param,
                 apmf::sources::SourceId a_client, ServeCheck* a_dry = nullptr);

    // ANY THREAD. GetCombatMoveState (APMF_API_v38): copy under the leaf lock, no game call.
    std::uint32_t GetState(APMF_API::Handle a_handle, APMF_API::APMF_CombatMoveStateInfo* a_out);

    // Revert / kPreLoadGame: forget every record. A move still queued is dropped with the main-thread
    // queue's Discard() in plugin.cpp; its record goes here (it reads Unknown after).
    void ResetAll(const char* a_why);

}
