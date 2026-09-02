#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 12 -- IDLE / ANIMATION. One-shot promote (CHANNEL-MAP ch.12): play a
// one-shot animation. The AI's own idle manager is not gated (no clean deny on the
// AI's own idles); this is a one-shot injection, not a hold. Two documented paths
// exist: AIProcess::PlayIdle(TESIdleForm*) and NotifyAnimationGraph(event). This
// channel uses the FORM-FREE graph path ("IdleForceDefaultState") so it works on
// any humanoid with no hardcoded form; a specific PlayIdle(TESIdleForm) is the
// richer variant awaiting the API's per-request form param (v2). No state to
// restore (one-shot).
// ============================================================================

namespace {

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

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
            spdlog::info("[ch.12] 0x{:08X} one-shot idle (IdleForceDefaultState) accepted={}.",
                         actor->GetFormID(), ok);
        }

        void Release(RE::Actor*) override {}   // one-shot, nothing to restore
    };

}

APMF_REGISTER_CHANNEL(IdleChannel);
