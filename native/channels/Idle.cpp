#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 12 -- IDLE / ANIMATION. SANCTIONED BOUNDED ONE-SHOT PROMOTE (INVARIANTS
// #0(c), CHANNEL-MAP ch.12): play a one-shot animation. This facet has no meaningful
// deny form (the AI's own idle manager is not gated) and no AI decision to arbitrate
// around -- "play this idle" is the requested action itself, not a selection input a
// combat/dialogue AI later decides on, so a single deterministic call at Engage is
// lawful under #0(c), not a stand-in awaiting conversion. Two documented paths exist:
// AIProcess::PlayIdle(TESIdleForm*) and NotifyAnimationGraph(event). This channel
// uses the FORM-FREE graph path ("IdleForceDefaultState") so it works on any
// humanoid with no hardcoded form; a specific PlayIdle(TESIdleForm) is the richer
// variant awaiting the API's per-request form param (v2). No state to restore
// (one-shot).
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

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
            spdlog::info("[ch.12] 0x{} one-shot idle (IdleForceDefaultState) accepted={}.", apmf::log::Hex(id), ok);
        }

        void Release(RE::FormID, RE::Actor*) override {}   // one-shot, nothing to restore
    };

}

APMF_REGISTER_CHANNEL(IdleChannel);
