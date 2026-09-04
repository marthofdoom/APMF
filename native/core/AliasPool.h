#pragma once

// ============================================================================
// APMF core -- the ALIAS-DRIVE POOL (Docs/SPEC-ALIAS-DRIVE.md). ch.9's 0x49
// hook (core/PackageGate.cpp) already redirects an actor's alias-tier package
// OFFER to whatever a live kIntent_OfferPackage claim names -- but the engine
// only ever ASKS 0x49 for an actor whose OWN ExtraAliasInstanceArray already
// has SOME alias membership. A vanilla follower on a bare PlayerFollowerPackage
// (no custom AI framework, no alias at all) is never asked, so ch.9 never gets
// a chance to answer for him -- field-proven on deck 2026-09-04.
//
// FIX (a direct port of MFO's field-proven loot-travel alias mechanism,
// native/Packages.cpp): APMF ships its OWN tiny ESL (APMF_GenerateESP.py ->
// APMF.esl) carrying ONE claim quest (APMF_ClaimQuest) with a 16-slot
// ACTOR-ALIAS POOL at a high static priority. On a ch.9 Engage,
// ForceRefTo-fill a free pool slot with the claimed actor -- this is ONLY to
// put him onto the engine's alias ladder (satisfies the ExtraAliasInstanceArray
// gate); the EXISTING 0x49 hook still supplies the actual package, unchanged.
// On Release, evict him by force-filling the session's eviction XMarker into
// his slot (a null clear is a no-op -- MFO ENGINE_NOTES #34/#69/#70; the
// alias must never be left claimed-with-nothing, which STANDS STILL the
// actor). This module owns ONLY the alias-fill/evict plumbing; it never
// changes what package PackageGate.cpp's 0x49 hook returns.
//
// THREADING. Every entry point here (ClaimSlot/ReleaseActor/ReleaseAll/
// EnsureEvictMarker/the co-save callbacks) runs on APMF's single MAIN/writer
// thread: ClaimSlot/ReleaseActor are called only from channels/OfferPackage
// .cpp's Engage/Release, which Channel.h's contract confines to the game
// thread (the ControlMap Drain/writer seat); ReleaseAll/EnsureEvictMarker are
// called only from plugin.cpp's SKSE messaging callbacks (kPreLoadGame/
// kPostLoadGame/kNewGame) and the co-save Save/Load/Revert callbacks -- all
// confirmed the same MAIN thread as the writer seat (INVARIANTS #12). So the
// slot-occupancy table needs no lock, mirroring core/AvLedger's ledger.
//
// AE-ONLY NATIVE, SE VM FALLBACK. ForceRefTo/CreateRefHandleByAliasID's
// verified reloc ids (25052/25066) are AE-only (ported verbatim from MFO's
// Packages.cpp, which cross-checked them against SKSE64 2.2.6's own source
// for this exact runtime + Address Library) -- off AE, ClaimSlot/ReleaseActor
// dispatch ReferenceAlias.ForceRefTo/.Clear through the VM instead (same
// fallback shape MFO's command-quest fill/release use). VR-REFUSED entirely
// (the reloc ids are SE/AE only, like every other version-pinned seat in this
// codebase) -- see ResolveForms.
// ============================================================================

namespace apmf::aliaspool {

    inline constexpr int kNumSlots = 16;

    // Resolve APMF.esl's forms (APMF_ClaimQuest, APMF_PlaceholderPackage) and
    // log the alias-pool's shape (mirrors MFO's Forms::Resolve). Call once
    // from kDataLoaded, any time after SKSE's data-loaded messaging fires. A
    // miss (plugin absent/disabled) disables ONLY the alias-drive path --
    // ClaimSlot then degrades to false, never a hard requirement elsewhere.
    // Logs (and does nothing else) on VR: the reloc ids below are SE/AE only.
    void ResolveForms();

    // Mint the session's eviction XMarker (vanilla base 0x3B) via
    // PlaceObjectAtMe, force-persisted so its handle survives cell unloads.
    // MAIN THREAD ONLY (PlaceObjectAtMe mutates the cell). Call once per
    // session from kPostLoadGame/kNewGame, BEFORE ReleaseAll so its evictions
    // displace with the marker rather than falling back to the player.
    // Idempotent (a no-op once already minted this session). Ported verbatim
    // from MFO's Packages::EnsureEvictMarker.
    void EnsureEvictMarker();

    // Put `actor` onto the engine's alias ladder: claim a free pool slot and
    // ForceRefTo-fill it with `actor` (native first, VM-dispatch fallback off
    // AE). Call from channels/OfferPackage.cpp's Engage (game thread only).
    // Idempotent if `actorID` already holds a slot. Returns false -- a
    // graceful degrade, never a crash -- on VR, a fill failure on BOTH
    // routes, or all 16 slots already claimed (16 concurrently-claimed
    // actors is generous headroom; see Docs/SPEC-ALIAS-DRIVE.md). On false,
    // PackageGate.cpp's 0x49 hook simply has nothing to override for this
    // actor (identical to today's pre-alias-drive behavior).
    bool ClaimSlot(RE::FormID actorID, RE::Actor* actor);

    // Evict `actorID` from whichever pool slot it currently occupies (the
    // in-process occupancy table is authoritative here -- ClaimSlot/
    // ReleaseActor are its only writers). Force-fills the eviction marker
    // into that slot (never a null clear -- a no-op, MFO ENGINE_NOTES #34)
    // so the actor detaches and the framework/vanilla package resumes. Call
    // from channels/OfferPackage.cpp's Release (game thread only). `actor`
    // may be null (deleted form) -- the slot is still freed by FormID, just
    // without the EvaluatePackage nudge. No-op if `actorID` holds no slot.
    void ReleaseActor(RE::FormID actorID, RE::Actor* actor);

    // Evict EVERY occupied slot to the eviction marker and clear the
    // in-process occupancy table. Scans by ALIAS OCCUPANCY (CreateRefHandle
    // ByAliasID on all 16 slots), not just the in-process table -- a save
    // written by an earlier session (or before this actor was released) can
    // hold a native alias fill this session's table never learned about,
    // exactly like MFO's own per-slot loot/retreat sweeps. Call on
    // kPreLoadGame, kPostLoadGame/kNewGame (post-load reconcile), and
    // OnRevert -- INVARIANTS #3-equivalent: a missed sweep leaves an actor
    // claimed-with-nothing-meaningful across a load, which STANDS STILL him
    // (Docs/SPEC-ALIAS-DRIVE.md §4). `why` is a short log tag.
    void ReleaseAll(const char* why);

    // --- SKSE serialization (main thread), wired from plugin.cpp. The
    // co-save record is a LIVE-SLOT diagnostic mirror, NOT the reset
    // mechanism: the alias fills persist NATIVELY in the .ess (they are
    // engine-serialized quest-alias state, same as MFO's loot/retreat/
    // command aliases), so ReleaseAll's unconditional per-alias sweep above
    // is the real reset regardless of what this record says. Save/Load/
    // ApplyPending/Revert mirror core/AvLedger's 4-callback shape exactly
    // (Docs/INVARIANTS.md #15's pattern) for consistency, even though
    // ApplyPending here only logs -- it applies no state, because there is
    // nothing to restore: eviction, not restoration, is correct here (an
    // occupied slot's fill was never something we want to put BACK). ---
    void Save(SKSE::SerializationInterface* intf);
    void Load(SKSE::SerializationInterface* intf, std::uint32_t version);
    void ApplyPending();   // kPostLoadGame: log the co-saved live-slot snapshot, then clear it
    void Revert();         // clear the in-process occupancy table + pending (no restore)

    // Co-save record tag + the CURRENT layout version.
    //   v1: count, then per entry {slot (uint32), actor FormID}   (8 bytes/entry)
    // A reader for EVERY shipped version is kept FOREVER (Docs/INVARIANTS.md
    // #15's discipline). Bump this + add a reader branch on ANY layout
    // change; never change a layout under an existing version number.
    inline constexpr std::uint32_t kRecordType = 'ADRV';
    inline constexpr std::uint32_t kRecordVersion = 1;

}
