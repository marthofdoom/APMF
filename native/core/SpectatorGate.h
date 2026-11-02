#pragma once

// ============================================================================
// core/SpectatorGate -- the PACKAGE facet's seat for the engine's Spectator package
// (ClickUp 86e3m5feu, 2026-10-12). See SpectatorGate.cpp's header for the engine path
// (disassembled on 1.6.1170, 1.5.97 and 1.7.104) and the decision rule.
//
// One write_vfunc on VTABLE_Character[0] slot 0xDC, Actor::InitiateSpectator. While an
// actor holds a winning ch.9 kIntent_OfferPackage claim whose package resolves, and the
// claim's client outranks the Engine on the Package facet (APMF_Sources.ini), the
// engine's Spectator package is not created and the claimed package keeps the facet.
// The Spectator package is attributed to the Engine source on the Package facet.
// Since 2026-11-01 the same seat also refuses a Spectator package whose TARGET is a HELD combat
// substrate holder or the substrate anchor (the town-panic facet), ranked as the substrate's claim.
// ============================================================================

namespace apmf::spectatorgate {

    // kDataLoaded, MAIN thread, after packagegate::Install(). VR- and exact-runtime-gated,
    // [PackageGate] iSpectatorSeat (0 = not installed, 1 = observe only, 2 = enforce,
    // default 2), SeatVerified() on the Character vtable.
    void Install();

    // ANY THREAD. The seat is installed in ENFORCE mode, so it refuses a Spectator package that watches a HELD combat
    // substrate holder or the anchor (when the substrate's claim outranks the Engine on the Package facet).
    // channels/CombatSubstrate.cpp logs a known-hole line at each OPEN otherwise.
    bool Enforcing();
    // ANY THREAD, never null: why Enforcing() is false ("" when it is true).
    const char* NotEnforcingReason();

    // ~30 s RULE C totals line (zeros included). Arbiter::OncePerFrame, MAIN thread.
    void Heartbeat();

    // Revert / new game: forget the per-actor transition memory of the outgoing world.
    void ResetAll();

}
