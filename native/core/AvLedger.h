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
// -> {captured PRE-override value, the value WE applied}, and is CO-SAVED via SKSE
// serialization. On load the co-saved set is swept and each AV restored regardless
// of live engaged-state, then cleared. So an outstanding override is never stranded.
//
// CLOBBER GUARD: restore/apply only when the AV STILL equals the value we applied.
// If a quest or another mod changed the AV while our override was live, that newer
// value wins and we leave it (we only drop our stale record). Otherwise APMF would
// silently overwrite another author's write.
//
// ONE-CHANNEL-PER-AV ASSUMPTION: the ledger keys by (FormID, AV), so it assumes at
// most one APMF channel overrides a given AV on a given actor at a time (true today:
// disposition/gait/detection own disjoint AVs). If two channels ever shared an AV,
// the second Override would not re-capture the prior (idempotent), and the first
// Restore would drop the record out from under the second -- refcount per AV then.
//
// Game/main thread only (channel Engage/Release run on the game thread via the
// ControlMap drain; SKSE save/load/revert callbacks run on the main thread -- see
// the callback-thread quiescence note in INVARIANTS #12).
// ============================================================================

namespace apmf::av {

    // Set actor.av := value, capturing the pre-existing value ONCE into the co-saved
    // ledger (idempotent per (id, av)). Use this instead of SetActorValue for any
    // persisted AV a channel overrides. `id` is the actor's FormID (state key).
    void Override(RE::FormID id, RE::Actor* actor, RE::ActorValue av, float value);

    // Drop the ledger entry for (id, av); if `actor` is non-null AND the AV still
    // equals the value we applied, restore it to the captured prior first (else the
    // newer external value wins). `actor` may be null (deleted form) -> entry dropped
    // without an engine write (no leak).
    void Restore(RE::FormID id, RE::Actor* actor, RE::ActorValue av);

    // --- SKSE serialization (main thread), wired from plugin.cpp ---
    void Save(SKSE::SerializationInterface* intf);                            // write the live ledger
    void Load(SKSE::SerializationInterface* intf, std::uint32_t version);     // parse a record into pending
    void ApplyPending();                                                     // kPostLoadGame: restore + clear
    void Revert();                                                           // clear ledger + pending

    inline constexpr std::uint32_t kRecordType = 'AVOV';   // co-save record tag
    inline constexpr std::uint32_t kRecordVersion = 1;

}
