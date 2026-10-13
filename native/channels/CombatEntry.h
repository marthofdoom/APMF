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

// ============================================================================
// THE HELD ENGAGEMENT (marth 2026-10-12: "once an engagement is called in harbinger its move-to, it
// cannot be lost"). A client-called engagement -- the winning ch.21 entry and the winning ch.20 pin
// of an actor -- is HELD: the target the client named is never dropped because the ENGINE's own
// detection lost it. See channels/CombatEntry.cpp "THE HELD ENGAGEMENT" and Docs/INVARIANTS.md
// #0 (i).
// ============================================================================
namespace apmf::combatentry::hold {

    // kDataLoaded, after targetpin::Install / combatentry::Install: reads [EngagementHold], and
    // installs the KNOWLEDGE seats (two write_call<5> post-call hooks on the engine's
    // CombatGroup::UpdateTargetDetection, the only writer of a combat target's kTargetLost) on
    // exactly 1.6.1170 / 1.5.97 after the address self-check. GAME THREAD. Idempotent.
    void Install();

    // [EngagementHold] bEngagementHold (default 1): ch.20 does not end a pin on "target lost",
    // ch.23 does not leash the held target's approach, the hold Poll runs. Any thread.
    bool Enabled();

    // Are the knowledge seats installed? False = the hold DEGRADES (review F-4): ch.20 ends a pin on
    // "target lost" as before. Any thread.
    bool KnowledgeInstalled();

    // ch.20 / ch.21 register what they hold (GAME THREAD; Apply on Engage / OnOwnerChanged, Clear
    // on Release / ResetAll). `handle` = the target's ActorHandle, resolved on the game thread.
    // `range` = ch.21's param.fval, the client's engage range in game units (0 = no range rule).
    void SetPin(RE::FormID actor, RE::FormID target, RE::ActorHandle handle);
    void ClearPin(RE::FormID actor);
    void SetEntry(RE::FormID actor, RE::FormID target, RE::ActorHandle handle, float range);
    void ClearEntry(RE::FormID actor);

    // Does `actor` hold an engagement on the target whose ActorHandle native value is `handle`?
    // 0 = no, 1 = via its ch.20 pin, 2 = via its ch.21 entry. ANY THREAD (shared lock on the hold
    // map, the ch.20 / ch.23 precedent). Counts a leash exemption when `countLeash` is set.
    int HeldVia(RE::FormID actor, std::uint32_t handle, bool countLeash = false);
    // Does `actor` hold any engagement at all? ANY THREAD (shared lock).
    bool HoldsAny(RE::FormID actor);

    // The re-engage COOLDOWN: after Harbinger ENDED an engagement itself (a watchdog anomaly, or the
    // range rule), a new request of the same actor -> target is refused for 60 s (the range rule
    // refuses a ch.21 re-entry only; a watchdog end refuses a ch.20 pin too). Logged once per
    // cooldown. ANY THREAD. Asked by ch.20 / ch.21 Apply: the claim is ACCEPTED and ended one frame
    // later ("re-engage cooldown"), never refused synchronously -- a synchronous refusal means "seat not
    // installed" to clients (MFO drops its pin / entry route for the session on it).
    bool RefuseReengage(RE::FormID actor, RE::FormID target, bool isPin);

    // Game thread, Arbiter::OncePerFrame (after the ch.20 / ch.21 Polls): per-hold telemetry
    // (distance moved per 5 s window, distance to the target, attack / cast starts, engine-lost
    // overrides), the no-progress watchdog (loud anomaly + release) and the bleed-out drive.
    void Poll();

    // ~30 s RULE C line (zeros included) while any hold exists. Game thread.
    void Heartbeat();

    // kPreLoadGame / revert: drop the per-actor telemetry + drive state (the hold map itself is
    // cleared by ch.20 / ch.21 ResetAll). GAME THREAD.
    void ResetAll(const char* why);

}
