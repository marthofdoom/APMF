#include "PCH.h"
#include "channels/CombatEntry.h"
#include "channels/TargetPin.h"   // Installed(): whether the ch.20 pin could engage, for the entry log
#include "core/FacetAttribution.h"   // Harbinger 1b: the engine's refusal is named on the Combat facet
#include "channels/CombatReentryDeny.h"   // ClientEntryScope: this entry passes a ch.22 re-entry deny
#include "core/Allowance.h"       // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"
#include "core/Sources.h"         // the held engagement: census of its two call-site seats
#include "channels/CombatApproach.h"   // the held engagement's bleed-out drive (a Harbinger-internal bound)

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

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
            // Harbinger 1b: the engine decided this actor's combat entry against the claim (attribution
            // only: ch.21 enforces nothing; the engine's refusal stands).
            apmf::facetattr::Observe(apmf::sources::Facet::kCombat, apmf::sources::kEngineSource, id, "");
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
        // THE HELD ENGAGEMENT: this actor now holds an engagement on `tf` (or nothing, for an unusable target).
        if (why) apmf::combatentry::hold::ClearEntry(id);
        else apmf::combatentry::hold::SetEntry(id, tf, h);

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
            apmf::combatentry::hold::ClearEntry(id);
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
            why = "VR runtime (StartCombat is verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {   // F2b: 1.7.104 proven (StartCombat 38561 identical)
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (StartCombat is verified on those three only)";
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
        for (const auto& [id, e] : g_entries) hold::ClearEntry(id);
        g_entries.clear();
        g_count.store(0, std::memory_order_relaxed);
        if (n != 0) spdlog::info("[ch.21] {} -- dropped {} combat-entry entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

// ============================================================================
// THE HELD ENGAGEMENT (marth 2026-10-12, after the LoreRim field run of APMF 7f2d638 + MFO 03b25ff:
// "The correct call is once an engagement is called in harbinger its move-to, it cannot be lost."
// and, on a pinned foe in bleed-out: "Theres no problem targeting a downed enemy, but if it wont
// path to it then theres the issue."). INVARIANTS #0 (i).
//
// WHAT WENT WRONG. A client (MFO's engage-on-sight) called ch.21 against a foe Harbinger's own senses
// found (its ray, or hearing). The ENGINE had not detected that foe: on its first detection update
// for the new target (~260 ms after entry, 27 of 29 entries in the field) it flagged the group's
// CombatTarget kTargetLost. ch.20 then ended the pin ("pin ended: target lost"), the fight stayed
// (a lost target is still a group target), nobody drove it, and the engine SEARCHED for 75-90 s
// until its own ShouldRemoveTarget timed the target out. Outside a ch.23 leash, every search leaf
// that would take the follower farther was denied and it stood still.
//
// WHERE "LOST" COMES FROM (disassembly of both unpacked images, 2026-10-12; agentlog
// apmf-engagement-movto.md). CombatTarget::flags bit 1 (kTargetLost, u16 +0xA6) is written by ONE
// engine function, CombatGroup::UpdateTargetDetection (AE id 44725 0x805090 / SE id 43501 0x76B5D0;
// the SetLost-shaped helper AE 0x802B60 has no callers, every other +0xA6 write is an array copy).
// It runs per (member, target) detection event and sets kTargetLost on three paths: the member's
// detection level is positive but under the engine's threshold (AE 0x8053EA -- it still refreshes
// the last-known location to the target's real position), the last-known time (+0x90) is more than
// 10 s old (AE 0x805462), or the level is negative (AE 0x8054F2). Each path stamps +0x94 with the
// AI clock's "now". Its own UN-lose (AE 0x805483) clears the bit and stores -FLT_MAX in +0x94. Two
// callers, both plain E8 calls: CombatGroupDetectionListener::ProcessEvent (its vtable slot 2,
// AE id 44699 +0x85 / SE id 43477 +0x85) and the group's periodic member x target detection pass
// (AE id 44804 +0x143 / SE id 43572 +0x143). Downstream, the lost bit is what makes the movement
// selector take SEARCH (channels/CombatApproach.cpp header: Search sits above Close Movement) and
// what CombatGroup::ShouldRemoveTarget (AE 0x80C680) times out after 30 / 60 s.
//
// THE MECHANISM (principle 1: the facet is the engine's KNOWLEDGE of one target). A post-call seat
// on both call sites (write_call<5>, chaining: the engine's update always runs first and whole). When
// the detecting member HOLDS an engagement on that target, after the engine's update:
//   * if the engine flagged kTargetLost, it is un-lost EXACTLY as the engine un-loses it (bit cleared,
//     +0x94 = -FLT_MAX), and when the flag was set in THIS update the last-known time (+0x90) takes
//     the engine's own "now" (+0x94 as the engine just stamped it), so its 10 s timeout restarts;
//   * the last-known location (+0x10 position, +0x20 cell / worldspace) is the target's real one,
//     read exactly as the engine's GetWorldLocation reads it (AE 0x2EA5E0).
// The engine then does what it does with a target it knows: its own pursuit leaves path to it, its
// own detection takes over when it comes into view (the group pass synthesises a detection event
// for a target in range), its own attacks fire. Nothing else is written: not the detection level,
// not the stealth points, not the group's search state, not the selector's choice.
// The write mirrors the engine's own: this same call walked and wrote the same array entry with no
// lock, on the same thread, an instant earlier. Lost is set and cleared inside one call, so a reader
// on another thread can see the bit for that instant: ch.20 therefore no longer ENDS a pin on it.
//
// WHAT ENDS A HOLD. The ch.21 / ch.20 claim ending as before (target dead, disabled, unloaded or
// unresolvable; owner dead; the client's release; an outranking claim; for ch.21 the engine ending
// the fight) -- but NEVER "target lost". And the no-progress watchdog below: a held engagement that
// makes no path progress (no distance gained toward the target, no attack or cast started, not
// within reach) for kNoProgressMs of world time is logged as an ANOMALY and released (both the ch.21
// and the ch.20 claim naming that target). Never held forever on an unreachable target; never a
// silent end (principle 7).
//
// THE BLEED-OUT DRIVE. A held target in bleed-out (or essential-down) stays pinned. Whether the
// engine's own combat AI closes on such a target is NOT proven by disassembly here, so the held
// move-to drives it: while the target is down and the actor is farther than kBleedReachU, the
// actor's combat area carries "within kBleedRadiusU of the target" (a Harbinger-internal ch.24 bound,
// channels/CombatApproach.cpp), so the engine's own Return To Combat Area paths it there. A live
// ch.24 claim on the actor wins over it. Off with [EngagementHold] bBleedoutDrive=0.
//
// THREADING. The seats run wherever the engine runs the detection update (the listener's event
// dispatch, or the group update). They read the hold map under a shared lock (never across the
// engine's call), the published claim snapshot (lock-free) and the references the engine passed
// (refcounted by its caller). The hold map is written on the game thread only (ch.20 / ch.21 Apply,
// Release, ResetAll). Poll is the game thread (Arbiter::OncePerFrame).
// ============================================================================

namespace {

    namespace hold_detail {

        using apmf::log::Hex;

        // ---- INI ([EngagementHold]) ----
        std::atomic<bool> g_enabled{ true };
        std::atomic<bool> g_bleedDrive{ true };
        std::atomic<bool> g_trace{ false };
        std::atomic<bool> g_holdInstallTried{ false };

        // ---- the hold map (game thread writes, any thread reads) ----
        struct Hold {
            RE::FormID      entryTarget = 0;   // ch.21
            std::uint32_t   entryHandle = 0;   // its ActorHandle's native value (what the group entry holds)
            RE::ActorHandle entryH{};
            RE::FormID      pinTarget   = 0;   // ch.20
            std::uint32_t   pinHandle   = 0;
            RE::ActorHandle pinH{};
            // written by the seats / ActionGate under the SHARED lock: atomics
            mutable std::atomic<std::uint32_t> lostCleared{ 0 };   // the engine flagged lost; un-lost
            mutable std::atomic<std::uint32_t> locRefreshed{ 0 };  // last-known location written
            mutable std::atomic<std::uint32_t> notWritten{ 0 };    // 1b: Engine outranks the claim on Detection
            mutable std::atomic<std::uint32_t> leashExempt{ 0 };   // ch.23 let the held approach run
            bool Empty() const { return entryTarget == 0 && pinTarget == 0; }
        };
        std::shared_mutex                                    g_holdMx;
        std::unordered_map<RE::FormID, std::unique_ptr<Hold>> g_holds;
        std::atomic<std::size_t>                             g_holdCount{ 0 };

        // ---- the knowledge seats ----
        using Detect_t = std::uint64_t (*)(void* a_group, RE::TESObjectREFR* a_member, RE::TESObjectREFR* a_target,
                                           void* a_event);
        constexpr int kSites = 2;
        constexpr const char* kSiteName[kSites] = { "EngagementHold.Site.ListenerDetection",
                                                    "EngagementHold.Site.GroupDetectionPass" };
        constexpr std::uint32_t kSiteOff = 0x85;    // listener ProcessEvent: AE 44699 / SE 43477 (both +0x85)
        constexpr std::uint32_t kPassOff = 0x143;   // group detection pass:  AE 44804 / SE 43572 (both +0x143)
        std::atomic<std::uintptr_t> g_origDetect[kSites]{};
        std::atomic<std::uint64_t>  g_siteSeen[kSites]{};
        std::atomic<bool>           g_siteObserved[kSites]{};
        std::atomic<bool>           g_seatsInstalled{ false };
        std::atomic<const char*>    g_seatsWhy{ "before kDataLoaded" };
        std::atomic<std::uint64_t>  g_heldEvents{ 0 }, g_lostCleared{ 0 }, g_locWritten{ 0 }, g_notFound{ 0 },
            g_noSpace{ 0 }, g_stood{ 0 };

        // CombatTarget layout (CommonLib's, re-measured against UpdateTargetDetection on both images).
        static_assert(offsetof(RE::CombatTarget, lastKnownLoc) == 0x10);
        static_assert(offsetof(RE::BGSWorldLocation, cellOrWorldSpace) == 0x10);
        static_assert(offsetof(RE::CombatTarget, lastKnownTimeStamp) == 0x90);
        static_assert(offsetof(RE::CombatTarget, unk94) == 0x94);
        static_assert(offsetof(RE::CombatTarget, flags) == 0xA6);
        static_assert(sizeof(RE::CombatTarget) == 0xA8);
        static_assert(offsetof(RE::CombatGroup, targets) == 0x08);
        constexpr std::uint16_t kFlagKnown = 1u << 0;
        constexpr std::uint16_t kFlagLost  = 1u << 1;

        RE::CombatTarget* FindTarget(void* a_group, std::uint32_t a_handle) {
            auto* g = static_cast<RE::CombatGroup*>(a_group);
            for (auto& t : g->targets)
                if (t.targetHandle.native_handle() == a_handle) return &t;
            return nullptr;
        }

        // GetWorldLocation's space (AE 0x2EA5E0): interior cell, else the cell's worldspace, else the cell.
        RE::TESForm* SpaceForm(RE::TESObjectREFR* a_ref) {
            return const_cast<RE::TESForm*>(
                static_cast<const RE::TESForm*>(apmf::combatapproach::SpaceOfRef(a_ref)));
        }

        // Trace probe ([EngagementHold] bKnowledgeTrace=1, PASSIVE, default 0): one line per member per
        // second with what the engine did and what the seat wrote.
        std::mutex                                    g_traceMx;
        std::unordered_map<RE::FormID, std::uint64_t> g_traceLast;

        template <int I>
        std::uint64_t DetectThunk(void* a_group, RE::TESObjectREFR* a_member, RE::TESObjectREFR* a_target, void* a_event) {
            const auto orig = reinterpret_cast<Detect_t>(g_origDetect[I].load(std::memory_order_relaxed));
            g_siteSeen[I].fetch_add(1, std::memory_order_relaxed);
            if (!g_siteObserved[I].exchange(true, std::memory_order_relaxed))
                spdlog::info("[hold] seat OBSERVED: {} ran (CombatGroup::UpdateTargetDetection; first of the session, "
                             "principle 5).",
                             kSiteName[I]);
            if (g_holdCount.load(std::memory_order_relaxed) == 0 || !a_group || !a_member || !a_target)
                return orig(a_group, a_member, a_target, a_event);

            const RE::FormID mid = a_member->GetFormID();
            const RE::FormID tid = a_target->GetFormID();
            std::uint32_t    handle = 0;
            bool             viaPin = false;
            {
                std::shared_lock lk(g_holdMx);
                const auto it = g_holds.find(mid);
                if (it != g_holds.end()) {
                    const Hold& h = *it->second;
                    if (h.pinTarget == tid && h.pinHandle != 0) { handle = h.pinHandle; viaPin = true; }
                    else if (h.entryTarget == tid && h.entryHandle != 0) handle = h.entryHandle;
                }
            }
            if (handle == 0) return orig(a_group, a_member, a_target, a_event);

            // Before the engine's update: was it already lost? (Same unlocked read the engine makes.)
            const RE::CombatTarget* pre      = FindTarget(a_group, handle);
            const std::uint16_t     preFlags = pre ? pre->flags.underlying() : 0;

            const std::uint64_t r = orig(a_group, a_member, a_target, a_event);   // the engine's update, whole

            g_heldEvents.fetch_add(1, std::memory_order_relaxed);
            RE::CombatTarget* t = FindTarget(a_group, handle);
            if (!t) {
                g_notFound.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
            const std::uint16_t engFlags = t->flags.underlying();
            const float         eng90 = t->lastKnownTimeStamp.timeStamp, eng94 = t->unk94.timeStamp;

            std::shared_lock lk(g_holdMx);
            const auto it = g_holds.find(mid);
            if (it == g_holds.end()) return r;   // released while the engine ran
            const Hold& h = *it->second;
            if (!(viaPin ? (h.pinTarget == tid && h.pinHandle == handle)
                         : (h.entryTarget == tid && h.entryHandle == handle)))
                return r;

            if ((engFlags & kFlagLost) != 0) {
                // HARBINGER 1b: the engine's own detection is the source on the Detection facet; the claim
                // overrides it only when it outranks Engine (defaults: client 50 > Engine).
                APMF_API::APMF_Param p{};
                float                basis = 0.0f;
                APMF_API::Handle     ch    = APMF_API::kInvalidHandle;
                apmf::ControlMap::Get().TryGetOwningClaimBasis(
                    mid, viaPin ? APMF_API::kIntent_TargetPin : APMF_API::kIntent_CombatEntry, p, basis, &ch);
                bool enforce = true;
                try {
                    enforce = apmf::facetattr::Decide(apmf::sources::Facet::kDetection, ch,
                                                      apmf::sources::kEngineSource, mid, "")
                                  .enforce;
                } catch (...) {
                    enforce = true;
                }
                if (!enforce) {
                    h.notWritten.fetch_add(1, std::memory_order_relaxed);
                    g_stood.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                // The engine's own un-lose (AE 0x805483): clear the bit, lost time = -FLT_MAX.
                if ((preFlags & kFlagLost) == 0) t->lastKnownTimeStamp.timeStamp = eng94;   // its "now", just stamped
                t->flags = static_cast<RE::CombatTarget::Flags>(engFlags & ~kFlagLost);
                t->unk94.timeStamp = -(std::numeric_limits<float>::max)();
                h.lostCleared.fetch_add(1, std::memory_order_relaxed);
                g_lostCleared.fetch_add(1, std::memory_order_relaxed);
            }
            if (RE::TESForm* space = SpaceForm(a_target)) {
                t->lastKnownLoc.pos              = a_target->GetPosition();
                t->lastKnownLoc.cellOrWorldSpace = space;
                h.locRefreshed.fetch_add(1, std::memory_order_relaxed);
                g_locWritten.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_noSpace.fetch_add(1, std::memory_order_relaxed);
            }

            if (g_trace.load(std::memory_order_relaxed)) {
                const auto now = apmf::clock::MonotonicMs();
                bool       due = false;
                {
                    std::scoped_lock tl(g_traceMx);
                    auto&            last = g_traceLast[mid];
                    if (now - last >= 1000) { last = now; due = true; }
                }
                if (due)
                    spdlog::info("[hold] trace 0x{} -> 0x{} ({}): flags before 0x{:X}, after the engine 0x{:X} "
                                 "(+0x90 {:.2f}, +0x94 {:.2f}), written 0x{:X} (+0x90 {:.2f}); detect level {}.",
                                 Hex(mid), Hex(tid), I == 0 ? "listener" : "group pass", preFlags, engFlags, eng90,
                                 eng94, t->flags.underlying(), t->lastKnownTimeStamp.timeStamp, t->detectLevel);
            }
            (void)kFlagKnown;
            return r;
        }

        // ---- Poll state (game thread only) ----
        constexpr std::uint64_t kPollMs        = 250;
        constexpr std::uint64_t kTickClampMs   = 500;     // one Poll gap counts at most this much world time
        constexpr std::uint64_t kWindowMs      = 5000;    // the telemetry line per hold
        // NO-PROGRESS BOUND (principle 9, sized from the real cadences): the engine's own last-known
        // timeout is 10 s (AE 0x805462) and its ShouldRemoveTarget gives up a lost target after 30 / 60 s;
        // the movement selector re-picks every 0.25 s and the window above is 5 s. 20 s = two of the
        // engine's own lost timeouts, 80 selector periods, four telemetry windows -- a FLOOR long enough
        // for a path around an obstacle, short enough that a stuck follower is released well before the
        // engine itself would have dropped the fight.
        constexpr std::uint64_t kNoProgressMs  = 20000;
        constexpr float         kProgressStepU = 64.0f;   // closer than the best so far by this much = progress
        constexpr float         kReachU        = 300.0f;  // within this of the target = arrived (no path left)
        constexpr float         kBleedReachU   = 256.0f;  // the bleed-out drive starts farther than this...
        constexpr float         kBleedRadiusU  = 160.0f;  // ...and bounds the actor within this of the target
        constexpr std::uint64_t kDriveStallMs  = 3000;    // ch.24's own stall floor (12 x its 0.25 s period)

        std::uint64_t g_worldMs = 1, g_lastTickMs = 0, g_lastPollMs = 0, g_lastHeartbeatMs = 0;
        std::uint64_t g_anomalies = 0, g_drivesStarted = 0, g_windowsLogged = 0;

        struct Track {
            RE::FormID      target = 0;
            RE::ActorHandle th{};
            bool            viaPin = false;
            bool            ended  = false;   // the watchdog released it; wait for the claims to go
            bool            inCombat = false;
            std::uint64_t   lastProgressMs = 0, windowStartMs = 0, lastActMs = 0;
            RE::NiPoint3    lastPos{};
            const void*     lastSpace = nullptr;
            float           moved = 0.0f, windowStartDist = -1.0f, bestDist = -1.0f, lastDist = -1.0f;
            std::uint32_t   attacks = 0, casts = 0, totalAttacks = 0, totalCasts = 0;
            bool            wasAttacking = false, wasCasting = false;
            std::uint32_t   lostBase = 0, locBase = 0, leashBase = 0;
            bool            drive = false, driveStallLogged = false;
        };
        std::unordered_map<RE::FormID, Track> g_tracks;

        bool IsCasting(RE::Actor* a) {
            for (auto* c : a->GetActorRuntimeData().magicCasters)
                if (c && c->state.get() != RE::MagicCaster::State::kNone) return true;
            return false;
        }

        const char* LifeName(RE::ACTOR_LIFE_STATE s) {
            switch (s) {
            case RE::ACTOR_LIFE_STATE::kBleedout:       return "BLEEDING OUT";
            case RE::ACTOR_LIFE_STATE::kEssentialDown:  return "ESSENTIAL DOWN";
            case RE::ACTOR_LIFE_STATE::kUnconcious:     return "unconscious";
            case RE::ACTOR_LIFE_STATE::kRestrained:     return "restrained";
            default:                                    return "up";
            }
        }

        void StopDrive(RE::FormID a_id, Track& a_t, const char* a_why) {
            if (!a_t.drive) return;
            apmf::combatapproach::InternalInfo info{};
            apmf::combatapproach::GetInternalInfo(a_id, info);
            apmf::combatapproach::ClearInternalBound(a_id);
            a_t.drive = false;
            spdlog::info("[hold] 0x{} bleed-out drive OFF ({}): the actor's combat area carried it {} time(s); the "
                         "engine's own area answers again.",
                         Hex(a_id), a_why, info.applied);
        }

    }

}

namespace apmf::combatentry::hold {

    using namespace hold_detail;

    bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

    void Install() {
        if (g_holdInstallTried.exchange(true)) return;
        g_enabled.store(GetPrivateProfileIntA("EngagementHold", "bEngagementHold", 1, kIni) != 0);
        g_bleedDrive.store(GetPrivateProfileIntA("EngagementHold", "bBleedoutDrive", 1, kIni) != 0);
        g_trace.store(GetPrivateProfileIntA("EngagementHold", "bKnowledgeTrace", 0, kIni) != 0);
        if (!g_enabled.load()) {
            g_seatsWhy.store("[EngagementHold] bEngagementHold=0");
            spdlog::warn("[hold] held engagement OFF ([EngagementHold] bEngagementHold=0): a ch.20 pin ENDS on "
                         "\"target lost\" again (the pre-2026-10-12 contract), no knowledge seat, no leash exemption.");
            return;
        }
        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime";
        } else if (!(REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) || REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97))) {
            // 1.7.104: the three ids are absent from the MIT id table (VerifiedAddresses: NOT verified);
            // resolving them would be a fatal id miss, so nothing is computed there.
            why = "the runtime is not exactly 1.6.1170 or 1.5.97 (1.7.104: the seat ids are not in the MIT id table)";
        } else if (GetPrivateProfileIntA("EngagementHold", "bTargetKnowledge", 1, kIni) == 0) {
            why = "[EngagementHold] bTargetKnowledge=0";
        }
        std::uintptr_t fn = 0, sites[kSites]{};
        if (!why) {
            fn       = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43501, 44725) }.address();
            sites[0] = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43477, 44699) }.address() + kSiteOff;
            sites[1] = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43572, 44804) }.address() + kPassOff;
            bool ok = apmf::allowance::SeatVerified(fn, "EngagementHold.CombatGroup.UpdateTargetDetection");
            ok      = apmf::allowance::SeatVerified(sites[0], kSiteName[0]) && ok;
            ok      = apmf::allowance::SeatVerified(sites[1], kSiteName[1]) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            // The bytes NOW (another DLL may have patched a site since the self-check): E8 -> the function.
            for (int i = 0; i < kSites && !why; ++i) {
                const auto* b = reinterpret_cast<const std::uint8_t*>(sites[i]);
                std::int32_t rel = 0;
                std::memcpy(&rel, b + 1, sizeof(rel));
                if (b[0] != 0xE8 || sites[i] + 5 + static_cast<std::intptr_t>(rel) != fn)
                    why = "a call site no longer calls CombatGroup::UpdateTargetDetection (another patcher got there first)";
            }
        }
        if (why) {
            g_seatsWhy.store(why);
            spdlog::warn("[hold] knowledge seats NOT installed -- {}. Held engagements still never END on \"target "
                         "lost\" and the leash still exempts their approach, but the engine's own detection can still "
                         "lose the target and search for it.",
                         why);
            return;
        }
        // Our own trampoline (never freed: the engine may run a detection update during shutdown), from
        // SKSE's branch pool when it has room, else a page near the module. EquipSink's stays its own.
        static auto* s_tr = new SKSE::Trampoline("APMF.EngagementHold");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(64);
        if (mem) s_tr->set_trampoline(mem, 64);
        else s_tr->create(64);
        const std::uintptr_t prev0 = s_tr->write_call<5>(sites[0], &DetectThunk<0>);
        g_origDetect[0].store(prev0, std::memory_order_release);
        const std::uintptr_t prev1 = s_tr->write_call<5>(sites[1], &DetectThunk<1>);
        g_origDetect[1].store(prev1, std::memory_order_release);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kDetection, kSiteName[0], sites[0], prev0);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kDetection, kSiteName[1], sites[1], prev1);
        g_seatsWhy.store("");
        g_seatsInstalled.store(true, std::memory_order_release);
        spdlog::info("[hold] knowledge seats installed: CombatGroup::UpdateTargetDetection (RVA 0x{:X}) post-call at "
                     "the detection listener (+0x{:X}) and the group detection pass (+0x{:X}). A held target the "
                     "engine flags LOST is un-lost the engine's own way and its last-known location is its real one. "
                     "bleed-out drive {}, trace {}.",
                     fn - REL::Module::get().base(), kSiteOff, kPassOff, g_bleedDrive.load() ? "ON" : "OFF",
                     g_trace.load() ? "ON" : "OFF");
    }

    namespace {
        void SetSlot(RE::FormID a_actor, bool a_pin, RE::FormID a_target, RE::ActorHandle a_handle) {
            std::unique_lock lk(g_holdMx);
            auto& slot = g_holds[a_actor];
            if (!slot) slot = std::make_unique<Hold>();
            const std::uint32_t n = a_target != 0 ? a_handle.native_handle() : 0;
            if (a_pin) { slot->pinTarget = a_target; slot->pinHandle = n; slot->pinH = a_handle; }
            else { slot->entryTarget = a_target; slot->entryHandle = n; slot->entryH = a_handle; }
            if (slot->Empty()) g_holds.erase(a_actor);
            g_holdCount.store(g_holds.size(), std::memory_order_relaxed);
        }
    }

    void SetPin(RE::FormID a, RE::FormID t, RE::ActorHandle h) { SetSlot(a, true, t, h); }
    void ClearPin(RE::FormID a) { SetSlot(a, true, 0, RE::ActorHandle{}); }
    void SetEntry(RE::FormID a, RE::FormID t, RE::ActorHandle h) { SetSlot(a, false, t, h); }
    void ClearEntry(RE::FormID a) { SetSlot(a, false, 0, RE::ActorHandle{}); }

    bool IsHeld(RE::FormID a_actor, std::uint32_t a_handle, bool a_countLeash) {
        if (a_handle == 0 || !g_enabled.load(std::memory_order_relaxed) ||
            g_holdCount.load(std::memory_order_relaxed) == 0)
            return false;
        std::shared_lock lk(g_holdMx);
        const auto it = g_holds.find(a_actor);
        if (it == g_holds.end()) return false;
        const Hold& h = *it->second;
        if (h.pinHandle != a_handle && h.entryHandle != a_handle) return false;
        if (a_countLeash) h.leashExempt.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool HoldsAny(RE::FormID a_actor) {
        if (!g_enabled.load(std::memory_order_relaxed) || g_holdCount.load(std::memory_order_relaxed) == 0) return false;
        std::shared_lock lk(g_holdMx);
        return g_holds.contains(a_actor);
    }

    void Poll() {
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_lastTickMs != 0) g_worldMs += (std::min)(now - g_lastTickMs, kTickClampMs);
        g_lastTickMs = now;
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        if (g_holdCount.load(std::memory_order_relaxed) == 0 && g_tracks.empty()) return;
        if (now - g_lastPollMs < kPollMs) return;
        g_lastPollMs = now;
        const std::uint64_t wnow = g_worldMs;

        // Snapshot the holds (effective target: the pin's when there is one -- the pin decides whom the
        // actor fights -- else the entry's).
        struct Snap { RE::FormID id, target; RE::ActorHandle handle; bool viaPin; std::uint32_t lost, loc, leash; };
        std::vector<Snap> snaps;
        {
            std::shared_lock lk(g_holdMx);
            snaps.reserve(g_holds.size());
            for (const auto& [id, h] : g_holds) {
                const bool pin = h->pinTarget != 0 && h->pinHandle != 0;
                const RE::FormID    tf = pin ? h->pinTarget : h->entryTarget;
                const std::uint32_t th = pin ? h->pinHandle : h->entryHandle;
                if (tf == 0 || th == 0) continue;
                const RE::ActorHandle ah = pin ? h->pinH : h->entryH;
                snaps.push_back({ id, tf, ah, pin, h->lostCleared.load(), h->locRefreshed.load(), h->leashExempt.load() });
            }
        }
        // Tracks of holds that are gone.
        for (auto it = g_tracks.begin(); it != g_tracks.end();) {
            const bool live = std::any_of(snaps.begin(), snaps.end(),
                                          [&](const Snap& s) { return s.id == it->first && s.target == it->second.target; });
            if (live) { ++it; continue; }
            StopDrive(it->first, it->second, "the hold ended");
            it = g_tracks.erase(it);
        }

        for (const auto& sn : snaps) {
            auto& t = g_tracks[sn.id];
            if (t.target != sn.target) {   // a new hold (or a re-point): fresh track
                StopDrive(sn.id, t, "re-pointed");
                t = Track{};
                t.target = sn.target;
                t.th     = sn.handle;
                t.viaPin = sn.viaPin;
                t.lastProgressMs = t.windowStartMs = wnow;
                t.lostBase = sn.lost; t.locBase = sn.loc; t.leashBase = sn.leash;
                spdlog::info("[hold] 0x{} HELD engagement on 0x{} (via {}): it cannot be lost to the engine's "
                             "detection; it ends when the target is dead, disabled or unloaded, the client releases "
                             "it, a higher-priority claim takes it, or {} s pass with no path progress.",
                             Hex(sn.id), Hex(sn.target), sn.viaPin ? "ch.20 pin" : "ch.21 entry", kNoProgressMs / 1000);
            }
            if (t.ended) continue;

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            auto  tptr  = t.th.get();
            auto* target = tptr.get();
            if (!actor || !target || actor->IsDead() || target->IsDead()) continue;   // ch.20 / ch.21 Poll end it

            // In a fight? (The POINTER only, APMF-B26.) Out of combat nothing is expected to move.
            const bool inCombat = actor->GetActorRuntimeData().combatController != nullptr;
            if (!inCombat) {
                if (t.inCombat) StopDrive(sn.id, t, "out of combat");
                t.inCombat = false;
                t.lastProgressMs = wnow;
                continue;
            }
            if (!t.inCombat) { t.inCombat = true; t.lastProgressMs = wnow; }

            const RE::NiPoint3 pos   = actor->GetPosition();
            const void*        space = apmf::combatapproach::SpaceOfRef(actor);
            if (t.lastSpace && space == t.lastSpace) t.moved += pos.GetDistance(t.lastPos);
            t.lastPos = pos; t.lastSpace = space;
            const void* tspace = apmf::combatapproach::SpaceOfRef(target);
            const bool  same   = space && space == tspace;
            const float dist   = same ? pos.GetDistance(target->GetPosition()) : -1.0f;
            t.lastDist = dist;
            if (t.windowStartDist < 0.0f) t.windowStartDist = dist;

            // Attack / cast starts (sampled every 250 ms: a swing or a cast lasts longer than that).
            const bool attacking = actor->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
            const bool casting   = IsCasting(actor);
            if (attacking && !t.wasAttacking) { ++t.attacks; ++t.totalAttacks; t.lastActMs = wnow; }
            if (casting && !t.wasCasting) { ++t.casts; ++t.totalCasts; t.lastActMs = wnow; }
            t.wasAttacking = attacking; t.wasCasting = casting;

            // PROGRESS: closer than ever by a step, an attack / cast started, or within reach.
            if (dist >= 0.0f && (t.bestDist < 0.0f || dist < t.bestDist - kProgressStepU)) {
                t.bestDist = dist;
                t.lastProgressMs = wnow;
            }
            if (t.lastActMs == wnow || (dist >= 0.0f && dist <= kReachU)) t.lastProgressMs = wnow;

            // THE BLEED-OUT DRIVE (see the section header).
            const auto life     = target->AsActorState()->GetLifeState();
            const bool downed   = life == RE::ACTOR_LIFE_STATE::kBleedout || life == RE::ACTOR_LIFE_STATE::kEssentialDown;
            if (g_bleedDrive.load(std::memory_order_relaxed) && downed && same) {
                if (!t.drive && dist > kBleedReachU) {
                    APMF_API::APMF_Param p{};
                    float                basis = 0.0f;
                    APMF_API::Handle     ch    = APMF_API::kInvalidHandle;
                    apmf::ControlMap::Get().TryGetOwningClaimBasis(
                        sn.id, sn.viaPin ? APMF_API::kIntent_TargetPin : APMF_API::kIntent_CombatEntry, p, basis, &ch);
                    if (apmf::combatapproach::SetInternalBound(sn.id, target->GetPosition(), tspace, kBleedRadiusU, ch,
                                                               "held engagement (bleed-out drive)")) {
                        t.drive = true;
                        t.driveStallLogged = false;
                        ++g_drivesStarted;
                        spdlog::info("[hold] 0x{} bleed-out drive ON: held target 0x{} is {} at {:.0f}u -- the "
                                     "actor's combat area is centred on it (R {:.0f}) so the engine's own Return To "
                                     "Combat Area paths it there.",
                                     Hex(sn.id), Hex(sn.target), LifeName(life), dist, kBleedRadiusU);
                    }
                } else if (t.drive) {
                    // Keep X on the target (it may be carried, pushed or slide).
                    APMF_API::APMF_Param p{};
                    float                basis = 0.0f;
                    APMF_API::Handle     ch    = APMF_API::kInvalidHandle;
                    apmf::ControlMap::Get().TryGetOwningClaimBasis(
                        sn.id, sn.viaPin ? APMF_API::kIntent_TargetPin : APMF_API::kIntent_CombatEntry, p, basis, &ch);
                    apmf::combatapproach::SetInternalBound(sn.id, target->GetPosition(), tspace, kBleedRadiusU, ch,
                                                           "held engagement (bleed-out drive)");
                    apmf::combatapproach::InternalInfo info{};
                    if (apmf::combatapproach::GetInternalInfo(sn.id, info) && !info.clientWins &&
                        info.msSinceApply != ~0ull && info.msSinceApply > kDriveStallMs && dist > kBleedRadiusU &&
                        !t.driveStallLogged) {
                        t.driveStallLogged = true;
                        spdlog::warn("[hold] 0x{} bleed-out drive NOT CARRIED for {} ms: the actor's standard combat "
                                     "area is not updating with it (another area is current, it is disabled, or the "
                                     "engine's own path to the target failed). Distance {:.0f}u. Left on; the "
                                     "no-progress watchdog decides.",
                                     Hex(sn.id), info.msSinceApply, dist);
                    }
                }
            } else if (t.drive) {
                StopDrive(sn.id, t, downed ? "the target changed cell" : "the target is up again");
            }

            // The telemetry line, once per window.
            if (wnow - t.windowStartMs >= kWindowMs) {
                ++g_windowsLogged;
                spdlog::info("[hold] 0x{} -> 0x{}: moved {:.0f}u in {:.1f} s, distance {:.0f} -> {:.0f} (best {:.0f}), "
                             "attacks {}, casts {}; the engine lost it {} time(s) (knowledge kept, location written {}), "
                             "leash let the approach run {}; target {}; drive {}; no progress for {:.1f} s.",
                             Hex(sn.id), Hex(sn.target), t.moved, (wnow - t.windowStartMs) / 1000.0, t.windowStartDist,
                             dist, t.bestDist, t.attacks, t.casts, sn.lost - t.lostBase, sn.loc - t.locBase,
                             sn.leash - t.leashBase, LifeName(life), t.drive ? "ON" : "off",
                             (wnow - t.lastProgressMs) / 1000.0);
                t.windowStartMs   = wnow;
                t.moved           = 0.0f;
                t.windowStartDist = dist;
                t.attacks = t.casts = 0;
                t.lostBase = sn.lost; t.locBase = sn.loc; t.leashBase = sn.leash;
            }

            // THE NO-PROGRESS WATCHDOG (principle 7: loud, then released -- never held forever).
            if (wnow - t.lastProgressMs >= kNoProgressMs) {
                ++g_anomalies;
                t.ended = true;
                StopDrive(sn.id, t, "no path progress");
                spdlog::warn("[hold] ANOMALY 0x{} -> 0x{}: NO PATH PROGRESS for {} s while in combat (distance {:.0f}, "
                             "best {:.0f}, {}; no attack or cast started; target {}). The held engagement is RELEASED "
                             "(the ch.21 and ch.20 claims naming this target); the client may engage again.",
                             Hex(sn.id), Hex(sn.target), kNoProgressMs / 1000, dist, t.bestDist,
                             same ? "same cell / worldspace" : "the target is in another cell or worldspace",
                             LifeName(life));
                EndClaim(sn.id, sn.target, "held engagement: no path progress (anomaly)");
                apmf::targetpin::EndHeld(sn.id, sn.target, "held engagement: no path progress (anomaly)");
            }
        }
    }

    void Heartbeat() {
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        if (g_holdCount.load(std::memory_order_relaxed) == 0 && g_tracks.empty()) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (now - g_lastHeartbeatMs < 30000) return;
        g_lastHeartbeatMs = now;
        spdlog::info("[hold] heartbeat: {} hold(s); knowledge seats {} (listener calls {}, group-pass calls {}); held "
                     "detection updates {}: engine-lost un-lost {}, last-known location written {}, entry not found {}, "
                     "no space {}, not written by priority {}; telemetry windows {}; bleed-out drives started {}; "
                     "no-progress anomalies {}.",
                     g_holdCount.load(), g_seatsInstalled.load() ? "installed" : g_seatsWhy.load(), g_siteSeen[0].load(),
                     g_siteSeen[1].load(), g_heldEvents.load(), g_lostCleared.load(), g_locWritten.load(),
                     g_notFound.load(), g_noSpace.load(), g_stood.load(), g_windowsLogged, g_drivesStarted, g_anomalies);
    }

    void ResetAll(const char* why) {
        std::size_t n = g_tracks.size();
        for (auto& [id, t] : g_tracks)
            if (t.drive) apmf::combatapproach::ClearInternalBound(id);   // ch.24's own ResetAll drops it too
        g_tracks.clear();
        {
            std::scoped_lock tl(g_traceMx);
            g_traceLast.clear();
        }
        if (n != 0) spdlog::info("[hold] {} -- dropped {} held-engagement track(s).", why, n);
    }

}

APMF_REGISTER_CHANNEL(CombatEntryChannel);
