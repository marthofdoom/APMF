#pragma once

#include "APMF_API.h"

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

    // ch.23 PURSUIT LEASH (ABI v16, kIntent_PursuitLeash) -- the in-combat leash, its own
    // intent on ch.7's leaf seats. See ActionGate.cpp's PURSUIT LEASH section for the leaves,
    // the rule and the two seats.
    // True once Install() armed every half (INI, ForceFail pair, verified SetFailed/Ascend,
    // act+pop+update on all 14 leaves). ControlMap refuses a ch.23 request while false.
    bool        PursuitArmed();
    const char* PursuitNotArmedReason();
    // GAME THREAD ONLY (ch.23 Engage / OnOwnerChanged / Release). SetLeash resolves the
    // winning claim's anchor (param.target) to a handle and stores it with the radius
    // (param.fval); an unusable anchor/radius clears the entry instead (logged). The seats read the entry; the published claim
    // stays the gate.
    void SetLeash(RE::FormID a_id, const APMF_API::APMF_Param& a_param);
    void ClearLeash(RE::FormID a_id);
    // The load / revert boundary (plugin.cpp), beside the other channels' ResetAll: no
    // leash entry (a handle into the outgoing world) crosses it.
    void ResetLeash(const char* a_why);
    // RULE C heartbeat, once per frame from Arbiter::OncePerFrame (game thread); prints
    // every ~30 s while any leash is set, zeros included. Cheap otherwise.
    void PursuitHeartbeat();

}
