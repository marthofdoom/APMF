#pragma once
#include "APMF_API.h"

// ============================================================================
// Channel 20 -- TARGET PIN (kIntent_TargetPin, ABI v13). See channels/TargetPin.cpp
// for the design; Docs/CHANNEL-MAP.md row 20, Docs/DENY-COMPLETENESS-AUDIT.md row 20,
// Docs/INVARIANTS.md #0 (f), Docs/INTEGRATION.md's v13 section.
// ============================================================================

namespace apmf::targetpin {

    // Install the seat: Character vtable slot 0xE4 (Actor::UpdateCombat), write_vfunc,
    // chaining. Call at kDataLoaded. Idempotent. Refuses (loudly, with the reason kept for
    // NotInstalledReason) on VR, on any runtime other than exactly 1.6.1170 / 1.5.97, on
    // [TargetPin] bTargetPin=0, and when the address self-check refuses the Character
    // vtable. GAME THREAD.
    void Install();

    // Is the seat installed? False before kDataLoaded and after any refusal above. A false
    // answer makes ControlMap::EnqueueRequest REFUSE a kIntent_TargetPin claim
    // synchronously (kInvalidHandle) -- the ch.17 / ch.19 "seat down = claim refused"
    // contract. ANY THREAD (one relaxed atomic load).
    bool Installed();

    // Why Installed() is false, for the one-time refusal log. Never null. Any thread.
    const char* NotInstalledReason();

    // End-of-pin monitor (marth 2026-09-25: "If APMF can no longer track the target, it's
    // lost, and dropped"), from Arbiter::OncePerFrame -- the ch.19 Poll seat. Self-throttled
    // (250 ms); one relaxed atomic load while nothing is pinned. When the owner is dead or
    // the target is dead, disabled, unloaded, unresolvable, or LOST (the seat saw kTargetLost
    // in the group entry), it ENDS the winning claim itself (EnqueueRelease) and logs the
    // reason; the Release line repeats it. A target that is merely not a combat target yet
    // does NOT end the pin. Not a re-assert: it writes no engine state. GAME THREAD.
    void Poll();

    // Drop every per-actor pin entry (the world is being replaced). For the kPreLoadGame /
    // revert boundary, beside travel::ResetAll(): ControlMap::Clear() makes no
    // channel->Release calls, so without this the entries of the outgoing world would
    // stay. They could not pin anything on their own (the seat also requires the
    // published claim), but they must not outlive their world. GAME THREAD.
    void ResetAll(const char* why);

}
