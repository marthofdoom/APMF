#pragma once

#include <cstdint>

namespace RE {
    using FormID = std::uint32_t;   // identical to CommonLib's alias (APMF_API.h does the same)
    class Actor;
    class AIProcess;
    class ScriptEventSourceHolder;
    class TESObjectREFR;
}

// ============================================================================
// THE SUBSTRATE PROBES (substrate round R1; PASSIVE: reads only, no write, no hook, no address id).
// Field attribution lines for ch.27's P6 (does the body move under its own package while held) and
// the panic channels: the speed / distance baseline around each window, the context of a re-pick
// that did not run, the frame of each "other" ignoring-bit reset, the aggressor bit around the
// window and how long the hands stay drawn after the close. See channels/SubstrateProbe.cpp.
// Round R5 adds (still passive): the procedure fields at each OTHER reset, every package swap while
// held, the pause / open menus at a stalled re-pick, the anchor's collision flag at the open, and the
// TESCombatEvent sink order once at kDataLoaded.
//
// [CombatSubstrate] bSubstrateProbes (default 1 while the substrate is in test). Off: every entry
// point below returns after one load.
//
// EVERYTHING HERE IS GAME THREAD: channels/CombatSubstrate.cpp calls these from its Poll seat
// (Arbiter::OncePerFrame) and from the functions Poll runs (Enter, WriteBitAfterRepick, Serve,
// CloseSub, DropSub, DryRunSchedule), plus Install and ResetAll. The probes keep their own state;
// they never read the substrate's internal Sub records or slot table.
// ============================================================================

namespace apmf::substrateprobe {

    // combatsubstrate::Install, after the substrate installed. Reads bSubstrateProbes. `a_dialogueOk`:
    // the MenuTopicManager rows verified (the dry run's dialogue test), so the re-pick context may read
    // MenuTopicManager::speaker; false = that field is logged as "unknown".
    void Install(bool a_dialogueOk);
    bool Enabled();

    // combatsubstrate::ResetAll (revert / kPreLoadGame): forget every window (the world is replaced).
    void Reset();

    // combatsubstrate::Poll, every frame, before its early return: samples the windows, runs the
    // post-close timers, writes the summary lines. One load while nothing is tracked.
    void Poll(std::uint64_t a_now);

    // DryRunSchedule, every frame while the dry run is on, BEFORE its period test: `a_dueMs` is when
    // the next pick round runs. In the last second before it the loaded player teammates are sampled
    // every 250 ms, so a pick carries a baseline from 1 s before its request.
    void BeforeDryRound(std::uint64_t a_now, std::uint64_t a_dueMs);
    // DryRunSchedule, for each pick: open a window and hand it the pre-round samples.
    void Requested(RE::FormID a_actor, std::uint64_t a_now);

    // Enter, just BEFORE StartCombat(anchor): the aggressor bit before the open. Opens the window of a
    // client substrate (its baseline starts here).
    void BeforeEntry(RE::FormID a_actor, const RE::AIProcess* a_proc, std::uint64_t a_now);
    // Enter, after a successful entry (the OPEN line). `a_anchor`: its form flag 0x10 is read once (R5 F-7).
    void Opened(RE::FormID a_actor, std::uint64_t a_now, const RE::TESObjectREFR* a_anchor);

    // WriteBitAfterRepick, when the bit is written although kEvpBuffered is still set ("the re-pick
    // did NOT run within 2000 ms" of UNPAUSED time, R5 F-5): one context line, with the wall time and its
    // split into run / paused / stalled time, the pause state and the open menus (R5 F-7).
    void RepickNotRun(RE::Actor* a_actor, const RE::AIProcess* a_proc, std::uint64_t a_waitedMs, std::uint64_t a_runMs,
                      std::uint64_t a_pausedMs, std::uint64_t a_stalledMs);

    // combatsubstrate::Install (kDataLoaded, main thread), right after Harbinger's TESCombatEvent sink was added
    // (R5 F-4, the probe only; nothing is filtered): one line with the source's sink array in send order, each
    // sink's module and RTTI class, and whether Harbinger's sink (`a_harbinger`) is ahead of the Papyrus VM's.
    void CombatEventSinkOrder(RE::ScriptEventSourceHolder* a_holder, const void* a_harbinger);

    // Serve, at each ignoring-bit reset it attributes to OTHER (same package, same start time).
    void OtherReset(RE::FormID a_actor, const RE::AIProcess* a_proc, std::uint64_t a_now);

    // CloseSub (a_dropped false) and DropSub (a_dropped true, real combat), at the top, before the
    // StopCombat / RemoveTarget: the close values, then the post-close timers run from Poll.
    // `a_opened` false (a close before the entry) forgets the window.
    void Closing(RE::FormID a_actor, RE::Actor* a_ref, bool a_opened, bool a_dropped, std::uint64_t a_now);

}
