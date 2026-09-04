#pragma once
#include <chrono>
#include <cstdint>

// ============================================================================
// APMF core -- the neutral monotonic clock. A single steady_clock-based
// millisecond tick shared by anything that needs a wall-independent deadline or
// timeline: the ch.8b cast-claim TTL (core/ControlMap Drain), the cast-path
// observer (core/CastObserve), and the package-drift diagnostics
// (core/NonAliasProbe). Moved OUT of NonAliasProbe (design.md §3.4) so a channel
// / the control map never has to depend on a probe file just for a timestamp.
//
// steady_clock (NOT system_clock): monotonic, never jumps on a wall-clock or DST
// change -- correct for TTL deadlines and relative timelines. Header-only + inline
// so every TU shares one definition with zero link surface.
// ============================================================================

namespace apmf::clock {

    inline std::uint64_t MonotonicMs() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

}
