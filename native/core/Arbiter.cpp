#include "PCH.h"
#include "channels/CombatSubstrate.h"
#include "core/Log.h"
#include "core/Arbiter.h"
#include "core/Clock.h"          // review F2: the frame-critical ERROR rate limit
#include "core/CombatOps.h"       // H1: the owner-thread StopCombat queue (Poll)
#include "core/CastProxy.h"       // castproxy::OnOwnerUpdate (self-flip landing watch, owner seat)
#include "core/ControlMap.h"
#include "core/CasterTypeCensus.h"
#include "core/NonAliasProbe.h"
#include "core/CastObserve.h"
#include "core/RestoreCensus.h"
#include "core/ActionGate.h"
#include "core/CastLeafNature.h"   // ch.7 hostile offense (ABI v40): heartbeat
#include "core/Aversion.h"         // ch.32 aversion (ABI v41): heartbeat
#include "core/CombatMove.h"       // ch.31 block hold (ABI v43): HoldPoll
#include "core/CombatStyle.h"      // ch.33 combat style (ABI v45): Heartbeat
#include "core/AttackSelectProbe.h"
#include "core/AttackSelect.h"
#include "core/ProjectileAim.h"
#include "core/PackageGate.h"
#include "core/MainThread.h"
#include "core/Registry.h"
#include "core/Sightline.h"
#include "core/CastGuard.h"        // ch.25 cast guard (ABI v21)
#include "core/EquipPreference.h"   // ch.26 heartbeat (Pump)
#include "core/Sources.h"          // Harbinger 1a
#include "core/PackageSources.h"   // Harbinger 1a
#include "core/SpectatorGate.h"
#include "core/ActorValueObserve.h"   // Harbinger 1b: the observe-only actor-value seat's heartbeat
#include "channels/Travel.h"
#include "channels/TargetPin.h"
#include "channels/CombatEntry.h"
#include "channels/Idle.h"
#include "channels/CombatReentryDeny.h"
#include "channels/CombatApproach.h"

#include <atomic>
#include <exception>
#include <string>

namespace apmf {

    namespace {
        constexpr std::uint32_t kReleaseAllKey = 0x52;   // Numpad0 : release ALL controlled NPCs
        constexpr float         kTestBasis     = 100.0f;  // test claims sit mid-range (a real client can outbid)

        std::uint64_t TestKey(RE::FormID id, APMF_API::Intent intent) {
            return (static_cast<std::uint64_t>(id) << 32) | static_cast<std::uint32_t>(intent);
        }
    }

    Arbiter& Arbiter::Get() {
        static Arbiter s_instance;
        return s_instance;
    }

    // ---- Review F2 of 75e1f75: PER-SUBSYSTEM exception guards (stability audit M5, refined). ----
    // One exception used to stop the WHOLE frame stage for the session (Drain, Pump, every poll): the denies stood
    // (frozen followers) and the posted tasks piled up. Now every subsystem call is caught where it enters, and only
    // THAT subsystem is stopped for the rest of the session (an ERROR naming it, never retried, never masked:
    // principle 7); every other subsystem of the frame keeps running. Two calls are FRAME-CRITICAL and keep running
    // whatever happens: ControlMap::Drain (it publishes every claim and release) and mainthread::Pump (it runs every
    // posted task and completion; a throwing TASK is caught inside Pump by itself). If one of those two itself throws,
    // every claim is released ONCE (ControlMap::ReleaseAll, guarded), so every controlled actor falls back to the
    // engine's own AI instead of standing under denies nobody can lift, and it is an ERROR.
    // C++ exceptions only (/EHsc): an access violation is not one.
    namespace {
        struct Stage {
            const char*       name;
            std::atomic<bool> off{ false };
        };

        void StageFault(Stage& a_s, const char* a_what) {
            if (a_s.off.exchange(true)) return;
            spdlog::error("[arbiter] EXCEPTION in {} ({}) -- THAT subsystem is STOPPED for the rest of this session (no "
                          "retry, never masked); every other subsystem keeps running. Report this log.",
                          a_s.name, a_what ? a_what : "a non-standard exception");
        }

        template <class F>
        void RunStage(Stage& a_s, F&& a_f) {
            if (a_s.off.load(std::memory_order_relaxed)) return;
            try {
                a_f();
            } catch (const std::exception& e) {
                StageFault(a_s, e.what());
            } catch (...) {
                StageFault(a_s, nullptr);
            }
        }

        // MAIN only (OncePerFrame).
        bool          g_criticalReleased = false;
        std::uint64_t g_criticalLastMs = 0, g_criticalUnlogged = 0;

        template <class F>
        void FrameCritical(const char* a_name, F&& a_f) {
            const char* what = nullptr;
            try {
                a_f();
                return;
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
                what = "a non-standard exception";
            }
            const std::uint64_t now = apmf::clock::MonotonicMs();
            if (g_criticalLastMs != 0 && now - g_criticalLastMs < 10000 && g_criticalReleased) {
                ++g_criticalUnlogged;
                return;
            }
            g_criticalLastMs = now;
            spdlog::error("[arbiter] EXCEPTION in the frame-critical {} ({}) -- it keeps running every frame{}{}. Report "
                          "this log.",
                          a_name, what,
                          g_criticalReleased ? "; every claim was already released once this session"
                                             : "; EVERY CLAIM IS RELEASED NOW (once per session): the controlled actors "
                                               "fall back to the engine's own AI",
                          g_criticalUnlogged ? fmt::format(" ({} more such exception(s) not logged)", g_criticalUnlogged)
                                             : std::string());
            g_criticalUnlogged = 0;
            if (g_criticalReleased) return;
            g_criticalReleased = true;
            try {
                ControlMap::Get().ReleaseAll("a frame-critical stage threw (review F2 of 75e1f75)");
            } catch (const std::exception& e) {
                spdlog::error("[arbiter] the release of every claim ITSELF threw ({}) -- claims may still stand.", e.what());
            } catch (...) {
                spdlog::error("[arbiter] the release of every claim ITSELF threw (a non-standard exception) -- claims may "
                              "still stand.");
            }
        }
    }

// The stage is named by the call's own text: the ERROR says exactly which subsystem stopped.
#define APMF_FRAME_STAGE(call)                         \
    do {                                               \
        static Stage s_stage{ #call };                 \
        RunStage(s_stage, [&] { call; });              \
    } while (0)
#define APMF_ACTOR_STAGE(call) APMF_FRAME_STAGE(call)

    void Arbiter::OnActorUpdate(RE::Actor* actor) {
        // Review F2 of 75e1f75: each sub-stage is guarded on its own (any thread: the stage flag is atomic). The
        // channels' Tick stops alone when it throws; the self-flip watch keeps running, and the reverse.
        APMF_ACTOR_STAGE(ControlMap::Get().OnActorUpdate(actor));
        // Self-flip landing watch (feat/apmf-self-delivery-proxy, review F4): checked on the
        // owner's OWN update, after its Actor::Update. One relaxed load when nothing is armed.
        APMF_ACTOR_STAGE(apmf::castproxy::OnOwnerUpdate(actor));
    }

    void Arbiter::OncePerFrame(float a_delta) {
        FrameCritical("ControlMap::Drain", [] { ControlMap::Get().Drain(); });

        // Drain the confirmed-main-thread task queue, on this SAME seat and STRICTLY
        // AFTER ControlMap::Drain() has published its new snapshot. That ordering is
        // load-bearing, not incidental: ch.8b's Release posts the delivery-flip proxy
        // teardown here precisely so it lands AFTER the cleared claim is visible to
        // the combat-thread cast seats (Docs/INVARIANTS.md #20's release-ordering
        // rule; channels/CastCompose.cpp). See core/MainThread.h for why this seat
        // and not SKSE's AddTask.
        FrameCritical("apmf::mainthread::Pump", [] { apmf::mainthread::Pump(); });

        // H1 (2026-11-16, core/CombatOps.h): the owner-thread StopCombat queue's main-side checks, right after the
        // pump delivered this frame's drain completions: an op whose actor lost its controller is cancelled, one
        // whose actor's combat update never ran for 1 s of game time is made here (an ERROR). One relaxed load while
        // nothing is queued; the 60 s RULE C line.
        APMF_FRAME_STAGE(apmf::combatops::Poll(a_delta));

        // ABI v18 OWN LINE OF SIGHT (core/Sightline.h). The one place its rays are cast for the
        // table: asked-for pairs are (re)measured here, at most kLosMaxPairsPerFrame per frame
        // (3 rays each), no lock held across a ray. One relaxed load when not armed.
        APMF_FRAME_STAGE(apmf::sightline::Pump());

        // ch.25 CAST GUARD (kIntent_CastGuard, ABI v21; core/CastGuard.h): refreshes the seats'
        // any-guard pre-gate from the snapshot Drain just published, and measures the line of fire
        // of every guarded caster (at most kMaxMeasuresPerFrame per frame). Main thread, no lock
        // held across an engine call; the friendlies' own-ray pairs it asks for are measured by
        // sightline::Pump on the next frames.
        APMF_FRAME_STAGE(apmf::castguard::Pump());

        // ch.26 EQUIP PREFERENCE (ABI v22; core/EquipPreference.h): the 30 s heartbeat of the score
        // lift and the force counters. One relaxed load and a clock read between beats.
        APMF_FRAME_STAGE(apmf::equippref::Pump());

        // COMBAT SUBSTRATE (S2a, channels/CombatSubstrate.h): opens, serves, closes and drops substrates,
        // after Drain published (the floors it files are seen here one frame later) and after the pump.
        // One relaxed load while nothing is open or scheduled.
        APMF_FRAME_STAGE(apmf::combatsubstrate::Poll());

        // Docs/SPEC-PACKAGE-HOLD.md §4.1 item 1 -- OBSERVE-ONLY package-drift
        // correlation probe. Reuses this EXISTING once-per-frame game-thread seat
        // (no new thread, no new hook); self-gated on NonAliasProbe's NumLock
        // switch and self-throttled to ~250ms internally, so this call costs one
        // relaxed atomic load whenever the probe is off (the default).
        APMF_FRAME_STAGE(apmf::nonaliasprobe::PollClaimedPackages());

        // CASTER-TYPE CENSUS (2026-09-29). PASSIVE, [Probe] bCasterTypeCensus (default 1 while
        // field testing, STATUS pre-release checklist). Runs after Drain() so it reads the claim
        // snapshot just published; opens a window per driving cast claim and prints which
        // CombatMagicCaster type the engine built and fired for it, including nothing.
        // Self-throttled ~200 ms; one relaxed load when not armed. See core/CasterTypeCensus.h.
        APMF_FRAME_STAGE(apmf::castertypecensus::Poll());

        // OBSERVE-AND-REPLICATE cast-path probe (marth 2026-09-04). Fully passive:
        // reads loaded high-process actors' MagicCaster state + registers a passive
        // anim-event sink on casting NPCs, logging the exact cast sequence so MFO can
        // replicate it. Always-on, per-actor rate-limited, self-throttled (~100ms) --
        // no hotkey, no toggle, never mutates. See core/CastObserve.h.
        APMF_FRAME_STAGE(apmf::castobserve::Poll());

        // PHASE 0b RESTORE-CASTER CENSUS (2026-09-29). PASSIVE, [Probe] bRestoreCensus (default 1
        // while field testing, STATUS pre-release checklist). Self-throttled ~200 ms; opens a window
        // per restore-shaped cast claim and prints what the engine's Restore caster did, including
        // nothing. Runs after Drain() so it reads the snapshot just published. See core/RestoreCensus.h.
        APMF_FRAME_STAGE(apmf::restorecensus::Poll());

        // PFP Phase 0 (marth 2026-09-06) -- movement-leaf probe heartbeat (RULE C:
        // print the stage's hit count even when zero, so a dead anchor is never
        // mistaken for "nothing to report"). INI-gated ([Probe.mvcbt] Enable=0
        // default); a relaxed atomic-bool load the rest of the time. See
        // core/ActionGate.cpp's PFP section.
        APMF_FRAME_STAGE(apmf::actiongate::PfpHeartbeat());

        // ch.23 PURSUIT LEASH heartbeat (ABI v16), same RULE C shape: every ~30 s while any
        // leash is set, zeros included; one relaxed load when none is.
        APMF_FRAME_STAGE(apmf::actiongate::PursuitHeartbeat());
        // ch.7 HOSTILE OFFENSE (ABI v40), RULE C: the verdict totals, ~30 s after any cast met an Offense-only deny.
        APMF_FRAME_STAGE(apmf::castleafnature::Heartbeat());
        // ch.32 AVERSION (ABI v41), RULE C: the totals, ~30 s after any selection landed on a claimant.
        APMF_FRAME_STAGE(apmf::aversion::Heartbeat());
        // ch.31 BLOCK HOLD (ABI v43): window expiry, the broken-hold tests, the re-raise. Self-throttled 100 ms;
        // one relaxed load while no guard is held.
        APMF_FRAME_STAGE(apmf::combatmove::HoldPoll());
        // ch.33 COMBAT STYLE (ABI v45): rank new claims, ask the engine's own refresh of a controller's cached
        // style after an engage / release. One relaxed load while no claim stands.
        APMF_FRAME_STAGE(apmf::combatstyle::Heartbeat());
        // ch.23 LEASH RETURN (2026-10-12): a leashed, un-held actor whose search was just leash-denied is
        // walked back by its own combat area (a Harbinger-internal ch.24 bound). Self-throttled 250 ms;
        // one relaxed load while no leash is set.
        APMF_FRAME_STAGE(apmf::actiongate::LeashReturnPoll());

        // [Probe] bRangedSelect (PASSIVE, phase 1 of the Ranged-selection deny): per-actor leaf
        // summaries ~15 s and a RULE C totals line ~30 s, zeros included. One relaxed load when OFF.
        APMF_FRAME_STAGE(apmf::actiongate::RangedProbeHeartbeat());
        // [Probe] bAttackSelect (PASSIVE, ClickUp 86e3bw775): drains the attack-selection records into lines,
        // per-actor summaries ~20 s, RULE C totals ~30 s (zeros included). One relaxed load when OFF.
        APMF_FRAME_STAGE(apmf::atkprobe::Heartbeat());
        // ch.18 ATTACK SELECTION (ABI v34): the ~60 s totals line (zeros included) once a claim was filed, and the
        // re-patched-site check. One relaxed load otherwise.
        APMF_FRAME_STAGE(apmf::attackselect::Heartbeat());
        // ch.29 PROJECTILE AIM (ABI v36): sums the engine's own frame delta into the unpaused game clock the shot
        // window runs on, resolves shots whose flight window elapsed (misses), the ~60 s totals line once a claim
        // was filed, and the re-patched-site check. One relaxed load otherwise.
        APMF_FRAME_STAGE(apmf::projectileaim::Heartbeat(a_delta));

        // "Does 0x49 actually redirect?" probe (marth 2026-09-06, core/PackageGate.cpp).
        // Same RULE C shape as the PfpHeartbeat above: self-throttled internally
        // (~30s), INI-gated ([PackageGate] EnableRedirectLog, default ON), one
        // relaxed atomic-bool load the rest of the time.
        APMF_FRAME_STAGE(apmf::packagegate::Heartbeat());
        // The Spectator seat (core/SpectatorGate.cpp): RULE C totals ~30 s, zeros included.
        APMF_FRAME_STAGE(apmf::spectatorgate::Heartbeat());
        APMF_FRAME_STAGE(apmf::avobserve::Heartbeat());

        // ch.19 (kIntent_Travel) LEG MONITOR. Reuses this EXISTING once-per-frame
        // game-thread seat -- no new thread, no new hook. It is NOT a re-assert
        // (Channel.h): it never re-offers a package, it only decides when a leg has
        // ENDED (arrived / the actor is in combat / the destination is gone / stuck)
        // and drops the internal ch.9 offer. Deliberately NOT on Channel::Tick, which
        // runs from the multi-threaded Character 0xAD seat while this monitor resolves
        // handles and makes engine calls. Self-throttled internally; one relaxed atomic
        // load while nothing is travelling.
        APMF_FRAME_STAGE(apmf::travel::Poll());

        // ch.20 (kIntent_TargetPin) END-OF-PIN MONITOR, same seat and same shape as the
        // travel monitor above: it only decides that a pin has ENDED (owner dead; target
        // dead / disabled / unloaded / unresolvable / lost) and releases that claim. It
        // writes no engine state. Self-throttled; one relaxed atomic load while nothing
        // is pinned.
        APMF_FRAME_STAGE(apmf::targetpin::Poll());

        // ch.21 (kIntent_CombatEntry) END-OF-CLAIM MONITOR, same seat and shape as the pin
        // monitor above: it only decides that an entry claim has ENDED (owner dead; target
        // dead / disabled / unloaded / unresolvable; "combat ended" -- the actor's
        // combatController pointer is null after a successful entry) and releases it. It never re-enters
        // combat and writes no engine state. Self-throttled; one relaxed atomic load while
        // nothing is claimed.
        APMF_FRAME_STAGE(apmf::combatentry::Poll());

        // THE HELD ENGAGEMENT (2026-10-12, channels/CombatEntry.cpp): per-hold telemetry (distance moved
        // per 5 s window, attacks / casts, engine-lost overrides), the no-progress watchdog (a loud
        // anomaly, then the ch.21 / ch.20 claims are released) and the bleed-out drive. After the ch.20 /
        // ch.21 monitors so an ordinary end is theirs. Self-throttled 250 ms; cheap while nothing is held.
        APMF_FRAME_STAGE(apmf::combatentry::hold::Poll());
        APMF_FRAME_STAGE(apmf::combatentry::hold::Heartbeat());

        // ch.12 idle v2 (kIntent_Idle with a form, ABI v17) END-OF-CLAIM MONITOR + OBSERVATION
        // LOG, same seat and shape: it ends a v2 idle claim whose owner died and writes the one
        // animation-confirmation line per accepted play. It writes no engine state.
        // Self-throttled; one relaxed atomic load while no v2 idle is claimed.
        APMF_FRAME_STAGE(apmf::idle::Poll());

        // HARBINGER 1a (core/Sources.h): apply menu slider changes (atomic + APMF_Sources.ini),
        // the 60 s seat census (Q13 re-hook on top), the package capture poll and the scene
        // probe's heartbeat. Same once-per-frame MAIN seat, after Drain() published.
        APMF_FRAME_STAGE(apmf::sources::Pump());
        APMF_FRAME_STAGE(apmf::packagesources::Poll());

        // ch.22 (kIntent_CombatReentryDeny) END-OF-WINDOW MONITOR, same seat and shape: it ends a
        // deny claim when its window elapses or the owner dies, and logs a DENY MISS (the actor
        // entered combat under a live window without a ch.21 entry passing the seat). It writes
        // no engine state. Self-throttled; one relaxed atomic load while nothing is claimed.
        APMF_FRAME_STAGE(apmf::reentrydeny::Poll());

        // ch.24 (kIntent_CombatApproach, ABI v19) CLAIM MONITOR, same seat and shape: it resolves
        // each claim's X to a position + cell space for the area seat to carry (the seat never
        // looks up a form), decides the state (waiting / approaching / holding / yielded / disabled
        // / leashed) from what the seats saw, ENDS a claim that is over (arrived, X lost, combat
        // ended, the engine stopped carrying the bound, actor gone) by releasing it, and writes
        // the per-actor status line. It makes NO engine write itself (the seats do, on the combat
        // thread). Self-throttled (100 ms); the ~30 s heartbeat prints the seat counters, zeros
        // included, while any claim exists.
        APMF_FRAME_STAGE(apmf::combatapproach::Poll());
        APMF_FRAME_STAGE(apmf::combatapproach::Heartbeat());
    }

#undef APMF_ACTOR_STAGE
#undef APMF_FRAME_STAGE

    void Arbiter::ReleaseAll(const char* why) {
        ControlMap::Get().ReleaseAll(why);
        m_testHandles.clear();
    }

    RE::Actor* Arbiter::CrosshairActor() const {
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) {
                auto* a = ref->As<RE::Actor>();
                if (a && !a->IsPlayerRef()) return a;
            }
        }
        return nullptr;
    }

    void Arbiter::DispatchHotkey(std::uint32_t code) {
        if (code == kReleaseAllKey) {
            spdlog::info("[test] release-all key -- dropping every controlled NPC.");
            ReleaseAll("hotkey-release-all");
            return;
        }

        auto* channel = Registry::Get().ChannelForHotkey(code);
        if (!channel) return;   // not one of our keys

        auto* actor = CrosshairActor();
        if (!actor) {
            spdlog::warn("[test] ch.{} {} REFUSED -- aim the crosshair at a follower/NPC first.",
                         channel->ChannelNo(), channel->Name());
            return;
        }

        const RE::FormID     id     = actor->GetFormID();
        const APMF_API::Intent intent = channel->ServesIntent();
        const std::uint64_t  key    = TestKey(id, intent);

        if (auto it = m_testHandles.find(key); it != m_testHandles.end()) {
            ControlMap::Get().EnqueueRelease(it->second);
            m_testHandles.erase(it);
            spdlog::info("[test] ch.{} {} -- REMOVED 0x{} '{}' from the controlled set.",
                         channel->ChannelNo(), channel->Name(), apmf::log::Hex(id),
                         actor->GetName() ? actor->GetName() : "?");
            return;
        }

        // Test surface passes no param -> each channel uses its default (cast-select
        // -> Firebolt, combat-target -> the player). The C-ABI RequestEx path is what
        // carries a real client's chosen spell/target.
        const APMF_API::Handle h = ControlMap::Get().EnqueueRequest(id, intent, kTestBasis, nullptr);
        if (h == APMF_API::kInvalidHandle) return;   // ControlMap already logged the refusal
        m_testHandles[key] = h;
        spdlog::info("[test] ch.{} {} -- ADDED 0x{} '{}' to the controlled set (h={}). Aim another NPC "
                     "+ a key to add it too; Numpad0 releases all.",
                     channel->ChannelNo(), channel->Name(), apmf::log::Hex(id),
                     actor->GetName() ? actor->GetName() : "?", h);
    }

}
