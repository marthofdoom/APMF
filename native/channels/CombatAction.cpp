#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 7 -- COMBAT ACTIONS. Arbitration + claim lifecycle ONLY here; the
// real ENFORCEMENT lives one layer down, in core/ActionGate.cpp (T1, the
// combat behavior-tree leaf deny -- Docs/ALLOWANCE-TEMPLATE.md §3/§7,
// graduated 2026-09-03 from the field-proven T1Probe, Docs/PROBE-ALLOWANCE.md).
//
// APMF MODERATES; it does NOT generate behavior. A client claims this facet
// naming which action CATEGORY to deny (APMF_Param::ival, an
// APMF_API::CombatActionCategory bitmask -- start with kCombatActionCat_Offense).
// ActionGate.cpp denies exactly the leaves classified into a set category for
// the winning claim's actor; everything else (movement, block, dodge, cover,
// search, the selector nodes, ...) keeps firing natively -- this channel
// itself makes NO engine write, exactly like ch.8/ch.6/ch.14's arbitration-
// only shape. Engage/OnOwnerChanged/Release only log the claim lifecycle.
// ============================================================================

namespace {

    class CombatActionChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-action"; }
        int              ChannelNo() const override { return 7; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatAction; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x9C, "NumpadEnter : CLAIM the combat-action facet (arbitration only; the test surface "
                        "carries no category, so a test claim denies nothing -- see APMF_RequestEx)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.7] 0x{} combat-action facet CLAIMED (deny mask 0x{}). Arbitration only -- "
                         "core/ActionGate.cpp's T1 leaf hook is what actually denies the named categories.",
                         apmf::log::Hex(id), apmf::log::Hex(static_cast<std::uint32_t>(param.ival), 2));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.7] 0x{} combat-action claim RE-POINTED (deny mask 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(static_cast<std::uint32_t>(param.ival), 2));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.7] 0x{} combat-action facet released.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(CombatActionChannel);
