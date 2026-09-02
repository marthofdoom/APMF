#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 11 -- AI-ATTRIBUTE (aggression / confidence / assistance / morality).
// The cleanest source-gate on the board (design.md Section 1a, CHANNEL-MAP ch.11):
// these dynamic ActorValues ARE the inputs the engine's own combat/flee/assist
// decisions read. Setting them biases the AI's own behavior -- no override, no
// re-assert. Package-independent. Per-NPC prior values captured for exact restore.
//
// Test bias: a "fight and stand your ground, help me" disposition -- aggression ->
// Aggressive(2), confidence -> Foolhardy(4), assistance -> HelpsFriendsAndAllies(2),
// morality -> AnyCrime(0).
// ============================================================================

namespace {

    struct Prev { float aggression, confidence, assistance, morality; };

    class AttributeChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "ai-attribute"; }
        int              ChannelNo() const override { return 11; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Disposition; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x50, "Numpad2 : toggle disposition bias (aggression/confidence/assistance/morality)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            auto* avo = actor->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.11] 0x{:08X} no ActorValueOwner.", actor->GetFormID()); return; }
            Prev p{
                avo->GetActorValue(RE::ActorValue::kAggression),
                avo->GetActorValue(RE::ActorValue::kConfidence),
                avo->GetActorValue(RE::ActorValue::kAssistance),
                avo->GetActorValue(RE::ActorValue::kMorality),
            };
            m_prev[actor->GetFormID()] = p;
            avo->SetActorValue(RE::ActorValue::kAggression, 2.0f);   // Aggressive
            avo->SetActorValue(RE::ActorValue::kConfidence, 4.0f);   // Foolhardy
            avo->SetActorValue(RE::ActorValue::kAssistance, 2.0f);   // Helps friends and allies
            avo->SetActorValue(RE::ActorValue::kMorality,   0.0f);   // Any crime
            spdlog::info("[ch.11] 0x{:08X} disposition biased (aggr {:.0f}->2, conf {:.0f}->4, assist {:.0f}->2, "
                         "moral {:.0f}->0). Clean input-gate, no re-assert.",
                         actor->GetFormID(), p.aggression, p.confidence, p.assistance, p.morality);
        }

        void Release(RE::Actor* actor) override {
            if (actor) {
                if (auto it = m_prev.find(actor->GetFormID()); it != m_prev.end()) {
                    if (auto* avo = actor->AsActorValueOwner()) {
                        avo->SetActorValue(RE::ActorValue::kAggression, it->second.aggression);
                        avo->SetActorValue(RE::ActorValue::kConfidence, it->second.confidence);
                        avo->SetActorValue(RE::ActorValue::kAssistance, it->second.assistance);
                        avo->SetActorValue(RE::ActorValue::kMorality,   it->second.morality);
                    }
                    spdlog::info("[ch.11] 0x{:08X} disposition restored.", actor->GetFormID());
                }
            }
            if (actor) m_prev.erase(actor->GetFormID());
        }

    private:
        std::unordered_map<RE::FormID, Prev> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(AttributeChannel);
