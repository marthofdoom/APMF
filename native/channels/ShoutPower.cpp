#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 14 -- SHOUT / POWER SELECTION (CHANNEL-MAP ch.14: "DOCUMENTED select").
// Select which shout occupies the voice slot via ActorEquipManager::EquipShout
// (bound). This is a SELECT (the AI still triggers the shout); the trigger is not
// gated. Sticky one-shot promote -- the voice slot holds the selection, so there is
// no per-tick work and Release does not force-unequip (equip-select is sticky like
// weapon draw). Test facet: select Unrelenting Force (guarded lookup -- a missing
// form is a safe logged no-op). A per-request shout form is a v2 API addition.
// ============================================================================

namespace {

    constexpr RE::FormID kUnrelentingForce = 0x0001307B;   // Skyrim.esm Unrelenting Force (TESShout)

    class ShoutPowerChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "shout-power"; }
        int              ChannelNo() const override { return 14; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_ShoutPower; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x37, "NumpadStar : select shout (Unrelenting Force)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            auto* shout = RE::TESForm::LookupByID<RE::TESShout>(kUnrelentingForce);
            if (!shout) { spdlog::warn("[ch.14] 0x{} shout form not found -- no-op.", apmf::log::Hex(id)); return; }
            if (auto* eqm = RE::ActorEquipManager::GetSingleton()) {
                eqm->EquipShout(actor, shout);
                spdlog::info("[ch.14] 0x{} shout selected (Unrelenting Force). Sticky select; AI triggers it.",
                             apmf::log::Hex(id));
            }
        }

        void Release(RE::FormID, RE::Actor*) override {}   // sticky select; nothing force-restored
    };

}

APMF_REGISTER_CHANNEL(ShoutPowerChannel);
