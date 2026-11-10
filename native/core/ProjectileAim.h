#pragma once
#include "APMF_API.h"

#include <cstdint>

// ============================================================================
// core/ProjectileAim -- ch.29 PROJECTILE AIM (kIntent_ProjectileAim, ABI v36, card H5, marth 2026-11-10:
// "Ranger at most 3 misses in 10, Jaqspur at most 1, never-miss is acceptable"). See ProjectileAim.cpp's
// header for the engine path, the evidence and every verified byte.
//
// THE SEAT. TESObjectWEAP::Fire (AE 18102 / SE 17693 / 1.7.104 raw 0x28C450) builds a Projectile::LaunchData
// on its stack and calls Projectile::Launch ONCE (AE +0xE60 / SE +0xE82 / 1.7.104 +0xE70). Harbinger rewrites
// that one call. For a non-player shooter whose WINNING ch.29 claim's budget says the shot must land, the
// LaunchData's angleZ / angleX are recomputed from the shooter's own CombatProjectileAimController (its raw
// lead point and launch speed) and the projectile's gravity, exactly the engine's own ballistic solve WITHOUT
// the two deliberate scatters (the controller's random offset, the Archery cone Fire just added). Then the
// engine's Launch runs as it always does. Every other shot, shooter and caller is untouched.
//
// THREADING. The thunk runs on the thread that runs Fire (the shooter's animation event, the game thread).
// It reads the ControlMap RCU snapshot, the engine's own data in place (bounded) and takes one LEAF mutex
// for the per-actor window and counters (no engine call under it). The TESHitEvent sink runs on the event
// dispatch thread and takes the same leaf mutex (atomics and FormIDs only). Install / Heartbeat: MAIN thread.
// GetState / ResetAll / RefuseReason: any thread.
// ============================================================================

namespace apmf::projectileaim {

    // kDataLoaded, MAIN thread. [ProjectileAim] bProjectileAim (default 1). Exact 1.6.1170 / 1.5.97 /
    // 1.7.104, VR refused, SeatVerified rows, the site's bytes re-read before the write. Refusal = logged,
    // nothing written, every ch.29 claim refused.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // ControlMap::EnqueueRequest: why a kIntent_ProjectileAim request must be refused (nullptr = accept).
    const char* RefuseReason(RE::FormID actor);

    void ResetAll(const char* why);     // revert / kPreLoadGame: windows, pending shots, counters
    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_ProjectileAimInfo* out);

    // channels/ProjectileAim.cpp Engage, writer thread: true while the site still calls Harbinger's thunk.
    bool SiteIntact();

    // Arbiter::OncePerFrame, MAIN thread. `a_delta` = the frame step the engine handed PlayerCharacter::Update
    // (world time x the killcam multiplier, see Arbiter.h): summed while the game is not paused into the clock the
    // shot window runs on. Resolves shots whose flight window elapsed (a MISS), logs them when the probe is on, and
    // the ~60 s totals line while the seat is installed and a claim was ever filed.
    void Heartbeat(float a_delta);

}
