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

    // ---- HARBINGER-INTERNAL BOUND (2026-10-12) -------------------------------------------------
    // The same "within R of X" the seats carry for a ch.24 claim, set by Harbinger itself for a
    // mechanism that IS a move-to: a held engagement driving to a bleeding-out target
    // (channels/CombatEntry.cpp) and ch.23's leash return (core/ActionGate.cpp). Not a client
    // claim and not arbitrated as one: the seats carry it only while the actor has NO live ch.24
    // claim (the client's declared area always wins), and Harbinger 1b ranks `owner` (the claim the
    // bound implements) against the area's source exactly as for a claim. One per actor; a second
    // Set replaces it. `space` = SpaceOfRef(the reference X was read from). GAME THREAD.
    // Returns false (nothing set) when the seats are not installed or the input is unusable.
    bool SetInternalBound(RE::FormID actor, const RE::NiPoint3& x, const void* space, float radius,
                          APMF_API::Handle owner, const char* who);
    // Drop it; the engine's own area answers again from its next update (put back + re-activated
    // like a finished claim's). GAME THREAD.
    void ClearInternalBound(RE::FormID actor);
    struct InternalInfo {
        std::uint32_t applied      = 0;      // times the area carried it
        std::uint32_t stood        = 0;      // times 1b let the area's source stand
        std::uint64_t msSinceApply = ~0ull;  // WORLD ms since the area last carried it (~0 = never)
        bool          clientWins   = false;  // a live ch.24 claim is carried instead
    };
    bool GetInternalInfo(RE::FormID actor, InternalInfo& out);   // GAME THREAD; false = none set
    // The engine's own "space" of a reference (AE 0x2EA5E0 GetWorldLocation's rule): an interior cell,
    // else the worldspace (or the cell). nullptr = no parent cell. Any thread (reads pointers only).
    const void* SpaceOfRef(const RE::TESObjectREFR* ref);

}
