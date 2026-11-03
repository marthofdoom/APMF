#pragma once
#include "APMF_API.h"
#include "core/Allowance.h"   // allowance::Hand

// ============================================================================
// APMF core -- ch.25 CAST GUARD (kIntent_CastGuard, ABI v21, 2026-10-12).
//
// Two per-actor guards a client puts on an NPC's OWN casts, enforced at the cast seats
// Harbinger already owns. No new hook, no new engine address:
//
//   kCastGuard_LineOfFire     HOLD an OFFENSIVE cast while a FRIENDLY stands in its line of
//                             fire (MFO's friendly-fire hold, CasterConsent.cpp 0x0A, moved
//                             under Harbinger). Seat 0x06 (CheckStartCast, core/CastSeats.cpp,
//                             combat thread, advisory) answers NO unless a fresh measurement
//                             says the line is clear; seat 0x0A (CheckCast on the LEFT / RIGHT
//                             hand casters, core/CastGate.cpp) refuses on a fresh measurement
//                             that says blocked. 0x0A is consulted by the engine at the start
//                             of a cast (ActorMagicCaster::RequestCastImpl, slot 3), at its
//                             RELEASE (StartCastImpl, slot 6) and on every tick of a channel
//                             (UpdateImpl); a NO at the release runs the engine's own
//                             cannot-cast handler, which interrupts the cast. Disassembled on
//                             all three builds: 1.6.1170 0x5B0840 / 0x5B0A90, 1.5.97 0x541A70 /
//                             0x541CB0, 1.7.104 0x5BF370 / 0x5BF5C0, each `call [vt+0x50]`.
//   kCastGuard_ChannelRight / kCastGuard_ChannelLeft
//                             REFUSE the AI's own CONCENTRATION spell on that HAND caster at
//                             0x0A (start and every channel tick), unless a live kIntent_Cast
//                             claim on that hand names that spell (the client's own claimed
//                             stream). The INSTANT caster (a client's direct stream) and the
//                             voice caster are never touched. MFO's Exact concentration bound.
//
// THE MEASUREMENT (line of fire). Pump(), on the main thread once per frame, measures every
// actor whose winning live guard claim carries kCastGuard_LineOfFire or kCastGuard_Bystanders (at most
// kMaxMeasuresPerFrame per frame, round robin): the aim (the target of the actor's live
// driving cast claim, else its combat target), then every loaded actor near the line from
// the caster to the aim (within kLinePad, projecting between the two) or near the aim. A
// FRIENDLY is an actor that is not hostile to the caster (the engine's own
// Actor::IsHostileToActor, the verified SpaceQuery.IsHostileToActor row) and, for a caster
// that is a player teammate, also the player and every teammate. A friendly near the LINE
// counts only while Harbinger's own ray (core/Sightline.h Read) does not read it OCCLUDED
// from the caster -- a wall between them means the bolt cannot reach it; an unknown or
// unavailable reading counts it (never clear by fallback, principle 7). A friendly near the
// AIM counts against a spell whose largest effect area (feet x 64/3 = units) + kBlastPad reaches it; that radius
// is spell-specific, so the seat compares it. The result is published per caster in a
// seqlock slot table: readers on any thread are lock-free.
//
// BYSTANDER HITS (kCastGuard_Bystanders, ABI v32, 2026-11-02, marth: a follower that would be
// caught committing a crime does not do it). Its own bit, so kCastGuard_LineOfFire keeps exactly
// its meaning (offensive non-Self spells, friend = not hostile, plus the party for a teammate).
// A BYSTANDER is narrower (marth "Bystanders only"): not hostile to the caster, not the player,
// not a player teammate, not commanded by the caster, the player or a teammate. Hitting the
// player or the party is not a crime. NO new hook:
//   (a) WEAPONS. HoldWeaponAttack, called from the ch.7 attack-leaf act() thunk
//       (core/ActionGate.cpp ActThunkBody, BEFORE its ch.7 category gate so the uncategorised
//       AttackFromCover leaf is covered too; the leaf is denied with the same ForceFail act()/pop()
//       PAIR a ch.7 claim uses). The aim is the actor's OWN combat target, never a cast claim's.
//       A ranged leaf (RangedAttack, AttackFromCover) is held while a bystander is within kLinePad
//       of the line to it. A melee leaf (Attack, AttackLow, Bash, SpecialAttack, AttackFromCover)
//       is held while a bystander is no farther than the target + kMeleeReachPad and inside the
//       cone of kMeleeConeCos toward it. No line test for melee: a swing does not travel.
//       Held only on a FRESH measurement, never on an unknown one: a failed measurement must not
//       freeze every swing of a follower (counted, the heartbeat's "allowed, reading unknown").
//   (b) SELF AREA SPELLS. A Self-delivered spell with a hostile effect and an area (Fire Storm
//       style), or a cloak whose associated spell is hostile with an area, is held at 0x0A / 0x06
//       while a bystander stands within area + kBlastPad of the CASTER (a fresh reading at 0x0A, an
//       unknown reading holds at 0x06 like any offensive item). A RUNE is delivered at a target
//       location, so it is a LineOfFire item (the blast test around the aim).
// Weapon holds are attributed on the CombatAction facet (the ch.7 leaves), spell holds on Cast.
//
// ATTRIBUTION (Harbinger controller 1a). The competing source is the engine's own AI
// ("Engine" on the Cast facet). A refusal stands only while the guard claim's client
// outranks it (sources::ClaimPriority vs sources::Priority(kEngineSource, kCast)); every
// would-be refusal counts a hit on Engine and an overruled or a stood (core/Sources.h).
//
// THREADING. RefuseReason/TtlMs: any thread. Pump/Install/ResetAll: MAIN thread only.
// DenyCheckCast/HoldStartCast: any thread, lock-free except the sources handle lookup
// (a shared lock, taken only on a would-be refusal).
// ============================================================================

namespace RE {
    class Actor;
    class CombatController;
    class MagicItem;
}

namespace apmf::castguard {

    inline constexpr float         kLinePad             = 80.0f;   // a friendly this close to the line is in it
    inline constexpr float         kBlastPad            = 64.0f;   // added to the spell's largest area (in units)
    inline constexpr float         kMeleeReachPad       = 48.0f;   // a swing reaches a bystander this much past the target ...
    inline constexpr float         kMeleeConeCos        = 0.819f;  // ... within 35 degrees of the direction to the target
    inline constexpr float         kUnitsPerFoot        = 64.0f / 3.0f;   // EffectItem::area is in FEET
    inline constexpr std::uint64_t kFreshMs             = 250;     // a measurement older than this is unknown
    inline constexpr std::uint32_t kMaxMeasuresPerFrame = 8;

    // kDataLoaded, MAIN thread, after sightline::Install and castgate::Install. Arms the
    // line-of-fire measurement on an exact verified runtime when the hostility row verifies.
    void        Install();
    bool        LineOfFireArmed();
    const char* NotArmedReason();

    // ControlMap::EnqueueRequest (any thread): nullptr = accept, else why it is refused.
    const char*   RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param);
    // The granted window for param.fval seconds (0 => kCastDefaultTtlMs; clamped to kCastMaxTtlMs).
    std::uint32_t TtlMs(float a_seconds);

    // MAIN thread, Arbiter::OncePerFrame (after ControlMap::Drain).
    void Pump();
    // Game thread: kPreLoadGame / revert. Drops every published measurement.
    void ResetAll(const char* a_why);

    // core/CastGate.cpp 0x0A, after the engine said YES: true = refuse this cast (the caller
    // returns false with kMultipleCast). Hand casters only; kUnknown answers false.
    bool DenyCheckCast(RE::Actor* a_actor, RE::FormID a_fid, RE::MagicItem* a_spell, allowance::Hand a_hand);
    // core/CastSeats.cpp 0x06 (combat thread): true = answer NO for this poll. The deliberating
    // actor is the controller's attacker, resolved only for an offensive item.
    bool HoldStartCast(RE::CombatController* a_cc, RE::MagicItem* a_item);
    // core/ActionGate.cpp ActThunkBody (combat thread), for an attack leaf of a_fid: true = deny the leaf
    // this tick. a_kind: kWeaponMelee (the swing arc), kWeaponRanged (the line), or both (AttackFromCover).
    inline constexpr std::uint8_t kWeaponMelee  = 1;
    inline constexpr std::uint8_t kWeaponRanged = 2;
    bool HoldWeaponAttack(RE::FormID a_fid, std::uint8_t a_kind);

}
