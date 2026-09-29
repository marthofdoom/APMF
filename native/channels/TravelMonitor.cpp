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

// Channel 19 -- TRAVEL: the per-frame leg monitor (apmf::travel::Poll), which decides
// when a leg is OVER. See Travel.cpp for the design and the threading note.

namespace apmf::travel {

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
                EndLeg(id, leg, "the actor no longer resolves (unloaded or deleted)", false, APMF_API::kLeg_ActorGone);
                continue;
            }
            if (a->IsDead()) {
                EndLeg(id, leg, "the actor is dead", false, APMF_API::kLeg_ActorGone);
                continue;
            }
            if (!a->Is3DLoaded()) {
                EndLeg(id, leg, "the actor's 3D is not loaded", false, APMF_API::kLeg_ActorGone);
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
                EndLeg(id, leg, "the actor is IN COMBAT -- combat cancels the movement", false,
                       APMF_API::kLeg_CombatCancelled);
                continue;
            }

            // The DESTINATION, and ARRIVAL -- both depend on which KIND it is.
            if (leg.destKind == DestKind::kCell) {
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(leg.destId);
                if (!cell) {
                    EndLeg(id, leg, "the destination cell no longer resolves", false, APMF_API::kLeg_DestGone);
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
                    EndLeg(id, leg, "ARRIVED (the actor's parent cell is the destination cell)", false,
                           APMF_API::kLeg_Arrived);
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
                    EndLeg(id, leg, "the destination no longer resolves (deleted)", false, APMF_API::kLeg_DestGone);
                    continue;
                }
                if (d->IsDisabled()) {
                    EndLeg(id, leg, "the destination was disabled", false, APMF_API::kLeg_DestGone);
                    continue;
                }
                // DEATH ends the leg only if the destination was ALIVE when targeted
                // (see Leg::destAliveAtTarget). A destination dead at target time is a
                // corpse to walk to, and ends the leg only on the other checks here.
                if (leg.destAliveAtTarget && d->IsDead()) {
                    EndLeg(id, leg, "the destination died during travel", false, APMF_API::kLeg_DestGone);
                    continue;
                }

                // ARRIVED. The same radius this leg wrote into the package record, so
                // the engine's own stop and this test fire at the same distance.
                // `GetPosition()` is `data.location` -- no 3D required.
                const float dist = a->GetPosition().GetDistance(d->GetPosition());
                if (dist <= leg.radius) {
                    EndLeg(id, leg, "ARRIVED (inside the arrival radius)", false, APMF_API::kLeg_Arrived);
                    continue;
                }
            }

            // ABI v12: BLOCKED. After every legitimate end above (arrival wins over a blip at
            // the destination), before the safety net. See kBlockedEndMs / kMbMaxStepMs for the
            // engine fact, the sizing and the running-time clock. The running package is read
            // under its own ActorPackage lock (review F7): see ReadRunningPackage.
            {
                const RunningPackage cur  = ReadRunningPackage(a);
                const std::uint64_t  step = leg.mbLastPollMs == 0 ? 0 : std::min(now - leg.mbLastPollMs, kMbMaxStepMs);
                leg.mbLastPollMs = now;
                if (cur.movementBlocked) {
                    if (!leg.mbRun) {
                        leg.mbRun     = true;
                        leg.mbAccumMs = 0;
                        const auto p  = a->GetPosition();
                        spdlog::info("[travel-leg] 0x{} Movement Blocked (package 0x{}) at {:.0f},{:.0f},{:.0f} -- the leg "
                                     "ends as BLOCKED if it holds {} ms of running time.",
                                     Hex(id), Hex(cur.formId), p.x, p.y, p.z, kBlockedEndMs);
                    } else {
                        leg.mbAccumMs += static_cast<std::uint32_t>(step);
                    }
                    leg.mbMissed = false;
                    if (leg.mbAccumMs >= kBlockedEndMs) {
                        // Probe 2 needs the destination; EndLeg deletes a point leg's marker.
                        RE::NiPointer<RE::TESObjectREFR> p2Dest;
                        RE::NiPoint3                     p2DestPos{};
                        bool                             p2HaveDest = false;
                        if (leg.toPosition) {
                            p2DestPos  = leg.point;
                            p2HaveDest = true;
                        } else if (leg.destKind == DestKind::kRef) {
                            p2Dest = leg.destHandle.get();
                            if (p2Dest) {
                                p2DestPos  = p2Dest->GetPosition();
                                p2HaveDest = true;
                            }
                        }
                        const RE::FormID p2DestId = leg.destId;
                        BlockInfo blk{};
                        blk.stall = a->GetPosition();
                        blk.ms    = leg.mbAccumMs;
                        FindBlocker(a, leg, blk);
                        const std::string why = std::format(
                            "BLOCKED -- the engine held the actor in its Movement Blocked package (0x{}) for {} ms of "
                            "running time at {:.0f},{:.0f},{:.0f}; {}",
                            Hex(cur.formId), blk.ms, blk.stall.x, blk.stall.y, blk.stall.z,
                            blk.blockerKind == APMF_API::kBlocker_None
                                ? std::string("no actor in front (a STATIC block)")
                                : std::format("{} 0x{} in front", BlockerKindName(blk.blockerKind), Hex(blk.blocker)));
                        EndLeg(id, leg, why.c_str(), true, APMF_API::kLeg_Blocked, &blk);
                        GateProbe(id, a, blk.stall);
                        GateProbe2(id, a, blk.stall, p2Dest.get(), p2DestId, p2HaveDest, p2DestPos);
                        continue;
                    }
                } else if (leg.mbRun) {
                    if (!leg.mbMissed) {
                        leg.mbMissed = true;   // one miss tolerated (review F6)
                    } else {
                        spdlog::info("[travel-leg] 0x{} Movement Blocked cleared after {} ms of running time (under the "
                                     "{} ms end).", Hex(id), leg.mbAccumMs, kBlockedEndMs);
                        leg.mbRun     = false;
                        leg.mbMissed  = false;
                        leg.mbAccumMs = 0;
                    }
                }
            }

            ObserveGait(id, a, leg);   // ABI v12: one passive line per gait leg, never a write

            // The safety net, last: everything above is a legitimate end, this is a
            // failure report.
            if (now - leg.legStartedMs > kLegMaxMs) {
                EndLeg(id, leg, "STUCK -- no arrival, no combat, and the destination is still there "
                                "after the leg safety net elapsed (unreachable or cross-worldspace "
                                "destination, blocked path, or an outranking package is winning)", true,
                       APMF_API::kLeg_StuckTimeout);
            }
        }
    }

}
