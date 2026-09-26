#include "PCH.h"
#include "channels/CombatEntry.h"
#include "channels/TargetPin.h"   // Installed(): whether the ch.20 pin could engage, for the entry log
#include "channels/CombatReentryDeny.h"   // ClientEntryScope: this entry passes a ch.22 re-entry deny
#include "core/Allowance.h"       // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/Travel.cpp, channels/TargetPin.cpp and core/EquipSink.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 21 -- COMBAT ENTRY (kIntent_CombatEntry, ABI v14, marth 2026-09-25, ClickUp
// 86e3940zb: "a third-party modder needs to make an NPC fight a target").
//
// WHAT IT IS. A client claims {actor, target}. Harbinger makes the actor ENTER COMBAT
// against that target through the engine's own entry function, Actor::StartCombat, ONCE
// per engage (and once more per Repoint / owner change), on the game thread. That is the
// whole mechanism. It is INVARIANTS #0 action (g), written there with its conditions.
// Together with ch.20 (kIntent_TargetPin, same target) the NPC fights that target: entry
// puts the target into the actor's combat group's targets, the pin answers the engine's
// target selector with it.
//
// THE ENGINE'S ENTRY PATH (disassembly of both unpacked images, 2026-09-25; scratchpad
// log apmf-combat-entry.md has the listings).
//   Actor::StartCombat: 1.6.1170 id 38561 at 0x6B6930, 1.5.97 id 37608 at 0x6251B0 (the
//   fork's binding, mit-3.7 fde0f3ae). SIGNATURE, every argument register checked (the
//   old ch.6 CTD was a 2-argument call leaving garbage in R8): rcx = this (kept in rdi),
//   rdx = target (rsi), r8 = a CombatGroup* to JOIN (rbp: [rbp+0x30] is the group's
//   member count, then CombatManager AE 46874 / SE 45574 builds the controller INTO that
//   group), returns `movzx eax, r14b` = bool. r8 = nullptr is the engine's own usage: its
//   caller AE 40814 does `xor r8d, r8d` at 0x7505A3 before the call. We pass nullptr.
//   It REFUSES (returns false, builds nothing) when: `this` is the global actor at AE id
//   401069 / SE id 514905; the actor's lifeState (AE [+0xC8] / SE [+0xC0], bits 21-24) is
//   6 restrained or 3 unconscious; target == this; this->IsDead(true) (vtable 0x99); a
//   boolFlags bit (AE [+0xE8] / SE [+0xE0], bit 11); a process test (AE 39445 / SE 38447);
//   target->IsDead(false); AE 38303 && !37899 (SE 21343 && !36875); and a DISTANCE test
//   (when AE 37346 / SE 36355 returns 0.0: squared distance > (AE 37478 / SE 36479 * 1.25)^2).
//   It dereferences the actor's currentProcess WITHOUT a null check (AE 0x6B69E3 -> 39445
//   reads [process+0x10]; SE 0x625261 -> 38447), so this channel never calls it for an
//   actor with no process (see the gates in Enter()).
//   The body runs under a GLOBAL spinlock (AE `lock cmpxchg` at 0x6B6A85 / SE 0x625303),
//   which is the engine's own serialisation of concurrent entries.
//   NOT IN COMBAT (controller AE [+0x160] / SE [+0x158] null): after its own re-arm equip
//   (the ch.17 seat governs that), CombatManager AE 46873 / SE 45573 builds a new
//   CombatGroup (46899 / 45599), adds the actor as a member, constructs the 0xE0-byte
//   CombatController (ctor AE 33214 / SE 32467, which builds its Standard target selector),
//   then calls CombatGroup::AddTarget(group, target) (AE 44704 / SE 43482). IF AddTarget
//   FAILS THE NEW CONTROLLER IS DESTROYED and StartCombat returns false. So on this path
//   "the engine entered" and "the target is a combat-group target" are the same fact.
//   ALREADY IN COMBAT: if the target's handle differs from the controller's targetHandle
//   (+0x2C), AE 33228 / SE 32481 calls AddTarget(group, target) and IGNORES its result;
//   StartCombat returns true. No SetTarget: the engine's selectors still choose (and ch.20
//   answers them). So on this path the return value says nothing about membership, and
//   the entry log does NOT read the group to find out (APMF-B26: no controller or group
//   dereference from the main thread); ch.20's own log says whether its pin engages.
//   AddTarget: a null target fails; an existing member succeeds (AE 44706 / SE 43484 under
//   the group's read lock); otherwise it needs AE 44708 true and 44713 false (SE 43486 /
//   43491), then appends a 0xA8-byte CombatTarget and tells each member (AE 37759).
//   A FIXED target selector is NOT built by StartCombat: the Fixed selector ctor (AE 47197)
//   has two callers, AE 40814 +0x1BB (0x75066B -- a StartCombat caller that builds one
//   with priority 3 in a global slot) and AE 48846. The ch.20 pin covers both classes.
//   THE ENGINE ENDS COMBAT through Actor::StopCombat (Character vtable 0xE5, AE 0x6B70A0
//   id 38566 / SE 0x625920 id 37613), which destroys the controller, clears it and clears
//   currentCombatTarget -- or through its own group / AI-reset paths. This channel calls
//   none of them.
//
// RELEASE AND END (marth: "on release Harbinger stops forcing and does not undo"; and, for
// ch.20, "if APMF can no longer track the target, it's lost and dropped" -- so NO claim of
// this channel is ever left live and inert).
//   * ONE CALL PER DECLARATION. One StartCombat per Engage and one per OnOwnerChanged (a
//     Repoint, or a new winning claim); no Tick, no watch, no retry, no re-entry when the
//     engine ends the fight. Each call answers a declaration the client (or the new winning
//     client) just made, so it is not SUSTAINING a decision (#0 (g) condition 3).
//   * Release calls NO StopCombat. Once the engine has entered, the fight is ENGINE STATE
//     (a controller, a group, allies told, detection, crime), not something Harbinger holds;
//     stopping it would be an UNDO, and it would also tear down any combat the actor is in
//     for its own reasons (it was attacked meanwhile, or was already fighting when the claim
//     arrived and StartCombat only added a target). The engine ends it the way it ends any
//     fight; a client that wants it over calls Actor::StopCombat itself.
//   * THE CLAIM ENDS -- Harbinger releases it and logs "entry ended: <reason>", repeated on
//     the Release line -- when:
//       - the engine REFUSED the entry ("engine refused entry: <cause if known>");
//       - the entry could not be attempted (actor not loaded / no AI process / dead; target
//         not an Actor / not loaded / disabled / dead / unresolvable) -- Enter() or Apply();
//       - the fight ENDED: the entry succeeded and the actor's combatController pointer is
//         now null ("combat ended") -- Poll(), reading the POINTER only, never the controller
//         (Actor::IsInCombat is not used: its body reads a byte at controller +0x43, AE
//         0x6B6DD0 / SE 0x625660, which is a dereference; B26);
//       - the owner died, or the target died, was disabled, unloaded or stopped resolving
//         -- Poll().
//     A client that wants another entry sends a NEW request. (A Repoint of a still-live
//     claim makes one more call too, but an ended claim is gone.)
//   * THE ENGINE GAVE UP = DROPPED (review F1, option a). When a claim ends because the engine
//     ended the fight ("combat ended") or refused the entry, and a RIVAL claim on the same actor
//     takes over naming the SAME target, it does NOT re-enter: its owner-change Apply ends it
//     with the same reason. The reason is kept on the actor's entry keyed by target
//     (engineGaveUp), across Apply, until the actor's last combat-entry claim is released. A
//     rival naming a DIFFERENT target still gets its one call.
//   * Save load / revert / new game: claims are never saved (ControlMap::Clear), so the
//     entry state is dropped with them (ResetAll) and a task still queued is discarded with
//     the main-thread queue. Whatever combat the engine was in is saved and restored by the
//     ENGINE; the client re-requests if it wants Harbinger again.
//
// DENY-COMPLETENESS (principle 2, with its 2026-09-25 scope: the client owns the
// consequences). The facet is "enter combat against this target". Harbinger switches NO
// facet on as a substrate: combat itself is what the client asked for, so what combat
// brings (weapon draw, StartCombat's own re-arm equip, music, detection, crime, allies and
// guards joining) is the declared action's consequence and is not denied. Competing sources
// are not denied either: the engine may pick another target (claim ch.20 to answer that),
// and anyone may end the fight. Nothing here overrides the engine after the one call.
//
// THREADING. Engage / OnOwnerChanged / Release run inside ControlMap::Drain on the
// confirmed-main seat (the player 0xAD seat, core/MainThread.h). Anything that ENDS a claim
// (EnqueueRelease) runs from mainthread::Pump or Poll, never from inside Drain (Channel.h:
// no ControlMap write from a lifecycle call) -- Apply() posts its ending. The entry task
// runs from Pump right after Drain PUBLISHES (it re-reads the published claim, the
// OfferPackage precedent); Poll runs on the same seat. g_entries is therefore touched on
// ONE thread only and takes no lock. After the call the channel reads the actor's
// combatController POINTER (null or not) and nothing behind it: no controller or group
// dereference from the main thread (APMF-B26's exposure is not widened).
//
// VERSION ROBUSTNESS. The only engine address is StartCombat's, a verified row on both
// runtimes (spec.json "CombatEntry.Actor.StartCombat"), refused unless it verifies; exact
// 1.6.1170 / 1.5.97 only (the fork's binding also refuses VR by name); [CombatEntry]
// bCombatEntry. The only raw read is the actor's own combatController pointer, through the
// fork's ACTOR_RUNTIME_DATA accessor.
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    // Per-actor rate limit for the entry log line (a client may Repoint every frame).
    constexpr std::uint64_t kLogEveryMs = 2000;

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the channel is gated there)" };

    // One entry per actor with an engaged ch.21. GAME THREAD ONLY (see THREADING).
    struct Entry {
        RE::FormID      target = 0;    // the claim's param.form this entry was resolved for
        RE::ActorHandle handle{};      // resolved on the game thread at Engage / OnOwnerChanged
        std::uint32_t   gen = 0;       // bumps per Apply; a posted task carries it
        std::uint32_t   attempts = 0;  // StartCombat calls made
        std::uint32_t   entered = 0;   // ... that returned true
        std::uint32_t   refused = 0;   // ... that returned false (the engine refused)
        std::uint32_t   suppressed = 0;   // entry log lines held back by the rate limit
        std::uint64_t   lastLogMs = 0;
        bool            resolved = false;   // the target resolved to an actor (Poll watches only these)
        bool            inFight = false;    // the last call entered and the controller was present
        bool            ending = false;     // Harbinger is releasing this claim
        std::string     endedReason;        // for the Release line
        // Review F1 (option a, marth: "the engine gave up = dropped"): targets whose entry the
        // ENGINE ended ("combat ended") or refused, with that reason. Keyed by TARGET and kept
        // across Apply (the gen bump does not wipe it) for as long as this actor has any
        // combat-entry claim: a rival claim taking over with the SAME target is ended with the
        // same reason instead of re-entering. Dropped with the entry on the last Release.
        std::unordered_map<RE::FormID, std::string> engineGaveUp;
    };

    std::unordered_map<RE::FormID, Entry> g_entries;
    std::atomic<std::size_t>              g_count{ 0 };   // Poll's pre-gate
    std::uint32_t                         g_nextGen = 0;

    bool HasController(RE::Actor* a) {
        return a->GetActorRuntimeData().combatController != nullptr;   // the POINTER only (B26)
    }

    // End the WINNING claim this entry was resolved for: record the reason, EnqueueRelease
    // its handle, log it. MAIN SEAT, OUTSIDE Drain only (Pump / Poll). A claim that has moved
    // on (a newer Apply) is left alone; that Apply owns the entry now.
    void EndClaim(RE::FormID id, RE::FormID tf, std::string why) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.target != tf || it->second.ending) return;
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_CombatEntry, param, basis, &h) ||
            param.form != tf || h == APMF_API::kInvalidHandle)
            return;
        it->second.ending      = true;
        if (why.starts_with("combat ended") || why.starts_with("engine refused entry"))
            it->second.engineGaveUp.try_emplace(tf, why);   // keep the FIRST (engine) reason
        it->second.endedReason = why;
        apmf::ControlMap::Get().EnqueueRelease(h);
        spdlog::info("[ch.21] 0x{} entry ended: {} (target 0x{}, claim h={}). Harbinger released the claim and "
                     "stops nothing; a mod that wants another entry sends a new request.",
                     Hex(id), why, Hex(tf), h);
    }

    // What the pin side looks like, for the log only. No group read (B26): on the new-fight
    // path membership is the return value; on the already-in-combat path ch.20's own log says.
    std::string PinText(RE::FormID id, RE::FormID tf) {
        if (!apmf::targetpin::Installed()) return "no (the ch.20 seats are not installed)";
        APMF_API::APMF_Param pin{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_TargetPin, pin))
            return "no ch.20 claim yet (the engine's own selector chooses; claim kIntent_TargetPin to hold it)";
        if (pin.form != tf) return fmt::format("no (the winning pin names 0x{}, not this target)", Hex(pin.form));
        return "a ch.20 claim names this target";
    }

    // The one engine call. Posted by Apply(); runs from mainthread::Pump right after Drain
    // published the claim. Re-validates everything it was posted for and drops itself
    // (logged) if anything moved. Every outcome other than "entered" ENDS the claim.
    void Enter(RE::FormID id, RE::FormID tf, std::uint32_t gen, const char* what) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.target != tf || it->second.ending) {
            spdlog::info("[ch.21] 0x{} entry for target 0x{} DROPPED (stale): a newer engage, a release or a "
                         "load landed before it ran.",
                         Hex(id), Hex(tf));
            return;
        }

        APMF_API::APMF_Param now{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_CombatEntry, now) || now.form != tf) {
            spdlog::info("[ch.21] 0x{} entry for target 0x{} DROPPED (stale): the published winning claim now "
                         "names 0x{}.",
                         Hex(id), Hex(tf), Hex(now.form));
            return;
        }

        auto* actor  = RE::TESForm::LookupByID<RE::Actor>(id);
        auto  tptr   = it->second.handle.get();
        auto* target = tptr.get();
        const char* why = nullptr;
        if (!actor)                                              why = "the actor does not resolve";
        else if (actor->IsPlayerRef())                           why = "the actor is the player";
        else if (!actor->Is3DLoaded())                           why = "the actor is not loaded";
        else if (!actor->GetActorRuntimeData().currentProcess)  why = "the actor has no AI process "
                                                                      "(StartCombat dereferences it unchecked)";
        else if (actor->IsDead())                                why = "the actor is dead";
        else if (!target)                                        why = "the target does not resolve";
        else if (target->IsDead())                               why = "the target is dead";
        else if (target->IsDisabled())                           why = "the target is disabled";
        else if (!target->Is3DLoaded())                          why = "the target is not loaded";
        if (why) {
            spdlog::warn("[ch.21] 0x{} entry {} against 0x{} NOT CALLED: {}. No engine call was made.",
                         Hex(id), what, Hex(tf), why);
            EndClaim(id, tf, fmt::format("entry not attempted: {}", why));
            return;
        }

        const bool wasInCombat = HasController(actor);
        const auto lifeState   = actor->AsActorState()->GetLifeState();

        // THE CALL. Once. The fork's binding: RELOCATION_ID(37608, 38561), bool(Actor*,
        // Actor*, void*); nullptr third argument = "no group to join" (the engine's own
        // usage, AE 0x7505A3). The scope lets it through a ch.22 re-entry deny on this actor:
        // a client's declared entry is not denied (INVARIANTS #0 (h) condition 4).
        bool ok = false;
        {
            const apmf::reentrydeny::ClientEntryScope pass(id);
            ok = actor->StartCombat(target, nullptr);
        }

        Entry& e = it->second;   // no insertion/erase since `it` was taken (StartCombat never calls back into us)
        ++e.attempts;
        ok ? ++e.entered : ++e.refused;
        const bool hasCc = HasController(actor);
        e.inFight        = ok && hasCc;

        if (!ok) {
            // The engine's refusal preconditions (disassembly, header). Name the ones we can see.
            const char* cause =
                lifeState == RE::ACTOR_LIFE_STATE::kRestrained ? "the actor is restrained" :
                lifeState == RE::ACTOR_LIFE_STATE::kUnconcious ? "the actor is unconscious" :
                actor->IsDead(true)                            ? "the actor is dead" :   // engine: dl=1, AE 0x6B69BF
                target->IsDead(false)                          ? "the target is dead" :  // engine: edx=0, AE 0x6B69FF
                                                                  "cause not reported by the engine (its distance "
                                                                  "test, its global-actor identity test -- AE id "
                                                                  "401069 / SE id 514905 -- or one of its actor / "
                                                                  "process flags)";
            spdlog::warn("[ch.21] 0x{} entry {} against 0x{}: REQUESTED -> the engine REFUSED (StartCombat returned "
                         "false; {} before the call).",
                         Hex(id), what, Hex(tf), wasInCombat ? "in combat" : "not in combat");
            EndClaim(id, tf, fmt::format("engine refused entry: {}", cause));
            return;
        }
        if (!hasCc) {   // a true return must leave a controller; say so loudly if it did not (principle 7)
            spdlog::warn("[ch.21] 0x{} entry {} against 0x{}: StartCombat returned TRUE but the actor has NO "
                         "combat controller after the call.",
                         Hex(id), what, Hex(tf));
            EndClaim(id, tf, "combat ended (no controller right after a successful entry)");
            return;
        }

        const auto nowMs = apmf::clock::MonotonicMs();
        if (e.lastLogMs != 0 && nowMs - e.lastLogMs < kLogEveryMs) {
            ++e.suppressed;
            return;
        }
        e.lastLogMs = nowMs;
        const std::uint32_t held = std::exchange(e.suppressed, 0);
        spdlog::info("[ch.21] 0x{} entry {} against 0x{}: REQUESTED -> the engine ENTERED ({}). Target is a "
                     "combat-group target: {}. Pin: {}.{}",
                     Hex(id), what, Hex(tf),
                     wasInCombat ? "was already in combat: the engine was asked to add the target to its group"
                                 : "was not in combat: new controller and group",
                     wasInCombat ? "not read (already in combat: the engine does not report it; ch.20's log says "
                                   "whether its pin engages)"
                                 : "yes (a new fight is kept only when the engine accepted the target)",
                     PinText(id, tf),
                     held ? fmt::format(" ({} earlier entry line(s) suppressed)", held) : std::string());
    }

    // Posted by Apply() when the client's target cannot be used: the claim is ended, never
    // left live and inert. Runs from Pump (outside Drain).
    void EndUnusable(RE::FormID id, RE::FormID tf, std::uint32_t gen, std::string why) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.target != tf) return;   // moved on
        EndClaim(id, tf, std::move(why));
    }

    // Resolve `param.form` on the game thread, (re)write this actor's entry and post ONE
    // task: the entry call, or -- for a target that cannot be used (0, self, not an actor)
    // -- the claim's ending. Inside Drain: nothing here writes the ControlMap.
    void Apply(RE::FormID id, const APMF_API::APMF_Param& param, const char* what) {
        const RE::FormID tf  = param.form;
        const char*      why = nullptr;
        RE::ActorHandle  h{};
        if (tf == 0) {
            why = "param.form is 0 (an entry needs the TARGET actor's FormID)";
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

        // Keep the counters across a re-point (they are reported on Release).
        Entry e{};
        if (const auto it = g_entries.find(id); it != g_entries.end()) e = it->second;
        e.target      = tf;
        e.handle      = h;
        e.gen         = ++g_nextGen;
        e.resolved    = why == nullptr;
        e.inFight     = false;
        e.ending      = false;
        e.endedReason.clear();
        const std::uint32_t gen = e.gen;
        g_entries.insert_or_assign(id, std::move(e));
        g_count.store(g_entries.size(), std::memory_order_relaxed);

        if (why) {
            spdlog::warn("[ch.21] 0x{} combat-entry claim {} -- target 0x{} cannot be used: {}. No engine call; "
                         "Harbinger ends the claim.",
                         Hex(id), what, Hex(tf), why);
            std::string reason = fmt::format("entry not attempted: {}", why);
            apmf::mainthread::Post([id, tf, gen, reason] { EndUnusable(id, tf, gen, reason); });
            return;
        }
        // Review F1: the engine already gave up on THIS target for this actor (it ended the fight
        // or refused the entry, under an earlier claim). A claim that takes over naming the same
        // target does not re-enter: it ends with the same reason. A different target is a new
        // declaration and gets its one call.
        if (const auto& gave = g_entries.at(id).engineGaveUp; gave.contains(tf)) {
            std::string reason = fmt::format("{} (earlier claim; a claim taking over with the same target does "
                                             "not re-enter)",
                                             gave.at(tf));
            spdlog::warn("[ch.21] 0x{} combat-entry {} -> target 0x{}: NOT CALLED, the engine already gave up on "
                         "this target for this actor ({}). Harbinger ends the claim.",
                         Hex(id), what, Hex(tf), gave.at(tf));
            apmf::mainthread::Post([id, tf, gen, reason] { EndUnusable(id, tf, gen, reason); });
            return;
        }
        spdlog::info("[ch.21] 0x{} combat-entry {} -> target 0x{}: ONE Actor::StartCombat call is queued for the "
                     "next main-thread pump (after this claim is published).",
                     Hex(id), what, Hex(tf));
        apmf::mainthread::Post([id, tf, gen, what] { Enter(id, tf, gen, what); });
    }

    class CombatEntryChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-entry"; }
        int              ChannelNo() const override { return 21; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatEntry; }

        // Posted names are string literals (the task outlives this call).
        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "RE-POINTED");
        }

        // Relinquish (INVARIANTS #5a): nothing to restore, NO StopCombat (see RELEASE AND END).
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            const auto it = g_entries.find(id);
            if (it == g_entries.end()) {
                spdlog::info("[ch.21] 0x{} combat-entry released (no entry state).", Hex(id));
                return;
            }
            const Entry e = it->second;
            g_entries.erase(it);
            g_count.store(g_entries.size(), std::memory_order_relaxed);
            spdlog::info("[ch.21] 0x{} combat-entry released ({}) (target 0x{}): StartCombat called {} time(s) -- "
                         "entered {}, refused by the engine {}. Harbinger calls no StopCombat: the fight, if any, "
                         "is the engine's.",
                         Hex(id), e.endedReason.empty() ? std::string("by the client, an unload or a load")
                                                        : fmt::format("ENDED BY HARBINGER: {}", e.endedReason),
                         Hex(e.target), e.attempts, e.entered, e.refused);
        }
    };

}

namespace apmf::combatentry {

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (StartCombat is verified on 1.6.1170 and 1.5.97 only)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (StartCombat is verified on those two only)";
        } else if (GetPrivateProfileIntA("CombatEntry", "bCombatEntry", 1, kIni) == 0) {
            why = "[CombatEntry] bCombatEntry=0 in Data/SKSE/Plugins/APMF.ini";
        } else if (!apmf::allowance::SeatVerified(
                       REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37608, 38561) }.address(),
                       "CombatEntry.Actor.StartCombat")) {
            why = "the address self-check refused Actor::StartCombat";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.21] combat-entry NOT available -- {}. kIntent_CombatEntry claims are REFUSED.", why);
            return;
        }
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.21] combat-entry available: one Actor::StartCombat(target, nullptr) per engage / "
                     "re-point, on the main thread. No hook.");
    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Poll() {
        if (g_count.load(std::memory_order_relaxed) == 0) return;
        static std::uint64_t s_lastMs = 0;
        const std::uint64_t  now      = apmf::clock::MonotonicMs();
        if (now - s_lastMs < 250) return;
        s_lastMs = now;

        struct Snap { RE::FormID id, target; RE::ActorHandle handle; bool inFight; };
        std::vector<Snap> snaps;
        snaps.reserve(g_entries.size());
        for (const auto& [id, e] : g_entries)
            if (!e.ending && e.resolved) snaps.push_back({ id, e.target, e.handle, e.inFight });

        for (const auto& sn : snaps) {
            const char* why   = nullptr;
            auto*       owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            auto        tptr  = sn.handle.get();
            if (owner && owner->IsDead())                   why = "owner dead";
            else if (!tptr)                                 why = "target unresolvable";
            else if (tptr->IsDead())                        why = "target dead";
            else if (tptr->IsDisabled())                    why = "target disabled";
            else if (!tptr->Is3DLoaded())                   why = "target unloaded";
            else if (sn.inFight && owner && !HasController(owner))
                                                            why = "combat ended";
            if (why) EndClaim(sn.id, sn.target, why);
        }
    }

    void ResetAll(const char* why) {
        const std::size_t n = g_entries.size();
        g_entries.clear();
        g_count.store(0, std::memory_order_relaxed);
        if (n != 0) spdlog::info("[ch.21] {} -- dropped {} combat-entry entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(CombatEntryChannel);
