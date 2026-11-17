#pragma once
#include <cstdint>
#include <functional>
#include <string>

// ============================================================================
// THE COMBAT OPS: a StopCombat Harbinger decides is run BY THE ENGINE, on the engine's own road
// (stability audit H1 / APMF-B26, 2026-11-16; rebuilt the same day after the 0.20.4 field hang, v0.20.5).
//
// WHY. Actor::StopCombat (Character slot 0xE5) frees the actor's CombatController INLINE, with no lock and
// no reference count (AE 0x6B7214 -> ~CombatController 0x558500, 0x6B7235 nulls +0x160). Two places are
// wrong for it:
//   * Harbinger's main-thread pump (the 0.20.3 road). The pump runs at the END of PlayerCharacter::Update
//     and the engine runs the NPC updates beside it (the "Actor update" list; "Combat and magic" may start
//     once the player update signalled its mid-point), so the free could meet that actor's own update
//     (APMF-B26).
//   * the actor's own Character::UpdateCombat job (the 0.20.4 road, H1). THE ENGINE NEVER DOES THAT, and
//     0.20.4's claim that it does was wrong. A fight that ends inside UpdateCombat is not ended there: the
//     tail of UpdateCombat (AE 0x6B7073 / SE 0x6258FE / 1.7.104 0x6C9AF3) only POSTS task-queue message 0x2B
//     (sender AE 36960 0x656530 / SE 35985 0x5C57A0 / 1.7.104 0x668F20), and the message is run later, where
//     the engine drains its task queue (below). 0.20.4 ran Actor::StopCombat inside the "Combat and magic"
//     job list and the Deck hung at the first combat frame after a load (2026-11-16: every frame-driven log
//     stopped at that frame, the process stayed alive, no crash log). INVARIANTS #4a now forbids it.
//
// WHAT THIS DOES. Harbinger posts the ENGINE'S OWN message through the ENGINE'S OWN sender, from main:
//   sender(TaskQueueInterface*, Actor*)  ->  {type 0x2B, the actor's handle}  ->  the engine's queue.
// The engine runs it where it runs every combat end of its own: the task-queue drain (AE 36891 0x654880 /
// SE 35916 0x5C1BE0 / 1.7.104 0x667270), handler AE 0x657FEE: resolve the handle, hold the reference,
// `call [vtbl+0x728]` = Actor::StopCombat. The drain has two call sites and nothing else calls it:
//   * the "Post process" frame job (AE 39092 0x6D34B0 -> 36588 0x6479E0 @0x6479FC; SE 38136 0x640DB0 ->
//     35582 0x5B44C0 @0x5B44DC; 1.7.104 @0x65A3DC): the running game. "Post process" is in a LATER job
//     list than "Combat and magic" (the AE frame job table 0x2011928: ... Combat and magic, Destructible,
//     JobListEnd ... Post process, Poll controls, JobListEnd);
//   * Main::Update itself (AE 36564 @0x646A3C, SE 35565 @0x5B36A8, 1.7.104 @0x65941E), taken only when the
//     game is paused or time is frozen, after the frame's AI job pass was joined (AE 0x5FC210 @0x64694D).
// (SKSE's own task queue rides the same two call sites: skse64 Hooks_Threads.cpp.) So no Harbinger frame
// is on the stack when the controller is freed, and the free has the engine's own timing and lifetime
// discipline: it is the engine's own combat end. The decision stays on main (the ranks, the claims, the
// logs); only the free is the engine's.
//
// THE ENGINE'S QUEUE FLAG. The poster queues the message while the engine's flag byte is set (AE 0x20130B8
// id 381120, SE 0x1DEF8A0 id 509007; statically 1; cleared only inside AE 13754, which puts it back, and
// by AE 36551) and otherwise RUNS it on the calling thread, as it does when its queue refuses the push.
// Harbinger does not read the flag: when the controller is already gone as the sender returns, the result
// says so (How::kEngineAtPost) with a WARNING, and it is counted. That one call was then a main-thread
// StopCombat (the 0.20.3 exposure), made by the engine's own rule and never silent.
//
// DONE. The engine's queue has no completion. Main checks every pending op once per frame (Poll), reading
// only the controller's ADDRESS:
//   * no controller, or a different one than the decision named: the engine ran it (or ended the fight
//     itself first). Result: stopped, How::kEngineQueue;
//   * the SAME address after kNotRunSec of UNPAUSED game time (the drain runs every frame): NOT stopped,
//     with an ERROR line. Nothing is made on main: the caller's next pass decides again (principle 7).
// WHAT WAS LOST against 0.20.4 (flagged): the drain used to compare the controller with the one the
// decision named AT THE FREE and skip a different one. The engine's message names the ACTOR. A fight the
// engine ends and restarts between the post and the drain (the same frame) is ended by Harbinger's
// message; and a new controller the allocator puts at the old address reads as "not run" for kNotRunSec.
// Both need a new fight inside one frame of the decision.
//
// APMF-B26 (the use-after-free). Closed against Harbinger's own StopCombat exactly as far as the engine
// closes it against its own: the free is made by the engine at its own drain point. Main-thread READERS
// keep the #4a rule: nothing on main reads THROUGH an actor's controller while an op for it is pending
// (Pending() is true from the decision until Poll saw the controller gone).
//
// THE SAVE (INVARIANTS "THE SAVE-TIMING RULE"). kSaveGame runs inside the save call, before the engine
// writes the actor records. plugin.cpp's kSaveGame case runs FlushForSave() first (every pending op whose
// controller is still there is made inline, on main) and wraps the restore passes in a SyncScope (an op
// asked inside it runs inline). Both save seats run where the frame's job graph is idle: BGSSaveLoadManager's
// queued save at Main::Update AE 0x645F3A and the autosave chain at 0x645F0E come after the graph's events
// are reset (0x645EDE) and before the frame's kick (0x64613A / 0x64613F); a menu save runs with the game
// paused. The P0 probe samples the flush (Site::kSaveFlush).
//
// THE LOAD. ResetAll (kPreLoadGame / revert) drops every op (the outgoing world's actors) and the
// main-thread Discard drops the completions already posted. A message already in the engine's queue is the
// engine's: it names a handle, and the engine resolves it or drops it.
//
// AVAILABILITY. Needs the verified Character vtable (the inline save path calls slot 0xE5) and the
// engine sender's row (VerifiedAddresses "CombatOps.TaskQueue.QueueStopCombat"). SENDER ROW REFUSED: the
// FLAGGED WORKAROUND, loud at Install and on every use: the StopCombat is made from Harbinger's main pump
// (How::kMainFallback, the 0.20.3 road), which RE-OPENS APMF-B26 (Docs/REVIEW-BACKLOG.md). No vtable or an
// unsupported runtime: StopCombat() refuses (false) and the callers refuse their StopCombat by name.
//
// THREADING. Everything here is MAIN thread, except Pending() (any thread). One leaf mutex guards the
// table (no engine call, no log, no other lock under it). NOTHING HERE RUNS ON A COMBAT JOB.
// ============================================================================

namespace RE {
    class Actor;
}

namespace apmf::combatops {

    enum class How : std::uint8_t {
        kNone,          // not run (the result of a refused request; never delivered)
        kEngineQueue,   // the engine ran its own message 0x2B from its task queue (or ended the fight first)
        kEngineAtPost,  // WARNING: the controller was gone as the engine's sender returned (its queue was off / full)
        kSaveSync,      // inline on main inside the save call (FlushForSave / SyncScope)
        kMainFallback,  // ERROR: the sender's row was refused; made on main from the pump (re-opens APMF-B26)
        kGone,          // cancelled on main before the engine was asked: the actor no longer had a controller
        kDropped,       // dropped at a load / revert (never delivered)
    };

    struct Result {
        RE::FormID  actor   = 0;
        bool        stopped = false;   // the controller the decision named is gone: Actor::StopCombat ran (the engine's
                                       // message, or inline on main), or the engine ended that fight first
        How         how     = How::kNone;
        const char* why     = "";      // a literal: why it was skipped (empty when stopped)
    };
    using Done = std::function<void(const Result&)>;

    // kDataLoaded, before the held engagement and the combat substrate install (both ask Available()). Main thread.
    void Install();
    bool        Available();
    const char* UnavailableWhy();   // never null

    // Ask the engine to run Actor::StopCombat for `a_actor` (see the header). MAIN thread. Coalesces with a pending
    // op for the same actor (one message, every completion runs). `a_done` runs on MAIN once the op is over: from
    // Poll (the engine ran it, or it was not run), or inline BEFORE this returns (inside a SyncScope, on the fallback
    // road, or when the engine ran the message at the request). `a_inline` (optional) is set true in that inline
    // case, false when it was posted or joined one. Returns false when not available, the actor is not a Character,
    // or no op is pending and the actor has no controller (nothing asked, `a_done` not called).
    bool StopCombat(RE::Actor* a_actor, const char* a_who, Done a_done = {}, bool* a_inline = nullptr);

    // Attach `a_done` to the op pending for this actor, without a new decision. False = none
    // is pending (nothing attached). MAIN thread. (Review F5 of 75e1f75: the held engagement's "already queued"
    // path keeps its caller's completion.)
    bool Attach(RE::FormID a_actor, Done a_done);

    // A StopCombat Harbinger decided for this actor is PENDING: from the decision until Poll saw the controller gone
    // (or answered "not run"). INVARIANTS #4a: while this is true nothing on main reads THROUGH this actor's combat
    // controller (its group, its target, its state, Actor::StartCombat, IsInCombat): the engine may be freeing it at
    // its task-queue drain right now. The controller POINTER may still be read. Any thread.
    bool Pending(RE::FormID a_actor);

    // Actor::IsInCombat (Character slot 0xE3 reads controller+0x43: AE 0x6B6DD0, SE 0x625660, 1.7.104 0x6C9850)
    // made #4a-safe for MAIN: while a stop is Pending, the answer is "it still has a controller" (the pointer only).
    bool InCombat(RE::Actor* a_actor);

    // How a result came about, for a log line.
    const char* HowText(How a_how);

    // MAIN, once per frame (Arbiter::OncePerFrame, right after mainthread::Pump): the DONE checks above and the
    // 60 s RULE C line. `a_delta`: the engine's own world-time frame step (0 while paused).
    void Poll(float a_delta);

    // kSaveGame, FIRST (plugin.cpp): every pending op answered now, completions inline: one whose controller is
    // still the decided one is made inline on main, one the engine already ran is reported as that. MAIN.
    void FlushForSave();

    // kPreLoadGame / revert: every pending op dropped, no engine call, no completion. MAIN.
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
