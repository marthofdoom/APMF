#pragma once

// ============================================================================
// RESTORE-CASTER CENSUS (phase 0b of the animated-heal design, 2026-09-29,
// fix/apmf-cast-instant-caster). PASSIVE: reads and log lines only, no hook of
// its own, no write to the actor or the engine. `[Probe] bRestoreCensus` (code
// default 1 while field testing; STATUS pre-release checklist resets it to 0).
//
// WHY (principle 5, ENGINE_NOTES §0.41). The engine builds a
// CombatMagicCasterRestore, and so runs the Restore seats a cast claim answers
// (core/CastSeats.cpp 0x06/0x07/0x0A/0x0D), only when its OWN behaviour logic
// decides to heal. A restore-shaped kIntent_Cast claim can therefore stand with
// nothing to answer (MFO field 2026-09-08: a self heal waited 16.7 s, an ally heal
// 0.6 s). This census counts, per actor and per claim, what the engine actually
// did while the claim stood, INCLUDING THE ZERO CASE.
//
// WHAT IT OBSERVES, per window = (actor, claimed hand, driven form) for a live,
// driving (not deny-only), restore-shaped kIntent_Cast claim:
//   * OPEN: the claim's spell/proxy/target, self vs ally, concentration, whether
//     the actor has a CombatController.
//   * RESTORE SEAT USE (combat thread, from core/CastSeats.cpp on the Restore
//     caster vtable ONLY): seat 0x06 CheckStartCast (and whether the claim or the
//     native test answered), seat 0x0A GetMagicTarget; distinct Restore caster
//     INSTANCES seen for the driven form; Restore activity for OTHER items on the
//     same actor (the engine deciding to heal with something else). Construction
//     itself is not hooked: "caster seen" means first seen at a seat.
//   * HAND ENGAGEMENT (main thread poll): the claimed hand's MagicCaster holding
//     the driven form in a non-None state (charge/ready/cast), and the highest
//     state reached.
//   * ZERO: milestone lines at 2/5/10/20/40 s while no Restore seat has seen the
//     driven form, and a CLOSE summary with a verdict.
// Throttled: one global line budget (30 lines per 10 s, the overflow count is
// reported), plus a 60 s heartbeat that prints even when nothing happened (a
// dead census is never mistaken for "no claims").
// ============================================================================

namespace apmf::restorecensus {

    // Cheap pre-gate for the combat-thread seats: armed AND at least one window open.
    bool Active();

    // COMBAT THREAD (core/CastSeats.cpp), Restore caster vtable only. `a_seat` is the
    // vfunc slot (0x06 or 0x0A). `a_claimAnswered` = the ch.8b claim answered this
    // call; `a_answer` = the value returned (0x06 only; ignored for 0x0A). Takes one
    // leaf lock, never calls into the engine.
    void NoteRestoreSeat(RE::FormID a_actor, const void* a_caster, RE::FormID a_item, std::uint32_t a_seat,
                         bool a_claimAnswered, bool a_answer);

    // GAME THREAD (Arbiter::OncePerFrame). Self-throttled (~200 ms). Opens and closes
    // windows from the published cast claims, reads the claimed hand's caster state,
    // prints milestones, summaries and the heartbeat. Reads the INI on first call.
    void Poll();

}
