#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 4 -- WEAPON DRAW / SHEATHE. One-shot promote (design.md Section 4b
// Tier A, CHANNEL-MAP ch.4): Actor::DrawWeaponMagicHands(bool). The weapon state
// is sticky (not per-frame), so this is a clean one-shot -- no re-assert.
// Package-independent.
// ============================================================================

namespace {

    class WeaponDrawChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "weapon-draw"; }
        int         ChannelNo() const override { return 4; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4C, "Numpad5 : toggle weapon drawn/sheathed" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.4] REFUSED -- no gated target."); return; }
            target->DrawWeaponMagicHands(true);
            engaged.store(true);
            spdlog::info("[ch.4] engaged on 0x{:08X} -- DrawWeaponMagicHands(true) (one-shot, sticky).",
                         target->GetFormID());
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor) actor->DrawWeaponMagicHands(false);
            spdlog::info("[ch.4] released -- DrawWeaponMagicHands(false).");
        }
    };

}

APMF_REGISTER_CHANNEL(WeaponDrawChannel);
