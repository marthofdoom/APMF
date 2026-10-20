#include "PCH.h"
#include "channels/SubstrateBark.h"

#include "channels/TargetPin.h"   // Layer0Installed: the UpdateCombat seat this deny rides
#include "core/Allowance.h"       // SeatVerified, the exact-runtime gates
#include "core/Clock.h"
#include "core/Log.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <string>

// ============================================================================
// THE SUBSTRATE BARK DENY (substrate round R5, fix F-2 of SUBSTRATE-R2-RESEARCH.md section 3; INVARIANTS #0 (j)
// condition 2 (f); marth 2026-10-19: "close the combat-start bark hole").
//
// WHERE THE BARK COMES FROM (PROVEN on 1.6.1170 and 1.5.97, verified rows in brackets):
//   * Actor::UpdateCombat (Character slot 0xE4; AE 38565 0x6B6E70 / SE 37612 0x625700) calls the controller update
//     (AE 33217 / 33218, SE 32470 / 32471), which, while CombatController::startedCombat (+0x35) is 0, calls the
//     controller START (AE 33231 / SE 32484) and then sets +0x35 = 1. 33231 has no other caller, and the 0xE4 slot
//     has exactly one virtual call site on AE (46902 0x83F61C, the per-actor UpdateCombat job, by handle).
//   * The start requests a DETECTION topic (r8d = 5): NormalToCombat (59) when the group detects the target, else
//     NormalToAlert (57), through the combat-dialogue request (AE 44672 / SE 43466) [Substrate.Bark.
//     ControllerStartTopicCall], which hands the controller to the combat-dialogue core (AE 44673 / SE 43467)
//     [Substrate.Bark.DialogueCoreCall]. The same start sends the "entered combat" TESCombatEvent (not this file).
//   * The core takes r8 = [controller] = the CombatGroup and, for a detection topic, the timer at group + 0x58 +
//     (subtype - 55) * 8 [Substrate.Bark.DetectionTimerSlot] (CommonLib CombatGroup::detectionDialogueTimers[11],
//     AITimer {start, duration}), and SKIPS the line while (AI clock - start) <= duration [Substrate.Bark.TimerGate,
//     which also proves the AI clock global, Substrate.Bark.AIClock: AE 0x319B05C id 404125 / SE 0x2F3A644 id
//     517597]. When it does speak it arms that timer itself (AE 0x8025C3). The core takes NO lock around the timer.
//   * The earlier record's "combat-start topic" (StartCombat's AE 0x6B6CFB block) is NOT a line: it tells the
//     actor's scene it entered combat (vfunc 0x4A, 24131) and clears head-track slot 5 (39472); the substrate never
//     opens on an actor in a scene (fix F-1).
//
// THE DENY: the holder group's NormalToAlert ([2], +0x68) and NormalToCombat ([4], +0x78) timers are written to
// {the AI clock now, the TTL + 2 s}. The engine's own gate then skips the line. Nothing else is written; the
// controller start (the CombatInventory, the combat state, the cast road) runs as before.
//
// WHERE AND WHEN (a CombatGroup has NO reference count, review F6 of b1c78b2). The choice: a PROVABLY SAFE POINT,
// not the group's write lock. The write runs in the holder's OWN UpdateCombat (TargetPin's layer 0, before the chain
// below it), on that update's own job thread, immediately before the engine's update reads [controller] and the same
// two timers, unlocked, in the same call. So the group's lifetime here is exactly the one the engine itself relies on
// for its own unlocked read and arm a few instructions later, and the ORDER is guaranteed: the first update of the
// holder's controller is always preceded by this seat (33231's only road is 0xE4). The group's write lock would not
// help: the engine's reader (44673) does not take it, and the game thread's StartCombat return is not ordered
// against the UpdateCombat job (the record is therefore made PENDING before the StartCombat call). A pending record
// whose holder has no controller or no group yet stays pending.
//
// REAL COMBAT (#0 (j) condition 5: never suppress a real fight's behaviour). In the held engagement's detection
// seat, inside the engine's own write-locked update of the group (WriteLockedHere, the music deny's rule), a group
// that shows a real target and still reads EXACTLY Harbinger's two values gets the values it had before the write
// back. The real-combat DROP does the same on the game thread under the group's WRITE lock (BSWriteLockGuard, the
// verified lock rows) for the group DropSub removes the anchor from: a hit or the party's fight never shows the seat
// a real target in the holder's group. The seat also READS the two timers at the first detection pass (about 0.5 s after the group is built): a
// value that is no longer Harbinger's means the engine passed every other gate and armed it, i.e. it SPOKE.
//
// THREADING. Arm / Disarm / Closing / Reset: game thread. BeforeUpdateCombat: any UpdateCombat job (one actor per
// call; two holders in one group write the same two values, which is benign). The two seats: the combat-manager
// job, under the engine's group write lock. The records are atomics; a slot's generation is re-read after the
// fields (a reused slot reads as another record) before anything is written to the engine.
//
// SCOPE. Exactly 1.6.1170 and 1.5.97 (1.7.104: the bark rows have no 1.7.104 entry and a RELOCATION_ID miss is
// fatal there, so the ids are never built: refused by name). [CombatSubstrate] bSubstrateBarkDeny=0 switches it
// off (every OPEN logs the hole).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    // CombatGroup::detectionDialogueTimers index = subtype - 55 (the core's `lea eax,[rdi-0x37]`)
    constexpr int kTimerAlert  = 57 - 55;   // NormalToAlert
    constexpr int kTimerCombat = 59 - 55;   // NormalToCombat

    static_assert(offsetof(RE::CombatGroup, detectionDialogueTimers) == 0x58,
                  "CombatGroup+0x58 detectionDialogueTimers (AE 44673 0x8023BB / SE 43467 0x768EB3 `add rbx,0xB; "
                  "lea rbx,[r8+rbx*8]`): re-verify");
    static_assert(sizeof(RE::AITimer) == 8 && offsetof(RE::AITimer, aiTimer) == 0 && offsetof(RE::AITimer, timer) == 4,
                  "AITimer {start +0, duration +4} (the core's `subss xmm0,[rbx]; comiss xmm0,[rbx+4]`): re-verify");
    static_assert(offsetof(RE::CombatController, combatGroup) == 0x00 && offsetof(RE::CombatController, startedCombat) == 0x35,
                  "CombatController+0 group (the core's `mov r8,[rax]`), +0x35 startedCombat (AE 0x558A2E / SE 0x4FD3A2): "
                  "re-verify");

    // record state bits
    enum : std::uint32_t {
        kUsed      = 1u << 0,   // the record belongs to a substrate (Arm .. Closing)
        kPending   = 1u << 1,   // armed on the holder's next UpdateCombat
        kArmed     = 1u << 2,   // Harbinger's values written
        kLate      = 1u << 3,   // the controller had ALREADY started (+0x35 = 1) at the write: the first update ran first
        kSeen      = 1u << 4,   // the first detection pass read the two timers
        kChanged   = 1u << 5,   // a later read no longer showed Harbinger's values (the engine armed one)
        kRestored  = 1u << 6,   // a real target: the values from before the write were put back
        kNoRestore = 1u << 7,   // a real target, but the timers no longer read Harbinger's values: left alone
    };

    struct Rec {
        std::atomic<std::uint32_t> gen{ 0 };        // bumped by Arm and by every forget (the seats re-read it)
        std::atomic<std::uint32_t> state{ 0 };
        std::atomic<RE::FormID>    holder{ 0 };
        std::atomic<std::uint32_t> holderH{ 0 };
        std::atomic<std::uint32_t> holdBits{ 0 };   // the duration Harbinger writes
        std::atomic<std::uint32_t> clockBits{ 0 };  // the start Harbinger wrote (the AI clock then)
        std::array<std::atomic<std::uint32_t>, 4> orig{};   // {start, duration} x {alert, combat} before the write
        std::array<std::atomic<std::uint32_t>, 4> seen{};   // the first detection pass's read
        std::array<std::atomic<std::uint32_t>, 4> changed{};   // the first read that was not Harbinger's
        std::atomic<std::uint64_t> armAskedMs{ 0 }, armedMs{ 0 }, seenMs{ 0 }, changedMs{ 0 }, restoredMs{ 0 };
    };
    std::array<Rec, apmf::substratebark::kMaxSlots> g_recs{};
    std::atomic<std::uint32_t> g_pending{ 0 };   // records in kPending (BeforeUpdateCombat's pre-gate)
    std::atomic<std::uint32_t> g_armed{ 0 };     // records in kArmed (the seats' pre-gate)

    std::atomic<bool>        g_on{ false };
    std::atomic<const char*> g_off{ "not installed" };
    const volatile float*    g_clock = nullptr;   // the AI clock (verified row Substrate.Bark.AIClock)

    std::uint32_t Bits(float f) { return std::bit_cast<std::uint32_t>(f); }
    float         Flt(std::uint32_t b) { return std::bit_cast<float>(b); }

    bool IsMember(const RE::CombatGroup* a_g, std::uint32_t a_h) {
        if (a_h == 0) return false;
        for (const auto& m : a_g->members)
            if (m.memberHandle.native_handle() == a_h) return true;
        return false;
    }

    // Forget a record (game thread). The generation bump makes a seat that read the old fields skip its write.
    void Forget(Rec& r) {
        const std::uint32_t st = r.state.exchange(0, std::memory_order_acq_rel);
        r.gen.fetch_add(1, std::memory_order_acq_rel);
        if (st & kPending) g_pending.fetch_sub(1, std::memory_order_relaxed);
        if (st & kArmed) g_armed.fetch_sub(1, std::memory_order_relaxed);
        r.holder.store(0, std::memory_order_relaxed);
        r.holderH.store(0, std::memory_order_relaxed);
    }

    // Do the two timers read EXACTLY the values some armed record wrote (Harbinger's)? `a_orig` (optional) gets that
    // record's values from before ITS write. Two holders in one group (a merge) write the same group: the second one
    // ADOPTS the first one's "before" values, so a put-back restores the engine's own.
    bool HarbingerValues(const std::array<std::uint32_t, 4>& a_v, std::array<std::uint32_t, 4>* a_orig) {
        if (a_v[0] != a_v[2] || a_v[1] != a_v[3]) return false;
        for (auto& r : g_recs) {
            if (!(r.state.load(std::memory_order_acquire) & kArmed)) continue;
            if (r.clockBits.load(std::memory_order_relaxed) != a_v[0] || r.holdBits.load(std::memory_order_relaxed) != a_v[1])
                continue;
            if (a_orig)
                for (int k = 0; k < 4; ++k) (*a_orig)[k] = r.orig[k].load(std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    std::array<std::uint32_t, 4> Read(const RE::CombatGroup* a_g) {
        const auto& ta = a_g->detectionDialogueTimers[kTimerAlert];
        const auto& tc = a_g->detectionDialogueTimers[kTimerCombat];
        return { Bits(ta.aiTimer), Bits(ta.timer), Bits(tc.aiTimer), Bits(tc.timer) };
    }

    std::string TimerText(const std::array<std::atomic<std::uint32_t>, 4>& a_v) {
        return fmt::format("NormalToAlert {{{:.2f}, {:.2f}}} NormalToCombat {{{:.2f}, {:.2f}}}",
                           static_cast<double>(Flt(a_v[0].load(std::memory_order_relaxed))),
                           static_cast<double>(Flt(a_v[1].load(std::memory_order_relaxed))),
                           static_cast<double>(Flt(a_v[2].load(std::memory_order_relaxed))),
                           static_cast<double>(Flt(a_v[3].load(std::memory_order_relaxed))));
    }

    std::uint64_t Since(std::uint64_t a_at, std::uint64_t a_from) { return a_at > a_from ? a_at - a_from : 0; }

}

namespace apmf::substratebark {

    const char* Install() {
        char b[8]{};
        REX::W32::GetPrivateProfileStringA("CombatSubstrate", "bSubstrateBarkDeny", "", b, sizeof(b), kIni);
        if (b[0] && std::atoi(b) == 0) {
            g_off.store("[CombatSubstrate] bSubstrateBarkDeny=0 in APMF.ini", std::memory_order_release);
            g_on.store(false, std::memory_order_release);
            return nullptr;
        }
        const bool ae = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
        if (apmf::allowance::IsRuntime1_7_104() || !(ae || REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)))
            return "the bark deny's rows are verified on exactly 1.6.1170 and 1.5.97 only (1.7.104: "
                   "Substrate.Bark.ControllerStartTopicCall / DialogueCoreCall / DetectionTimerSlot / TimerGate / AIClock "
                   "are not in its id table)";
        if (!apmf::targetpin::Layer0Installed())
            return "the bark deny rides Character::UpdateCombat (0xE4) layer 0 (channels/TargetPin.cpp), which was not "
                   "installed at plugin load ([TargetPin] bTargetPin=0, or its row refused; see the [ch.20] lines). "
                   "[CombatSubstrate] bSubstrateBarkDeny=0 runs the substrate without it (the hole is logged per OPEN)";
        const auto start = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(32484, 33231) }.address();
        const auto req   = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43466, 44672) }.address();
        const auto core  = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43467, 44673) }.address();
        const auto clock = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(517597, 404125) }.address();
        bool ok = apmf::allowance::SeatVerified(start + (ae ? 0xA9 : 0xBD), "Substrate.Bark.ControllerStartTopicCall");
        ok = apmf::allowance::SeatVerified(req + (ae ? 0xFB : 0x114), "Substrate.Bark.DialogueCoreCall") && ok;
        ok = apmf::allowance::SeatVerified(core + (ae ? 0x106 : 0x10E), "Substrate.Bark.DetectionTimerSlot") && ok;
        ok = apmf::allowance::SeatVerified(core + (ae ? 0x118 : 0x120), "Substrate.Bark.TimerGate") && ok;
        ok = apmf::allowance::SeatVerified(clock, "Substrate.Bark.AIClock") && ok;
        // the drop's put-back takes the group's write lock (BSWriteLockGuard) on the game thread
        ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RE::Offset::BSReadWriteLock::LockForWrite }.address(),
                                           "CommonLib.BSReadWriteLock.LockForWrite") && ok;
        ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RE::Offset::BSReadWriteLock::UnlockForWrite }.address(),
                                           "CommonLib.BSReadWriteLock.UnlockForWrite") && ok;
        if (!ok)
            return "the address self-check refused a row the bark deny rests on (listed above): without it the substrate "
                   "would switch the combat-start bark on undenied";
        g_clock = reinterpret_cast<const volatile float*>(clock);
        g_off.store("", std::memory_order_release);
        g_on.store(true, std::memory_order_release);
        spdlog::info("[substrate] bark deny ON: the holder group's NormalToAlert / NormalToCombat timers (CombatGroup+0x68 / "
                     "+0x78) pre-armed in the holder's own UpdateCombat (0xE4 layer 0) before its controller's first "
                     "update; put back on a real target. Rows: ControllerStartTopicCall, DialogueCoreCall, "
                     "DetectionTimerSlot, TimerGate, AIClock.");
        return nullptr;
    }

    bool        On() { return g_on.load(std::memory_order_acquire); }
    const char* OffReason() { return g_off.load(std::memory_order_acquire); }

    void Arm(int a_slot, RE::FormID a_holder, std::uint32_t a_holderH, float a_holdSec) {
        if (!On() || a_slot < 0 || a_slot >= kMaxSlots) return;
        Rec& r = g_recs[a_slot];
        if (r.state.load(std::memory_order_acquire) & kUsed) Forget(r);   // a stale record (no Closing): replaced
        r.holder.store(a_holder, std::memory_order_relaxed);
        r.holderH.store(a_holderH, std::memory_order_relaxed);
        r.holdBits.store(Bits(a_holdSec), std::memory_order_relaxed);
        r.clockBits.store(0, std::memory_order_relaxed);
        for (auto* arr : { &r.orig, &r.seen, &r.changed })
            for (auto& v : *arr) v.store(0, std::memory_order_relaxed);
        r.armAskedMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
        for (auto* t : { &r.armedMs, &r.seenMs, &r.changedMs, &r.restoredMs }) t->store(0, std::memory_order_relaxed);
        r.gen.fetch_add(1, std::memory_order_acq_rel);
        g_pending.fetch_add(1, std::memory_order_relaxed);
        r.state.store(kUsed | kPending, std::memory_order_release);   // publishes the fields above
    }

    void Disarm(int a_slot) {
        if (a_slot < 0 || a_slot >= kMaxSlots) return;
        if (g_recs[a_slot].state.load(std::memory_order_acquire) & kUsed) Forget(g_recs[a_slot]);
    }

    void BeforeUpdateCombat(RE::Actor* a_actor) {
        if (g_pending.load(std::memory_order_relaxed) == 0 || !a_actor) return;
        const RE::FormID id = a_actor->GetFormID();
        for (auto& r : g_recs) {
            if (r.holder.load(std::memory_order_acquire) != id) continue;
            const std::uint32_t gen = r.gen.load(std::memory_order_acquire);
            const std::uint32_t st  = r.state.load(std::memory_order_acquire);
            if (!(st & kPending) || r.holder.load(std::memory_order_acquire) != id) continue;
            auto* cc = a_actor->GetActorRuntimeData().combatController;
            auto* g  = cc ? cc->combatGroup : nullptr;
            if (!g || !g_clock) return;   // StartCombat has not built it yet: stays pending
            const float now  = *g_clock;
            const float hold = Flt(r.holdBits.load(std::memory_order_relaxed));
            if (r.gen.load(std::memory_order_acquire) != gen) return;   // forgotten / re-armed meanwhile
            auto& ta = g->detectionDialogueTimers[kTimerAlert];
            auto& tc = g->detectionDialogueTimers[kTimerCombat];
            std::array<std::uint32_t, 4> before = Read(g);
            HarbingerValues(before, &before);   // a co-holder already wrote this group: keep the engine's own values
            for (int k = 0; k < 4; ++k) r.orig[k].store(before[k], std::memory_order_relaxed);
            ta.aiTimer = now;
            ta.timer   = hold;
            tc.aiTimer = now;
            tc.timer   = hold;
            r.clockBits.store(Bits(now), std::memory_order_relaxed);
            r.armedMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
            // pending -> armed in ONE compare-exchange: a Forget (game thread) that won the race leaves the state 0 and
            // the counters to its own exchange.
            const std::uint32_t add = kArmed | (cc->startedCombat ? kLate : 0u);
            std::uint32_t       cur = r.state.load(std::memory_order_acquire);
            while ((cur & (kUsed | kPending)) == (kUsed | kPending) &&
                   !r.state.compare_exchange_weak(cur, (cur & ~kPending) | add, std::memory_order_acq_rel)) {
            }
            if ((cur & (kUsed | kPending)) == (kUsed | kPending)) {
                g_pending.fetch_sub(1, std::memory_order_relaxed);
                g_armed.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
    }

    void SeatAnchorPass(RE::CombatGroup* a_group, bool a_writeLockedHere) {
        if (g_armed.load(std::memory_order_relaxed) == 0 || !a_group || !a_writeLockedHere) return;
        for (auto& r : g_recs) {
            const std::uint32_t gen = r.gen.load(std::memory_order_acquire);
            const std::uint32_t st  = r.state.load(std::memory_order_acquire);
            if (!(st & kArmed) || (st & (kRestored | kNoRestore))) continue;
            if (!IsMember(a_group, r.holderH.load(std::memory_order_relaxed))) continue;
            const std::array<std::uint32_t, 4> v = Read(a_group);
            if (r.gen.load(std::memory_order_acquire) != gen) continue;
            const auto now = apmf::clock::MonotonicMs();
            if (!(st & kSeen)) {
                for (int k = 0; k < 4; ++k) r.seen[k].store(v[k], std::memory_order_relaxed);
                r.seenMs.store(now, std::memory_order_relaxed);
                r.state.fetch_or(kSeen, std::memory_order_acq_rel);
            }
            if (!(st & kChanged) && !HarbingerValues(v, nullptr)) {
                for (int k = 0; k < 4; ++k) r.changed[k].store(v[k], std::memory_order_relaxed);
                r.changedMs.store(now, std::memory_order_relaxed);
                r.state.fetch_or(kChanged, std::memory_order_acq_rel);
            }
        }
    }

    void SeatRealTarget(RE::CombatGroup* a_group, bool a_writeLockedHere) {
        if (g_armed.load(std::memory_order_relaxed) == 0 || !a_group || !a_writeLockedHere) return;
        for (auto& r : g_recs) {
            const std::uint32_t gen = r.gen.load(std::memory_order_acquire);
            const std::uint32_t st  = r.state.load(std::memory_order_acquire);
            if (!(st & kArmed) || (st & (kRestored | kNoRestore))) continue;
            if (!IsMember(a_group, r.holderH.load(std::memory_order_relaxed))) continue;
            std::array<std::uint32_t, 4> o{};
            const bool ours = HarbingerValues(Read(a_group), &o);
            if (r.gen.load(std::memory_order_acquire) != gen) continue;   // re-armed / forgotten meanwhile: not ours
            auto& ta = a_group->detectionDialogueTimers[kTimerAlert];
            auto& tc = a_group->detectionDialogueTimers[kTimerCombat];
            if (ours) {
                ta.aiTimer = Flt(o[0]);
                ta.timer   = Flt(o[1]);
                tc.aiTimer = Flt(o[2]);
                tc.timer   = Flt(o[3]);
            }
            r.restoredMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
            r.state.fetch_or(ours ? kRestored : kNoRestore, std::memory_order_acq_rel);
        }
    }

    void Closing(int a_slot, RE::FormID a_holder, bool a_dropped, std::uint64_t a_nowMs, RE::CombatGroup* a_dropGroup) {
        if (a_slot < 0 || a_slot >= kMaxSlots) return;
        Rec& r = g_recs[a_slot];
        // The real-combat DROP: the seat's put-back needs a locked detection pass that sees the real target, which a
        // hit or the party's fight (the foe not in this group), or a first sighting outside the locked update, never
        // gives. So the drop puts the values back itself, on the game thread under the group's WRITE lock, on the group
        // DropSub reads from the holder's controller for its RemoveTarget (the same game-thread exposure, APMF-B26).
        if (a_dropped && a_dropGroup) {
            const std::uint32_t s0 = r.state.load(std::memory_order_acquire);
            if ((s0 & kUsed) && (s0 & kArmed) && !(s0 & (kRestored | kNoRestore)) &&
                r.holder.load(std::memory_order_acquire) == a_holder) {
                RE::BSWriteLockGuard         lk(a_dropGroup->lock);
                std::array<std::uint32_t, 4> o{};
                const bool                   ours = HarbingerValues(Read(a_dropGroup), &o);
                if (ours) {
                    auto& ta   = a_dropGroup->detectionDialogueTimers[kTimerAlert];
                    auto& tc   = a_dropGroup->detectionDialogueTimers[kTimerCombat];
                    ta.aiTimer = Flt(o[0]);
                    ta.timer   = Flt(o[1]);
                    tc.aiTimer = Flt(o[2]);
                    tc.timer   = Flt(o[3]);
                }
                r.restoredMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
                r.state.fetch_or(ours ? kRestored : kNoRestore, std::memory_order_acq_rel);
            }
        }
        const std::uint32_t st = r.state.load(std::memory_order_acquire);
        if (!(st & kUsed) || r.holder.load(std::memory_order_acquire) != a_holder) {
            if (!On())
                spdlog::warn("[substrate] 0x{} BARK not denied: the bark deny is OFF ({}); the controller's first update "
                             "could say NormalToCombat / NormalToAlert.",
                             Hex(a_holder), OffReason());
            return;
        }
        const std::uint64_t t0 = r.armAskedMs.load(std::memory_order_relaxed);
        try {
            std::string what;
            if (st & kPending) {
                what = fmt::format("NEVER ARMED: no UpdateCombat of the holder with a controller and a group ran in {} ms "
                                   "(no first controller update either, so no bark)",
                                   Since(a_nowMs, t0));
            } else {
                what = fmt::format("armed at +{} ms in the holder's UpdateCombat (0xE4 layer 0) to start {:.2f}, "
                                   "duration {:.1f} s (before: {}){}",
                                   Since(r.armedMs.load(std::memory_order_relaxed), t0),
                                   static_cast<double>(Flt(r.clockBits.load(std::memory_order_relaxed))),
                                   static_cast<double>(Flt(r.holdBits.load(std::memory_order_relaxed))), TimerText(r.orig),
                                   (st & kLate) ? " -- LATE: the controller had ALREADY started (+0x35 = 1), its first "
                                                  "update ran before this seat and may have barked"
                                                : "");
                what += (st & kSeen)
                            ? fmt::format("; first detection pass +{} ms read {}", Since(r.seenMs.load(std::memory_order_relaxed), t0),
                                          TimerText(r.seen))
                            : std::string("; no detection pass of his group read the timers while open");
                what += (st & kChanged)
                            ? fmt::format("; CHANGED at +{} ms to {} -- not Harbinger's value: the engine armed a timer "
                                          "itself, i.e. a NormalToAlert / NormalToCombat line was SAID (or the group was "
                                          "re-armed by another holder)",
                                          Since(r.changedMs.load(std::memory_order_relaxed), t0), TimerText(r.changed))
                            : (st & kSeen) ? std::string("; Harbinger's value HELD at every read (no line)")
                                           : std::string();
                if (st & kRestored)
                    what += fmt::format("; a real target at +{} ms: the values from before the write PUT BACK",
                                        Since(r.restoredMs.load(std::memory_order_relaxed), t0));
                else if (st & kNoRestore)
                    what += fmt::format("; a real target at +{} ms: NOT put back (the timers no longer read Harbinger's "
                                        "values)",
                                        Since(r.restoredMs.load(std::memory_order_relaxed), t0));
                else if (a_dropped)
                    what += "; dropped for real combat with no group to put the values back in";
            }
            spdlog::info("[substrate] 0x{} BARK DENY result: {}.", Hex(a_holder), what);
        } catch (...) {
        }
        Forget(r);
    }

    void Reset() {
        for (auto& r : g_recs)
            if (r.state.load(std::memory_order_acquire) & kUsed) Forget(r);
    }

}
