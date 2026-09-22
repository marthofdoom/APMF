#pragma once

// ============================================================================
// Channel 19 -- COMBAT ENGAGE (kIntent_CombatEngage, ABI v10). See
// channels/CombatEngage.cpp for the design; Docs/CHANNEL-MAP.md row 19,
// Docs/DENY-COMPLETENESS-AUDIT.md row 19, Docs/INTEGRATION.md's v10 section.
//
// Everything declared here is GAME-THREAD ONLY unless a comment says otherwise.
// ============================================================================

namespace apmf::combatengage {

    // Read the INI switches and resolve APMF.esl's approach packages. Call at
    // kDataLoaded, after the data handler exists. Idempotent. VR-refused.
    void Install();

    // Is ch.19 able to do anything at all? False when VR, when
    // [CombatEngage] bCombatEngage=0, or when APMF.esl's packages did not resolve.
    // A false answer makes ControlMap::EnqueueRequest refuse a kIntent_CombatEngage
    // claim synchronously (kInvalidHandle) instead of accepting one that would
    // silently do nothing -- the same "seat down = claim refused" contract ch.17
    // already uses. Any thread (one relaxed atomic load).
    bool Installed();

    // Why Installed() is false, for the one-time refusal log. Never null.
    const char* NotInstalledReason();

    // Per-frame approach monitor, from Arbiter::OncePerFrame (the confirmed-main
    // PlayerCharacter 0xAD seat). Self-throttled internally; costs one relaxed
    // atomic load when nothing is engaged. NOT a re-assert -- it only decides when
    // an approach has ENDED (arrived / perceived / target gone / stuck).
    void Poll();

    // Drop every engagement and free every package slot WITHOUT releasing
    // sub-claims (the world is being replaced). For the kPreLoadGame / revert
    // boundary, beside castproxy::ResetAll(): ControlMap::Clear() makes no
    // channel->Release calls at all, so without this the slot table would stay
    // owned by actors that no longer exist and every later engagement would
    // overflow. GAME THREAD.
    void ResetAll(const char* why);

}
