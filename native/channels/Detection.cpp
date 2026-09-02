#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 16 -- DETECTION / STEALTH. Clean AV gate (CHANNEL-MAP ch.16): set the
// detection-related ActorValues the engine reads. No re-assert, package-independent.
// Test tuning: quiet her movement (kMovementNoiseMult -> 0, silent to detection)
// and sharpen her own senses (kDetectLifeRange +50%). Per-NPC prior values captured.
// ============================================================================

namespace {

    struct Prev { float noise, detect; };

    class DetectionChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "detection"; }
        int              ChannelNo() const override { return 16; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Detection; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x48, "Numpad8 : toggle stealth tuning (silent noise + keener senses)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            auto* avo = actor->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.16] 0x{:08X} no ActorValueOwner.", actor->GetFormID()); return; }
            Prev p{
                avo->GetActorValue(RE::ActorValue::kMovementNoiseMult),
                avo->GetActorValue(RE::ActorValue::kDetectLifeRange),
            };
            m_prev[actor->GetFormID()] = p;
            avo->SetActorValue(RE::ActorValue::kMovementNoiseMult, 0.0f);
            avo->SetActorValue(RE::ActorValue::kDetectLifeRange, p.detect * 1.5f);
            spdlog::info("[ch.16] 0x{:08X} stealth tuned (noise {:.2f}->0, detectRange {:.0f}->{:.0f}). Clean AV gate.",
                         actor->GetFormID(), p.noise, p.detect, p.detect * 1.5f);
        }

        void Release(RE::Actor* actor) override {
            if (actor) {
                if (auto it = m_prev.find(actor->GetFormID()); it != m_prev.end()) {
                    if (auto* avo = actor->AsActorValueOwner()) {
                        avo->SetActorValue(RE::ActorValue::kMovementNoiseMult, it->second.noise);
                        avo->SetActorValue(RE::ActorValue::kDetectLifeRange, it->second.detect);
                    }
                    spdlog::info("[ch.16] 0x{:08X} detection AVs restored.", actor->GetFormID());
                }
                m_prev.erase(actor->GetFormID());
            }
        }

    private:
        std::unordered_map<RE::FormID, Prev> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(DetectionChannel);
