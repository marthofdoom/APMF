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
// table AND the object's own GetName() slot-1 virtual, unhooked) for the
// CLAIMED actor only (hotkey-armed on the crosshair-aimed NPC; global
// install, per-actor LOGGING gate -- unclaimed NPCs pay only the orig()
// passthrough, no logging work). Also resolves the CombatBehaviorTreeControl
// +0x158 ambiguity (CombatController* directly per CPR, vs the CommonLib-doc
// +0x20 hop hypothesis) by reading BOTH and logging which yields a live
// attackerHandle.
//
// Phase 1 (DENY): a second hotkey denies ONLY the `CombatBehaviorAttack`
// leaf for the claimed actor via the engine's own failure protocol
// (`CombatBehaviorTreeControl::SetFailed(true)`, resolved at install time by
// disassembling `CombatBehaviorForceFail`'s own act() body for its internal
// CALL to SetFailed -- see T1Probe.cpp -- since SetFailed's SE Address-
// Library id (46240) has no known AE counterpart in any header this project
// can reach; the ForceFail-body derivation works on SE+AE uniformly because
// it reads the ACTUAL compiled bytes at runtime rather than a static table).
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
