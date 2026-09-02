#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 6 -- COMBAT-TARGET, STEER ONLY (CHANNEL-MAP ch.6: "steer DOCUMENTED; PIN
// GAP"). Seed the actor's combat with a chosen target via Actor::StartCombat
// (Address-Library bound, unbound in CommonLib -- INVARIANTS #8). This STEERS the
// target; it does NOT PIN it (the threat re-selector can still re-choose each tick
// -- the PIN is a probe-gated GAP, NOT built here). Release stops combat via the
// bound StopCombat() vfunc (0xE5).
//
// Target: the client's chosen actor via RequestEx's APMF_Param.form (a v2 addition).
// With NO param (form == 0, e.g. the NumpadMinus test hotkey) we fall back to the
// PLAYER as a self-evident demo target.
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
                { 0x4A, "NumpadMinus : STEER combat target to the player (no-param demo)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor || REL::Module::IsVR()) return;   // reloc IDs are SE/AE only
            RE::Actor* target = param.form ? RE::TESForm::LookupByID<RE::Actor>(param.form) : nullptr;
            if (!target) target = RE::PlayerCharacter::GetSingleton();   // no-param fallback: the player
            if (!target) return;
            Native::StartCombat(actor, target);
            spdlog::info("[ch.6] 0x{} STEER -- StartCombat(0x{}). Steers, does NOT pin (re-selector "
                         "may re-choose; PIN is a GAP).", apmf::log::Hex(id), apmf::log::Hex(target->GetFormID()));
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (!actor) return;
            actor->StopCombat();
            spdlog::info("[ch.6] 0x{} StopCombat().", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(CombatTargetChannel);
