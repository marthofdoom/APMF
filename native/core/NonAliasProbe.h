#pragma once

// ============================================================================
// OBSERVE-ONLY diagnostic for Docs/PROBE-NONALIAS-PACKAGE.md's runtime
// question: when a follower is APMF-claimed and the framework's non-alias
// package is fighting for control (the Cicero / `0009BE51` field symptom --
// a plain, non-alias package that only the existing 0x49 re-assert loop can
// contest), does `Actor::CheckForCurrentAliasPackage` (0x49, the existing T3
// package-offer gate, core/PackageGate.cpp) actually get CALLED for that
// actor at all, and does `Actor::PutCreatedPackage` (0xDF -- the only other
// package-shaped `Actor` vtable slot; PROBE doc §3 row 2) ever carry that
// actor's assigned package through?
//
// NO decision, NO denial, NO argument or return-value change on either hook
// -- pure logging, chained to the original unconditionally (INVARIANTS #17:
// write_vfunc ONLY, engine-answer/chain first). This file:
//   1. Owns the 0xDF (PutCreatedPackage) observe hook (RTTI-verified via
//      core/Allowance.h's InstallOnVtables, same VTABLE_Character[0] symbol
//      core/PackageGate.cpp's 0x49 hook already uses).
//   2. Exposes IsEnabled()/RateLimitOK() so core/PackageGate.cpp's EXISTING
//      0x49 thunk can add its own observe-log line on the SAME debug switch
//      and the SAME per-actor rate limit, without a second toggle key.
//   3. Owns a runtime vtable/RTTI dumper (DumpActor) -- deliverable 2: a
//      reusable "find sites ourselves" tool so future probes don't need a
//      fresh hand-reversing pass; logs the vtable's module-relative RVA, the
//      RTTI type name (best-effort, see .cpp), the first ~0x60 slot RVAs,
//      and the two known slots (0x49, 0xDF) called out by index.
//
// HOTKEYS (both OFF by default; see .cpp Install()/OnHotkey()). Every one of
// the 17 conventional Numpad0-9/./+/-/*//Enter scancodes is ALREADY claimed
// by a real channel or the native-bit probe (grep confirmed, 2026-09-03) --
// so this uses the two adjacent, genuinely-unclaimed lock-key scancodes
// instead, and says so plainly rather than silently double-booking an
// already-owned key:
//   - DIK 0x45 (NumLock)    -- toggle deliverable-1 observe logging (0x49 +
//                              0xDF) on/off. OFF by default; while off, both
//                              hooks cost one relaxed atomic load and
//                              nothing else -- no per-frame log noise.
//   - DIK 0x46 (ScrollLock) -- one-shot: dump the crosshair-aimed actor's
//                              vtable/RTTI (deliverable 2). Independent of
//                              the NumLock switch -- always armed once
//                              Install() has run.
// ============================================================================

namespace apmf::nonaliasprobe {

    // Install the 0xDF (PutCreatedPackage) observe hook. RTTI-verified
    // (Allowance::DerivesFrom against RE::RTTI_Character) on
    // VTABLE_Character[0] ONLY -- the SAME single vtable core/PackageGate.cpp's
    // 0x49 hook uses (never PlayerCharacter, §0.38). Chains to the original
    // unconditionally, never alters the incoming TESPackage*/bools or the
    // (void) return. VR-refused (0xDF is SE/AE-only verified, matching the
    // 0x49 gate's own VR gate -- Docs/PROBE-NONALIAS-PACKAGE.md never
    // resolved the VR layout). Call once at kDataLoaded, after
    // packagegate::Install(). Idempotent.
    void Install();

    // Dispatch a raw keyboard scancode: NumLock toggles the debug switch,
    // ScrollLock fires a one-shot vtable/RTTI dump on the crosshair-aimed
    // actor. Called from core/Input.cpp's InputSink alongside
    // nativebitprobe::OnHotkey. No-op for any other code.
    void OnHotkey(std::uint32_t a_code);

    // Debug-switch state (relaxed read). Exposed so core/PackageGate.cpp's
    // 0x49 thunk can gate its own observe-log addition on this same switch.
    bool IsEnabled();

    // Shared per-actor rate limiter (2s window) for BOTH the 0x49 log
    // (called from PackageGate.cpp) and this file's own 0xDF log -- one
    // shared table so a busy actor sighted on both hooks in the same tick
    // doesn't double the spam. Returns true (log it) at most once per actor
    // per window; false otherwise. Callers must check IsEnabled() first --
    // this does not itself consult the switch.
    bool RateLimitOK(RE::FormID a_actor);

}
