#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/AvLedger.h"

// ============================================================================
// Channel 1a -- GAIT / SPEED. Clean AV gate (CHANNEL-MAP ch.1a): scale kSpeedMult,
// the input the movement layer reads for pace. No re-assert, package-independent.
// The multiplier is arbitrary (the code applies any factor); it defaults to 0.5
// (half speed). A per-request multiplier awaits an API v2 param.
//
// kSpeedMult PERSISTS in the .ess, so the write goes through the co-saved AV ledger
// (core/AvLedger, INVARIANTS #15) -- restored even across a save/load, never
// stranded. ChannelNo() returns 1 (the parent movement facet); this channel is the
// 1a sub-split and is identified by its distinct Intent/Name, never by ChannelNo.
// ============================================================================

namespace {

    constexpr float kSpeedFactor = 0.5f;   // arbitrary multiplier (default: half)

    class SpeedChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "gait-speed"; }
        int              ChannelNo() const override { return 1; }   // ch.1a (sub-split of ch.1)
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Gait; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x47, "Numpad7 : toggle gait scale (kSpeedMult x0.5)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            auto* avo = actor->AsActorValueOwner();
            if (!avo) { spdlog::warn("[ch.1a] 0x{} no ActorValueOwner.", apmf::log::Hex(id)); return; }
            const float prev = avo->GetActorValue(RE::ActorValue::kSpeedMult);
            apmf::av::Override(id, actor, RE::ActorValue::kSpeedMult, prev * kSpeedFactor);
            spdlog::info("[ch.1a] 0x{} kSpeedMult {:.0f}->{:.0f} (x{:.2f}). Clean AV gate, co-saved.",
                         apmf::log::Hex(id), prev, prev * kSpeedFactor, kSpeedFactor);
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            apmf::av::Restore(id, actor, RE::ActorValue::kSpeedMult);
            spdlog::info("[ch.1a] 0x{} kSpeedMult restored.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(SpeedChannel);
