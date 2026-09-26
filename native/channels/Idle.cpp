#include "PCH.h"
#include "channels/Idle.h"
#include "core/Allowance.h"       // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ============================================================================
// Channel 12 -- IDLE / ANIMATION. SANCTIONED BOUNDED ONE-SHOT PROMOTE (INVARIANTS
// #0(c), CHANNEL-MAP ch.12): play an animation. This facet has no meaningful deny
// form (the AI's own idle manager is not gated) and no AI decision to arbitrate
// around -- "play this idle" is the requested action itself, not a selection input a
// combat/dialogue AI later decides on, so a single deterministic call at Engage is
// lawful under #0(c), not a stand-in awaiting conversion.
//
// v1 (param.form == 0, every ABI): the FORM-FREE graph path, one
// NotifyAnimationGraph("IdleForceDefaultState") at Engage. Unchanged in behaviour: no
// gate, no Release action, no owner-change action. param.target is not read.
//
// v2 (ABI v17, param.form != 0): "play THIS idle AT this target".
//   param.form   = a TESIdleForm (an IDLE record), REQUIRED for v2;
//   param.target = an optional reference the idle is played at (0 = none: the engine
//                  then uses the actor's own process target, AE 0x6DDF94).
//   ONE call at Engage (and one per owner change / Repoint, each a new declaration):
//   AIProcess::PlayIdle(actor, idle, target). ONE call at Release, and only when the
//   idle is still HELD: NotifyAnimationGraph("IdleForceDefaultState"). No Tick.
//
// THE ENGINE CALL (disassembly of both unpacked images, 2026-09-25; agent log
// apmf-idle-v2.md has the listings). The fork's AIProcess::PlayIdle is
// SetupSpecialIdle(this, actor, DEFAULT_OBJECT::kActionIdle = 64, idle, true, false,
// target), RELOCATION_ID(38290, 39256) = SE 0x64B140 / AE 0x6DDE70 (real bodies, no
// thunk). The engine's own Papyrus Actor.PlayIdle native (AE id 55105 / SE id 54395,
// "Cannot play a None idle on an actor") makes exactly that call on both runtimes:
// process from [actor+0xF8] AE / [actor+0xF0] SE, r8d = 0x40, arg5 = 1, arg6 = 0,
// arg7 = target. What the body does with it:
//   * returns false at once when process->high ([rcx+0x10]) is null -- so the actor
//     must be in high process (checked here before the call, and named);
//   * arg5 = true: the idle's OWN conditions are evaluated against (actor, target)
//     (idle+0x20, AE call 29888); a failing condition returns false. Vanilla
//     IdleActivatePickUpLow carries IsCarryable == 1 on the TARGET, so it is refused
//     against a container. IdleLockPick and IdleGive carry no conditions;
//   * the action is then PERFORMED (AE id 39625), which may return true because the
//     idle was QUEUED on the process, not because the graph already changed state. So
//     "PlayIdle returned true" means ACCEPTED, and this channel confirms the animation
//     separately from the graph's own events (below).
//
// HELD, AND THE RELEASE RESET. The IDLE record carries no usable loop flag (DATA
// loopMin / loopMax are 0 on IdleLockPick, IdleGive, IdleActivatePickUpLow and
// IdleForceDefaultState in Skyrim.esm) and the engine has no "current idle" query we
// can trust (the IsIdlePlaying condition function, AE id 21514, is a stub that always
// answers 0; middleHigh->lastIdlePlayed is the LAST idle, not the current one; and
// high->currentProcessIdle is CLEARED by a successful SetupSpecialIdle). What the
// engine DOES say is in the behaviour graph: a one-shot idle clip ends by raising the
// graph event "IdleStop" (mt_behavior: MT_LockPick is MODE_SINGLE_PLAY with an IdleStop
// trigger at the end of the clip, about 5.6 s; InteractionObjectState, the state the
// interaction idles play in, raises IdleStop on exit and returns to MT_Default_State on
// it). A looping idle raises nothing until something ends it. So:
//     HELD = PlayIdle accepted the idle AND the actor's graph has not raised
//            "IdleStop" since the call.
// A per-actor BSAnimationGraphEvent sink (the core/CastObserve.cpp precedent, whose
// clip-trigger events were captured on the deck 2026-09-04) records it. Release sends
// IdleForceDefaultState only when HELD, the actor is loaded and alive; otherwise it
// sends nothing and says why. The same sink gives the observation log: the first graph
// events after the call (the animation confirmation) or a loud "no animation event".
//
// CROUCH (for clients; nothing here writes sneak). In mt_behavior the sneak locomotion
// lives INSIDE MT_Default_State, and IdleLockPick is an unconditional local wildcard on
// the root state machine to InteractionObjectState -> MT_LockPick, one clip, no sneak
// variant. So an idle played from a crouch leaves the sneak locomotion, plays the
// STANDING clip, and returns to MT_Default_State on IdleStop, where the sneak locomotion
// resumes (sneaking is actor state, not graph state). There is no kneeling lockpick in
// vanilla; a client that wants one needs a kneeling IDLE of its own.
//
// RELEASE AND END. The claim ENDS -- Harbinger releases it and logs the reason, repeated
// on the Release line -- when: the idle could not be played (the form is not an IDLE,
// the target is not a loaded reference, the actor is not loaded / dead / has no AI
// process or no high process data), the engine refused it (PlayIdle returned false),
// or the owner died (Poll). An unload is the ControlMap's own sweep (it releases every
// channel of an unloaded actor); a save load / revert / new game drops every claim
// (ControlMap::Clear) and ResetAll drops the entries. A client that wants another
// idle sends a Repoint or a new request.
//
// THREADING. Engage / OnOwnerChanged / Release run inside ControlMap::Drain on the
// confirmed-main seat. The play task runs from mainthread::Pump right after Drain
// PUBLISHES (it re-reads the published claim, the ch.21 precedent). Anything that ENDS a
// claim (EnqueueRelease) runs from Pump or Poll, never inside Drain. g_entries and
// g_sinks are touched on that one thread only. The anim sink's ProcessEvent runs on
// whatever thread updates the actor's graph, so its record (g_obs) is behind g_obsMx
// and holds only plain values; the sink makes no engine call.
//
// VERSION ROBUSTNESS. The only new engine address is SetupSpecialIdle's, a verified row
// on both runtimes (spec.json "Idle.AIProcess.SetupSpecialIdle"), refused unless it
// verifies; exact 1.6.1170 / 1.5.97 only; VR refused by name. The only raw reads are the
// actor's currentProcess and its high pointer, through the fork's accessors.
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr std::uint64_t kConfirmWaitMs = 3000;   // no graph event this long after an accepted play = say so
    constexpr std::size_t   kFirstTags     = 8;      // graph events kept for the confirmation line

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the v2 path is gated there)" };

    bool IEquals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    }

    // ---- The anim-graph observation (ANY THREAD for the sink, under g_obsMx). ----
    struct Obs {
        bool                     armed = false;
        std::uint32_t            gen = 0;        // the play this record belongs to
        std::uint64_t            playMs = 0;
        bool                     idleStop = false;
        std::uint64_t            idleStopMs = 0;
        std::uint32_t            events = 0;
        std::vector<std::string> first;          // the first kFirstTags tags after the call
    };
    std::mutex                          g_obsMx;
    std::unordered_map<RE::FormID, Obs> g_obs;

    class IdleAnimSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
    public:
        explicit IdleAnimSink(RE::FormID a_fid) : fid(a_fid) {}

        // PASSIVE: record, never consume, never touch the engine. BSFixedString pools are
        // case-insensitive (the first-interned casing wins), so the tag is compared that way.
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            const char*            tagC = a_event->tag.c_str();
            const std::string_view tag  = tagC ? std::string_view(tagC) : std::string_view{};
            if (tag.empty()) return RE::BSEventNotifyControl::kContinue;
            std::scoped_lock lock(g_obsMx);
            const auto it = g_obs.find(fid);
            if (it == g_obs.end() || !it->second.armed) return RE::BSEventNotifyControl::kContinue;
            Obs& o = it->second;
            ++o.events;
            if (o.first.size() < kFirstTags) o.first.emplace_back(tag);
            if (!o.idleStop && IEquals(tag, "IdleStop")) {
                o.idleStop   = true;
                o.idleStopMs = apmf::clock::MonotonicMs();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::FormID fid;
    };

    // One sink per actor ever claimed with a form, kept alive for the session (GAME THREAD
    // ONLY). A registration left on a graph after its claim is harmless: the sink records
    // only while its Obs is armed.
    std::unordered_map<RE::FormID, std::unique_ptr<IdleAnimSink>> g_sinks;

    IdleAnimSink* SinkFor(RE::FormID id) {
        auto& s = g_sinks[id];
        if (!s) s = std::make_unique<IdleAnimSink>(id);
        return s.get();
    }

    void Arm(RE::FormID id, std::uint32_t gen) {
        std::scoped_lock lock(g_obsMx);
        Obs o{};
        o.armed  = true;
        o.gen    = gen;
        o.playMs = apmf::clock::MonotonicMs();
        g_obs.insert_or_assign(id, std::move(o));
    }

    Obs Disarm(RE::FormID id) {
        std::scoped_lock lock(g_obsMx);
        const auto it = g_obs.find(id);
        if (it == g_obs.end()) return {};
        Obs o = std::move(it->second);
        g_obs.erase(it);
        return o;
    }

    Obs Peek(RE::FormID id) {
        std::scoped_lock lock(g_obsMx);
        const auto it = g_obs.find(id);
        return it == g_obs.end() ? Obs{} : it->second;
    }

    std::string TagList(const Obs& o) {
        std::string s;
        for (const auto& t : o.first) {
            if (!s.empty()) s += ", ";
            s += t;
        }
        return s.empty() ? std::string("none") : s;
    }

    // ---- The per-actor v2 entry (GAME THREAD ONLY). ----
    struct Entry {
        RE::FormID    idle = 0;          // the claim's param.form this entry was resolved for
        RE::FormID    target = 0;        // the claim's param.target
        std::uint32_t gen = 0;           // bumps per ApplyV2; a posted task carries it
        std::uint32_t plays = 0;         // PlayIdle calls made
        std::uint32_t accepted = 0;      // ... that returned true
        std::uint32_t refused = 0;       // ... that returned false
        bool          live = false;      // the last call was accepted (Release may reset a held idle)
        bool          confirmLogged = false;
        bool          ending = false;    // Harbinger is releasing this claim
        std::string   idleName;          // EDID + anim event, for the logs
        std::string   endedReason;       // for the Release line
    };

    std::unordered_map<RE::FormID, Entry> g_entries;
    std::atomic<std::size_t>              g_count{ 0 };   // Poll's pre-gate
    std::uint32_t                         g_nextGen = 0;

    // End the WINNING claim this entry was resolved for. MAIN SEAT, OUTSIDE Drain only. A
    // claim that has moved on (a newer ApplyV2) is left alone; that one owns the entry now.
    void EndClaim(RE::FormID id, std::uint32_t gen, const std::string& why) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.ending) return;
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_Idle, param, basis, &h) ||
            param.form != it->second.idle || param.target != it->second.target || h == APMF_API::kInvalidHandle)
            return;
        it->second.ending      = true;
        it->second.endedReason = why;
        apmf::ControlMap::Get().EnqueueRelease(h);
        spdlog::info("[ch.12] 0x{} idle claim ended: {} (idle 0x{}, target 0x{}, claim h={}). Harbinger released "
                     "the claim; a mod that wants another idle sends a new request.",
                     Hex(id), why, Hex(it->second.idle), Hex(it->second.target), h);
    }

    // The one engine call. Posted by ApplyV2(); runs from mainthread::Pump right after
    // Drain published the claim. Re-validates everything (game thread) and drops itself
    // (logged) if anything moved. Every outcome other than "accepted" ENDS the claim.
    void Play(RE::FormID id, std::uint32_t gen, const char* what) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.ending) {
            spdlog::info("[ch.12] 0x{} idle play DROPPED (stale): a newer engage, a release or a load landed "
                         "before it ran.",
                         Hex(id));
            return;
        }
        const RE::FormID idleId = it->second.idle;
        const RE::FormID tf     = it->second.target;

        APMF_API::APMF_Param now{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_Idle, now) || now.form != idleId ||
            now.target != tf) {
            spdlog::info("[ch.12] 0x{} idle play 0x{} DROPPED (stale): the published winning claim now names idle "
                         "0x{} target 0x{}.",
                         Hex(id), Hex(idleId), Hex(now.form), Hex(now.target));
            return;
        }

        // VALIDATION, on the game thread.
        auto*              actor  = RE::TESForm::LookupByID<RE::Actor>(id);
        RE::TESForm*       iform  = RE::TESForm::LookupByID(idleId);
        auto*              idle   = iform ? iform->As<RE::TESIdleForm>() : nullptr;
        RE::TESObjectREFR* target = nullptr;
        RE::AIProcess*     proc   = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        std::string        why;
        // A Repoint is not validated synchronously (ControlMap::EnqueueRepoint), so a
        // form-carrying Repoint of a v1 claim can reach here with the v2 path down.
        if (!g_installed.load(std::memory_order_acquire))
            why = fmt::format("idle v2 is not available ({})", g_notInstalledReason.load(std::memory_order_acquire));
        else if (!actor)               why = "the actor does not resolve";
        else if (!actor->Is3DLoaded()) why = "the actor is not loaded";
        else if (actor->IsDead())      why = "the actor is dead";
        else if (!proc)                why = "the actor has no AI process";
        else if (!proc->high)          why = "the actor is not in high process (the engine plays no idle there)";
        else if (!iform)               why = "param.form does not resolve to a form";
        else if (!idle)
            why = fmt::format("param.form is not an IDLE record (form type {})",
                              static_cast<std::uint32_t>(iform->GetFormType()));
        if (why.empty() && tf != 0) {
            RE::TESForm* tform = RE::TESForm::LookupByID(tf);
            target             = tform ? tform->As<RE::TESObjectREFR>() : nullptr;
            if (tf == id)                   why = "param.target is the actor itself";
            else if (!tform)                why = "param.target does not resolve to a form";
            else if (!target)               why = "param.target is not a reference";
            else if (target->IsDeleted())   why = "param.target is deleted";
            else if (target->IsDisabled())  why = "param.target is disabled";
            else if (!target->Is3DLoaded()) why = "param.target is not loaded";
        }
        if (!why.empty()) {
            spdlog::warn("[ch.12] 0x{} idle {} 0x{} at target 0x{} NOT PLAYED: {}. No engine call was made.",
                         Hex(id), what, Hex(idleId), Hex(tf), why);
            EndClaim(id, gen, fmt::format("idle not played: {}", why));
            return;
        }

        const char* edid = idle->formEditorID.c_str();
        const char* evn  = idle->animEventName.c_str();
        it->second.idleName = fmt::format("{} (event {})", (edid && *edid) ? edid : "?", (evn && *evn) ? evn : "?");

        // Observe from BEFORE the call: the graph may answer inside it. AddAnimationGraphEventSink
        // returns false when this sink is already on the graph (a re-point) or there is no graph.
        const bool sinkAdded = actor->AddAnimationGraphEventSink(SinkFor(id));
        Arm(id, gen);

        // THE CALL. Once. The fork's binding: RELOCATION_ID(38290, 39256).
        const bool ok = proc->PlayIdle(actor, idle, target);

        Entry& e = it->second;   // no insertion/erase since `it` was taken (PlayIdle never calls back into us)
        ++e.plays;
        ok ? ++e.accepted : ++e.refused;
        e.live          = ok;
        e.confirmLogged = false;

        if (!ok) {
            Disarm(id);
            spdlog::warn("[ch.12] 0x{} idle {} {} at target 0x{}: REQUESTED -> the engine REFUSED (PlayIdle returned "
                         "false: the idle's own conditions failed for this actor / target, or the actor is in a state "
                         "that plays no idle).",
                         Hex(id), what, e.idleName, Hex(tf));
            EndClaim(id, gen, "engine refused the idle (PlayIdle returned false)");
            return;
        }
        spdlog::info("[ch.12] 0x{} idle {} {} (0x{}) at target 0x{}: REQUESTED -> PlayIdle returned TRUE (accepted; "
                     "sink {}; the graph's own events confirm it below).",
                     Hex(id), what, e.idleName, Hex(idleId), Hex(tf),
                     sinkAdded ? "added" : "already on the graph, or no graph");
    }

    // (Re)write this actor's entry and post ONE task. Inside Drain: nothing here writes the
    // ControlMap and nothing here calls the engine.
    void ApplyV2(RE::FormID id, const APMF_API::APMF_Param& param, const char* what) {
        Entry e{};
        if (const auto it = g_entries.find(id); it != g_entries.end()) {
            e.plays    = it->second.plays;
            e.accepted = it->second.accepted;
            e.refused  = it->second.refused;
        }
        e.idle   = param.form;
        e.target = param.target;
        e.gen    = ++g_nextGen;
        const std::uint32_t gen = e.gen;
        g_entries.insert_or_assign(id, std::move(e));
        g_count.store(g_entries.size(), std::memory_order_relaxed);

        spdlog::info("[ch.12] 0x{} idle claim {} -> idle 0x{} at target 0x{}: ONE AIProcess::PlayIdle call is queued "
                     "for the next main-thread pump (after this claim is published).",
                     Hex(id), what, Hex(param.form), Hex(param.target));
        apmf::mainthread::Post([id, gen, what] { Play(id, gen, what); });
    }

    class IdleChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "idle-anim"; }
        int              ChannelNo() const override { return 12; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Idle; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4E, "NumpadPlus : play a one-shot idle (force default state)" },
            };
            return keys;
        }

        // Posted names are string literals (the task outlives this call).
        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (param.form != 0) {   // v2 (ABI v17)
                ApplyV2(id, param, "ENGAGED");
                return;
            }
            // v1, unchanged.
            if (!actor) return;
            const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
            spdlog::info("[ch.12] 0x{} one-shot idle (IdleForceDefaultState) accepted={}.", apmf::log::Hex(id), ok);
        }

        // A new winning declaration (a Repoint, or another claim taking over). v2: one more
        // PlayIdle for the new idle. v1 (form 0): nothing, as before v2 existed.
        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            if (param.form != 0) ApplyV2(id, param, "RE-POINTED");
        }

        // Relinquish (INVARIANTS #5a). A v1 claim has no entry: nothing to restore, as before.
        void Release(RE::FormID id, RE::Actor* actor) override {
            const auto it = g_entries.find(id);
            if (it == g_entries.end()) return;
            const Entry e = it->second;
            g_entries.erase(it);
            g_count.store(g_entries.size(), std::memory_order_relaxed);

            const Obs o = Disarm(id);
            if (actor) actor->RemoveAnimationGraphEventSink(SinkFor(id));

            std::string reset;
            if (!e.live) {
                reset = "no reset (no idle was accepted)";
            } else if (o.idleStop) {
                reset = fmt::format("no reset (not held: the graph raised IdleStop {} ms after the call)",
                                    o.idleStopMs - o.playMs);
            } else if (!actor) {
                reset = "no reset (held, but the actor did not resolve: unloaded or deleted)";
            } else if (!actor->Is3DLoaded()) {
                reset = "no reset (held, but the actor is not loaded)";
            } else if (actor->IsDead()) {
                reset = "no reset (held, but the actor is dead)";
            } else {
                const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
                reset = fmt::format("HELD (no IdleStop in {} ms) -> IdleForceDefaultState accepted={}",
                                    apmf::clock::MonotonicMs() - o.playMs, ok);
            }
            spdlog::info("[ch.12] 0x{} idle released ({}) (idle {} 0x{}, target 0x{}): PlayIdle called {} time(s) -- "
                         "accepted {}, refused {}. Release: {}. Graph events seen: {} [{}].",
                         Hex(id),
                         e.endedReason.empty() ? std::string("by the client, an unload or a load")
                                               : fmt::format("ENDED BY HARBINGER: {}", e.endedReason),
                         e.idleName.empty() ? std::string("?") : e.idleName, Hex(e.idle), Hex(e.target), e.plays,
                         e.accepted, e.refused, reset, o.events, TagList(o));
        }
    };

}

namespace apmf::idle {

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (PlayIdle is verified on 1.6.1170 and 1.5.97 only)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (PlayIdle is verified on those two only)";
        } else if (!apmf::allowance::SeatVerified(
                       REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38290, 39256) }.address(),
                       "Idle.AIProcess.SetupSpecialIdle")) {
            why = "the address self-check refused AIProcess::SetupSpecialIdle (PlayIdle)";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.12] idle v2 (param.form) NOT available -- {}. kIntent_Idle claims with a form are "
                         "REFUSED; the form-free v1 idle still works.",
                         why);
            return;
        }
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.12] idle v2 available: one AIProcess::PlayIdle(actor, idle, target) per engage / re-point, "
                     "on the main thread; IdleForceDefaultState at release only for a held idle. No hook.");
    }

    bool V2Installed() { return g_installed.load(std::memory_order_relaxed); }

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

        struct Snap { RE::FormID id; std::uint32_t gen; };
        std::vector<Snap> snaps;
        snaps.reserve(g_entries.size());
        for (const auto& [id, e] : g_entries)
            if (!e.ending && e.live) snaps.push_back({ id, e.gen });

        for (const auto& sn : snaps) {
            auto* owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            if (owner && owner->IsDead()) {
                EndClaim(sn.id, sn.gen, "owner dead");
                continue;
            }
            // The observation line, once per accepted play (principle 5).
            const auto it = g_entries.find(sn.id);
            if (it == g_entries.end() || it->second.gen != sn.gen || it->second.confirmLogged) continue;
            const Obs o = Peek(sn.id);
            if (!o.armed || o.gen != sn.gen) continue;
            if (o.events != 0 && (o.idleStop || o.first.size() >= kFirstTags || now - o.playMs >= kConfirmWaitMs)) {
                it->second.confirmLogged = true;
                spdlog::info("[ch.12] 0x{} idle {}: ANIMATION CONFIRMED by the graph ({} event(s) in {} ms: {}){}.",
                             Hex(sn.id), it->second.idleName, o.events, now - o.playMs, TagList(o),
                             o.idleStop ? fmt::format("; IdleStop at +{} ms (one-shot, ended by itself)",
                                                      o.idleStopMs - o.playMs)
                                        : std::string("; no IdleStop yet (still playing, or held)"));
            } else if (o.events == 0 && now - o.playMs >= kConfirmWaitMs) {
                it->second.confirmLogged = true;
                spdlog::warn("[ch.12] 0x{} idle {}: NO ANIMATION EVENT from the actor's graph in {} ms after "
                             "PlayIdle returned true (accepted, but not observed playing).",
                             Hex(sn.id), it->second.idleName, now - o.playMs);
            }
        }
    }

    void ResetAll(const char* why) {
        const std::size_t n = g_entries.size();
        g_entries.clear();
        g_count.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lock(g_obsMx);
            g_obs.clear();
        }
        if (n != 0) spdlog::info("[ch.12] {} -- dropped {} idle entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(IdleChannel);
