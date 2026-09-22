#pragma once

// ============================================================================
// Channel 19 -- TRAVEL (kIntent_Travel, ABI v10). See channels/Travel.cpp for the
// design; Docs/CHANNEL-MAP.md row 19, Docs/DENY-COMPLETENESS-AUDIT.md row 19,
// Docs/INTEGRATION.md's v10 section.
//
// Everything declared here is GAME-THREAD ONLY unless a comment says otherwise.
// ============================================================================

namespace apmf::travel {

    // Read the INI switches and resolve APMF.esl's travel packages. Call at
    // kDataLoaded, after the data handler exists. Idempotent. VR-refused.
    void Install();

    // Is ch.19 able to do anything at all? False when VR, when [Travel] bTravel=0,
    // or when APMF.esl's packages did not resolve. A false answer makes
    // ControlMap::EnqueueRequest refuse a kIntent_Travel claim synchronously
    // (kInvalidHandle) instead of accepting one that would silently do nothing --
    // the same "seat down = claim refused" contract ch.17 already uses. Any thread
    // (one relaxed atomic load).
    bool Installed();

    // Why Installed() is false, for the one-time refusal log. Never null.
    const char* NotInstalledReason();

    // Per-frame leg monitor, from Arbiter::OncePerFrame (the confirmed-main
    // PlayerCharacter 0xAD seat). Self-throttled internally; costs one relaxed
    // atomic load when nothing is travelling. NOT a re-assert -- it only decides
    // when a leg has ENDED (arrived / the actor is in combat / the destination is
    // gone / stuck).
    void Poll();

    // Drop every travel leg and free every package slot WITHOUT releasing the
    // internal ch.9 offer (the world is being replaced). For the kPreLoadGame /
    // revert boundary, beside castproxy::ResetAll(): ControlMap::Clear() makes no
    // channel->Release calls at all, so without this the slot table would stay
    // owned by actors that no longer exist and every later leg would overflow.
    // GAME THREAD.
    void ResetAll(const char* why);

}
