#include "PCH.h"
#include "channels/CombatEntry.h"
#include "channels/CombatSubstrate.h"   // S2a: the anchor's knowledge + real-combat trigger (a) ride the detection seat
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
#include "core/ActionGate.h"   // the held engagement's search drive reads ch.23's search stamps

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>
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
        float                nowBasis = 0.0f;
        APMF_API::Handle     entryH   = APMF_API::kInvalidHandle;   // menu "Served" credit
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_CombatEntry, now, nowBasis, &entryH) ||
            now.form != tf) {
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

        apmf::sources::NoteServedClaim(entryH, apmf::sources::Facet::kCombat);   // ch.21: the declared entry happened
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
        else apmf::combatentry::hold::SetEntry(id, tf, h, param.fval);

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
// path to it then theres the issue."; and, same day: "If the target is actually outside the engage on
// sight leash it should just be dropped unless it's hostile, canceling combat status on the
// follower."). INVARIANTS #0 (i).
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
// It runs per (member, target) detection event, but the CombatTarget entry it writes belongs to the
// GROUP: any member's event can set the flag. FOUR paths set it: the member's detection level is
// positive but under the engine's threshold (AE 0x8053EA / SE 0x76B886 -- it still refreshes the
// last-known location to the target's real position); the group's last-known time (+0x90) is more
// than 10 s old (AE 0x805462 / SE 0x76B8FF); the level is negative (AE 0x8054F2 / SE 0x76B997); the
// member stands near the last-known location and its own test of that spot passes without the target
// (AE 0x805584 / SE 0x76BA25). Each path stamps +0x94 with the AI clock's "now". Its own UN-lose
// (AE 0x805483) clears the bit and stores -FLT_MAX in +0x94. Two callers, both plain E8 calls:
// CombatGroupDetectionListener::ProcessEvent (its vtable slot 2, AE id 44699 +0x85 / SE id 43477
// +0x85) and the group's periodic member x target detection pass (AE id 44804 +0x143 / SE id 43572
// +0x143). Downstream, the lost bit is what makes the movement selector take SEARCH
// (channels/CombatApproach.cpp header: Search sits above Close Movement) and what
// CombatGroup::ShouldRemoveTarget (AE 0x80C680) times out after 30 / 60 s.
//
// THE MECHANISM (principle 1: the facet is the GROUP's knowledge of one target). A post-call seat on
// both call sites (write_call<5>, chaining: the engine's update always runs first and whole). After
// ANY member's update of a target that a MEMBER OF THAT GROUP holds an engagement on (review F-1: the
// entry is group-wide, so a non-holder's event -- the player's, another follower's -- re-flags it just
// as well):
//   * if the engine flagged kTargetLost (in this update or before), it is un-lost EXACTLY as the engine
//     un-loses it (bit cleared, +0x94 = -FLT_MAX) and the last-known time takes max(+0x90, +0x94): the
//     engine's own "now" when a lost path just stamped it, so its 10 s timeout restarts;
//   * the last-known location (+0x10 position, +0x20 cell / worldspace) is the target's real one,
//     read exactly as the engine's GetWorldLocation reads it (AE 0x2EA5E0).
// The engine then does what it does with a target it knows: its own pursuit leaves path to it, its
// own detection takes over when it comes into view, its own attacks fire. Nothing else is written:
// not the detection level, the stealth points, the group's search state or the selector's choice.
// Lost is set and cleared inside one call, so a reader on another thread can see the bit for that
// instant: with the seats installed ch.20 therefore no longer ENDS a pin on it.
//
// WHAT ENDS A HOLD. The ch.21 / ch.20 claim ending as before (target dead, disabled, unloaded or
// unresolvable; owner dead; the client's release; an outranking claim; for ch.21 the engine ending
// the fight; for ch.20 the target leaving the group's targets after it was in them) -- but NEVER
// "target lost" while the knowledge seats are installed. And three ends of its own, each logged EVERY
// time with its count for that actor -> target ("End #N", the heartbeat's "repeat ends"), and NO
// cooldown (marth 2026-10-12: "THe engage on sight is meant to be an aggressive setting. No waiting ever,
// engaged, or not engaged."): the client may engage the same target again at once.
//   * NO PATH PROGRESS (outside reach): over the last kNoProgressMs of world time the actor got no
//     64u closer than it was at the window's start, did not keep pace with a moving target (the
//     target moved >= 128u, the actor >= 64u, the gap grew by < 64u), and started no attack or cast
//     on the held target;
//   * ARRIVED BUT NOT FIGHTING (within kReachU): kArrivedIdleMs without an attack or cast on the
//     held target -- a close but unreachable target, or a downed foe the engine will not strike;
//   * NOT ENGAGED (ch.21 with param.fval > 0, the client's engage-on-sight range; marth 2026-10-12: "And
//     if the target is deemed \"not engaged\" combat is dropped immediately and previous behaviour
//     reinstated."): the first poll that finds the target farther than that from the actor (or in
//     another cell) and NOT actively hostile (its own combat target is in the party: the actor, the
//     player, a player teammate, an actor any of those commands, a member of the actor's combat group)
//     drops it IN THAT PASS -- no debounce: the actor's combat is ended through the engine's own
//     Actor::StopCombat (Character vtable slot 0xE5, verified on both images), ranked (not while ANOTHER
//     group target is attacking the party, review R-1; not when a winning ch.20 pin names a different
//     target; not when another client's ch.24 claim or Engine outranks the entry on Combat), and every
//     Harbinger-internal effect of the engagement is removed at once (DropNotEngaged).
//
// THE DRIVE. Two cases where the held move-to walks the actor through its own combat area (a
// Harbinger-internal ch.24 bound, channels/CombatApproach.cpp; a live ch.24 claim on the actor wins):
//   * BLEED-OUT: the held target is bleeding out / essential-down and the actor is farther than
//     kBleedReachU -> "within kBleedRadiusU of the target" (the arrived-but-not-fighting end bounds it);
//   * SEARCH ELSEWHERE (review F-6): the actor's group searches for some OTHER target and ch.23 just
//     leash-denied that search -> "within kReachU of the held target", until no search for 3 s.
// A drive the engine does not carry for kDriveStallMs is RELEASED and logged as a warning (never left
// on silently), and not retried for kDriveRetryMs. Neither starts when the actor's winning ch.23 leash
// outranks the engagement (review F-3).
//
// THREADING (review F-8, measured). The LISTENER site runs inside the "Non render-safe AI" job (the
// detection dispatcher AE 0x7AF410 -> 0x7AF830, job 0x6D3210) with NO group lock -- the engine's own
// writes there are equally lockless -- and synchronously from CombatGroup::AddTarget (AE 0x803A10)
// under the group's WRITE lock. The GROUP PASS site runs inside CombatGroup::Update (AE 0x803710, the
// "Combat manager" job) under the group's WRITE lock (+0x160, taken at 0x803740), and only for members
// that are NOT high process (0x69C6D0 = process+0x137 == 0 skips) -- so a follower near the player is
// updated only through the listener. The seats take no group lock themselves (an inversion against
// the detection system's own locks would be a deadlock) and do exactly the unlocked walk + write the
// engine's own function just did. They read the hold map under a shared lock (never across the
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
            std::uint32_t   holderHandle = 0;   // the holding actor's ActorHandle (group membership test)
            RE::FormID      entryTarget  = 0;   // ch.21
            std::uint32_t   entryHandle  = 0;   // its ActorHandle's native value (what the group entry holds)
            RE::ActorHandle entryH{};
            float           entryRange   = 0.0f;   // ch.21 param.fval: the client's engage range (0 = none)
            RE::FormID      pinTarget    = 0;   // ch.20
            std::uint32_t   pinHandle    = 0;
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

        // NO RE-ENGAGE COOLDOWN (marth 2026-10-12: "THe engage on sight is meant to be an aggressive setting. No
        // waiting ever, engaged, or not engaged."): an engagement Harbinger ends is re-engaged the moment the
        // client asks again. Each end is logged every time and counted per actor -> target (g_ends) so a
        // release -> re-engage cycle shows plainly in the field log.
        std::unordered_map<std::uint64_t, std::uint32_t> g_ends;   // game thread only
        std::uint32_t CountEnd(RE::FormID a, RE::FormID t) { return ++g_ends[(static_cast<std::uint64_t>(a) << 32) | t]; }
        std::uint64_t g_cycles = 0;   // ends of an engagement that had already been ended before (a repeat)

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
        std::atomic<std::uint64_t>  g_heldEvents{ 0 }, g_byOther{ 0 }, g_lostCleared{ 0 }, g_locWritten{ 0 },
            g_notFound{ 0 }, g_noSpace{ 0 }, g_stood{ 0 };

        // Layouts (CommonLib's, re-measured against UpdateTargetDetection on both images).
        static_assert(offsetof(RE::CombatTarget, lastKnownLoc) == 0x10);
        static_assert(offsetof(RE::BGSWorldLocation, cellOrWorldSpace) == 0x10);
        static_assert(offsetof(RE::CombatTarget, lastKnownTimeStamp) == 0x90);
        static_assert(offsetof(RE::CombatTarget, unk94) == 0x94);
        static_assert(offsetof(RE::CombatTarget, flags) == 0xA6);
        static_assert(sizeof(RE::CombatTarget) == 0xA8);
        static_assert(offsetof(RE::CombatGroup, targets) == 0x08);
        static_assert(offsetof(RE::CombatGroup, members) == 0x20);   // CombatMember stride 0xC, handle at +0
        static_assert(sizeof(RE::CombatMember) == 0x0C);
        constexpr std::uint16_t kFlagLost = 1u << 1;

        RE::CombatTarget* FindTarget(void* a_group, std::uint32_t a_handle) {
            auto* g = static_cast<RE::CombatGroup*>(a_group);
            for (auto& t : g->targets)
                if (t.targetHandle.native_handle() == a_handle) return &t;
            return nullptr;
        }

        bool GroupHasMember(void* a_group, std::uint32_t a_handle) {
            if (a_handle == 0) return false;
            auto* g = static_cast<RE::CombatGroup*>(a_group);
            for (const auto& m : g->members)
                if (m.memberHandle.native_handle() == a_handle) return true;
            return false;
        }

        // GetWorldLocation's space (AE 0x2EA5E0): interior cell, else the cell's worldspace, else the cell.
        RE::TESForm* SpaceForm(RE::TESObjectREFR* a_ref) {
            return const_cast<RE::TESForm*>(
                static_cast<const RE::TESForm*>(apmf::combatapproach::SpaceOfRef(a_ref)));
        }

        // Trace probe ([EngagementHold] bKnowledgeTrace=1, PASSIVE, default 0): one line per target per
        // second with what the engine did and what the seat wrote.
        std::mutex                                    g_traceMx;
        std::unordered_map<RE::FormID, std::uint64_t> g_traceLast;

        struct Cand {
            RE::FormID    holder = 0;
            std::uint32_t holderH = 0, targetH = 0;
            bool          viaPin = false;
        };

        template <int I>
        std::uint64_t DetectThunk(void* a_group, RE::TESObjectREFR* a_member, RE::TESObjectREFR* a_target, void* a_event) {
            const auto orig = reinterpret_cast<Detect_t>(g_origDetect[I].load(std::memory_order_acquire));
            g_siteSeen[I].fetch_add(1, std::memory_order_relaxed);
            if (!g_siteObserved[I].exchange(true, std::memory_order_relaxed))
                spdlog::info("[hold] seat OBSERVED: {} ran (CombatGroup::UpdateTargetDetection; first of the session, "
                             "principle 5).",
                             kSiteName[I]);
            // THE COMBAT SUBSTRATE (S2a, channels/CombatSubstrate.cpp): the anchor's entry in a holder's group is
            // kept known after the engine's update (the same un-lose write, for the anchor only); any OTHER target
            // in a holder's group is the real-combat trigger (a) -- seen here synchronously from AddTarget.
            if (apmf::combatsubstrate::AnyLive() && a_group && a_member && a_target) {
                if (apmf::combatsubstrate::IsAnchorRef(a_target)) {
                    const std::uint64_t r = orig(a_group, a_member, a_target, a_event);
                    apmf::combatsubstrate::AnchorKnowledge(a_group, a_target);
                    return r;
                }
                apmf::combatsubstrate::NoteGroupTarget(a_group, a_target);
            }
            if (g_holdCount.load(std::memory_order_relaxed) == 0 || !a_group || !a_member || !a_target)
                return orig(a_group, a_member, a_target, a_event);

            // Does any hold name this target? (review F-1: the entry is the GROUP's, so the event of
            // ANY member counts, as long as a holder is a member of this group).
            const RE::FormID tid   = a_target->GetFormID();
            const auto       names = [tid](const Hold& h) {
                return (h.pinTarget == tid && h.pinHandle != 0) || (h.entryTarget == tid && h.entryHandle != 0);
            };
            bool named = false;
            {
                std::shared_lock lk(g_holdMx);
                for (const auto& kv : g_holds)
                    if (names(*kv.second)) { named = true; break; }
            }
            if (!named) return orig(a_group, a_member, a_target, a_event);

            const std::uint64_t r = orig(a_group, a_member, a_target, a_event);   // the engine's update, whole

            // The first hold naming this target whose HOLDER is a member of this group. Every hold is walked,
            // with no cap (taunt audit 2026-10-12: a taunter pinned by foes of two enemy groups is held by more
            // than four actors across two groups; the old first-four sample could hold no member of this group
            // and leave it to lose the target). The member array is read unlocked as before, after the engine's
            // call; g_holdMx is shared and held across no engine call.
            Cand cand{};
            bool inGroup = false;
            {
                std::shared_lock lk(g_holdMx);
                for (const auto& [id, h] : g_holds) {
                    if (!names(*h) || !GroupHasMember(a_group, h->holderHandle)) continue;
                    const bool viaPin = h->pinTarget == tid && h->pinHandle != 0;
                    cand    = { id, h->holderHandle, viaPin ? h->pinHandle : h->entryHandle, viaPin };
                    inGroup = true;
                    break;
                }
            }
            if (!inGroup) return r;   // no holder in this group (e.g. the target's own group)
            const Cand* c = &cand;
            g_heldEvents.fetch_add(1, std::memory_order_relaxed);
            const RE::FormID mid = a_member->GetFormID();
            if (mid != c->holder) g_byOther.fetch_add(1, std::memory_order_relaxed);

            RE::CombatTarget* t = FindTarget(a_group, c->targetH);
            if (!t) {
                g_notFound.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
            const std::uint16_t engFlags = t->flags.underlying();
            const float         eng90 = t->lastKnownTimeStamp.timeStamp, eng94 = t->unk94.timeStamp;

            std::shared_lock lk(g_holdMx);
            const auto it = g_holds.find(c->holder);
            if (it == g_holds.end()) return r;   // released while the engine ran
            const Hold& h = *it->second;
            if (!(c->viaPin ? (h.pinTarget == tid && h.pinHandle == c->targetH)
                            : (h.entryTarget == tid && h.entryHandle == c->targetH)))
                return r;

            if ((engFlags & kFlagLost) != 0) {
                // HARBINGER 1b: the engine's own detection is the source on the Detection facet; the claim
                // overrides it only when it outranks Engine (defaults: client 50 > Engine).
                APMF_API::APMF_Param p{};
                float                basis = 0.0f;
                APMF_API::Handle     ch    = APMF_API::kInvalidHandle;
                apmf::ControlMap::Get().TryGetOwningClaimBasis(
                    c->holder, c->viaPin ? APMF_API::kIntent_TargetPin : APMF_API::kIntent_CombatEntry, p, basis, &ch);
                bool enforce = true;
                try {
                    enforce = apmf::facetattr::Decide(apmf::sources::Facet::kDetection, ch,
                                                      apmf::sources::kEngineSource, c->holder, "")
                                  .enforce;
                } catch (...) {
                    enforce = true;
                }
                if (!enforce) {
                    h.notWritten.fetch_add(1, std::memory_order_relaxed);
                    g_stood.fetch_add(1, std::memory_order_relaxed);
                    return r;
                }
                // The engine's own un-lose (AE 0x805483): clear the bit, lost time = -FLT_MAX. The last-known
                // time takes the later of the two stamps: a lost path stamps +0x94 with "now" (review F-1).
                t->lastKnownTimeStamp.timeStamp = (std::max)(eng90, eng94);
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
                    auto&            last = g_traceLast[tid];
                    if (now - last >= 1000) { last = now; due = true; }
                }
                if (due)
                    spdlog::info("[hold] trace 0x{} (held by 0x{}) event of 0x{} ({}): flags after the engine 0x{:X} "
                                 "(+0x90 {:.2f}, +0x94 {:.2f}), written 0x{:X} (+0x90 {:.2f}); detect level {}.",
                                 Hex(tid), Hex(c->holder), Hex(mid), I == 0 ? "listener" : "group pass", engFlags,
                                 eng90, eng94, t->flags.underlying(), t->lastKnownTimeStamp.timeStamp, t->detectLevel);
            }
            return r;
        }

        // ---- Poll state (game thread only) ----
        constexpr std::uint64_t kPollMs        = 250;
        constexpr std::uint64_t kTickClampMs   = 500;     // one Poll gap counts at most this much world time
        constexpr std::uint64_t kWindowMs      = 5000;    // the telemetry line per hold
        // NO-PROGRESS WINDOW (principle 9, sized from the real cadences): the engine's own last-known
        // timeout is 10 s (AE 0x805462) and its ShouldRemoveTarget gives up a lost target after 30 / 60 s;
        // the movement selector re-picks every 0.25 s and the telemetry window is 5 s. 20 s = two of the
        // engine's own lost timeouts, 80 selector periods, four telemetry windows -- long enough for a
        // path around an obstacle, short enough that a stuck follower is released well before the engine
        // itself would have dropped the fight. Progress is WINDOWED (review F-2): compared with the sample
        // kNoProgressMs ago, not with the best distance ever.
        constexpr std::uint64_t kNoProgressMs  = 20000;
        constexpr std::uint64_t kArrivedIdleMs = 20000;   // within reach, no attack / cast on the held target
        constexpr float         kProgressStepU = 64.0f;   // closer than the window start by this much = progress
        constexpr float         kPaceTargetU   = 128.0f;  // "kept pace": the target moved this much ...
        constexpr float         kPaceActorU    = 64.0f;   // ... the actor this much, and the gap grew < a step
        constexpr float         kReachU        = 300.0f;  // within this of the target = arrived (no path left)
        constexpr float         kBleedReachU   = 256.0f;  // the bleed-out drive starts farther than this...
        constexpr float         kBleedRadiusU  = 160.0f;  // ...and bounds the actor within this of the target
        constexpr std::uint64_t kDriveStallMs  = 3000;    // ch.24's own stall floor (12 x its 0.25 s period)
        constexpr std::uint64_t kDriveRetryMs  = 10000;   // a released (stalled) drive is not retried before this
        constexpr std::uint64_t kSearchStartMs = 1500;    // a search leash-denied this recently starts the search drive
        constexpr std::uint64_t kSearchIdleMs  = 3000;    // no search leaf for this long ends it

        std::uint64_t g_worldMs = 1, g_lastTickMs = 0, g_lastPollMs = 0, g_lastHeartbeatMs = 0;
        std::uint64_t g_anomalies = 0, g_arrivedIdle = 0, g_rangeDrops = 0, g_stopCombats = 0, g_drivesStarted = 0,
                      g_drivesStalled = 0, g_windowsLogged = 0;

        enum class Drive : std::uint8_t { kNone, kBleedout, kSearch };
        const char* DriveName(Drive d) {
            return d == Drive::kBleedout ? "bleed-out" : d == Drive::kSearch ? "search-elsewhere" : "off";
        }

        struct Sample {
            std::uint64_t ms   = 0;
            float         dist = -1.0f;   // -1: the target is in another cell / worldspace
            RE::NiPoint3  tpos{};
            float         path = 0.0f;    // the actor's cumulative path length at this sample
        };

        struct Track {
            RE::FormID      target = 0;
            RE::ActorHandle th{};
            bool            viaPin = false;
            bool            ended  = false;   // a watchdog / range end released it; wait for the claims to go
            bool            inCombat = false;
            std::deque<Sample> samples;
            float           path = 0.0f;
            std::uint64_t   windowStartMs = 0, lastHeldActMs = 0, arrivedSinceMs = 0;
            RE::NiPoint3    lastPos{};
            const void*     lastSpace = nullptr;
            float           moved = 0.0f, windowStartDist = -1.0f;
            std::uint32_t   attacks = 0, casts = 0;   // on the HELD target, this window
            std::uint32_t   elsewhereActs = 0;        // attacks / casts on another target while paused (R-3)
            bool            wasAttacking = false, wasCasting = false;
            std::uint32_t   lostBase = 0, locBase = 0, leashBase = 0;
            Drive           drive = Drive::kNone;
            std::uint64_t   driveSinceMs = 0, driveRetryAfterMs = 0;
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

        // Is the target ACTIVELY HOSTILE (marth's range rule): in combat (controller POINTER, APMF-B26) and its
        // own combat target is one of the PARTY: the actor, the player, a player teammate, an actor commanded by
        // any of those (a summon, a reanimated thrall: AIProcess middleHigh commandingActor, a field read), or a
        // member of the actor's own combat group (`a_members`, its allies in this fight; may be null). Game thread.
        bool ActivelyHostile(RE::Actor* a_target, RE::Actor* a_actor, const std::vector<std::uint32_t>* a_members,
                             RE::FormID* a_victim = nullptr) {
            if (!a_target->GetActorRuntimeData().combatController) return false;
            const RE::ActorHandle cth = a_target->GetActorRuntimeData().currentCombatTarget;
            const auto            ctp = cth.get();
            const auto*           ct  = ctp.get();
            if (!ct) return false;
            if (a_victim) *a_victim = ct->GetFormID();
            if (ct == a_actor || ct->IsPlayerRef() || ct->IsPlayerTeammate()) return true;
            if (a_members && std::find(a_members->begin(), a_members->end(), cth.native_handle()) != a_members->end())
                return true;
            const auto  cmdp = ct->GetCommandingActor();
            const auto* cmd  = cmdp.get();
            return cmd && (cmd == a_actor || cmd->IsPlayerRef() || cmd->IsPlayerTeammate());
        }

        void StopDrive(RE::FormID a_id, Track& a_t, const char* a_why) {
            if (a_t.drive == Drive::kNone) return;
            apmf::combatapproach::InternalInfo info{};
            apmf::combatapproach::GetInternalInfo(a_id, info);
            apmf::combatapproach::ClearInternalBound(a_id);
            spdlog::info("[hold] 0x{} {} drive OFF ({}): the actor's combat area carried it {} time(s); the engine's "
                         "own area answers again.",
                         Hex(a_id), DriveName(a_t.drive), a_why, info.applied);
            a_t.drive = Drive::kNone;
        }

        APMF_API::Handle ClaimHandle(RE::FormID a_actor, APMF_API::Intent a_intent, RE::FormID* a_form = nullptr) {
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, a_intent, p, basis, &h))
                return APMF_API::kInvalidHandle;
            if (a_form) *a_form = a_intent == APMF_API::kIntent_PursuitLeash ? p.target : p.form;
            return h;
        }

        // Review F-3: does the actor's WINNING ch.23 leash outrank the engagement claim `a_eng` on the leash's
        // own facet (CombatAction)? A tie is the engagement's (marth 2026-10-12: the engagement wins).
        bool LeashOutranks(RE::FormID a_actor, APMF_API::Handle a_eng) {
            const APMF_API::Handle lh = ClaimHandle(a_actor, APMF_API::kIntent_PursuitLeash);
            if (lh == APMF_API::kInvalidHandle) return false;
            return apmf::sources::ClaimPriority(lh, apmf::sources::Facet::kCombatAction) >
                   apmf::sources::ClaimPriority(a_eng, apmf::sources::Facet::kCombatAction);
        }

        // The Character vtable (verified at Install) for the StopCombat identity check.
        std::uintptr_t g_vtChar = 0;
        std::atomic<bool> g_stopCombatOk{ false };

        // The actor's combat group, copied under the group's own READ lock (BSReadLockGuard, recursive for a
        // writer; nothing nested inside it): its targets other than `a_skip` and its members' handles. Game
        // thread; the controller is read here exactly as StopCombat below is called (APMF-B26).
        void ReadGroup(RE::Actor* a_actor, std::vector<RE::ActorHandle>* a_targets, std::vector<std::uint32_t>& a_members,
                       std::uint32_t a_skip) {
            auto* cc = a_actor->GetActorRuntimeData().combatController;
            auto* g  = cc ? cc->combatGroup : nullptr;
            if (!g) return;
            RE::BSReadLockGuard guard(g->lock);
            if (a_targets)
                for (const auto& t : g->targets)
                    if (t.targetHandle.native_handle() != a_skip) a_targets->push_back(t.targetHandle);
            for (const auto& m : g->members) a_members.push_back(m.memberHandle.native_handle());
        }

        // marth's range rule: end the actor's combat through the engine's own Actor::StopCombat (Character
        // vtable slot 0xE5: AE 0x6B70A0 / SE 0x625920, `void(this)`, it reads rcx only), RANKED. Game thread.
        // THREADING (APMF-B26 cross-reference): StopCombat frees the actor's CombatController (actor+0x160)
        // INLINE with no lock (AE 0x6B7214 -> 0x558500, 0x6B7235). It is called here from the player's update
        // seat (Arbiter::OncePerFrame), i.e. Main::Update's own context, which runs right after the engine
        // resets its AI job lists (AE 0x5FC260 / 0x5FC180) -- so the combat jobs that read the controller are
        // believed quiescent there (INFERRED from that reset, not proven), the same exposure as a script's
        // Actor.StopCombat. Returns what it did, for the log.
        std::string StopCombatRanked(RE::Actor* a_actor, APMF_API::Handle a_entry, RE::FormID a_dropped,
                                     RE::ActorHandle a_droppedH) {
            auto* cc = a_actor->GetActorRuntimeData().combatController;
            if (!cc) return "not in combat";
            if (!g_stopCombatOk.load(std::memory_order_relaxed))
                return "NOT ended: the StopCombat seat failed its self-check at install";
            if (*reinterpret_cast<const std::uintptr_t*>(a_actor) != g_vtChar)
                return "NOT ended: the actor is not a Character (vtable identity check)";
            const RE::FormID id = a_actor->GetFormID();
            RE::FormID       pinT = 0;
            if (ClaimHandle(id, APMF_API::kIntent_TargetPin, &pinT) != APMF_API::kInvalidHandle && pinT != 0 &&
                pinT != a_dropped)
                return "NOT ended: a winning ch.20 pin names another target (that fight is wanted)";
            // Review R-1: the dropped target was the fight only if NO OTHER live target of the actor's group is
            // attacking the party. The group's targets and members are copied under the group's own READ lock
            // (BSReadLockGuard, recursive for a writer; nothing nested inside it), then judged outside it.
            std::vector<RE::ActorHandle> others;
            std::vector<std::uint32_t>   members;
            ReadGroup(a_actor, &others, members, a_droppedH.native_handle());
            for (const auto& oh : others) {
                const auto op = oh.get();
                auto*      o  = op.get();
                if (!o || o->IsDead() || o->IsDisabled()) continue;
                RE::FormID victim = 0;
                if (ActivelyHostile(o, a_actor, &members, &victim))
                    return fmt::format("NOT ended: 0x{} is still attacking the party (its target 0x{}) -- that fight "
                                       "stays the engine's",
                                       Hex(o->GetFormID()), Hex(victim));
            }
            const APMF_API::Handle ah = ClaimHandle(id, APMF_API::kIntent_CombatApproach);
            if (ah != APMF_API::kInvalidHandle &&
                apmf::sources::ClientForHandle(ah) != apmf::sources::ClientForHandle(a_entry) &&
                apmf::sources::ClaimPriority(ah, apmf::sources::Facet::kCombat) >
                    apmf::sources::ClaimPriority(a_entry, apmf::sources::Facet::kCombat))
                return "NOT ended: another client's ch.24 claim outranks the entry on the Combat facet";
            bool enforce = true;
            try {
                enforce = apmf::facetattr::Decide(apmf::sources::Facet::kCombat, a_entry, apmf::sources::kEngineSource,
                                                  id, "")
                              .enforce;
            } catch (...) {
                enforce = true;
            }
            if (!enforce) return "NOT ended: Engine outranks the entry on the Combat facet (APMF_Sources.ini)";
            a_actor->StopCombat();
            ++g_stopCombats;
            return "combat ENDED (Actor::StopCombat)";
        }

        // marth 2026-10-12: "And if the target is deemed \"not engaged\" combat is dropped immediately and previous
        // behaviour reinstated." Everything the engagement put on the actor goes in THIS pass: combat (StopCombat,
        // ranked, review R-1), the ch.21 claim and a ch.20 pin naming the target (released), the hold slots (so the
        // knowledge seats and ch.23's pursuit exemption stop at once, not at the next Drain), any Harbinger-internal
        // ch.24 bound (a drive, a leash return) and ch.23's search stamps for the actor. Nothing is re-armed: the
        // track is `ended` until the claims are gone, and nothing here waits on a timer. The engine's own group
        // entry is not undone (it is the engine's; the group goes with the controller StopCombat frees). Game thread.
        void DropNotEngaged(RE::Actor* a_actor, RE::FormID a_id, Track& a_t, RE::FormID a_target,
                            RE::ActorHandle a_targetH, const std::string& a_where, float a_range) {
            const APMF_API::Handle entryH = ClaimHandle(a_id, APMF_API::kIntent_CombatEntry);
            RE::FormID             pinT   = 0;
            const APMF_API::Handle pinH   = ClaimHandle(a_id, APMF_API::kIntent_TargetPin, &pinT);
            const char*            drive  = DriveName(a_t.drive);
            const std::string      stop   = StopCombatRanked(a_actor, entryH, a_target, a_targetH);
            const bool             ended  = stop.starts_with("combat ENDED");
            constexpr const char*  kWhy   = "held engagement: not engaged (out of engage range, not hostile)";
            if (a_t.target == a_target) a_t.ended = true;
            if (a_t.drive != Drive::kNone) StopDrive(a_id, a_t, "not engaged");
            apmf::combatapproach::ClearInternalBound(a_id);   // a leash return or any other internal bound
            apmf::actiongate::ForgetEngagement(a_id, "the engagement was dropped (not engaged)");
            EndClaim(a_id, a_target, kWhy);
            const bool pinNamed = pinH != APMF_API::kInvalidHandle && pinT == a_target;
            if (pinNamed) apmf::targetpin::EndHeld(a_id, a_target, kWhy);
            {
                std::unique_lock lk(g_holdMx);
                if (const auto it = g_holds.find(a_id); it != g_holds.end()) {
                    Hold& h = *it->second;
                    if (h.entryTarget == a_target) { h.entryTarget = 0; h.entryHandle = 0; h.entryH = {}; h.entryRange = 0.0f; }
                    if (h.pinTarget == a_target) { h.pinTarget = 0; h.pinHandle = 0; h.pinH = {}; }
                    if (h.Empty()) g_holds.erase(it);
                }
                g_holdCount.store(g_holds.size(), std::memory_order_relaxed);
            }
            ++g_rangeDrops;
            const std::uint32_t nth = CountEnd(a_id, a_target);
            if (nth > 1) ++g_cycles;
            const std::string released = fmt::format(
                "ch.21 claim h={}{}, the hold (knowledge seats + leash exemption), drive {}, internal combat-area "
                "bound, ch.23 search stamps / leash return",
                entryH, pinNamed ? fmt::format(", ch.20 pin h={}", pinH) : std::string(), drive);
            if (ended)
                spdlog::info("[hold] 0x{} -> 0x{} DROPPED (not engaged) -- combat ended, prior behaviour restored "
                             "(released: {}). {} (range {:.0f}), not hostile. End #{} of this engagement this session.",
                             Hex(a_id), Hex(a_target), released, a_where, a_range, nth);
            else
                spdlog::info("[hold] 0x{} -> 0x{} DROPPED (not engaged) -- {} (released: {}). {} (range {:.0f}), not "
                             "hostile. End #{} of this engagement this session.",
                             Hex(a_id), Hex(a_target), stop, released, a_where, a_range, nth);
        }

        // End this hold (a watchdog anomaly): both claims naming the target and the drive. No cooldown (marth
        // 2026-10-12). Returns how many times this actor -> target has been ended this session.
        std::uint32_t EndHold(RE::FormID a_id, Track& a_t, const char* a_reason) {
            a_t.ended = true;
            StopDrive(a_id, a_t, a_reason);
            EndClaim(a_id, a_t.target, a_reason);
            apmf::targetpin::EndHeld(a_id, a_t.target, a_reason);
            const std::uint32_t n = CountEnd(a_id, a_t.target);
            if (n > 1) ++g_cycles;
            return n;
        }
    }

}

namespace apmf::combatentry::hold {

    using namespace hold_detail;

    bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
    bool KnowledgeInstalled() { return g_seatsInstalled.load(std::memory_order_acquire); }

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
        const bool exact = !REL::Module::IsVR() && (REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) ||
                                                    REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97));
        // marth's range rule's StopCombat: a virtual call through the verified Character vtable (slot 0xE5).
        if (exact) {
            REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
            if (apmf::allowance::SeatVerified(vtChar.address(), "EngagementHold.Character.StopCombat(0xE5)")) {
                g_vtChar = vtChar.address();
                g_stopCombatOk.store(true);
            }
        }
        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime";
        } else if (!exact) {
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
            spdlog::warn("[hold] knowledge seats NOT installed -- {}. Held engagements DEGRADE to the old contract: a "
                         "pin ends when the engine loses the target (review F-4); the leash still exempts a held "
                         "approach and the watchdog / range rule still run.",
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
        // Review F-5: the original is stored BEFORE the site is patched (the thunk can run the instant the
        // E8 is rewritten), and it is the VERIFIED call target, not whatever write_call reports.
        for (int i = 0; i < kSites; ++i) g_origDetect[i].store(fn, std::memory_order_release);
        const std::uintptr_t prev0 = s_tr->write_call<5>(sites[0], &DetectThunk<0>);
        const std::uintptr_t prev1 = s_tr->write_call<5>(sites[1], &DetectThunk<1>);
        if (prev0 != fn || prev1 != fn)
            spdlog::error("[hold] knowledge seat patch: the previous call target changed between the check and the "
                          "patch (0x{:X} / 0x{:X}, expected 0x{:X}); the verified function is called.",
                          prev0, prev1, fn);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kDetection, kSiteName[0], sites[0], fn);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kDetection, kSiteName[1], sites[1], fn);
        g_seatsWhy.store("");
        g_seatsInstalled.store(true, std::memory_order_release);
        spdlog::info("[hold] knowledge seats installed: CombatGroup::UpdateTargetDetection (RVA 0x{:X}) post-call at "
                     "the detection listener (+0x{:X}) and the group detection pass (+0x{:X}). A target held by a "
                     "member of the group that the engine flags LOST is un-lost the engine's own way and its "
                     "last-known location is its real one. bleed-out drive {}, trace {}, StopCombat {}.",
                     fn - REL::Module::get().base(), kSiteOff, kPassOff, g_bleedDrive.load() ? "ON" : "OFF",
                     g_trace.load() ? "ON" : "OFF", g_stopCombatOk.load() ? "verified" : "REFUSED");
    }

    namespace {
        void SetSlot(RE::FormID a_actor, bool a_pin, RE::FormID a_target, RE::ActorHandle a_handle, float a_range) {
            if (!std::isfinite(a_range) || a_range < 0.0f) a_range = 0.0f;
            std::uint32_t holderH = 0;
            if (a_target != 0)
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actor)) holderH = a->GetHandle().native_handle();
            std::unique_lock lk(g_holdMx);
            auto& slot = g_holds[a_actor];
            if (!slot) slot = std::make_unique<Hold>();
            const std::uint32_t n = a_target != 0 ? a_handle.native_handle() : 0;
            if (holderH != 0) slot->holderHandle = holderH;
            if (a_pin) { slot->pinTarget = a_target; slot->pinHandle = n; slot->pinH = a_handle; }
            else { slot->entryTarget = a_target; slot->entryHandle = n; slot->entryH = a_handle; slot->entryRange = a_range; }
            if (slot->Empty()) g_holds.erase(a_actor);
            g_holdCount.store(g_holds.size(), std::memory_order_relaxed);
        }
    }

    void SetPin(RE::FormID a, RE::FormID t, RE::ActorHandle h) { SetSlot(a, true, t, h, 0.0f); }
    void ClearPin(RE::FormID a) { SetSlot(a, true, 0, RE::ActorHandle{}, 0.0f); }
    void SetEntry(RE::FormID a, RE::FormID t, RE::ActorHandle h, float range) { SetSlot(a, false, t, h, range); }
    void ClearEntry(RE::FormID a) { SetSlot(a, false, 0, RE::ActorHandle{}, 0.0f); }

    int HeldVia(RE::FormID a_actor, std::uint32_t a_handle, bool a_countLeash) {
        if (a_handle == 0 || !g_enabled.load(std::memory_order_relaxed) ||
            g_holdCount.load(std::memory_order_relaxed) == 0)
            return 0;
        std::shared_lock lk(g_holdMx);
        const auto it = g_holds.find(a_actor);
        if (it == g_holds.end()) return 0;
        const Hold& h = *it->second;
        const int via = h.pinHandle == a_handle ? 1 : h.entryHandle == a_handle ? 2 : 0;
        if (via != 0 && a_countLeash) h.leashExempt.fetch_add(1, std::memory_order_relaxed);
        return via;
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
        struct Snap {
            RE::FormID      id, target;
            RE::ActorHandle handle;
            bool            viaPin;
            RE::FormID      entryTarget;
            RE::ActorHandle entryH;
            float           range;
            std::uint32_t   lost, loc, leash;
        };
        std::vector<Snap> snaps;
        {
            std::shared_lock lk(g_holdMx);
            snaps.reserve(g_holds.size());
            for (const auto& [id, h] : g_holds) {
                const bool pin = h->pinTarget != 0 && h->pinHandle != 0;
                const RE::FormID    tf = pin ? h->pinTarget : h->entryTarget;
                const std::uint32_t th = pin ? h->pinHandle : h->entryHandle;
                if (tf == 0 || th == 0) continue;
                snaps.push_back({ id, tf, pin ? h->pinH : h->entryH, pin, h->entryTarget, h->entryH, h->entryRange,
                                  h->lostCleared.load(), h->locRefreshed.load(), h->leashExempt.load() });
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
                t.windowStartMs = wnow;
                t.lostBase = sn.lost; t.locBase = sn.loc; t.leashBase = sn.leash;
                spdlog::info("[hold] 0x{} HELD engagement on 0x{} (via {}{}): it cannot be lost to the engine's "
                             "detection; it ends when the target is dead, disabled or unloaded, the client releases "
                             "it, a higher-priority claim takes it, after {} s with no path progress or {} s arrived "
                             "without fighting it{}.",
                             Hex(sn.id), Hex(sn.target), sn.viaPin ? "ch.20 pin" : "ch.21 entry",
                             KnowledgeInstalled() ? "" : "; DEGRADED: no knowledge seats, ends on lost",
                             kNoProgressMs / 1000, kArrivedIdleMs / 1000,
                             sn.range > 0.0f ? fmt::format(", or beyond the engage range {:.0f} while not hostile",
                                                           sn.range)
                                             : std::string());
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
                t.samples.clear();
                t.arrivedSinceMs = 0;
                continue;
            }
            t.inCombat = true;

            const RE::NiPoint3 pos   = actor->GetPosition();
            const void*        space = apmf::combatapproach::SpaceOfRef(actor);
            if (t.lastSpace && space == t.lastSpace) {
                const float step = pos.GetDistance(t.lastPos);
                t.moved += step;
                t.path  += step;
            }
            t.lastPos = pos; t.lastSpace = space;
            const void*        tspace = apmf::combatapproach::SpaceOfRef(target);
            const bool         same   = space && space == tspace;
            const RE::NiPoint3 tpos   = target->GetPosition();
            const float        dist   = same ? pos.GetDistance(tpos) : -1.0f;
            if (t.windowStartDist < 0.0f) t.windowStartDist = dist;

            // Attack / cast STARTS. On THE HELD target they are progress (review F-2 / F-4). While the actor's
            // own combat target is ANOTHER actor -- a pin PAUSED by kTargetPin_OwnLineOfSight (the engine's pick
            // stands by design), or a ch.21-only hold whose engine target is another foe -- the hold is paused
            // by design, and fighting is progress too (review R-3: an intended pause is never an anomaly).
            // Sampled every 250 ms: a swing or a cast lasts longer than that.
            const std::uint32_t curT = actor->GetActorRuntimeData().currentCombatTarget.native_handle();
            const bool onHeld    = curT == t.th.native_handle();
            const bool elsewhere = curT != 0 && !onHeld;
            const bool attacking = actor->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
            const bool casting   = IsCasting(actor);
            const bool started   = (attacking && !t.wasAttacking) || (casting && !t.wasCasting);
            if (started && onHeld) {
                if (attacking && !t.wasAttacking) ++t.attacks;
                if (casting && !t.wasCasting) ++t.casts;
                t.lastHeldActMs = wnow;
            } else if (started && elsewhere) {
                ++t.elsewhereActs;
                t.lastHeldActMs = wnow;   // paused by design and fighting: not stuck
            }
            t.wasAttacking = attacking; t.wasCasting = casting;

            t.samples.push_back({ wnow, dist, tpos, t.path });
            while (t.samples.size() > 1 && t.samples[1].ms + kNoProgressMs <= wnow) t.samples.pop_front();

            const auto life   = target->AsActorState()->GetLifeState();
            const bool downed = life == RE::ACTOR_LIFE_STATE::kBleedout || life == RE::ACTOR_LIFE_STATE::kEssentialDown;
            const APMF_API::Handle engH =
                ClaimHandle(sn.id, sn.viaPin ? APMF_API::kIntent_TargetPin : APMF_API::kIntent_CombatEntry);

            // ---- marth's RANGE RULE (ch.21 with a client range). marth 2026-10-12: "And if the target is deemed
            // \"not engaged\" combat is dropped immediately and previous behaviour reinstated." No debounce: the
            // first poll that finds the target out of range and not hostile drops it, in this same pass. ----
            if (sn.range > 0.0f && sn.entryTarget != 0) {
                auto  ep  = sn.entryH.get();
                auto* et  = ep.get();
                const bool  esame = et && space && space == apmf::combatapproach::SpaceOfRef(et);
                const float ed    = esame ? pos.GetDistance(et->GetPosition()) : -1.0f;
                const bool  out   = et && !et->IsDead() && (!esame || ed > sn.range);
                if (out) {
                    std::vector<std::uint32_t> members;
                    ReadGroup(actor, nullptr, members, 0);
                    if (!ActivelyHostile(et, actor, &members)) {
                        DropNotEngaged(actor, sn.id, t, sn.entryTarget, sn.entryH,
                                       esame ? fmt::format("{:.0f}u away", ed) : std::string("in another cell"),
                                       sn.range);
                        continue;
                    }
                }
            }

            // ---- THE DRIVE (bleed-out / search elsewhere) ----
            const std::uint64_t srchDenied = apmf::actiongate::SearchDeniedAgeMs(sn.id);
            const std::uint64_t srchSeen   = apmf::actiongate::SearchSeenAgeMs(sn.id);
            Drive want = Drive::kNone;
            if (same && g_bleedDrive.load(std::memory_order_relaxed) && downed &&
                (t.drive == Drive::kBleedout || dist > kBleedReachU))
                want = Drive::kBleedout;
            else if (same && (srchDenied <= kSearchStartMs || (t.drive == Drive::kSearch && srchSeen <= kSearchIdleMs)))
                want = Drive::kSearch;
            if (want != Drive::kNone && LeashOutranks(sn.id, engH)) want = Drive::kNone;   // review F-3
            if (want != t.drive && t.drive != Drive::kNone)
                StopDrive(sn.id, t, want == Drive::kNone ? (downed ? "no longer needed" : "the target is up / the search ended")
                                                         : "drive changed");
            if (want != Drive::kNone) {
                const float r = want == Drive::kBleedout ? kBleedRadiusU : kReachU;
                if (t.drive == Drive::kNone) {
                    if (wnow >= t.driveRetryAfterMs &&
                        apmf::combatapproach::SetInternalBound(sn.id, tpos, tspace, r, engH,
                                                               want == Drive::kBleedout ? "held engagement (bleed-out drive)"
                                                                                        : "held engagement (search drive)")) {
                        t.drive        = want;
                        t.driveSinceMs = wnow;
                        ++g_drivesStarted;
                        spdlog::info("[hold] 0x{} {} drive ON: held target 0x{} is {} at {:.0f}u{} -- the actor's combat "
                                     "area is centred on it (R {:.0f}) so the engine's own Return To Combat Area paths "
                                     "it there.",
                                     Hex(sn.id), DriveName(want), Hex(sn.target), LifeName(life), dist,
                                     want == Drive::kSearch ? " and the leash just refused a search for another target"
                                                            : "",
                                     r);
                    }
                } else {
                    // Keep X on the target (it may be carried, pushed or slide).
                    apmf::combatapproach::SetInternalBound(sn.id, tpos, tspace, r, engH,
                                                           want == Drive::kBleedout ? "held engagement (bleed-out drive)"
                                                                                    : "held engagement (search drive)");
                    apmf::combatapproach::InternalInfo info{};
                    if (apmf::combatapproach::GetInternalInfo(sn.id, info) && !info.clientWins && dist > r &&
                        wnow - t.driveSinceMs > kDriveStallMs &&
                        (info.msSinceApply == ~0ull || info.msSinceApply > kDriveStallMs)) {
                        // Review F-6: never left on silently -- released, loud, not retried for a while.
                        ++g_drivesStalled;
                        spdlog::warn("[hold] 0x{} {} drive NOT CARRIED for {} ms (another combat area is current, the "
                                     "standard area is disabled, or the engine's own path to the target failed); "
                                     "distance {:.0f}u. RELEASED; not retried for {} s.",
                                     Hex(sn.id), DriveName(t.drive),
                                     info.msSinceApply == ~0ull ? wnow - t.driveSinceMs : info.msSinceApply, dist,
                                     kDriveRetryMs / 1000);
                        StopDrive(sn.id, t, "not carried");
                        t.driveRetryAfterMs = wnow + kDriveRetryMs;
                    }
                }
            }

            // The telemetry line, once per window.
            if (wnow - t.windowStartMs >= kWindowMs) {
                ++g_windowsLogged;
                spdlog::info("[hold] 0x{} -> 0x{}: moved {:.0f}u in {:.1f} s, distance {:.0f} -> {:.0f}, attacks {}, casts "
                             "{} (on it), {} on another target while paused; the engine lost it {} time(s) (knowledge "
                             "kept, location written {}), leash let the approach run {}; target {}; drive {}.",
                             Hex(sn.id), Hex(sn.target), t.moved, (wnow - t.windowStartMs) / 1000.0, t.windowStartDist,
                             dist, t.attacks, t.casts, t.elsewhereActs, sn.lost - t.lostBase, sn.loc - t.locBase,
                             sn.leash - t.leashBase, LifeName(life), DriveName(t.drive));
                t.windowStartMs   = wnow;
                t.moved           = 0.0f;
                t.windowStartDist = dist;
                t.attacks = t.casts = t.elsewhereActs = 0;
                t.lostBase = sn.lost; t.locBase = sn.loc; t.leashBase = sn.leash;
            }

            // ---- THE WATCHDOGS (principle 7: loud and released, every time; no cooldown -- marth 2026-10-12) ----
            const bool inReach = dist >= 0.0f && dist <= kReachU;
            if (inReach) {
                if (t.arrivedSinceMs == 0) t.arrivedSinceMs = wnow;
                if (t.lastHeldActMs > t.arrivedSinceMs) t.arrivedSinceMs = t.lastHeldActMs;   // a strike restarts it
                if (wnow - t.arrivedSinceMs >= kArrivedIdleMs) {
                    ++g_arrivedIdle;
                    const std::uint32_t nth = EndHold(sn.id, t, "held engagement: arrived but not fighting it (anomaly)");
                    spdlog::warn("[hold] ANOMALY 0x{} -> 0x{}: ARRIVED (within {:.0f}u, distance {:.0f}) but no attack or "
                                 "cast on it for {} s (target {}). The held engagement is RELEASED (the ch.21 and ch.20 "
                                 "claims naming it); the client may engage again at once. End #{} of this engagement "
                                 "this session.",
                                 Hex(sn.id), Hex(sn.target), kReachU, dist, kArrivedIdleMs / 1000, LifeName(life), nth);
                }
                continue;
            }
            t.arrivedSinceMs = 0;
            const Sample& s0 = t.samples.front();
            if (s0.ms + kNoProgressMs > wnow) continue;   // not a full window yet
            const bool acted   = t.lastHeldActMs >= s0.ms && t.lastHeldActMs != 0;
            const bool closer  = dist >= 0.0f && s0.dist >= 0.0f && dist <= s0.dist - kProgressStepU;
            const bool kept    = dist >= 0.0f && s0.dist >= 0.0f && tpos.GetDistance(s0.tpos) >= kPaceTargetU &&
                              t.path - s0.path >= kPaceActorU && dist <= s0.dist + kProgressStepU;
            if (acted || closer || kept) continue;
            ++g_anomalies;
            const float         s0d = s0.dist, aMoved = t.path - s0.path, tMoved = tpos.GetDistance(s0.tpos);
            const std::uint32_t nth = EndHold(sn.id, t, "held engagement: no path progress (anomaly)");
            spdlog::warn("[hold] ANOMALY 0x{} -> 0x{}: NO PATH PROGRESS for {} s while in combat (distance {:.0f} -> "
                         "{:.0f}, actor moved {:.0f}u, target moved {:.0f}u, {}; no attack or cast on it; target {}). The "
                         "held engagement is RELEASED (the ch.21 and ch.20 claims naming it); the client may engage again "
                         "at once. End #{} of this engagement this session.",
                         Hex(sn.id), Hex(sn.target), kNoProgressMs / 1000, s0d, dist, aMoved, tMoved,
                         same ? "same cell / worldspace" : "the target is in another cell or worldspace", LifeName(life),
                         nth);
        }
    }

    void Heartbeat() {
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        if (g_holdCount.load(std::memory_order_relaxed) == 0 && g_tracks.empty()) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (now - g_lastHeartbeatMs < 30000) return;
        g_lastHeartbeatMs = now;
        spdlog::info("[hold] heartbeat: {} hold(s); knowledge seats {} (listener calls {}, group-pass calls {}); held "
                     "detection updates {} ({} by a non-holder member): engine-lost un-lost {}, last-known location "
                     "written {}, entry not found {}, no space {}, not written by priority {}; telemetry windows {}; "
                     "drives started {} (released not carried {}); anomalies: no progress {}, arrived not fighting {}; "
                     "range drops {} (combat ended {}); repeat ends of an already-ended engagement {}.",
                     g_holdCount.load(), g_seatsInstalled.load() ? "installed" : g_seatsWhy.load(), g_siteSeen[0].load(),
                     g_siteSeen[1].load(), g_heldEvents.load(), g_byOther.load(), g_lostCleared.load(), g_locWritten.load(),
                     g_notFound.load(), g_noSpace.load(), g_stood.load(), g_windowsLogged, g_drivesStarted,
                     g_drivesStalled, g_anomalies, g_arrivedIdle, g_rangeDrops, g_stopCombats, g_cycles);
    }

    void ResetAll(const char* why) {
        std::size_t n = g_tracks.size();
        for (auto& [id, t] : g_tracks)
            if (t.drive != Drive::kNone) apmf::combatapproach::ClearInternalBound(id);   // ch.24's own ResetAll drops it too
        g_tracks.clear();
        {
            std::scoped_lock tl(g_traceMx);
            g_traceLast.clear();
        }
        g_ends.clear();
        if (n != 0) spdlog::info("[hold] {} -- dropped {} held-engagement track(s).", why, n);
    }

}

APMF_REGISTER_CHANNEL(CombatEntryChannel);
