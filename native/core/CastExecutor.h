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
//             {form = spell, ival = HAND, target = castTarget})
//   Repoint(handle, {form, ival, target})   -- switch spell/hand/target IN
//                                      PLACE, or fire again with the SAME
//                                      values (a repeat pulse)
//   Release(handle)                 -- ends the cast, restores the hand
//
// ABI NOTE (2026-09-05): `target` (and a reserved `pos` -- see below) were
// APPENDED to the frozen APMF_Param at the END (APMF_API.h documents append-only
// as the supported extension path) -- a zero-init caller reads target=0/pos=0,
// byte-identical to before. No new vtable slot, no APMF_API_vN, no abiVersion
// bump.
//
// HAND (param.ival): 0 auto (free hand, prefers right) / 1 right / 2 left /
// 3 dual (both hands, same spell). See HandMode below.
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

    // param.ival for kIntent_SelectSpell's ACT contract. APPEND-ONLY positions
    // (this is read from the EXISTING, already-unread ival field -- see
    // APMF_API.h's kIntent_SelectSpell comment; not a new ABI surface).
    enum HandMode : std::int32_t {
        kHandAuto  = 0,   // pick a free hand (no weapon there), prefers right
        kHandRight = 1,
        kHandLeft  = 2,
        kHandDual  = 3,   // both hands, same spell
    };

    // Wired 1:1 from channels/CastingSelect.cpp's Channel overrides. All THREE
    // run on the confirmed-main Drain seat (INVARIANTS #4) -- direct engine
    // mutation (equip, NotifyAnimationGraph) is safe to call synchronously from
    // inside these; only the multi-frame poll continuations need
    // core/MainThread.h's Post/Pump.
    void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param);
    void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param);
    void Release(RE::FormID id, RE::Actor* actor);

}
