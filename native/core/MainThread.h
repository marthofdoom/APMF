#pragma once

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

}
