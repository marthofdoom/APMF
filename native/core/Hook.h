#pragma once

namespace apmf::hook {
    // Patch the Character + PlayerCharacter vtables at Actor::Update index 0xAD
    // (once), routing every NPC tick through the arbiter. VR-refused. Idempotent.
    void Install();
}
