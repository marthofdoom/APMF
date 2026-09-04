#pragma once

// ============================================================================
// OBSERVE-AND-REPLICATE cast-path probe (marth 2026-09-04). MFO must drive a
// non-alias follower through a full ANIMATED spell cast; rather than GUESS the
// trigger (the shelved TESActionData::Process attack/release guess), APMF's global
// 0xAD receiver WATCHES the engine drive a normal NPC through a real animated cast
// and logs the EXACT sequence, so MFO can replicate the proven path.
//
// FULLY PASSIVE (marth's hard rule): NO hotkeys, NO toggles. Always-on,
// per-actor rate-limited, chain-to-original / never-mutate. Two read sources,
// both correlated to the actor + spell + a shared monotonic tick (core/Clock.h)
// so the lines interleave into one readable timeline:
//
//   1. MagicCaster STATE MACHINE (poll, game thread). Every frame from
//      Arbiter::OncePerFrame (self-throttled ~100ms), scans the loaded high-process
//      actors and reads each hand's MagicCaster (left/right/instant): currentSpell +
//      cast state (kNone->charge->release). Logs ONLY on a transition (state or spell
//      change) -- pure reads via CommonLib accessors, no hook, cannot crash.
//
//   2. Animation-graph CAST EVENTS (per-actor event sink). When the poll first sees
//      an actor with an active caster, APMF registers a passive
//      BSTEventSink<BSAnimationGraphEvent> on it (a non-behavior-altering observe
//      listener, the standard version-robust API -- NOT a vtable-index guess). The
//      sink logs the real cast-relevant anim event STRINGS in sequence (the
//      MLh_/MRh_ SpellReady/Aim/Fire/Release-style tags the engine actually fires),
//      returns kContinue (never consumes), never mutates. Deduped per (actor,tag) so
//      repeats don't spam while the sequence is preserved.
//
// GOAL: a timeline that reveals the precise MagicCaster-state + anim-event sequence
// of a real NPC cast, so MFO can drive the same sequence (SPEC-FORCED-CAST.md).
// ============================================================================

namespace apmf::castobserve {

    // Call once per frame from Arbiter::OncePerFrame (game thread). Self-throttles
    // internally (~100ms); near-zero the rest of the time. Reads MagicCaster state
    // for loaded high-process actors, logs transitions, and registers the passive
    // anim-event sink on newly-seen casting actors. Never mutates anything.
    void Poll();

}
