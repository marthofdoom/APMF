#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// ============================================================================
// ch.25 SELF BLAST, PARTY HOLD (kCastGuard_SelfBlastParty / kCastGuard_SelfBlastSummons): the rule.
//
// PURE LOGIC, std only, no engine type, so tools/self_blast/self_blast_test.cpp runs exactly this
// code. core/SelfBlast.cpp reads the engine facts, core/CastGuard.cpp measures and asks.
//
// WHERE THE ENGINE TAKES THE RADIUS (disassembled, 1.6.1170 / 1.5.97 / 1.7.104; evidence in MAP.md
// `core/SelfBlast`): MagicCaster::FindTargets on a Self-delivered spell gathers every living actor
// but the caster whose ORIGIN is nearer to the blast point than EFIT area x fMagicUnitsPerFoot (a
// game setting, 22 by default, NOT 64/3), each effect against its own area, and an MGEF with the
// No Area flag has none. The blast point is the caster's magic node, EXCEPT that when the costliest effect
// has an explosion the Self branch moves it to the caster's ORIGIN + his height x that explosion's
// vertical offset mult (1.6.1170 0x5BC9F5, 1.5.97 0x54D5D3, 1.7.104 0x5CB599). A wall between the point and
// the actor's mid height excludes it unless the SPELL ignores line of sight. An MGEF explosion
// is placed at the same point with its record radius, or EFIT area x fMagicUnitsPerFoot when the effect has an
// area (1.6.1170 0x7CF2CA), x the dual-cast scale; it hits on its own only when it
// carries damage, an enchantment, or is a CHAIN explosion (which replaces the area pass).
// ============================================================================
namespace apmf::selfblast {

    // Past the radius a party member still counts: the engine's centre is the magic node, not the caster's
    // origin (an arm's reach up and forward, under ~120u), plus a frame or two of running.
    inline constexpr float kPartyPad = 160.0f;

    // No explosion point: the engine's blast point stays at the magic node.
    inline constexpr float kNoPoint = std::numeric_limits<float>::quiet_NaN();

    // The radius of an effect's own explosion (F2): the engine overrides the record radius with the effect's area when
    // it has one, scaled; the larger of the two is taken. A scale under 1 is not believed.
    inline float ExplosionUnits(float a_record, std::uint32_t a_areaFeet, bool a_noArea, float a_unitsPerFoot, float a_scale) {
        if (!std::isfinite(a_record) || !std::isfinite(a_scale)) return std::numeric_limits<float>::infinity();
        const float area = a_noArea || a_areaFeet == 0 ? 0.0f : static_cast<float>(a_areaFeet) * a_unitsPerFoot;
        return (a_record > area ? a_record : area) * (a_scale > 1.0f ? a_scale : 1.0f);
    }

    struct EffectFacts {
        bool          hostile        = false;   // MGEF kHostile
        bool          detrimental    = false;   // MGEF kDetrimental
        bool          noArea         = false;   // MGEF kNoArea: the engine reads its area as 0
        std::uint32_t areaFeet       = 0;       // EFIT area
        bool          explosionHarms = false;   // its explosion carries damage or an enchantment, or is a chain
        float         explosionUnits = 0.0f;    // that explosion's radius (ExplosionUnits)
        bool          hasExplosion   = false;   // the MGEF names an explosion, harmful or not: it moves the blast point
        float         explosionMult  = 0.0f;    // that explosion's vertical offset mult
    };

    struct Blast {
        float radius       = 0.0f;    // game units round the caster; 0 = not a harmful self blast
        bool  throughWalls = false;   // a wall does not save anybody from it
        bool  hasPoint     = false;   // the engine may blast from origin + height x pointMult instead of the magic node
        float pointMult    = 0.0f;
    };

    // The blast of a SELF-delivered item. A harmful explosion's own wall rule is not relied on: such a
    // blast reads throughWalls (the safe side).
    inline Blast BlastOf(const EffectFacts* a_e, std::size_t a_n, float a_unitsPerFoot, bool a_spellIgnoresLos) {
        Blast b;
        bool  harmful = false;
        for (std::size_t i = 0; i < a_n; ++i) harmful |= a_e[i].hostile || a_e[i].explosionHarms;
        if (!harmful) return b;
        b.throughWalls = a_spellIgnoresLos;
        const auto grow = [&b](float r) {
            if (!std::isfinite(r)) r = std::numeric_limits<float>::infinity();   // unreadable: never small
            if (r > b.radius) b.radius = r;
        };
        for (std::size_t i = 0; i < a_n; ++i) {
            const EffectFacts& e = a_e[i];
            if ((e.hostile || e.detrimental) && !e.noArea && e.areaFeet != 0)
                grow(static_cast<float>(e.areaFeet) * a_unitsPerFoot);
            if (e.explosionHarms) {
                grow(e.explosionUnits);
                b.throughWalls = true;
            }
            // Which effect is the costliest is the engine's call, so every explosion counts: two that disagree, or a
            // mult that is not a number, leave the point unknown and no wall is believed.
            if (e.hasExplosion) {
                if (!std::isfinite(e.explosionMult) || (b.hasPoint && b.pointMult != e.explosionMult)) b.throughWalls = true;
                b.hasPoint  = true;
                b.pointMult = e.explosionMult;
            }
        }
        return b;
    }

    // Who the hold protects. Independent of kCastGuard_Bystanders' own set, which excludes all of these.
    // a_commandedByParty: commanded by the caster, the player or a teammate.
    constexpr bool CountsAsParty(bool a_player, bool a_teammate, bool a_commandedByParty, bool a_ownedByPlayer,
                                 bool a_summons) {
        return a_player || a_teammate || (a_summons && (a_commandedByParty || a_ownedByPlayer));
    }

    // One caster's published measurement, as a seat reads it.
    struct Reading {
        bool          found          = false;   // a consistent slot was read
        bool          party          = false;   // the party was measured
        bool          summons        = false;   // ... with summons and horses counted
        std::uint64_t measuredMs     = 0;
        std::uint32_t anyId          = 0;       // the nearest member, walls ignored (0 = nobody)
        float         anyDist        = std::numeric_limits<float>::infinity();
        std::uint32_t openId         = 0;       // the nearest member no wall was PROVEN to shelter
        float         openDist       = std::numeric_limits<float>::infinity();
        float         points[2]      = { kNoPoint, kNoPoint };   // the explosion points the wall rays also started at (mults)
    };

    // Was the wall test taken from this blast's own point too? A blast no claim named (or one claimed since) was not:
    // then no wall is believed for it.
    inline bool WallsTested(const Blast& a_b, const Reading& a_r) {
        return !a_b.hasPoint || a_r.points[0] == a_b.pointMult || a_r.points[1] == a_b.pointMult;
    }
    inline bool ThroughWalls(const Blast& a_b, const Reading& a_r) { return a_b.throughWalls || !WallsTested(a_b, a_r); }

    enum class Answer { kClear, kHold, kUnknown };

    inline Answer Judge(const Blast& a_b, const Reading& a_r, bool a_wantSummons, std::uint64_t a_now,
                        std::uint64_t a_freshMs) {
        if (a_b.radius <= 0.0f) return Answer::kClear;
        if (!a_r.found || !a_r.party || (a_wantSummons && !a_r.summons) || a_r.measuredMs == 0 ||
            a_now < a_r.measuredMs || a_now - a_r.measuredMs > a_freshMs)
            return Answer::kUnknown;
        const bool          any = ThroughWalls(a_b, a_r);
        const std::uint32_t id  = any ? a_r.anyId : a_r.openId;
        const float         d   = any ? a_r.anyDist : a_r.openDist;
        return id != 0 && !(d > a_b.radius + kPartyPad) ? Answer::kHold : Answer::kClear;   // a NaN holds
    }

    // As a seat asks it: the reading is taken FIRST, the clock after it (F3). The other way round a publish in between
    // makes the reading newer than `now`, which reads Unknown and cuts a cast at 0x0A.
    template <class ReadFn, class ClockFn>
    Answer JudgeNow(const Blast& a_b, ReadFn&& a_read, ClockFn&& a_clock, bool a_wantSummons, std::uint64_t a_freshMs,
                    Reading& a_r, std::uint64_t& a_now) {
        a_r   = a_read();
        a_now = a_clock();
        return Judge(a_b, a_r, a_wantSummons, a_now, a_freshMs);
    }

    // Fold one measured member in. a_sheltered: a wall was proven between (a distance that is not a number is 0).
    inline void Fold(Reading& a_r, std::uint32_t a_id, float a_dist, bool a_sheltered) {
        if (!std::isfinite(a_dist)) a_dist = 0.0f;
        if (a_dist < a_r.anyDist) {
            a_r.anyDist = a_dist;
            a_r.anyId   = a_id;
        }
        if (!a_sheltered && a_dist < a_r.openDist) {
            a_r.openDist = a_dist;
            a_r.openId   = a_id;
        }
    }

    // ---- the engine half (core/SelfBlast.cpp) ----
}

namespace RE {
    class Actor;
    class MagicItem;
}

namespace apmf::selfblast {
    // kDataLoaded, MAIN thread: finds fMagicUnitsPerFoot by name. nullptr = served, else why not.
    const char* Install();
    const char* NotArmedReason();   // any thread
    // Any thread (immutable form data and one game setting float). a_dual: the dual-cast explosion and scale.
    Blast BlastOfItem(RE::MagicItem* a_item, bool a_dual);
    // MAIN thread: is world geometry PROVEN to stand between the caster's blast and a_other? The engine's own
    // ray (layer SPELLEXPLOSION, the target's collision group, to mid height) from three points round the magic
    // node and from each explosion point in a_points (origin + height x mult; NaN = none); true only when all were
    // cast and met world geometry. Anything else is false: it counts.
    bool WallBetween(RE::Actor* a_caster, RE::Actor* a_other, const float (&a_points)[2]);
}
