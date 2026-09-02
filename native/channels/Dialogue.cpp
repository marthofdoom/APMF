#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 10 -- DIALOGUE. One-shot promote (design.md Section 4b Tier A,
// CHANNEL-MAP ch.10, "coarse"): interrupt the actor's current dialogue via
// Actor::StopCurrentDialogue() (vfunc 0x4F). Momentary -- fires once, holds no
// lasting authority (so it never engages the arbiter's target lock).
//
// A persistent dialogue-AVAILABILITY toggle (SetDialogueWithPlayer, vfunc 0x41)
// is a documented follow-up -- its exact signature is left for a live build to
// pin so this channel stays a clean, known one-shot.
// ============================================================================

namespace {

    class DialogueChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "dialogue"; }
        int         ChannelNo() const override { return 10; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4D, "Numpad6 : stop current dialogue (one-shot)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (!target) { spdlog::warn("[ch.10] REFUSED -- no gated target."); return; }
            target->StopCurrentDialogue();
            spdlog::info("[ch.10] one-shot on 0x{:08X} -- StopCurrentDialogue().", target->GetFormID());
            apmf::Arbiter::Get().ClearTargetIfIdle();   // momentary: release the target if nothing else holds it
        }
    };

}

APMF_REGISTER_CHANNEL(DialogueChannel);
