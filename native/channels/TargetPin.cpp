#include "PCH.h"
#include "channels/TargetPin.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
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
// 86e3cr9u7: "yes, start the target pin").
//
// WHAT IT IS. A client claims {actor, target}. While that claim is the WINNING
// kIntent_TargetPin claim on the actor, APMF's seat on Character::UpdateCombat (vtable
// slot 0xE4) runs the engine's own combat update FIRST and then, if the engine HOLDS a
// combat target that is not the claimed one, writes the claimed target back into
// `currentCombatTarget` and the CombatController's `targetHandle` (the old one moves to
// `previousTargetHandle`). The engine's own AI then fights that target: its own attack
// selection, movement, spells and equips. Lifted from MFO's field-proven
// native/Targeting.cpp (UpdateCombat hook, MFO origin/main) into Harbinger as a
// first-class facet, so a client needs no hook of its own.
//
// THE GUARD THAT IS THE WHOLE SAFETY ARGUMENT: NEVER WRITE WHEN THE ENGINE HAS NO TARGET.
// If vanilla cleared the target -- the foe fled, died, went undetected, combat ended --
// it did that for a reason Harbinger cannot see. Commanding WHICH foe is ours;
// commanding THAT there is a foe is not. So the pin never starts a fight and never
// revives one the engine ended (INVARIANTS #0: combat entry is forbidden here, and
// this is the line that keeps it out). The same guard is what makes the pin stop by
// itself when the engine ends combat: StopCombat nulls currentCombatTarget (MFO
// ENGINE_NOTES 0.47 item 1, both runtimes), so the next update writes nothing.
//
// DENY-COMPLETENESS (principle 2), stated plainly. The facet is "which actor this NPC
// fights". Its competing sources, and what happens to each:
//   * the ENGINE'S OWN re-pick (CombatTargetSelector, threat) -- DENIED, at the seat,
//     every combat update, before the next update reads it. That is the point.
//     It is a SEAT-TIME CORRECTION, not a source block: inside one UpdateCombat call
//     the engine's own pick exists until the original returns, and whatever that same
//     call consumed from it (its behaviour tree runs off the controller) saw the
//     engine's pick for that one update. That is the shape MFO field-proved and the
//     brief specified; a source-level deny of CombatTargetSelector is NOT built and is
//     recorded as an open item (Docs/DENY-COMPLETENESS-AUDIT.md row 20). INVARIANTS #2's
//     label applies: this is a known-incomplete block at one-update granularity, not a
//     clean gate, and it says so here.
//   * another framework's OWN currentCombatTarget write -- NOT denied (ch.6 row 6 gap,
//     unchanged). A framework whose seat runs after ours (a later write_vfunc on the
//     same slot chains OUTSIDE us and so writes last) wins. That includes MFO's own
//     Targeting hook during the transition: both are chaining vtable hooks and neither
//     replaces the other; MFO's Targeting must defer to the claim owner (MFO's task).
//   * NOTHING ELSE is claimed: no attack selection (ch.7), no casting (ch.8/8b), no
//     equip (ch.15/17), no movement (ch.1), no aggression (ch.11), no combat entry.
//
// WHAT THE CLIENT OWNS (principle 2 scope, marth 2026-09-25). Pinning an NPC onto a
// guard, a friendly or a bystander produces crime, bounty, faction, aggression and
// ally-joining reactions exactly as the engine does them. Harbinger does not deny
// them and Release does not undo them.
//
// THREADING (MFO ENGINE_NOTES 0.47 item 3). UpdateCombat runs as a JOB on the BSJobs
// worker threads, several actors in parallel; its one call site (AE 0x83f5e0+0x3c,
// SE 0x7a8390+0x3c) holds a NiPointer on the actor across the call, so `a_this` lives
// through this thunk. The thunk therefore:
//   * reads the CLAIM from the ControlMap's published RCU snapshot (TryGetOwningClaim,
//     lock-free, any thread) -- the pin follows the WINNING claim only;
//   * reads the TARGET'S HANDLE from this file's own map under a shared_lock. The
//     handle is resolved ONCE on the game thread (Engage / OnOwnerChanged, where form
//     lookups are legal) -- the thunk never does a form lookup (the CastSeatClaim
//     rule, core/ControlMap.h);
//   * requires the two to AGREE (map entry's target FormID == the published claim's
//     param.form). Between a Drain applying a Repoint and its Publish the two can
//     briefly disagree; the pin then writes nothing for that update (engine keeps its
//     own pick) -- never a write of a stale target;
//   * touches ONLY this actor's own fields, after its own original returned, on the
//     same thread. The controller pointer is read once. A StopCombat on ANOTHER
//     thread against this same actor while this thunk runs (a script, a package
//     evaluation, a kill -- 0.47 item 4) is the same race the engine's own update body
//     and MFO's hook live with; it is flagged for review, not hidden.
//
// VERSION ROBUSTNESS (CLAUDE.md "What breaks" 4). Slot 0xE4 is SE == AE (CommonLib's
// RelocateVirtual second index is VR) and holds Actor::UpdateCombat on the Character
// vtable on BOTH unpacked images: AE 0x6B6E70 / SE 0x625700, signature `void(Actor*)`
// (the call site sets only rcx and discards rax). Recorded in spec.json (Character row,
// slot 0xE4) and guarded at install by SeatVerified + an EXACT-runtime gate + VR
// refusal + the [TargetPin] bTargetPin INI kill-switch. `currentCombatTarget` is
// reached through CommonLib's per-runtime ACTOR_RUNTIME_DATA accessor (SE +0xFC /
// AE +0x104 absolute); `targetHandle` 0x2C and `previousTargetHandle` 0x30 sit below the
// CombatController's +0x68 AE divergence (static_asserts below).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the seat installs there)" };

    // One entry per actor with an engaged ch.20. Written ONLY on the game thread (the
    // lifecycle calls run inside ControlMap::Drain / ReleaseAll; ResetAll at the world
    // boundary) under a unique_lock; read by the seat on BSJobs workers under a
    // shared_lock. The counters are the seat's only mutation and are atomics, so a
    // shared_lock is enough for them.
    struct Pin {
        RE::FormID      target = 0;    // the claim's param.form this handle was resolved for
        RE::ActorHandle handle{};      // resolved on the game thread; never looked up by the seat
        mutable std::atomic<std::uint32_t> writes{ 0 };        // engine held another target: pinned back
        mutable std::atomic<std::uint32_t> held{ 0 };          // engine already held ours
        mutable std::atomic<std::uint32_t> noEngineTarget{ 0 };// engine held none: nothing written (the guard)
        mutable std::atomic<std::uint32_t> targetGone{ 0 };    // target dead/disabled/unloaded/unresolved
        mutable std::atomic<bool>          firstWriteLogged{ false };
    };

    std::shared_mutex                  g_pinMx;
    std::unordered_map<RE::FormID, Pin> g_pins;
    // Relaxed pre-gate: the seat fires for EVERY Character in combat in the world; with no
    // pin anywhere this one load is its whole cost. Updated under g_pinMx's unique_lock.
    std::atomic<std::size_t>           g_pinCount{ 0 };

    // AE +8 LAYOUT GUARD (CLAUDE.md "What breaks" 4; the fork static_asserts
    // sizeof(CombatController) == 0x68 and moves everything past it behind
    // GetRuntimeData()). Both handles this seat writes must stay below +0x68.
    static_assert(offsetof(RE::CombatController, targetHandle) < 0x68,
                  "targetHandle is past the CombatController AE layout divergence (+0x68)");
    static_assert(offsetof(RE::CombatController, previousTargetHandle) < 0x68,
                  "previousTargetHandle is past the CombatController AE layout divergence (+0x68)");

    struct UpdateCombatHook {
        static void thunk(RE::Actor* a_this) {
            // ALWAYS the engine first. The pin corrects the engine's choice after it is
            // made; it never replaces the engine's combat bookkeeping.
            func(a_this);

            if (g_pinCount.load(std::memory_order_relaxed) == 0 || !a_this) return;

            const RE::FormID          self = a_this->GetFormID();
            APMF_API::APMF_Param      claim{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(self, APMF_API::kIntent_TargetPin, claim)) return;
            if (claim.form == 0) return;

            std::shared_lock lk(g_pinMx);
            const auto it = g_pins.find(self);
            // No entry (refused at engage) or an entry for another target (a Repoint not
            // yet published, or published but not yet applied here): write nothing.
            if (it == g_pins.end() || it->second.target != claim.form) return;
            const Pin& pin = it->second;

            // Hold the NiPointer for the whole body -- never `.get().get()`, which drops
            // the reference at the end of the statement.
            auto        targetPtr = pin.handle.get();
            RE::Actor*  target    = targetPtr.get();
            if (!target || target->IsDead() || target->IsDisabled() || !target->Is3DLoaded()) {
                pin.targetGone.fetch_add(1, std::memory_order_relaxed);
                return;   // the pin stops; the engine's own targeting stands
            }
            if (a_this->IsDead()) return;

            auto& rt         = a_this->GetActorRuntimeData();
            auto  currentPtr = rt.currentCombatTarget.get();
            if (!currentPtr) {
                // THE GUARD: the engine holds no target. Never start or revive a fight.
                pin.noEngineTarget.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (currentPtr.get() == target) {
                pin.held.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const RE::FormID engineChoice = currentPtr->GetFormID();
            rt.currentCombatTarget = pin.handle;
            if (auto* cc = rt.combatController) {
                cc->previousTargetHandle = cc->targetHandle;
                cc->targetHandle         = pin.handle;
            }
            pin.writes.fetch_add(1, std::memory_order_relaxed);

            // One line per engagement, so a log shows the seat EXECUTING (principle 5)
            // without a line per combat update.
            if (!pin.firstWriteLogged.exchange(true, std::memory_order_relaxed)) {
                spdlog::info("[ch.20] 0x{} FIRST PIN: the engine picked 0x{}, pinned back to 0x{} (combat "
                             "controller {}).",
                             Hex(self), Hex(engineChoice), Hex(claim.form),
                             rt.combatController ? "present" : "ABSENT -- currentCombatTarget only");
            }
        }

        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0xE4;   // Actor::UpdateCombat (SE == AE index)
    };

    // Resolve `param.form` on the game thread and (re)write this actor's entry. A refused
    // target ERASES the entry, so the claim stays live but inert and the log says why.
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
            spdlog::info("[ch.20] 0x{} target-pin {} -> target 0x{}. Pins only while the engine holds a "
                         "combat target; never starts combat.",
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

        // Relinquish (INVARIANTS #5a): nothing to restore. The engine's own targeting
        // takes over on the next combat update; the target the pin last wrote stays
        // until the engine re-picks, exactly as any engine pick would.
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            std::uint32_t w = 0, hd = 0, ne = 0, tg = 0;
            RE::FormID    t = 0;
            bool          had = false;
            {
                std::unique_lock lk(g_pinMx);
                if (const auto it = g_pins.find(id); it != g_pins.end()) {
                    had = true;
                    t   = it->second.target;
                    w   = it->second.writes.load(std::memory_order_relaxed);
                    hd  = it->second.held.load(std::memory_order_relaxed);
                    ne  = it->second.noEngineTarget.load(std::memory_order_relaxed);
                    tg  = it->second.targetGone.load(std::memory_order_relaxed);
                    g_pins.erase(it);
                }
                g_pinCount.store(g_pins.size(), std::memory_order_relaxed);
            }
            if (had) {
                spdlog::info("[ch.20] 0x{} target-pin released (target 0x{}): pinned back {} engine re-pick(s), "
                             "held {} update(s), engine had no target on {}, target gone on {}. The engine's own "
                             "targeting takes over.",
                             Hex(id), Hex(t), w, hd, ne, tg);
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
            why = "VR runtime (slot 0xE4 is not verified for VR)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (the seat is verified on those two only)";
        } else if (GetPrivateProfileIntA("TargetPin", "bTargetPin", 1, kIni) == 0) {
            why = "[TargetPin] bTargetPin=0 in Data/SKSE/Plugins/APMF.ini";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.20] target-pin seat NOT installed -- {}. kIntent_TargetPin claims are REFUSED.", why);
            return;
        }

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
        if (!apmf::allowance::SeatVerified(vtbl.address(), "TargetPin.Character.UpdateCombat")) {
            g_notInstalledReason.store("the address self-check refused the Character vtable",
                                       std::memory_order_release);
            spdlog::error("[ch.20] target-pin seat NOT installed (self-check refused the Character vtable). "
                          "kIntent_TargetPin claims are REFUSED.");
            return;
        }
        UpdateCombatHook::func = vtbl.write_vfunc(UpdateCombatHook::idx, UpdateCombatHook::thunk);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.20] target-pin seat installed (Character::UpdateCombat, vtable slot 0x{:X}, chaining: "
                     "the engine's update runs first).",
                     UpdateCombatHook::idx);
    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
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
