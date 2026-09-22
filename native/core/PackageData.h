#pragma once

// ============================================================================
// PACKAGE-DATA ACCESS -- read a templated TESPackage's inputs BY NAME and point
// its Location input at a live reference.
//
// This is a PORT of MFO's field-run accessor (marth-follower-overhaul
// `native/Packages.cpp` -- `FindInput` / `ReadLocation` / `SetAPMFLootTravelTarget`,
// and the LAYOUT derivation at the top of that file), carried over WITH its
// guards and its reasoning rather than re-derived. It exists here because ch.19
// (`kIntent_Travel`) walks the actor for a client, so the package it
// re-points is APMF's own (`Data/APMF.esl`, `APMF_GenerateESL.py`) and the write
// has to live in APMF.
//
// EVERY function here is GAME-THREAD ONLY. They walk engine-owned package data
// and mutate a form; nothing in this file is safe from the 0xAD Character seat
// (which is NOT single-threaded -- core/Hook.cpp).
//
// The discipline is MFO's, unchanged: resolve the input BY NAME (never by index),
// guard the recovered pointer by the input's OWN reported type name, and DECLINE
// LOUDLY without writing anything if either guard fails. A half-mutated package
// would send an actor to the wrong place; a mis-modelled offset would be a silent
// memory stomp.
// ============================================================================

namespace apmf::packagedata {

    // Point `a_pkg`'s "Place to Travel" Location input at `a_ref` via a runtime
    // handle (locType 0, "Near Reference") with arrival radius `a_radius`.
    //
    // Returns false WITHOUT having written anything if the package is not a
    // templated package, if the template input is not declared, if this instance
    // supplies no value for it, or if the value slot does not report itself as a
    // Location carrier.
    //
    // GAME THREAD ONLY.
    bool SetTravelTarget(RE::TESPackage* a_pkg, RE::TESObjectREFR* a_ref, float a_radius);

}
