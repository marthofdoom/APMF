#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 6 -- COMBAT-TARGET, STEER ONLY (CHANNEL-MAP ch.6: "steer DOCUMENTED; PIN
// GAP"). Seed the actor's combat with a chosen target via Actor::StartCombat
// (Address-Library bound, unbound in CommonLib -- INVARIANTS #8). This STEERS the
// target; it does NOT PIN it (the threat re-selector can still re-choose each tick
// -- the PIN is a probe-gated GAP, NOT built here). Release stops combat via the
// bound StopCombat() vfunc (0xE5).
//
// Test facet: make her fight the PLAYER (a self-evident demo target -- the API's
// per-request target form is a v2 addition; for now the demo target is the player).
// VR-refused (the StartCombat reloc IDs are SE/AE only).
// ============================================================================

namespace {

    namespace Native {
        // Actor::StartCombat(target) -- seed combat against a_target.
        void StartCombat(RE::Actor* a_actor, RE::Actor* a_target) {
            using func_t = void (*)(RE::Actor*, RE::Actor*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(37608, 38561) };
            func(a_actor, a_target);
        }
    }

    class CombatTargetChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-target"; }
        int              ChannelNo() const override { return 6; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatTarget; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4A, "NumpadMinus : STEER combat target to the player (demo)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor || REL::Module::IsVR()) return;   // reloc IDs are SE/AE only
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            Native::StartCombat(actor, player);
            spdlog::info("[ch.6] 0x{:08X} STEER -- StartCombat(player). Steers, does NOT pin (re-selector "
                         "may re-choose; PIN is a GAP).", actor->GetFormID());
        }

        void Release(RE::Actor* actor) override {
            if (!actor) return;
            actor->StopCombat();
            spdlog::info("[ch.6] 0x{:08X} StopCombat().", actor->GetFormID());
        }
    };

}

APMF_REGISTER_CHANNEL(CombatTargetChannel);
