#pragma once
#include "APMF_API.h"

// ============================================================================
// Channel 21 -- COMBAT ENTRY (kIntent_CombatEntry, ABI v14). See channels/CombatEntry.cpp
// for the design; Docs/CHANNEL-MAP.md row 21, Docs/DENY-COMPLETENESS-AUDIT.md row 21,
// Docs/INVARIANTS.md #0 (g), Docs/INTEGRATION.md's v14 section.
// ============================================================================

namespace apmf::combatentry {

    // Gate the channel: exactly 1.6.1170 / 1.5.97, not VR, [CombatEntry] bCombatEntry=1,
    // and the address self-check on Actor::StartCombat (the one engine function this
    // channel calls). Installs NO hook. Call at kDataLoaded. Idempotent. A refusal keeps
    // its reason for NotInstalledReason. GAME THREAD.
    void Install();

    // Is the channel usable? False before kDataLoaded and after any refusal above. A
    // false answer makes ControlMap::EnqueueRequest REFUSE a kIntent_CombatEntry claim
    // synchronously (kInvalidHandle) -- the ch.17 / ch.19 / ch.20 "seat down = claim
    // refused" contract. ANY THREAD (one relaxed atomic load).
    bool Installed();

    // Why Installed() is false, for the one-time refusal log. Never null. Any thread.
    const char* NotInstalledReason();

    // End-of-claim monitor, from Arbiter::OncePerFrame (the ch.19 / ch.20 Poll seat).
    // Self-throttled (250 ms); one relaxed atomic load while nothing is claimed. When the
    // owner is dead, or the target is dead, disabled, not loaded or unresolvable, it ENDS
    // the winning claim itself (EnqueueRelease) and logs the reason; the Release line
    // repeats it. It never re-enters combat and writes no engine state. GAME THREAD.
    void Poll();

    // Drop every per-actor entry (the world is being replaced), beside
    // targetpin::ResetAll at the kPreLoadGame / revert boundary. An entry task already
    // posted and not yet run finds no entry and drops itself. GAME THREAD.
    void ResetAll(const char* why);

}
