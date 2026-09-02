#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 10 -- DIALOGUE. One-shot promote (CHANNEL-MAP ch.10, "coarse"): interrupt
// the actor's current dialogue via Actor::PauseCurrentDialogue() (vfunc 0x4F,
// bound). Engage fires it once; there is no lasting state, so Release is a no-op
// (the claim simply refcounts the engagement until released).
//
// A persistent dialogue-AVAILABILITY toggle (SetDialogueWithPlayer, vfunc 0x41) is
// a documented follow-up left for a live build to pin its flag semantics.
// ============================================================================

namespace {

    class DialogueChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "dialogue"; }
        int              ChannelNo() const override { return 10; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Dialogue; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4D, "Numpad6 : pause current dialogue (one-shot)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor) override {
            if (!actor) return;
            actor->PauseCurrentDialogue();
            spdlog::info("[ch.10] 0x{} PauseCurrentDialogue() (one-shot).", apmf::log::Hex(id));
        }

        void Release(RE::FormID, RE::Actor*) override {}   // nothing to restore
    };

}

APMF_REGISTER_CHANNEL(DialogueChannel);
