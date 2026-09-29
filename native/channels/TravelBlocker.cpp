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

// Channel 19 -- TRAVEL: the running package read under its lock (the Movement Blocked
// verdict's input) and the directional blocker check. See Travel.cpp for the design.

    // ---- ABI v12: the running package, read the engine's way, under its lock ----------
    // The engine picks the RUNNING ActorPackage as process->middleHigh->runOncePackage when
    // that holds a package, else process->currentPackage (1.6.1170 0x70F590 / 1.5.97
    // 0x67BDD0, the same selection CommonLib's AIProcess::GetRunningPackage makes, unlocked
    // in the engine too). The package pointer and its type are then read under that
    // ActorPackage's own `packageLock` (review F7): follower AI updates run on job threads
    // and swap the package under that lock, and a runtime package such as Movement Blocked
    // (an 0xFF form) is not something this file may assume outlives the swap. If the
    // selection races a runOnce clear, the locked read sees no package: one missed poll,
    // which the BLOCKED clock tolerates. GAME THREAD (Poll).

    RunningPackage ReadRunningPackage(RE::Actor* a_actor) {
        RunningPackage out{};
        auto* proc = a_actor->GetActorRuntimeData().currentProcess;
        if (!proc) return out;
        RE::ActorPackage* ap = (proc->middleHigh && proc->middleHigh->runOncePackage.package)
                                   ? &proc->middleHigh->runOncePackage
                                   : &proc->currentPackage;
        RE::BSSpinLockGuard lk(ap->packageLock);
        if (const auto* pkg = ap->package) {
            out.formId          = pkg->GetFormID();
            out.movementBlocked = pkg->packData.packType.get() == RE::PACKAGE_PROCEDURE_TYPE::kMovementBlocked;
        }
        return out;
    }

    const char* BlockerKindName(std::uint32_t a_kind) {
        switch (a_kind) {
        case APMF_API::kBlocker_Player:   return "the PLAYER";
        case APMF_API::kBlocker_Teammate: return "a TEAMMATE";
        case APMF_API::kBlocker_Actor:    return "an ACTOR";
        default:                          return "nothing";
        }
    }

    // The directional proximity check at a BLOCKED verdict (see kBlockerReach). Fills
    // `a_blk.blocker` / `blockerKind` with the nearest live actor in front, or leaves them
    // 0 / kBlocker_None (a static block). Candidates: the high-process actors and the
    // player. Reads positions, angles and flags only. GAME THREAD (Poll).
    void FindBlocker(RE::Actor* a_actor, const Leg& a_leg, BlockInfo& a_blk) {
        const RE::NiPoint3 me    = a_actor->GetPosition();
        const float        face  = a_actor->GetAngleZ();
        const RE::NiPoint3 fwdA{ std::sin(face), std::cos(face), 0.0f };
        RE::NiPoint3       fwdB{};
        bool               haveB = false;
        {
            RE::NiPoint3 goal{};
            bool         haveGoal = false;
            if (a_leg.destKind == DestKind::kRef) {
                auto dptr = a_leg.destHandle.get();
                if (auto* d = dptr.get()) {
                    goal     = d->GetPosition();
                    haveGoal = true;
                }
            }
            if (haveGoal) {
                const float gx = goal.x - me.x, gy = goal.y - me.y;
                const float gl = std::sqrt(gx * gx + gy * gy);
                if (gl > 1.0f) {
                    fwdB  = RE::NiPoint3{ gx / gl, gy / gl, 0.0f };
                    haveB = true;
                }
            }
        }
        const float cosLimit = std::cos(kBlockerHalfAngleDeg * 3.14159265f / 180.0f);
        auto* const player   = RE::PlayerCharacter::GetSingleton();
        float       bestD    = kBlockerReach + 1.0f;
        RE::Actor*  best     = nullptr;
        const auto consider = [&](RE::Actor& o) {
            if (&o == a_actor || o.IsDead() || o.IsDisabled() || !o.Is3DLoaded())
                return RE::BSContainer::ForEachResult::kContinue;
            const RE::NiPoint3 p = o.GetPosition();
            const float dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
            if (std::fabs(dz) > kBlockerMaxDz) return RE::BSContainer::ForEachResult::kContinue;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > kBlockerReach || d >= bestD) return RE::BSContainer::ForEachResult::kContinue;
            // Inside 1u counts as in front (standing on top of each other).
            const bool inA = d < 1.0f || (dx * fwdA.x + dy * fwdA.y) / d >= cosLimit;
            const bool inB = haveB && d >= 1.0f && (dx * fwdB.x + dy * fwdB.y) / d >= cosLimit;
            if (!inA && !inB) return RE::BSContainer::ForEachResult::kContinue;
            bestD = d;
            best  = &o;
            return RE::BSContainer::ForEachResult::kContinue;
        };
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            pl->ForEachHighActor([&](RE::Actor& o) {
                return (player && &o == player) ? RE::BSContainer::ForEachResult::kContinue : consider(o);
            });
        }
        if (player) consider(*player);
        if (!best) return;
        a_blk.blocker     = best->GetFormID();
        a_blk.blockerKind = best == player              ? APMF_API::kBlocker_Player
                            : best->IsPlayerTeammate() ? APMF_API::kBlocker_Teammate
                                                        : APMF_API::kBlocker_Actor;
        spdlog::info("[travel-leg] 0x{} blocker check: {} 0x{} '{}' {:.0f}u in front (reach {:.0f}u, cone +-{:.0f} deg of "
                     "facing or goal bearing).",
                     Hex(a_actor->GetFormID()), BlockerKindName(a_blk.blockerKind), Hex(a_blk.blocker),
                     best->GetName() ? best->GetName() : "", bestD, kBlockerReach, kBlockerHalfAngleDeg);
    }
