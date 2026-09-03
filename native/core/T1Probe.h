#pragma once

// ============================================================================
// T1 PROBE -- combat behavior-tree leaf `Enter`/`act` (slot 0x02 on the ~70
// `VTABLE_CombatBehaviorTreeNodeObject_*` leaf vtables). Docs/ALLOWANCE-
// TEMPLATE.md §2 item 1 / §6 3-cycle probe steps 0-1. THROWAWAY;
// INSTRUMENTATION ONLY -- observe-only Phase 0, one deny test in Phase 1.
// NOT wired to any client, NOT a permanent channel. Field-test-first: see
// Docs/PROBE-ALLOWANCE.md for the hotkey map and pass/fail criteria.
//
// Phase 0 (OBSERVE): every leaf's act() is hooked but ALWAYS calls the
// original and returns its result unmodified -- pure observation. Logs the
// first time each leaf fires (leaf identity via our own install-time name
// table AND the object's own GetName() slot-1 virtual, unhooked) for any
// actor in the SHARED claim SET (core/ProbeClaimSet -- multiple actors at
// once, generalized 2026-09-03 from a single claimed actor so a whole
// battle can be claimed and denied at once; global install, per-actor
// LOGGING gate -- unclaimed NPCs pay only the orig() passthrough, no
// logging work). Also resolves the CombatBehaviorTreeControl +0x158
// ambiguity (CombatController* directly per CPR, vs the CommonLib-doc +0x20
// hop hypothesis) by reading BOTH and logging which yields a live
// attackerHandle -- RESOLVED for 1.6.1170: hypothesis B (the +0x20 hop).
//
// Phase 1 (DENY): a hotkey denies ONLY the `CombatBehaviorAttack` leaf, for
// EVERY actor in the claim set, via the engine's own failure protocol --
// invoking `CombatBehaviorForceFail`'s own ORIGINAL act() implementation
// directly (not a hand-reconstructed `SetFailed` call; see T1Probe.cpp's
// file header for the field-crash history and why).
// ============================================================================

namespace apmf::t1probe {

    // Install slot-0x02 OBSERVE hooks on all 70 leaf vtables (once). VR-refused.
    void Install();

    // Route a test-surface hotkey. No-op unless armed + a T1 scancode.
    void OnHotkey(std::uint32_t a_code);

    // Game-thread pump: periodic (~5s) OBSERVE census + any queued log flushes.
    // Call once per frame (Arbiter::OncePerFrame). No-op when disarmed.
    void OncePerFrame();

    // Drop the claim without touching the engine (kPreLoadGame -- actors are
    // about to be replaced; nothing to restore, mirrors AliasPkgProbe).
    void ClearOnPreLoad();

}
