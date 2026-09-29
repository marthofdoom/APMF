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

// Channel 19 -- TRAVEL: the declared gait written into the package record, and the
// passive read-back of the engine's running copy. See Travel.cpp for the design.

    // ---- ABI v12: GAIT -------------------------------------------------------------

    const char* SpeedName(std::uint32_t a_speed) {
        switch (a_speed) {
        case 0:  return "Walk";
        case 1:  return "Jog";
        case 2:  return "Run";
        case 3:  return "FastWalk";
        default: return "authored";
        }
    }

    // Write the leg's declared gait (kTravel_SpeedSet + bits 3-4) into its package record,
    // or put the record's AUTHORED speed back when the leg declares none, so a gait never
    // leaks from one leg to the next leg on the same record. Returns what the leg-state
    // mirror reports as `speed` (0..3, or 0xFFFFFFFF when nothing was written). GAME
    // THREAD, called right after a successful PointPackage on the same record.
    //
    // THE ENGINE PATH, read on both images (2026-09-24), not assumed:
    //   * When a package STARTS on an actor, the engine copies the record into the actor's
    //     running-package state (ActorPackage): 1.6.1170 0x6CE2C0 (id 38970) / 1.5.97
    //     0x63BD40 (id 38015) do `mov eax,[pkg+0x20]; mov [ap+0x24],eax` (packFlags ->
    //     modifiedPackageFlag) and `call getter; mov [ap+0x2B],al`, where the getter
    //     (1.6.1170 0x491340 id 29543 / 1.5.97 0x4363E0 id 28769) is `movzx eax, byte
    //     [pkg+0x26]; ret` (PACKAGE_DATA::maxSpeed).
    //   * The movement code reads that COPY: e.g. 1.6.1170 0x482841 / 1.5.97 0x4280C1 take
    //     the running ActorPackage (process->middleHigh->runOncePackage when it holds a
    //     package, else process->currentPackage: 1.6.1170 0x70F590 / 1.5.97 0x67BDD0), test
    //     bit 13 of +0x24 (0x2000, kPreferredSpeed) and only then pass +0x2B on.
    // So the record's flag and byte are the engine's own gait input, read at package start.
    // APMF writes them BEFORE the ch.9 offer is queued (a fresh leg), so no actor can start
    // this record between the two writes. A live re-point that changes the gait rewrites
    // the record, but the running copy keeps the old speed until the package next starts:
    // that is logged, never hidden.
    std::uint32_t ApplyGait(RE::FormID a_id, const Leg& a_leg, int a_slot, bool a_liveRepoint) {
        using Flag  = RE::PACKAGE_DATA::GeneralFlag;
        using Speed = RE::PACKAGE_DATA::PreferredSpeed;
        RE::TESPackage* pkg = (a_slot >= 0 && static_cast<std::size_t>(a_slot) < kTravelSlots) ? g_pkg[a_slot] : nullptr;
        if (!pkg) return 0xFFFFFFFFu;

        const std::uint32_t want = DeclaredSpeed(a_leg.flags);
        if (!g_gaitVerified) {
            if (want != 0xFFFFFFFFu) {
                spdlog::error("[travel-gait] 0x{} declared gait {} NOT applied -- the running build ({}) is not one the "
                              "package speed path was read on (1.6.1170 / 1.5.97). The leg walks at the record's "
                              "authored speed, and GetTravelLegState reports speed = none.",
                              Hex(a_id), SpeedName(want), REL::Module::get().version().string("."));
            }
            return 0xFFFFFFFFu;
        }

        auto&      pd          = pkg->packData;
        const bool beforeFlag  = pd.packFlags.all(Flag::kPreferredSpeed);
        const auto beforeSpeed = pd.maxSpeed.underlying();
        if (want == 0xFFFFFFFFu) {
            // Clear the flag BEFORE touching the byte, set it AFTER, so a reader never pairs
            // the flag with a byte that is not the authored one.
            if (!g_authoredPrefSpeed[a_slot]) pd.packFlags.reset(Flag::kPreferredSpeed);
            pd.maxSpeed = static_cast<Speed>(g_authoredSpeed[a_slot]);
            if (g_authoredPrefSpeed[a_slot]) pd.packFlags.set(Flag::kPreferredSpeed);
        } else {
            // The byte first, then the flag that makes the engine honour it.
            pd.maxSpeed = static_cast<Speed>(want);
            pd.packFlags.set(Flag::kPreferredSpeed);
        }
        const bool changed = beforeFlag != pd.packFlags.all(Flag::kPreferredSpeed) ||
                             beforeSpeed != pd.maxSpeed.underlying();
        if (want != 0xFFFFFFFFu) {
            spdlog::info("[travel-gait] 0x{} slot {} package 0x{}: gait {} written (PKDT flag 0x2000 set, speed byte "
                         "{}).", Hex(a_id), a_slot, Hex(pkg->GetFormID()), SpeedName(want), want);
        }
        if (a_liveRepoint && changed) {
            spdlog::warn("[travel-gait] 0x{} re-point CHANGED the gait of a live leg (record now {}). The engine copies "
                         "the speed when the package STARTS, so the actor keeps the gait it started with until the "
                         "package next starts. Release and re-request to change gait mid-walk.",
                         Hex(a_id), SpeedName(want));
        }
        return want;
    }

    // One passive line per leg that declared a gait: what the ENGINE's running-package copy
    // holds once our package runs, which is the field proof the write above lands. Read
    // under the ActorPackage's own spin lock. GAME THREAD (Poll).
    void ObserveGait(RE::FormID a_id, RE::Actor* a_actor, Leg& a_leg) {
        if (a_leg.gaitLogged || DeclaredSpeed(a_leg.flags) == 0xFFFFFFFFu) return;
        if (a_leg.slot < 0 || !g_pkg[a_leg.slot]) return;
        auto* proc = a_actor->GetActorRuntimeData().currentProcess;
        if (!proc) return;
        RE::ActorPackage* ap = (proc->middleHigh && proc->middleHigh->runOncePackage.package)
                                   ? &proc->middleHigh->runOncePackage
                                   : &proc->currentPackage;
        RE::TESPackage* running = nullptr;
        std::uint32_t   flags   = 0;
        std::int32_t    speed   = -1;
        {
            RE::BSSpinLockGuard lk(ap->packageLock);
            running = ap->package;
            flags   = ap->modifiedPackageFlag;
            speed   = ap->preferredSpeed;
        }
        if (running != g_pkg[a_leg.slot]) return;   // not running yet; try again next poll
        a_leg.gaitLogged = true;
        spdlog::info("[travel-gait] 0x{} running package 0x{}: engine copy preferred-speed flag={} speed={} ({}); "
                     "declared {}.", Hex(a_id), Hex(running->GetFormID()), (flags & 0x2000u) != 0, speed,
                     SpeedName(static_cast<std::uint32_t>(speed)), SpeedName(DeclaredSpeed(a_leg.flags)));
    }
