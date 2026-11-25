#pragma once

#include "APMF_API.h"

#include <cstdint>

// The row equip admission's decision (core/EquipGate.cpp): which of the engine's own reasons keeps its NO for a claim's
// driven form. PURE LOGIC (tools/row_equip/row_equip_test.cpp).
namespace apmf::rowequip {

    // Ward and Bound Item: cast on the caster himself, armed apart from the v46 hostile rows.
    inline constexpr std::uint32_t kOwnRows = APMF_API::kCastSeat_Ward | APMF_API::kCastSeat_BoundItem;
    constexpr bool OwnRow(std::uint32_t a_row) { return (a_row & kOwnRows) != 0; }

    // The engine equips a ward only ABOVE this share of full Magicka (AE 0x81ED63 comiss / jbe; no permanent Magicka
    // reads 1.0, AE 0x666FAB).
    constexpr float MagickaShare(float a_now, float a_permanent) { return a_permanent != 0.0f ? a_now / a_permanent : 1.0f; }
    constexpr bool  WardTooLow(float a_share, float a_limit) { return !(a_share > a_limit); }

    enum class Keep : std::uint8_t { kAdmit, kFleeing, kApplied, kWardFloor, kBroke };
    constexpr Keep Decide(std::uint32_t a_row, bool a_fleeing, bool a_onTarget, bool a_onCaster, bool a_wardLow, bool a_broke) {
        if (a_fleeing) return Keep::kFleeing;
        if (a_onTarget || (OwnRow(a_row) && a_onCaster)) return Keep::kApplied;
        if (a_row == APMF_API::kCastSeat_Ward && a_wardLow) return Keep::kWardFloor;
        if (a_broke) return Keep::kBroke;
        return Keep::kAdmit;
    }

}
