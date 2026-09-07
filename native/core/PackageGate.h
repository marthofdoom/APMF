#pragma once

// T3 -- package-offer allowance (ch.9, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven AliasPkgProbe (Docs/PROBE-ALLOWANCE.md
// "Probe 2" -- PROVEN for Phases 1-2, engage/release). See PackageGate.cpp
// for the design; Docs/ALLOWANCE-TEMPLATE.md §3/§7.
namespace apmf::packagegate {

    // Patch VTABLE_Character[0] slot 0x49 (CheckForCurrentAliasPackage) once.
    // Call at kDataLoaded. VR-refused (the vtable index + EvaluatePackage
    // reloc are SE/AE only). Idempotent.
    void Install();

    // Nudge the actor's package selection to re-evaluate NOW: Address-Library
    // Actor::EvaluatePackage(actor, true, false) -- resetAI stays false,
    // never a full AI reset (mirrors the field-proven AliasPkgProbe.cpp
    // mechanism exactly). Exposed so a package-offer claim takes effect within
    // one eval instead of waiting for the engine's own natural poll.
    //
    // MUST NOT be called from inside a Channel lifecycle call (corrected
    // 2026-09-06; the earlier "Engage/OnOwnerChanged/Release call this, and
    // they already run on the game thread" claim is RETRACTED). Those run
    // inside ControlMap::Drain's apply loop, BEFORE Drain Publish()es -- so
    // the 0x49 thunk would answer off the PREVIOUS generation and never see
    // the claim the nudge is about. channels/OfferPackage.cpp posts it through
    // apmf::mainthread::Post instead, so it runs one hop past that Publish.
    // Right thread, wrong MOMENT was the whole bug.
    //
    // No-op on a null actor.
    void EvaluatePackage(RE::Actor* a_actor);

    // RULE C heartbeat (marth 2026-09-06, "does 0x49 actually redirect?" probe) --
    // call once per frame from Arbiter::OncePerFrame (game thread), same seat
    // ActionGate.cpp's PfpHeartbeat() and NonAliasProbe.cpp's PollClaimedPackages()
    // already use. INI-gated ([PackageGate] EnableRedirectLog, default ON -- see
    // PackageGate.cpp); self-throttled internally to a fixed cadence, so this costs
    // one relaxed atomic-bool load the rest of the time. Prints the cumulative
    // 0x49-fired / fired-for-a-claimed-actor / redirect-won counters EVEN WHEN THEY
    // ARE ZERO, so "the hook never fires for our claim" reads as a visible zero
    // line, never as silence indistinguishable from "nothing to report."
    void Heartbeat();

}
