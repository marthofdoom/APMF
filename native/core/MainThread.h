#pragma once

#include <cstddef>
#include <functional>

// ============================================================================
// APMF core -- the CONFIRMED-MAIN-THREAD task pump for the cast-EXECUTE drive
// (feat/cast-act, ch.8 SelectSpell +ACT). Skyrim's `Actor::Update` (0xAD) is
// FIELD-PROVEN multi-thread for Character actors (INVARIANTS #4/#12,
// [threadcheck], core/Hook.cpp) -- but the PLAYERCHARACTER's own 0xAD seat is
// confirmed single/main-thread (the SAME seat `ControlMap::Drain` already runs
// from, core/Hook.cpp's `PlayerUpdateHook`). This module reuses that ALREADY
// PROVEN seat rather than trusting SKSE's own `TaskInterface::AddTask` --
// MFO's own hard-won lesson (its `MainThread::Post` exists because AddTask does
// NOT reliably land on a 3D/equip-safe main thread: "SKSE AddTask runs on a job
// worker; MainThread::Post is the ONLY road to main"). APMF does not need a new
// hook to get the same guarantee -- it already has one.
//
// Any thread may Post(); Pump() executes every queued task ONCE, in FIFO order,
// and must be called ONLY from the confirmed-main seat (`Arbiter::OncePerFrame`,
// right after `ControlMap::Drain()`). A task that needs another frame re-Posts
// itself.
//
// ITS ONE LOAD-BEARING USE TODAY (the forced cast drive that used to re-Post its
// phase chain here is retired): ch.8b's claim Release posts the delivery-flip
// proxy teardown through this queue so the un-teach happens STRICTLY AFTER the
// cleared claim has been published to the combat-thread cast seats. Release runs
// inside `ControlMap::Drain`, before `Publish()`; `Pump()` runs right after
// `Drain()` returns, so this is the cheapest correct place to put "one hop later,
// same thread" (Docs/INVARIANTS.md #20).
// ============================================================================

namespace apmf::mainthread {

    // Enqueue `fn` to run on the next Pump() call. Any thread; a brief
    // mutex-guarded push -- never blocks on engine state.
    void Post(std::function<void()> fn);

    // Drain and run every task queued so far, in FIFO order. MAIN-THREAD-ONLY
    // (called from Arbiter::OncePerFrame, never from OnActorUpdate). A task
    // Post()'d DURING this Pump() call runs on the NEXT Pump() -- this drains a
    // local swap of the queue, never the live one, so it cannot loop forever on
    // a self-reposting chain and cannot re-enter mid-drain.
    void Pump();

    // Drop every queued task WITHOUT running it; returns how many were dropped.
    // Same seat rule as Pump() (called from the kPreLoadGame / revert handlers in
    // plugin.cpp, which run on the main thread).
    //
    // WHY IT EXISTS (2026-09-06 review of the ch.9 nudge deferral). Posted tasks
    // otherwise CROSS THE LOAD BOUNDARY. `ControlMap::ReleaseAll` on kPreLoadGame
    // calls every channel's Release, and a channel that defers work through Post()
    // has nothing Pump() it until the FIRST PLAYER UPDATE AFTER THE LOAD -- by which
    // time the world has been swapped. A persistent NPC's reference survives that
    // swap, so the task's handle still resolves and its re-validation still passes;
    // it then acts on the NEW world's actor, un-asked.
    //
    // For what is queued today that is HARMLESS, not dangerous -- and "harmless" is
    // the accurate word: ch.9's nudge serializes nothing, touches no alias fill, and
    // keeps `EvaluatePackage`'s resetAI=false; ch.8b's proxy teardown post is moot
    // because `castproxy::ResetAll()` runs right after it anyway. (The original fix
    // commit called the deferral "strictly safer"; review corrected that to
    // "harmless", and this comment records the corrected claim, not the first one.)
    // It is still an UNREQUESTED engine write on the far side of a world swap, and
    // the hole is GENERAL: every future teardown-path Post() inherits it. So the
    // teardown paths flush the queue explicitly instead of relying on what happens
    // to be in it.
    std::size_t Discard();

}
