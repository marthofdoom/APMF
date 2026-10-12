#pragma once

// ============================================================================
// core/PackageSources -- HARBINGER 1a, the PACKAGE facet (ch.9 offer / ch.19 travel):
// capture + recognition of the package that runs instead of a claimed one, and the
// PASSIVE PROBE for the scene-package seat candidate.
//
// CAPTURE (no new seat). The plan's "RUNNING package read": on the main-thread pump,
// every actor holding a winning kIntent_OfferPackage claim (ch.19's internal offer
// included) has its running package compared with the claimed one. A different
// package is an outside input; its SOURCE is the plugin of the actor's current SCENE
// (BGSScene::GetFile(0), its parent quest named in the log), else the plugin that
// defines the package (TESPackage::GetFile(0)), else -- a package created at runtime
// (FF-range: Movement Blocked, combat spectator, dialogue) -- the Engine. A scene with
// no file is UNATTRIBUTED (allowed + alerted). The priority of that source on
// [Package] is compared with the claim's client's and logged; nothing is enforced.
//
// WHY NOTHING IS ENFORCED YET (principle 5). Marth's Q3 ruling is "hold the scene off
// while a claim outranks it". The seat candidate, from disassembly of both runtimes:
// Character vtable slot 0x4A, Actor::GetCurrentScene (AE 0x662150 / SE 0x5D19E0: reads
// the kHasSceneExtra bit at +0xE8 / +0xE0, tail-calls the extra-data lookup; this only,
// returns BGSScene*). Actor::EvaluatePackage (AE 37401 0x66CCF0 +0x1F0 / SE 36407)
// re-evaluates the package ONLY when it answers null, so a null answer for an outranked
// scene would let the 0x49 offer through. Disassembly proves the path EXISTS, not that it
// is the one holding FIWG's walk; so 1a ships the seat PROBE-ONLY: an observe-only
// chaining hook on slot 0x4A that, for an actor with a package claim, records every
// distinct caller (return RVA) that asked while a scene stood, and what the hold-off
// WOULD have decided. The field log decides whether the deny is built there.
//
// THREADING. Poll(): main thread (Arbiter::OncePerFrame), self-throttled 500 ms, one
// relaxed load while nothing is claimed. The 0x4A probe: any thread; it reads only the
// lock-free ControlMap snapshot, and takes a small mutex only for a claimed actor.
// ============================================================================

namespace apmf::packagesources {

    // kDataLoaded, after packagegate::Install. Exact 1.6.1170 / 1.5.97 / 1.7.104, VR
    // refused, the Character vtable self-checked, [Harbinger] bSceneProbe (default 1).
    void Install();

    // Main thread, once per frame. Capture poll + the probe's 60 s heartbeat.
    void Poll();

    // The world is being replaced (kPreLoadGame / revert): forget the per-actor memory.
    void ResetAll();

}
