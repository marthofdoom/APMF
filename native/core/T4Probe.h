#pragma once

// ============================================================================
// T4 PROBE -- `TESActionData::Process` body-command seat.
// Docs/ALLOWANCE-TEMPLATE.md §3 T4 row / §6 3-cycle probe step 2.
// THROWAWAY; INSTRUMENTATION ONLY -- observe-only. NOT wired to any client,
// NOT a permanent channel. See Docs/PROBE-ALLOWANCE.md for the hotkey map
// and pass/fail criteria.
//
// Two questions, in order:
//   1. Is `TESActionData::Process` (vtable slot 5, `VTABLE_TESActionData[0]`)
//      reached VIRTUALLY, or does valhallaCombat's own known call site
//      (`RELOCATION_ID(48139,49170)+0x4D7/0x435`) devirtualise straight to
//      the callee? Resolved at install time by reading the rel32 byte at
//      that call site and comparing the computed target against the actual
//      function pointer STORED in `VTABLE_TESActionData[0]`'s slot-5 cell
//      (not the vtable's own address -- the value AT that cell).
//   2. Whichever seat wins, hook it OBSERVE-only and log, for a claimed
//      actor, `BGSAction` editorID + FormID, the raw ActionInput "priority"
//      field (CommonLib ships this as an unreversed `uint32_t unk20` at
//      +0x20 -- ALLOWANCE-TEMPLATE.md's `Priority{kImperative,kQueue,kTry}`
//      claim is UNCONFIRMED against any header; logged raw for the field
//      run to correlate against observed behaviour), and `source` FormID --
//      across combat/sandbox/dialogue/player-command, to measure coverage.
// ============================================================================

namespace apmf::t4probe {

    // Read the call-site rel32, compare to the vtable slot, install whichever
    // seat wins (once). VR-refused (RELOCATION_ID + VTABLE index are SE/AE-only).
    void Install();

    // Route a test-surface hotkey. No-op unless armed + the T4 scancode.
    void OnHotkey(std::uint32_t a_code);

    // Game-thread pump: periodic (~5s) OBSERVE census. Call once per frame
    // (Arbiter::OncePerFrame). No-op when disarmed.
    void OncePerFrame();

    // Drop the claim without touching the engine (kPreLoadGame).
    void ClearOnPreLoad();

}
