#include "PCH.h"
#include "core/CombatMove.h"
#include "core/CombatMove_internal.h"

#include "core/Clock.h"
#include "core/ControlMap.h"         // TryGetOwningClaimBasis (the substrate test)
#include "core/FacetAttribution.h"   // Decide: the calling client vs the Engine source on the CombatAction facet
#include "core/Log.h"

#include <array>
#include <cmath>
#include <mutex>
#include <vector>

// ============================================================================
// ABI v43 THE BLOCK HOLD (kCombatMove_Block / kCombatMove_BlockEnd, ch.31). core/CombatMove.h has the engine
// road; Docs/FACETS.md "Combat move" has the contract.
//
// SHAPE. A one-shot request with a DURATION. The posted step (RunBlock, game thread) checks, sends the engine's
// Left Attack action ONCE and reads the actor's want-block state back; a hold record then stands until its
// window ends. While it stands:
//   * the combat thread's leaf seats (core/ActionGate.cpp ActThunkBody) ask BlockHeld(actor) and fail the
//     actor's Offense, Block and BlockAttack leaves at their entry (the ch.7 ForceFail act()/pop() pair). The
//     question is answered from g_heldFid, a fixed table of atomics: no lock, no form, no allocation;
//   * HoldPoll (game thread, once per frame, throttled) ends the hold at its window (the Left Release action),
//     ends it when it cannot be kept, and raises the guard again when the engine dropped it inside the window.
//
// THREADS. g_holds is written on the game thread only (RunBlock / RunBlockEnd / HoldPoll, all from
// Arbiter::OncePerFrame's pumps) and by ResetHolds at the load boundary; g_hmx makes that boundary safe and is a
// leaf lock around the table alone (no game call, no log, no other lock inside it). The engine actions are sent
// with NO lock held.
//
// NEVER A MASK. A guard the engine will not raise is a Failed record with its reason. A guard that drops and
// cannot be raised again ends the hold (HoldBroken) with its reason. A re-raise is logged and flagged on the
// record (kCombatMoveInfo_ReRaised): it is the declared window being enforced, and it is visible.
// ============================================================================
namespace apmf::combatmove {

    namespace {
        using apmf::log::Hex;
        using APMF_API::Handle;
        using detail::Sent;

        constexpr std::uint32_t kMaxHolds = APMF_API::kCombatBlockMaxHolds;
        constexpr std::uint64_t kPollMs   = 100;   // HoldPoll's own cadence (the frame seat calls it every frame)

        struct Hold {
            RE::FormID              actor       = 0;
            Handle                  handle      = APMF_API::kInvalidHandle;
            apmf::sources::SourceId client      = apmf::sources::kNoSource;
            std::uint64_t           startMs     = 0;
            std::uint64_t           endMs       = 0;
            std::uint64_t           lastRaiseMs = 0;   // the last Left Attack action sent for this hold
            std::uint32_t           reRaises    = 0;
            std::uint32_t           raiseFails  = 0;   // re-raise attempts the engine refused, since the last success
            bool                    downLogged  = false;
        };

        std::mutex        g_hmx;     // leaf: the vector only
        std::vector<Hold> g_holds;   // game thread (and ResetHolds)

        // The lock-free mirror the combat thread reads. A slot holds the actor's FormID, 0 = free. Written on
        // the game thread only (under g_hmx with the vector), so a slot is never claimed twice.
        std::array<std::atomic<RE::FormID>, kMaxHolds> g_heldFid{};
        std::atomic<std::uint32_t>                     g_heldN{ 0 };

        std::atomic<std::uint64_t> g_lastPollMs{ 0 };
        std::atomic<std::uint64_t> t_started{ 0 }, t_renewed{ 0 }, t_expired{ 0 }, t_ended{ 0 }, t_broken{ 0 },
            t_reRaised{ 0 }, t_denyOffense{ 0 }, t_denyGuard{ 0 }, t_blockedHits{ 0 };

        void MirrorSet(RE::FormID a_fid) {   // g_hmx held
            for (auto& s : g_heldFid) {
                RE::FormID zero = 0;
                if (s.compare_exchange_strong(zero, a_fid, std::memory_order_release)) {
                    g_heldN.fetch_add(1, std::memory_order_release);
                    return;
                }
            }
        }
        void MirrorClear(RE::FormID a_fid) {   // g_hmx held
            for (auto& s : g_heldFid) {
                RE::FormID want = a_fid;
                if (s.compare_exchange_strong(want, 0, std::memory_order_release)) {
                    g_heldN.fetch_sub(1, std::memory_order_release);
                    return;
                }
            }
        }

        // Would the engine's Left Attack action BLOCK with what is in his hands? nullptr = yes; else why not.
        // The engine's post-action step sets the want-block state only when the idle it picked is "blockStart",
        // and what it picks follows the hands: a shield or a torch in the left hand blocks; an empty left hand
        // (or a two-handed weapon, which fills both) blocks with a melee weapon in the right. Anything else in
        // the left hand makes the same action a left-hand attack or cast, which must never be sent as a "block".
        const char* HandsWouldNotBlock(RE::Actor* a_actor) {
            auto* left  = a_actor->GetEquippedObject(true);
            auto* right = a_actor->GetEquippedObject(false);
            if (left) {
                if (auto* armo = left->As<RE::TESObjectARMO>(); armo && armo->IsShield()) return nullptr;
                if (left->Is(RE::FormType::Light)) return nullptr;
                if (left != right) return "the left hand holds a weapon, a spell, a staff or a scroll";
            }
            auto* weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
            if (!weap) return right ? "the right hand holds no melee weapon and the left no shield" : "bare hands";
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kOneHandDagger:
            case RE::WEAPON_TYPE::kOneHandAxe:
            case RE::WEAPON_TYPE::kOneHandMace:
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:
                return nullptr;
            default:
                return "the right hand holds a bow, a crossbow, a staff or a fist weapon";
            }
        }

        bool WantBlocking(RE::Actor* a_actor) { return a_actor->AsActorState()->actorState2.wantBlocking != 0; }

        // The checks RunBlock and the re-raise share (the actor can hold a guard at all). nullptr = yes.
        const char* CannotGuard(RE::Actor* a_actor) {
            auto* proc = a_actor->GetActorRuntimeData().currentProcess;
            if (!a_actor->Is3DLoaded() || !proc || !proc->high) return "not loaded (no 3D or no high process)";
            if (a_actor->IsDead()) return "dead";
            if (a_actor->IsInKillMove()) return "in a kill move";
            if (!a_actor->AsActorState()->IsWeaponDrawn()) return "his weapon is not drawn";
            return nullptr;
        }

        // Game thread, NO lock held. Lower the guard by the engine's own action. The engine clears the want-block
        // state in its post-action step; a refused release is logged (the state then stands until the engine's
        // own next release or the actor's next block-stop).
        void SendRelease(RE::Actor* a_actor, Handle a_handle, const char* a_why) {
            if (!WantBlocking(a_actor)) return;   // already down: nothing to lower
            std::string event;
            const bool  ok = detail::ProcessAction(a_actor, detail::ActionLeftRelease(), nullptr, &event);
            if (!ok || WantBlocking(a_actor))
                spdlog::warn("[combat-move] {} block RELEASE not applied (actor 0x{} '{}', {}): the Left Release action "
                             "returned {} with event '{}' and his want-block state is {}. The engine's own next release "
                             "lowers it. Report this line if he stays in guard.",
                             a_handle, Hex(a_actor->GetFormID()), a_actor->GetName(), a_why, ok, event,
                             WantBlocking(a_actor) ? "still set" : "clear");
        }

        // Game thread. Remove the hold of `a_actor` from the table and the mirror; returns it (handle 0 = none).
        Hold TakeHold(RE::FormID a_actor) {
            std::scoped_lock lock(g_hmx);
            for (auto it = g_holds.begin(); it != g_holds.end(); ++it) {
                if (it->actor != a_actor) continue;
                Hold h = *it;
                g_holds.erase(it);
                MirrorClear(a_actor);
                return h;
            }
            return {};
        }

        std::string_view ClientName(apmf::sources::SourceId a_client) {
            return a_client == apmf::sources::kNoSource ? std::string_view{ "none" } : apmf::sources::Name(a_client);
        }
    }

    bool AnyBlockHeld() { return g_heldN.load(std::memory_order_acquire) != 0; }

    bool BlockHeld(RE::FormID a_actor) {
        if (a_actor == 0 || g_heldN.load(std::memory_order_acquire) == 0) return false;
        for (const auto& s : g_heldFid)
            if (s.load(std::memory_order_acquire) == a_actor) return true;
        return false;
    }

    void NoteHoldDeny(bool a_guardLeaf) {
        (a_guardLeaf ? t_denyGuard : t_denyOffense).fetch_add(1, std::memory_order_relaxed);
    }

    namespace detail {

        void NoteBlockedHit(RE::FormID a_target, RE::FormID a_cause) {
            if (!BlockHeld(a_target)) return;
            Handle h = APMF_API::kInvalidHandle;
            {
                std::scoped_lock lock(g_hmx);
                for (const auto& hold : g_holds)
                    if (hold.actor == a_target) {
                        h = hold.handle;
                        break;
                    }
            }
            if (h == APMF_API::kInvalidHandle) return;
            t_blockedHits.fetch_add(1, std::memory_order_relaxed);
            MarkInfo(h, APMF_API::kCombatMoveInfo_BlockedAHit, 0, a_cause);
        }

        void ResetHolds() {
            std::size_t n = 0;
            {
                std::scoped_lock lock(g_hmx);
                n = g_holds.size();
                g_holds.clear();
                for (auto& s : g_heldFid) s.store(0, std::memory_order_release);
                g_heldN.store(0, std::memory_order_release);
            }
            // The records were forgotten with the record table (they read Unknown). No action is sent: the actors
            // belong to the outgoing world.
            if (n != 0) spdlog::info("[combat-move] forgot {} block hold(s) at the world boundary.", n);
        }

        void RunBlock(Handle a_handle, RE::FormID a_actor, float a_holdS, apmf::sources::SourceId a_client) {
            Sent       sent{};
            const auto refuse = [&](std::uint32_t a_why, Handle a_blocker, const std::string& a_detail) {
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Refused, a_why, a_blocker, sent)) return;
                spdlog::warn("[combat-move] {} REFUSED (block by actor 0x{}): {}{}", a_handle, Hex(a_actor), WhyText(a_why),
                             a_detail);
            };

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            if (!actor) return refuse(APMF_API::kCombatMoveWhy_ActorGone, 0, "");
            if (actor->IsPlayerRef()) return refuse(APMF_API::kCombatMoveWhy_ActorGone, 0, " (the player is never moved)");
            if (const char* why = CannotGuard(actor))
                return refuse(APMF_API::kCombatMoveWhy_NotReady, 0, std::format(" ({})", why));
            if (actor->IsInCombat()) sent.info |= APMF_API::kCombatMoveInfo_InCombat;
            sent.info |= APMF_API::kCombatMoveInfo_WeaponOut;

            // A combat substrate's allow-list leaves the actor nothing but its cast: no guard under it.
            {
                APMF_API::APMF_Param p{};
                float                b     = 0.0f;
                Handle               owner = APMF_API::kInvalidHandle;
                if (apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_CombatSubstrate, p, b, &owner))
                    return refuse(APMF_API::kCombatMoveWhy_SubstrateHeld, owner, std::format(" (h={})", owner));
            }
            // The hold denies the engine tree's own leaves: the calling client must outrank the Engine source on
            // the CombatAction facet (the test a ch.7 deny passes). Decided here, once per request.
            {
                const auto v = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, a_handle,
                                                       apmf::sources::kEngineSource, a_actor, "");
                if (!v.enforce)
                    return refuse(APMF_API::kCombatMoveWhy_Outranked, 0,
                                  std::format(" (client priority {}, Engine {})", v.claimPrio, v.srcPrio));
            }
            if (const char* why = HandsWouldNotBlock(actor))
                return refuse(APMF_API::kCombatMoveWhy_NoBlockItem, 0, std::format(" ({})", why));

            const std::uint64_t now    = apmf::clock::MonotonicMs();
            const std::uint64_t holdMs = static_cast<std::uint64_t>(std::llround(static_cast<double>(a_holdS) * 1000.0));

            // A standing hold on this actor: the same client's is RENEWED (the guard stays up, the newer handle
            // takes the hold); another client's stands.
            Hold prev{};
            bool full = false;
            {
                std::scoped_lock lock(g_hmx);
                for (const auto& h : g_holds)
                    if (h.actor == a_actor) {
                        prev = h;
                        break;
                    }
                full = prev.handle == APMF_API::kInvalidHandle && g_holds.size() >= kMaxHolds;
            }
            if (prev.handle != APMF_API::kInvalidHandle && prev.client != a_client)
                return refuse(APMF_API::kCombatMoveWhy_HeldByOther, prev.handle,
                              std::format(" (h={}, client '{}')", prev.handle, ClientName(prev.client)));
            if (full) return refuse(APMF_API::kCombatMoveWhy_HoldTableFull, 0, std::format(" ({} holds)", kMaxHolds));

            const bool wasUp = WantBlocking(actor);
            if (prev.handle != APMF_API::kInvalidHandle && wasUp) {
                // RENEW: no action. The guard is up; the newer request owns the rest of it.
                {
                    std::scoped_lock lock(g_hmx);
                    for (auto& h : g_holds)
                        if (h.actor == a_actor) {
                            h.handle = a_handle;
                            h.endMs  = now + holdMs;
                        }
                }
                sent.info |= APMF_API::kCombatMoveInfo_HoldLive;
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Started, APMF_API::kCombatMoveWhy_None, 0, sent)) {
                    TakeHold(a_actor);   // the record is gone (a reset since the post): no hold without its record
                    return;
                }
                EndHold(prev.handle, APMF_API::kCombatMoveWhy_HoldRenewed);
                t_renewed.fetch_add(1, std::memory_order_relaxed);
                spdlog::debug("[combat-move] {} block RENEWED: actor 0x{} '{}' guard held {:.2f} s more (took over from {}).",
                              a_handle, Hex(a_actor), actor->GetName(), a_holdS, prev.handle);
                return;
            }

            // The engine's own block test before the action: no attack in progress.
            const auto atk = actor->AsActorState()->GetAttackState();
            if (atk != RE::ATTACK_STATE_ENUM::kNone)
                return refuse(APMF_API::kCombatMoveWhy_Busy, 0, std::format(" (attack state {})", static_cast<std::uint32_t>(atk)));

            std::string event;
            const bool  ok = wasUp ? true : detail::ProcessAction(actor, detail::ActionLeftAttack(), nullptr, &event);
            const bool  up = WantBlocking(actor);
            if (!ok || !up) {
                const std::uint32_t why = ok ? APMF_API::kCombatMoveWhy_NotApplied : APMF_API::kCombatMoveWhy_EngineRefused;
                // A standing hold of this client (its guard had dropped) is left as it is: its own window and the
                // poll's re-raise decide it. Only THIS request failed.
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Failed, why, 0, sent)) return;
                if (ok)
                    spdlog::error("[combat-move] {} FAILED (block by actor 0x{} '{}'): the Left Attack action returned true "
                                  "with event '{}' but his want-block state did not set: the engine ran it as something other "
                                  "than a block (the hands test passed: left 0x{}, right 0x{}). Report this line.",
                                  a_handle, Hex(a_actor), actor->GetName(), event,
                                  Hex(actor->GetEquippedObject(true) ? actor->GetEquippedObject(true)->GetFormID() : 0),
                                  Hex(actor->GetEquippedObject(false) ? actor->GetEquippedObject(false)->GetFormID() : 0));
                else
                    spdlog::warn("[combat-move] {} FAILED (block by actor 0x{} '{}'): {} (staggered, recoiling, mid-draw, or "
                                 "his behaviour graph has no block from this state); in combat {} (client '{}').",
                                 a_handle, Hex(a_actor), actor->GetName(), WhyText(why),
                                 (sent.info & APMF_API::kCombatMoveInfo_InCombat) != 0, ClientName(a_client));
                return;
            }

            {
                std::scoped_lock lock(g_hmx);
                bool             found = false;
                for (auto& h : g_holds)
                    if (h.actor == a_actor) {   // the renewed hold whose guard had dropped: raised again by this request
                        h.handle      = a_handle;
                        h.endMs       = now + holdMs;
                        h.lastRaiseMs = now;
                        h.raiseFails  = 0;
                        h.downLogged  = false;
                        found         = true;
                    }
                if (!found) {
                    Hold h{};
                    h.actor       = a_actor;
                    h.handle      = a_handle;
                    h.client      = a_client;
                    h.startMs     = now;
                    h.endMs       = now + holdMs;
                    h.lastRaiseMs = now;
                    g_holds.push_back(h);
                    MirrorSet(a_actor);
                }
            }
            sent.info |= APMF_API::kCombatMoveInfo_HoldLive;
            if (!Finish(a_handle, APMF_API::kCombatMoveState_Started, APMF_API::kCombatMoveWhy_None, 0, sent)) {
                TakeHold(a_actor);
                SendRelease(actor, a_handle, "its record was reset");
                return;
            }
            if (prev.handle != APMF_API::kInvalidHandle) EndHold(prev.handle, APMF_API::kCombatMoveWhy_HoldRenewed);
            t_started.fetch_add(1, std::memory_order_relaxed);
            spdlog::info("[combat-move] {} STARTED: actor 0x{} '{}' BLOCK held for {:.2f} s ({}); his Offense, Block and "
                         "BlockAttack leaves fail at entry while it stands; in combat {} (client '{}').",
                         a_handle, Hex(a_actor), actor->GetName(), a_holdS,
                         wasUp ? "his guard was already up: no action sent" : std::format("Left Attack action, event '{}'", event),
                         (sent.info & APMF_API::kCombatMoveInfo_InCombat) != 0, ClientName(a_client));
        }

        void RunBlockEnd(Handle a_handle, RE::FormID a_actor, apmf::sources::SourceId a_client) {
            const Sent sent{};
            Hold       hold{};
            bool       other = false;
            {
                std::scoped_lock lock(g_hmx);
                for (const auto& h : g_holds)
                    if (h.actor == a_actor) {
                        if (h.client == a_client)
                            hold = h;
                        else
                            other = true;
                        break;
                    }
            }
            if (hold.handle == APMF_API::kInvalidHandle) {
                if (Finish(a_handle, APMF_API::kCombatMoveState_Refused, APMF_API::kCombatMoveWhy_NoHold, 0, sent))
                    spdlog::info("[combat-move] {} REFUSED (block end by actor 0x{}): {}{}.", a_handle, Hex(a_actor),
                                 WhyText(APMF_API::kCombatMoveWhy_NoHold), other ? " (another client's hold stands)" : "");
                return;
            }
            TakeHold(a_actor);
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            if (actor && actor->Is3DLoaded()) SendRelease(actor, hold.handle, "block end");
            EndHold(hold.handle, APMF_API::kCombatMoveWhy_HoldEndedByClient);
            t_ended.fetch_add(1, std::memory_order_relaxed);
            if (Finish(a_handle, APMF_API::kCombatMoveState_Started, APMF_API::kCombatMoveWhy_None, 0, sent))
                spdlog::info("[combat-move] {} block END: actor 0x{} guard lowered (hold {} ended after {} ms, {} re-raise(s)).",
                             a_handle, Hex(a_actor), hold.handle, apmf::clock::MonotonicMs() - hold.startMs, hold.reRaises);
        }
    }

    void HoldPoll() {
        if (g_heldN.load(std::memory_order_acquire) == 0) return;
        const std::uint64_t now  = apmf::clock::MonotonicMs();
        const std::uint64_t last = g_lastPollMs.load(std::memory_order_relaxed);
        if (now - last < kPollMs) return;
        g_lastPollMs.store(now, std::memory_order_relaxed);

        std::vector<Hold> holds;
        {
            std::scoped_lock lock(g_hmx);
            holds = g_holds;
        }
        for (const auto& h : holds) {
            try {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(h.actor);
                // Over, with its reason. `a_release`: send the engine's Left Release first (the actor can take it).
                const auto end = [&](std::uint32_t a_why, const char* a_detail, bool a_release) {
                    TakeHold(h.actor);
                    if (a_release && actor) SendRelease(actor, h.handle, a_detail);
                    detail::EndHold(h.handle, a_why);
                    (a_why == APMF_API::kCombatMoveWhy_HoldExpired ? t_expired : t_broken).fetch_add(1, std::memory_order_relaxed);
                    if (a_why == APMF_API::kCombatMoveWhy_HoldExpired)
                        spdlog::info("[combat-move] {} block ENDED: actor 0x{} window over after {} ms ({} re-raise(s)); guard "
                                     "lowered by the Left Release action.", h.handle, Hex(h.actor), now - h.startMs, h.reRaises);
                    else
                        spdlog::warn("[combat-move] {} block ENDED early: actor 0x{} -- {} ({}), after {} ms of {} ({} "
                                     "re-raise(s)).", h.handle, Hex(h.actor), detail::WhyText(a_why), a_detail,
                                     now - h.startMs, h.endMs - h.startMs, h.reRaises);
                };
                if (!actor) {
                    end(APMF_API::kCombatMoveWhy_HoldBroken, "the actor is no longer in memory", false);
                    continue;
                }
                if (const char* why = CannotGuard(actor)) {
                    end(APMF_API::kCombatMoveWhy_HoldBroken, why, false);
                    continue;
                }
                {
                    APMF_API::APMF_Param p{};
                    float                b     = 0.0f;
                    Handle               owner = APMF_API::kInvalidHandle;
                    if (apmf::ControlMap::Get().TryGetOwningClaimBasis(h.actor, APMF_API::kIntent_CombatSubstrate, p, b, &owner)) {
                        end(APMF_API::kCombatMoveWhy_HoldBroken, "a combat substrate took the actor", true);
                        continue;
                    }
                }
                if (now >= h.endMs) {
                    end(APMF_API::kCombatMoveWhy_HoldExpired, "window over", true);
                    continue;
                }
                if (WantBlocking(actor)) continue;   // held: nothing to do

                // The engine dropped the guard inside the window.
                auto* avo = actor->AsActorValueOwner();
                if (avo && !(avo->GetActorValue(RE::ActorValue::kStamina) > 0.0f)) {
                    end(APMF_API::kCombatMoveWhy_HoldBroken, "stamina at 0: the engine's own block rule dropped the guard", false);
                    continue;
                }
                if (const char* why = HandsWouldNotBlock(actor)) {
                    end(APMF_API::kCombatMoveWhy_HoldBroken, why, false);
                    continue;
                }
                const auto* st = actor->AsActorState();
                const bool  able = st->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && st->actorState2.staggered == 0 &&
                                   st->actorState2.recoil == 0 && st->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal;
                if (!able || now - h.lastRaiseMs < APMF_API::kCombatBlockReRaiseMs) continue;   // wait until he can block again

                std::string event;
                const bool  ok = detail::ProcessAction(actor, detail::ActionLeftAttack(), nullptr, &event);
                const bool  up = WantBlocking(actor);
                std::uint32_t reRaises = 0, fails = 0;
                bool          firstDown = false;
                {
                    std::scoped_lock lock(g_hmx);
                    for (auto& live : g_holds)
                        if (live.actor == h.actor && live.handle == h.handle) {
                            live.lastRaiseMs = now;
                            if (ok && up) {
                                ++live.reRaises;
                                live.raiseFails = 0;
                            } else {
                                ++live.raiseFails;
                            }
                            firstDown       = !live.downLogged;
                            live.downLogged = true;
                            reRaises        = live.reRaises;
                            fails           = live.raiseFails;
                        }
                }
                if (ok && up) {
                    t_reRaised.fetch_add(1, std::memory_order_relaxed);
                    detail::MarkInfo(h.handle, APMF_API::kCombatMoveInfo_ReRaised, 0);
                    if (firstDown || reRaises % 8 == 0)
                        spdlog::info("[combat-move] {} block RE-RAISED: actor 0x{} '{}' -- the engine dropped his guard inside "
                                     "the window ({} ms left); Left Attack action again, event '{}' (re-raise {}; logged for "
                                     "the first and every 8th).", h.handle, Hex(h.actor), actor->GetName(), h.endMs - now,
                                     event, reRaises);
                } else if (fails == 1 || fails % 8 == 0) {
                    spdlog::warn("[combat-move] {} block re-raise REFUSED: actor 0x{} '{}' -- the Left Attack action returned {} "
                                 "with event '{}', want-block {}; trying again every {} ms until the window ends ({} ms left; "
                                 "attempt {}).", h.handle, Hex(h.actor), actor->GetName(), ok, event, up ? "set" : "clear",
                                 APMF_API::kCombatBlockReRaiseMs, h.endMs - now, fails);
                }
            } catch (...) {
                TakeHold(h.actor);
                detail::EndHold(h.handle, APMF_API::kCombatMoveWhy_HoldBroken);
                spdlog::error("[combat-move] {} block ENDED: an exception inside the hold poll (actor 0x{}). Report this line.",
                              h.handle, Hex(h.actor));
            }
        }

        // RULE C heartbeat: every ~30 s while any hold stood since the last line, zeros included.
        static std::uint64_t s_lastBeat = 0;
        if (now - s_lastBeat >= 30000) {
            s_lastBeat = now;
            spdlog::info("[combat-move] H block holds: live={} started={} renewed={} expired={} endedByClient={} broken={} "
                         "reRaised={} | leaves failed: offense={} block/blockAttack={} | blocked hits on a held actor={}",
                         g_heldN.load(), t_started.load(), t_renewed.load(), t_expired.load(), t_ended.load(), t_broken.load(),
                         t_reRaised.load(), t_denyOffense.load(), t_denyGuard.load(), t_blockedHits.load());
        }
    }

}
