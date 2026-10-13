#pragma once
#include "APMF_API.h"

// ============================================================================
// Channel 20 -- TARGET PIN (kIntent_TargetPin, ABI v13). See channels/TargetPin.cpp
// for the design; Docs/CHANNEL-MAP.md row 20, Docs/DENY-COMPLETENESS-AUDIT.md row 20,
// Docs/INVARIANTS.md #0 (f), Docs/INTEGRATION.md's v13 section.
// ============================================================================

namespace apmf::targetpin {

    // Harbinger 1a: install UpdateCombat layer 0 (Character vtable slot 0xE4) at APMF's own
    // SKSEPluginLoad, as early as APMF can, so the 0xE4 hooks other plugins install later land above
    // it and each gets a Harbinger layer of its own (the census re-hooks). Same gates as Install().
    // MAIN thread, before kDataLoaded. Install() refuses the whole pin if this did not install.
    void InstallLayer0();

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
    // the target is dead, disabled, unloaded, unresolvable, or -- ONLY with [EngagementHold]
    // bEngagementHold=0 -- LOST (the seat saw kTargetLost in the group entry; since marth's
    // 2026-10-12 ruling a held pin is never ended for that), it ENDS the winning claim itself (EnqueueRelease) and logs the
    // reason; the Release line repeats it. A target that is merely not a combat target yet
    // does NOT end the pin. Not a re-assert: it writes no engine state. GAME THREAD.
    void Poll();

    // THE HELD ENGAGEMENT (2026-10-12): since marth's ruling a pin no longer ends on "target lost"
    // (unless [EngagementHold] bEngagementHold=0). The held engagement's no-progress watchdog
    // (channels/CombatEntry.cpp hold::Poll) ends this actor's WINNING pin claim through here when it
    // still names `target`: logged "pin ended: <why>", repeated on the Release line. `why` must be a
    // string literal. GAME THREAD, never inside Drain.
    void EndHeld(RE::FormID actor, RE::FormID target, const char* why);

    // Drop every per-actor pin entry (the world is being replaced). For the kPreLoadGame /
    // revert boundary, beside travel::ResetAll(): ControlMap::Clear() makes no
    // channel->Release calls, so without this the entries of the outgoing world would
    // stay. They could not pin anything on their own (the seat also requires the
    // published claim), but they must not outlive their world. GAME THREAD.
    void ResetAll(const char* why);

}
