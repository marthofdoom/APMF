#include "PCH.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PackageData.h"
#include "core/Registry.h"
#include "channels/Travel.h"

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
// A client claims kIntent_Travel naming a DESTINATION REFERENCE. APMF walks the
// claimed actor to within ~75u of it and lets go the moment the actor is in
// combat. That is the entire feature.
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
// RefileSubClaim), and a client never sees it.
//
// ---------------------------------------------------------------------------
// WHAT IT DOES, AND THE SHORT LIST OF WHAT IT DOES NOT
// ---------------------------------------------------------------------------
// DOES: point an APMF-OWNED Travel package (Data/APMF.esl, one record per
// concurrent leg) at the destination reference via its "Place to Travel" Location
// input (core/PackageData.cpp), offer it through ch.9's proven 0x49 redirect + the
// posted EvaluatePackage nudge, and END the leg on ARRIVAL, on the ACTOR ENTERING
// COMBAT, or on the destination going away.
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
    bool                     g_observeOnly = true;
    RE::TESPackage*          g_pkg[kTravelSlots]{};

    // ---- Per-leg state. GAME THREAD ONLY (see the threading note). ----
    struct Leg {
        RE::ActorHandle       actorHandle{};
        RE::FormID            destId = 0;
        RE::ObjectRefHandle   destHandle{};   // a REFERENCE, not necessarily an actor
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
    };

    std::unordered_map<RE::FormID, Leg> g_legs;
    RE::FormID                          g_slotOwner[kTravelSlots]{};   // 0 == free
    std::atomic<std::uint32_t>          g_legCount{ 0 };               // Poll's relaxed pre-gate
    std::uint64_t                       g_lastPollMs = 0;

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

    // Point this leg's package slot at the destination and offer it through ch.9.
    // Returns false (having offered nothing) if anything in the chain declines --
    // the failure is LOGGED, never masked with a retry or a fallback.
    bool StartLeg(RE::FormID a_id, Leg& a_leg, RE::TESObjectREFR* a_dest) {
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

        if (!apmf::packagedata::SetTravelTarget(pkg, a_dest, a_leg.radius)) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- could not point package 0x{}'s Location at "
                          "destination 0x{} (core/PackageData.cpp declined; see the [pkgdata] line above "
                          "for which guard failed). NOTHING was offered: an unpointed travel package "
                          "would walk the actor to the placeholder ref.",
                          Hex(a_id), Hex(pkg->GetFormID()), Hex(a_leg.destId));
            FreeSlot(slot);
            return false;
        }

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
        if (!claimed || now.form != a_leg.destId) {
            spdlog::info("[travel] 0x{} {} DROPPED (stale) -- posted for destination 0x{}, now claim={} "
                         "destination 0x{}; a newer claim owns this edge.",
                         Hex(a_id), a_step, Hex(a_leg.destId), claimed, Hex(now.form));
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
            if (param.form == 0) {
                spdlog::error("[travel] 0x{} engage IGNORED -- param.form is 0 (no destination).", Hex(id));
                return;
            }

            Leg leg{};
            leg.destId = param.form;
            leg.radius = ClampRadius(id, param.fval);
            leg.flags  = static_cast<std::uint32_t>(param.ival);
            if (actor && actor->IsHandleValid()) leg.actorHandle = actor->GetHandle();

            // ANY LOADED REFERENCE is a legal destination -- an actor is just the
            // common case. The Location write needs a ref handle and nothing more, so
            // no reference-type restriction is imposed beyond that.
            auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(leg.destId);
            if (!dest) {
                spdlog::error("[travel] 0x{} engage IGNORED -- destination 0x{} is not a live object "
                              "reference.", Hex(id), Hex(leg.destId));
                return;
            }
            leg.destHandle = dest->CreateRefHandle();

            g_legs[id] = leg;
            g_legCount.store(static_cast<std::uint32_t>(g_legs.size()), std::memory_order_relaxed);

            spdlog::info("[travel] 0x{} travel facet CLAIMED -- destination 0x{} '{}', radius {}, "
                         "flags 0x{}.",
                         Hex(id), Hex(leg.destId), dest->GetName() ? dest->GetName() : "?",
                         static_cast<std::uint32_t>(leg.radius), Hex(leg.flags, 2));

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

            if (param.form == 0) {
                spdlog::error("[travel] 0x{} re-point IGNORED -- param.form is 0 (no destination).",
                              Hex(id));
                return;
            }

            auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(param.form);
            if (!dest) {
                spdlog::error("[travel] 0x{} re-point IGNORED -- destination 0x{} is not a live object "
                              "reference; the previous destination 0x{} stands.",
                              Hex(id), Hex(param.form), Hex(leg.destId));
                return;
            }

            const RE::FormID was = leg.destId;
            leg.destId     = param.form;
            leg.destHandle = dest->CreateRefHandle();
            leg.radius     = ClampRadius(id, param.fval);
            leg.flags      = static_cast<std::uint32_t>(param.ival);

            spdlog::info("[travel] 0x{} travel claim RE-POINTED -- destination 0x{} -> 0x{} '{}'.",
                         Hex(id), Hex(was), Hex(leg.destId), dest->GetName() ? dest->GetName() : "?");

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
            apmf::mainthread::Post([leg] { DropOfferAndFreeSlot(leg.hPackage, leg.slot); });
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

            RE::NiPointer<RE::TESObjectREFR> destPtr = leg.destHandle.get();   // keeps it alive below
            RE::TESObjectREFR*               dest    = destPtr.get();
            if (!dest) {
                spdlog::error("[travel] 0x{} {} DROPPED -- destination 0x{} no longer resolves (unloaded "
                              "or deleted).", Hex(id), step, Hex(leg.destId));
                return;
            }

            if (g_observeOnly) {
                spdlog::info("[travel-observe] 0x{} {} -- WOULD walk to destination 0x{} at radius {}, "
                             "offering an APMF travel package at basis {}. Nothing claimed, nothing "
                             "written: [Travel] bTravelObserveOnly=1.",
                             Hex(id), step, Hex(leg.destId),
                             static_cast<std::uint32_t>(leg.radius), basis);
                return;
            }

            // A live leg is RE-POINTED in place (rewrite the record's Location, then
            // either re-file the offer at a moved basis or re-point it so its nudge
            // fires again); otherwise start a fresh one.
            if (leg.legLive && leg.slot >= 0 && g_pkg[leg.slot]) {
                if (!apmf::packagedata::SetTravelTarget(g_pkg[leg.slot], dest, leg.radius)) {
                    spdlog::error("[travel-leg] 0x{} re-point FAILED -- could not re-point package 0x{}'s "
                                  "Location; ENDING the leg rather than leaving the actor walking at the "
                                  "old destination.",
                                  Hex(id), Hex(g_pkg[leg.slot]->GetFormID()));
                    EndLeg(id, leg, "the Location re-point was declined", true);
                    return;
                }
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
                StartLeg(id, leg, dest);
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

        g_observeOnly = GetPrivateProfileIntA("Travel", "bTravelObserveOnly", 1,
                                              "Data/SKSE/Plugins/APMF.ini") != 0;

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
        spdlog::info("[travel] installed -- {} of {} travel packages resolved from {}; mode = {}. "
                     "Arrival radius default {}u (client range {}-{}u), poll {} ms, leg safety net {} ms. "
                     "A leg ends on ARRIVAL, on the ACTOR ENTERING COMBAT, or on the destination being "
                     "gone -- and nothing else. No perception test of any kind, and no other intent is "
                     "claimed on the client's behalf.",
                     resolved, kTravelSlots, kPlugin,
                     g_observeOnly ? "OBSERVE-ONLY (bTravelObserveOnly=1: logs what it would do, claims "
                                     "nothing, writes nothing)"
                                   : "ACTIVE",
                     static_cast<std::uint32_t>(kDefaultRadiusUnits),
                     static_cast<std::uint32_t>(kMinRadiusUnits),
                     static_cast<std::uint32_t>(kMaxRadiusUnits),
                     kPollPeriodMs, kLegMaxMs);
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_notInstalledReason.load(std::memory_order_acquire); }

    void ResetAll(const char* why) {
        if (g_legs.empty()) {
            for (auto& o : g_slotOwner) o = 0;
            return;
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

            // The DESTINATION. Note IsDisabled() reads the ref's kInitiallyDisabled
            // form flag, which is the SAME flag the engine's runtime Disable() sets --
            // so it covers a scripted despawn, not only an editor-disabled ref.
            auto  dptr = leg.destHandle.get();
            auto* d    = dptr.get();
            if (!d) {
                EndLeg(id, leg, "the destination no longer resolves (unloaded or deleted)", false);
                continue;
            }
            if (d->IsDisabled()) {
                EndLeg(id, leg, "the destination was disabled", false);
                continue;
            }
            if (d->IsDead()) {
                EndLeg(id, leg, "the destination is dead", false);
                continue;
            }
            if (!d->Is3DLoaded()) {
                EndLeg(id, leg, "the destination's 3D is not loaded", false);
                continue;
            }

            // ARRIVED. The same radius this leg wrote into the package record, so the
            // engine's own stop and this test fire at the same distance.
            const float dist = a->GetPosition().GetDistance(d->GetPosition());
            if (dist <= leg.radius) {
                EndLeg(id, leg, "ARRIVED (inside the arrival radius)", false);
                continue;
            }

            // The safety net, last: everything above is a legitimate end, this is a
            // failure report.
            if (now - leg.legStartedMs > kLegMaxMs) {
                EndLeg(id, leg, "STUCK -- no arrival, no combat, and the destination is still there "
                                "after the leg safety net elapsed (unreachable destination, blocked "
                                "path, or an outranking package is winning)", true);
            }
        }
    }

}

APMF_REGISTER_CHANNEL(TravelChannel);
