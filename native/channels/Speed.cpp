#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 1a -- GAIT / SPEED. Clean AV gate (CHANNEL-MAP ch.1a): scale kSpeedMult,
// the input the movement layer reads for pace. No re-assert, package-independent.
// The multiplier is arbitrary (the code applies any factor); it defaults to 0.5
// (half speed). A per-request multiplier awaits an API v2 param -- for now the
// factor is a channel constant. Per-NPC prior speed captured for exact restore.
// ============================================================================

namespace {

    constexpr float kSpeedFactor = 0.5f;   // arbitrary multiplier (default: half)

    class SpeedChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "gait-speed"; }
        int              ChannelNo() const override { return 1; }   // ch.1a
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Gait; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x47, "Numpad7 : toggle gait scale (kSpeedMult x0.5)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            auto* avo = actor->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.1a] 0x{:08X} no ActorValueOwner.", actor->GetFormID()); return; }
            const float prev = avo->GetActorValue(RE::ActorValue::kSpeedMult);
            m_prev[actor->GetFormID()] = prev;
            avo->SetActorValue(RE::ActorValue::kSpeedMult, prev * kSpeedFactor);
            spdlog::info("[ch.1a] 0x{:08X} kSpeedMult {:.0f}->{:.0f} (x{:.2f}). Clean AV input-gate.",
                         actor->GetFormID(), prev, prev * kSpeedFactor, kSpeedFactor);
        }

        void Release(RE::Actor* actor) override {
            auto it = actor ? m_prev.find(actor->GetFormID()) : m_prev.end();
            if (actor && it != m_prev.end()) {
                if (auto* avo = actor->AsActorValueOwner())
                    avo->SetActorValue(RE::ActorValue::kSpeedMult, it->second);
                spdlog::info("[ch.1a] 0x{:08X} kSpeedMult restored to {:.0f}.", actor->GetFormID(), it->second);
            }
            if (it != m_prev.end()) m_prev.erase(it);
            else if (actor) m_prev.erase(actor->GetFormID());
        }

    private:
        std::unordered_map<RE::FormID, float> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(SpeedChannel);
