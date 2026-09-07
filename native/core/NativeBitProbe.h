#pragma once

// ============================================================================
// NATIVE-BIT PROBE -- no hook. Docs/ALLOWANCE-TEMPLATE.md's "native-bit tier"
// (§1 "Native deny bits", §3 bottom row): wholesale per-domain denies via
// `Actor::BOOL_FLAGS` (`kAttackingDisabled` 1<<20, `kCastingDisabled` 1<<21)
// -- no vfunc, no RTTI, no VR gate (a plain bit flip is version-stable by
// construction). THROWAWAY; INSTRUMENTATION ONLY -- toggles a live bit on
// the crosshair-aimed NPC and logs the before/after state so marth can
// field-observe whether it cleanly stops attacking/casting or wedges/
// stutters. Not wired to any client, not a permanent channel.
//
// CONFIG-GATED, DEFAULT OFF (2026-09-07). This is the ONE probe in the
// codebase that MUTATES live actor state, so it must never be reachable in a
// shipped game: `Install()` no-ops unless
//
//     [Probe.NativeBit]
//     Enable=1
//
// is set in `Data/SKSE/Plugins/APMF.ini`, and even then the keys only arrive
// if the keyboard surface is separately armed ([Input] EnableTestSurface=1,
// core/Input.h). Two independent opt-ins, both default OFF -- CLAUDE.md's
// "no hotkeys, no toggles ... config-gated, default OFF" rule.
// ============================================================================

namespace apmf::nativebitprobe {

    void Install();
    void OnHotkey(std::uint32_t a_code);

}
