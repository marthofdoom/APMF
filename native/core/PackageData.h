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

    // Point `a_pkg`'s "Place to Travel" Location input at a CELL (locType 1,
    // "In Cell"). Same guards, same all-or-nothing contract as SetTravelTarget.
    //
    // WHY THIS IS A SEPARATE ENTRY POINT and not a flag: the two locTypes read
    // DIFFERENT members of the same 8-byte union, and that is not a guess -- it is
    // read off both unpacked images. `PackageLocation::AllocateLocation` (vtable
    // slot 1) switches on locType through a 13-entry jump table, and:
    //   case 0 kNearReference : `mov eax, DWORD PTR [rsi+0x10]`  -- a 4-BYTE read,
    //                           the ObjectRefHandle, handed to a handle lookup.
    //   case 1 kInCell        : `mov rcx, QWORD PTR [rsi+0x10]`  -- an 8-BYTE
    //                           POINTER read, null-checked, then used as `this`.
    // Writing a handle where a pointer is read (or the reverse) would hand the
    // engine a truncated or a garbage pointer, so the two writes stay separate
    // functions with separate types in their signatures.
    //
    // `a_radius` is written for symmetry, but the cell path does not use it: the
    // engine's case-1 body never reads `rad`, and APMF's own arrival test for a cell
    // is parent-cell identity, not distance.
    //
    // GAME THREAD ONLY.
    bool SetTravelCell(RE::TESPackage* a_pkg, RE::TESObjectCELL* a_cell, float a_radius);

}
