#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the ABI v18 AWARENESS query, SenseActor: "does this actor SENSE that
// one?", for an engage-on-sight gambit (marth 2026-10-05: "We also want harbinger line
// of sight for attack on sight, its currently using what seems really bad, and
// directional. we want the appearance of more senses than sight.").
//
// The contract (senses, defaults, statuses, struct layouts) is APMF_API.h's "ABI v18:
// LINE OF SIGHT AND AWARENESS" section. This file is the implementation. It ANSWERS and
// changes nothing: no facet, no claim, no handle, no engine write.
//
// THE SENSES AND THEIR ENGINE SOURCES (disassembly of all three builds, 2026-10-05):
//   SIGHT      core/Sightline.h's own ray (no view cone, no facing).
//   HEARING    (noise) the target's HighProcessData::actorsGeneratedDetectionEvent (+0x3D8),
//              a DetectionEvent {u32 level +0x00, NiPoint3 location +0x04, float AI-clock
//              stamp +0x10, ObjectRefHandle ref +0x14} (size 0x18). Its ONLY writer is
//              AIProcess::SetActorsDetectionEvent (SE 38311 0x64CDF0 / AE 39286 0x6DF9B0 /
//              1.7.104 0x6F2560): AIProcess+0x10 -> high, high+0x3D8 -> the event, allocated
//              once and then overwritten in place (freed only from AE 41406, the high
//              process teardown). Its seven callers on 1.6.1170 are the engine's noise
//              sources: MagicCaster::PlayReleaseSound (AE 34448: a spell's release, level =
//              the effect's casting sound level), the projectile code (AE 43869 launch,
//              44213 impact, 44216 actor hit), AE 34993, AE 22556 and AE 56152. The level is
//              the engine's own value (AE 0x416070 maps SOUND_LEVEL 0..3 to it).
//              NOVELTY, NOT AGE: the stamp is the engine's AI clock (AE 0x319B05C, id
//              404125), and that id is NOT in the 1.7.104 MIT id table, so Harbinger never
//              reads the clock (an absent id is fatal there). A noise is NEW when its stamp
//              differs from the one Harbinger saw last time it looked at that actor; its age
//              is measured on Harbinger's own monotonic clock from that moment.
//              (combat) Actor::IsInCombat (Character slot 0xE3: process->combat state).
//   PROXIMITY  3D distance.
//   ENGAGED    Actor::IsInCombat + the target's currentCombatTarget (ACTOR_RUNTIME_DATA,
//              resolved through the handle table: CommonLib.LookupReferenceByHandle row).
// Considered and NOT used: the engine's detection (Actor::RequestDetectionLevel, SE 36748 /
// AE 37764) -- its sight half is the facing, cone-limited test marth asked to replace, and
// it computes detection state on the call.
//
// THREADING. MAIN THREAD ONLY (apmf::hook::OnMainThread), like the v11 space queries: the
// sight ray needs it. The noise read is a plain member read of the target's live process
// (process -> high -> event pointer -> 0x18 bytes copied out at once); the event object
// is freed only when the engine tears the high process down (AE 41406), and that is NOT
// proven to run on the main thread only -- the same exposure every main-thread read of
// another actor's high process has (MAP.md "What breaks"). The noise baselines live in a
// main-thread map (no lock). Cost per call: at most 3 rays (none when the pair's verdict
// is younger than kLosRefreshMs) plus member reads.
//
// INI: Data/SKSE/Plugins/APMF.ini [Awareness] bHearNoise (default 1) -- 0 turns OFF the
// NOISE route only (the +0x3D8 read); kAwareDetail_NoiseRouteOff marks every answer then.
// The whole query follows the line-of-sight service: not armed -> kQuery_Unsupported.
//
// PASSIVE PROBE (principle 5: observe before relying). Always-on, rate-limited:
//   [aware] noise    -- each NEW noise Harbinger sees on a queried actor (<= 1 per actor
//                       per 2 s): level, position, the event's ref
//   [aware] senses   -- a (viewer, target) pair's sense set CHANGED
//   [aware] heartbeat -- every 30 s while queried: per-sense fire counts
// ============================================================================

namespace apmf::awareness {

    // kDataLoaded, AFTER sightline::Install(). Idempotent. Logs once.
    void Install();

    // The C-ABI SenseActor body (ClientAPI wraps it in a catch-all). MAIN THREAD ONLY.
    std::uint32_t SenseActor(const APMF_API::APMF_AwarenessQuery* a_q, APMF_API::APMF_AwarenessResult* a_out);

    // Game thread: kPreLoadGame / revert. Drops the noise baselines and the probe state.
    void ResetAll(const char* a_why);

}
