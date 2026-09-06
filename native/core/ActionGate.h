#pragma once

// T1 -- combat-action allowance (ch.7, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven T1Probe (Docs/PROBE-ALLOWANCE.md
// "Probe 1" -- observe+deny PROVEN, deck, 1.6.1170). See ActionGate.cpp for
// the design; Docs/ALLOWANCE-TEMPLATE.md §3/§7.
namespace apmf::actiongate {

    // Patch slot 0x02 (act/Enter) on all 70 CombatBehaviorTreeNodeObject leaf
    // vtables (once), RTTI-verified. Call at kDataLoaded. VR-refused
    // (the 70 vtable indices are SE/AE-only verified). Idempotent.
    void Install();

    // PFP Phase 0 (marth 2026-09-06, Docs -- see ActionGate.cpp's PFP section):
    // OBSERVE-ONLY movement-leaf heartbeat, RULE C (no silent negatives). Call
    // once per frame from Arbiter::OncePerFrame (game thread) -- self-throttles
    // internally to a coarse cadence, near-zero cost when [Probe.mvcbt] is
    // disabled (the default). Prints even when every counter is zero.
    void PfpHeartbeat();

}
