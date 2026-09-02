#pragma once

// ============================================================================
// 0x49 PACKAGE-OFFER PROBE (throwaway; field-test-first). Demystifies the ONE
// intentional package-tier promote of design.md §5a: hook
// `Actor::CheckForCurrentAliasPackage` (VIRTUAL 0x49) on VTABLE_Character ONLY and,
// for an actor APMF holds a package-offer claim on, return the CLIENT's package so
// the engine runs it NATIVELY (real pathing/procedures) with exactly one
// OnPackageChange each way. This file PROVES the mechanism before anything is built
// on it -- it is NOT wired to MFO loot/travel and does NOT build travel/nav.
//
// Phased (see AliasPkgProbe.cpp): Phase 0 = does 0x49 even fire (make-or-break);
// Phases 1-3 = engage / release / save-load, driven by one test hotkey.
// VR-refused (the vtable index + EvaluatePackage reloc are SE/AE only).
// ============================================================================

namespace apmf::probe {

    // Install the 0x49 Character-vtable hook (once). Call at kDataLoaded, after the
    // 0xAD hook. VR-refused. Idempotent.
    void Install();

    // Route a test-surface hotkey (from the input sink). Toggles the package-offer
    // claim on the crosshair-aimed NPC. No-op for any other scancode / when disarmed.
    void OnHotkey(std::uint32_t a_code);

    // Game-thread pump: apply a pending EvaluatePackage (engage/release) and emit the
    // periodic Phase-0/observability summary. Call once per frame from the game-thread
    // seat (Arbiter::OncePerFrame). No-op when disarmed / nothing pending.
    void OncePerFrame();

}
