#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- ch.8 SelectSpell's +ACT mode (feat/cast-act, marth 2026-09-05).
// Wired from channels/CastingSelect.cpp's Engage/OnOwnerChanged/Release. Turns
// a kIntent_SelectSpell claim from ARBITRATE-ONLY (the client selects + casts
// itself) into APMF OWNING the whole cast: equip the resolved hand(s), drive the
// engine's own observed animated cast sequence (core/CastObserve.h captured it),
// and guarantee delivery even if the drive can't animate. The client's contract
// stays the existing APMF_Param shape -- NO ABI change:
//
//   RequestEx(actor, kIntent_SelectSpell, basis,
//             {form = spell, ival = HAND | kActFlag_Drive, target = castTarget})
//   Repoint(handle, {form, ival, target})   -- switch spell/hand/target IN
//                                      PLACE, or fire again with the SAME
//                                      values (a repeat pulse)
//   Release(handle)                 -- ends the cast, restores the hand
//
// OPT-IN (marth 2026-09-05 -- the offense-safety fix). kIntent_SelectSpell has
// TWO legitimate modes and this claim alone cannot tell them apart without an
// explicit flag:
//   1. GATE-ONLY (the channel's ORIGINAL/base mode, still the DEFAULT): the
//      client's OWN AI casts the selected spell; APMF only narrows/denies
//      competitors (core/CastGate.cpp/EquipGate.cpp). APMF makes NO equip/anim
//      write. This is what a bare claim (`ival` with `kActFlag_Drive` CLEAR --
//      including the pre-existing `ival == 0` shape) gets, byte-identical to
//      the channel's behavior before +ACT existed. MFO's offense gambit uses
//      THIS mode and must keep using it unchanged.
//   2. +ACT / DRIVE (opt-in): APMF equips + animates + fires, as described
//      below. Requested ONLY by setting `kActFlag_Drive` in `ival` (e.g.
//      `ival = kHandRight | kActFlag_Drive`). `channels/CastingSelect.cpp`
//      checks this bit BEFORE ever calling into this module -- Engage/
//      OnOwnerChanged here are only reached for a claim that opted in.
// See `kActFlag_Drive`/`kHandModeMask` below.
//
// ABI NOTE (2026-09-05): `target` (and a reserved `pos` -- see below) were
// APPENDED to the frozen APMF_Param at the END (APMF_API.h documents append-only
// as the supported extension path) -- a zero-init caller reads target=0/pos=0,
// byte-identical to before. `kActFlag_Drive` is likewise just a new BIT read out
// of the already-present, already-unread-by-this-channel `ival` field. No new
// vtable slot, no APMF_API_vN, no abiVersion bump.
//
// HAND (param.ival bits 0-1, kHandModeMask): 0 auto (free hand, prefers right) /
// 1 right / 2 left / 3 dual (both hands, same spell). See HandMode below.
//
// TARGET (param.target, marth 2026-09-05 -- the heal-the-player fix). Priority:
// (1) `param.target` if the client named one explicitly -- the ONLY correct
// source for heal/buff-another: the actor's own COMBAT target is its FOE, not
// whoever it should be healing (claiming ch.6 kIntent_CombatTarget = the ally
// would wrongly redirect the actor to FIGHT the ally). (2) if the client left
// `target` at 0, fall back to a winning kIntent_CombatTarget (ch.6) claim on
// the actor (the offense case: cast at your foe -- the client names WHO via
// ch.6 exactly as it already does for hostile targeting). (3) self. APMF never
// invents a target beyond this (INVARIANTS #0/#17) -- no live-engine
// currentCombatTarget read, deliberately (no verified accessor; see
// CastExecutor.cpp).
//
// LOCATION (param.posX/Y/Z) is RESERVED, ACCEPTED-BUT-NOT-YET-WIRED: a future
// pass for location-delivery casts (Rune/AoE ground-target). Aiming a driven
// cast (or the CastSpellImmediate fallback) at a bare world point needs its own
// plumbing (the engine has no simple "set a target point" field on MagicCaster
// the way `desiredTarget` covers an actor target) -- deliberately deferred
// rather than half-implemented. All-zero today; a client MAY pass a point now
// with no effect (forward-compatible once wired).
//
// CONCENTRATION (2026-09-05 field fix). A concentration spell (e.g. Fast
// Healing) does not just pulse once: after the initial SpellFire, its
// magnitude is applied by the engine's OWN per-frame caster update for as
// long as it stays charged/casting, exactly like a player holding the cast
// button. The drive reflects this: PhaseFire hands a concentration cast to
// PhaseHold, which keeps the channel (and the internal ch.8b claim) alive --
// no re-interrupt, no re-fire -- for a bounded WALL-CLOCK window (kConcHoldMs,
// ~3s of real time, never a frame count) or until the caster exits that state on
// its own, THEN stops it (InterruptCast + the balanced anim-graph stop tail) and
// parks. An instant/fire-and-forget spell parks after a short bounded grace that
// lets its release animation finish, as before. Either way, a same-spell
// Repoint while still active is a no-op (`hd.inFlight`) -- it won't interrupt
// an in-progress hold to "restart" it.
//
// DELIVERY-FLIP PROXY IS EQUIPPABLE (2026-09-05 field fix). `ActorEquipManager
// ::EquipSpell` can only SELECT a spell the actor already KNOWS
// (`Actor::HasSpell`) -- it does not teach one. The proxy is therefore
// `AddSpell`'d onto the actor when minted and `RemoveSpell`'d when freed
// (TRANSIENT -- never persisted as a real learned spell, added/removed around
// the same window it's equipped) so the caster can actually select it. See
// core/CastExecutor.cpp's `proxy` namespace for the leak-safety argument (one
// choke point, `proxy::Free`, that every teardown path already calls).
//
// PROTECTION. Internally rides the EXISTING, now per-hand-scoped ch.8b
// (kIntent_Cast) deny (core/ActionGate.cpp/CastGate.cpp/EquipGate.cpp,
// feat/deny-perhand) for the resolved hand(s) -- no new deny mechanism. Since the
// client no longer force-equips (that race is what caused the deck CTD this
// deny-perhand pass fixed), APMF is the SOLE equipper for a claimed hand; there
// is no competing writer to defend against on the main-thread side.
//
// DELIVERY GUARANTEE (marth's rule, #5): the drive always tries to fire via the
// animated sequence; if it cannot (VR, never selects, never charges), APMF falls
// back to an immediate CastSpellImmediate on the kInstant caster so the effect
// ALWAYS lands. The client needs no fallback of its own.
// ============================================================================

namespace apmf::castexec {

    // param.ival for kIntent_SelectSpell's ACT contract -- bits 0-1 are the hand,
    // bit 2 is the drive opt-in. APPEND-ONLY positions (read from the EXISTING,
    // previously-unread `ival` field -- see APMF_API.h's kIntent_SelectSpell
    // comment; not a new ABI surface).
    enum HandMode : std::int32_t {
        kHandAuto  = 0,   // pick a FREE hand (nothing held there -- no weapon, no
                          // shield, no torch; a spell counts as free), prefers right
        kHandRight = 1,
        kHandLeft  = 2,
        kHandDual  = 3,   // both hands, same spell -- SEE THE KNOWN GAP BELOW
    };
    inline constexpr std::int32_t kHandModeMask = 0x3;   // bits 0-1 -- mask ival to a HandMode

    // KNOWN GAP -- kHandDual IS STRUCTURALLY UNPROTECTED (H8, 2026-09-05 review;
    // deliberately NOT fixed in the drive-hardening pass, it is a CLAIM-MODEL
    // change). The ch.8b protection claim is per-ACTOR: only ONE kIntent_Cast claim
    // wins on an actor at a time, and it names exactly one hand (its
    // kCastFlag_LeftHand bit). A dual drive therefore starts TWO hand drives but
    // only ONE of them can hold the winning cast claim -- the other hand is left
    // either unprotected (the AI free to re-arm/charge it) or, worse, denied by
    // APMF's own per-hand gates, since a claim scoped to the right hand does not
    // admit the left hand's cast of the same form. Making dual correct needs the
    // claim itself to carry BOTH hands (a hand BITMASK in CastFlags rather than a
    // single hint bit) plus per-hand gate resolution against that mask -- an
    // append-only ABI addition and a matching change in every gate that reads the
    // hand today. Not on the critical path: MFO drives single-hand kHandAuto.

    // OPT-IN bit (bit 2, value 4). CLEAR (the default -- including ival == 0,
    // the pre-+ACT shape) => GATE-ONLY: this module is never even called: the
    // client's own AI casts, APMF only narrows/denies. SET => this module
    // equips + drives + guarantees delivery. See the file header's OPT-IN
    // section. `channels/CastingSelect.cpp` tests this bit; CastExecutor.cpp
    // itself masks it off before reading the hand (`ival & kHandModeMask`).
    inline constexpr std::int32_t kActFlag_Drive = 1 << 2;

    // Wired 1:1 from channels/CastingSelect.cpp's Channel overrides, called
    // ONLY when the claim's `ival` has `kActFlag_Drive` set (a gate-only claim
    // never reaches these). All THREE run on the confirmed-main Drain seat
    // (INVARIANTS #4) -- direct engine mutation (equip, NotifyAnimationGraph)
    // is safe to call synchronously from inside these; only the multi-frame
    // poll continuations need core/MainThread.h's Post/Pump.
    void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param);
    void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param);

    // Tear down any active drive for `id` (interrupt/anim/deselect/release the
    // internal ch.8b claim/free the proxy) if one exists. Safe to call
    // UNCONDITIONALLY (a no-op when nothing was driving this actor) -- so
    // channels/CastingSelect.cpp calls this on every Release AND whenever a
    // Repoint drops the drive opt-in bit (switching an already-driven claim
    // back to gate-only must restore the hand, not leave it mid-cast).
    void Release(RE::FormID id, RE::Actor* actor);

    // Drop ALL drive state and reset the delivery-flip proxy pool. Call on the SKSE
    // revert callback (new game / pre-load wipe) and after the kPreLoadGame
    // ReleaseAll (H2/M8, 2026-09-05 review). Two things depend on it:
    //   * the proxies are runtime 0xFF dynamic forms holding the SOURCE spell's
    //     `Effect*` BY POINTER -- they must have those borrowed pointers cleared
    //     before the load-time form purge, or the purge frees a live spell's effect
    //     array through a dead proxy (MFO's Actuation_Direct.cpp lesson);
    //   * `ControlMap::Clear()` (revert) never calls channel->Release, so without
    //     this the 4-slot proxy pool stays occupied by a stale owner FOREVER and
    //     every later ally heal declines with "proxy pool overflow".
    // MAIN THREAD ONLY (the SKSE revert / kPreLoadGame seat). Makes no engine calls.
    void ResetAll();

    // Un-teach + deselect every live delivery-flip proxy. Call on the SKSE SAVE
    // callback (H3): the proxies are transient runtime 0xFF forms and a save taken
    // mid-drive must never capture a reference to one. A drive interrupted this way
    // degrades to the guaranteed-delivery fallback, so the effect still lands.
    // MAIN THREAD ONLY (SKSE's save callback seat).
    void PreSaveSweep();

}
