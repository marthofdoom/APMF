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
// ============================================================================

namespace apmf::nativebitprobe {

    void Install();
    void OnHotkey(std::uint32_t a_code);

}
