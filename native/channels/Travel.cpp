#include "PCH.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate (ABI v12 gate probe)
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PackageData.h"
#include "core/PositionCast.h"   // ABI v11: PlaceMarker / DeleteMarker for kTravel_ToPosition legs
#include "core/Registry.h"
#include "channels/Travel.h"
#include "channels/Travel_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

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
// on the destination going away, or (ABI v12) BLOCKED: the engine holding the actor in
// its own Movement Blocked package for kBlockedEndMs. Every end is recorded as a
// TravelLegState a client reads with APMF_API_v12::GetTravelLegState, because the
// client's claim outlives the leg.
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
// ABI v12 adds exactly two cross-thread structures, each behind its own mutex and
// never holding it across an engine call: the leg-state mirror `g_state` (written here,
// read by any client thread through GetTravelLegState) and the gate-probe watch list
// `g_watches` (written here, read by the OpenClose / Activate event sinks, which run on
// whatever thread the engine sends those events from). The sinks read only plain
// fields of the event's own reference and hand every engine call (GetOpenState, form
// lookups) to `mainthread::Post`.
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
}

    RE::TESPackage*          g_pkg[kTravelSlots]{};

    // ABI v12 GAIT. The AUTHORED speed of each package record (APMF_GenerateESL.py writes
    // PKDT flags 0x2000 + byte 6 = 2, Run), captured at Install, so a leg WITHOUT
    // kTravel_SpeedSet walks at exactly what was authored even when the same record's
    // previous leg declared another gait. `g_gaitVerified` = the running build is one the
    // speed path was read on (1.6.1170 / 1.5.97); anything else leaves the record alone
    // and says so.
    bool                     g_authoredPrefSpeed[kTravelSlots]{};
    std::uint8_t             g_authoredSpeed[kTravelSlots]{};
    bool                     g_gaitVerified = false;

    void ResetBlockedClock(Leg& a_leg) {
        a_leg.mbAccumMs    = 0;
        a_leg.mbLastPollMs = 0;
        a_leg.mbRun        = false;
        a_leg.mbMissed     = false;
    }

    std::unordered_map<RE::FormID, Leg> g_legs;
    RE::FormID                          g_slotOwner[kTravelSlots]{};   // 0 == free
    std::atomic<std::uint32_t>          g_legCount{ 0 };               // Poll's relaxed pre-gate
    std::uint64_t                       g_lastPollMs = 0;
    // Which package records were last pointed at an APMF marker (ABI v11). Read at the
    // load boundary (ResetAll), where the record would otherwise keep a handle to a
    // marker from the world being replaced.
    // The marker FormID each record was last pointed at (0 = not a marker).
    RE::FormID                          g_slotMarkerId[kTravelSlots]{};

    std::mutex                                  g_stateMx;
    std::unordered_map<RE::FormID, LegStateRec> g_state;   // guarded by g_stateMx
    // One stamp counter for every record, NEVER reset while the game runs (ResetAll
    // clears the records, not this), so a `seq` a client cached before a load can never
    // equal one issued after it (review F5). Guarded by g_stateMx.
    std::uint32_t                               g_seqCounter = 0;

namespace {

    class GateEventSink final : public RE::BSTEventSink<RE::TESOpenCloseEvent>,
                                public RE::BSTEventSink<RE::TESActivateEvent> {
    public:
        static GateEventSink* GetSingleton() { static GateEventSink s; return &s; }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::TESOpenCloseEvent>*) override {
            try {
                if (a_event) OnGateEvent("OPEN-CLOSE", a_event->ref.get(), a_event->activeRef.get(), a_event->opened ? 1 : 0);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event,
                                              RE::BSTEventSource<RE::TESActivateEvent>*) override {
            try {
                if (a_event) OnGateEvent("ACTIVATE", a_event->objectActivated.get(), a_event->actionRef.get(), -1);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

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
                SetLegStateFor(id, leg, APMF_API::kLeg_Pending, 0xFFFFFFFFu);
                spdlog::info("[travel] 0x{} travel facet CLAIMED -- destination POINT {:.0f},{:.0f},{:.0f} (APMF "
                             "places its own XMarker there), radius {}, flags 0x{}.",
                             Hex(id), leg.point.x, leg.point.y, leg.point.z, static_cast<std::uint32_t>(leg.radius),
                             Hex(leg.flags, 2));
                apmf::mainthread::Post([id] { Compose(id, "engage"); });
                return;
            }

            if (!ClassifyDestination(id, leg.destId, leg.destKind, "engage")) {
                SetLegStateFor(id, leg, APMF_API::kLeg_Failed, 0xFFFFFFFFu);   // ABI v12: the claim does nothing
                return;
            }

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
            SetLegStateFor(id, leg, APMF_API::kLeg_Pending, 0xFFFFFFFFu);

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

            // ABI v12 (review F2): a REFUSED re-point. The claim now declares a destination
            // ch.19 cannot walk to, so the previous leg is ENDED rather than left walking to
            // a destination nobody declares any more (CLAUDE.md principle 4: enforce only
            // what was declared), and the state names the REFUSED destination as kLeg_Failed
            // so the client learns at once. The leg entry stays (not live); a later valid
            // Repoint starts a fresh leg through Compose. Nothing on `leg` is changed before
            // the refusal is decided.
            const auto refuse = [&](const char* a_why, RE::FormID a_form, const RE::NiPoint3& a_point) {
                spdlog::error("[travel] 0x{} re-point REFUSED -- {}. {}", Hex(id), a_why,
                              leg.legLive ? "The previous leg is ENDED (it no longer matches the claim)."
                                          : "No leg was running.");
                EndLeg(id, leg, "a re-point was refused", true, APMF_API::kLeg_Failed);
                SetLegState(id, APMF_API::kLeg_Failed, a_form, a_point, MirroredSpeed(id), leg.ownerHandle);
            };

            // ABI v11: a Repoint to a POINT. The current destination (a ref, a cell, or
            // the previous marker) stays in force until Compose places the new marker and
            // re-points the package; Compose then deletes the old marker.
            if ((static_cast<std::uint32_t>(param.ival) & APMF_API::kTravel_ToPosition) != 0) {
                const RE::NiPoint3 pt{ param.posX, param.posY, param.posZ };
                if (param.form != 0 || !std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) {
                    refuse("kTravel_ToPosition needs form 0 and a finite point", param.form,
                           std::isfinite(pt.x) && std::isfinite(pt.y) && std::isfinite(pt.z) ? pt : RE::NiPoint3{});
                    return;
                }
                leg.toPosition = true;
                leg.point      = pt;
                leg.radius     = ClampRadius(id, param.fval);
                leg.flags      = static_cast<std::uint32_t>(param.ival);
                spdlog::info("[travel] 0x{} travel claim RE-POINTED -- destination 0x{} -> POINT {:.0f},{:.0f},{:.0f}.",
                             Hex(id), Hex(leg.destId), pt.x, pt.y, pt.z);
                SetLegStateFor(id, leg, APMF_API::kLeg_Pending, MirroredSpeed(id));
                apmf::mainthread::Post([id] { Compose(id, "re-point"); });
                return;
            }

            if (param.form == 0) {
                refuse("param.form is 0 (no destination)", 0, RE::NiPoint3{});
                return;
            }

            DestKind kind = DestKind::kRef;
            if (!ClassifyDestination(id, param.form, kind, "re-point")) {
                refuse("the destination is not a live reference or cell (logged above)", param.form, RE::NiPoint3{});
                return;
            }
            // Resolve the new reference BEFORE touching the leg, so a failure cannot leave it
            // half re-pointed (review F2).
            RE::ObjectRefHandle newHandle{};
            if (kind == DestKind::kRef) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(param.form);
                if (!ref) {
                    refuse("the destination reference no longer resolves", param.form, RE::NiPoint3{});
                    return;
                }
                newHandle = ref->CreateRefHandle();
            }

            const RE::FormID was = leg.destId;
            leg.toPosition = false;   // a form destination; any marker is deleted by Compose
            leg.destId     = param.form;
            leg.destKind   = kind;
            leg.destHandle = newHandle;
            leg.radius     = ClampRadius(id, param.fval);
            leg.flags      = static_cast<std::uint32_t>(param.ival);

            spdlog::info("[travel] 0x{} travel claim RE-POINTED -- destination 0x{} -> {} 0x{}.",
                         Hex(id), Hex(was), kind == DestKind::kCell ? "CELL" : "ref", Hex(leg.destId));
            SetLegStateFor(id, leg, APMF_API::kLeg_Pending, MirroredSpeed(id));

            apmf::mainthread::Post([id] { Compose(id, "re-point"); });
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            auto it = g_legs.find(id);
            if (it == g_legs.end()) {
                spdlog::info("[travel] 0x{} travel facet released (nothing was started).", Hex(id));
                SetLegState(id, APMF_API::kLeg_Released, 0, RE::NiPoint3{}, 0xFFFFFFFFu,
                            APMF_API::kInvalidHandle);   // ABI v12
                return;
            }
            SetLegStateFor(id, it->second, APMF_API::kLeg_Released, MirroredSpeed(id));   // ABI v12
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

            float            basis = 0.0f;
            APMF_API::Handle owner = APMF_API::kInvalidHandle;
            if (!StillOurs(id, leg, step, &basis, &owner)) return;
            leg.basis       = basis;
            leg.ownerHandle = owner;   // ABI v12: the claim this leg now belongs to

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
                    if (leg.legLive) EndLeg(id, leg, "the destination marker could not be placed", true, APMF_API::kLeg_Failed);
                    else SetLegStateFor(id, leg, APMF_API::kLeg_Failed, 0xFFFFFFFFu);
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
                    // ABI v12 (review F2): a live leg would otherwise keep walking to the OLD
                    // destination while the state reads Failed for the new one. End it.
                    if (leg.legLive) EndLeg(id, leg, "the re-pointed destination no longer resolves", true,
                                            APMF_API::kLeg_Failed);
                    else SetLegStateFor(id, leg, APMF_API::kLeg_Failed, MirroredSpeed(id));
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
                    EndLeg(id, leg, "the Location re-point was declined", true, APMF_API::kLeg_Failed);
                    return;
                }
                g_slotMarkerId[leg.slot] = (leg.markerId != 0 && leg.destId == leg.markerId) ? leg.markerId : 0;
                const std::uint32_t speed = ApplyGait(id, leg, leg.slot, true);   // ABI v12
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
                ResetBlockedClock(leg);                          // ABI v12: a new route, a new blocked clock
                SetLegStateFor(id, leg, APMF_API::kLeg_Walking, speed);
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

        // ABI v12 GAIT: remember each record's AUTHORED speed (see ApplyGait), and whether
        // this build is one the speed path was read on.
        for (std::size_t i = 0; i < kTravelSlots; ++i) {
            if (!g_pkg[i]) continue;
            g_authoredPrefSpeed[i] = g_pkg[i]->packData.packFlags.all(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);
            g_authoredSpeed[i]     = g_pkg[i]->packData.maxSpeed.underlying();
        }
        const auto game  = REL::Module::get().version();
        const bool exact = game == REL::Version{ 1, 6, 1170, 0 } || game == REL::Version{ 1, 5, 97, 0 };
        g_gaitVerified   = exact;
        spdlog::info("[travel-gait] {} -- authored record speed {} (flag 0x2000 {}). A leg with kTravel_SpeedSet "
                     "writes its own gait into its record before the package is offered.",
                     exact ? "gait ARMED" : "gait NOT armed (this build is not 1.6.1170 / 1.5.97; a declared gait is "
                                            "refused by name per leg)",
                     g_pkg[0] ? SpeedName(g_authoredSpeed[0]) : "?", g_pkg[0] && g_authoredPrefSpeed[0] ? "set" : "clear");

        // ABI v12 PASSIVE GATE PROBE: arms only when every engine address it calls verifies
        // on an exact build. A refusal leaves travel itself untouched.
        if (!exact) {
            spdlog::warn("[travel-gate] probe NOT armed -- runtime {} is not verified (only 1.6.1170 and 1.5.97 are).",
                         game.string("."));
        } else {
            bool verified = apmf::allowance::SeatVerified(
                REL::Relocation<std::uintptr_t>{ RELOCATION_ID(14180, 14288) }.address(), "Travel.GetOpenState");
            verified = apmf::allowance::SeatVerified(
                           REL::Relocation<std::uintptr_t>{ RELOCATION_ID(14108, 14298) }.address(),
                           "Travel.ScriptEventSourceHolder.GetSingleton") && verified;
            verified = apmf::allowance::SeatVerified(
                           REL::Relocation<std::uintptr_t>{ RELOCATION_ID(13177, 13322) }.address(), "Travel.TES.GetCell") &&
                       verified;
            auto* holder = verified ? RE::ScriptEventSourceHolder::GetSingleton() : nullptr;
            if (!verified) {
                spdlog::error("[travel-gate] probe NOT armed -- the mit-3.7 self-check refused an address it calls.");
            } else if (!holder) {
                spdlog::error("[travel-gate] probe NOT armed -- ScriptEventSourceHolder is unavailable at kDataLoaded.");
            } else {
                holder->AddEventSink<RE::TESOpenCloseEvent>(GateEventSink::GetSingleton());
                holder->AddEventSink<RE::TESActivateEvent>(GateEventSink::GetSingleton());
                g_probeReady.store(true, std::memory_order_release);
                spdlog::info("[travel-gate] probe ARMED (passive): a BLOCKED leg logs the DOOR/ACTI references within "
                             "{:.0f}u of the stall, then OPEN-CLOSE / ACTIVATE events near it for {} min.",
                             kProbeRadius, kWatchMs / 60000);

                // Probe 2 (passive, [Travel] bGateProbe2, default OFF since v0.9.10): arms only
                // with probe 1 (it reuses probe 1's verified GetOpenState / TES::GetCell) and
                // when the three vtables it identifies objects by verify on this exact build.
                if (GetPrivateProfileIntA("Travel", "bGateProbe2", 0, "Data/SKSE/Plugins/APMF.ini") == 0) {
                    spdlog::info("[travel-gate2] probe 2 NOT armed -- [Travel] bGateProbe2=0.");
                } else {
                    const auto vtA = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BShkbAnimationGraph[0] }.address();
                    const auto vtB = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbBehaviorGraph[0] }.address();
                    const auto vtS = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbStateMachine[0] }.address();
                    bool v2 = apmf::allowance::SeatVerified(vtA, "Travel.Probe2.BShkbAnimationGraph");
                    v2      = apmf::allowance::SeatVerified(vtB, "Travel.Probe2.hkbBehaviorGraph") && v2;
                    v2      = apmf::allowance::SeatVerified(vtS, "Travel.Probe2.hkbStateMachine") && v2;
                    if (!v2) {
                        spdlog::error("[travel-gate2] probe 2 NOT armed -- the mit-3.7 self-check refused a vtable it "
                                      "identifies objects by.");
                    } else {
                        g_vtAnimGraph     = vtA;
                        g_vtBehaviorGraph = vtB;
                        g_vtStateMachine  = vtS;
                        g_probe2Ready.store(true, std::memory_order_release);
                        spdlog::info("[travel-gate2] probe 2 ARMED (passive): a BLOCKED leg logs the navmesh under the "
                                     "stall and the destination, and the door/activator/obstacle references near the "
                                     "stall->destination segment with their behaviour-graph state and Havok collision.");
                    }
                }
            }
        }

        g_installed.store(true, std::memory_order_release);
        spdlog::info("[travel] installed -- {} of {} travel packages resolved from {}. "
                     "Arrival radius default {}u (client range {}-{}u), poll {} ms, leg safety net {} ms. "
                     "A leg ends on ARRIVAL, on the ACTOR ENTERING COMBAT, on the destination being "
                     "gone, or BLOCKED (the engine's Movement Blocked package held for {} ms) -- and nothing "
                     "else. No perception test of any kind, and no other intent is claimed on the client's "
                     "behalf.",
                     resolved, kTravelSlots, kPlugin,
                     static_cast<std::uint32_t>(kDefaultRadiusUnits),
                     static_cast<std::uint32_t>(kMinRadiusUnits),
                     static_cast<std::uint32_t>(kMaxRadiusUnits),
                     kPollPeriodMs, kLegMaxMs, kBlockedEndMs);
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
        // ABI v12: the leg-state mirror and the gate-probe watches describe the world being
        // replaced. The package records keep whatever gait their last leg wrote; the next
        // leg on each record writes its own (or the authored one) before it is offered.
        {
            std::lock_guard lk(g_stateMx);
            g_state.clear();
        }
        {
            std::lock_guard lk(g_watchMx);
            g_watches.clear();
            g_watchCount.store(0, std::memory_order_relaxed);
        }
        g_lastProbe.clear();
        g_lastProbe2.clear();
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

    std::uint32_t GetLegState(RE::FormID actor, APMF_API::APMF_TravelLegInfo* out) {
        LegStateRec r{};
        bool        found = false;
        {
            std::lock_guard lk(g_stateMx);
            if (auto it = g_state.find(actor); it != g_state.end()) {
                r     = it->second;
                found = true;
            }
        }
        const std::uint32_t state = found ? r.state : static_cast<std::uint32_t>(APMF_API::kLeg_None);
        // THE SIZE RULE (review R2-1): test against the FROZEN v12 prefix size, never against
        // sizeof(APMF_TravelLegInfo), which grows when a later ABI appends a field and would
        // then starve every shipped v12 client. A field appended later is written only when
        // it lies entirely inside `out->size` (offsetof(field) + sizeof(field) <= size).
        if (out && out->size >= APMF_API::kTravelLegInfoV12Size) {
            const std::uint64_t age = found ? apmf::clock::MonotonicMs() - r.sinceMs : 0;
            out->state     = state;
            out->actor     = actor;
            out->destForm  = r.destForm;
            out->destX     = r.point.x;
            out->destY     = r.point.y;
            out->destZ     = r.point.z;
            out->msInState = age > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(age);
            out->seq       = r.seq;
            out->stallX    = r.stall.x;
            out->stallY    = r.stall.y;
            out->stallZ    = r.stall.z;
            out->blockedMs = r.blockedMs;
            out->speed     = found ? r.speed : 0xFFFFFFFFu;
            out->reserved  = 0;
            out->ownerHandle = r.owner;
            out->blocker     = r.blocker;
            out->blockerKind = r.blockerKind;
        }
        return state;
    }

}

APMF_REGISTER_CHANNEL(TravelChannel);
