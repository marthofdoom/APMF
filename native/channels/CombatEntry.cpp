#include "PCH.h"
#include "channels/CombatEntry.h"
#include "channels/TargetPin.h"   // Installed(): whether the ch.20 pin could engage, for the entry log
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
//   answers them). This is why the entry log reads the group membership back instead of
//   inferring it from the return value.
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
// RELEASE AND END (marth: "on release Harbinger stops forcing and does not undo").
//   * Harbinger never re-enters. It makes ONE StartCombat per Engage / OnOwnerChanged (a
//     Repoint, or a new winning claim) and nothing else: no Tick, no watch, no retry.
//     A second entry after the engine ended the fight would be SUSTAINING a decision,
//     which #0 forbids. A client that wants another entry Repoints (that is also the retry
//     after an engine refusal) -- its declared act, not ours.
//   * Release calls NO StopCombat. Once the engine has entered, the fight is ENGINE STATE
//     (a controller, a group, allies told, detection, crime), not something Harbinger holds;
//     stopping it would be an UNDO, and it would also tear down any combat the actor is in
//     for its own reasons (it was attacked meanwhile, or was already fighting when the claim
//     arrived and StartCombat only added a target). The engine ends it the way it ends any
//     fight; a client that wants it over calls Actor::StopCombat itself.
//   * The claim ENDS BY ITSELF (the ch.20 precedent) when the target is dead, disabled, not
//     loaded or unresolvable, or the owner is dead: Poll() releases the winning claim and
//     logs "entry ended: <reason>"; the Release line repeats it.
//   * Save load / revert / new game: claims are never saved (ControlMap::Clear), so the
//     entry state is dropped with them (ResetAll) and an entry task still queued is
//     discarded with the main-thread queue. Whatever combat the engine was in is saved and
//     restored by the ENGINE; the client re-requests if it wants Harbinger again.
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
// confirmed-main seat (the player 0xAD seat, core/MainThread.h); the entry task runs from
// mainthread::Pump right after Drain PUBLISHES (so it re-reads the published claim, the
// OfferPackage precedent); Poll runs on the same seat. g_entries is therefore touched on
// ONE thread only and takes no lock. The entry log reads the actor's controller and its
// group's target list (under the group's own read lock) on the main thread right after
// StartCombat returns -- the same reads StartCombat's own already-in-combat path makes on
// its calling thread, with the same exposure to a StopCombat on another thread
// (APMF-B26, accepted; not closed here).
//
// VERSION ROBUSTNESS. The only engine address is StartCombat's, a verified row on both
// runtimes (spec.json "CombatEntry.Actor.StartCombat"), refused unless it verifies; exact
// 1.6.1170 / 1.5.97 only (the fork's binding also refuses VR by name); [CombatEntry]
// bCombatEntry. Controller / group reads use the fork's declared members, below +0x68
// (static_asserts), exactly as ch.20.
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
        std::uint32_t   gen = 0;       // bumps per Apply; a posted entry task carries it
        std::uint32_t   attempts = 0;  // StartCombat calls made
        std::uint32_t   entered = 0;   // ... that returned true
        std::uint32_t   refused = 0;   // ... that returned false (the engine refused)
        std::uint32_t   skipped = 0;   // entry tasks that did not call (gate or stale)
        std::uint32_t   suppressed = 0;   // entry log lines held back by the rate limit
        std::uint64_t   lastLogMs = 0;
        bool            ending = false;          // Poll() is releasing this claim
        const char*     endedReason = nullptr;   // for the Release line
    };

    std::unordered_map<RE::FormID, Entry> g_entries;
    std::atomic<std::size_t>              g_count{ 0 };   // Poll's pre-gate
    std::uint32_t                         g_nextGen = 0;

    static_assert(offsetof(RE::CombatController, combatGroup) < 0x68);
    static_assert(offsetof(RE::CombatController, targetHandle) < 0x68);
    static_assert(offsetof(RE::CombatGroup, targets) == 0x08);
    static_assert(offsetof(RE::CombatGroup, lock) == 0x160);
    static_assert(sizeof(RE::CombatTarget) == 0xA8);
    static_assert(offsetof(RE::CombatTarget, targetHandle) == 0x00);

    // Is `h` one of the group's combat targets, and is it flagged lost? Under the group's
    // own read lock -- the same walk as channels/TargetPin.cpp's GroupMembership and the
    // engine's own membership test (AE 44706 / SE 43484). kTargetLost: CombatTarget::flags,
    // u16 +0xA6, bit 1, verified on both runtimes (ch.20).
    enum class Membership { kNoGroup, kAbsent, kHeld, kLost };
    Membership GroupMembership(RE::CombatController* cc, const RE::ActorHandle& h) {
        auto* group = cc ? cc->combatGroup : nullptr;
        if (!group) return Membership::kNoGroup;
        RE::BSReadLockGuard guard(group->lock);
        for (const auto& t : group->targets) {
            if (t.targetHandle == h)
                return t.flags.any(RE::CombatTarget::Flags::kTargetLost) ? Membership::kLost : Membership::kHeld;
        }
        return Membership::kAbsent;
    }

    const char* MembershipText(Membership m) {
        switch (m) {
        case Membership::kNoGroup: return "no (no combat controller / group)";
        case Membership::kAbsent:  return "no (not in combatGroup->targets)";
        case Membership::kHeld:    return "yes";
        case Membership::kLost:    return "yes, but flagged kTargetLost";
        }
        return "?";
    }

    // Could a ch.20 pin engage on this target right now? Answered for the log only.
    std::string PinText(RE::FormID id, RE::FormID tf, Membership m) {
        if (!apmf::targetpin::Installed()) return "no (the ch.20 seats are not installed)";
        APMF_API::APMF_Param pin{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_TargetPin, pin))
            return "no (this actor has no kIntent_TargetPin claim; the engine's own selector chooses)";
        if (pin.form != tf) return fmt::format("no (the winning pin names 0x{}, not this target)", Hex(pin.form));
        if (m != Membership::kHeld) return "no (the target is not a live combat-group target)";
        return "yes (pin names this target and it is a combat-group target)";
    }

    // The one engine call. Posted by Apply(); runs from mainthread::Pump right after Drain
    // published the claim. Re-validates everything it was posted for and drops itself
    // (logged) if anything moved.
    void Enter(RE::FormID id, RE::FormID tf, std::uint32_t gen, const char* what) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.target != tf || it->second.ending) {
            spdlog::info("[ch.21] 0x{} entry for target 0x{} DROPPED (stale): a newer engage, a release or a "
                         "load landed before it ran.",
                         Hex(id), Hex(tf));
            return;
        }
        Entry& e = it->second;

        APMF_API::APMF_Param now{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_CombatEntry, now) || now.form != tf) {
            ++e.skipped;
            spdlog::info("[ch.21] 0x{} entry for target 0x{} DROPPED (stale): the published winning claim now "
                         "names 0x{}.",
                         Hex(id), Hex(tf), Hex(now.form));
            return;
        }

        auto* actor  = RE::TESForm::LookupByID<RE::Actor>(id);
        auto  tptr   = e.handle.get();
        auto* target = tptr.get();
        const char* why = nullptr;
        if (!actor)                                                   why = "the actor does not resolve";
        else if (actor->IsPlayerRef())                                why = "the actor is the player";
        else if (!actor->Is3DLoaded())                                why = "the actor is not loaded";
        else if (!actor->GetActorRuntimeData().currentProcess)       why = "the actor has no AI process "
                                                                           "(StartCombat dereferences it unchecked)";
        else if (actor->IsDead())                                     why = "the actor is dead";
        else if (!target)                                             why = "the target does not resolve";
        else if (target->IsDead())                                    why = "the target is dead";
        else if (target->IsDisabled())                                why = "the target is disabled";
        else if (!target->Is3DLoaded())                               why = "the target is not loaded";
        if (why) {
            ++e.skipped;
            spdlog::warn("[ch.21] 0x{} entry {} against 0x{} NOT CALLED: {}. No engine call was made. (A dead, "
                         "disabled or unloaded target, or a dead actor, ends the claim at the next poll; otherwise "
                         "the claim stays live and inert -- Repoint to retry, or Release.)",
                         Hex(id), what, Hex(tf), why);
            return;
        }

        auto* const   ccBefore      = actor->GetActorRuntimeData().combatController;
        const bool    wasInCombat   = ccBefore != nullptr;
        const auto    lifeState     = actor->AsActorState()->GetLifeState();

        // THE CALL. Once. The fork's binding: RELOCATION_ID(37608, 38561), bool(Actor*,
        // Actor*, void*); nullptr third argument = "no group to join" (the engine's own
        // usage, AE 0x7505A3).
        const bool ok = actor->StartCombat(target, nullptr);
        ++e.attempts;
        ok ? ++e.entered : ++e.refused;

        auto* const      ccAfter = actor->GetActorRuntimeData().combatController;
        const Membership m       = GroupMembership(ccAfter, e.handle);

        const auto nowMs = apmf::clock::MonotonicMs();
        if (nowMs - e.lastLogMs < kLogEveryMs && e.lastLogMs != 0) {
            ++e.suppressed;
            return;
        }
        e.lastLogMs = nowMs;
        const std::uint32_t held = std::exchange(e.suppressed, 0);

        if (ok) {
            spdlog::info("[ch.21] 0x{} entry {} against 0x{}: REQUESTED -> the engine ENTERED ({}; controller {}). "
                         "Target is a combat-group target: {}. ch.20 pin could engage: {}.{}",
                         Hex(id), what, Hex(tf),
                         wasInCombat ? "was already in combat: the engine adds the target to its group"
                                     : "was not in combat: new controller and group",
                         ccAfter ? (ccAfter == ccBefore ? "kept" : "built") : "ABSENT after a true return",
                         MembershipText(m), PinText(id, tf, m),
                         held ? fmt::format(" ({} earlier entry line(s) suppressed)", held) : std::string());
        } else {
            spdlog::warn("[ch.21] 0x{} entry {} against 0x{}: REQUESTED -> the engine REFUSED (StartCombat returned "
                         "false; actor lifeState {}, {} before the call). The engine refuses a restrained (6) or "
                         "unconscious (3) actor, a dead actor or target, a target that fails its own distance "
                         "test, and a few engine flags. Nothing was built by Harbinger; the claim stays live and "
                         "inert -- Repoint to retry, or Release.{}",
                         Hex(id), what, Hex(tf), static_cast<std::uint32_t>(lifeState),
                         wasInCombat ? "in combat" : "not in combat",
                         held ? fmt::format(" ({} earlier entry line(s) suppressed)", held) : std::string());
        }
    }

    // Resolve `param.form` on the game thread, (re)write this actor's entry and post ONE
    // entry task. A refused target (0, self, not an actor) ERASES the entry, so the claim
    // stays live but inert and the log says why -- that refusal is the client's input, so
    // Poll() does not end it (the ch.20 rule).
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
        Entry prev{};
        if (const auto it = g_entries.find(id); it != g_entries.end()) prev = it->second;
        g_entries.erase(id);
        if (!why) {
            Entry e    = prev;
            e.target   = tf;
            e.handle   = h;
            e.gen      = ++g_nextGen;
            e.ending   = false;
            e.endedReason = nullptr;
            const std::uint32_t gen = e.gen;
            g_entries.emplace(id, e);
            apmf::mainthread::Post([id, tf, gen, what] { Enter(id, tf, gen, what); });
        }
        g_count.store(g_entries.size(), std::memory_order_relaxed);

        if (why) {
            spdlog::warn("[ch.21] 0x{} combat-entry claim {} but INERT -- target 0x{}: {}. No engine call is made; "
                         "Repoint it to an actor or Release it.",
                         Hex(id), what, Hex(tf), why);
        } else {
            spdlog::info("[ch.21] 0x{} combat-entry {} -> target 0x{}: ONE Actor::StartCombat call is queued for "
                         "the next main-thread pump (after this claim is published).",
                         Hex(id), what, Hex(tf));
        }
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
                spdlog::info("[ch.21] 0x{} combat-entry released (it was inert).", Hex(id));
                return;
            }
            const Entry e = it->second;
            g_entries.erase(it);
            g_count.store(g_entries.size(), std::memory_order_relaxed);
            spdlog::info("[ch.21] 0x{} combat-entry released ({}) (target 0x{}): StartCombat called {} time(s) -- "
                         "entered {}, refused by the engine {}; entry tasks not called {}. Harbinger calls no "
                         "StopCombat: the fight, if any, is the engine's.",
                         Hex(id), e.endedReason ? fmt::format("ENDED BY HARBINGER: {}", e.endedReason)
                                                : std::string("by the client, an unload or a load"),
                         Hex(e.target), e.attempts, e.entered, e.refused, e.skipped);
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

        struct Snap { RE::FormID id, target; RE::ActorHandle handle; };
        std::vector<Snap> snaps;
        snaps.reserve(g_entries.size());
        for (const auto& [id, e] : g_entries)
            if (!e.ending) snaps.push_back({ id, e.target, e.handle });

        for (const auto& sn : snaps) {
            const char* why   = nullptr;
            auto*       owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            auto        tptr  = sn.handle.get();
            if (owner && owner->IsDead())  why = "owner dead";
            else if (!tptr)                why = "target unresolvable";
            else if (tptr->IsDead())       why = "target dead";
            else if (tptr->IsDisabled())   why = "target disabled";
            else if (!tptr->Is3DLoaded())  why = "target unloaded";
            if (!why) continue;

            // End the WINNING claim -- the one this entry was resolved for.
            APMF_API::APMF_Param param{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(sn.id, APMF_API::kIntent_CombatEntry, param, basis,
                                                                 &h) ||
                param.form != sn.target || h == APMF_API::kInvalidHandle)
                continue;   // the claim moved; the next Apply rewrites the entry
            const auto it = g_entries.find(sn.id);
            if (it == g_entries.end() || it->second.target != sn.target || it->second.ending) continue;
            it->second.ending      = true;
            it->second.endedReason = why;
            apmf::ControlMap::Get().EnqueueRelease(h);
            spdlog::info("[ch.21] 0x{} entry ended: {} (target 0x{}, claim h={}). Harbinger released the claim and "
                         "stops nothing; a mod that wants another entry must request again.",
                         Hex(sn.id), why, Hex(sn.target), h);
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
