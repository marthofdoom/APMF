#pragma once

// ============================================================================
// APMF core -- the AV OVERRIDE LEDGER (co-saved). The AV-based channels
// (Attribute/Speed/Detection) mutate dynamic ActorValues that PERSIST in the .ess.
// A RAM-only "prior value" would be STRANDED if the player saves while engaged and
// reloads: after a load the live control map is empty, so a plain Release-on-load
// restores nothing and the AV mutation is permanent (MFO's "fix-forward never
// cleans old saves" class). INVARIANTS #15.
//
// FIX: every AV override goes through this ledger, which records (actor FormID, AV)
// -> the captured PRE-override value, and is CO-SAVED via SKSE serialization. On
// load the co-saved set is swept and each AV restored regardless of live
// engaged-state, then cleared. So an outstanding override is never stranded.
//
// Game/main thread only (channel Engage/Release run on the game thread via the
// ControlMap drain; SKSE save/load/revert callbacks run on the main thread).
// ============================================================================

namespace apmf::av {

    // Set actor.av := value, capturing the pre-existing value ONCE into the co-saved
    // ledger (idempotent per (actor, av)). Use this instead of SetActorValue for any
    // persisted AV a channel overrides.
    void Override(RE::Actor* actor, RE::ActorValue av, float value);

    // Restore actor.av to its captured value and drop the ledger entry. `actor` may
    // be null (unloaded/deleted) -> no engine write, entry left for the load sweep.
    void Restore(RE::Actor* actor, RE::ActorValue av);

    // --- SKSE serialization (main thread), wired from plugin.cpp ---
    void Save(SKSE::SerializationInterface* intf);   // write the live ledger
    void Load(SKSE::SerializationInterface* intf);   // parse a co-saved record into pending
    void ApplyPending();                             // kPostLoadGame: restore + clear pending
    void Revert();                                   // clear ledger + pending (revert/new game)

    inline constexpr std::uint32_t kRecordType = 'AVOV';   // co-save record tag
    inline constexpr std::uint32_t kRecordVersion = 1;

}
