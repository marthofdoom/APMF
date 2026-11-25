#pragma once

#include "APMF_API.h"
#include "core/BehaviorTreeCatalog.h"

#include <cstdint>
#include <string>

// ============================================================================
// [Probe] bMoveProbe (default 0): THE PASSIVE MOVEMENT PROBES P1, P2, P3 of the strict
// movement plan (MFO Docs/research/2026-11/tight-movement-plan.md section 9). They answer,
// per actor with a live ch.24 claim:
//   P1 which leaf / attack / hit reaction moved him toward or away from the claim's point;
//   P2 why a Return To Combat Area path ends with status 5 (the area switch-off);
//   P3 which aim controller holds his aim, and whether an attack leaf is running.
// Nothing here denies, writes engine state or changes a return value. P1 and P3 ride seats
// that exist (ch.7 act/pop, ch.24 S1 and Poll); P2 is one chaining observe-only vfunc seat,
// installed only while the key is 1. Everything answers at once while nothing is watched.
// ============================================================================
namespace apmf::moveprobe {

    // kDataLoaded, after actiongate::Install() and before any claim can be served.
    void Install();

    // True when the key is 1 and the runtime is supported. One relaxed load.
    bool On();
    // ...and at least one ch.24 claim is being watched. One relaxed load.
    bool Watching();

    // ch.7 act() / pop() thunks (combat threads), before the node's own function. Observe only. `a_denied`
    // later in the thunk is not known here: an act counts as ENTERED, a force-failed one included.
    void NoteAct(std::uintptr_t a_vt, RE::FormID a_actor);
    void NotePop(std::uintptr_t a_vt, RE::FormID a_actor);

    // ch.24 S1 (combat thread, inside the controller's own update): the controller's current aim controller.
    void NoteAim(RE::FormID a_actor, RE::CombatController* a_cc);

    // ch.24 Poll (GAME THREAD, no ch.24 lock held): one sample of a live claim. `a_why` ends nothing here.
    void Sample(RE::FormID a_id, RE::Actor* a_actor, APMF_API::Handle a_claim, const RE::NiPoint3& a_point);
    // The claim ended (GAME THREAD, no ch.24 lock held): the P1 + P2 summary, and the record is dropped.
    void Finish(RE::FormID a_id, const char* a_why);
    // Revert / load: records dropped without a summary.
    void Reset(const char* a_why);

    // P3: " P3 ..." for the gait line, empty while the probe is off. Any thread.
    std::string AimText(RE::FormID a_id);

}
