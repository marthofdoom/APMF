#pragma once
#include "APMF_API.h"

// ============================================================================
// core/Aversion -- ch.32 AVERSION (kIntent_Aversion, ABI v41, MFO class plan card H2 "less likely to be
// targeted"). The seats and their state; channels/Aversion.cpp is the arbitration half. Design, engine path,
// per-runtime proof and deny-completeness: core/Aversion.cpp. Docs: FACETS ch.32, CHANNEL-MAP row 32,
// INVARIANTS #0 (m), DENY-COMPLETENESS-AUDIT row 32.
// ============================================================================

namespace apmf::aversion {

    // Prove and install the three call-site seats (kDataLoaded, GAME THREAD, once). Refuses by name on VR, on a
    // runtime other than exactly 1.6.1170 / 1.5.97 / 1.7.104, on [Aversion] bAversion=0, on a failed address row
    // or byte window. All three seats install or none does.
    void Install();
    bool        Installed();             // any thread
    const char* NotInstalledReason();    // never null; any thread

    // ControlMap::EnqueueRequest: why a kIntent_Aversion request must be refused (nullptr = accept). Any thread.
    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param);

    // The channel's lifecycle (GAME THREAD): the claimant's reference handle is what the seats compare a pick
    // with, so the seats never look a form up. `a_actor` may be null (the entry is then dropped: inert).
    void SetClaimant(RE::FormID a_id, RE::Actor* a_actor, const APMF_API::APMF_Param& a_param, const char* a_what);
    void ClearClaimant(RE::FormID a_id);

    // The world is being replaced (revert / kPreLoadGame): drop every claimant entry and the counters. GAME THREAD.
    void ResetAll(const char* a_why);

    // RULE C heartbeat, from Arbiter::OncePerFrame: the session totals, ~30 s after any selection landed on a
    // claimant. One relaxed load while nothing changed. GAME THREAD.
    void Heartbeat();
}
