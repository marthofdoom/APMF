#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 10 -- DIALOGUE. DENY -- suppresses the actor's own in-progress dialogue
// (CHANNEL-MAP ch.10, "coarse") via Actor::PauseCurrentDialogue() (vfunc 0x4F,
// bound). This does not manufacture dialogue; it interrupts/suppresses the AI's own
// ongoing dialogue engagement at the source, the same lever class as movement
// full-block or a package yield (INVARIANTS #0(b)). Engage fires it once; there is
// no lasting state, so Release is a no-op (the claim simply refcounts the
// engagement until released).
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

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            actor->PauseCurrentDialogue();
            spdlog::info("[ch.10] 0x{} PauseCurrentDialogue() (one-shot).", apmf::log::Hex(id));
        }

        void Release(RE::FormID, RE::Actor*) override {}   // nothing to restore
    };

}

APMF_REGISTER_CHANNEL(DialogueChannel);
