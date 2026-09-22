#include "PCH.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PackageData.h"
#include "core/Registry.h"
#include "channels/CombatEngage.h"

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) --
// the same one-line import core/Input.cpp, core/ActionGate.cpp and
// core/NonAliasProbe.cpp already use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 19 -- COMBAT ENGAGE (kIntent_CombatEngage, ABI v10, marth 2026-09-22).
//
// WHAT PROBLEM THIS SOLVES. A third-party modder built a "hotkey sends my
// teammates at that enemy" plugin against ABI v9 and gave up, because at v9 the
// public API cannot express "go attack this actor now": ch.6 is arbitration only,
// ch.9 offers a package the CLIENT has to ship, and the approach leg + combat
// entry existed only inside MFO. Their plugin ended up re-pushing StartCombat
// every 3 s at foes the actor had never detected, which is the engine's SEARCH
// behaviour, not a charge. ch.19 is the missing verb: the client passes an actor
// and a target, and APMF does the rest.
//
// ---------------------------------------------------------------------------
// WHAT v1 IS -- THE TRAVEL LEG, AND ONLY THE TRAVEL LEG (marth's scope cut,
// 2026-09-22: "for now lets try having it just travel to the enemy")
// ---------------------------------------------------------------------------
// A ch.19 claim composes exactly two things, BOTH of which already existed:
//
//   (1) an internal ch.6 `kIntent_CombatTarget` claim for the same actor, at the
//       SAME basis the client asked for -- so APMF is still the single arbiter of
//       who owns the combat-target facet, and a second client bidding higher
//       still wins it. ch.6 is arbitration-only; it makes no engine call.
//
//   (2) an internal ch.9 `kIntent_OfferPackage` claim, also at that basis, naming
//       an APMF-OWNED Travel package (Data/APMF.esl, one record per concurrent
//       engagement slot) whose "Place to Travel" Location input this channel
//       first points at the target ref (core/PackageData.cpp). The existing 0x49
//       redirect hands that package to the actor and the existing posted
//       EvaluatePackage nudge makes it take within one evaluation.
//
// The approach then ENDS -- the ch.9 sub-claim is released -- as soon as the
// actor has arrived, can see the target, has detected the target, or the target
// is gone. From that instant the actor behaves EXACTLY as vanilla would, which
// for a hostile foe in sight is the engine's own combat AI. That hand-off IS the
// v1 design, not a shortfall of it.
//
// DELIBERATELY ABSENT IN v1, and both were in the original design:
//   * NO `StartCombat`. `Docs/INVARIANTS.md #0` forbids a channel calling it and
//     v1 does not need it, so **#0 is untouched by this branch.** Combat entry is
//     left to the engine (or to the client). If the field cycle shows the engine
//     does NOT pick the fight up on arrival, THAT is the evidence that justifies
//     amending #0 for a bounded entry call -- earning the amendment instead of
//     assuming it.
//   * NO `Character::UpdateCombat` (0xE4) target PIN. No new engine seat exists in
//     this branch at all: ch.19 rides the 0x49 seat ch.9 already owns and the
//     0xAD PlayerCharacter seat Arbiter::OncePerFrame already runs on.
//
// ---------------------------------------------------------------------------
// THREADING -- every line in this file is on the confirmed-main seat
// ---------------------------------------------------------------------------
// `Engage`/`OnOwnerChanged`/`Release` run inside `ControlMap::Drain`'s apply loop,
// which runs on the PlayerCharacter 0xAD seat. `Poll()` runs on that same seat,
// later in the same `Arbiter::OncePerFrame`. Every deferred step goes through
// `mainthread::Post`, whose `Pump()` is also that seat. So `g_engaged` and
// `g_slotOwner` are touched by exactly one thread and need no lock.
//
// `Channel::Tick` is deliberately NOT overridden. Tick runs from the *Character*
// 0xAD seat, which is field-proven multi-threaded (core/Hook.cpp's
// [threadcheck]), and the approach monitor resolves actor handles and asks the
// engine for line-of-sight and detection -- exactly the class of engine read this
// project has been burned by off-thread. The monitor lives on OncePerFrame
// instead. It is also NOT a re-assert (Channel.h: "a re-assert loop is a FAILED
// block"): it never re-offers anything, it only decides when an approach is OVER.
//
// ---------------------------------------------------------------------------
// ORDERING -- why every ControlMap write is POSTED
// ---------------------------------------------------------------------------
// Channel.h forbids writing to the ControlMap from a lifecycle call, because
// those calls run BEFORE `Drain` publishes the new snapshot. This channel's whole
// job is to make two more claims, so every one of them is posted through
// `mainthread::Post` and runs one hop PAST that Publish -- the same idiom, for
// the same reason, that ch.9's own nudge uses (INVARIANTS #20). Each posted task
// RE-VALIDATES the ch.19 claim it was posted for against the now-published
// snapshot and drops itself if the state moved.
// ============================================================================

namespace {

    using apmf::log::Hex;

    // ---- Shipped plugin + its FROZEN FormID band (APMF_GenerateESL.py) ----
    constexpr const char* kPlugin        = "APMF.esl";
    constexpr RE::FormID  kEngagePkgBase = 0x800;   // APMF_EngageTravelPackage0
    // One package record per CONCURRENT engagement. The Location input lives on
    // the RECORD, not on the actor, so two actors approaching two different
    // targets need two records -- this is the same reason MFO ships four
    // loot-travel records. Eight is generous for a party-scale client and costs 8
    // PACK records in a 2.5 KB plugin; past it the approach leg is REFUSED and
    // logged loudly (never silently shared, which would send both actors to one
    // destination).
    constexpr std::size_t kEngageSlots = 8;

    // The default arrival radius, matching the radius the ESL authors into every
    // record. Keeping the two equal is load-bearing: the engine stops the actor at
    // the PACKAGE's radius, so if APMF's own arrival test used a different number,
    // "the engine thinks it arrived" and "APMF thinks it arrived" could disagree
    // forever. A client that passes its own fval gets it written into the record's
    // Location as well, so they stay equal per engagement.
    constexpr float kDefaultRadiusUnits = 128.0f;

    // How often the approach monitor actually evaluates. Line-of-sight and
    // detection are real engine queries; at most kEngageSlots of them, four times
    // a second, is cheap and far finer than a travel leg needs.
    constexpr std::uint64_t kPollPeriodMs = 250;

    // A SAFETY NET, not a feature budget (CLAUDE.md principle 9: size a deadline
    // from the real cadence, never from a guess). An approach that has not
    // arrived, gained line of sight, detected the target, or lost the target in
    // two minutes is STUCK -- unreachable target, blocked navmesh, an outranking
    // package we are losing. Two minutes is far longer than any legitimate
    // cross-cell travel we expect to see, so it cannot cut a live approach short.
    // When it fires the approach is dropped and the reason is logged as a FAILURE,
    // loudly and by name -- it is not retried and it is not hidden (principle 7).
    constexpr std::uint64_t kApproachMaxMs = 120000;

    // ---- Install state (written once at kDataLoaded, read from any thread) ----
    std::atomic<bool>        g_installed{ false };
    // Atomic for the same reason core/EquipSink.cpp's twin is: ControlMap's
    // synchronous refusal reads it from whatever thread the client called
    // RequestEx on, while Install writes it on the game thread.
    std::atomic<const char*> g_notInstalledReason{ "not yet initialized (before kDataLoaded)" };
    bool                     g_observeOnly = true;
    RE::TESPackage*          g_pkg[kEngageSlots]{};

    // ---- Per-engagement state. GAME THREAD ONLY (see the threading note). ----
    struct Engagement {
        RE::ActorHandle   actorHandle{};
        RE::FormID        targetId   = 0;
        RE::ActorHandle   targetHandle{};
        float             radius     = kDefaultRadiusUnits;
        std::uint32_t     flags      = 0;
        float             basis      = 0.0f;
        int               slot       = -1;                          // -1 == no package slot held
        APMF_API::Handle  hTarget    = APMF_API::kInvalidHandle;    // the ch.6 sub-claim
        APMF_API::Handle  hPackage   = APMF_API::kInvalidHandle;    // the ch.9 sub-claim
        bool              approachLive = false;
        std::uint64_t     approachStartedMs = 0;
    };

    std::unordered_map<RE::FormID, Engagement> g_engaged;
    RE::FormID                                 g_slotOwner[kEngageSlots]{};   // 0 == free
    std::atomic<std::uint32_t>                 g_engagedCount{ 0 };           // Poll's relaxed pre-gate
    std::uint64_t                              g_lastPollMs = 0;

    // ---- Slots ----------------------------------------------------------------

    int AcquireSlot(RE::FormID a_actor) {
        for (std::size_t i = 0; i < kEngageSlots; ++i) {
            if (g_slotOwner[i] == 0) {
                g_slotOwner[i] = a_actor;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void FreeSlot(int a_slot) {
        if (a_slot >= 0 && static_cast<std::size_t>(a_slot) < kEngageSlots) g_slotOwner[a_slot] = 0;
    }

    // ---- The composition -------------------------------------------------------

    // Release the ch.9 sub-claim and free the package slot. The ch.6 sub-claim and
    // the client's own ch.19 claim are NOT touched: APMF never revokes a claim the
    // client still holds -- it only ends the work it started.
    void EndApproach(RE::FormID a_id, Engagement& a_e, const char* a_why, bool a_failure) {
        if (!a_e.approachLive) return;
        a_e.approachLive = false;

        if (a_e.hPackage != APMF_API::kInvalidHandle) {
            apmf::ControlMap::Get().EnqueueRelease(a_e.hPackage);
            a_e.hPackage = APMF_API::kInvalidHandle;
        }
        FreeSlot(a_e.slot);
        a_e.slot = -1;

        const auto elapsed = apmf::clock::MonotonicMs() - a_e.approachStartedMs;
        if (a_failure) {
            spdlog::error("[ch.19-approach] 0x{} approach ABANDONED after {} ms -- {}. The ch.19 claim "
                          "still stands and is now doing NOTHING; the client should Release it or "
                          "Repoint to a reachable target.",
                          Hex(a_id), elapsed, a_why);
        } else {
            spdlog::info("[ch.19-approach] 0x{} approach ENDED after {} ms -- {}. The engine's own AI "
                         "has the actor from here.", Hex(a_id), elapsed, a_why);
        }
    }

    // Point this engagement's package slot at the target and offer it through ch.9.
    // Returns false (having offered nothing) if anything in the chain declines --
    // the failure is LOGGED, never masked with a retry or a fallback.
    bool StartApproach(RE::FormID a_id, Engagement& a_e, RE::Actor* a_target) {
        if (a_e.flags & APMF_API::kEngage_NoApproach) {
            spdlog::info("[ch.19-approach] 0x{} no approach -- the client set kEngage_NoApproach.",
                         Hex(a_id));
            return false;
        }

        const int slot = AcquireSlot(a_id);
        if (slot < 0) {
            spdlog::error("[ch.19-approach] 0x{} approach REFUSED -- all {} package slots are in use by "
                          "other engagements. A package's destination lives on the RECORD, so two "
                          "approaches cannot share one; this actor gets the ch.6 claim and no travel.",
                          Hex(a_id), kEngageSlots);
            return false;
        }

        RE::TESPackage* pkg = g_pkg[slot];
        if (!pkg) {
            spdlog::error("[ch.19-approach] 0x{} approach REFUSED -- slot {} has no package (0x{} in {} "
                          "did not resolve).", Hex(a_id), slot, Hex(kEngagePkgBase + slot, 3), kPlugin);
            FreeSlot(slot);
            return false;
        }

        if (!apmf::packagedata::SetTravelTarget(pkg, a_target, a_e.radius)) {
            spdlog::error("[ch.19-approach] 0x{} approach REFUSED -- could not point package 0x{}'s "
                          "Location at target 0x{} (core/PackageData.cpp declined; see the [pkgdata] "
                          "line above for which guard failed). NOTHING was offered: an unpointed "
                          "travel package would walk the actor to the placeholder ref.",
                          Hex(a_id), Hex(pkg->GetFormID()), Hex(a_e.targetId));
            FreeSlot(slot);
            return false;
        }

        APMF_API::APMF_Param p9{};
        p9.form = pkg->GetFormID();
        const APMF_API::Handle h =
            apmf::ControlMap::Get().EnqueueRequest(a_id, APMF_API::kIntent_OfferPackage, a_e.basis, &p9);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::error("[ch.19-approach] 0x{} approach REFUSED -- the ch.9 sub-claim was refused "
                          "(ControlMap logged why).", Hex(a_id));
            FreeSlot(slot);
            return false;
        }

        a_e.slot              = slot;
        a_e.hPackage          = h;
        a_e.approachLive      = true;
        a_e.approachStartedMs = apmf::clock::MonotonicMs();
        spdlog::info("[ch.19-approach] 0x{} approach STARTED -- slot {}, package 0x{} -> target 0x{}, "
                     "radius {}, ch.9 handle {} at basis {}. ch.9 posts its own EvaluatePackage nudge.",
                     Hex(a_id), slot, Hex(p9.form), Hex(a_e.targetId),
                     static_cast<std::uint32_t>(a_e.radius), h, a_e.basis);
        return true;
    }

    // Re-read the published ch.19 claim and confirm this deferred step is still the
    // one the world wants. A posted task outlives the moment it was posted for: the
    // claim may have been released, replaced by a higher-basis one, or re-pointed
    // at another target before Pump ran.
    bool StillOurs(RE::FormID a_id, const Engagement& a_e, const char* a_step, float* a_outBasis) {
        APMF_API::APMF_Param now{};
        float                basis = 0.0f;
        const bool claimed = apmf::ControlMap::Get().TryGetOwningClaimBasis(
            a_id, APMF_API::kIntent_CombatEngage, now, basis);
        if (!claimed || now.form != a_e.targetId) {
            spdlog::info("[ch.19] 0x{} {} DROPPED (stale) -- posted for target 0x{}, now claim={} "
                         "target 0x{}; a newer claim owns this edge.",
                         Hex(a_id), a_step, Hex(a_e.targetId), claimed, Hex(now.form));
            return false;
        }
        if (a_outBasis) *a_outBasis = basis;
        return true;
    }

    // Resolve the target actor for an engagement, on the game thread. Returns the
    // NiPointer, NOT a raw pointer: the handle's get() hands back a REFCOUNTED
    // smart pointer, and returning the raw pointer out of it would drop that
    // reference at the return statement and leave the caller holding a pointer
    // whose only owner just went away.
    RE::NiPointer<RE::Actor> ResolveTarget(const Engagement& a_e) {
        return a_e.targetHandle.get();
    }

    // ---- The channel -----------------------------------------------------------

    class CombatEngageChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-engage"; }
        int              ChannelNo() const override { return 19; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatEngage; }

        // NO test-surface hotkey. The crosshair test surface can only name ONE
        // actor, and ch.19 needs an actor AND a target; a hotkey that engaged the
        // aimed NPC against a target APMF picked would be APMF inventing intent
        // (CLAUDE.md principle 4). Drive it from the C-ABI.

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            // ControlMap::EnqueueRequest already refused a claim with no target and a
            // claim made while the channel is down, so reaching here means both held
            // at request time. Re-test the target anyway -- the request was enqueued
            // on some other thread, possibly frames ago.
            if (param.form == 0) {
                spdlog::error("[ch.19] 0x{} engage IGNORED -- param.form is 0 (no target).", Hex(id));
                return;
            }

            Engagement e{};
            e.targetId = param.form;
            e.radius   = (param.fval > 0.0f) ? param.fval : kDefaultRadiusUnits;
            e.flags    = static_cast<std::uint32_t>(param.ival);
            if (actor && actor->IsHandleValid()) e.actorHandle = actor->GetHandle();

            auto* target = RE::TESForm::LookupByID<RE::Actor>(e.targetId);
            if (!target) {
                spdlog::error("[ch.19] 0x{} engage IGNORED -- target 0x{} is not a live Actor form.",
                              Hex(id), Hex(e.targetId));
                return;
            }
            e.targetHandle = target->CreateRefHandle();

            g_engaged[id] = e;
            g_engagedCount.store(static_cast<std::uint32_t>(g_engaged.size()), std::memory_order_relaxed);

            spdlog::info("[ch.19] 0x{} combat-engage CLAIMED -- target 0x{} '{}', radius {}, flags 0x{}.",
                         Hex(id), Hex(e.targetId), target->GetName() ? target->GetName() : "?",
                         static_cast<std::uint32_t>(e.radius), Hex(e.flags, 2));

            // Compose one hop past this Drain's Publish (see the ordering note).
            apmf::mainthread::Post([id] { Compose(id, "engage"); });
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            auto it = g_engaged.find(id);
            if (it == g_engaged.end()) {
                // A winning claim arrived on a channel that was already engaged but
                // whose state we never built (a refused engage). Treat it as a fresh
                // engage rather than half-applying a re-point.
                Engage(id, actor, param);
                return;
            }
            Engagement& e = it->second;

            if (param.form == 0) {
                spdlog::error("[ch.19] 0x{} re-point IGNORED -- param.form is 0 (no target).", Hex(id));
                return;
            }

            auto* target = RE::TESForm::LookupByID<RE::Actor>(param.form);
            if (!target) {
                spdlog::error("[ch.19] 0x{} re-point IGNORED -- target 0x{} is not a live Actor form; "
                              "the previous target 0x{} stands.",
                              Hex(id), Hex(param.form), Hex(e.targetId));
                return;
            }

            const RE::FormID was = e.targetId;
            e.targetId     = param.form;
            e.targetHandle = target->CreateRefHandle();
            e.radius       = (param.fval > 0.0f) ? param.fval : kDefaultRadiusUnits;
            e.flags        = static_cast<std::uint32_t>(param.ival);

            spdlog::info("[ch.19] 0x{} combat-engage RE-POINTED -- target 0x{} -> 0x{} '{}'.",
                         Hex(id), Hex(was), Hex(e.targetId),
                         target->GetName() ? target->GetName() : "?");

            apmf::mainthread::Post([id] { Compose(id, "re-point"); });
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            auto it = g_engaged.find(id);
            if (it == g_engaged.end()) {
                spdlog::info("[ch.19] 0x{} combat-engage released (nothing was composed).", Hex(id));
                return;
            }
            const Engagement e = it->second;   // by VALUE: the entry goes away now
            g_engaged.erase(it);
            g_engagedCount.store(static_cast<std::uint32_t>(g_engaged.size()), std::memory_order_relaxed);

            spdlog::info("[ch.19] 0x{} combat-engage RELEASED -- dropping the ch.6 and ch.9 sub-claims.",
                         Hex(id));

            // POSTED, like every other ControlMap write here: this Release runs inside
            // Drain's apply loop, before Publish. The slot stays OWNED until the
            // posted teardown runs, so a release-then-re-request inside ONE Drain
            // cannot hand the new engagement the very record the outgoing ch.9 claim
            // is still offering (there are kEngageSlots of them; it gets another).
            apmf::mainthread::Post([e] {
                auto& map = apmf::ControlMap::Get();
                if (e.hPackage != APMF_API::kInvalidHandle) map.EnqueueRelease(e.hPackage);
                if (e.hTarget  != APMF_API::kInvalidHandle) map.EnqueueRelease(e.hTarget);
                FreeSlot(e.slot);
            });
        }

    private:
        // Build (or rebuild) the composition for `id`. Runs on the confirmed-main
        // seat, one hop past the Drain that engaged/re-pointed the claim.
        static void Compose(RE::FormID id, const char* step) {
            auto it = g_engaged.find(id);
            if (it == g_engaged.end()) return;   // released before this ran
            Engagement& e = it->second;

            float basis = 0.0f;
            if (!StillOurs(id, e, step, &basis)) return;
            e.basis = basis;

            RE::NiPointer<RE::Actor> targetPtr = ResolveTarget(e);   // keeps the target alive below
            RE::Actor*               target    = targetPtr.get();
            if (!target) {
                spdlog::error("[ch.19] 0x{} {} DROPPED -- target 0x{} no longer resolves (unloaded or "
                              "deleted).", Hex(id), step, Hex(e.targetId));
                return;
            }

            if (g_observeOnly) {
                spdlog::info("[ch.19-observe] 0x{} {} -- WOULD claim ch.6 combat-target on 0x{} and "
                             "ch.9 package 0x{} (slot would be allocated) at basis {}, Location -> "
                             "0x{} radius {}. Nothing claimed, nothing written: [CombatEngage] "
                             "bEngageObserveOnly=1.",
                             Hex(id), step, Hex(e.targetId),
                             Hex(g_pkg[0] ? g_pkg[0]->GetFormID() : 0), basis,
                             Hex(e.targetId), static_cast<std::uint32_t>(e.radius));
                return;
            }

            // (1) the ch.6 combat-target claim -- made once, then re-pointed.
            APMF_API::APMF_Param p6{};
            p6.form = e.targetId;
            if (e.hTarget == APMF_API::kInvalidHandle) {
                e.hTarget = apmf::ControlMap::Get().EnqueueRequest(
                    id, APMF_API::kIntent_CombatTarget, basis, &p6);
                if (e.hTarget == APMF_API::kInvalidHandle) {
                    spdlog::error("[ch.19] 0x{} ch.6 sub-claim REFUSED (ControlMap logged why). The "
                                  "approach still runs; APMF is simply not recorded as owning the "
                                  "combat-target facet.", Hex(id));
                } else {
                    spdlog::info("[ch.19] 0x{} ch.6 sub-claim h={} at basis {} -> target 0x{}.",
                                 Hex(id), e.hTarget, basis, Hex(e.targetId));
                }
            } else {
                apmf::ControlMap::Get().EnqueueRepoint(e.hTarget, &p6);
                spdlog::info("[ch.19] 0x{} ch.6 sub-claim h={} re-pointed -> target 0x{}.",
                             Hex(id), e.hTarget, Hex(e.targetId));
            }

            // (2) the approach. A live approach is RE-POINTED in place (rewrite the
            // record's Location, re-point the ch.9 claim so its nudge fires again);
            // otherwise start a fresh one.
            if (e.approachLive && e.slot >= 0 && g_pkg[e.slot]) {
                if (!apmf::packagedata::SetTravelTarget(g_pkg[e.slot], target, e.radius)) {
                    spdlog::error("[ch.19-approach] 0x{} re-point FAILED -- could not re-point package "
                                  "0x{}'s Location; ENDING the approach rather than leaving the actor "
                                  "walking at the old target.",
                                  Hex(id), Hex(g_pkg[e.slot]->GetFormID()));
                    EndApproach(id, e, "the Location re-point was declined", true);
                    return;
                }
                APMF_API::APMF_Param p9{};
                p9.form = g_pkg[e.slot]->GetFormID();
                apmf::ControlMap::Get().EnqueueRepoint(e.hPackage, &p9);
                e.approachStartedMs = apmf::clock::MonotonicMs();   // a new leg, a new deadline
                spdlog::info("[ch.19-approach] 0x{} approach RE-POINTED -- slot {}, package 0x{} -> "
                             "target 0x{}.", Hex(id), e.slot, Hex(p9.form), Hex(e.targetId));
            } else {
                StartApproach(id, e, target);
            }
        }
    };

}

namespace apmf::combatengage {

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;

        if (REL::Module::IsVR()) {
            g_notInstalledReason.store("VR runtime (ch.9's 0x49 seat is SE/AE only, so the approach has no "
                                   "delivery mechanism)", std::memory_order_release);
            spdlog::warn("[ch.19] NOT installed -- {}.", g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        if (GetPrivateProfileIntA("CombatEngage", "bCombatEngage", 1,
                                  "Data/SKSE/Plugins/APMF.ini") == 0) {
            g_notInstalledReason.store("[CombatEngage] bCombatEngage=0 in Data/SKSE/Plugins/APMF.ini", std::memory_order_release);
            spdlog::info("[ch.19] NOT installed -- {}.", g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        g_observeOnly = GetPrivateProfileIntA("CombatEngage", "bEngageObserveOnly", 1,
                                              "Data/SKSE/Plugins/APMF.ini") != 0;

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            g_notInstalledReason.store("TESDataHandler unavailable at kDataLoaded", std::memory_order_release);
            spdlog::error("[ch.19] NOT installed -- {}.", g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        std::size_t resolved = 0;
        for (std::size_t i = 0; i < kEngageSlots; ++i) {
            const RE::FormID local = kEngagePkgBase + static_cast<RE::FormID>(i);
            g_pkg[i] = dh->LookupForm<RE::TESPackage>(local, kPlugin);
            if (g_pkg[i]) {
                ++resolved;
            } else {
                // Name the form. A bare count is the aggregate that hides a
                // 100%-systematic failure.
                spdlog::error("[ch.19] MISSING APMF_EngageTravelPackage{} (0x{} in {}) -- is the plugin "
                              "installed and enabled?", i, Hex(local, 3), kPlugin);
            }
        }

        if (resolved == 0) {
            g_notInstalledReason.store("Data/APMF.esl is missing or disabled (no approach package resolved)", std::memory_order_release);
            spdlog::error("[ch.19] NOT installed -- {}. A kIntent_CombatEngage claim will be REFUSED so "
                          "the client's own degrade path runs.",
                          g_notInstalledReason.load(std::memory_order_relaxed));
            return;
        }

        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.19] installed -- {} of {} approach packages resolved from {}; mode = {}. "
                     "Arrival radius default {}u, poll {} ms, approach safety net {} ms.",
                     resolved, kEngageSlots, kPlugin,
                     g_observeOnly ? "OBSERVE-ONLY (bEngageObserveOnly=1: logs what it would do, "
                                     "claims nothing, writes nothing)"
                                   : "ACTIVE",
                     static_cast<std::uint32_t>(kDefaultRadiusUnits), kPollPeriodMs, kApproachMaxMs);
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_notInstalledReason.load(std::memory_order_acquire); }

    void ResetAll(const char* why) {
        if (g_engaged.empty()) {
            for (auto& o : g_slotOwner) o = 0;
            return;
        }
        spdlog::info("[ch.19] {} -- dropping {} engagement(s) and freeing every package slot without "
                     "releasing sub-claims (the world is being replaced).", why, g_engaged.size());
        g_engaged.clear();
        g_engagedCount.store(0, std::memory_order_relaxed);
        for (auto& o : g_slotOwner) o = 0;
    }

    void Poll() {
        // Relaxed pre-gate: nothing engaged costs one atomic load.
        if (g_engagedCount.load(std::memory_order_relaxed) == 0) return;

        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (now - g_lastPollMs < kPollPeriodMs) return;
        g_lastPollMs = now;

        for (auto& [id, e] : g_engaged) {
            if (!e.approachLive) continue;

            // The ACTOR first: an approach by an actor that is gone or dead is over
            // whatever the target is doing.
            auto  aptr = e.actorHandle.get();
            auto* a    = aptr.get();
            if (!a) {
                EndApproach(id, e, "the actor no longer resolves (unloaded or deleted)", false);
                continue;
            }
            if (a->IsDead()) {
                EndApproach(id, e, "the actor is dead", false);
                continue;
            }
            if (!a->Is3DLoaded()) {
                EndApproach(id, e, "the actor's 3D is not loaded", false);
                continue;
            }

            // The TARGET. Note IsDisabled() reads the ref's kInitiallyDisabled form
            // flag, which is the SAME flag the engine's runtime Disable() sets -- so
            // it covers a scripted despawn, not only an editor-disabled ref.
            auto  tptr = e.targetHandle.get();
            auto* t    = tptr.get();
            if (!t) {
                EndApproach(id, e, "the target no longer resolves (unloaded or deleted)", false);
                continue;
            }
            if (t->IsDead() || t->IsDisabled()) {
                EndApproach(id, e, t->IsDead() ? "the target is dead" : "the target was disabled", false);
                continue;
            }
            if (!t->Is3DLoaded()) {
                EndApproach(id, e, "the target's 3D is not loaded", false);
                continue;
            }

            // ARRIVED. Same radius the engine's own package stop uses, so the two
            // cannot disagree.
            const float dist = a->GetPosition().GetDistance(t->GetPosition());
            if (dist <= e.radius) {
                EndApproach(id, e, "ARRIVED (inside the arrival radius)", false);
                continue;
            }

            // PERCEIVED. Either the actor can see the target, or the engine's own
            // detection says it knows about it. Either one means the engine's combat
            // AI has everything it needs and the travel package is now in its way.
            // Both are main-thread engine calls; this whole loop is on the confirmed
            // -main seat and bounded to kEngageSlots entries at kPollPeriodMs.
            bool unused = false;
            if (a->HasLineOfSight(t, unused)) {
                EndApproach(id, e, "PERCEIVED (line of sight to the target)", false);
                continue;
            }
            if (a->RequestDetectionLevel(t) > 0) {
                EndApproach(id, e, "PERCEIVED (detection level > 0)", false);
                continue;
            }

            // The safety net, last: everything above is a legitimate end, this is a
            // failure report.
            if (now - e.approachStartedMs > kApproachMaxMs) {
                EndApproach(id, e, "STUCK -- no arrival, no line of sight, no detection and the target "
                                   "is still alive after the approach safety net elapsed (unreachable "
                                   "target, blocked path, or an outranking package is winning)", true);
            }
        }
    }

}

APMF_REGISTER_CHANNEL(CombatEngageChannel);
