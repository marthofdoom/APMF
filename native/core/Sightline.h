#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- HARBINGER'S OWN LINE OF SIGHT (ABI v18, ClickUp 86e3h6qj9).
//
// The contract (the ray, the verdicts, the timings) is APMF_API.h's "ABI v18: LINE
// OF SIGHT AND AWARENESS" section; this file is the service behind it. It is used by:
//   * the C-ABI GetLineOfSight (any thread)                       -> Read()
//   * ch.8b seats 0x06 / 0x07 for kCastFlag_OwnLineOfSight claims  -> Read()
//     (core/CastSeats.cpp, combat thread)
//   * ch.20's selector seat for kTargetPin_OwnLineOfSight claims   -> Read()
//     (channels/TargetPin.cpp, BSJobs combat worker)
//   * the awareness query's sight sense (main thread)              -> MeasureNow()
//
// WHY NOT Actor::HasLineOfSight. For an NPC viewer the engine's call (AE id 53829,
// 0x9BA120) casts no ray: it returns the byte at +0x14 of a per-(viewer, target)
// entry of the AIProcess LOS cache (0x68F8D0 -> 0x6EC990 -> 0x7AF3A0), refreshed on
// the engine's own schedule, and stale or false for targets the engine does not keep
// warm (allies, the player). Harbinger never calls it.
//
// THE TABLE. A fixed array of kSlots pair slots, each a SEQLOCK (one writer, many
// readers). The WRITER is the game's main thread only (Pump, MeasureNow, ResetAll).
// READERS are any thread and never block: they read a slot's key + packed verdict +
// stamp between two sequence loads and retry (twice) on a torn read. A reader that
// finds the pair marks it ASKED (a relaxed store of its own timestamp, outside the
// seqlock payload); a reader that does not find it drops the key into a small
// lock-free request ring that Pump drains. No mutex is taken on any reader path, so
// the combat-thread seats stay lock-free.
//
// THE PUMP. Arbiter::OncePerFrame (the confirmed main seat) calls Pump() once per
// frame: drain the request ring into slots, drop pairs nobody asked for in
// kLosInterestMs, and re-measure at most kLosMaxPairsPerFrame pairs whose newest
// measurement is at least kLosRefreshMs old, oldest first.
//
// LOCK SCOPE (the 2026-09-30 freeze hypothesis). Harbinger takes NO lock around a
// ray. bhkWorld::PickObject (vtable slot 0x33) takes the world's worldLock for READ
// itself, around the cast, on all three builds (AE 0xE86560 +0x115 -> LockForRead
// 0xCC90C0, released at +0x3C9 -> UnlockForRead 0xCC9380; SE 0xDA7580 -> 0xC072D0 /
// 0xC07590; 1.7.104 0x104BE80 -> 0xCE3250 / 0xCE3510), gated by a .data byte that is 1
// and never written. BSReadWriteLock is READER-PREFERRING on all three (LockForRead
// spins only while bit 31 -- a writer HOLDING the lock -- is set; LockForWrite is a
// bare CAS 0 -> 0x80000001 and sets no pending bit), so the older outer read guard
// (MFO Sightline, SpaceQuery) was a recursive read that cannot deadlock against a
// waiting writer -- and is also unnecessary. With no outer guard, Harbinger's lock
// scope is exactly the engine's own: one read lock per pick, held inside the engine
// function. The leaf mutex below (unavailability log throttle) is main-thread only and
// never held across an engine call.
//
// PICK SKIPPED. PickObject has an engine early-out (a frame-budget style test on
// three .data values) that returns without casting and sets bhkPickData::unkC0 (+0xC0)
// -- the ONLY writer of that byte in the function. A ray whose pick comes back with
// +0xC0 set was NOT cast, so it is never read as clear: the measurement is
// UNAVAILABLE (kLosWhy_PickSkipped) and is retried at the next refresh.
//
// RUNTIME. Exactly 1.6.1170, 1.5.97 and 1.7.104. Every engine address the ray uses is a
// verified row (spec.json): bhkWorld (slot 0x33 PickObject), Character (slots 0xC2
// GetEyeVector, 0x73/0x74 GetBoundMin/Max), Sightline.Actor.GetCollisionFilterInfo,
// Sightline.TESObjectCELL.GetbhkWorld, Sightline.bhkWorld.WorldScale (ripref through
// AIProcess::KnockExplosion). Install() refuses the whole service if any one fails.
// INI: Data/SKSE/Plugins/APMF.ini [Sightline] bOwnLineOfSight (default 1) -- 0 turns the
// service off: GetLineOfSight answers kLos_Unsupported, SenseActor kQuery_Unsupported,
// and the two own-line-of-sight bits are refused at the request.
// ============================================================================

namespace apmf::sightline {

    // kDataLoaded: arm the service on a verified runtime. Idempotent. Logs once.
    void Install();
    bool        Armed();
    const char* NotArmedReason();

    // One stored measurement. `verdict` is the STORED verdict (Visible / Occluded /
    // Unavailable, or Unknown = never measured); FreshVerdict() applies kLosFreshMs.
    struct Reading {
        std::uint32_t verdict = APMF_API::kLos_Unknown;
        std::uint32_t ageMs   = 0xFFFFFFFFu;   // since the measurement; 0xFFFFFFFF = never
        std::uint32_t occRun  = 0;             // consecutive OCCLUDED measurements
        std::uint32_t sample  = 0;             // the clear ray: 1 feet, 2 torso, 3 head
        std::uint32_t why     = 0;             // a LosWhy
    };

    // ANY THREAD, lock-free, never blocks, never casts. Reads the pair's newest stored
    // measurement; with `a_ask` it also marks the pair asked-for (or queues it when it is
    // not tracked yet), which is what keeps it measured. Not armed: verdict kLos_Unsupported
    // (a seat whose claim got an own-line-of-sight bit by Repoint then holds, and logs it).
    Reading Read(RE::FormID a_viewer, RE::FormID a_target, bool a_ask = true);

    // Unknown when never measured or older than kLosFreshMs; otherwise the stored verdict.
    std::uint32_t FreshVerdict(const Reading& a_r);

    // The C-ABI GetLineOfSight body (ClientAPI wraps it in a catch-all).
    std::uint32_t GetLineOfSight(RE::FormID a_viewer, RE::FormID a_target, APMF_API::APMF_LosInfo* a_out);

    // MAIN THREAD ONLY. Measure the pair now -- unless it was measured less than
    // kLosRefreshMs ago, in which case that measurement is returned and `a_reused` is set
    // -- store it, and return it. The caller has already checked the thread.
    Reading MeasureNow(RE::Actor* a_viewer, RE::Actor* a_target, bool& a_reused);

    // MAIN THREAD (Arbiter::OncePerFrame, after ControlMap::Drain). See the header.
    void Pump();

    // Game thread: kPreLoadGame / revert. Drops every tracked pair.
    void ResetAll(const char* a_why);

    // Counters the cast and pin seats feed for the heartbeat (relaxed atomics).
    void NoteSeatHold();     // a seat refused / paused for a non-VISIBLE verdict
    void NoteSeatPass();     // a seat answered with a VISIBLE verdict
    void NoteSeatStop();     // seat 0x07 stopped a channel on agreeing OCCLUDED readings

    // Human-readable names for the log.
    const char* VerdictName(std::uint32_t a_verdict);
    const char* WhyName(std::uint32_t a_why);
    const char* SampleName(std::uint32_t a_sample);

}
