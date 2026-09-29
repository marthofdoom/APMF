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

// Channel 19 -- TRAVEL: the leg-state mirror, the APMF markers, the package slots and
// the leg itself (start / point / end / still-ours). See Travel.cpp for the design.

namespace {

    const char* LegStateName(std::uint32_t a_state) {
        switch (a_state) {
        case APMF_API::kLeg_None:            return "none";
        case APMF_API::kLeg_Pending:         return "pending";
        case APMF_API::kLeg_Walking:         return "walking";
        case APMF_API::kLeg_Arrived:         return "arrived";
        case APMF_API::kLeg_Blocked:         return "BLOCKED";
        case APMF_API::kLeg_CombatCancelled: return "combat-cancelled";
        case APMF_API::kLeg_DestGone:        return "destination-gone";
        case APMF_API::kLeg_StuckTimeout:    return "stuck-timeout";
        case APMF_API::kLeg_ActorGone:       return "actor-gone";
        case APMF_API::kLeg_Failed:          return "failed";
        case APMF_API::kLeg_Released:        return "released";
        default:                             return "?";
        }
    }

}

    // The gait a leg's flags DECLARE (0..3), or 0xFFFFFFFF for none.
    std::uint32_t DeclaredSpeed(std::uint32_t a_flags) {
        if ((a_flags & APMF_API::kTravel_SpeedSet) == 0) return 0xFFFFFFFFu;
        return (a_flags & APMF_API::kTravel_SpeedMask) >> 3;
    }

    // Record a state change. GAME THREAD (every caller is). `a_speed` = what was written
    // into the record (0xFFFFFFFF = nothing written); pass the previous value to keep it.
    // `a_owner` = the ch.19 claim handle the state belongs to.
    void SetLegState(RE::FormID a_id, std::uint32_t a_state, RE::FormID a_destForm, const RE::NiPoint3& a_point,
                     std::uint32_t a_speed, APMF_API::Handle a_owner, const BlockInfo* a_block) {
        std::uint32_t seq = 0;
        {
            std::lock_guard lk(g_stateMx);
            auto& r       = g_state[a_id];
            r.state       = a_state;
            r.destForm    = a_destForm;
            r.point       = a_point;
            r.sinceMs     = apmf::clock::MonotonicMs();
            r.seq         = ++g_seqCounter;
            r.stall       = a_block ? a_block->stall : RE::NiPoint3{};
            r.blockedMs   = a_block ? a_block->ms : 0;
            r.blocker     = a_block ? a_block->blocker : 0;
            r.blockerKind = a_block ? a_block->blockerKind : static_cast<std::uint32_t>(APMF_API::kBlocker_None);
            r.speed       = a_speed;
            r.owner       = a_owner;
            seq           = r.seq;
        }
        spdlog::info("[travel-state] 0x{} -> {} (seq {}, destination 0x{}, claim h={}).", Hex(a_id),
                     LegStateName(a_state), seq, Hex(a_destForm), a_owner);
    }

    // The speed field the mirror currently holds for `a_id` (0xFFFFFFFF if none).
    std::uint32_t MirroredSpeed(RE::FormID a_id) {
        std::lock_guard lk(g_stateMx);
        auto it = g_state.find(a_id);
        return it == g_state.end() ? 0xFFFFFFFFu : it->second.speed;
    }

    // SetLegState for a leg: a point leg reports its declared point and destForm 0 (its
    // destId is APMF's marker, a FormID the client never sees).
    void SetLegStateFor(RE::FormID a_id, const Leg& a_leg, std::uint32_t a_state, std::uint32_t a_speed,
                        const BlockInfo* a_block) {
        SetLegState(a_id, a_state, a_leg.toPosition ? 0 : a_leg.destId,
                    a_leg.toPosition ? a_leg.point : RE::NiPoint3{}, a_speed, a_leg.ownerHandle, a_block);
    }

namespace {

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

namespace {

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
    // ABI v12: `a_state` is the TravelLegState the end is recorded as (GetTravelLegState);
    // `a_block` is for kLeg_Blocked only.
    void EndLeg(RE::FormID a_id, Leg& a_leg, const char* a_why, bool a_failure, std::uint32_t a_state,
                const BlockInfo* a_block) {
        if (!a_leg.legLive) return;
        a_leg.legLive = false;
        ResetBlockedClock(a_leg);
        SetLegStateFor(a_id, a_leg, a_state, MirroredSpeed(a_id), a_block);

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
            SetLegStateFor(a_id, a_leg, APMF_API::kLeg_Failed, 0xFFFFFFFFu);
            return false;
        }

        RE::TESPackage* pkg = g_pkg[slot];
        if (!pkg) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- slot {} has no package (0x{} in {} did not "
                          "resolve).", Hex(a_id), slot, Hex(kTravelPkgBase + slot, 3), kPlugin);
            FreeSlot(slot);
            SetLegStateFor(a_id, a_leg, APMF_API::kLeg_Failed, 0xFFFFFFFFu);
            return false;
        }

        if (!PointPackage(a_id, a_leg, pkg)) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- could not point package 0x{}'s Location at "
                          "destination 0x{} (see the [pkgdata] line above for which guard failed). "
                          "NOTHING was offered: an unpointed travel package would walk the actor to the "
                          "placeholder ref.",
                          Hex(a_id), Hex(pkg->GetFormID()), Hex(a_leg.destId));
            FreeSlot(slot);
            SetLegStateFor(a_id, a_leg, APMF_API::kLeg_Failed, 0xFFFFFFFFu);
            return false;
        }

        g_slotMarkerId[slot] = (a_leg.markerId != 0 && a_leg.destId == a_leg.markerId) ? a_leg.markerId : 0;

        // ABI v12 gait: written into the record BEFORE the offer below is queued, so the
        // package cannot start on the actor with the previous leg's speed.
        const std::uint32_t speed = ApplyGait(a_id, a_leg, slot, false);

        APMF_API::APMF_Param p9{};
        p9.form = pkg->GetFormID();
        const APMF_API::Handle h =
            apmf::ControlMap::Get().EnqueueRequest(a_id, APMF_API::kIntent_OfferPackage, a_leg.basis, &p9);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::error("[travel-leg] 0x{} REFUSED -- the internal ch.9 offer was refused (ControlMap "
                          "logged why).", Hex(a_id));
            FreeSlot(slot);
            SetLegStateFor(a_id, a_leg, APMF_API::kLeg_Failed, speed);
            return false;
        }

        a_leg.slot         = slot;
        a_leg.hPackage     = h;
        a_leg.basisPackage = a_leg.basis;   // what the offer was actually FILED at
        a_leg.legLive      = true;
        a_leg.legStartedMs = apmf::clock::MonotonicMs();
        ResetBlockedClock(a_leg);
        a_leg.gaitLogged   = false;
        SetLegStateFor(a_id, a_leg, APMF_API::kLeg_Walking, speed);
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
    bool StillOurs(RE::FormID a_id, const Leg& a_leg, const char* a_step, float* a_outBasis,
                   APMF_API::Handle* a_outOwner) {
        APMF_API::APMF_Param now{};
        float                basis = 0.0f;
        APMF_API::Handle     owner = APMF_API::kInvalidHandle;
        const bool claimed = apmf::ControlMap::Get().TryGetOwningClaimBasis(
            a_id, APMF_API::kIntent_Travel, now, basis, &owner);
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
        if (a_outOwner) *a_outOwner = owner;
        return true;
    }
