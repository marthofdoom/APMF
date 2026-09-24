#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- POSITION CAST (ABI v11, kCastFlag_AtPosition).
//
// A client's kIntent_Cast RequestEx can name a WORLD POINT instead of an actor.
// APMF places a non-persistent XMarker (Skyrim.esm 0x3B) at the point and casts
// the spell FROM that marker, blamed on the client's actor, with the exact call
// sequence the engine's own Papyrus Spell.RemoteCast uses:
//     marker->GetMagicCaster(kInstant)->InterruptCast(false);
//     caster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, actor);
// then deletes the marker one main-thread hop later (Disable + SetDelete(true)).
//
// WHY THE MARKER IS THE CASTER (disassembly, both runtimes; the full table is
// Docs/ADDRESS-TABLE-2026-09-15.md, ADDENDUM 2026-09-23). The engine's cast core
// (AE 0x5bc160 / SE 0x54cd10) places a Target Location spell at the CASTER's own
// magic node, or its position when it has none. It never reads the target it was
// handed for that delivery. An actor "casting at a marker" therefore lands at the
// actor's hand. A marker's NonActorMagicCaster has no magic node and reports no
// out-actor, so the location is the marker itself and no navmesh snap moves it.
//
// WHAT IS REFUSED, BY NAME: summons (the engine applies a summon effect only to the
// actor that cast it, so a marker can never summon), every delivery other than
// Target Location, concentration and constant-effect spells, the three spell types
// Papyrus RemoteCast refuses (disease, ability, addiction), any other cast flag set
// alongside kCastFlag_AtPosition, an unloaded or dead actor, a cell that is not
// attached. Every refusal is one WARN line naming the reason. Nothing is retried.
//
// DOCTRINE (Docs/INVARIANTS.md #0 action (e), PROPOSED, pending marth -- so the cast
// ships OFF: [PositionCast] bPositionCast defaults to 0). This would be the
// one place APMF itself makes a cast call. It is legal because no decision seat
// exists to compose instead (an NPC's AI has no seat that aims a location spell at
// a point, and seat 0x0A carries an Actor*), because the client declared both the
// spell and the point, and because it is a single one-shot call on the main thread
// with no re-assert. It is NOT a claim: it never enters the control map, so no seat
// can ever read it as an actor target, and it holds no facet.
//
// MARKER LIFETIME. Each marker lives from its cast to the next main-thread pump
// (one frame). It is tracked by its ObjectRefHandle (the handle carries age bits,
// so a recycled 0xFF FormID can never be mistaken for it) and re-checked before
// deletion: same FormID, base still the XMarker. At most kMaxLiveMarkers exist at
// once; a request past that is refused. Revert and kPreLoadGame forget the table
// without touching any reference (the old world is going away). The worst-case
// save footprint: a save taken inside that one frame captures at most one marker
// per position cast issued in it (never more than kMaxLiveMarkers). Since the
// co-saved ledger below, the load of such a save DELETES them (see the ledger).
//
// THREADING. Enqueue() is safe from any thread (it copies POD and posts). Every
// other function is MAIN-THREAD ONLY (apmf::mainthread::Pump, or the SKSE
// revert / kPreLoadGame handlers, which run on the main thread).
// ============================================================================

namespace apmf::poscast {

    // At most this many markers exist at once (see MARKER LIFETIME above).
    inline constexpr std::size_t kMaxLiveMarkers = 16;

    // kDataLoaded: runtime gate (exactly 1.6.1170 or 1.5.97, never VR), the INI
    // switch ([PositionCast] bPositionCast, default 1), and the XMarker base form.
    // Logs the outcome once. Idempotent.
    void Install();

    // True once Install() succeeded. Any thread.
    bool Installed();

    // Why Install() refused ("" when installed). Any thread.
    const char* NotInstalledReason();

    // ANY THREAD. Validate what can be validated from POD alone and post the
    // delivery to the main thread. Returns false (and logs the reason) when the
    // request is refused synchronously; the caller then returns kInvalidHandle.
    // `handle` is only a label for the log lines.
    bool Enqueue(APMF_API::Handle handle, RE::FormID actor, const APMF_API::APMF_Param& param);

    // ── Shared XMarker helpers (ABI v11): the position cast above, and ch.19's
    // position legs (channels/Travel.cpp), which point a travel package at one. ──

    // True once Install() found a verified runtime (exactly 1.6.1170 or 1.5.97, never
    // VR) and the Skyrim.esm XMarker base. INDEPENDENT of [PositionCast]
    // bPositionCast: that switch turns off the cast, not the markers. Any thread.
    bool MarkersSupported();

    // MAIN THREAD. Place a non-persistent XMarker at `a_point` in `a_actor`'s cell and
    // worldspace (TESDataHandler::CreateReferenceAtLocation, forcePersist = false) and
    // prove its cell is attached. Returns the marker's handle, or an empty handle with
    // the reason in `a_why` (a marker that was created but failed the cell check has
    // already been deleted). The CALLER owns the marker from here: it must call
    // DeleteMarker exactly once, by this handle.
    RE::ObjectRefHandle PlaceMarker(RE::Actor* a_actor, const RE::NiPoint3& a_point, std::string& a_why);

    // MAIN THREAD. Disable() + SetDelete(true) the marker behind `a_handle`, but only
    // after re-checking it: the handle still resolves (its age bits reject a recycled
    // slot), to `a_formID` (a recycled 0xFF FormID never matches a live handle), on the
    // XMarker base, not already deleted. Returns nullptr when it deleted the marker,
    // else why it did not (a reference that is not provably ours is never touched).
    const char* DeleteMarker(const RE::ObjectRefHandle& a_handle, RE::FormID a_formID);

    // ── THE CO-SAVED MARKER LEDGER (record 'XMRK', v1) ─────────────────────────
    // Every marker PlaceMarker made and DeleteMarker has not yet deleted (position
    // casts AND ch.19 position legs) is recorded, and the record is co-saved. A load
    // of that save then deletes each recorded marker the loaded world still holds,
    // so no save carries an APMF marker forward. "Fix-forward never cleans old
    // saves": stopping the write was not enough, the load must sweep.
    //
    // RECORD LAYOUT, v1 (unique ID 'APMF', record type 'XMRK'), little-endian:
    //     u32 count
    //     count x { u32 formID, f32 x, f32 y, f32 z }     (16 bytes per entry)
    // A reader is kept for every shipped version forever (INVARIANTS #15); a record
    // newer than this build is skipped and logged. A save with no 'XMRK' record
    // (0.9.7 and earlier) simply sweeps nothing.
    //
    // WHAT A LOAD DELETES, and only this (all three proofs, else FORGOTTEN, never
    // touched): the recorded 0xFF FormID resolves to a reference, its base is the
    // Skyrim.esm XMarker, and it stands within 1u of the recorded position (and it is
    // not already deleted). A recorded marker that is not in memory at the sweep (its
    // cell is not loaded) is CARRIED: re-saved and retried at the next load that finds
    // it, capped at kMaxCarriedMarkers (the oldest dropped, loudly).
    //
    // TIMELINE: revert callback -> RevertMarkers (forget the outgoing world's ledger);
    // SKSE load callback -> LoadMarkers (read the record into the carried list; refs
    // are not trusted yet); kPostLoadGame POSTS SweepCarriedMarkers to the confirmed-
    // main pump, so it runs on the FIRST player-Update after the load (the loaded
    // world's references exist and are looked up there). kPreLoadGame does not touch
    // the ledger; its Discard() drops a sweep a second load pre-empts, and the revert
    // then clears the record that sweep would have read. SKSE save callback -> SaveMarkers.
    inline constexpr std::uint32_t kMarkerRecordType    = 'XMRK';
    inline constexpr std::uint32_t kMarkerRecordVersion = 1;
    inline constexpr std::size_t   kMaxCarriedMarkers   = 64;

    void SaveMarkers(SKSE::SerializationInterface* a_intf);                         // SKSE save callback
    void LoadMarkers(SKSE::SerializationInterface* a_intf, std::uint32_t a_version); // SKSE load callback
    void RevertMarkers();                                                           // SKSE revert callback
    void SweepCarriedMarkers(const char* a_when);                                   // kPostLoadGame, MAIN THREAD

    // MAIN THREAD. Forget every tracked marker WITHOUT touching it (revert /
    // kPreLoadGame: the references belong to the world being replaced). Logs how
    // many were abandoned.
    void ResetAll(const char* why);

}
