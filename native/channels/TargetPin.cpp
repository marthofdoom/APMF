#include "PCH.h"
#include "channels/TargetPin.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Registry.h"

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
// THE OBSERVER (not a belt). Character vtable slot 0xE4 (UpdateCombat) is also hooked,
// OBSERVE-ONLY: it writes nothing. For a pinned actor it snapshots the selector seat's
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
// lost, and dropped"). The seat declines and records a reason when the pinned target is
// LOST (its group entry carries kTargetLost: CombatTarget::flags, u16 +0xA6, bit 1, verified
// on both runtimes), dead, disabled, not loaded or unresolvable. targetpin::Poll() -- the
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
//     defers) -- NOT denied (ch.6 row 6 gap, unchanged). The observer reports it as
//     OVERWRITTEN; Docs/REVIEW-BACKLOG.md carries the MFO closure.
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
        if (!apmf::ControlMap::Get().TryGetOwningClaim(self, APMF_API::kIntent_TargetPin, claim)) return;
        if (claim.form == 0) return;

        std::shared_lock lk(g_pinMx);
        const auto it = g_pins.find(self);
        if (it == g_pins.end() || it->second.target != claim.form) return;
        const Pin& pin = it->second;
        pin.seatHits.fetch_add(1, std::memory_order_relaxed);
        const auto decline = [&pin] { pin.lastDecision.store(2, std::memory_order_relaxed); };
        const auto endWith = [&pin](const char* why) {
            if (why == kReasonLost) {   // the trusted reason always wins over a transient one
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
        if (m == Membership::kLost) {
            pin.targetLost.fetch_add(1, std::memory_order_relaxed);
            endWith(kReasonLost);   // the engine can no longer locate it: the pin ends
            decline();
            return;
        }
        if (engineAnswer == pin.handle.native_handle()) {
            pin.held.fetch_add(1, std::memory_order_relaxed);
            pin.lastDecision.store(1, std::memory_order_relaxed);
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

        *a_out = pin.handle.native_handle();   // DENY the engine's pick at its source
        pin.denied.fetch_add(1, std::memory_order_relaxed);
        pin.lastDecision.store(1, std::memory_order_relaxed);
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

    // OBSERVE-ONLY. Writes nothing. See "THE OBSERVER" in the header.
    struct UpdateCombatObserver {
        static void thunk(RE::Actor* a_this) {
            if (g_pinCount.load(std::memory_order_relaxed) == 0 || !a_this) {
                func(a_this);
                return;
            }
            const RE::FormID self = a_this->GetFormID();
            RE::FormID       tf   = 0;
            std::uint32_t    hits0 = 0, denied0 = 0;
            {
                std::shared_lock lk(g_pinMx);
                if (const auto it = g_pins.find(self); it != g_pins.end()) {
                    tf      = it->second.target;
                    hits0   = it->second.seatHits.load(std::memory_order_relaxed);
                    denied0 = it->second.denied.load(std::memory_order_relaxed);
                }
            }
            // F-A: UpdateCombat skips UpdateTarget on purpose on its early-outs (form flag
            // bit 21; IsDead(true); no current process; the 0xA9 extra-data test -- AE
            // 0x6B6E8F / 0x6B6EA9 / 0x6B6EB7 / 0x6B6EC7-0x6B6ED3, SE 0x62571F / 0x625739 /
            // 0x625747 / 0x625757-0x625763; and no controller). Those paths still sync the
            // controller's target into currentCombatTarget (AE 0x662BE0 / SE 0x5D2470). A
            // MISS is judged only when the actor is alive and had a controller both before
            // and after the update.
            auto* const ccBefore = tf ? a_this->GetActorRuntimeData().combatController : nullptr;
            // Closing round F2: the target BEFORE the update, so a change made during it with no
            // seat call is a MISS whatever the seat decided last time.
            const RE::ActorHandle curBefore = a_this->GetActorRuntimeData().currentCombatTarget;

            func(a_this);   // the engine's update, with the selector seat inside it

            if (tf == 0 || !ccBefore) return;
            if (a_this->IsDead() || !a_this->GetActorRuntimeData().combatController) return;
            APMF_API::APMF_Param claim{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(self, APMF_API::kIntent_TargetPin, claim) ||
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
            // A MISS: the seat was not called this update AND either the target CHANGED during
            // the update (whatever the seat decided last time -- a sticky decline must not hide
            // a real miss for good), or it is unchanged and the seat's last decision was not a
            // decline (a declined pin legitimately leaves the engine's standing pick in place).
            const bool changed = a_this->GetActorRuntimeData().currentCombatTarget != curBefore;
            if (hits1 == hits0 && (changed || pin.lastDecision.load(std::memory_order_relaxed) != 2)) {
                const auto n = pin.seatMissed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (RateOk(pin.lastMissLogMs)) {
                    spdlog::warn("[ch.20] 0x{} SOURCE SEAT MISSED: the actor ended its combat update aimed at 0x{}, "
                                 "not the pin 0x{}, and the target-selector seat was never called for it. The pin "
                                 "is NOT holding. ({} so far)",
                                 Hex(self), Hex(currentPtr->GetFormID()), Hex(tf), n);
                }
            } else if (denied1 != denied0) {
                const auto n = pin.overwritten.fetch_add(1, std::memory_order_relaxed) + 1;
                if (RateOk(pin.lastOverwriteLogMs)) {
                    spdlog::warn("[ch.20] 0x{} OVERWRITTEN: the selector seat answered with the pin 0x{}, but the "
                                 "actor ended its combat update aimed at 0x{} -- something wrote the target after "
                                 "the seat (another mod's hook, or another engine path). ({} so far)",
                                 Hex(self), Hex(tf), Hex(currentPtr->GetFormID()), n);
                }
            }
            // else: the seat ran and declined for a logged, counted reason.
        }

        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0xE4;   // Actor::UpdateCombat (SE == AE index)
    };

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

        if (why) {
            spdlog::warn("[ch.20] 0x{} target-pin claim {} but INERT -- target 0x{}: {}. The handle stays "
                         "live and pins nothing; Repoint it to an actor or Release it.",
                         Hex(id), what, Hex(tf), why);
        } else {
            spdlog::info("[ch.20] 0x{} target-pin {} -> target 0x{}. The engine's target selection is answered "
                         "with it while the actor is fighting and it is one of the group's combat targets; never "
                         "starts combat.",
                         Hex(id), what, Hex(tf));
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
            struct { std::uint32_t hits, denied, held, none, notTgt, gone, lost, missed, over; } c{};
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
                          p.overwritten.load() };
                    why = p.endedReason.load();
                    g_pins.erase(it);
                }
                g_pinCount.store(g_pins.size(), std::memory_order_relaxed);
            }
            if (had) {
                spdlog::info("[ch.20] 0x{} target-pin released ({}) (target 0x{}): selector seat called {} time(s) -- "
                             "denied the engine's pick {}, engine already on the pin {}, engine had no target {}, "
                             "pin not a combat target {}, target gone {}, target lost {}; observer: seat missed {}, "
                             "overwritten after the seat {}. The engine's own selection answers again.",
                             Hex(id), why ? fmt::format("ENDED BY HARBINGER: {}", why) : std::string("by the client, "
                             "an unload or a load"), Hex(t), c.hits, c.denied, c.held, c.none, c.notTgt, c.gone,
                             c.lost, c.missed, c.over);
            } else {
                spdlog::info("[ch.20] 0x{} target-pin released (it was inert).", Hex(id));
            }
        }
    };

}

namespace apmf::targetpin {

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the seats are verified on 1.6.1170 and 1.5.97 only)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (the seats are verified on those two only)";
        } else if (GetPrivateProfileIntA("TargetPin", "bTargetPin", 1, kIni) == 0) {
            why = "[TargetPin] bTargetPin=0 in Data/SKSE/Plugins/APMF.ini";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.20] target-pin seats NOT installed -- {}. kIntent_TargetPin claims are REFUSED.", why);
            return;
        }

        // Verify ALL THREE before writing ANY: a pin with half a mechanism is refused whole.
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        REL::Relocation<std::uintptr_t> vtStd{ RE::VTABLE_CombatTargetSelectorStandard[0] };
        REL::Relocation<std::uintptr_t> vtFix{ RE::VTABLE_CombatTargetSelectorFixed[0] };
        bool ok = apmf::allowance::SeatVerified(vtStd.address(), "TargetPin.CombatTargetSelectorStandard.SelectTarget");
        ok = apmf::allowance::SeatVerified(vtFix.address(), "TargetPin.CombatTargetSelectorFixed.SelectTarget") && ok;
        ok = apmf::allowance::SeatVerified(vtChar.address(), "TargetPin.Character.UpdateCombat(observer)") && ok;
        if (!ok) {
            g_notInstalledReason.store("the address self-check refused a seat vtable", std::memory_order_release);
            spdlog::error("[ch.20] target-pin seats NOT installed (self-check refused a vtable, listed above). "
                          "kIntent_TargetPin claims are REFUSED.");
            return;
        }

        g_vtStandard = vtStd.address();
        g_vtFixed    = vtFix.address();
        SelectStandardHook::func   = vtStd.write_vfunc(kSelectTargetSlot, SelectStandardHook::thunk);
        SelectFixedHook::func      = vtFix.write_vfunc(kSelectTargetSlot, SelectFixedHook::thunk);
        UpdateCombatObserver::func = vtChar.write_vfunc(UpdateCombatObserver::idx, UpdateCombatObserver::thunk);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.20] target-pin seats installed: SOURCE DENY on CombatTargetSelectorStandard / Fixed "
                     "SelectTarget (slot 0x{:X}, chaining), OBSERVE-ONLY on Character::UpdateCombat (slot 0x{:X}).",
                     kSelectTargetSlot, UpdateCombatObserver::idx);
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
            const char* why = sn.seatWhy == kReasonLost ? kReasonLost : nullptr;
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

    void ResetAll(const char* why) {
        std::size_t n = 0;
        {
            std::unique_lock lk(g_pinMx);
            n = g_pins.size();
            g_pins.clear();
            g_pinCount.store(0, std::memory_order_relaxed);
        }
        if (n != 0) spdlog::info("[ch.20] {} -- dropped {} target-pin entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(TargetPinChannel);
