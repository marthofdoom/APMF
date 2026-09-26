#pragma once
#include "APMF_API.h"

// ============================================================================
// Channel 22 -- COMBAT RE-ENTRY DENY (kIntent_CombatReentryDeny, ABI v15). See
// channels/CombatReentryDeny.cpp for the design; Docs/CHANNEL-MAP.md row 22,
// Docs/DENY-COMPLETENESS-AUDIT.md row 22, Docs/INVARIANTS.md #0 (h), Docs/INTEGRATION.md's
// v15 section.
// ============================================================================

namespace apmf::reentrydeny {

    // Install the seat: Character vtable slot 0x99 (Actor::IsDead), write_vfunc, chaining,
    // answering only at Actor::StartCombat's own self-check call site. Call at kDataLoaded.
    // Idempotent. Refuses (loudly, reason kept for NotInstalledReason) on VR, on any runtime
    // other than exactly 1.6.1170 / 1.5.97, on [CombatReentryDeny] bCombatReentryDeny=0, and
    // when the address self-check refuses the Character vtable or the StartCombat call-site
    // row. GAME THREAD.
    void Install();

    // Is the seat installed? False before kDataLoaded and after any refusal above. A false
    // answer makes ControlMap::EnqueueRequest REFUSE a kIntent_CombatReentryDeny claim
    // synchronously (kInvalidHandle). ANY THREAD (one relaxed atomic load).
    bool Installed();

    // Why Installed() is false, for the one-time refusal log. Never null. Any thread.
    const char* NotInstalledReason();

    // End-of-window monitor, from Arbiter::OncePerFrame (the ch.19 / ch.20 / ch.21 Poll
    // seat). Self-throttled (250 ms); one relaxed atomic load while nothing is claimed. It
    // ENDS the winning claim itself (EnqueueRelease) when the window has elapsed or the owner
    // is dead, and it reports a DENY MISS loudly (the actor gained a combat controller while
    // the window held and the seat did not let a ch.21 entry through). Writes no engine
    // state. GAME THREAD.
    void Poll();

    // Drop every per-actor entry (the world is being replaced), beside
    // combatentry::ResetAll at the kPreLoadGame / revert boundary. GAME THREAD.
    void ResetAll(const char* why);

    // Review F3: the time each claim was REQUESTED (NoteRequest, from
    // ControlMap::EnqueueRequest for kIntent_CombatReentryDeny, before the op is queued) or last
    // REPOINTED (NoteRepoint, from ControlMap::EnqueueRepoint for every handle; a handle this
    // channel never recorded is ignored after one relaxed load when none is recorded). A claim's
    // window runs from that time, so a claim that takes over from a rival gets only what is left
    // of its own window. ANY THREAD (own mutex).
    void NoteRequest(APMF_API::Handle handle, RE::FormID actor);
    void NoteRepoint(APMF_API::Handle handle);

    // ch.21 PRECEDENCE. channels/CombatEntry.cpp holds one of these around its own
    // Actor::StartCombat call: while it lives, the seat lets THAT actor's entry through on
    // THIS thread (a client's declared combat entry is not denied by another client's, or
    // its own, re-entry deny). Thread-local; nests; any thread.
    class ClientEntryScope {
    public:
        explicit ClientEntryScope(RE::FormID actor);
        ~ClientEntryScope();
        ClientEntryScope(const ClientEntryScope&)            = delete;
        ClientEntryScope& operator=(const ClientEntryScope&) = delete;

    private:
        RE::FormID prev_;
    };

}
