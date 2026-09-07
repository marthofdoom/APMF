# Spec: making the ch.9 package-offer hold ROBUST

> **STATUS BANNER (added 2026-09-07) — §1 IS WRONG, §2.2 IS CONFIRMED, §3/§4 ARE STILL LIVE.**
> - **§1's "Part A is already built and correct" is the single most costly wrong sentence in
>   this repo's docs.** Part A was NOT correct: `channels/OfferPackage.cpp` fires its
>   `EvaluatePackage` nudge BEFORE the claim publishes, so the engine's 0x49 question is
>   answered with the PRE-claim package. The graduated channel's first field run engaged
>   **0 of 6** dispatches (MFO `Docs/DIAG-2026-09-06-loot-travel.md`). This document verified
>   the wrong property: it checked that the call happens on the game thread (§1 `:53-60`,
>   citing `Channel.h`) and that `EvaluatePackage` fires once per real transition (`:61-69`),
>   and never checked WHEN it fires relative to `ControlMap::Publish()`. Right thread, wrong
>   MOMENT. Fix on the unmerged branch `fix/apmf-offerpackage-nudge-ordering`; the retracted
>   "the lifecycle calls it directly, they already run on the game thread" claim is corrected
>   in `Docs/STATUS.md` and in `PackageGate.{h,cpp}` on that branch. **NOT SHIPPED.**
> - **§2.2 IS NO LONGER A HYPOTHESIS — PROMOTED TO A MEASURED FINDING (2026-09-07).**
>   `:120`'s "the engine is very likely NOT consulted on every occasion" is exactly right and
>   is now measured: across ~75 s of live dispatches the engine's own evaluation cadence
>   produced **ZERO** 0x49 questions, and 16/16 wins followed an explicit nudge. Everything
>   this file infers from that hypothesis should be read as resting on evidence now.
> - **§`:112-117` "No expiry/TTL exists on APMF's side" was true for ch.9 in 2026-09-03 and is
>   false for the framework today** — `kIntent_Cast` claims are TTL-bounded (`INVARIANTS #3c`),
>   and that TTL is itself a documented gap (`DENY-COMPLETENESS-AUDIT.md` open gap 11).
> - **§3 and §4 remain LIVE guidance:** "re-nudge churn is off the table" and "find the other
>   seat" are still the right shape of the remaining problem, and §4's correlation probe is
>   what `core/NonAliasProbe.cpp`'s poll implements.

Status: **implementation spec (2026-09-03), research-only — no code in this doc has been
applied.** ~~Answers marth's brief: Part A (one-shot engage/repoint/release nudge) turns out to be
**already built and correct** — this doc documents and verifies it rather than proposing new
code.~~ (That claim is RETRACTED — see the banner above.) Part B (a sticky/priority hold that stops the drift back to the framework package) is
**not buildable today without violating the scalpel constraint below** — every static candidate
is either the wrong shape, an unreversed-ABI risk, or a hammer. The recommendation is to ship
Part A alone and gate any Part B work behind a new, precisely-scoped probe (§4).

## 0. The scalpel constraint (marth, mid-task)

Before any Part B candidate is accepted it must pass ALL of:

1. Deny **only** the specific competing framework package's **selection**, and **only** while the
   ch.9 offer-claim is active on that actor. Nothing broader.
2. Leave the framework's **other** behavior fully intact — its scripts, quest logic, any AI that
   isn't the loot-travel package must keep running during the hold.
3. Be **fully reversible**: the instant the claim releases, the framework package resumes with
   **zero residual state** — no permanent priority change, no lingering flags.

Any candidate that permanently mutates package priority, broadly disables the actor's AI/package
system, sets a wholesale native deny bit, or leaves state behind after release is **rejected**,
not weighed. Every candidate below carries an explicit `Clobber check:` line against this bar.

## 1. Part A — the one-shot engage/repoint/release nudge: ALREADY BUILT, verified correct

**Finding: this is not new work.** `native/channels/OfferPackage.cpp:46-63` already calls
`apmf::packagegate::EvaluatePackage(actor)` from all three transition points the brief asked for:
`Engage` (46-51), `OnOwnerChanged` — i.e. Repoint or a claim-owner change (53-57), and `Release`
(59-63). This was graduated 2026-09-03 from the field-proven `AliasPkgProbe` (`Docs/
PROBE-ALLOWANCE.md` "Probe 2") per the file's own header comment (`OfferPackage.cpp:22-27`).

### 1.1 The exact call chain (game-thread, transition-only — both guarantees traced end to end)

```
PlayerCharacter::Update (vtable 0xAD)          native/core/Hook.cpp:60-81  (PlayerUpdateHook::thunk)
  -> Arbiter::OncePerFrame()                    native/core/Arbiter.cpp:27-29
    -> ControlMap::Drain()                      native/core/ControlMap.cpp:91-145
      -> ApplyRequest(op, next)                 native/core/ControlMap.cpp:147-210
           freshChannel -> channel->Engage(...)          :190-191  (0->1 claim)
           !freshChannel && newOwner -> OnOwnerChanged()  :202-203  (basis-driven owner change)
      -> ApplyRelease(handle, next)              native/core/ControlMap.cpp:212-265
           last claim gone -> channel->Release(...)       :244      (1->0 claim)
           new owner after removal -> OnOwnerChanged()    :255      (incumbent drops, next claim wins)
      -> ApplyRepoint(handle, param, next)        native/core/ControlMap.cpp:267-301
           owning claim reproints -> OnOwnerChanged()     :297      (same handle, in-place retarget)
        -> OfferPackageChannel::Engage/OnOwnerChanged/Release   native/channels/OfferPackage.cpp:46-63
             -> apmf::packagegate::EvaluatePackage(actor)        native/core/PackageGate.cpp:110-115
                  Actor::EvaluatePackage(actor, true, false)     RELOCATION_ID(36407,37401), resetAI=false
```

- **Game-thread guarantee:** `PlayerUpdateHook::thunk` hooks `VTABLE_PlayerCharacter[0]` slot
  0xAD, i.e. `PlayerCharacter::Update` — the seat `Hook.cpp:63-68`'s own comment calls "the player
  ticks exactly ONCE per frame on the game thread", and the one `[threadcheck]` instrumentation in
  the codebase (`Hook.cpp:73-77`, cross-referenced by `Docs/INVARIANTS.md` #12) treats this
  specific seat as the authoritative single-threaded writer reference. `Channel.h:54-61`'s own
  contract states the per-NPC lifecycle (`Engage`/`OnOwnerChanged`/`Release`) is "ALL on the game
  thread (the ControlMap drives them from the 0xAD hook / its once-per-frame drain)" — traced and
  confirmed true above, not just asserted.
- **Not-per-tick guarantee:** `Drain()` only walks the queued ops (`ControlMap.cpp:91-124`) — a
  quiet frame with no `Request`/`Release`/`Repoint` enqueued returns at line 109 before touching
  any channel (`ops.empty() && !anyUnloaded`). `EnsureClaimLocked` on MFO's side (`native/
  APMFBridge.cpp:79`, `marth-follower-overhaul` repo) is itself a no-op on an unchanged package
  form — "a mutex lock + map lookup + timestamp write, no actual APMF RequestEx/Repoint call"
  (`Packages.cpp:1203-1205`) — so a steady-state hold enqueues nothing at all, and `EvaluatePackage`
  fires exactly once per real transition (engage / repoint-to-a-different-package / release), never
  once per frame. This is the correct fix for the churn the per-tick MFO-side re-assert used to
  cause.
- **`resetAI` stays `false`** (`PackageGate.cpp:114`) — never a full AI reset, matching the
  field-proven `AliasPkgProbe` mechanism exactly (per the file's own citation).

### 1.2 Verdict on Part A

**No changes needed.** It already satisfies the brief's three asks (engage, Repoint, and — as a
bonus not explicitly asked for but correct — release) with a traced, verified game-thread /
transition-only call chain. Section 4's probe should still capture whether the SINGLE nudge is
sufficient to make the override stick (§2/§3 below) or whether the drift happens downstream of
where Part A can reach at all.

## 2. Part B — why does `curPkg` drift back to `0009BE51`? (diagnosis)

### 2.1 What the record actually shows

Cross-referencing three sources:

- **`design.md:210-216`** (§5a, the mechanism's own design contract): "The engine calls
  [`CheckForCurrentAliasPackage`] to ask 'does an ALIAS give this actor a package right now?' …
  else return `original(self)`." This frames 0x49 as consulted whenever the engine performs an
  alias-tier package evaluation, not gated behind a separate precondition APMF can't see.
- **`design.md:237-239`** (the Tuxborn audit): "Simple Follower Framework and every custom
  follower are ALIAS-TIER with ZERO PapyrusUtil package overrides" — i.e. Cicero's `0009BE51` is
  itself alias-tier, not the "genuinely non-alias procedure-list pick" `Docs/
  PROBE-NONALIAS-PACKAGE.md` worried about when it was written (that doc's own title question was
  answered, informally, by this later design-doc finding — both packages compete WITHIN the same
  0x49-governed tier, they are not in different tiers). This also rules out `Docs/
  INVARIANTS.md` #3a/§5b's "script-tier override" explanation for the drift: a `PapyrusUtil`
  override would sit ABOVE APMF and be structurally invisible to it, but the audit found zero such
  overrides in the actual load order, so that is not what's fighting the claim here.
- **The field-observed probe log** (per the brief): `engineOrig=0x0009BE51 … hookReturns=0xFE067836`
  — confirms the hook fires for Cicero's FormID and DOES return the claimed package when it fires
  (this rules out `Docs/PROBE-NONALIAS-PACKAGE.md` §5 hypothesis (B) — "the vfunc never fires for
  Cicero" — outright; hypothesis (A), "it fires and IS consulted", is the one supported by direct
  field evidence). What the probe log does **not** establish is whether the hook fires on
  **every** engine re-evaluation, or only at some subset of them.

### 2.2 The leading hypothesis (not proven — flagged as such)

MFO's own claim-hold is a **true standing claim**: `EnsureClaimLocked` (`APMFBridge.cpp:79` in
`marth-follower-overhaul`) only re-enqueues a `RequestEx`/`Repoint` when the offered package
**form actually changes** — a steady hold produces zero further ControlMap traffic
(`Packages.cpp:1203-1205`, confirmed in §1.1). No expiry/TTL exists on APMF's side
(`grep -rn "expir|ttl|500|timeout"` across `native/core/*.cpp,*.h` — zero matches outside
`CombatBehaviorRE.h`'s unrelated `500ms`-shaped IDs); the 500ms "expiry backstop" referenced in
MFO's own comments is a **client-side** keep-alive concept in `APMFBridge.cpp`, not anything APMF
enforces. So the ControlMap claim, once made, sits present and correct in APMF's map for the
entire hold — the drift is not explained by APMF silently dropping the claim.

That leaves one honest explanation this spec can state with real (if incomplete) support: **0x49
is very likely NOT consulted on every occasion the engine reconsiders Cicero's current package.**

> **PROMOTED FROM HYPOTHESIS TO MEASURED FINDING, 2026-09-07.** This is now the strongest-evidenced
> statement in the file, and it should be read as fact, not as this spec's best guess: over ~75 s of
> live loot dispatches the engine's own evaluation cadence produced **ZERO** 0x49 questions, while
> **16 of 16** redirect wins followed an explicit `EvaluatePackage` nudge (MFO
> `Docs/DIAG-2026-09-06-loot-travel.md`). The engine does not re-ask on a useful cadence of its
> own. Everything downstream of this paragraph rests on measurement now.
The regular package-selection machinery (`Docs/PROBE-NONALIAS-PACKAGE.md` §2's own finding: no
`RE::ProcedureManager`/`RE::PackageManager` class is exposed anywhere in the pinned CommonLib
tree — "the subsystem that actually walks an actor's package list … is entirely unreversed")
almost certainly asks the alias-offer question only at specific decision points (a package
starting, ending, or being invalidated), not on a fixed per-frame or per-tick cadence. Between
those decision points, the AI just continues running whatever `currentPackage` currently is. If
Cicero's OWN package system independently reaches a point where it reconsiders and re-picks
`0009BE51` through a different internal branch than the one that calls 0x49 — or reaches a
decision point where 0x49 IS called but returns `FE067836` correctly and the engine adopts it,
and then some LATER, unrelated decision point (not driven by anything APMF's nudge fired for)
causes another reconsideration that again lands on `0009BE51` — this spec cannot distinguish
those two shapes from the evidence in hand. **This is exactly the honest gap `Docs/
PROBE-NONALIAS-PACKAGE.md` §5 flagged and could not close from headers alone; it still can't be
closed from headers alone. It needs the timeline-correlated runtime probe in §4.**

## 3. Part B candidates — ranked, each scored against the scalpel constraint

| # | Candidate | Shape | Scalpel verdict |
|---|---|---|---|
| 1 | **Extend Part A: an event-driven re-nudge keyed to a real "the engine is about to reconsider" signal**, if one can be found | Same `EvaluatePackage(true,false)` call APMF already makes, fired more precisely | **Only remaining scalpel candidate** — see §3.1 |
| 2 | `Actor::PutCreatedPackage` (0xDF) as a second T3-style gate | vtable hook, RTTI-verifiable | **Wrong shape, not a stickiness fix** — see §3.2 |
| 3 | `Actor::GetCurrentPackage()` direct patch | non-virtual call-site patch | **Rejected outright — banned by #17, not scored further** |
| 4 | `AIProcess::currentPackage` raw write | plain-data member write | **Rejected outright — not reachable, not a deny, not scored further** |
| 5 | `BGSProcedureTreeProcedure`'s `Unk_XX` vtable slots | blind vtable hook, unreversed signatures | **Rejected outright — unreversed ABI, not scored further** |
| 6 | Native deny bits (`kMovementBlocked`, `SetDontMove`+`KeepOffsetFromActor`, `SetActivationBlocked`) | wholesale per-domain flags | **Rejected outright — explicit hammer, not scored further** |
| 7 | Runtime `TESPackage` priority mutation | mutate the package asset's own priority field | **Rejected outright — not actor-scoped, can't be reversible per-claim, not scored further** |
| 8 | A second, dedicated vtable choke on the actual package-stack ranking function | would need `RE::ProcedureManager`/`RE::PackageManager` — does not exist as an exposed class | **Rejected outright — no such class in the pinned CommonLib tree (`Docs/PROBE-NONALIAS-PACKAGE.md` §2), nothing to hook** |

Full clobber-check reasoning:

### 3.1 Candidate 1 — event-driven re-nudge (the only real candidate)

**Mechanism:** keep using the exact same lever APMF already has sanctioned use of —
`Actor::EvaluatePackage(actor, true, false)` — but fire it in response to a genuine "the engine
just reconsidered and may have drifted" signal instead of only at claim-transition boundaries.

**Clobber check: does this touch the framework's non-package behavior or its post-release
resumption? — No, by construction.** It is the identical call already shipped in Part A
(`PackageGate.cpp:110-115`), which:
- touches only the ONE `Actor::EvaluatePackage` call, itself scoped to the single claimed actor
  (`OfferPackage.cpp:46-63` passes the specific `actor` from the Channel lifecycle, never a
  broadcast);
- is a re-EVALUATION, not a state mutation — it asks the engine to re-derive its current package
  from the SAME inputs (the 0x49 answer + whatever else feeds the pick), so it has no memory and
  leaves nothing behind. `Release()` still triggers the same call, so relinquish is unchanged
  (`OfferPackage.cpp:59-63`) — the framework package resumes exactly as it does today;
- makes no write to `TESPackage`, no priority mutation, no native bit — Cicero's own scripts, his
  quest logic, and any AI facet other than "which package is currently his" are completely
  untouched by this call, matching `design.md:222-226`'s own three guardrails for the offer
  channel verbatim (bounded window, clean relinquish, no alias/run-once state touched).

**Why it isn't buildable today regardless:** there is no known, CommonLib-exposed signal for "the
engine is about to reconsider this actor's package" to hang the extra nudge on. `Docs/
PROBE-NONALIAS-PACKAGE.md` §2's exhaustive header read found no `RE::ProcedureManager`/
`RE::PackageManager` class and no reversed `BGSProcedureTreeProcedure` per-node `Update`/`Evaluate`
slot to observe (let alone trigger from). The only fallback that stays within the scalpel bar
without inventing a new engine dependency is a **bounded, LOW-FREQUENCY timer re-nudge** (e.g.
once every 1-2s, not per-frame) while a ch.9 claim is live — this is honestly still "the churn we
removed," just at ~1% of the old frequency instead of zero. It satisfies every clobber-check line
above (same narrow call, same actor, same full reversibility on release) but it is not free: it
reintroduces a `Tick()`-shaped periodic call, which `Docs/INVARIANTS.md` #1/#2 treats as a
KNOWN-INCOMPLETE-BLOCK pattern requiring explicit labeling, not a clean gate. **This is the one
candidate this spec can recommend building — but only after §4's probe confirms it would actually
help (see §4.3), and even then it should default to whatever the probe's measured drift-interval
suggests, not a guessed cadence.**

### 3.2 Candidate 2 — `PutCreatedPackage` (0xDF)

**Mechanism:** a second vtable gate on `Actor::PutCreatedPackage`, which takes the incoming
`TESPackage*` as an argument (`Docs/PROBE-NONALIAS-PACKAGE.md` §3 row 2).

**Clobber check: does this touch the framework's non-package behavior or its post-release
resumption? — Not evaluated further; disqualified on shape, not on clobber risk.** `Docs/
PROBE-NONALIAS-PACKAGE.md:128-137` already found its neighbor functions
(`InitiateTresPassPackage` 0xD7, `InitiateFlee` 0xDD, `InitiateGetUpPackage` 0xDE) are all
special-circumstance package spawners (trespass/flee/get-up), strongly suggesting `PutCreatedPackage`
is the shared callee those call to install a DYNAMICALLY CREATED package — not the seat the engine
uses when switching between two regular authored packages (the `0009BE51` vs `FE067836` case).
Section 6.2 of that doc flags this as "worth a cheap runtime probe" but genuinely unconfirmed
either way. If a probe later shows it DOES fire for the regular pick, it would still need its own
clobber assessment (a conditional rewrite of `a_package` before forwarding, engine-answer-first,
same shape as 0x49) — but that is speculative pending data this spec does not have.

## 4. The runtime probe Part B actually needs

**Verdict: Part B cannot be built from static analysis today — not because no mechanism was
found, but because the ONE mechanism that survives the scalpel bar (§3.1) can't be wired up
without either (a) a currently-nonexistent engine signal, or (b) a bounded-but-nonzero periodic
timer whose cadence would be a guess without field data.** Before writing any Part B code, run
this probe.

### 4.1 What to log

Extend `native/core/NonAliasProbe.cpp`'s existing OBSERVE-only discipline (same NumLock switch,
same per-actor rate limit shape — `NonAliasProbe.h:45-74`) with a **second, independent** log
line that does NOT run inside the 0x49 thunk:

1. A periodic (every ~250ms, gated behind the same debug switch, off by default) call to
   `actor->GetCurrentPackage()` for the CURRENTLY ch.9-claimed actor(s) only (iterate
   `ControlMap`'s claimed-with-`kIntent_OfferPackage` set — cheap, matches `INVARIANTS` #13's
   "controlled NPCs are a small map" assumption), logging: FormID, the package's FormID, and a
   monotonic tick counter or timestamp.
2. Tag the EXISTING 0x49 observe line (`PackageGate.cpp:77-84`) with the same monotonic
   tick/timestamp so the two logs can be interleaved and read as one timeline.
3. Log whether `ControlMap::Get().TryGetOwningClaim(...)` succeeds and what `claim.form` reads, on
   BOTH the periodic poll (item 1) and inside the 0x49 thunk (already available as `claim.form`
   at `PackageGate.cpp:53-57`, just needs to be added to the log line) — this directly tests §2.2's
   "is the claim continuously present" assumption rather than relying on the client-side reasoning
   in §2.2.

### 4.2 What the timeline distinguishes

- If `GetCurrentPackage()` ever reads `0009BE51` while `TryGetOwningClaim` reports the claim
  STILL present and named `FE067836` at the SAME poll → the claim is not the problem; the engine
  is choosing `0009BE51` DESPITE 0x49 being answerable correctly, meaning either 0x49 wasn't
  called during whatever reconsideration produced that value, or it was called and its answer was
  not adopted. Correlate against the 0x49 observe line's tick counter: no 0x49 call in that window
  → confirms §2.2's leading hypothesis (0x49 isn't consulted on every reconsideration) and
  justifies building §3.1's bounded timer, sized to the measured gap between drifts. A 0x49 call
  IS present in that window with `hookReturns=FE067836` yet `GetCurrentPackage()` still reads
  `0009BE51` moments later → a DIFFERENT, deeper problem (the return is being computed correctly
  but not durably adopted) that this spec cannot currently explain and would need its own
  follow-up, not a re-nudge (a re-nudge would just repeat the same ignored call).
- If `GetCurrentPackage()` reads `0009BE51` at a poll where `TryGetOwningClaim` reports **no
  claim** for that actor → the claim itself is lapsing (a client-side MFO issue, `EnsureClaimLocked`
  or its caller in `Packages.cpp` not actually holding continuously) — this is out of APMF's scope
  to fix and should be reported back to the MFO side rather than treated as an APMF Part B gap.

### 4.3 Decision gate after the probe

- **Gap confirmed (0x49 not consulted on every reconsideration), claim continuously present** →
  do NOT build a bounded re-nudge timer. **A periodic re-nudge IS re-assertion, and re-assertion
  is a FAILURE by the standing principle (marth 2026-09-04) — it is the exact mechanism we removed
  from MFO's loot-travel, merely relocated into APMF; sizing it from a measured drift-gap does not
  change what it is. Re-nudge is OFF the table.** The correct answer is a SIGNAL TO DENY: identify
  the engine's OTHER package-reconsideration decision point — the seat that re-selects `0009BE51`
  during the drift, the one 0x49 does not cover — and DENY the framework package's SELECTION there,
  at the source, engine-answer-first, the SAME shape as the 0x49 gate but on the seat that actually
  fires. Because no `ProcedureManager`/`PackageManager` chokepoint is exposed in the pinned
  CommonLib tree (§2), locating that seat needs a targeted RE spike: disassemble the compiled
  package-pick routine to find the reconsideration call, then a chainable virtual/vtable deny point
  on it that passes the §0 scalpel bar (deny only this package's selection, only while the claim is
  held, fully reversible). The probe's role is therefore to CONFIRM the deny seat is elsewhere than
  0x49 and point the RE spike at it — not to measure a re-nudge cadence.
- **Claim is lapsing client-side** → no APMF change; feed the finding back to MFO's
  `APMFBridge.cpp`/`Packages.cpp` Pump() cadence.
- **0x49 fires and answers correctly but the engine still doesn't durably adopt it** → this is a
  genuinely new, deeper question outside this spec's evidence; do not guess a fix — it would need
  its own RE spike (disassembly of the compiled package-pick routine), same caveat `Docs/
  PROBE-NONALIAS-PACKAGE.md` §6 already gives for the `BGSProcedureTreeProcedure` unknowns.
- **In all three cases, Part A ships as-is regardless** — it is already correct, already the
  narrowest possible mechanism, and (per marth's own framing) staying nudge-only rather than
  reaching for a hammer is an acceptable outcome on its own.

## 5. Risk / sequencing note

- **Safe to ship now:** Part A. It is already in the tree (current branch
  `feat/allowance-channels-t1-t3`, `native/channels/OfferPackage.cpp` +
  `native/core/PackageGate.cpp`), already field-proven for Phases 1-2 (`Docs/PROBE-ALLOWANCE.md`
  "Probe 2"), traced end-to-end in §1.1 above, and satisfies every constraint in the original
  brief. No further code change is needed for Part A; this doc's only Part-A action item is
  making sure `Docs/CHANNEL-MAP.md`/`Docs/STATUS.md` keep citing it as done (they already do).
- **Needs the probe first:** any Part B code. §3's scan found exactly one candidate that survives
  the scalpel constraint (§3.1, an event-driven or bounded-timer re-nudge using the SAME call Part
  A already makes) and it cannot be responsibly sized or even confirmed necessary without §4's
  timeline data. Every other candidate is rejected on shape or on the explicit hammer criteria
  marth gave, not deferred pending more research — building any of #2 (wrong shape, unconfirmed
  seat), #3-8 (banned by #17, unreversed ABI, or non-actor-scoped/irreversible) would not pass
  review regardless of probe results.
- **Acceptable fallback:** if §4's probe comes back inconclusive or shows the gap is too
  irregular to size a sane bounded-timer cadence, staying nudge-only (Part A alone) is the
  correct outcome per marth's own instruction, not a shortfall to keep chasing.

## Evidence file index

- `native/channels/OfferPackage.cpp:1-68`, `native/core/PackageGate.cpp:1-118`,
  `native/core/PackageGate.h` — Part A's implementation (already shipped).
- `native/core/Hook.cpp:60-105`, `native/core/Arbiter.cpp:23-34`,
  `native/core/ControlMap.cpp:88-301`, `native/core/Channel.h:54-89` — the traced game-thread /
  transition-only call chain (§1.1).
- `design.md:202-241` (§5a "the package-OFFER channel", §5b "structurally beneath script
  overrides") — the mechanism's own design contract and the Tuxborn zero-PapyrusUtil-override
  audit that rules out script-tier interference as the drift's cause.
- `Docs/PROBE-NONALIAS-PACKAGE.md` (full doc) — the prior static research spike this doc builds
  on directly; its §5/§6 "cannot distinguish from headers alone" conclusion still holds and is
  the basis for §4's probe spec here.
- `Docs/HOOK-SITE-COVERAGE.md` §5 — confirms no `RE::ProcedureManager`/`RE::PackageManager`
  class exists in the pinned CommonLib tree, closing off candidate #8 definitively.
- `Docs/ALLOWANCE-TEMPLATE.md` §1 ("NOT generic — package procedures"), §4 ("Honest gaps"),
  `Docs/CHANNEL-MAP.md` row 9 — prior documentation this spec corroborates rather than
  contradicts.
- `Docs/INVARIANTS.md` #0/#1/#2 (moderate-not-generate, block-don't-force, known-incomplete-block
  labeling), #17 (chainable-vtable-only, no call-site patches, ADAPT-don't-degrade) — the rules
  every candidate in §3 was scored against.
- `native/core/NonAliasProbe.cpp`/`.h` — the existing OBSERVE-only probe infrastructure §4's new
  probe should extend (same switch, same rate-limit, same never-touch-the-return discipline).
- `marth-follower-overhaul` repo (client side, read for context only, no changes proposed there):
  `native/Packages.cpp:1196-1210` (`Pump()`'s unconditional per-slot `OfferPackage` keep-alive
  call and its own comment on why it's cheap), `native/APMFBridge.cpp:73-79,273-283`
  (`EnsureClaimLocked`, the 500ms concept — confirmed CLIENT-side, not an APMF-enforced expiry).
