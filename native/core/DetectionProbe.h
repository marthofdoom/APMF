#pragma once

// ============================================================================
// core/DetectionProbe -- HARBINGER 1a, the DETECTION facet: census + PASSIVE PROBE of the
// detection-value call site (the SmartNPCTargetSelector "H2" shape, _research/
// smartnpc-targetselector-interop-2026-10-05.md section 1).
//
// THE SITE. One rel32 call per runtime into the per-(source, target) detection-level
// routine: AE 42742 + 0x67B (0x7B0D8B) -> 37774 (0x690870); SE 41659 + 0x526 (0x719EB6) ->
// 36758 (0x5FD870); each callee has exactly this one caller on its runtime (full E8/E9
// scan of both unpacked images, 2026-10-11). Ten pointer arguments (rcx = the detecting
// actor, rdx = the detected actor, r8 = int* detection value, r9 and six stack slots =
// out-pointers), the return value is unused by the caller (both runtimes: the caller
// reloads its locals right after the call). SmartNPCTargetSelector write_call<5>-patches
// this E8 at SKSEPluginLoad and forces the value to +1000 / -1000 for its "focus".
//
// marth Q2: the detection override is "arbitrated by priority like everything else". That
// needs this seat, and a seat is built only after it is OBSERVED running (principle 5):
// the caller function and thread are not established. So 1a installs a PASSIVE chaining
// patch on top (it calls whatever is there -- the engine routine, or SmartNPC's patch --
// and returns its result unchanged), census-registers the module it found there, counts
// calls, and for an actor holding a target pin logs the value produced for the PIN and
// how often other targets came out at the +/-1000 sentinels. Enforcement is Phase 1a.2,
// built on what this log shows.
//
// THREADING. The thunk runs on whatever thread the engine detects on: atomics, the
// lock-free ControlMap snapshot, rate-limited spdlog. A private SKSE::Trampoline (never
// SKSE::AllocTrampoline again: its set_trampoline would release core/EquipSink's stubs),
// deliberately leaked so no exit-time free can strand a running call.
// ============================================================================

namespace apmf::detectionprobe {

    // kDataLoaded. Exact 1.6.1170 / 1.5.97 / 1.7.104, VR refused, the call-site row
    // self-checked (E8 byte), [Harbinger] bDetectionProbe (default 1).
    void Install();

    // Main thread, once per frame: the 60 s heartbeat (zeros included).
    void Heartbeat();

}
