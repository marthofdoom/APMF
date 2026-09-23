#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the ABI v11 SPACE QUERIES (read-only).
//
//   FindEmptySpace       "where is a standable, empty spot N units that way?"
//   FindHostilesInSpace  "which live actors inside this sphere are hostile to X?"
//
// Both ANSWER and change nothing: no facet, no claim, no handle, no engine write.
// The contracts (inputs, outputs, statuses, costs) are in APMF_API.h's
// "ABI v11: SPACE QUERIES" section; this file is the implementation behind it.
//
// THREADING: synchronous, TRUE MAIN THREAD ONLY (apmf::hook::OnMainThread(), the
// player-Update thread). Anything else returns kQuery_NotMainThread without doing
// any work. Havok ray casts run under the world's read lock, the way the engine's
// own callers and MFO's field-proven sight check do it.
//
// RUNTIME: the pick vfunc (bhkWorld 0x33), the world scale and the hostility test
// were read on 1.6.1170 and 1.5.97 (Docs/ADDRESS-TABLE-2026-09-15.md, ADDENDUM
// 2026-09-23). Any other runtime, and VR, answers kQuery_Unsupported.
// ============================================================================

namespace apmf::spacequery {

    // kDataLoaded: arm the queries on a verified runtime and log it once. Idempotent.
    void Install();

    std::uint32_t FindEmptySpace(const APMF_API::APMF_SpaceQuery* a_query, APMF_API::APMF_SpaceResult* a_out);

    std::uint32_t FindHostilesInSpace(const APMF_API::APMF_HostileQuery* a_query, RE::FormID* a_outActors,
                                      std::uint32_t a_capacity, std::uint32_t* a_outCount,
                                      std::uint32_t* a_outTotal);

}
