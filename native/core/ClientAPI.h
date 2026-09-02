#pragma once

// ============================================================================
// APMF Layer 2 -- the CLIENT API (design.md Section 2 / Section 7). STUB SEAM ONLY.
//
// This is where a client mod (MFO, future mods) plugs in WITHOUT claiming a
// package or an alias. A client declares an intent for an actor on its own basis
// and releases when done; APMF arbitrates all outstanding client requests plus
// any intercepted unaware-source intent, then engages/denies the matching
// channels through the SAME arbiter the test-surface hotkeys drive today.
//
// It is intentionally NOT built yet: the open questions in design.md Section 7
// (raw package vs high-level intent vs raw movement; numeric basis vs policy
// callback; how a client request ranks against an unaware intercepted source and
// how the yield cadence is chosen) are unresolved and must not be frozen into an
// API prematurely. The declarations below fix the SHAPE and the seam; the bodies
// log "not implemented" and no-op. When built, Request() resolves an intent to a
// channel + params and calls that channel's engage on Arbiter's gated actor;
// Complete() calls the channel's release.
// ============================================================================

namespace apmf::client {

    using Handle = std::uint32_t;
    constexpr Handle kInvalid = 0;

    // High-level intents map 1:1 onto channel families (leaning: clients declare
    // goals, APMF owns the execution -- design.md Section 7). OPEN taxonomy.
    enum class Intent : std::uint32_t {
        kNone = 0,
        kMovementDeny,      // ch.1  suspend the actor's own planner (a client will drive/promote)
        kSetDisposition,    // ch.11 aggression/confidence/assistance/morality bias
        kHeadtrack,         // ch.5  look-at
        kSelectSpell,       // ch.8  own the cast selection
        kWeaponDrawn,       // ch.4  draw/sheathe
        kDialogueToggle,    // ch.10 allow/deny dialogue
        kGait,              // ch.1a speed
        kDetection,         // ch.16 detection AVs
    };

    // A client's override basis. TODO(design.md Section 7): a numeric priority is
    // the placeholder; the real resolution may be a policy callback APMF invokes.
    struct Basis {
        float priority = 0.0f;
    };

    // Declare an intent for an actor. Returns a handle to release later.
    // STUB: logs and returns kInvalid.
    Handle Request(RE::Actor* actor, Intent intent, Basis basis);

    // Release a prior request. STUB: logs, no-op.
    void Complete(Handle handle);

}
