#pragma once

// ============================================================================
// core/ActorValueObserve -- HARBINGER 1b: the OBSERVE-ONLY actor-value seat (2026-10-12).
// See ActorValueObserve.cpp's header for the seat, its verification and the rule.
//
// Three write_vfunc on Character's ActorValueOwner vtable (VTABLE_Character[5]): 0x04
// SetBaseActorValue (SetActorValue 0x07 tail-jumps into it), 0x05 ModActorValue, 0x06
// RestoreActorValue. A write to an actor value Harbinger holds for a WINNING claim (ch.16
// detection: MovementNoiseMult / DetectLifeRange; ch.1a gait: SpeedMult; ch.11 disposition:
// Aggression / Confidence / Assistance / Morality), by anyone but APMF, is named by its
// caller module and counted on the Detection / ActorValue facet. Nothing is ever denied,
// changed or delayed: every call runs the original with the original arguments.
// ============================================================================

namespace apmf::avobserve {

    // kDataLoaded, MAIN thread. VR- and exact-runtime-gated (1.6.1170 / 1.5.97 / 1.7.104),
    // [ActorValueObserve] iActorValueSeat (0 = not installed, 1 = observe, default 1),
    // SeatVerified() on the ActorValueOwner vtable row of the running build.
    void Install();

    // ~30 s RULE C totals line (zeros included). Arbiter::OncePerFrame, MAIN thread.
    void Heartbeat();

}
