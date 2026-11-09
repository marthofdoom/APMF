#pragma once
#include "APMF_API.h"

#include <cstdint>

// ============================================================================
// core/AttackSelect -- ch.18 ATTACK SELECTION (kIntent_AttackSelection, ABI v34, ClickUp 86e3bw775;
// marth 2026-10-27 "weight seat ALLOWED", 2026-11-09 floor rulings). See AttackSelect.cpp's header for
// the engine path, the evidence and every verified byte.
//
// THE SEAT. StartAttack (AE 49170 / SE 48139 / 1.7.104 raw 0x8B7D60) weighs each candidate attack entry
// of the roll with one call (AE/1.7.104 +0x28D, SE +0x2AE) to the weight function (AE 50675 / SE 49748 /
// 1.7.104 raw 0x8F2830), then draws at weighted random among the candidates with weight > 0. Harbinger
// rewrites that ONE call (the #17a amendment of 2026-11-09): the engine's weight is computed first, and
// for an actor whose winning ch.18 claim denies the candidate's class the call answers 0 instead, so the
// engine's own draw never picks it. Every other candidate, actor and caller keeps the engine's answer.
//
// THREADING. The thunk runs on the combat AI thread that runs the actor's behaviour tree. It reads the
// ControlMap RCU snapshot, this file's published declaration snapshot (atomic shared_ptr), the roll's own
// context (the engine's data, in place, bounded) and takes one LEAF mutex for the per-actor window and
// counters (no engine call under it). NoteClaim / Set / Forget / ResetAll / GetState: any thread.
// Install / Heartbeat: MAIN thread.
// ============================================================================

namespace apmf::attackselect {

    // kDataLoaded, MAIN thread. [AttackSelect] bAttackSelect (default 1). Exact 1.6.1170 / 1.5.97 /
    // 1.7.104, VR refused, SeatVerified rows, the site's bytes re-read before the write. Refusal = logged,
    // nothing written, every ch.18 claim refused.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // ControlMap::EnqueueRequest: why a kIntent_AttackSelection request must be refused (nullptr = accept).
    const char* RefuseReason(RE::FormID actor);
    // ClientAPI: a kIntent_AttackSelection Request/RequestEx returned `h`. Only such a handle may declare.
    void NoteClaim(APMF_API::Handle h);
    // ClientAPI::SetAttackMix. Copies `mix` synchronously, validates, publishes at once. nullptr = clear.
    void Set(APMF_API::Handle h, const APMF_API::APMF_AttackMix* mix);
    void Forget(APMF_API::Handle h);    // a client Release (a no-op for a non-ch.18 handle)
    void ResetAll(const char* why);     // revert / kPreLoadGame: declarations, windows, counters
    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_AttackMixInfo* out);

    // channels/AttackSelection.cpp Engage, writer thread: true while the site still calls Harbinger's
    // stub (another DLL may re-patch it after Install); logs loudly once when it does not.
    bool SiteIntact();

    // Arbiter::OncePerFrame, MAIN thread: the ~60 s totals line (zeros included) while the seat is
    // installed and a ch.18 claim has ever been filed this session. One relaxed load otherwise.
    void Heartbeat();

}
