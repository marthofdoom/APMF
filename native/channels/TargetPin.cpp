#include "PCH.h"
#include "channels/TargetPin.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/Sightline.h"   // ABI v18 kTargetPin_OwnLineOfSight
#include "core/Sources.h"     // Harbinger 1a: target capture, census, priorities
#include "channels/CombatEntry.h"   // THE HELD ENGAGEMENT (2026-10-12): a pin is held; kLost no longer ends it

#include <intrin.h>   // _ReturnAddress: the 0xE4 layers' segment proof
#include <shared_mutex>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/Travel.cpp and core/EquipSink.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 20 -- TARGET PIN (kIntent_TargetPin, ABI v13, marth 2026-09-25, ClickUp
// 86e3cr9u7: "yes, start the target pin"; design correction the same day: "APMF offers
// pinning, it must block engine pinning").
//
// WHAT IT IS. A client claims {actor, target}. While that claim is the WINNING
// kIntent_TargetPin claim on the actor, the ENGINE'S OWN TARGET SELECTION IS ANSWERED
// WITH THE PINNED TARGET AT ITS SOURCE, so the engine's own pick never reaches the
// controller. The NPC's own AI then fights that target: attacks, movement, spells, equips.
//
// WHERE THE ENGINE PICKS (disassembly of both unpacked images, 2026-09-25):
//   Actor::UpdateCombat (Character vtable slot 0xE4; AE 0x6B6E70 / SE 0x625700) calls
//   CombatController::UpdateTarget exactly once (AE 0x6B6F1C -> 0x559930, SE 0x6257AC ->
//   0x4FE300; the only caller on each). UpdateTarget walks the controller's
//   targetSelectors array and, for each active selector (flags +0x20: bit0 set, bit1
//   clear), calls its vtable SLOT 6 -- `ActorHandle* SelectTarget(this, ActorHandle* out)`,
//   a hidden-return-slot call: rdx = out, the 4-byte handle is stored at [rdx] and rdx is
//   returned. The FIRST non-zero answer wins; if it differs from the controller's
//   targetHandle (+0x2C) UpdateTarget calls CombatController::SetTarget (AE 0x559630 /
//   SE 0x4FDFE0), which writes targetHandle AND the actor's currentCombatTarget. SetTarget
//   has two callers per runtime: UpdateTarget, and one that passes 0 (the engine
//   CLEARING the target). Nothing else in the CombatController code writes +0x2C
//   besides its constructor.
//   The selector classes are CombatTargetSelectorStandard (the threat/scoring picker,
//   slot 6 AE 0x84CFE0 / SE 0x7B5E50) and CombatTargetSelectorFixed (returns the handle
//   it was built with, AE 0x84DAF0 / SE 0x7B6920); the base's slot 6 is a purecall.
//   Selector layout, from both constructors on both runtimes (AE 0x84CFBA / 0x84DA3D,
//   SE 0x7B5E2A / 0x7B686D): +0x10 CombatController*, +0x18 handle, +0x1C priority,
//   +0x20 flags. CommonLib declares these classes only by name, so +0x10 is a raw offset
//   and carries the INVARIANTS #20 guards: an install-time address self-check on both
//   vtables, a per-call vtable-identity check, and the INI kill-switch.
//
// THE SEAT (the source deny). write_vfunc on slot 6 of BOTH selector vtables, chaining:
// the original runs first (the selector keeps its own bookkeeping), then, for an actor
// whose winning kIntent_TargetPin claim resolves, the answer is REPLACED with the pinned
// handle when ALL of these hold:
//   * the engine's own answer is non-zero -- the engine holds a target, i.e. the actor is
//     fighting. A zero answer means "no target" and is left alone: the pin never starts a
//     fight and never revives one the engine ended (INVARIANTS #0 (f) condition 2);
//   * the pinned target is alive, enabled and loaded;
//   * the pinned target is IN THE ACTOR'S COMBAT GROUP'S TARGET LIST
//     (`combatController->combatGroup->targets`, read under the group's own read lock).
//     APMF only chooses among the targets the engine itself holds; a non-foe becomes
//     pinnable when something (the client's own combat entry) makes it a combat target.
//     A pinned target outside the list is NOT written -- counted and logged, rate-limited.
// The engine's pick is then never seen by SetTarget, the controller, the actor's
// currentCombatTarget or anything UpdateCombat reads after UpdateTarget (the process-side
// read of +0x2C at AE 0x6B6F34 / SE 0x6257C4 included). That is the "block engine
// pinning" marth asked for.
//
// HARBINGER 1a (2026-10-11, plan harbinger-controller-plan-2026-10-11.md, marth Q1/Q4/Q13):
// the 0xE4 observer below is now LAYER 0 of Harbinger's target capture. It still judges the
// two failures described next, and in addition attributes an OVERWRITTEN to the hook that
// wrote it (census of the 0xE4 chain, core/Sources) and, when the pin claim's priority is
// strictly higher than that writer's, RESTORES the pin at the outermost layer after every
// chained hook ran, through the engine's own CombatController::SetTarget. Each segment of the
// 0xE4 chain is proven to hold one module; a write in an unproven segment is a DEFECT, allowed
// and alerted. INVARIANTS #0 (f) condition 4 as amended 2026-10-11. See "THE 0xE4 LAYERS" below.
// THE OBSERVER (not a belt) -- the original design, kept as layer 0's judgement. Character
// vtable slot 0xE4 (UpdateCombat) is also hooked; before Harbinger 1a it wrote nothing. For a pinned actor it snapshots the selector seat's
// counters before the engine's update and, after it, checks the outcome. If the actor
// ends the update aimed at someone else while the seat was NEVER CALLED, that is a
// SOURCE-SEAT MISS and it is logged loudly (principle 7: a miss must show, not be
// papered over by a rewrite). If the seat DID answer with the pin and the actor still
// ends aimed elsewhere, something wrote after the seat (another framework, another
// path) -- logged as OVERWRITTEN. The previous cut's post-update rewrite is GONE: it was
// the mechanism marth rejected, and keeping it as a belt would hide exactly those two
// failures.
//
// THE PIN ENDS BY ITSELF (marth 2026-09-25: "If APMF can no longer track the target, it's
// lost, and dropped" -- SUPERSEDED for "lost" by marth 2026-10-12: "once an engagement is called
// in harbinger its move-to, it cannot be lost". A pin is a HELD engagement (channels/CombatEntry.cpp
// "THE HELD ENGAGEMENT", INVARIANTS #0 (i)): kTargetLost makes the seat DECLINE that update and
// nothing more, unless [EngagementHold] bEngagementHold=0 restores the old end). The seat declines
// and records a reason when the pinned target is dead, disabled, not loaded or unresolvable (and,
// hold off only, LOST: its group entry carries kTargetLost: CombatTarget::flags, u16 +0xA6, bit 1,
// verified on both runtimes). targetpin::Poll() -- the
// Arbiter's once-per-frame game-thread seat, the ch.19 leg-monitor precedent -- checks the
// same target conditions LIVE plus the OWNER being dead -- trusting only the seat's "target
// lost" (the group list it must not read itself), and clearing any other seat reason the live
// check does not confirm (a worker can see a transient null 3D) -- and ENDS the winning claim itself
// (ControlMap::EnqueueRelease), logging "pin ended: <reason>"; the Release line repeats the
// reason. A target merely NOT (YET) in the group's targets does not end the pin: it
// declines, because the client's combat entry may still add it.
//
// DENY-COMPLETENESS (principle 2), stated plainly. The facet is "which actor this NPC
// fights". Competing sources:
//   * the ENGINE'S OWN pick (both selector classes) -- DENIED at the source, above.
//   * the engine CLEARING the target (SetTarget(0): combat ending, the foe lost) -- NOT
//     denied, by design (#0 (f) condition 2).
//   * another framework's OWN currentCombatTarget / targetHandle write (e.g. a later
//     UpdateCombat hook, SmartNPCTargetSelector, MFO's own Targeting hook until MFO
//     defers) -- since Harbinger 1a ARBITRATED BY PRIORITY at the outermost 0xE4 layer:
//     restored when the claim outranks the named writer, kept otherwise, and allowed +
//     alerted when the writer cannot be named.
//   * NOTHING ELSE is claimed: no attack selection, casting, equip, movement, aggression
//     or combat entry.
//
// WHAT THE CLIENT OWNS (principle 2 scope, marth 2026-09-25). The world's reaction to the
// fight the client declared -- crime, bounty, faction, aggression, allies joining --
// happens as the engine does it and is not undone on release.
//
// THREADING. UpdateCombat runs as a BSJobs worker job, several actors in parallel (MFO
// ENGINE_NOTES 0.47 item 3); UpdateTarget and so the selector seat run inside it, for
// that one actor. The seat:
//   * reads the CLAIM from the ControlMap's published RCU snapshot (lock-free);
//   * reads the pinned handle from this file's map under a shared_lock (filled on the
//     game thread at Engage / OnOwnerChanged -- the seat never looks up a form);
//   * resolves the attacking actor from the controller's attackerHandle (a handle-table
//     read, the same one core/CastSeats.cpp does on the combat thread);
//   * takes the combat GROUP's read lock (BSReadLockGuard, the engine's own
//     BSReadWriteLock::LockForRead, self-checked row). LockForRead is recursive for a
//     thread that already holds the write lock (AE 0xCC90C0: compares the writer thread
//     id, then `lock inc` the count), so taking it inside the engine's update cannot
//     self-deadlock.
// KNOWN EXPOSURE, ACCEPTED (review F2, SEV-4, threading carve-out, decided 2026-09-25):
// a StopCombat on ANOTHER thread against this same actor while the seat or the observer
// runs (a script, a package evaluation, a kill -- 0.47 item 4) frees the controller under
// the read. That is identical to vanilla's own UpdateCombat body and to MFO's hook. The
// closure is a StopCombat (slot 0xE5) seat sharing a per-actor lock, which belongs to the
// combat-substrate task. Recorded in Docs/REVIEW-BACKLOG.md.
//
// VERSION ROBUSTNESS. All three seats are vtable slots (principle 11: no call-site
// patch), each on a vtable that is a self-checked row derived from our own executables
// (spec.json: Character slot 0xE4, CombatTargetSelectorStandard / Fixed slot 0x06),
// exact 1.6.1170 / 1.5.97 only, VR refused, [TargetPin] bTargetPin. If ANY of the three
// is refused, none is installed and every claim is refused: a pin with half a mechanism
// is worse than none.
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    // Selector layout (see the header). Raw because CommonLib declares the classes by
    // name only; guarded by the per-call vtable-identity check in SelectorSeat.
    constexpr std::uintptr_t kSelectorController = 0x10;

    // Rate limit for the per-pin warning lines (not-a-combat-target, seat miss,
    // overwritten). Counters are always kept; only the log is limited.
    constexpr std::uint64_t kLogEveryMs = 5000;

    // The ONE seat-recorded end reason Poll() trusts without a live re-check: it comes from
    // the group's target list, which Poll must not read off the combat job (APMF-B26). Every
    // other reason is re-checked live on the game thread (review closing round F1: a worker
    // can see a null Get3D2 during a 3D rebuild -- a transform, a skeleton swap, Reset3D, a
    // script Disable+Enable -- so one worker observation must not end the pin).
    constexpr const char* kReasonLost = "target lost";
    // Review F-4 ((g) condition 5, "never live and inert"): the target was one of the group's targets and
    // is not any more (the engine dropped it, or the fight it was in ended). Trusted like kReasonLost.
    constexpr const char* kReasonAbsent   = "target no longer a combat target (it was one)";

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the seat installs there)" };

    // One entry per actor with an engaged ch.20. Written ONLY on the game thread under a
    // unique_lock; read by the seats on BSJobs workers under a shared_lock. The counters
    // are the seats' only mutation and are atomics, so a shared_lock is enough for them.
    struct Pin {
        RE::FormID      target = 0;    // the claim's param.form this handle was resolved for
        RE::ActorHandle handle{};      // resolved on the game thread; never looked up by the seats
        // selector seat
        mutable std::atomic<std::uint32_t> seatHits{ 0 };        // SelectTarget called for this actor
        mutable std::atomic<std::uint32_t> denied{ 0 };          // engine's pick replaced by the pin
        mutable std::atomic<std::uint32_t> held{ 0 };            // engine's pick already was the pin
        mutable std::atomic<std::uint32_t> noEngineTarget{ 0 };  // engine answered "no target": left alone
        mutable std::atomic<std::uint32_t> notCombatTarget{ 0 }; // pin not in combatGroup->targets
        mutable std::atomic<std::uint32_t> targetGone{ 0 };      // target dead/disabled/unloaded/unresolved
        mutable std::atomic<std::uint32_t> targetLost{ 0 };      // group entry flagged kTargetLost
        mutable std::atomic<std::uint32_t> notInSight{ 0 };      // ABI v18: own ray not VISIBLE, pin paused
        mutable std::atomic<std::uint64_t> lastNotInSightLogMs{ 0 };
        // observer
        mutable std::atomic<std::uint32_t> seatMissed{ 0 };      // aimed elsewhere, seat never called
        mutable std::atomic<std::uint32_t> overwritten{ 0 };     // seat pinned, something wrote after
        mutable std::atomic<bool>          firstDenyLogged{ false };
        // The seat's last decision, for the observer (F-A): 0 never called, 1 answered
        // (denied or held), 2 declined.
        mutable std::atomic<std::uint8_t>  lastDecision{ 0 };
        // "Can't hold it any more" (marth 2026-09-25: "If APMF can no longer track the
        // target, it's lost, and dropped"). The seat records a reason it saw on a worker
        // (target lost / dead / disabled / unloaded / unresolvable); Poll() -- game
        // thread -- ends the claim and sets `ending` so neither path acts twice.
        mutable std::atomic<const char*>   seatEndReason{ nullptr };
        mutable std::atomic<bool>          ending{ false };
        mutable std::atomic<const char*>   endedReason{ nullptr };   // for the Release line
        mutable std::atomic<std::uint64_t> lastNotTargetLogMs{ 0 };
        mutable std::atomic<std::uint64_t> lastLostLogMs{ 0 };      // the held "flagged LOST" line
        mutable std::atomic<bool>          everPresent{ false };    // the target has been in the group's targets
        mutable std::atomic<std::uint64_t> lastMissLogMs{ 0 };
        mutable std::atomic<std::uint64_t> lastOverwriteLogMs{ 0 };
    };

    std::shared_mutex                   g_pinMx;
    std::unordered_map<RE::FormID, Pin> g_pins;
    // Relaxed pre-gate: the seats fire for EVERY combatant in the world; with no pin
    // anywhere this one load is their whole cost. Updated under g_pinMx's unique_lock.
    std::atomic<std::size_t>            g_pinCount{ 0 };

    // The exact vtables the two selector thunks were installed on (per-call identity).
    std::uintptr_t g_vtStandard = 0;
    std::uintptr_t g_vtFixed    = 0;

    // AE +8 LAYOUT GUARD: the controller members read here must stay below +0x68 (the
    // fork static_asserts sizeof(CombatController) == 0x68 and moves the rest behind
    // GetRuntimeData()).
    static_assert(offsetof(RE::CombatController, combatGroup) < 0x68);
    static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68);
    static_assert(offsetof(RE::CombatController, targetHandle) < 0x68);
    static_assert(offsetof(RE::CombatController, previousTargetHandle) == 0x30);   // Harbinger 1a restore
    // CombatGroup: targets at +0x08 (BSTArray: data +0x08, size +0x18), CombatTarget
    // stride 0xA8 with the handle at +0x00, lock at +0x160 -- verified on both runtimes
    // (group ctor AE 0x803240 / SE 0x769DF0; target walk AE 0x804A10 / SE 0x76B0F0).
    static_assert(offsetof(RE::CombatGroup, targets) == 0x08);
    static_assert(offsetof(RE::CombatGroup, lock) == 0x160);
    static_assert(sizeof(RE::CombatTarget) == 0xA8);
    static_assert(offsetof(RE::CombatTarget, targetHandle) == 0x00);
    static_assert(sizeof(RE::ActorHandle) == sizeof(std::uint32_t));

    bool RateOk(std::atomic<std::uint64_t>& last) {
        const auto now  = apmf::clock::MonotonicMs();
        auto       prev = last.load(std::memory_order_relaxed);
        return now - prev >= kLogEveryMs && last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
    }

    // Where is `h` in the group's target list? Read under the group's own read lock.
    // kLost: the entry is there but flagged kTargetLost (CombatTarget::flags, a u16 at
    // +0xA6, bit 1 -- verified on both runtimes: the engine sets it with `or cx, 2` at
    // AE 0x8053EA / SE 0x76B886 and reads it with `shr al,1; and al,1` at AE 0x802B61 /
    // SE 0x769621). A lost target is one the engine can no longer locate.
    enum class Membership { kAbsent, kHeld, kLost };
    Membership GroupMembership(RE::CombatGroup* group, const RE::ActorHandle& h) {
        RE::BSReadLockGuard guard(group->lock);
        for (const auto& t : group->targets) {
            if (t.targetHandle == h)
                return t.flags.any(RE::CombatTarget::Flags::kTargetLost) ? Membership::kLost : Membership::kHeld;
        }
        return Membership::kAbsent;
    }

    // The selector seat body, shared by both vtables. `out` is what the original
    // returned (the engine's own answer slot).
    void AnswerSelection(void* a_self, std::uint32_t* a_out, std::uintptr_t a_expectedVt) {
        if (g_pinCount.load(std::memory_order_relaxed) == 0 || !a_self || !a_out) return;
        if (*reinterpret_cast<const std::uintptr_t*>(a_self) != a_expectedVt) return;   // #20 identity

        auto* cc = *reinterpret_cast<RE::CombatController* const*>(
            reinterpret_cast<std::uintptr_t>(a_self) + kSelectorController);
        if (!cc) return;
        auto        attackerPtr = cc->attackerHandle.get();
        RE::Actor*  attacker    = attackerPtr.get();
        if (!attacker) return;

        const RE::FormID     self = attacker->GetFormID();
        APMF_API::APMF_Param claim{};
        float                claimBasis = 0.0f;
        APMF_API::Handle     claimHandle = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(self, APMF_API::kIntent_TargetPin, claim, claimBasis,
                                                            &claimHandle))
            return;
        if (claim.form == 0) return;

        std::shared_lock lk(g_pinMx);
        const auto it = g_pins.find(self);
        if (it == g_pins.end() || it->second.target != claim.form) return;
        const Pin& pin = it->second;
        pin.seatHits.fetch_add(1, std::memory_order_relaxed);
        const auto decline = [&pin] { pin.lastDecision.store(2, std::memory_order_relaxed); };
        const auto endWith = [&pin](const char* why) {
            if (why == kReasonLost || why == kReasonAbsent) {   // trusted reasons win over a transient one
                pin.seatEndReason.store(why, std::memory_order_relaxed);
                return;
            }
            const char* none = nullptr;
            pin.seatEndReason.compare_exchange_strong(none, why, std::memory_order_relaxed);
        };
        if (pin.ending.load(std::memory_order_relaxed)) {   // Harbinger is ending this claim
            decline();
            return;
        }

        const std::uint32_t engineAnswer = *a_out;
        if (engineAnswer == 0) {   // the engine holds no target: never start or revive a fight
            pin.noEngineTarget.fetch_add(1, std::memory_order_relaxed);
            decline();
            return;
        }

        auto       targetPtr = pin.handle.get();
        RE::Actor* target    = targetPtr.get();
        const char* gone = !target               ? "target unresolvable" :
                           target->IsDead()      ? "target dead" :
                           target->IsDisabled()  ? "target disabled" :
                           !target->Is3DLoaded() ? "target unloaded" : nullptr;
        if (gone) {
            pin.targetGone.fetch_add(1, std::memory_order_relaxed);
            endWith(gone);   // Poll() ends the claim on the game thread
            decline();
            return;
        }

        auto* group = cc->combatGroup;
        const Membership m = group ? GroupMembership(group, pin.handle) : Membership::kAbsent;
        if (m != Membership::kAbsent) pin.everPresent.store(true, std::memory_order_relaxed);
        if (m == Membership::kLost) {
            const auto n = pin.targetLost.fetch_add(1, std::memory_order_relaxed) + 1;
            // Held only while the knowledge seats keep the engine's knowledge alive (review F-4): without them
            // a lost target stays lost and the pin would outlive it inert -- the old end applies.
            if (apmf::combatentry::hold::Enabled() && apmf::combatentry::hold::KnowledgeInstalled()) {
                // THE HELD ENGAGEMENT (marth 2026-10-12: "once an engagement is called in harbinger its
                // move-to, it cannot be lost"): the engine's detection losing the target does NOT end the
                // pin. The knowledge seats (channels/CombatEntry.cpp) un-lose a held target right after
                // the engine flags it; a reader here can still see the flag for that instant, or for
                // longer when those seats are not installed. This update declines (the engine's pick
                // stands, usually the same target); the pin stays.
                decline();
                if (RateOk(pin.lastLostLogMs)) {
                    spdlog::info("[ch.20] 0x{} pin 0x{} is flagged LOST in the group entry -- HELD (not ended): "
                                 "the engine's pick stands this update. ({} so far)",
                                 Hex(self), Hex(claim.form), n);
                }
                return;
            }
            endWith(kReasonLost);   // hold off / seats not installed: the pre-2026-10-12 contract -- the pin ends
            decline();
            return;
        }
        if (engineAnswer == pin.handle.native_handle()) {
            pin.held.fetch_add(1, std::memory_order_relaxed);
            pin.lastDecision.store(1, std::memory_order_relaxed);
            // The engine already picked the pin: nothing was denied or changed, so this is NOT "Served" (menu
            // 2026-10-18: the counter was ~90% no-ops). Served is credited where the pick is actually denied below.
            // The client is still ACTIVE on the facet though: stamp last-seen only (menu winner window).
            apmf::sources::NoteActive(apmf::sources::ClientForHandle(claimHandle), apmf::sources::Facet::kTarget);
            return;
        }
        if (m == Membership::kAbsent && pin.everPresent.load(std::memory_order_relaxed)) {
            endWith(kReasonAbsent);   // review F-4: it WAS a group target and is not: the pin ends
            decline();
            return;
        }
        if (m == Membership::kAbsent) {
            decline();
            const auto n = pin.notCombatTarget.fetch_add(1, std::memory_order_relaxed) + 1;
            if (RateOk(pin.lastNotTargetLogMs)) {
                spdlog::info("[ch.20] 0x{} pin 0x{} is NOT a combat target of this actor's group ({}) -- the "
                             "engine's own pick stands. APMF only chooses among targets the engine holds. ({} so "
                             "far)",
                             Hex(self), Hex(claim.form), group ? "not in combatGroup->targets" : "no combat group",
                             n);
            }
            return;
        }
        // ABI v18 kTargetPin_OwnLineOfSight: the client asked for the pin only while the actor
        // can SEE the target by Harbinger's own ray. Not VISIBLE (occluded, unknown = not
        // measured yet or stale, unavailable) -> PAUSE: the engine's own pick stands this
        // update, like the pauses above. Lock-free read by FormID; it also keeps the pair
        // measured while the seat polls.
        if ((static_cast<std::uint32_t>(claim.ival) & APMF_API::kTargetPin_OwnLineOfSight) != 0) {
            const auto r = apmf::sightline::Read(self, claim.form, true);
            const auto v = apmf::sightline::FreshVerdict(r);
            if (v != APMF_API::kLos_Visible) {
                decline();
                apmf::sightline::NoteSeatHold();
                const auto n = pin.notInSight.fetch_add(1, std::memory_order_relaxed) + 1;
                if (RateOk(pin.lastNotInSightLogMs)) {
                    spdlog::info("[ch.20] 0x{} pin 0x{} PAUSED: own line of sight is {} -- the engine's own pick "
                                 "stands until the target is visible again. ({} so far)",
                                 Hex(self), Hex(claim.form), apmf::sightline::VerdictName(v), n);
                }
                return;
            }
            apmf::sightline::NoteSeatPass();
        }

        *a_out = pin.handle.native_handle();   // DENY the engine's pick at its source
        pin.denied.fetch_add(1, std::memory_order_relaxed);
        pin.lastDecision.store(1, std::memory_order_relaxed);
        apmf::sources::NoteServedClaim(claimHandle, apmf::sources::Facet::kTarget);   // the engine's pick denied
        apmf::sources::NoteHit(apmf::sources::kEngineSource, apmf::sources::Facet::kTarget);        // the engine's pick was
        apmf::sources::NoteOverruled(apmf::sources::kEngineSource, apmf::sources::Facet::kTarget);  //   the source denied (menu Hits/Blocked)
        if (!pin.firstDenyLogged.exchange(true, std::memory_order_relaxed)) {
            spdlog::info("[ch.20] 0x{} FIRST SOURCE DENY: the engine's target selector picked handle 0x{}, "
                         "answered with the pin 0x{} instead.",
                         Hex(self), Hex(engineAnswer), Hex(claim.form));
        }
    }

    struct SelectStandardHook {
        static std::uint32_t* thunk(void* a_self, std::uint32_t* a_out) {
            std::uint32_t* r = func(a_self, a_out);   // the engine's own selection first
            AnswerSelection(a_self, r, g_vtStandard);
            return r;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct SelectFixedHook {
        static std::uint32_t* thunk(void* a_self, std::uint32_t* a_out) {
            std::uint32_t* r = func(a_self, a_out);
            AnswerSelection(a_self, r, g_vtFixed);
            return r;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    constexpr std::size_t kSelectTargetSlot = 0x06;   // CombatTargetSelector::SelectTarget (both runtimes)
    // 1.7.104 raw RVA of CombatController::SetTarget (tools/verified_addresses idmap-1.7.104-apmf.csv
    // rva:0x559630 -> 0x5614A0, VerifiedAddresses row TargetPin.CombatController.SetTarget.1_7_104).
    constexpr std::uintptr_t kSetTarget17 = 0x5614A0;

    // ------------------------------------------------------------------------------------
    // THE 0xE4 LAYERS (Harbinger controller 1a, plan "Target claim / pin", marth Q1 / Q13, and the
    // design rule of 2026-10-11: "harbinger should ALWAYS be able to tell").
    //
    // A HARBINGER LAYER BETWEEN EVERY PAIR OF FOREIGN HOOKS. Layer 0 is installed at APMF's own
    // SKSEPluginLoad (InstallLayer0), as early as APMF can, so it normally wraps the engine's
    // UpdateCombat alone and every other plugin's 0xE4 hook lands ON TOP of it. Each time the census
    // (core/Sources: kPostLoad, kPostPostLoad, kInputLoaded, kDataLoaded, kPostLoadGame, kNewGame,
    // every 60 s) finds a new foreign hook on the slot, Harbinger installs a new layer directly above
    // it (RehookOnTop, up to kMaxLayers - 1 re-hooks). So segment i -- the code between layer i and
    // layer i-1 -- is normally EXACTLY ONE foreign module (segment 0: the engine alone).
    //
    // PROOF, NOT ASSUMPTION. Each layer records the return address of whoever called it. Segment i
    // (i >= 1) is single-module only if layer i-1 was called from inside the image of the module layer
    // i wraps (the module that hooked over layer i-1 calls it directly). Segment 0 is engine-only only
    // if layer 0 displaced the engine's own function. Two plugins hooking the slot inside one SKSE
    // dispatch window (no census can run between them) share a segment: the caller check FAILS, that is
    // logged loudly as an ATTRIBUTION DEFECT, and nothing is enforced in that segment. Harbinger cannot
    // put a layer between them afterwards without patching the plugins' own saved pointers.
    //
    // Per combat update of a PINNED actor the layers run nested on one thread; each records the actor's
    // target when the chain below it returns. The FIRST layer entered (the outermost) judges:
    //   * SOURCE SEAT MISSED (unchanged): aimed elsewhere, the selector seat never called.
    //   * OVERWRITTEN: the seat answered with the pin this update and the actor still ends aimed
    //     elsewhere. Every segment whose exit value differs from the one below it WROTE the target; its
    //     writer is that segment's proven module (segment 0: the Engine). A write in a segment that is
    //     not proven single-module is a DEFECT: allowed + alerted (the degrade rule, Q4), never undone.
    //   * Otherwise the claim's priority (its client's, APMF_Sources.ini [Target]) is compared with every
    //     writer's. Strictly higher -> Harbinger RESTORES the pin at the outermost layer, after every
    //     chained hook ran (Q1), through the engine's own CombatController::SetTarget (AE 33235 / SE 32488
    //     / 1.7.104 raw 0x5614A0, rcx = controller, rdx = Actor*): targetHandle, currentCombatTarget, the
    //     cached target and the group bookkeeping of the target it replaces, exactly as UpdateTarget does.
    //     previousTargetHandle is put back to its value before the update with a plain store (SetTarget and
    //     UpdateTarget never write +0x30; the writers did) -- the one field written directly. Logged
    //     `captured <src> overruled`. Equal or lower -> `captured <src> stands`.
    // The seat answering with the pin already proved the pin alive, loaded and one of the group's targets
    // this update, so the restore never starts or revives a fight (#0 (f), amended condition 4).
    // THREADING: the BSJobs worker running this actor's UpdateCombat (the same thread UpdateTarget calls
    // SetTarget on); the frame is thread_local; per-layer data are atomics written on the main thread
    // before the layer is published by the vtable write.
    // ------------------------------------------------------------------------------------
    constexpr int kMaxLayers = 8;   // layer 0 + seven re-hooks
    std::atomic<int>            g_layerCount{ 0 };   // layers installed (main thread writes)
    std::atomic<int>            g_segSrc[kMaxLayers]{ -1, -1, -1, -1, -1, -1, -1, -1 };
    std::atomic<std::uintptr_t> g_layerFunc[kMaxLayers]{};   // what each layer chains to
    std::atomic<std::uintptr_t> g_segLo[kMaxLayers]{}, g_segHi[kMaxLayers]{};   // image of that module
    std::atomic<bool>           g_seg0EngineOnly{ false };
    std::atomic<bool>           g_defectLogged[kMaxLayers]{};
    std::uintptr_t              g_vtCharAddr = 0;

    using SetTarget_t = void (*)(RE::CombatController*, RE::Actor*);
    std::atomic<std::uintptr_t> g_setTarget{ 0 };   // 0 = the setter is not verified: no restore

    struct Frame {
        int             depth = 0;
        RE::FormID      actor = 0;
        RE::FormID      tf    = 0;
        std::uint32_t   hits0 = 0, denied0 = 0, held0 = 0;
        RE::CombatController* ccBefore = nullptr;
        RE::ActorHandle curBefore{};
        RE::ActorHandle prevBefore{};
        RE::ActorHandle exitAt[kMaxLayers]{};
        bool            exitValid[kMaxLayers]{};
        std::uintptr_t  callerRet[kMaxLayers]{};
    };
    thread_local Frame t_frame;

    struct TargetCapture {   // per pinned actor, the Harbinger-level counters (logged on release)
        std::atomic<std::uint32_t> restored{ 0 }, stood{ 0 }, unattributed{ 0 };
        std::atomic<std::uint64_t> lastLogMs{ 0 };
    };
    std::mutex                                                   g_capMx;
    std::unordered_map<RE::FormID, std::unique_ptr<TargetCapture>> g_cap;

    TargetCapture& CaptureFor(RE::FormID id) {
        std::scoped_lock lk(g_capMx);
        auto& p = g_cap[id];
        if (!p) p = std::make_unique<TargetCapture>();
        return *p;
    }

    // Is segment `i` proven to hold exactly one module (segment 0: the engine alone)? Logs a failed
    // proof once per segment, loudly.
    bool SegmentProven(const Frame& fr, int i, RE::FormID self) {
        bool        ok   = false;
        const char* why  = "";
        if (i == 0) {
            ok  = g_seg0EngineOnly.load(std::memory_order_relaxed);
            why = "layer 0 did not displace the engine's own UpdateCombat (a plugin hooked the slot before "
                  "APMF loaded), so what runs below it cannot be told apart";
        } else {
            const auto lo = g_segLo[i].load(std::memory_order_relaxed), hi = g_segHi[i].load(std::memory_order_relaxed);
            ok = g_segSrc[i].load(std::memory_order_relaxed) >= 0 && fr.exitValid[i - 1] && lo < hi &&
                 fr.callerRet[i - 1] >= lo && fr.callerRet[i - 1] < hi;
            why = "the layer below was not called from the module this layer wraps: another module sits between "
                  "them (two plugins hooked the slot inside one load window)";
        }
        if (!ok && !g_defectLogged[i].exchange(true, std::memory_order_relaxed)) {
            spdlog::error("[ch.20] ATTRIBUTION DEFECT on Character::UpdateCombat segment {} (actor 0x{}): {}. Writes in "
                          "this segment are allowed and alerted, never enforced. (logged once per segment)",
                          i, Hex(self), why);
        }
        return ok;
    }

    void Judge(RE::Actor* a_this) {
        const Frame& fr = t_frame;
        const RE::FormID self = fr.actor;
        const RE::FormID tf   = fr.tf;
        if (tf == 0 || !fr.ccBefore) return;
        auto* cc = a_this->GetActorRuntimeData().combatController;
        if (a_this->IsDead() || !cc) return;
        APMF_API::APMF_Param claim{};
        float                basis = 0.0f;
        APMF_API::Handle     claimH = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(self, APMF_API::kIntent_TargetPin, claim, basis, &claimH) ||
            claim.form != tf)
            return;

        std::shared_lock lk(g_pinMx);
        const auto it = g_pins.find(self);
        if (it == g_pins.end() || it->second.target != tf) return;
        const Pin& pin = it->second;

        auto currentPtr = a_this->GetActorRuntimeData().currentCombatTarget.get();
        if (!currentPtr) return;                    // no target: nothing to judge
        auto targetPtr = pin.handle.get();
        if (!targetPtr || currentPtr.get() == targetPtr.get()) return;   // on the pin, or pin gone

        const auto hits1   = pin.seatHits.load(std::memory_order_relaxed);
        const auto denied1 = pin.denied.load(std::memory_order_relaxed);
        const auto held1   = pin.held.load(std::memory_order_relaxed);
        // A MISS: the seat was not called this update AND either the target CHANGED during
        // the update (whatever the seat decided last time -- a sticky decline must not hide
        // a real miss for good), or it is unchanged and the seat's last decision was not a
        // decline (a declined pin legitimately leaves the engine's standing pick in place).
        const bool changed = a_this->GetActorRuntimeData().currentCombatTarget != fr.curBefore;
        if (hits1 == fr.hits0 && (changed || pin.lastDecision.load(std::memory_order_relaxed) != 2)) {
            const auto n = pin.seatMissed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (RateOk(pin.lastMissLogMs)) {
                spdlog::warn("[ch.20] 0x{} SOURCE SEAT MISSED: the actor ended its combat update aimed at 0x{}, "
                             "not the pin 0x{}, and the target-selector seat was never called for it. The pin "
                             "is NOT holding. ({} so far)",
                             Hex(self), Hex(currentPtr->GetFormID()), Hex(tf), n);
            }
            return;
        }
        const bool answered = hits1 != fr.hits0 && (denied1 != fr.denied0 || held1 != fr.held0) &&
                              pin.lastDecision.load(std::memory_order_relaxed) == 1;
        if (!answered) return;   // the seat ran and declined for a logged, counted reason

        // OVERWRITTEN. Who wrote it: walk the segments from the innermost out.
        const auto n = pin.overwritten.fetch_add(1, std::memory_order_relaxed) + 1;
        const RE::ActorHandle pinH = pin.handle;
        RE::ActorHandle below = pinH;   // the seat's answer, applied inside the engine's update
        apmf::sources::SourceId writers[kMaxLayers]{ -1, -1, -1, -1, -1, -1, -1, -1 };
        int  nWriters = 0;
        bool unproven = false;
        apmf::sources::SourceId unprovenSrc[kMaxLayers]{ -1, -1, -1, -1, -1, -1, -1, -1 };   // the module owning each unproven segment
        int nUnproven = 0;
        for (int i = 0; i < kMaxLayers; ++i) {
            if (!fr.exitValid[i]) continue;
            if (fr.exitAt[i] != below) {
                if (!SegmentProven(fr, i, self)) {
                    unproven = true;
                    const int seg = i == 0 ? -1 : g_segSrc[i].load(std::memory_order_relaxed);
                    unprovenSrc[nUnproven++] = seg >= 0 ? seg : apmf::sources::kEngineSource;
                }
                else writers[nWriters++] = i == 0 ? apmf::sources::kEngineSource : g_segSrc[i].load(std::memory_order_relaxed);
            }
            below = fr.exitAt[i];
        }
        auto& cap = CaptureFor(self);
        const bool logNow = RateOk(cap.lastLogMs);
        for (int w = 0; w < nWriters; ++w) apmf::sources::NoteHit(writers[w], apmf::sources::Facet::kTarget);
        const bool unknownWrite = unproven || nWriters == 0;
        const bool unknownOver  = unknownWrite && apmf::sources::OverrideUnknown(apmf::sources::Facet::kTarget);
        if (unknownWrite) {
            cap.unattributed.fetch_add(1, std::memory_order_relaxed);
            if (logNow) {
                spdlog::warn("[ch.20] 0x{} OVERWRITTEN: the selector seat answered with the pin 0x{}, but the actor "
                             "ended its combat update aimed at 0x{}, written in a segment Harbinger cannot prove (see "
                             "the ATTRIBUTION DEFECT line). {} ({} so far)",
                             Hex(self), Hex(tf), Hex(currentPtr->GetFormID()),
                             unknownOver ? "Unknown sources can be overridden is ON for Target: treated as the Unattributed priority, at least the owning module's."
                                         : "ALLOWED.", n);
            }
            apmf::sources::Unattributed(apmf::sources::Facet::kTarget, self,
                                        "DEFECT: the combat target was rewritten in an unproven 0xE4 segment");
            if (!unknownOver) return;
        }
        apmf::sources::SourceId client = apmf::sources::kNoSource;
        const int claimPrio = apmf::sources::ClaimPriority(claimH, apmf::sources::Facet::kTarget, &client);
        int  topPrio = -1;
        apmf::sources::SourceId top = writers[0];
        for (int w = 0; w < nWriters; ++w) {
            const int p = apmf::sources::Priority(writers[w], apmf::sources::Facet::kTarget);
            if (p > topPrio) { topPrio = p; top = writers[w]; }
        }
        if (unknownOver) {
            // An unknown write ranks at the Unattributed row's priority (its slider), but never below the
            // module that owns the unproven segment, and it STANDS when that module is the claiming client
            // (its own hook must not fight its own claim).
            int floorPrio = apmf::sources::Priority(apmf::sources::kUnattributedSource, apmf::sources::Facet::kTarget);
            apmf::sources::SourceId floorSrc = apmf::sources::kUnattributedSource;
            for (int u = 0; u < nUnproven; ++u) {
                const auto src = unprovenSrc[u];
                const int  p   = src == client ? claimPrio : apmf::sources::Priority(src, apmf::sources::Facet::kTarget);
                if (p > floorPrio) { floorPrio = p; floorSrc = src; }
            }
            if (floorPrio > topPrio) { topPrio = floorPrio; top = floorSrc; }
        }
        const std::string clientName = client != apmf::sources::kNoSource
                                           ? std::string(apmf::sources::Name(client)) : std::string("an internal claim");
        const auto setTarget = reinterpret_cast<SetTarget_t>(g_setTarget.load(std::memory_order_relaxed));
        // SetTarget writes the attacker's currentCombatTarget through the controller's attacker handle
        // without a null check (AE 0x55972E): call it only when that handle resolves to this actor.
        bool attackerOk = false;
        if (claimPrio > topPrio && setTarget) {
            auto att   = cc->attackerHandle.get();
            attackerOk = att && att.get() == a_this;
            if (!attackerOk && logNow) {
                spdlog::warn("[ch.20] 0x{} restore SKIPPED: the combat controller's attacker handle does not resolve "
                             "to this actor, and SetTarget writes through it unchecked. {} stands this update.",
                             Hex(self), apmf::sources::Name(top));
            }
        }
        if (claimPrio > topPrio && setTarget && attackerOk) {
            setTarget(cc, targetPtr.get());               // the engine's own setter, as UpdateTarget calls it
            cc->previousTargetHandle = fr.prevBefore;      // never written by SetTarget / UpdateTarget
            const bool held = a_this->GetActorRuntimeData().currentCombatTarget == pinH;
            cap.restored.fetch_add(1, std::memory_order_relaxed);
            apmf::sources::NoteServed(client, apmf::sources::Facet::kTarget);   // a foreign write restored
            for (int w = 0; w < nWriters; ++w) apmf::sources::NoteOverruled(writers[w], apmf::sources::Facet::kTarget);
            if (unknownOver) {
                apmf::sources::NoteOverruled(apmf::sources::kUnattributedSource, apmf::sources::Facet::kTarget);
                if (logNow)
                    spdlog::warn("[sources] 0x{} UNATTRIBUTED Target write OVERRIDDEN: \"Unknown sources can be "
                                 "overridden\" is on for Target, so the unattributed writer ranks at {} (the Unattributed "
                                 "row, or its segment's module if higher) and the pin claim ({} priority {}) outranks it.",
                                 Hex(self), topPrio, clientName, claimPrio);
            }
            if (logNow) {   // rate-limited, including the "still not the pin" case (review round 2)
                spdlog::info("[ch.20] 0x{} captured {} overruled: it rewrote the target to 0x{} after the seat answered "
                             "with the pin; the pin claim ({} priority {}) outranks it (priority {}), so Harbinger "
                             "restored the pin 0x{} through CombatController::SetTarget at the outermost UpdateCombat "
                             "layer{}. ({} restore(s))",
                             Hex(self), apmf::sources::Name(top), Hex(currentPtr->GetFormID()), clientName, claimPrio,
                             topPrio, Hex(tf), held ? "" : " -- BUT the actor's target is still not the pin after it",
                             cap.restored.load(std::memory_order_relaxed));
            }
        } else {
            cap.stood.fetch_add(1, std::memory_order_relaxed);
            for (int w = 0; w < nWriters; ++w) apmf::sources::NoteStood(writers[w], apmf::sources::Facet::kTarget);
            if (logNow) {
                spdlog::info("[ch.20] 0x{} captured {} stands: it rewrote the target to 0x{} after the seat answered "
                             "with the pin 0x{}; {}. ({} so far)",
                             Hex(self), apmf::sources::Name(top), Hex(currentPtr->GetFormID()), Hex(tf),
                             !setTarget ? std::string("the engine's SetTarget was not verified on this build, so "
                                                      "Harbinger does not restore")
                             : claimPrio > topPrio
                                 ? std::string("the restore was skipped: the controller's attacker handle does not "
                                               "resolve to this actor")
                                 : fmt::format("its priority {} is not below the pin claim's ({} priority {})",
                                               topPrio, clientName, claimPrio),
                             cap.stood.load(std::memory_order_relaxed));
            }
        }
    }

    // The shared layer body. `a_layer` = this layer's index; `a_next` = what it chains to; `a_ret` =
    // the return address of whoever called this layer (the proof that a segment is one module).
    void LayerBody(int a_layer, RE::Actor* a_this, void (*a_next)(RE::Actor*), std::uintptr_t a_ret) {
        Frame& fr = t_frame;
        if (fr.depth == 0) {
            if (g_pinCount.load(std::memory_order_relaxed) == 0 || !a_this) {
                a_next(a_this);
                return;
            }
            const RE::FormID self = a_this->GetFormID();
            RE::FormID       tf   = 0;
            std::uint32_t    hits0 = 0, denied0 = 0, held0 = 0;
            {
                std::shared_lock lk(g_pinMx);
                if (const auto it = g_pins.find(self); it != g_pins.end()) {
                    tf      = it->second.target;
                    hits0   = it->second.seatHits.load(std::memory_order_relaxed);
                    denied0 = it->second.denied.load(std::memory_order_relaxed);
                    held0   = it->second.held.load(std::memory_order_relaxed);
                }
            }
            if (tf == 0) {   // not pinned: nothing to observe, no frame
                a_next(a_this);
                return;
            }
            fr = Frame{};
            fr.actor   = self;
            fr.tf      = tf;
            fr.hits0   = hits0;
            fr.denied0 = denied0;
            fr.held0   = held0;
            // F-A: UpdateCombat skips UpdateTarget on purpose on its early-outs (form flag
            // bit 21; IsDead(true); no current process; the 0xA9 extra-data test -- AE
            // 0x6B6E8F / 0x6B6EA9 / 0x6B6EB7 / 0x6B6EC7-0x6B6ED3, SE 0x62571F / 0x625739 /
            // 0x625747 / 0x625757-0x625763; and no controller). A MISS is judged only when the
            // actor is alive and had a controller both before and after the update.
            fr.ccBefore  = a_this->GetActorRuntimeData().combatController;
            fr.curBefore = a_this->GetActorRuntimeData().currentCombatTarget;
            if (fr.ccBefore) fr.prevBefore = fr.ccBefore->previousTargetHandle;
            fr.callerRet[a_layer] = a_ret;
            fr.depth = 1;
            a_next(a_this);   // the whole chain below, the selector seat inside the engine's update
            fr.exitAt[a_layer]    = a_this->GetActorRuntimeData().currentCombatTarget;
            fr.exitValid[a_layer] = true;
            fr.depth = 0;
            try {
                Judge(a_this);
            } catch (...) {
            }
            fr = Frame{};
            return;
        }
        // Nested inside the outermost layer: the same actor's update (any other actor here
        // would be a re-entrant UpdateCombat, which the engine does not do -- pass it through).
        if (!a_this || a_this->GetFormID() != fr.actor) {
            a_next(a_this);
            return;
        }
        fr.callerRet[a_layer] = a_ret;
        ++fr.depth;
        a_next(a_this);
        --fr.depth;
        fr.exitAt[a_layer]    = a_this->GetActorRuntimeData().currentCombatTarget;
        fr.exitValid[a_layer] = true;
    }

    // Layer 0, installed at SKSEPluginLoad (InstallLayer0).
    struct UpdateCombatObserver {
        static void thunk(RE::Actor* a_this) {
            LayerBody(0, a_this, Next, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        }
        static void Next(RE::Actor* a) { func(a); }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0xE4;   // Actor::UpdateCombat (SE == AE index)
    };

    // Layers 1..7, installed by the census directly above each new foreign hook (Q13).
    template <int N>
    struct OuterLayer {
        static void Next(RE::Actor* a) {
            reinterpret_cast<void (*)(RE::Actor*)>(g_layerFunc[N].load(std::memory_order_acquire))(a);
        }
        static void thunk(RE::Actor* a_this) {
            LayerBody(N, a_this, Next, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        }
    };

    std::uintptr_t LayerThunk(int n) {
        switch (n) {
        case 1: return reinterpret_cast<std::uintptr_t>(&OuterLayer<1>::thunk);
        case 2: return reinterpret_cast<std::uintptr_t>(&OuterLayer<2>::thunk);
        case 3: return reinterpret_cast<std::uintptr_t>(&OuterLayer<3>::thunk);
        case 4: return reinterpret_cast<std::uintptr_t>(&OuterLayer<4>::thunk);
        case 5: return reinterpret_cast<std::uintptr_t>(&OuterLayer<5>::thunk);
        case 6: return reinterpret_cast<std::uintptr_t>(&OuterLayer<6>::thunk);
        case 7: return reinterpret_cast<std::uintptr_t>(&OuterLayer<7>::thunk);
        default: return 0;
        }
    }

    void SetSegment(int n, std::uintptr_t a_wrapped) {
        const auto src = apmf::sources::ForAddress(a_wrapped, apmf::sources::Kind::kHook);
        std::uintptr_t lo = 0, hi = 0;
        if (!apmf::sources::ModuleRange(a_wrapped, lo, hi)) lo = hi = 0;
        g_segSrc[n].store(src == apmf::sources::kEngineSource ? -1 : src, std::memory_order_relaxed);
        g_segLo[n].store(lo, std::memory_order_relaxed);
        g_segHi[n].store(hi, std::memory_order_relaxed);
        g_layerFunc[n].store(a_wrapped, std::memory_order_release);
    }

    // The census found slot 0xE4 holding `a_current` (a foreign hook installed over Harbinger's
    // outermost layer). Put a new layer directly above it. MAIN thread.
    std::uintptr_t RehookOnTop(std::uintptr_t a_current) {
        const int n = g_layerCount.load(std::memory_order_relaxed);
        if (n >= kMaxLayers || !g_vtCharAddr) {
            spdlog::error("[ch.20] slot 0xE4 was hooked over again and all {} Harbinger layers are in use -- NOT "
                          "re-hooking; writes above the last layer are not attributed (a DEFECT, logged per update).",
                          kMaxLayers);
            return 0;
        }
        SetSegment(n, a_current);   // before the slot points at the layer
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        const std::uintptr_t was = vtChar.write_vfunc(UpdateCombatObserver::idx, LayerThunk(n));
        if (was != a_current) SetSegment(n, was);   // it moved between the census read and our write
        g_layerCount.store(n + 1, std::memory_order_relaxed);
        const int seg = g_segSrc[n].load();
        spdlog::warn("[ch.20] Harbinger layer {} installed directly above {} on Character::UpdateCombat (0xE4): that "
                     "module's target writes are attributed to it alone and arbitrated by priority ([Target] in "
                     "APMF_Sources.ini).",
                     n, seg >= 0 ? std::string(apmf::sources::Name(seg)) : std::string("an unnamed hook"));
        return LayerThunk(n);
    }

    // Resolve `param.form` on the game thread and (re)write this actor's entry. A refused
    // target (not an actor, 0, self) ERASES the entry, so the claim stays live but inert and
    // the log says why -- that refusal is the client's input, not a target APMF lost track of,
    // so it is not ended by Poll().
    void Apply(RE::FormID id, const APMF_API::APMF_Param& param, const char* what) {
        const RE::FormID tf  = param.form;
        const char*      why = nullptr;
        RE::ActorHandle  h{};
        if (tf == 0) {
            why = "param.form is 0 (a pin needs the TARGET actor's FormID)";
        } else if (tf == id) {
            why = "the target is the actor itself";
        } else if (auto* form = RE::TESForm::LookupByID(tf); !form) {
            why = "no form has that FormID";
        } else if (auto* target = form->As<RE::Actor>(); !target) {
            why = "the form is not an Actor";
        } else {
            h = target->GetHandle();
            if (!h) why = "the actor has no reference handle";
        }

        std::unique_lock lk(g_pinMx);
        g_pins.erase(id);
        if (!why) {
            auto [it, ok] = g_pins.try_emplace(id);
            it->second.target = tf;
            it->second.handle = h;
        }
        g_pinCount.store(g_pins.size(), std::memory_order_relaxed);
        lk.unlock();
        // THE HELD ENGAGEMENT: the pin is what this actor holds (nothing, for an inert claim).
        if (why) apmf::combatentry::hold::ClearPin(id);
        else apmf::combatentry::hold::SetPin(id, tf, h);

        if (why) {
            spdlog::warn("[ch.20] 0x{} target-pin claim {} but INERT -- target 0x{}: {}. The handle stays "
                         "live and pins nothing; Repoint it to an actor or Release it.",
                         Hex(id), what, Hex(tf), why);
        } else {
            spdlog::info("[ch.20] 0x{} target-pin {} -> target 0x{}. The engine's target selection is answered "
                         "with it while the actor is fighting and it is one of the group's combat targets{}; never "
                         "starts combat.",
                         Hex(id), what, Hex(tf),
                         (static_cast<std::uint32_t>(param.ival) & APMF_API::kTargetPin_OwnLineOfSight) != 0
                             ? " and Harbinger's own ray sees it (ABI v18 kTargetPin_OwnLineOfSight)" : "");
        }
    }

    class TargetPinChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "target-pin"; }
        int              ChannelNo() const override { return 20; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_TargetPin; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "RE-POINTED");
        }

        // Relinquish (INVARIANTS #5a): nothing to restore. The engine's own selection
        // answers again from the next combat update on.
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            struct { std::uint32_t hits, denied, held, none, notTgt, gone, lost, missed, over, notSeen; } c{};
            RE::FormID  t   = 0;
            bool        had = false;
            const char* why = nullptr;
            {
                std::unique_lock lk(g_pinMx);
                if (const auto it = g_pins.find(id); it != g_pins.end()) {
                    had = true;
                    const Pin& p = it->second;
                    t = p.target;
                    c = { p.seatHits.load(), p.denied.load(), p.held.load(), p.noEngineTarget.load(),
                          p.notCombatTarget.load(), p.targetGone.load(), p.targetLost.load(), p.seatMissed.load(),
                          p.overwritten.load(), p.notInSight.load() };
                    why = p.endedReason.load();
                    g_pins.erase(it);
                }
                g_pinCount.store(g_pins.size(), std::memory_order_relaxed);
            }
            apmf::combatentry::hold::ClearPin(id);
            std::uint32_t restored = 0, stood = 0, unattrib = 0;
            {
                std::scoped_lock lk(g_capMx);
                if (const auto ci = g_cap.find(id); ci != g_cap.end()) {
                    restored = ci->second->restored.load();
                    stood    = ci->second->stood.load();
                    unattrib = ci->second->unattributed.load();
                    g_cap.erase(ci);
                }
            }
            if (had && (restored || stood || unattrib)) {
                spdlog::info("[ch.20] 0x{} Harbinger target capture over this pin: {} outside write(s) overruled "
                             "(pin restored), {} outranked the claim and stood, {} unattributed and allowed.",
                             Hex(id), restored, stood, unattrib);
            }
            if (had) {
                spdlog::info("[ch.20] 0x{} target-pin released ({}) (target 0x{}): selector seat called {} time(s) -- "
                             "denied the engine's pick {}, engine already on the pin {}, engine had no target {}, "
                             "pin not a combat target {}, target gone {}, target lost {}, paused not in sight {}; "
                             "observer: seat missed {}, overwritten after the seat {}. The engine's own selection "
                             "answers again.",
                             Hex(id), why ? fmt::format("ENDED BY HARBINGER: {}", why) : std::string("by the client, "
                             "an unload or a load"), Hex(t), c.hits, c.denied, c.held, c.none, c.notTgt, c.gone,
                             c.lost, c.notSeen, c.missed, c.over);
            } else {
                spdlog::info("[ch.20] 0x{} target-pin released (it was inert).", Hex(id));
            }
        }
    };

}

namespace apmf::targetpin {

    // The gates shared by both install steps; nullptr = open.
    const char* RefusedWhy() {
        if (REL::Module::IsVR()) return "VR runtime (the seats are verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        if (!apmf::allowance::RuntimeSupported())   // F2b: 1.7.104 proven (selectors, 0xE4, CombatGroup)
            return "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (the seats are verified on those three only)";
        if (GetPrivateProfileIntA("TargetPin", "bTargetPin", 1, kIni) == 0)
            return "[TargetPin] bTargetPin=0 in Data/SKSE/Plugins/APMF.ini";
        return nullptr;
    }

    std::atomic<bool> g_layer0Installed{ false };

    void InstallLayer0() {
        static std::atomic<bool> s_tried{ false };
        if (s_tried.exchange(true)) return;
        if (const char* why = RefusedWhy()) {
            spdlog::warn("[ch.20] UpdateCombat layer 0 NOT installed at plugin load -- {}.", why);
            return;
        }
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        if (!apmf::allowance::SeatVerified(vtChar.address(), "TargetPin.Character.UpdateCombat(layer 0)")) {
            spdlog::error("[ch.20] UpdateCombat layer 0 NOT installed (self-check refused the Character vtable).");
            return;
        }
        const std::uintptr_t prevE4 = *reinterpret_cast<const std::uintptr_t*>(
            vtChar.address() + sizeof(void*) * UpdateCombatObserver::idx);
        // Segment 0 is the engine alone only if layer 0 displaced the engine's own function.
        g_seg0EngineOnly.store(apmf::sources::ForAddress(prevE4, apmf::sources::Kind::kHook) ==
                                   apmf::sources::kEngineSource,
                               std::memory_order_relaxed);
        SetSegment(0, prevE4);
        g_vtCharAddr = vtChar.address();
        UpdateCombatObserver::func = vtChar.write_vfunc(UpdateCombatObserver::idx, UpdateCombatObserver::thunk);
        g_layerCount.store(1, std::memory_order_relaxed);
        g_layer0Installed.store(true, std::memory_order_release);
        apmf::sources::RegisterVtableSeat(apmf::sources::Facet::kTarget, "TargetPin.Character.UpdateCombat",
                                          vtChar.address() + sizeof(void*) * UpdateCombatObserver::idx,
                                          reinterpret_cast<std::uintptr_t>(&UpdateCombatObserver::thunk), prevE4,
                                          &RehookOnTop);
        if (g_seg0EngineOnly.load()) {
            spdlog::info("[ch.20] Harbinger layer 0 installed on Character::UpdateCombat (0xE4) at plugin load, directly "
                         "on the engine's function: every 0xE4 hook installed later is attributed by the layers above it.");
        } else {
            char mod[64]{};
            apmf::sources::ModuleNameOf(prevE4, mod, sizeof(mod));
            spdlog::error("[ch.20] Harbinger layer 0 installed on Character::UpdateCombat (0xE4) at plugin load, but {} "
                          "hooked the slot BEFORE APMF loaded: what runs below layer 0 cannot be told apart, and a target "
                          "write there is an ATTRIBUTION DEFECT (allowed, never enforced).",
                          mod[0] ? mod : "an unnamed hook");
        }
    }

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = RefusedWhy();
        if (!why && !g_layer0Installed.load(std::memory_order_acquire))
            why = "the UpdateCombat layer 0 was not installed at plugin load (logged there)";
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.20] target-pin seats NOT installed -- {}. kIntent_TargetPin claims are REFUSED.", why);
            return;
        }

        // Verify BOTH before writing EITHER: a pin with half a mechanism is refused whole.
        REL::Relocation<std::uintptr_t> vtStd{ RE::VTABLE_CombatTargetSelectorStandard[0] };
        REL::Relocation<std::uintptr_t> vtFix{ RE::VTABLE_CombatTargetSelectorFixed[0] };
        bool ok = apmf::allowance::SeatVerified(vtStd.address(), "TargetPin.CombatTargetSelectorStandard.SelectTarget");
        ok = apmf::allowance::SeatVerified(vtFix.address(), "TargetPin.CombatTargetSelectorFixed.SelectTarget") && ok;
        if (!ok) {
            g_notInstalledReason.store("the address self-check refused a seat vtable", std::memory_order_release);
            spdlog::error("[ch.20] target-pin seats NOT installed (self-check refused a vtable, listed above). "
                          "kIntent_TargetPin claims are REFUSED.");
            return;
        }

        // The restore setter (Harbinger 1a). 1.7.104: a raw-RVA row (AE id 33235 has no MIT-table
        // row; an id miss is fatal there). Unverified -> capture still runs, restore does not.
        {
            const bool           on17 = apmf::allowance::IsRuntime1_7_104();
            const std::uintptr_t st   = on17 ? REL::Offset(kSetTarget17).address()
                                             : REL::Relocation<std::uintptr_t>{ RELOCATION_ID(32488, 33235) }.address();
            if (apmf::allowance::SeatVerified(st, on17 ? "TargetPin.CombatController.SetTarget.1_7_104"
                                                       : "TargetPin.CombatController.SetTarget"))
                g_setTarget.store(st, std::memory_order_release);
            else
                spdlog::error("[ch.20] CombatController::SetTarget NOT verified -- Harbinger captures and attributes "
                              "target writes but does NOT restore the pin on this build.");
        }

        g_vtStandard = vtStd.address();
        g_vtFixed    = vtFix.address();
        SelectStandardHook::func   = vtStd.write_vfunc(kSelectTargetSlot, SelectStandardHook::thunk);
        SelectFixedHook::func      = vtFix.write_vfunc(kSelectTargetSlot, SelectFixedHook::thunk);
        const std::uintptr_t prevStd = SelectStandardHook::func.address();
        const std::uintptr_t prevFix = SelectFixedHook::func.address();
        g_installed.store(true, std::memory_order_release);

        using apmf::sources::Facet;
        apmf::sources::RegisterVtableSeat(Facet::kTarget, "TargetPin.CombatTargetSelectorStandard.SelectTarget",
                                          vtStd.address() + sizeof(void*) * kSelectTargetSlot,
                                          reinterpret_cast<std::uintptr_t>(&SelectStandardHook::thunk), prevStd);
        apmf::sources::RegisterVtableSeat(Facet::kTarget, "TargetPin.CombatTargetSelectorFixed.SelectTarget",
                                          vtFix.address() + sizeof(void*) * kSelectTargetSlot,
                                          reinterpret_cast<std::uintptr_t>(&SelectFixedHook::thunk), prevFix);
        spdlog::info("[ch.20] target-pin seats installed: SOURCE DENY on CombatTargetSelectorStandard / Fixed "
                     "SelectTarget (slot 0x{:X}, chaining); the UpdateCombat layers capture target writes after the "
                     "seat and restore the pin through SetTarget when the claim outranks the proven writer.",
                     kSelectTargetSlot);
    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Poll() {
        if (g_pinCount.load(std::memory_order_relaxed) == 0) return;
        static std::uint64_t s_lastMs = 0;
        const std::uint64_t  now      = apmf::clock::MonotonicMs();
        if (now - s_lastMs < 250) return;
        s_lastMs = now;

        struct Snap { RE::FormID id, target; RE::ActorHandle handle; const char* seatWhy; };
        std::vector<Snap> snaps;
        {
            std::shared_lock lk(g_pinMx);
            for (const auto& [id, p] : g_pins) {
                if (p.ending.load(std::memory_order_relaxed)) continue;
                snaps.push_back({ id, p.target, p.handle, p.seatEndReason.load(std::memory_order_relaxed) });
            }
        }

        for (const auto& sn : snaps) {
            // "target lost" is trusted as the seat saw it; everything else is checked LIVE here,
            // on the game thread, whatever the seat recorded.
            const char* why = (sn.seatWhy == kReasonLost || sn.seatWhy == kReasonAbsent) ? sn.seatWhy : nullptr;
            if (!why) {
                auto* owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
                auto  tptr  = sn.handle.get();
                if (owner && owner->IsDead())      why = "owner dead";
                else if (!tptr)                    why = "target unresolvable";
                else if (tptr->IsDead())           why = "target dead";
                else if (tptr->IsDisabled())       why = "target disabled";
                else if (!tptr->Is3DLoaded())      why = "target unloaded";
            }
            if (!why) {
                if (sn.seatWhy) {   // the seat saw a transient state that the live check does not confirm
                    std::shared_lock lk(g_pinMx);
                    if (const auto it = g_pins.find(sn.id); it != g_pins.end() && it->second.target == sn.target) {
                        const char* seen = sn.seatWhy;
                        it->second.seatEndReason.compare_exchange_strong(seen, nullptr, std::memory_order_relaxed);
                    }
                }
                continue;
            }

            // End the WINNING claim -- the one this entry was resolved for.
            APMF_API::APMF_Param param{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(sn.id, APMF_API::kIntent_TargetPin, param, basis,
                                                                 &h) ||
                param.form != sn.target || h == APMF_API::kInvalidHandle)
                continue;   // the claim moved; the next Apply rewrites the entry
            {
                std::unique_lock lk(g_pinMx);
                const auto it = g_pins.find(sn.id);
                if (it == g_pins.end() || it->second.target != sn.target || it->second.ending.load()) continue;
                it->second.ending.store(true);
                it->second.endedReason.store(why);
            }
            apmf::ControlMap::Get().EnqueueRelease(h);
            spdlog::info("[ch.20] 0x{} pin ended: {} (target 0x{}, claim h={}). Harbinger released the claim; a "
                         "mod that wants to keep chasing must pin again.",
                         Hex(sn.id), why, Hex(sn.target), h);
        }
    }

    void EndHeld(RE::FormID a_actor, RE::FormID a_target, const char* a_why) {
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_TargetPin, param, basis, &h) ||
            param.form != a_target || h == APMF_API::kInvalidHandle)
            return;   // no winning pin names this target
        {
            std::unique_lock lk(g_pinMx);
            const auto it = g_pins.find(a_actor);
            if (it == g_pins.end() || it->second.target != a_target || it->second.ending.load()) return;
            it->second.ending.store(true);
            it->second.endedReason.store(a_why);
        }
        apmf::ControlMap::Get().EnqueueRelease(h);
        spdlog::info("[ch.20] 0x{} pin ended: {} (target 0x{}, claim h={}). Harbinger released the claim; a mod that "
                     "wants to keep chasing must pin again.",
                     Hex(a_actor), a_why, Hex(a_target), h);
    }

    void ResetAll(const char* why) {
        std::size_t n = 0;
        {
            std::unique_lock lk(g_pinMx);
            n = g_pins.size();
            for (const auto& [id, p] : g_pins) apmf::combatentry::hold::ClearPin(id);
            g_pins.clear();
            g_pinCount.store(0, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lk(g_capMx);
            g_cap.clear();
        }
        if (n != 0) spdlog::info("[ch.20] {} -- dropped {} target-pin entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(TargetPinChannel);
