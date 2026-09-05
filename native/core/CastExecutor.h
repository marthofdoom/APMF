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
//   RequestEx(actor, kIntent_SelectSpell, basis, {form = spell, ival = HAND})
//   Repoint(handle, {form, ival})   -- switch spell/hand IN PLACE, or fire again
//                                      with the SAME {form,ival} (a repeat pulse)
//   Release(handle)                 -- ends the cast, restores the hand
//
// HAND (param.ival): 0 auto (free hand, prefers right) / 1 right / 2 left /
// 3 dual (both hands, same spell). See HandMode below.
//
// TARGET (no new field -- reads EXISTING claims, never invents one, INVARIANTS
// #0/#17): a self-delivery spell targets the caster UNLESS the actor also holds
// a winning kIntent_CombatTarget (ch.6) claim, in which case THAT actor is the
// intended recipient (the canonical "heal an ally" shape -- the client names WHO
// via ch.6 same as it already does for hostile targeting, and WHAT+HAND via
// ch.8's ACT claim). A non-self-delivery spell (already aimed) prefers the ch.6
// claim, then the actor's own live combat target (engine-answer-first, read-only,
// never selected by APMF), then falls back to self rather than firing at nothing.
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
