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

    // Drop the actor's remembered `[ch.9-redirect]` answer, so the NEXT claim on
    // this actor prints a line instead of being swallowed by the RULE D
    // transition dedup. Call it from the ch.9 RELEASE edge -- the edge that
    // actually KNOWS the claim went away.
    //
    // WHY THE EDGE HAS TO DO IT (round-2 review, 2026-09-06). PackageGate can only
    // notice "no claim" when the 0x49 hook is CONSULTED for the actor, and there is
    // no guarantee of such a consult between a release and the next same-form claim:
    // a release and a re-request that land in ONE ControlMap::Drain both post their
    // nudges, and at Pump the release nudge is correctly DROPPED as stale (the claim
    // is already back), so 0x49 is never called with no claim. The re-engage then
    // produces a byte-identical answer tuple -- same actor, same offered package,
    // same engine original -- and the dedup suppresses the line even though the
    // redirect happened. The mechanism would be working and the pass criterion would
    // read failure.
    //
    // MUST BE POSTED, NOT CALLED INLINE FROM THAT EDGE (F4-2, 2026-09-07 -- the
    // same correction the nudge itself needed). A Channel's Release runs inside
    // ControlMap::Drain's apply loop, BEFORE Drain Publish()es; the PUBLISHED map
    // therefore still holds the claim, and a combat-thread 0x49 consult in that
    // window takes the thunk's claim-present path and re-inserts the byte-identical
    // tuple the erase just removed -- restoring exactly the dedup false negative
    // this function exists to remove. channels/OfferPackage.cpp posts it through
    // apmf::mainthread::Post, queued AHEAD of its release nudge, so it runs one hop
    // past that Publish and strictly before any 0x49 consult a nudge causes.
    //
    // Idempotent, cheap (one mutex + one hash erase), and safe to call for an actor
    // that was never recorded. Any thread; takes only PackageGate's own log-dedup
    // mutex, so it cannot participate in a lock cycle.
    void ForgetRedirect(RE::FormID a_actor);

    // Drop EVERY remembered redirect answer, for every actor.
    //
    // WHY A WHOLESALE FORGET EXISTS AS WELL (F4-1, 2026-09-07). `ForgetRedirect`
    // above is driven from the ch.9 release EDGE, and the thunk's own no-claim
    // branch is the backstop -- but BOTH need something to happen for a given
    // actor. A revert / new game goes through `ControlMap::Clear()`, which by
    // design makes NO `channel->Release` calls (so no edge fires) and hands the
    // hook nothing to be consulted about (the actors are being replaced). The
    // remembered tuples therefore survive the world boundary, and the FIRST
    // dispatch of the same package on the same FormID in the NEW session produces
    // a byte-identical answer tuple, which the RULE D transition dedup swallows:
    // the redirect works and the pass criterion reads failure -- the exact
    // false-negative `ForgetRedirect` was added to remove, one scope wider.
    //
    // Call it at the world boundary (`plugin.cpp`'s OnRevert, beside
    // `mainthread::Discard()`), never per-actor. Idempotent, cheap (one relaxed
    // load when nothing is remembered), and erasing can only ever cause an EXTRA
    // `[ch.9-redirect]` line, never a missing one.
    void ForgetAllRedirects();

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
