#include "PCH.h"
#include "core/Log.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
#include "core/NonAliasProbe.h"
#include "core/CastObserve.h"
#include "core/ActionGate.h"
#include "core/PackageGate.h"
#include "core/MainThread.h"
#include "core/Registry.h"
#include "channels/Travel.h"
#include "channels/TargetPin.h"
#include "channels/CombatEntry.h"
#include "channels/CombatReentryDeny.h"

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

    void Arbiter::OnActorUpdate(RE::Actor* actor) {
        ControlMap::Get().OnActorUpdate(actor);
    }

    void Arbiter::OncePerFrame() {
        ControlMap::Get().Drain();

        // Drain the confirmed-main-thread task queue, on this SAME seat and STRICTLY
        // AFTER ControlMap::Drain() has published its new snapshot. That ordering is
        // load-bearing, not incidental: ch.8b's Release posts the delivery-flip proxy
        // teardown here precisely so it lands AFTER the cleared claim is visible to
        // the combat-thread cast seats (Docs/INVARIANTS.md #20's release-ordering
        // rule; channels/CastCompose.cpp). See core/MainThread.h for why this seat
        // and not SKSE's AddTask.
        apmf::mainthread::Pump();

        // Docs/SPEC-PACKAGE-HOLD.md §4.1 item 1 -- OBSERVE-ONLY package-drift
        // correlation probe. Reuses this EXISTING once-per-frame game-thread seat
        // (no new thread, no new hook); self-gated on NonAliasProbe's NumLock
        // switch and self-throttled to ~250ms internally, so this call costs one
        // relaxed atomic load whenever the probe is off (the default).
        apmf::nonaliasprobe::PollClaimedPackages();

        // OBSERVE-AND-REPLICATE cast-path probe (marth 2026-09-04). Fully passive:
        // reads loaded high-process actors' MagicCaster state + registers a passive
        // anim-event sink on casting NPCs, logging the exact cast sequence so MFO can
        // replicate it. Always-on, per-actor rate-limited, self-throttled (~100ms) --
        // no hotkey, no toggle, never mutates. See core/CastObserve.h.
        apmf::castobserve::Poll();

        // PFP Phase 0 (marth 2026-09-06) -- movement-leaf probe heartbeat (RULE C:
        // print the stage's hit count even when zero, so a dead anchor is never
        // mistaken for "nothing to report"). INI-gated ([Probe.mvcbt] Enable=0
        // default); a relaxed atomic-bool load the rest of the time. See
        // core/ActionGate.cpp's PFP section.
        apmf::actiongate::PfpHeartbeat();

        // ch.23 PURSUIT LEASH heartbeat (ABI v16), same RULE C shape: every ~30 s while any
        // leash is set, zeros included; one relaxed load when none is.
        apmf::actiongate::PursuitHeartbeat();

        // "Does 0x49 actually redirect?" probe (marth 2026-09-06, core/PackageGate.cpp).
        // Same RULE C shape as the PfpHeartbeat above: self-throttled internally
        // (~30s), INI-gated ([PackageGate] EnableRedirectLog, default ON), one
        // relaxed atomic-bool load the rest of the time.
        apmf::packagegate::Heartbeat();

        // ch.19 (kIntent_Travel) LEG MONITOR. Reuses this EXISTING once-per-frame
        // game-thread seat -- no new thread, no new hook. It is NOT a re-assert
        // (Channel.h): it never re-offers a package, it only decides when a leg has
        // ENDED (arrived / the actor is in combat / the destination is gone / stuck)
        // and drops the internal ch.9 offer. Deliberately NOT on Channel::Tick, which
        // runs from the multi-threaded Character 0xAD seat while this monitor resolves
        // handles and makes engine calls. Self-throttled internally; one relaxed atomic
        // load while nothing is travelling.
        apmf::travel::Poll();

        // ch.20 (kIntent_TargetPin) END-OF-PIN MONITOR, same seat and same shape as the
        // travel monitor above: it only decides that a pin has ENDED (owner dead; target
        // dead / disabled / unloaded / unresolvable / lost) and releases that claim. It
        // writes no engine state. Self-throttled; one relaxed atomic load while nothing
        // is pinned.
        apmf::targetpin::Poll();

        // ch.21 (kIntent_CombatEntry) END-OF-CLAIM MONITOR, same seat and shape as the pin
        // monitor above: it only decides that an entry claim has ENDED (owner dead; target
        // dead / disabled / unloaded / unresolvable; "combat ended" -- the actor's
        // combatController pointer is null after a successful entry) and releases it. It never re-enters
        // combat and writes no engine state. Self-throttled; one relaxed atomic load while
        // nothing is claimed.
        apmf::combatentry::Poll();

        // ch.22 (kIntent_CombatReentryDeny) END-OF-WINDOW MONITOR, same seat and shape: it ends a
        // deny claim when its window elapses or the owner dies, and logs a DENY MISS (the actor
        // entered combat under a live window without a ch.21 entry passing the seat). It writes
        // no engine state. Self-throttled; one relaxed atomic load while nothing is claimed.
        apmf::reentrydeny::Poll();
    }

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
