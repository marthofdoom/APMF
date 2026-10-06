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

// ---------------------------------------------------------------------------
// INTERRUPT ATTRIBUTION + CHANNEL END (fix/apmf-hand-claim-blocks-equip, 2026-10-06;
// _research/field-1006-heal-diagnosis.md "Logging that would settle what is still
// open", items 1 and 2). Same rules: always on, rate-limited, read and log only.
//
//   3. `[castobs] INTERRUPT-ATTRIB`: an `InterruptCast` anim event on an actor a live
//      kIntent_Cast claim stands on (a WATCHED actor, refreshed by the 100 ms poll from
//      the control map, at most kMaxWatched). Printed on the game thread at the next
//      frame: each hand's claim, each hand's MagicCaster state and spell (and the
//      instant caster), whether a CombatController exists, the behaviour-tree leaves
//      the actor entered in the last second (the AI's current ACTION), the
//      CombatMagicCasters the engine asked CheckStartCast on / fired in the last second
//      (that action's MAGIC CASTER), CheckCast refusals in the last 500 ms (engine or
//      APMF, with the reason), and every governed equip on the actor in the last 100 ms
//      with its path and the equip seat's verdict.
//   4. `[castobs] CHANNEL-END`: for a claim whose driven form is a CONCENTRATION cast,
//      the claimed hand's MagicCaster is read EVERY FRAME while the claim stands; when
//      it returns to state 0 after the channel ran (state >= 4 holding the driven
//      form), one line gives the state transitions with timestamps and the reasons that
//      are observable: seat 0x07's STOP decision and its why (claim TTL, Ward floor,
//      target gone/dead, own line of sight, stop percent), CheckCast refusals on that
//      hand (the engine re-runs CheckCast every channel tick and interrupts on NO --
//      INVARIANTS #18), the cast anim tags, whether the claim still stood, the actor's
//      magicka and Harbinger's own stored line-of-sight reading. The engine's own
//      concentration aim/LoS re-check is NOT seated anywhere in Harbinger, so the line
//      says so ("engine LoS re-check: not seated") instead of guessing.
//
// The recorders below are fed from the seats that already see each fact (the equip
// sink, ch.7's leaf act(), the caster-type census, the CheckCast gate, seat 0x07).
// ANY THREAD, a leaf lock only, no engine call, no form lookup, no log; each is a
// no-op unless the actor is WATCHED.
// ---------------------------------------------------------------------------
namespace apmf::castobserve {

    // Call once per frame from Arbiter::OncePerFrame (game thread). The state poll
    // self-throttles (~100ms); the interrupt drain and the concentration-channel
    // tracker run every frame and cost one relaxed load when idle. Reads MagicCaster
    // state for loaded high-process actors, logs transitions, and registers the
    // passive anim-event sink on newly-seen casting actors. Never mutates anything.
    void Poll();

    // Lock-free: is any actor watched / is this one?
    bool AnyWatched();
    bool Watching(RE::FormID a_actor);

    // `a_path` and `a_verdict` must be string literals (stored as pointers).
    void NoteEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, const char* a_verdict);
    void NoteLeafAct(RE::FormID a_actor, std::uintptr_t a_leafVtable);
    // `a_type` must be a string literal (the census' caster type name).
    void NoteCaster(RE::FormID a_actor, const char* a_type, RE::FormID a_item, bool a_fired, bool a_answer);
    // `a_source`: MagicSystem::CastingSource (0 L, 1 R, 2 voice, 3 instant); `a_reason`:
    // MagicSystem::CannotCastReason; `a_byApmf`: Harbinger's gate refused (else the engine).
    void NoteCheckCast(RE::FormID a_actor, int a_source, RE::FormID a_spell, std::uint32_t a_reason, bool a_byApmf);
    // `a_why` must be a string literal (seat 0x07's stop reason).
    void NoteStopCast(RE::FormID a_actor, const char* a_why);

}
