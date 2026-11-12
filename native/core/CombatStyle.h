#pragma once
#include "APMF_API.h"

// ============================================================================
// core/CombatStyle -- ch.33 COMBAT STYLE (kIntent_CombatStyle, ABI v45, MFO T2 batch M item 5 "strict class style").
// The two call-site seats and their state; channels/CombatStyle.cpp is the arbitration half. Design, engine path,
// per-runtime proof and deny-completeness: core/CombatStyle.cpp. Docs: FACETS ch.33, CHANNEL-MAP row 33,
// INVARIANTS #0 (n), DENY-COMPLETENESS-AUDIT row 33.
// ============================================================================

namespace apmf::combatstyle {

    // Prove and install the two call-site seats (kDataLoaded, GAME THREAD, once). Refuses by name on VR, on a
    // runtime other than exactly 1.6.1170 / 1.5.97 / 1.7.104, on [CombatStyle] bCombatStyle=0, on a failed address
    // row or byte window. Both seats install or neither does.
    void Install();
    bool        Installed();             // any thread
    const char* NotInstalledReason();    // never null; any thread

    // ControlMap::EnqueueRequest: why a kIntent_CombatStyle request must be refused (nullptr = accept). Any thread.
    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param);

    // The channel's lifecycle (GAME THREAD). SetClaim resolves param.form to a TESCombatStyle and publishes the
    // actor's slot for the seats (not live until the next Heartbeat has ranked the claim against the Engine source).
    // `a_actor` may be null (the slot is then dropped: inert).
    void SetClaim(RE::FormID a_id, RE::Actor* a_actor, const APMF_API::APMF_Param& a_param, const char* a_what);
    void ClearClaim(RE::FormID a_id);

    // The world is being replaced (revert / kPreLoadGame): drop every slot (ControlMap::Clear makes no
    // channel->Release calls). No engine call: the controllers belong to the outgoing world. GAME THREAD.
    void ResetAll(const char* a_why);

    // GAME THREAD, once per frame from Arbiter::OncePerFrame (one relaxed load while no claim stands and nothing
    // waits): rank new claims against the Engine source on the CombatAction facet (again every 5 s), have the
    // engine refresh the controller's cached style after an engage / owner change / release (its own
    // CombatController::UpdateCombatStyle), and the RULE C totals about every 30 s.
    void Heartbeat();
}
