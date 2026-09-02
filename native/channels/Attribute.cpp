#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 11 -- AI-ATTRIBUTE (aggression / confidence / assistance / morality).
// The cleanest source-gate on the board (design.md Section 1a, CHANNEL-MAP ch.11):
// these dynamic ActorValues ARE the inputs the engine's own combat/flee/assist
// decisions read. Setting them biases the AI's own behavior -- no override, no
// re-assert. Package-independent.
//
// Test surface: toggle a "make her fight" bias (aggression -> Aggressive,
// confidence -> Foolhardy). Release restores the captured values.
// ============================================================================

namespace {

    class AttributeChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "ai-attribute"; }
        int         ChannelNo() const override { return 11; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x50, "Numpad2 : toggle disposition bias (aggression+confidence up)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.11] REFUSED -- no gated target."); return; }
            auto* avo = target->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.11] REFUSED -- no ActorValueOwner."); return; }
            prevAggression = avo->GetActorValue(RE::ActorValue::kAggression);
            prevConfidence = avo->GetActorValue(RE::ActorValue::kConfidence);
            avo->SetActorValue(RE::ActorValue::kAggression, 2.0f);   // Aggressive
            avo->SetActorValue(RE::ActorValue::kConfidence, 4.0f);   // Foolhardy
            engaged.store(true);
            spdlog::info("[ch.11] engaged on 0x{:08X} -- aggression {:.0f}->2, confidence {:.0f}->4. Clean "
                         "input-gate (the AI reads these itself), set once, no re-assert.",
                         target->GetFormID(), prevAggression, prevConfidence);
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor) {
                if (auto* avo = actor->AsActorValueOwner()) {
                    avo->SetActorValue(RE::ActorValue::kAggression, prevAggression);
                    avo->SetActorValue(RE::ActorValue::kConfidence, prevConfidence);
                }
            }
            spdlog::info("[ch.11] released -- disposition restored (aggression={:.0f}, confidence={:.0f}).",
                         prevAggression, prevConfidence);
        }

    private:
        float prevAggression = 0.0f;
        float prevConfidence = 0.0f;
    };

}

APMF_REGISTER_CHANNEL(AttributeChannel);
