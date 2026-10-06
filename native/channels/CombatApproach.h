#pragma once
#include "APMF_API.h"

// ch.24 COMBAT APPROACH (kIntent_CombatApproach, ABI v19). See channels/CombatApproach.cpp.
namespace apmf::combatapproach {

    // kDataLoaded: the exact-version gate, [CombatApproach] bCombatApproach, the self-check
    // on every vtable it writes, then the seats (all write_vfunc, chaining). All or nothing.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // Game thread, Arbiter::OncePerFrame: resolves each claim's target, decides what the
    // seats apply, ends a claim that is over (Harbinger releases it), logs the per-actor line.
    void Poll();

    // Game thread, ~30 s: the RULE C counters, zeros included, while any claim exists.
    void Heartbeat();

    // SKSE kSaveGame (main thread, before the game writes the save): put the engine's own
    // area geometry back into every area the seats are carrying a bound in, so nothing of a
    // claim is written into the .ess. The next area update re-applies it.
    void RestoreBeforeSave();

    // kPreLoadGame / revert / new game: drop every entry (no engine write; the world is replaced).
    void ResetAll(const char* why);

    // APMF_API_v19::GetCombatApproachState. Any thread.
    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_CombatApproachInfo* out);

}
