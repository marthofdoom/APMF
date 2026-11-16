#pragma once

namespace apmf::hook {
    // Patch the Character + PlayerCharacter vtables at Actor::Update index 0xAD
    // (once), routing every NPC tick through the arbiter. VR-refused. Idempotent.
    void Install();

    // APMF-B45 (F2b): why the 0xAD arbiter seat -- the ONLY thing that drains and
    // engages claims -- will never run, or nullptr while it is installed or not yet
    // attempted (Install runs at kDataLoaded). ControlMap refuses every new claim
    // (kInvalidHandle) while this is non-null, so a client's degrade path runs instead
    // of a handle that never drains. Any thread; the reason is a string literal.
    const char* RefusedReason();

    // True when the CALLING thread is the one the PlayerCharacter Update seat runs on
    // (the thread ControlMap::Drain and apmf::mainthread::Pump run on). False before
    // that seat has ticked once. The ABI v11 space queries refuse to run anywhere else
    // (APMF_API.h, "ABI v11: SPACE QUERIES"). Any thread; two relaxed reads.
    bool OnMainThread();

    // ---- P0: the passive OVERLAP PROBE (2026-11-16, stability audit P0 / H1) ----------------
    // Does an NPC's own update run on another thread at the moment Harbinger's main-thread pump
    // touches combat state? Two counters, each the number of threads inside that update right now:
    // the Character 0xAD thunk (NPC Actor::Update, the engine's "Actor update" job list) and the
    // outermost Harbinger layer on Character::UpdateCombat 0xE4 (the "Combat and magic" job list,
    // channels/TargetPin.cpp). The pump SAMPLES both at its begin and end, and at every one of
    // Harbinger's own main-thread combat calls (StartCombat, RemoveTarget, a StopCombat made on
    // main, the anchor move, the save flush). One RULE C line every 60 s, zeros included:
    // `[overlap] ...`. Passive: no engine call, no state anyone reads back. Sample: MAIN thread.
    enum class Site : std::uint8_t {
        kPumpBegin,       // PlayerCharacter 0xAD thunk, before Arbiter::OncePerFrame
        kPumpEnd,         // ... after it
        kStartCombat,     // ch.21 entry / the substrate's entry (Actor::StartCombat, main)
        kRemoveTarget,    // CombatGroup::RemoveTarget under the group's write lock (main)
        kStopCombatMain,  // an Actor::StopCombat made ON MAIN (the save flush / save scope, or an undrained op)
        kAnchorMove,      // the substrate anchor's MoveTo / SetPosition (main)
        kSaveFlush,       // kSaveGame, before the queued combat ops run inline
        kCount
    };
    void Sample(Site a_site);
    // The outermost Harbinger 0xE4 layer brackets the whole chain with these (any thread).
    void CombatUpdateEnter();
    void CombatUpdateLeave();
    // MAIN, once per frame (the player seat): the 60 s `[overlap]` line.
    void OverlapHeartbeat();
}
