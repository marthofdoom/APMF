#pragma once

#include <cstdint>

// ============================================================================
// NO NEW MELEE ATTACK WHILE A STRICT ORDER TRAVELS (marth 2026-11-25): the decision core.
// PURE LOGIC (tools/move_order/move_order_test.cpp). core/MoveSuppress.cpp owns the table and the log.
// The tunables: SuppressTuning.
// ============================================================================
namespace apmf::movelogic {

    // Labelled placeholders until a field session is read.
    struct SuppressTuning {
        static constexpr std::uint64_t kFreshMs  = 1000;    // no pick for this long: not suppressed (the selector re-picks every 0.25 s)
        static constexpr std::uint64_t kStallMs  = 2500;    // travelling and not moved kStallMove: he may fight
        static constexpr float         kStallMove = 48.0f;
        static constexpr std::uint64_t kLegMs    = 15000;   // the longest one suppressed leg
        static constexpr std::uint64_t kRestMs   = 4000;    // after a stall or a full leg: attacks allowed this long
    };

    enum class SuppressEnd : std::uint8_t { kNone, kStall, kLeg };

    // The attack leaves' kinds (castguard::kWeaponMelee / kWeaponRanged, asserted in core/MoveSuppress.cpp).
    inline constexpr std::uint8_t kLeafMelee = 1, kLeafRanged = 2;
    // Is this leaf a MELEE attack for this fighter? A leaf of both kinds (attack from cover) is what his weapon is.
    constexpr bool MeleeLeaf(std::uint8_t a_kind, bool a_rangedWeapon) {
        return (a_kind & kLeafMelee) != 0 && ((a_kind & kLeafRanged) == 0 || !a_rangedWeapon);
    }

    // One per ACTOR, not per order: a client that re-points every 0.3 s must not restart the bounds.
    struct SuppressClock {
        std::uint64_t legMs = 0, anchorMs = 0, restUntilMs = 0, lastPickMs = 0;
        float         x = 0.0f, y = 0.0f, z = 0.0f;

        // One movement pick. `a_travel` = this pick sends him to the point (Travelling or Returning), `a_stood` = he
        // stands on it. Returns: new melee attacks are refused until the next pick.
        bool Pick(bool a_travel, bool a_stood, float a_x, float a_y, float a_z, std::uint64_t a_now, SuppressEnd& a_end) {
            a_end = SuppressEnd::kNone;
            const bool gap = lastPickMs != 0 && a_now - lastPickMs > SuppressTuning::kFreshMs;
            lastPickMs = a_now;
            if (a_stood || gap) legMs = 0;
            if (a_stood) restUntilMs = 0;
            if (!a_travel) { anchorMs = a_now; return false; }   // a wait (NotCarried, Yielded, down) is not a stall; the 15 s leg bound still runs
            if (a_now < restUntilMs) return false;
            const float dx = a_x - x, dy = a_y - y, dz = a_z - z;
            if (legMs == 0) legMs = a_now, anchorMs = a_now, x = a_x, y = a_y, z = a_z;
            else if (dx * dx + dy * dy + dz * dz >= SuppressTuning::kStallMove * SuppressTuning::kStallMove)
                anchorMs = a_now, x = a_x, y = a_y, z = a_z;
            if (a_now - anchorMs > SuppressTuning::kStallMs) a_end = SuppressEnd::kStall;
            else if (a_now - legMs > SuppressTuning::kLegMs) a_end = SuppressEnd::kLeg;
            if (a_end == SuppressEnd::kNone) return true;
            legMs       = 0;
            restUntilMs = a_now + SuppressTuning::kRestMs;
            return false;
        }
    };

}
