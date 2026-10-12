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
// actor whose winning live guard claim carries kCastGuard_LineOfFire (at most
// kMaxMeasuresPerFrame per frame, round robin): the aim (the target of the actor's live
// driving cast claim, else its combat target), then every loaded actor near the line from
// the caster to the aim (within kLinePad, projecting between the two) or near the aim. A
// FRIENDLY is an actor that is not hostile to the caster (the engine's own
// Actor::IsHostileToActor, the verified SpaceQuery.IsHostileToActor row) and, for a caster
// that is a player teammate, also the player and every teammate. A friendly near the LINE
// counts only while Harbinger's own ray (core/Sightline.h Read) does not read it OCCLUDED
// from the caster -- a wall between them means the bolt cannot reach it; an unknown or
// unavailable reading counts it (never clear by fallback, principle 7). A friendly near the
// AIM counts against a spell whose largest effect area + kBlastPad reaches it; that radius
// is spell-specific, so the seat compares it. The result is published per caster in a
// seqlock slot table: readers on any thread are lock-free.
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
    inline constexpr float         kBlastPad            = 64.0f;   // added to the spell's largest area
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

}
