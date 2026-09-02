#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 16 -- DETECTION / STEALTH. Clean AV gate (design.md CHANNEL-MAP ch.16):
// set the detection-related ActorValues the engine reads. No re-assert,
// package-independent. Test facet: quiet the actor (kMovementNoiseMult -> 0),
// making her movement silent to detection.
// ============================================================================

namespace {

    class DetectionChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "detection"; }
        int         ChannelNo() const override { return 16; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x48, "Numpad8 : toggle silent movement (kMovementNoiseMult 0)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.16] REFUSED -- no gated target."); return; }
            auto* avo = target->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.16] REFUSED -- no ActorValueOwner."); return; }
            prevNoise = avo->GetActorValue(RE::ActorValue::kMovementNoiseMult);
            avo->SetActorValue(RE::ActorValue::kMovementNoiseMult, 0.0f);
            engaged.store(true);
            spdlog::info("[ch.16] engaged on 0x{:08X} -- kMovementNoiseMult {:.2f}->0. Clean AV input-gate.",
                         target->GetFormID(), prevNoise);
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor) {
                if (auto* avo = actor->AsActorValueOwner()) avo->SetActorValue(RE::ActorValue::kMovementNoiseMult, prevNoise);
            }
            spdlog::info("[ch.16] released -- kMovementNoiseMult restored to {:.2f}.", prevNoise);
        }

    private:
        float prevNoise = 1.0f;
    };

}

APMF_REGISTER_CHANNEL(DetectionChannel);
