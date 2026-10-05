#pragma once

namespace apmf::hook {
    // Patch the Character + PlayerCharacter vtables at Actor::Update index 0xAD
    // (once), routing every NPC tick through the arbiter. VR-refused. Idempotent.
    void Install();

    // APMF-B45 (F2b): why the 0xAD arbiter seat -- the ONLY thing that drains and
    // engages claims -- will never run, or nullptr while it is installed or not yet
    // attempted (Install runs at kDataLoaded). ControlMap refuses every new claim
    // (kInvalidHandle) while this is non-null, so a client's degrade path runs instead
    // of a handle that never drains. Any thread; the reason is a string literal.
    const char* RefusedReason();

    // True when the CALLING thread is the one the PlayerCharacter Update seat runs on
    // (the thread ControlMap::Drain and apmf::mainthread::Pump run on). False before
    // that seat has ticked once. The ABI v11 space queries refuse to run anywhere else
    // (APMF_API.h, "ABI v11: SPACE QUERIES"). Any thread; two relaxed reads.
    bool OnMainThread();
}
