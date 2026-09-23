#pragma once

namespace apmf::hook {
    // Patch the Character + PlayerCharacter vtables at Actor::Update index 0xAD
    // (once), routing every NPC tick through the arbiter. VR-refused. Idempotent.
    void Install();

    // True when the CALLING thread is the one the PlayerCharacter Update seat runs on
    // (the thread ControlMap::Drain and apmf::mainthread::Pump run on). False before
    // that seat has ticked once. The ABI v11 space queries refuse to run anywhere else
    // (APMF_API.h, "ABI v11: SPACE QUERIES"). Any thread; two relaxed reads.
    bool OnMainThread();
}
