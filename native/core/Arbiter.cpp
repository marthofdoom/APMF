#include "PCH.h"
#include "core/Log.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
#include "core/NonAliasProbe.h"
#include "core/CastObserve.h"
#include "core/MainThread.h"
#include "core/Registry.h"

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

        // ch.8 SelectSpell +ACT (feat/cast-act): drain the cast-drive phase chain
        // (core/CastExecutor.cpp) on this SAME confirmed-main seat -- Engage/
        // OnOwnerChanged/Release (called from Drain() above) may start a drive, and
        // its multi-frame phases (equip-select poll, charge poll) re-Post themselves
        // here every frame. See core/MainThread.h for why this seat, not AddTask.
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
