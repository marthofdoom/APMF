#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 1a -- GAIT / SPEED. Clean AV gate (design.md CHANNEL-MAP ch.1a): set
// kSpeedMult, the input the movement layer reads for pace. No re-assert,
// package-independent. Test facet: halve the actor's speed.
// ============================================================================

namespace {

    class SpeedChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "gait-speed"; }
        int         ChannelNo() const override { return 1; }   // ch.1a (sub-split of movement)

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x47, "Numpad7 : toggle half speed (kSpeedMult)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.1a] REFUSED -- no gated target."); return; }
            auto* avo = target->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.1a] REFUSED -- no ActorValueOwner."); return; }
            prevSpeed = avo->GetActorValue(RE::ActorValue::kSpeedMult);
            avo->SetActorValue(RE::ActorValue::kSpeedMult, prevSpeed * 0.5f);
            engaged.store(true);
            spdlog::info("[ch.1a] engaged on 0x{:08X} -- kSpeedMult {:.0f}->{:.0f}. Clean AV input-gate.",
                         target->GetFormID(), prevSpeed, prevSpeed * 0.5f);
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor) {
                if (auto* avo = actor->AsActorValueOwner()) avo->SetActorValue(RE::ActorValue::kSpeedMult, prevSpeed);
            }
            spdlog::info("[ch.1a] released -- kSpeedMult restored to {:.0f}.", prevSpeed);
        }

    private:
        float prevSpeed = 100.0f;
    };

}

APMF_REGISTER_CHANNEL(SpeedChannel);
