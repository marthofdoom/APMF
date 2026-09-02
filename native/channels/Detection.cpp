#include "PCH.h"
#include "core/Registry.h"
#include "core/AvLedger.h"

// ============================================================================
// Channel 16 -- DETECTION / STEALTH. Clean AV gate (CHANNEL-MAP ch.16): set the
// detection-related ActorValues the engine reads. No re-assert, package-independent.
// Test tuning: quiet her movement (kMovementNoiseMult -> 0, silent to detection)
// and sharpen her own senses (kDetectLifeRange +50%).
//
// These AVs PERSIST in the .ess, so both writes go through the co-saved AV ledger
// (core/AvLedger, INVARIANTS #15) -- restored even across a save/load, never stranded.
// ============================================================================

namespace {

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

        void Engage(RE::FormID id, RE::Actor* actor) override {
            if (!actor) return;
            auto* avo = actor->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.16] 0x{:08X} no ActorValueOwner.", id); return; }
            const float detect = avo->GetActorValue(RE::ActorValue::kDetectLifeRange);
            apmf::av::Override(id, actor, RE::ActorValue::kMovementNoiseMult, 0.0f);
            apmf::av::Override(id, actor, RE::ActorValue::kDetectLifeRange, detect * 1.5f);
            spdlog::info("[ch.16] 0x{:08X} stealth tuned (noise->0, detectRange {:.0f}->{:.0f}). Co-saved AV gate.",
                         id, detect, detect * 1.5f);
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            apmf::av::Restore(id, actor, RE::ActorValue::kMovementNoiseMult);
            apmf::av::Restore(id, actor, RE::ActorValue::kDetectLifeRange);
            spdlog::info("[ch.16] 0x{:08X} detection AVs restored.", id);
        }
    };

}

APMF_REGISTER_CHANNEL(DetectionChannel);
