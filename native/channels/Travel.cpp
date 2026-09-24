#include "PCH.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PackageData.h"
#include "core/PositionCast.h"   // ABI v11: PlaceMarker / DeleteMarker for kTravel_ToPosition legs
#include "core/Registry.h"
#include "channels/Travel.h"

#include <cmath>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) --
// the same one-line import core/Input.cpp, core/ActionGate.cpp and
// core/NonAliasProbe.cpp already use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 19 -- TRAVEL (kIntent_Travel, ABI v10, marth 2026-09-22).
//
// THE CONTRACT, in marth's own words: "it goes to the target 50-100u from it. And
// combat interrupts and cancels the movement. That's all."
//
// A client claims kIntent_Travel naming a DESTINATION. APMF walks the claimed
// actor to it and lets go the moment the actor is in combat. That is the entire
// feature. Move-to-a-place is a staple, not a niche: the hotkey-hunt case below is
// one caller of it.
//
// ---------------------------------------------------------------------------
// WHAT A DESTINATION MAY BE, and what it may NOT -- all of it read off the engine
// ---------------------------------------------------------------------------
// `param.form` is ONE FormID and its record type decides which location kind is
// written. No new struct, no new field, no flag, and no ambiguity -- a FormID is
// exactly one kind of record.
//
//   * an object REFERENCE (REFR/ACHR, incl. an actor or an XMarker)
//         -> PackageLocation::Type::kNearReference (0), the ref's handle.
//            Arrival = distance to the ref <= the leg's radius.
//   * a CELL
//         -> PackageLocation::Type::kInCell (1), a pointer to the cell.
//            Arrival = the actor's PARENT CELL is that cell. Distance means nothing
//            for a cell, so the radius is not consulted for this kind (it is still
//            written, for symmetry; the engine's own in-cell path never reads it).
//
//   * a WORLD POINT (ABI v11, `kTravel_ToPosition`, `param.form` = 0, `param.pos`)
//         -> APMF places its OWN non-persistent XMarker at the point (the idiom
//            vanilla uses for "go to this spot") and the leg is an ordinary
//            REFERENCE leg to that marker. Nothing below changes for it except that
//            a marker never "dies". The marker's lifetime is the leg's: see
//            RetireMarkerLater and every EndLeg / Release / Compose path. This is
//            NOT Docs/INVARIANTS.md #0 action (e): no cast, no decision, only a
//            package destination.
//
// EVERYTHING ELSE IS REFUSED, and here is the evidence rather than an opinion.
// `PackageLocation::AllocateLocation` (vtable slot 1) switches on locType through a
// 13-entry jump table; both unpacked images were decoded, 2026-09-22 (SE fn
// 0x441BE0 / table 0x442194, AE fn 0x49C9E0 / table 0x49CF94), and every case body
// is instruction-for-instruction identical across the two runtimes:
//
//   0 kNearReference             4-byte handle read       -> SUPPORTED
//   1 kInCell                    8-byte pointer read      -> SUPPORTED
//   2 kNearPackageStartLocation  reads the CONTEXT only, no payload. Not a
//                                destination a client can name. REFUSED.
//   3 kNearEditorLocation        loads three CONSTANT floats from .rdata; it never
//                                reads this struct for coordinates, and our
//                                generated record has no editor location. REFUSED.
//   4 kObjectID, 5 kObjectType, 7 kAtPackagelocation
//                                the jump table sends all three STRAIGHT TO THE
//                                EPILOGUE -- the engine does not implement them
//                                here at all. REFUSED, with proof.
//   6 kNearLinkedReference       8-byte KEYWORD pointer; resolves against the
//                                ACTOR's own linked ref. The client is not naming a
//                                destination, APMF would be choosing one. REFUSED.
//   8/9 kAlias_*                 a 4-byte ALIAS INDEX resolved against the OWNING
//                                QUEST's alias machinery. Our record carries no
//                                QNAM, and giving it one would hijack that quest.
//                                REFUSED.
//   12 kNearSelf                 reads the actor only, no payload. "Stay put" is
//                                ch.1's facet, not travel's. REFUSED.
//
// AN EXPLICIT WORLD POSITION IS NOT EXPRESSIBLE AT ALL -- this is a hard NOT FOUND,
// proven three ways, not a decision:
//   (a) `PackageLocation` is 0x18 bytes: vptr, locType, rad, and an 8-byte union of
//       `TESForm*` / `ObjectRefHandle`. There is no coordinate storage anywhere in
//       it.
//   (b) the on-disk PLDT subrecord is 12 bytes -- type, data, radius -- in ALL 1988
//       vanilla instances of the Travel template in Skyrim.esm. No vanilla record
//       carries coordinates in a package location either.
//   (c) no case in the switch above reads coordinates out of the struct.
// Vanilla's own idiom for "go to this spot" is to place an XMarker and point at the
// REFERENCE -- which is case 0, already supported. So a client that wants a point
// passes a marker ref. A claim carrying `param.posX/posY/posZ` is REFUSED with that
// message rather than silently reinterpreted.
//
// WHAT PROBLEM IT SOLVES. A third-party "hotkey sends my teammates at that enemy"
// plugin failed on ABI v9 because nothing in the public API moves an NPC to
// somewhere: ch.6 only records who owns the combat-target facet, and ch.9 offers a
// package the CLIENT has to ship in its own plugin. Their plugin fell back to
// calling the engine's start-combat function on a timer, which makes an actor
// search and pace rather than charge, because the engine will not path anyone to a
// foe they have never detected. Travel is the missing verb.
//
// ---------------------------------------------------------------------------
// ONE INTENT, ONE FACET -- ch.19 DOES NOT COMPOSE OTHER INTENTS
// ---------------------------------------------------------------------------
// marth, 2026-09-22: "We should avoid combining intents anyway." An earlier cut of
// this channel filed an internal ch.6 `kIntent_CombatTarget` claim on the client's
// behalf. **That is gone.** A client that wants the combat-target facet arbitrated
// claims `kIntent_CombatTarget` itself -- which is exactly what the third-party
// plugin already did. Intents stay orthogonal: ch.19 owns movement-to-a-place and
// nothing else.
//
// The ONE internal claim that remains is a ch.9 `kIntent_OfferPackage` claim, and
// it is NOT a composed client intent -- it is the IMPLEMENTATION of travel. The
// only way to move an NPC natively is to give the engine a package, so the package
// offer is to travel what a vtable write is to a deny: mechanism, not meaning. It
// is filed at the ch.19 claim's own basis so that arbitration stays honest (see
// RefileOffer), and a client never sees it.
//
// ---------------------------------------------------------------------------
// WHAT IT DOES, AND THE SHORT LIST OF WHAT IT DOES NOT
// ---------------------------------------------------------------------------
// DOES: point an APMF-OWNED Travel package (Data/APMF.esl, one record per
// concurrent leg) at the destination via its "Place to Travel" Location input
// (core/PackageData.cpp), offer it through ch.9's proven 0x49 redirect + the posted
// EvaluatePackage nudge, and END the leg on ARRIVAL, on the ACTOR ENTERING COMBAT,
// or on the destination going away.
//
// DOES NOT: claim the target (no ch.6), enter combat (no `StartCombat`), pin a
// combat target (no 0xE4 seat), or fake perception of any kind. There is NO
// line-of-sight test and NO detection test -- an earlier cut had both, and ending
// on line of sight was actively wrong: LOS is GEOMETRY, and an actor beside the
// player with a clear view of an enemy across the room has line of sight to a foe
// the engine has not detected, which is precisely the send-them-at-that-enemy case,
// so the leg ended on the first poll before the actor had moved. Detection went
// with it because `RequestDetectionLevel` is an UNOBSERVED path for this project
// (CLAUDE.md principle 5) and the contract does not mention perception at all.
//
// `Docs/INVARIANTS.md #0` is UNTOUCHED by this channel, and needs no amendment:
// travel calls no decision-generating engine function.
//
// ---------------------------------------------------------------------------
// THREADING -- every line in this file is on the confirmed-main seat
// ---------------------------------------------------------------------------
// `Engage`/`OnOwnerChanged`/`Release` run inside `ControlMap::Drain`'s apply loop,
// which runs on the PlayerCharacter 0xAD seat. `Poll()` runs on that same seat,
// later in the same `Arbiter::OncePerFrame`. Every deferred step goes through
// `mainthread::Post`, whose `Pump()` is also that seat. So `g_legs` and
// `g_slotOwner` are touched by exactly one thread and need no lock.
//
// `Channel::Tick` is deliberately NOT overridden. Tick runs from the *Character*
// 0xAD seat, which is field-proven multi-threaded (core/Hook.cpp's [threadcheck]),
// and the monitor resolves actor handles and makes engine calls. The monitor lives
// on OncePerFrame instead. It is also NOT a re-assert (Channel.h: "a re-assert loop
// is a FAILED block"): it never re-offers anything, it only decides when a leg is
// OVER.
//
// ---------------------------------------------------------------------------
// ORDERING -- why every ControlMap write is POSTED
// ---------------------------------------------------------------------------
// Channel.h forbids writing to the ControlMap from a lifecycle call, because those
// calls run BEFORE `Drain` publishes the new snapshot. This channel's job is to
// make one more claim, so every one of them is posted through `mainthread::Post`
// and runs one hop PAST that Publish -- the same idiom, for the same reason, that
// ch.9's own nudge uses (INVARIANTS #20). Each posted task RE-VALIDATES the ch.19
// claim it was posted for against the now-published snapshot and drops itself if
// the state moved.
// ============================================================================

namespace {

    using apmf::log::Hex;

    // ---- Shipped plugin + its FROZEN FormID band (APMF_GenerateESL.py) ----
    constexpr const char* kPlugin        = "APMF.esl";
    constexpr RE::FormID  kTravelPkgBase = 0x800;   // APMF_TravelPackage0
    // One package record per CONCURRENT leg. The Location input lives on the
    // RECORD, not on the actor, so two actors walking to two different places need
    // two records -- the same reason MFO ships four loot-travel records. Eight is
    // generous for a party-scale client and costs 8 PACK records in a 2.5 KB
    // plugin; past it the leg is REFUSED and logged loudly (never silently shared,
    // which would send both actors to one destination).
    constexpr std::size_t kTravelSlots = 8;

    // THE ARRIVAL RADIUS. marth's contract is "it goes to the target 50-100u from
    // it", so the default is the middle of that band. A client may pass its own in
    // `param.fval`; it is CLAMPED to [kMinRadiusUnits, kMaxRadiusUnits] and the
    // clamp is LOGGED, never applied silently.
    //
    // The same number is written into the package record's own Location radius for
    // this leg (core/PackageData.cpp SetTravelTarget writes `loc->rad`), so the
    // engine's own package stop and APMF's arrival test fire at the same distance
    // and cannot drift apart. The record's AUTHORED radius is only a placeholder --
    // it is overwritten before the package is ever offered, and if that write is
    // declined the leg is refused outright rather than offered with a stale one.
    // **The DLL's own test is the AUTHORITY** for ending the leg: it is what
    // releases the ch.9 claim; the record's radius only decides where the engine
    // parks the actor.
    //
    // Bounds: below ~50u an actor cannot reliably reach the point (bodies,
    // furniture and the destination's own collision), and above 512u "arrived"
    // stops meaning anything for the hand-off this exists to make.
    constexpr float kDefaultRadiusUnits = 75.0f;
    constexpr float kMinRadiusUnits     = 50.0f;
    constexpr float kMaxRadiusUnits     = 512.0f;

    // How often the monitor evaluates. It does one distance compare and one virtual
    // `IsInCombat()` per live leg -- at most kTravelSlots of each, four times a
    // second. Far finer than a travel leg needs, and cheap.
    constexpr std::uint64_t kPollPeriodMs = 250;

    // A SAFETY NET, not a feature budget (CLAUDE.md principle 9: size a deadline
    // from the real cadence, never from a guess). A leg that has not arrived, not
    // been cancelled by combat, and not lost its destination in two minutes is STUCK
    // -- unreachable destination, blocked navmesh, an outranking package we are
    // losing. Two minutes is far longer than any legitimate cross-cell travel we
    // expect to see, so it cannot cut a live leg short. When it fires the leg is
    // dropped and the reason is logged as a FAILURE, loudly and by name -- it is not
    // retried and it is not hidden (principle 7).
    constexpr std::uint64_t kLegMaxMs = 120000;

    // Clamp a client-supplied radius, saying so when it lands outside the band.
    float ClampRadius(RE::FormID a_id, float a_requested) {
        if (a_requested <= 0.0f) return kDefaultRadiusUnits;   // 0 == "use the default"
        if (a_requested < kMinRadiusUnits || a_requested > kMaxRadiusUnits) {
            const float clamped = a_requested < kMinRadiusUnits ? kMinRadiusUnits : kMaxRadiusUnits;
            spdlog::warn("[travel] 0x{} requested arrival radius {} is outside [{}, {}] -- CLAMPED to {}.",
                         Hex(a_id), a_requested, kMinRadiusUnits, kMaxRadiusUnits, clamped);
            return clamped;
        }
        return a_requested;
    }

    // ---- Install state (written once at kDataLoaded, read from any thread) ----
    std::atomic<bool>        g_installed{ false };
    // Atomic for the same reason core/EquipSink.cpp's twin is: ControlMap's
    // synchronous refusal reads it from whatever thread the client called RequestEx
    // on, while Install writes it on the game thread.
    std::atomic<const char*> g_notInstalledReason{ "not yet initialized (before kDataLoaded)" };
    RE::TESPackage*          g_pkg[kTravelSlots]{};

    // ---- Per-leg state. GAME THREAD ONLY (see the threading note). ----
    // Which kind of destination `destId` names. Decided ONCE, at claim time, from
    // the form's own record type -- never re-inferred later.
    enum class DestKind { kRef, kCell };

    struct Leg {
        RE::ActorHandle       actorHandle{};
        RE::FormID            destId   = 0;
        DestKind              destKind = DestKind::kRef;
        RE::ObjectRefHandle   destHandle{};   // kRef only: a REFERENCE, not necessarily an actor
        float                 radius = kDefaultRadiusUnits;
        std::uint32_t         flags  = 0;
        float                 basis  = 0.0f;
        int                   slot   = -1;                          // -1 == no package slot held
        APMF_API::Handle      hPackage = APMF_API::kInvalidHandle;  // the INTERNAL ch.9 offer
        // The basis the ch.9 offer was actually FILED at. A claim's basis is
        // IMMUTABLE once applied (`ApplyRepoint` updates the param and nothing
        // else), so when the ch.19 winner changes and the new owner's basis differs,
        // the offer does NOT follow by itself -- it has to be re-filed. See Compose.
        float                 basisPackage = 0.0f;
        bool                  legLive = false;
        std::uint64_t         legStartedMs = 0;
        // Was the destination ALIVE when it was targeted (engage or re-point)? Only
        // then does its death end the leg ("the destination died during travel"). A
        // destination already dead at target time is a corpse to walk to (MFO's loot
        // legs), so its deadness never ends the leg. Sampled in Compose, reset on
        // every re-point. False for a cell and until Compose has sampled it.
        bool                  destAliveAtTarget = false;

        // ABI v11 position legs (kTravel_ToPosition). `point` is what the client
        // declared; `marker` is APMF's XMarker for the point `markerPoint` (the two
        // differ only between a Repoint and the Compose that replaces the marker).
        // While a marker exists, destHandle/destId name it. APMF owns the marker and
        // deletes it when the leg ends for any reason.
        bool                  toPosition = false;
        RE::NiPoint3          point{};
        RE::ObjectRefHandle   marker{};
        RE::FormID            markerId = 0;
        RE::NiPoint3          markerPoint{};
    };

    std::unordered_map<RE::FormID, Leg> g_legs;
    RE::FormID                          g_slotOwner[kTravelSlots]{};   // 0 == free
    std::atomic<std::uint32_t>          g_legCount{ 0 };               // Poll's relaxed pre-gate
    std::uint64_t                       g_lastPollMs = 0;
    // Which package records were last pointed at an APMF marker (ABI v11). Read at the
    // load boundary (ResetAll), where the record would otherwise keep a handle to a
    // marker from the world being replaced.
    // The marker FormID each record was last pointed at (0 = not a marker).
    RE::FormID                          g_slotMarkerId[kTravelSlots]{};

    // Point a FREE slot's record back at its authored placeholder (PlayerRef,
    // APMF_GenerateESL.py) if it still names `a_markerId`, so no record keeps a
    // handle to a marker APMF is deleting. A slot another leg already took has been
    // re-pointed by that leg (its id no longer matches) and is left alone. GAME THREAD.
    void UnpointSlotsAt(RE::FormID a_markerId, const char* a_why) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kTravelSlots; ++i) {
            if (a_markerId == 0 || g_slotMarkerId[i] != a_markerId) continue;
            if (g_slotOwner[i] != 0) continue;   // still a live leg's record; that leg re-points it
            g_slotMarkerId[i] = 0;
            const bool ok = g_pkg[i] && player &&
                            apmf::packagedata::SetTravelTarget(g_pkg[i], player, kDefaultRadiusUnits);
            if (ok) {
                spdlog::info("[travel] package slot {} pointed back at its placeholder (PlayerRef) -- it named marker "
                             "0x{} ({}).", i, Hex(a_markerId), a_why);
            } else {
                spdlog::error("[travel] package slot {} could NOT be pointed back at its placeholder (see the [pkgdata] "
                              "line) -- it keeps a handle to deleted marker 0x{} until its next leg re-points it; no "
                              "leg offers it before then.", i, Hex(a_markerId));
            }
        }
    }

    bool SamePoint(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

    // Delete a leg's marker ONE HOP LATER. From every caller (Poll, which runs after
    // Pump; a posted Release/Compose, which run inside Pump) a Post lands on the next
    // frame's Pump, strictly after the Drain that applies the ch.9 offer release queued
    // beside it -- so the package is never still offered at a deleted marker. The
    // delete re-checks handle, FormID and base (poscast::DeleteMarker), so a recycled
    // 0xFF FormID is never touched. GAME THREAD.
    void RetireMarkerLater(RE::FormID a_id, RE::ObjectRefHandle a_marker, RE::FormID a_markerId, const char* a_why) {
        if (a_markerId == 0) return;
        apmf::mainthread::Post([a_id, a_marker, a_markerId, a_why] {
            // First take the marker out of every free package record, then delete it:
            // nothing of APMF's names it afterwards. (The slot was freed by a Post queued
            // BEFORE this one, so FIFO order makes it free here.)
            UnpointSlotsAt(a_markerId, a_why);
            if (const char* no = apmf::poscast::DeleteMarker(a_marker, a_markerId)) {
                spdlog::warn("[travel] 0x{} destination marker 0x{} NOT deleted ({}) -- {}.", Hex(a_id),
                             Hex(a_markerId), a_why, no);
            } else {
                spdlog::info("[travel] 0x{} destination marker 0x{} deleted ({}).", Hex(a_id), Hex(a_markerId),
                             a_why);
            }
        });
    }

    // Hand a leg's marker to RetireMarkerLater and forget it on the leg.
    void DropLegMarker(RE::FormID a_id, Leg& a_leg, const char* a_why) {
        if (a_leg.markerId == 0) return;
        RetireMarkerLater(a_id, a_leg.marker, a_leg.markerId, a_why);
        a_leg.marker   = {};
        a_leg.markerId = 0;
    }

    // ---- Slots ----------------------------------------------------------------

    int AcquireSlot(RE::FormID a_actor) {
        for (std::size_t i = 0; i < kTravelSlots; ++i) {
            if (g_slotOwner[i] == 0) {
                g_slotOwner[i] = a_actor;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void FreeSlot(int a_slot) {
        if (a_slot >= 0 && static_cast<std::size_t>(a_slot) < kTravelSlots) g_slotOwner[a_slot] = 0;
    }

    // Release the internal ch.9 offer and give its package slot back -- IN THAT
    // ORDER, WITH A HOP BETWEEN THEM. This is the one piece of ordering in this file
    // that is not obvious, so it is spelled out.
    //
    // `EnqueueRelease` does not release anything; it queues an op that the NEXT
    // `ControlMap::Drain` applies. Until that Drain the ch.9 claim is still in the
    // PUBLISHED snapshot, so the 0x49 thunk can still answer with this package.
    // Freeing the slot in the same breath would therefore open a window in which a
    // DIFFERENT leg can take the slot and re-point that record's Location -- and for
    // one frame the outgoing actor's still-live offer would walk it somewhere else.
    //
    // `mainthread::Pump` swaps its queue, so a task posted from inside Pump runs on
    // the NEXT frame's Pump (core/MainThread.cpp), which is strictly after that
    // frame's `Drain` -- i.e. strictly after the release above has been applied. One
    // hop is exactly enough, and it is enough from EVERY caller (the lifecycle
    // Release, which already runs a hop late, and the per-frame monitor).
    void DropOfferAndFreeSlot(APMF_API::Handle a_hPackage, int a_slot) {
        if (a_hPackage != APMF_API::kInvalidHandle)
            apmf::ControlMap::Get().EnqueueRelease(a_hPackage);
        if (a_slot >= 0)
            apmf::mainthread::Post([a_slot] { FreeSlot(a_slot); });
    }

    // Re-file the internal ch.9 offer at a NEW basis, because a claim's basis cannot
    // be changed in place: `ControlMap::ApplyRepoint` updates the param and nothing
    // else, and `Claim::basis` is fixed at apply time. Without this, a ch.19 claim
    // that changed owner kept arbitrating its package offer at the FIRST owner's
    // basis -- so after client B outbid ch.19 at 50, a third client could still take
    // the package facet from B at 30, and every document that says "at YOUR basis"
    // was wrong.
    //
    // REQUEST THE NEW CLAIM FIRST, THEN RELEASE THE OLD ONE. The order is the whole
    // point, not a style choice. Both ops land in the SAME `Drain`, which applies its
    // queue in enqueue order (`core/ControlMap.cpp` Drain's `for (const auto& op :
    // ops)`), so ch.9's claim count on this actor goes 1 -> 2 -> 1 and NEVER touches
    // 0. No `Release`/`Engage` lifecycle fires, the offer is never withdrawn for even
    // one frame, and the 0x49 thunk keeps answering throughout. Releasing first would
    // drop the facet, the framework package would resume, and the actor would stop
    // walking -- for a re-file the client never asked to feel.
    //
    // On a REFUSED fresh request the OLD claim is kept (re-pointed, so it is at least
    // aimed correctly) rather than released: an offer at a stale basis is strictly
    // better than no offer. The refusal is logged and `basisPackage` is left alone,
    // so the next Compose tries again.
    APMF_API::Handle RefileOffer(RE::FormID a_id, APMF_API::Handle a_old, float a_oldBasis,
                                 float a_newBasis, const APMF_API::APMF_Param& a_param) {
        auto& map = apmf::ControlMap::Get();
        const APMF_API::Handle fresh =
            map.EnqueueRequest(a_id, APMF_API::kIntent_OfferPackage, a_newBasis, &a_param);
        if (fresh == APMF_API::kInvalidHandle) {
            // UNREACHABLE TODAY, and written down so nobody spends an afternoon trying
            // to make it fire (Fable round 2 on 3c8adbf, SEV-5). `EnqueueRequest`
            // refuses only for an intent no channel serves, for ch.17 with its seat
            // down, and for ch.19's own gates -- none of which applies to a ch.9
            // request -- and there is no per-actor claim cap anywhere in the
            // ControlMap. So ch.9 has no synchronous refusal path at all and this
            // branch cannot currently be entered. It stays because the refusal is part
            // of `EnqueueRequest`'s CONTRACT rather than of today's implementation, and
            // the alternative to handling it is dropping the facet on a case we did not
            // foresee. If a future ch.9 gate makes it reachable, this is already the
            // right behaviour: keep the old claim, re-point it, log, and leave
            // `basisPackage` alone so the next Compose retries.
            map.EnqueueRepoint(a_old, &a_param);
            spdlog::error("[travel] 0x{} internal package offer could NOT be re-filed from basis {} to {} "
                          "(the request was refused; ControlMap logged why). KEEPING h={} at the old "
                          "basis -- a stale basis beats dropping the leg.",
                          Hex(a_id), a_oldBasis, a_newBasis, a_old);
            return a_old;
        }
        map.EnqueueRelease(a_old);
        spdlog::info("[travel] 0x{} internal package offer RE-FILED basis {} -> {} (h={} -> h={}; the new "
                     "claim is requested BEFORE the old is released, in one Drain, so the offer is never "
                     "unheld).",
                     Hex(a_id), a_oldBasis, a_newBasis, a_old, fresh);
        return fresh;
    }

    // Classify a client-named destination FormID. Returns false (having logged the
    // reason) for anything ch.19 cannot honestly point a package at -- see the
    // locType table in this file's header for why each kind is in or out.
    bool ClassifyDestination(RE::FormID a_id, RE::FormID a_dest, DestKind& out, const char* a_step) {
        auto* form = RE::TESForm::LookupByID(a_dest);
        if (!form) {
            spdlog::error("[travel] 0x{} {} REFUSED -- destination 0x{} is not a live form.",
                          Hex(a_id), a_step, Hex(a_dest));
            return false;
        }
        if (form->As<RE::TESObjectREFR>()) { out = DestKind::kRef;  return true; }
        if (form->As<RE::TESObjectCELL>()) { out = DestKind::kCell; return true; }

        spdlog::error("[travel] 0x{} {} REFUSED -- destination 0x{} is a {} ({}), and a travel "
                      "destination must be an object REFERENCE or a CELL. A world POSITION is not "
                      "expressible: a package's location carries a form or a handle and no "
                      "coordinates at all, on disk or at runtime. Place a marker and pass the "
                      "MARKER REFERENCE, which is what vanilla does.",
                      Hex(a_id), a_step, Hex(a_dest),
                      static_cast<std::uint32_t>(form->GetFormType()),
                      form->GetFormEditorID() ? form->GetFormEditorID() : "?");
        return false;
    }

    // ---- The leg ---------------------------------------------------------------

    // Release the internal ch.9 offer and free the package slot. The client's own
    // ch.19 claim is NOT touched: APMF never revokes a claim the client still holds
    // -- it only ends the work it started.
    void EndLeg(RE::FormID a_id, Leg& a_leg, const char* a_why, bool a_failure) {
        if (!a_leg.legLive) return;
        a_leg.legLive = false;

        DropOfferAndFreeSlot(a_leg.hPackage, a_leg.slot);
        a_leg.hPackage = APMF_API::kInvalidHandle;
        a_leg.slot     = -1;
        // ABI v11: a position leg's marker lives exactly as long as the leg. The
        // claim may stand on; a later Repoint/Compose places a fresh marker.
        DropLegMarker(a_id, a_leg, "the leg ended");

        const auto elapsed = apmf::clock::MonotonicMs() - a_leg.legStartedMs;
        if (a_failure) {
            spdlog::error("[travel-leg] 0x{} ABANDONED after {} ms -- {}. The ch.19 claim still stands "
                          "and is now doing NOTHING; the client should Release it or Repoint to a "
                          "reachable destination.",
                          Hex(a_id), elapsed, a_why);
        } else {
            spdlog::info("[travel-leg] 0x{} ENDED after {} ms -- {}.", Hex(a_id), elapsed, a_why);
        }
    }

    // Write this leg's destination into `a_pkg`'s Location input, by KIND. The two
    // kinds write DIFFERENT members of the same 8-byte union (a 4-byte handle for a
    // reference, an 8-byte form pointer for a cell) -- see core/PackageData.h for the
    // disassembly that establishes which, and why they are separate functions.
    bool PointPackage(RE::FormID a_id, const Leg& a_leg, RE::TESPackage* a_pkg) {
        if (a_leg.destKind == DestKind::kCell) {
            auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_leg.destId);
            if (!cell) {
                spdlog::error("[travel] 0x{} destination CELL 0x{} no longer resolves.",
                              Hex(a_id), Hex(a_leg.destId));
                return false;
            }
            return apmf::packagedata::SetTravelCell(a_pkg, cell, a_leg.radius);
        }
        auto  ptr = a_leg.destHandle.get();
        auto* ref = ptr.get();
        if (!ref) {
            spdlog::error("[travel] 0x{} destination ref 0x{} no longer resolves.",
                          Hex(a_id), Hex(a_leg.destId));
            return false;
        }
        return apmf::packagedata::SetTravelTarget(a_pkg, ref, a_leg.radius);
    }

    // Point this leg's package slot at the destination and offer it through ch.9.
    // Returns false (having offered nothing) if anything in the chain declines --
    // the failure is LOGGED, never masked with a retry or a fallback.
    bool StartLeg(RE::FormID a_id, Leg& a_leg) {
        const int slot = AcquireSlot(a_id);
        if (slot < 0) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- all {} package slots are in use by other legs. A "
                          "package's destination lives on the RECORD, so two legs cannot share one.",
                          Hex(a_id), kTravelSlots);
            return false;
        }

        RE::TESPackage* pkg = g_pkg[slot];
        if (!pkg) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- slot {} has no package (0x{} in {} did not "
                          "resolve).", Hex(a_id), slot, Hex(kTravelPkgBase + slot, 3), kPlugin);
            FreeSlot(slot);
            return false;
        }

        if (!PointPackage(a_id, a_leg, pkg)) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- could not point package 0x{}'s Location at "
                          "destination 0x{} (see the [pkgdata] line above for which guard failed). "
                          "NOTHING was offered: an unpointed travel package would walk the actor to the "
                          "placeholder ref.",
                          Hex(a_id), Hex(pkg->GetFormID()), Hex(a_leg.destId));
            FreeSlot(slot);
            return false;
        }

        g_slotMarkerId[slot] = (a_leg.markerId != 0 && a_leg.destId == a_leg.markerId) ? a_leg.markerId : 0;

        APMF_API::APMF_Param p9{};
        p9.form = pkg->GetFormID();
        const APMF_API::Handle h =
            apmf::ControlMap::Get().EnqueueRequest(a_id, APMF_API::kIntent_OfferPackage, a_leg.basis, &p9);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- the internal ch.9 offer was refused (ControlMap "
                          "logged why).", Hex(a_id));
            FreeSlot(slot);
            return false;
        }

        a_leg.slot         = slot;
        a_leg.hPackage     = h;
        a_leg.basisPackage = a_leg.basis;   // what the offer was actually FILED at
        a_leg.legLive      = true;
        a_leg.legStartedMs = apmf::clock::MonotonicMs();
        spdlog::info("[travel-leg] 0x{} STARTED -- slot {}, package 0x{} -> destination 0x{}, radius {}, "
                     "internal ch.9 handle {} at basis {}. ch.9 posts its own EvaluatePackage nudge.",
                     Hex(a_id), slot, Hex(p9.form), Hex(a_leg.destId),
                     static_cast<std::uint32_t>(a_leg.radius), h, a_leg.basis);
        return true;
    }

    // Re-read the published ch.19 claim and confirm this deferred step is still the
    // one the world wants. A posted task outlives the moment it was posted for: the
    // claim may have been released, replaced by a higher-basis one, or re-pointed at
    // another destination before Pump ran.
    bool StillOurs(RE::FormID a_id, const Leg& a_leg, const char* a_step, float* a_outBasis) {
        APMF_API::APMF_Param now{};
        float                basis = 0.0f;
        const bool claimed = apmf::ControlMap::Get().TryGetOwningClaimBasis(
            a_id, APMF_API::kIntent_Travel, now, basis);
        // ABI v11: a position leg is identified by its declared POINT (its destId is
        // APMF's marker, a FormID the client never sees).
        const bool nowToPos = (static_cast<std::uint32_t>(now.ival) & APMF_API::kTravel_ToPosition) != 0;
        const bool same = a_leg.toPosition
                              ? (nowToPos && SamePoint(a_leg.point, RE::NiPoint3{ now.posX, now.posY, now.posZ }))
                              : (!nowToPos && now.form == a_leg.destId);
        if (!claimed || !same) {
            spdlog::info("[travel] 0x{} {} DROPPED (stale) -- posted for destination 0x{}{}, now claim={} "
                         "destination 0x{}{}; a newer claim owns this edge.",
                         Hex(a_id), a_step, Hex(a_leg.destId), a_leg.toPosition ? " (point)" : "", claimed,
                         Hex(now.form), nowToPos ? " (point)" : "");
            return false;
        }
        if (a_outBasis) *a_outBasis = basis;
        return true;
    }

    // ---- The channel -----------------------------------------------------------

    class TravelChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "travel"; }
        int              ChannelNo() const override { return 19; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Travel; }

        // NO test-surface hotkey. The crosshair test surface can only name ONE ref,
        // and ch.19 needs an actor AND a destination; a hotkey that sent the aimed
        // NPC to a destination APMF picked would be APMF inventing intent (CLAUDE.md
        // principle 4). Drive it from the C-ABI.

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            // ControlMap::EnqueueRequest already refused a claim with no destination
            // and a claim made while the channel is down, so reaching here means both
            // held at request time. Re-test anyway -- the request was enqueued on some
            // other thread, possibly frames ago.
            const bool toPos = (static_cast<std::uint32_t>(param.ival) & APMF_API::kTravel_ToPosition) != 0;
            if (param.form == 0 && !toPos) {
                spdlog::error("[travel] 0x{} engage IGNORED -- param.form is 0 (no destination).", Hex(id));
                return;
            }

            Leg leg{};
            leg.destId = param.form;
            leg.radius = ClampRadius(id, param.fval);
            leg.flags  = static_cast<std::uint32_t>(param.ival);
            if (actor && actor->IsHandleValid()) leg.actorHandle = actor->GetHandle();

            // ABI v11 position leg: the marker is placed by Compose, one hop past this
            // Drain's Publish, on the same confirmed-main seat as every other leg step.
            if (toPos) {
                if (param.form != 0) {   // ControlMap already refused this; re-tested, never trusted
                    spdlog::error("[travel] 0x{} engage IGNORED -- kTravel_ToPosition with a non-zero form.",
                                  Hex(id));
                    return;
                }
                leg.toPosition = true;
                leg.point      = RE::NiPoint3{ param.posX, param.posY, param.posZ };
                leg.destId     = 0;
                leg.destKind   = DestKind::kRef;
                g_legs[id]     = leg;
                g_legCount.store(static_cast<std::uint32_t>(g_legs.size()), std::memory_order_relaxed);
                spdlog::info("[travel] 0x{} travel facet CLAIMED -- destination POINT {:.0f},{:.0f},{:.0f} (APMF "
                             "places its own XMarker there), radius {}, flags 0x{}.",
                             Hex(id), leg.point.x, leg.point.y, leg.point.z, static_cast<std::uint32_t>(leg.radius),
                             Hex(leg.flags, 2));
                apmf::mainthread::Post([id] { Compose(id, "engage"); });
                return;
            }

            if (!ClassifyDestination(id, leg.destId, leg.destKind, "engage")) return;

            const char* destName = "?";
            if (leg.destKind == DestKind::kRef) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(leg.destId);
                if (!ref) return;                       // ClassifyDestination just resolved it
                leg.destHandle = ref->CreateRefHandle();
                if (ref->GetName()) destName = ref->GetName();
            } else {
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(leg.destId);
                if (!cell) return;
                if (cell->GetFormEditorID()) destName = cell->GetFormEditorID();
            }

            g_legs[id] = leg;
            g_legCount.store(static_cast<std::uint32_t>(g_legs.size()), std::memory_order_relaxed);

            spdlog::info("[travel] 0x{} travel facet CLAIMED -- destination {} 0x{} '{}', radius {}, "
                         "flags 0x{}.",
                         Hex(id), leg.destKind == DestKind::kCell ? "CELL" : "ref", Hex(leg.destId),
                         destName, static_cast<std::uint32_t>(leg.radius), Hex(leg.flags, 2));

            // Compose one hop past this Drain's Publish (see the ordering note).
            apmf::mainthread::Post([id] { Compose(id, "engage"); });
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            auto it = g_legs.find(id);
            if (it == g_legs.end()) {
                // A winning claim arrived on a channel that was already engaged but
                // whose state we never built (a refused engage). Treat it as a fresh
                // engage rather than half-applying a re-point.
                Engage(id, actor, param);
                return;
            }
            Leg& leg = it->second;

            // ABI v11: a Repoint to a POINT. The current destination (a ref, a cell, or
            // the previous marker) stays in force until Compose places the new marker and
            // re-points the package; Compose then deletes the old marker.
            if ((static_cast<std::uint32_t>(param.ival) & APMF_API::kTravel_ToPosition) != 0) {
                const RE::NiPoint3 pt{ param.posX, param.posY, param.posZ };
                if (param.form != 0 || !std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) {
                    spdlog::error("[travel] 0x{} re-point IGNORED -- kTravel_ToPosition needs form 0 and a finite "
                                  "point. The previous destination 0x{} stands.", Hex(id), Hex(leg.destId));
                    return;
                }
                leg.toPosition = true;
                leg.point      = pt;
                leg.radius     = ClampRadius(id, param.fval);
                leg.flags      = static_cast<std::uint32_t>(param.ival);
                spdlog::info("[travel] 0x{} travel claim RE-POINTED -- destination 0x{} -> POINT {:.0f},{:.0f},{:.0f}.",
                             Hex(id), Hex(leg.destId), pt.x, pt.y, pt.z);
                apmf::mainthread::Post([id] { Compose(id, "re-point"); });
                return;
            }

            if (param.form == 0) {
                spdlog::error("[travel] 0x{} re-point IGNORED -- param.form is 0 (no destination).",
                              Hex(id));
                return;
            }

            DestKind kind = DestKind::kRef;
            if (!ClassifyDestination(id, param.form, kind, "re-point")) {
                spdlog::error("[travel] 0x{} re-point IGNORED -- the previous destination 0x{} stands.",
                              Hex(id), Hex(leg.destId));
                return;
            }

            const RE::FormID was = leg.destId;
            leg.toPosition = false;   // a form destination; any marker is deleted by Compose
            leg.destId     = param.form;
            leg.destKind   = kind;
            leg.destHandle = {};
            leg.radius     = ClampRadius(id, param.fval);
            leg.flags      = static_cast<std::uint32_t>(param.ival);
            if (kind == DestKind::kRef) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(leg.destId);
                if (!ref) return;                       // ClassifyDestination just resolved it
                leg.destHandle = ref->CreateRefHandle();
            }

            spdlog::info("[travel] 0x{} travel claim RE-POINTED -- destination 0x{} -> {} 0x{}.",
                         Hex(id), Hex(was), kind == DestKind::kCell ? "CELL" : "ref", Hex(leg.destId));

            apmf::mainthread::Post([id] { Compose(id, "re-point"); });
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            auto it = g_legs.find(id);
            if (it == g_legs.end()) {
                spdlog::info("[travel] 0x{} travel facet released (nothing was started).", Hex(id));
                return;
            }
            const Leg leg = it->second;   // by VALUE: the entry goes away now
            g_legs.erase(it);
            g_legCount.store(static_cast<std::uint32_t>(g_legs.size()), std::memory_order_relaxed);

            spdlog::info("[travel] 0x{} travel facet RELEASED -- dropping the internal package offer.",
                         Hex(id));

            // POSTED, like every other ControlMap write here: this Release runs inside
            // Drain's apply loop, before Publish. The slot stays OWNED until the posted
            // teardown runs, so a release-then-re-request inside ONE Drain cannot hand
            // the new leg the very record the outgoing ch.9 claim is still offering
            // (there are kTravelSlots of them; it gets another).
            apmf::mainthread::Post([id, leg] {
                DropOfferAndFreeSlot(leg.hPackage, leg.slot);
                // ABI v11: the marker goes one hop after the offer release is queued
                // (RetireMarkerLater posts again from inside this Pump).
                RetireMarkerLater(id, leg.marker, leg.markerId, "the claim was released");
            });
        }

    private:
        // Build (or rebuild) the leg for `id`. Runs on the confirmed-main seat, one
        // hop past the Drain that engaged/re-pointed the claim.
        static void Compose(RE::FormID id, const char* step) {
            auto it = g_legs.find(id);
            if (it == g_legs.end()) return;   // released before this ran
            Leg& leg = it->second;

            float basis = 0.0f;
            if (!StillOurs(id, leg, step, &basis)) return;
            leg.basis = basis;

            // ABI v11: a marker that no longer matches what the claim declares (a
            // form destination now, or a different point) is taken OFF the leg here
            // and deleted after this Compose has re-pointed or ended the leg, on every
            // exit (see the tail of this function).
            RE::ObjectRefHandle oldMarker{};
            RE::FormID          oldMarkerId = 0;
            RE::NiPoint3        oldMarkerPoint{};
            if (leg.markerId != 0 && (!leg.toPosition || !SamePoint(leg.markerPoint, leg.point))) {
                oldMarker      = leg.marker;
                oldMarkerId    = leg.markerId;
                oldMarkerPoint = leg.markerPoint;
                leg.marker   = {};
                leg.markerId = 0;
            }

            ComposeLeg(id, leg, step);

            // NEVER DELETE A MARKER A LIVE SLOT STILL NAMES (review F5). If ComposeLeg
            // returned early with the leg still live and its package record still aimed
            // at the old marker (the new destination did not resolve, so nothing was
            // re-pointed), the old marker goes back on the leg: it is retired when that
            // leg ends (EndLeg frees the slot first), not now under a live offer.
            if (oldMarkerId != 0 && leg.legLive && leg.slot >= 0 && g_slotMarkerId[leg.slot] == oldMarkerId) {
                if (leg.markerId != 0) DropLegMarker(id, leg, "placed for a re-point that did not take");
                leg.marker      = oldMarker;
                leg.markerId    = oldMarkerId;
                leg.markerPoint = oldMarkerPoint;
                spdlog::info("[travel] 0x{} {}: marker 0x{} KEPT -- the live leg's package still names it; it is "
                             "deleted when the leg ends.", Hex(id), step, Hex(oldMarkerId));
                oldMarkerId = 0;
            }
            RetireMarkerLater(id, oldMarker, oldMarkerId, "re-pointed away from it");
            // A position leg that did not start (or was ended above) must not keep a
            // marker nothing walks to.
            if (!leg.legLive) DropLegMarker(id, leg, "the leg did not start");
        }

        // The body of Compose. Every return leaves `leg` consistent; Compose's tail
        // disposes of markers.
        static void ComposeLeg(RE::FormID id, Leg& leg, const char* step) {
            const float basis = leg.basis;

            // ABI v11: place this position leg's marker. The leg then IS a reference
            // leg to it, through the unchanged path below.
            if (leg.toPosition && leg.markerId == 0) {
                auto  aptr  = leg.actorHandle.get();
                auto* actor = aptr.get();
                std::string why = "the actor no longer resolves";
                RE::ObjectRefHandle h{};
                if (actor) h = apmf::poscast::PlaceMarker(actor, leg.point, why);
                auto  mptr   = h.get();
                auto* marker = mptr.get();
                if (!marker) {
                    spdlog::error("[travel] 0x{} {} REFUSED -- no destination marker at {:.0f},{:.0f},{:.0f}: {}.",
                                  Hex(id), step, leg.point.x, leg.point.y, leg.point.z, why);
                    if (leg.legLive) EndLeg(id, leg, "the destination marker could not be placed", true);
                    return;
                }
                leg.marker      = h;
                leg.markerId    = marker->GetFormID();
                leg.markerPoint = leg.point;
                leg.destKind    = DestKind::kRef;
                leg.destHandle  = h;
                leg.destId      = leg.markerId;
                spdlog::info("[travel] 0x{} {}: destination marker 0x{} placed at {:.0f},{:.0f},{:.0f} (cell 0x{}).",
                             Hex(id), step, Hex(leg.markerId), leg.point.x, leg.point.y, leg.point.z,
                             Hex(marker->GetParentCell() ? marker->GetParentCell()->GetFormID() : 0));
            }

            // A ref destination must still RESOLVE; a cell destination is a plain form
            // that does not unload, so the lookup in PointPackage is the only check it
            // needs.
            RE::NiPointer<RE::TESObjectREFR> destPtr;
            if (leg.destKind == DestKind::kRef) {
                destPtr = leg.destHandle.get();
                if (!destPtr.get()) {
                    spdlog::error("[travel] 0x{} {} DROPPED -- destination ref 0x{} no longer resolves "
                                  "(unloaded or deleted).", Hex(id), step, Hex(leg.destId));
                    return;
                }
                if (leg.toPosition) {
                    // A marker cannot die: the death rule does not apply to it.
                    leg.destAliveAtTarget = false;
                } else {
                    // Sample the destination's life at TARGET time (marth: "If the target
                    // is dead when targeted, it's fine. But if it dies during travel,
                    // drop."). Same IsDead call Poll's death end makes.
                    leg.destAliveAtTarget = !destPtr->IsDead();
                    spdlog::info("[travel] 0x{} {}: destination 0x{} {}.", Hex(id), step, Hex(leg.destId),
                                 leg.destAliveAtTarget ? "alive at target time"
                                                       : "dead at target time -- walking to a corpse");
                }
            } else {
                leg.destAliveAtTarget = false;
            }


            // A live leg is RE-POINTED in place (rewrite the record's Location, then
            // either re-file the offer at a moved basis or re-point it so its nudge
            // fires again); otherwise start a fresh one.
            if (leg.legLive && leg.slot >= 0 && g_pkg[leg.slot]) {
                if (!PointPackage(id, leg, g_pkg[leg.slot])) {
                    spdlog::error("[travel-leg] 0x{} re-point FAILED -- could not re-point package 0x{}'s "
                                  "Location; ENDING the leg rather than leaving the actor walking at the "
                                  "old destination.",
                                  Hex(id), Hex(g_pkg[leg.slot]->GetFormID()));
                    EndLeg(id, leg, "the Location re-point was declined", true);
                    return;
                }
                g_slotMarkerId[leg.slot] = (leg.markerId != 0 && leg.destId == leg.markerId) ? leg.markerId : 0;
                APMF_API::APMF_Param p9{};
                p9.form = g_pkg[leg.slot]->GetFormID();
                if (leg.basisPackage != basis) {
                    const APMF_API::Handle kept =
                        RefileOffer(id, leg.hPackage, leg.basisPackage, basis, p9);
                    if (kept != leg.hPackage) {   // the re-file took; the stale-basis claim is gone
                        leg.hPackage     = kept;
                        leg.basisPackage = basis;
                    }
                } else {
                    apmf::ControlMap::Get().EnqueueRepoint(leg.hPackage, &p9);
                }
                leg.legStartedMs = apmf::clock::MonotonicMs();   // a new leg, a new deadline
                spdlog::info("[travel-leg] 0x{} RE-POINTED -- slot {}, package 0x{} -> destination 0x{}.",
                             Hex(id), leg.slot, Hex(p9.form), Hex(leg.destId));
            } else {
                StartLeg(id, leg);
            }
        }
    };

}

namespace apmf::travel {

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;

        if (REL::Module::IsVR()) {
            g_notInstalledReason.store("VR runtime (ch.9's 0x49 seat is SE/AE only, so travel has no "
                                       "delivery mechanism)", std::memory_order_release);
            spdlog::warn("[travel] NOT installed -- {}.",
                         g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        if (GetPrivateProfileIntA("Travel", "bTravel", 1, "Data/SKSE/Plugins/APMF.ini") == 0) {
            g_notInstalledReason.store("[Travel] bTravel=0 in Data/SKSE/Plugins/APMF.ini",
                                       std::memory_order_release);
            spdlog::info("[travel] NOT installed -- {}.",
                         g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }


        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            g_notInstalledReason.store("TESDataHandler unavailable at kDataLoaded",
                                       std::memory_order_release);
            spdlog::error("[travel] NOT installed -- {}.",
                          g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        std::size_t resolved = 0;
        for (std::size_t i = 0; i < kTravelSlots; ++i) {
            const RE::FormID local = kTravelPkgBase + static_cast<RE::FormID>(i);
            g_pkg[i] = dh->LookupForm<RE::TESPackage>(local, kPlugin);
            if (g_pkg[i]) {
                ++resolved;
            } else {
                // Name the form. A bare count is the aggregate that hides a
                // 100%-systematic failure.
                spdlog::error("[travel] MISSING APMF_TravelPackage{} (0x{} in {}) -- is the plugin "
                              "installed and enabled?", i, Hex(local, 3), kPlugin);
            }
        }

        if (resolved == 0) {
            g_notInstalledReason.store("Data/APMF.esl is missing or disabled (no travel package resolved)",
                                       std::memory_order_release);
            spdlog::error("[travel] NOT installed -- {}. A kIntent_Travel claim will be REFUSED so the "
                          "client's own degrade path runs.",
                          g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        g_installed.store(true, std::memory_order_release);
        spdlog::info("[travel] installed -- {} of {} travel packages resolved from {}. "
                     "Arrival radius default {}u (client range {}-{}u), poll {} ms, leg safety net {} ms. "
                     "A leg ends on ARRIVAL, on the ACTOR ENTERING COMBAT, or on the destination being "
                     "gone -- and nothing else. No perception test of any kind, and no other intent is "
                     "claimed on the client's behalf.",
                     resolved, kTravelSlots, kPlugin,
                     static_cast<std::uint32_t>(kDefaultRadiusUnits),
                     static_cast<std::uint32_t>(kMinRadiusUnits),
                     static_cast<std::uint32_t>(kMaxRadiusUnits),
                     kPollPeriodMs, kLegMaxMs);
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_notInstalledReason.load(std::memory_order_acquire); }

    // ABI v11: put every package record that was last pointed at an APMF marker back
    // on its AUTHORED placeholder (PlayerRef, APMF_GenerateESL.py), so no record
    // carries a handle to a marker from the world being replaced into the next one.
    // The markers themselves are FORGOTTEN, never touched: they belong to that world.
    void RestoreMarkerSlots(const char* why) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kTravelSlots; ++i) {
            if (g_slotMarkerId[i] == 0) continue;
            g_slotMarkerId[i] = 0;
            if (!g_pkg[i] || !player) continue;
            const bool ok = apmf::packagedata::SetTravelTarget(g_pkg[i], player, kDefaultRadiusUnits);
            if (ok) {
                spdlog::info("[travel] {} -- package slot {} pointed back at its placeholder (PlayerRef); it last "
                             "pointed at an APMF marker.", why, i);
            } else {
                spdlog::error("[travel] {} -- package slot {} could NOT be pointed back at its placeholder (see the "
                              "[pkgdata] line). It keeps a stale marker handle until its next leg re-points it; "
                              "no leg offers it before then.", why, i);
            }
        }
    }

    void ResetAll(const char* why) {
        RestoreMarkerSlots(why);
        if (g_legs.empty()) {
            for (auto& o : g_slotOwner) o = 0;
            return;
        }
        std::size_t markers = 0;
        for (const auto& [id, leg] : g_legs) markers += leg.markerId != 0 ? 1 : 0;
        if (markers != 0) {
            spdlog::warn("[travel] {} -- forgetting {} destination marker(s) WITHOUT deleting them (they belong to "
                         "the world being replaced; a save taken mid-leg records them, and its load deletes them).",
                         why, markers);
        }
        spdlog::info("[travel] {} -- dropping {} leg(s) and freeing every package slot without releasing "
                     "the internal offers (the world is being replaced).", why, g_legs.size());
        g_legs.clear();
        g_legCount.store(0, std::memory_order_relaxed);
        for (auto& o : g_slotOwner) o = 0;
    }

    void Poll() {
        // Relaxed pre-gate: nothing travelling costs one atomic load.
        if (g_legCount.load(std::memory_order_relaxed) == 0) return;

        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (now - g_lastPollMs < kPollPeriodMs) return;
        g_lastPollMs = now;

        for (auto& [id, leg] : g_legs) {
            if (!leg.legLive) continue;

            // The ACTOR first: a leg whose actor is gone or dead is over whatever the
            // destination is doing.
            auto  aptr = leg.actorHandle.get();
            auto* a    = aptr.get();
            if (!a) {
                EndLeg(id, leg, "the actor no longer resolves (unloaded or deleted)", false);
                continue;
            }
            if (a->IsDead()) {
                EndLeg(id, leg, "the actor is dead", false);
                continue;
            }
            if (!a->Is3DLoaded()) {
                EndLeg(id, leg, "the actor's 3D is not loaded", false);
                continue;
            }

            // COMBAT CANCELS THE MOVEMENT. marth's contract: "combat interrupts and
            // cancels the movement."
            //
            // WHAT `IsInCombat()` ACTUALLY IS, verified rather than assumed (both
            // unpacked images, 2026-09-22). It is Character vtable slot 0xE3 (VR 0xE5,
            // which never runs here), and on BOTH runtimes its whole body is:
            //     mov rax, [rcx + 0x158]   (SE 1.5.97)  /  [rcx + 0x160]  (AE 1.6.1170)
            //     test rax, rax  -> je  return false          ; the CombatController*
            //     cmp byte [rax + 0x43], 0 -> jne return false
            //     return true
            // SE 0x625660 (addrlib 37609), AE 0x6B6DD0 (addrlib 38562), instruction-for-
            // instruction identical apart from that one member offset -- which is the
            // AE +8 shift, and which we never touch ourselves because the call goes
            // through the vtable.
            //
            // So the answer to "what does this mean for an actor the engine has not
            // given a controller yet" is: it reads the actor's `combatController`
            // pointer, and a NULL controller returns FALSE cleanly with no dereference.
            // An actor who has not been put in combat is simply not in combat. There is
            // no cheaper signal worth using: this IS the cheap signal -- one virtual
            // call, two loads and a compare, no allocation, no search.
            //
            // ONLY THE MOVEMENT IS CANCELLED. The client's ch.19 claim is left standing
            // (APMF never revokes a claim the client holds), and nothing else is
            // touched -- ch.19 owns no other facet to give back.
            if (a->IsInCombat()) {
                EndLeg(id, leg, "the actor is IN COMBAT -- combat cancels the movement", false);
                continue;
            }

            // The DESTINATION, and ARRIVAL -- both depend on which KIND it is.
            if (leg.destKind == DestKind::kCell) {
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(leg.destId);
                if (!cell) {
                    EndLeg(id, leg, "the destination cell no longer resolves", false);
                    continue;
                }
                // ARRIVAL FOR A CELL IS PARENT-CELL IDENTITY, not distance. A cell has
                // no single position to measure against -- an interior is a volume and
                // an exterior cell is a 4096-unit tile -- so "within 75u of a cell" has
                // no meaning. The actor is either in it or not, which is also exactly
                // what the engine's own in-cell package path is steering towards, so
                // the two agree by construction. The leg's radius is not consulted here
                // (it is still written into the record, for symmetry).
                if (a->GetParentCell() == cell) {
                    EndLeg(id, leg, "ARRIVED (the actor's parent cell is the destination cell)", false);
                    continue;
                }
            } else {
                // THE DESTINATION IS NOT REQUIRED TO BE LOADED, and this is the one
                // place it is easy to get backwards (Fable round 2 on 3c8adbf, SEV-3).
                // An earlier cut ended the leg as soon as `d->Is3DLoaded()` went false,
                // which forbade BY CONSTRUCTION the thing a vanilla Travel package
                // exists to do -- path across cells to somewhere the actor cannot yet
                // see -- so any destination outside the loaded area ended on the FIRST
                // poll, as a non-failure, with the actor never having moved. It also
                // put the documented XMarker case at risk, since whether a marker
                // reference carries a live NiNode at all is not something this pass
                // could establish (see below). The test is GONE.
                //
                // Nothing needs it: the arrival compare below uses `GetPosition()`,
                // which is a plain read of the reference's own `data.location` and is
                // valid whether or not any 3D is attached.
                //
                // WHAT BOUNDS A LEG, then, now that "too far away" is not an end
                // condition: (1) the actor entering combat, which is the contract's own
                // cancel and fires wherever the actor is; (2) the destination dying
                // DURING travel (alive when targeted), being disabled, or its HANDLE
                // going stale (the checks that remain
                // below -- a handle that no longer resolves really is gone, unlike an
                // unloaded-but-alive ref); (3) the client's own Release; and (4) the
                // kLegMaxMs safety net, which reports a leg that got nowhere as a
                // FAILURE rather than letting it hold a package slot for ever.
                //
                // DELIBERATELY NOT ADDED: a cross-worldspace refusal. It was considered
                // and rejected because it is the same mistake one level up -- an
                // interior has no worldspace at all, so "different worldspace" does not
                // mean "unreachable", and travelling from an interior out into Tamriel
                // is exactly what vanilla Travel packages do. No pathing heuristics
                // either; the engine owns pathing and the safety net owns giving up.
                //
                // IsDisabled() reads the ref's kInitiallyDisabled form flag, which is
                // the SAME flag the engine's runtime Disable() sets -- so it covers a
                // scripted despawn, not only an editor-disabled ref.
                auto  dptr = leg.destHandle.get();
                auto* d    = dptr.get();
                if (!d) {
                    EndLeg(id, leg, "the destination no longer resolves (deleted)", false);
                    continue;
                }
                if (d->IsDisabled()) {
                    EndLeg(id, leg, "the destination was disabled", false);
                    continue;
                }
                // DEATH ends the leg only if the destination was ALIVE when targeted
                // (see Leg::destAliveAtTarget). A destination dead at target time is a
                // corpse to walk to, and ends the leg only on the other checks here.
                if (leg.destAliveAtTarget && d->IsDead()) {
                    EndLeg(id, leg, "the destination died during travel", false);
                    continue;
                }

                // ARRIVED. The same radius this leg wrote into the package record, so
                // the engine's own stop and this test fire at the same distance.
                // `GetPosition()` is `data.location` -- no 3D required.
                const float dist = a->GetPosition().GetDistance(d->GetPosition());
                if (dist <= leg.radius) {
                    EndLeg(id, leg, "ARRIVED (inside the arrival radius)", false);
                    continue;
                }
            }

            // The safety net, last: everything above is a legitimate end, this is a
            // failure report.
            if (now - leg.legStartedMs > kLegMaxMs) {
                EndLeg(id, leg, "STUCK -- no arrival, no combat, and the destination is still there "
                                "after the leg safety net elapsed (unreachable or cross-worldspace "
                                "destination, blocked path, or an outranking package is winning)", true);
            }
        }
    }

}

APMF_REGISTER_CHANNEL(TravelChannel);
