#pragma once
#include <cstdint>
#include <functional>
#include <string>

// ============================================================================
// THE OWNER-THREAD COMBAT OPS (stability audit H1 / APMF-B26, 2026-11-16).
//
// WHY. Actor::StopCombat (Character slot 0xE5) frees the actor's CombatController INLINE, with no
// lock and no reference count (AE 0x6B7214 -> ~CombatController 0x558500, 0x6B7235 nulls +0x160;
// MFO ENGINE_NOTES 0.47). Harbinger used to call it from its main-thread pump (the substrate close,
// the held engagement's range rule, the anchor guard, the load clean). That pump runs at the END of
// PlayerCharacter::Update, and the engine runs the NPC updates beside it: the frame job graph is
// kicked before the player update (Main::Update AE 0x64613A / 0x64613F), the "Actor update" list
// (every high NPC's Actor::Update, our Character 0xAD seat) runs alongside it, and the player update
// signals its mid-point event (AE 0x7330E7 -> 0x733D10 -> SetEvent) before our thunk runs, so the
// "Combat and magic" list (every combat member's UpdateCombat, our 0xE4 layers and every combat seat
// that reads the controller) can run while the pump is still going. A main-thread StopCombat could
// therefore free a controller that actor's own update was reading. The P0 probe (core/Hook.h)
// measures how often the two meet.
//
// WHAT THIS DOES. The free happens where the engine itself frees it: on the actor's OWN update. A
// StopCombat Harbinger decides on main is QUEUED for that actor and run inside the actor's own
// Character::UpdateCombat (0xE4) job, at the outermost Harbinger layer, BEFORE the chain below runs
// (channels/TargetPin.cpp E4Outer): nothing of that actor's update is in flight on that thread, and
// the engine frees controllers from the same job lists itself (AE 37347 calls StopCombat inside the
// per-actor "Actor update" job 0x7706B0 at 0x667BA2, before the actor's own Actor::Update; the
// UpdateCombat tail's message processor 0x656F30 calls it at 0x658018). Every actor that HAS a
// controller is a member of a combat group, and the CombatManager walker (AE 0x83D070, id 46872,
// from the "Combat and magic" job 0x6D2920) pushes one UpdateCombat job for every member of every
// group, every frame, so a queued op meets its actor's update within one frame of game time.
// The decision stays on main (the ranks, the claims, the logs); only the free moves.
//
// THE CONTROLLER IT WAS DECIDED FOR. The queue records the controller pointer main saw (identity,
// never dereferenced). The drain stops only THAT controller: a different one (a new fight since the
// decision) or none (the engine ended it first) is skipped and said in the result.
//
// NOT DRAINED. Main checks every pending op once per frame (Poll):
//   * the actor has no controller any more (the engine ended the fight, a kill, an unload): nothing
//     is left to stop, and an actor without a controller gets no UpdateCombat job -- the op is
//     cancelled and its result says so;
//   * the game ran kUndrainedSec of UNPAUSED time (the engine's own frame step, 0 while paused) with
//     the actor still holding a controller and the op still queued: that actor's combat update is
//     not running at all, so a main-thread StopCombat cannot meet it. It is made on main with an
//     ERROR line (a documented degrade, counted, never silent -- principle 7).
//
// THE SAVE (INVARIANTS "THE SAVE-TIMING RULE"). kSaveGame runs inside the save call, before the
// engine writes the actor records, and a queued op would miss it. plugin.cpp's kSaveGame case runs
// FlushForSave() first (every pending op inline, on main) and wraps the restore passes in a
// SyncScope (an op asked inside it runs inline). Both save seats run where the frame's job graph is
// idle: BGSSaveLoadManager's queued save at Main::Update AE 0x645F3A and the autosave chain at
// 0x645F0E come after the graph's events are reset (0x645EDE) and before the frame's kick
// (0x64613A); a menu save runs with the game paused. The P0 probe samples the flush (Site::kSaveFlush)
// so the field proves it.
//
// THE LOAD. ResetAll (kPreLoadGame / revert) drops every op unrun (the outgoing world's actors) and
// the main-thread Discard drops the completions already posted.
//
// AVAILABILITY. Needs the 0xE4 layer 0 (channels/TargetPin.cpp InstallLayer0, [TargetPin]
// bTargetPin) and the verified Character vtable (the StopCombat identity check). Without them
// StopCombat() refuses (false) and the callers refuse their StopCombat by name: the held engagement's
// StopCombatAvailable() includes this, and the combat substrate refuses to install without it.
//
// THREADING. StopCombat / Poll / FlushForSave / ResetAll / SyncScope: MAIN thread. DrainFor: the
// actor's own UpdateCombat job (any worker, or main when main helps run jobs). Pending: any thread.
// One leaf mutex guards the queue (no engine call, no log, no other lock under it).
//
// QUEUED, RUNNING, DONE (review F1 / F3 of 75e1f75). The drain marks the entry RUNNING under the mutex and leaves
// it in the queue while Actor::StopCombat runs; it is erased (by its seq) only after StopCombat returned, and its
// completions -- every one attached meanwhile -- are posted then. So Pending() is true from the decision until the
// free is over, and main's #4a readers (INVARIANTS #4a) skip the actor for that whole time. Main never takes a
// running entry: Poll skips it (a running op is not "gone" and not "undrained"), FlushForSave leaves it. A drain
// that throws still answers its op ("not stopped" unless Execute returned); one that dies without answering is
// STUCK after kUndrainedSec of unpaused game time and Poll answers "not stopped" with an ERROR.
// ============================================================================

namespace RE {
    class Actor;
}

namespace apmf::combatops {

    enum class How : std::uint8_t {
        kNone,           // not run (the result of a refused request; never delivered)
        kOwnerUpdate,    // drained on the actor's own UpdateCombat, before the chain
        kSaveSync,       // inline on main inside the save call (FlushForSave / SyncScope)
        kMainUndrained,  // ERROR: the actor's combat update never ran; made on main
        kMainFaulted,    // ERROR: the drain faulted earlier this session; made on main
        kGone,           // cancelled on main: the actor no longer had a controller
        kDropped,        // dropped unrun at a load / revert (never delivered)
    };

    struct Result {
        RE::FormID  actor   = 0;
        bool        stopped = false;   // Actor::StopCombat was called
        How         how     = How::kNone;
        const char* why     = "";      // a literal: why it was skipped (empty when stopped)
    };
    using Done = std::function<void(const Result&)>;

    // kDataLoaded, after channels/TargetPin.cpp InstallLayer0 (plugin load) and before the held engagement and
    // the combat substrate install (both ask Available()). Main thread.
    void Install();
    bool        Available();
    const char* UnavailableWhy();   // never null

    // Queue Actor::StopCombat for `a_actor` on its own combat update (see the header). MAIN thread. Coalesces
    // with a pending op for the same actor, queued OR running (one StopCombat, every completion runs; review F5
    // of 75e1f75). `a_done` runs on MAIN once the op ran, was skipped or was cancelled: posted (apmf::mainthread)
    // after a drain, inline when main ran it (inside a SyncScope -- then BEFORE this returns -- or from Poll).
    // `a_inline` (optional) is set true when it ran inline here (its completion already ran), false when it was
    // queued or joined one: the caller's log says which without re-asking Pending (a drain may finish in between).
    // Returns false when not available, or no op is pending and the actor has no controller (nothing queued,
    // `a_done` not called).
    bool StopCombat(RE::Actor* a_actor, const char* a_who, Done a_done = {}, bool* a_inline = nullptr);

    // Attach `a_done` to the op pending for this actor (queued or running), without a new decision. False = none
    // is pending (nothing attached). MAIN thread. (Review F5 of 75e1f75: the held engagement's "already queued"
    // path keeps its caller's completion.)
    bool Attach(RE::FormID a_actor, Done a_done);

    // A StopCombat Harbinger decided for this actor is QUEUED or RUNNING (review F1 of 75e1f75: the entry stays
    // until the drain's Actor::StopCombat RETURNED). INVARIANTS #4a: while this is true nothing on main reads
    // THROUGH this actor's combat controller (its group, its target, its state, Actor::StartCombat, IsInCombat):
    // the drain may be freeing it on a combat job right now. The controller POINTER may still be read. Any thread.
    bool Pending(RE::FormID a_actor);

    // Actor::IsInCombat (Character slot 0xE3 reads controller+0x43: AE 0x6B6DD0, SE 0x625660, 1.7.104 0x6C9850)
    // made #4a-safe for MAIN: while a stop is Pending, the answer is "it still has a controller" (the pointer only).
    bool InCombat(RE::Actor* a_actor);

    // How a result came about, for a log line.
    const char* HowText(How a_how);

    // THE DRAIN. The outermost Harbinger layer on Character::UpdateCombat (0xE4), BEFORE the chain below runs.
    // Any thread (the actor's own job). One relaxed load when nothing is queued. Never throws.
    void DrainFor(RE::Actor* a_this);

    // MAIN, once per frame (Arbiter::OncePerFrame, right after mainthread::Pump so the posted completions of
    // this frame's drains are delivered first): the not-drained checks above and the 60 s RULE C line.
    // `a_delta`: the engine's own world-time frame step (0 while paused).
    void Poll(float a_delta);

    // kSaveGame, FIRST (plugin.cpp): every pending op inline on main, completions inline. MAIN.
    void FlushForSave();

    // kPreLoadGame / revert: every pending op dropped unrun, no engine call, no completion. MAIN.
    void ResetAll(const char* a_why);

    // Inside one, StopCombat() runs the op inline on main and calls its completion before returning. The save
    // passes only (plugin.cpp kSaveGame). MAIN; nests.
    class SyncScope {
    public:
        SyncScope();
        ~SyncScope();
        SyncScope(const SyncScope&)            = delete;
        SyncScope& operator=(const SyncScope&) = delete;
    };

}
