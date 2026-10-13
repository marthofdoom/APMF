#pragma once
#include "APMF_API.h"

// ============================================================================
// Channel 12 -- IDLE / ANIMATION (kIntent_Idle). v1 (param.form == 0): one
// NotifyAnimationGraph("IdleForceDefaultState") at Engage, unchanged. v2 (ABI v17,
// param.form = a TESIdleForm, param.target = an optional reference): one
// AIProcess::PlayIdle(actor, idle, target) at Engage and, only when that idle is
// still HELD at Release, one IdleForceDefaultState at Release. See channels/Idle.cpp
// for the design; Docs/CHANNEL-MAP.md row 12, Docs/DENY-COMPLETENESS-AUDIT.md row 12,
// Docs/INVARIANTS.md #0 (c), Docs/INTEGRATION.md's v17 section.
// ============================================================================

namespace apmf::idle {

    // Gate the v2 (form) path: exactly 1.6.1170 / 1.5.97, not VR, [Idle] bIdleV2=1, and the address
    // self-check on AIProcess::SetupSpecialIdle (the engine function PlayIdle calls).
    // Installs NO hook. Call at kDataLoaded. Idempotent. A refusal keeps its reason for
    // NotInstalledReason. The v1 (form-free) path is not gated by this. GAME THREAD.
    void Install();

    // Is the v2 path usable? False before kDataLoaded and after any refusal above. A
    // false answer makes ControlMap::EnqueueRequest REFUSE a kIntent_Idle claim whose
    // param.form is non-zero, synchronously (kInvalidHandle). ANY THREAD.
    bool V2Installed();

    // Why V2Installed() is false, for the one-time refusal log. Never null. Any thread.
    const char* NotInstalledReason();

    // End-of-claim monitor + observation log, from Arbiter::OncePerFrame (the ch.19-22
    // Poll seat). Self-throttled (250 ms); one relaxed atomic load while no v2 claim is
    // engaged. Ends a v2 claim whose owner died (EnqueueRelease, logged), and writes the
    // one animation-confirmation line per play. Writes no engine state. GAME THREAD.
    void Poll();

    // Drop every per-actor v2 entry (the world is being replaced), beside the other
    // channels' ResetAll at the kPreLoadGame / revert boundary. A play task already
    // posted and not yet run finds no entry and drops itself. GAME THREAD.
    void ResetAll(const char* why);

    // ---- ABI v24: the per-HANDLE idle state (APMF_API_v24::GetIdleState). ----
    // A small table keyed by claim handle, behind its own LEAF mutex (nothing is acquired
    // while it is held, and no engine call is made under it). Written on the game thread at
    // every ch.12 transition and by the three client-call notes below; pruned by Poll.

    // APMF_API_v24::GetIdleState. ANY THREAD: one copy under the leaf mutex, no game call.
    std::uint32_t GetState(APMF_API::Handle handle, APMF_API::APMF_IdleStateInfo* out);

    // ClientAPI, the CLIENT's thread, AFTER RequestEx(kIntent_Idle) returned `handle`: a claim
    // with param->form != 0 starts PENDING. Insert-only (the game thread may already have
    // played it). A null param, form 0 or an invalid handle is ignored.
    void NoteFiled(APMF_API::Handle handle, RE::FormID actor, const APMF_API::APMF_Param* param);

    // ClientAPI, the CLIENT's thread, BEFORE EnqueueRepoint: a known, not-ended handle re-pointed
    // to a form goes back to PENDING (the new declaration has not played). Form 0 or a null
    // param leaves the record to the game thread. Unknown handles are ignored.
    void NoteRepoint(APMF_API::Handle handle, const APMF_API::APMF_Param* param);

    // ClientAPI, the CLIENT's thread, on Release: a known handle is marked ENDED (released).
    void NoteReleased(APMF_API::Handle handle);

}
