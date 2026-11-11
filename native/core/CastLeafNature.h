#pragma once

#include "APMF_API.h"

#include <cstdint>
#include <string_view>

// ch.7 HOSTILE OFFENSE (ABI v40, marth 2026-11-11: "wouldn't it be non offense cast"). kCombatActionCat_Offense
// means HOSTILE actions. The four cast leaves carry the Offense bit only for a cast that is hostile: this file
// answers, at the leaf's act(), "is the spell this leaf is about to cast declared non-hostile?" from the spell
// record the leaf itself reads a few instructions later. See CastLeafNature.cpp for the engine path, the
// per-runtime proof and the nature rule. kCombatActionCat_Cast is untouched: it denies every cast.
namespace apmf::castleafnature {

    enum class LeafKind : std::uint8_t {
        kNone  = 0,   // not a cast leaf, or its read is not proven on this runtime: the Offense bit stands for every cast
        kSpell = 1,   // CastImmediateSpell / CastConcentrationSpell / PrepareDualCast: MagicItem* at context+0x08
        kShout = 2,   // CastShout: TESShout* at context+0x20
    };

    // Game thread, once, from actiongate::Install before it classifies the leaves. Per runtime (1.6.1170, 1.5.97,
    // 1.7.104, each with its own ids / raw RVAs, offsets and bytes): the verified-address rows of the six engine
    // functions and the byte windows that hold the reads. A leaf whose proof fails keeps the pre-v40 behaviour
    // (every cast of that leaf counts as Offense), named in the log. INI [CombatAction] bHostileOnlyOffense=0 = all off.
    void Install();

    // The proven kind of a cast leaf, by its catalog name ("CombatBehaviorCastImmediateSpell" ...). kNone before
    // Install, for a non-cast leaf and for a leaf whose proof was refused. Game thread (actiongate::Install).
    [[nodiscard]] LeafKind ProvenKind(std::string_view a_leafName);

    // COMBAT THREAD (the leaf's act(), before the leaf's own body). True only when the spell (or shout) the leaf
    // is about to cast is DECLARED non-hostile, so an Offense-only deny does not apply to it. False = it counts
    // as Offense: a hostile spell, an undeclared nature, or anything unreadable (logged, rate-limited). Reads the
    // thread's context window and immutable form data only. Never throws into the engine.
    [[nodiscard]] bool PassesOffenseDeny(LeafKind a_kind, void* a_thread, RE::FormID a_actor,
                                         APMF_API::Handle a_claim) noexcept;

    // RULE C heartbeat (game thread, ~30 s while any verdict was given since the last line; zeros included).
    void Heartbeat();
}
