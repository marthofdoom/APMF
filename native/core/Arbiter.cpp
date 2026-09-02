#include "PCH.h"
#include "core/Log.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
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

        const APMF_API::Handle h = ControlMap::Get().EnqueueRequest(id, intent, kTestBasis);
        if (h == APMF_API::kInvalidHandle) return;   // ControlMap already logged the refusal
        m_testHandles[key] = h;
        spdlog::info("[test] ch.{} {} -- ADDED 0x{} '{}' to the controlled set (h={}). Aim another NPC "
                     "+ a key to add it too; Numpad0 releases all.",
                     channel->ChannelNo(), channel->Name(), apmf::log::Hex(id),
                     actor->GetName() ? actor->GetName() : "?", h);
    }

}
