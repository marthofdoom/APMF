#pragma once

#include <cstdint>

// ============================================================================
// THE HELD ENGAGEMENT WATCHDOG's activity reader (channels/CombatEntry.cpp Poll, every 250 ms): did the actor START or
// RELEASE an attack, or start a cast, since the last sample?
//
// PURE LOGIC (tools/hold_activity/hold_activity_test.cpp). An attack state that never reads None is not idleness: an
// archer who draws again inside 250 ms, or a chain of swings, stays non-None for a whole fight (field 2026-11-25: 4
// shots landed, "attacks 0", dropped after 20 s). A bow HELD drawn, or a state that does not change, is not activity:
// a fighter who never looses still ends.
// To add a kind of attack: GroupOf + one branch of Step.
// ============================================================================
namespace apmf::holdlogic {

    // RE::ATTACK_STATE_ENUM's values (asserted in channels/CombatEntry.cpp).
    enum : std::uint32_t {
        kAtkNone = 0, kAtkDraw = 1, kAtkFollowThrough = 5, kAtkBash = 6,
        kBowDraw = 8, kBowDrawn = 10, kBowReleasing = 11, kBowFollowThrough = 14,
        kFire = 15, kFired = 17,
    };

    enum class Group : std::uint8_t { kNone, kMelee, kBash, kBow, kFire };
    constexpr Group GroupOf(std::uint32_t a_s) {
        return a_s >= kAtkDraw && a_s <= kAtkFollowThrough ? Group::kMelee
               : a_s == kAtkBash                           ? Group::kBash
               : a_s >= kBowDraw && a_s <= kBowFollowThrough ? Group::kBow
               : a_s >= kFire && a_s <= kFired             ? Group::kFire
                                                           : Group::kNone;
    }

    struct Seen {
        std::uint32_t attacks = 0, casts = 0;
        constexpr bool Any() const { return attacks != 0 || casts != 0; }
    };

    class Activity {
    public:
        // `a_entry` = the actor's current attack data (an identity, never dereferenced). `a_casters` = up to eight
        // caster states, four bits each.
        Seen Step(std::uint32_t a_state, std::uintptr_t a_entry, std::uint32_t a_casters) {
            Seen       s;
            const auto g = GroupOf(a_state), was = GroupOf(m_state);
            if (g != Group::kNone) {
                if (g != was) {
                    ++s.attacks;   // a swing, a bash, a draw
                } else if (a_state < m_state) {
                    ++s.attacks;   // back to an earlier stage: the next swing of a chain, the next draw
                } else if (g == Group::kBow && m_state <= kBowDrawn && a_state >= kBowReleasing) {
                    ++s.attacks;   // the arrow is loosed
                } else if (g == Group::kFire && m_state < kFired && a_state == kFired) {
                    ++s.attacks;
                } else if (g == Group::kMelee && a_entry != 0 && a_entry != m_entry) {
                    ++s.attacks;   // a chained swing: another attack entry without a state step back
                }
            }
            for (std::uint32_t i = 0; i < 8; ++i) {
                const std::uint32_t c = (a_casters >> (i * 4)) & 0xF, w = (m_casters >> (i * 4)) & 0xF;
                if (c != 0 && (w == 0 || c < w)) ++s.casts;   // a new cast, also one that never read None in between
            }
            m_state = a_state; m_entry = a_entry; m_casters = a_casters;
            return s;
        }

    private:
        std::uint32_t  m_state = 0, m_casters = 0;
        std::uintptr_t m_entry = 0;
    };

}
