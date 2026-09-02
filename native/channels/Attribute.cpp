#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/AvLedger.h"

// ============================================================================
// Channel 11 -- AI-ATTRIBUTE (aggression / confidence / assistance / morality).
// The cleanest source-gate on the board (design.md Section 1a, CHANNEL-MAP ch.11):
// these dynamic ActorValues ARE the inputs the engine's own combat/flee/assist
// decisions read. Setting them biases the AI's own behavior -- no override, no
// re-assert. Package-independent.
//
// These AVs PERSIST in the .ess, so every write goes through the co-saved AV ledger
// (core/AvLedger, INVARIANTS #15) -- the prior value is captured there and restored
// even across a save/load, so a save-while-engaged never strands the bias.
//
// Test bias: a "fight and stand your ground, help me" disposition -- aggression ->
// Aggressive(2), confidence -> Foolhardy(4), assistance -> HelpsFriendsAndAllies(2),
// morality -> AnyCrime(0).
// ============================================================================

namespace {

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

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            apmf::av::Override(id, actor, RE::ActorValue::kAggression, 2.0f);   // Aggressive
            apmf::av::Override(id, actor, RE::ActorValue::kConfidence, 4.0f);   // Foolhardy
            apmf::av::Override(id, actor, RE::ActorValue::kAssistance, 2.0f);   // Helps friends and allies
            apmf::av::Override(id, actor, RE::ActorValue::kMorality,   0.0f);   // Any crime
            spdlog::info("[ch.11] 0x{} disposition biased (aggr->2, conf->4, assist->2, moral->0). "
                         "Clean input-gate, co-saved.", apmf::log::Hex(id));
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            apmf::av::Restore(id, actor, RE::ActorValue::kAggression);
            apmf::av::Restore(id, actor, RE::ActorValue::kConfidence);
            apmf::av::Restore(id, actor, RE::ActorValue::kAssistance);
            apmf::av::Restore(id, actor, RE::ActorValue::kMorality);
            spdlog::info("[ch.11] 0x{} disposition restored.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(AttributeChannel);
