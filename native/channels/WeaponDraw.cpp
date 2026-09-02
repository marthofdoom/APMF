#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 4 -- WEAPON DRAW / SHEATHE. One-shot promote (CHANNEL-MAP ch.4):
// Actor::DrawWeaponMagicHands(bool) (vfunc 0xA6, bound). The weapon state is sticky
// (not per-frame), so this is a clean one-shot -- no re-assert. Package-independent.
// Engage draws; Release sheathes (restore-to-sheathed baseline).
// ============================================================================

namespace {

    class WeaponDrawChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "weapon-draw"; }
        int              ChannelNo() const override { return 4; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_WeaponDrawn; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4C, "Numpad5 : toggle weapon drawn/sheathed" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            actor->DrawWeaponMagicHands(true);
            spdlog::info("[ch.4] 0x{:08X} DrawWeaponMagicHands(true) (one-shot, sticky).", actor->GetFormID());
        }

        void Release(RE::Actor* actor) override {
            if (!actor) return;
            actor->DrawWeaponMagicHands(false);
            spdlog::info("[ch.4] 0x{:08X} DrawWeaponMagicHands(false).", actor->GetFormID());
        }
    };

}

APMF_REGISTER_CHANNEL(WeaponDrawChannel);
