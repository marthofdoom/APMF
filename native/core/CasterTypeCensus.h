#pragma once

#include <string_view>

// ============================================================================
// CASTER-TYPE CENSUS (2026-09-29, feat/apmf-castertype-probe). PASSIVE: two
// observe-only vtable seats and log lines, no deny, no force, no write to the
// actor, the claim or the engine. `[Probe] bCasterTypeCensus` (code default 1
// while field testing; Docs/STATUS.md's pre-release checklist resets it to 0).
//
// THE QUESTION IT ANSWERS (combat-substrate design Q-L, principle 5). Harbinger's
// ch.8b claim seats sit on 2 of the engine's 15 CombatMagicCaster types (Restore,
// Offensive). A claimed spell is served only if the engine BUILDS one of those two
// for it. Before widening seat 0 or seating more types, this census records, per
// live cast claim, WHICH caster type the engine actually built for the driven form,
// whether the engine then FIRED it from the hand, and the ZERO case (a claim
// standing with no caster built for it at all).
//
// THE TWO SEATS (15 caster vtables, RTTI-derivation-checked, VerifiedAddresses rows):
//   0x06 CheckStartCast   bool(CombatMagicCaster*, CombatController*) -- the engine
//        has BUILT a caster of this type and asks WHETHER to cast. Called after the
//        magic context minted it (item vfunc 0x15 CreateCaster). Logged: first sight
//        per (actor, caster instance), the answer the whole chain returned.
//   0x0B NotifyStartCast  void(CombatMagicCaster*, CombatController*) -- the engine
//        RELEASED the spell from the hand: fire-and-forget right after the hand
//        caster goes Charged -> Casting (AE 0x89feb0 / 0x89f2b0 / 0x8a2369, SE
//        0x808a30), concentration right after its StartCast succeeded (AE 0x89efb8).
//        This is the "fired through the animated path" signal.
// 0x06 chains first and then observes the returned answer; 0x0B (void) observes
// and then chains. The engine's answer is never altered. Scalar / void returns, so
// the hidden-sret bug class (the GetMagicTarget CTD) cannot apply.
//
// THE WINDOWS (game thread, Poll). One window per (actor, hand, driven form,
// target) for a live, driving (not deny-only) kIntent_Cast claim. A Repoint to the
// next recipient closes the window and opens the next one in the SAME SERIES, so
// an AUTO fan run as N real casts shows as a series of windows, each with its own
// verdict and the gap between fires. Per window: the predicted classification row
// (decoded engine table, native and with seat 0's self flip), whether a controller
// existed, every caster the engine built for this actor (for the driven form and
// for anything else), fires, the claimed hand's MagicCaster state (Charging /
// Charged / Casting = the hand animated), the kInstant caster holding the driven
// form (the direct, unanimated road), and cast anim events (via core/CastObserve's
// existing sink). Milestones at 3/10/30 s while nothing is built (the ZERO case),
// a CLOSE line with a verdict, a SERIES line, and a 60 s heartbeat that prints
// even when nothing happened. One global line budget.
// ============================================================================

namespace apmf::castertypecensus {

    // kDataLoaded, AFTER castseats::Install (so these observers wrap the claim seats
    // and see the answer the whole chain gives). VR / unverified runtime refused.
    void Install();

    // Cheap pre-gate for the combat-thread seats and the anim sink: armed AND at
    // least one window open. Two relaxed atomic loads.
    bool Active();

    // GAME THREAD (Arbiter::OncePerFrame, after Drain). Self-throttled (~200 ms).
    void Poll();

    // ANY THREAD (core/CastObserve's anim-event sink). Counts cast-release / cast-begin
    // anim tags for an actor with an open window. Leaf lock, no engine call.
    void NoteAnimEvent(RE::FormID a_actor, std::string_view a_tag);

}
