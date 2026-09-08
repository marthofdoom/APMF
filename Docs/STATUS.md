# APMF STATUS — living handoff (start here)

Updated 2026-09-06. The current state of the build: what's shipped, what's
probe-gated, what's next. Keep this current in the SAME change as any
build/finding/workflow change.

## HEAD OF WORK 2026-09-08 -- the three DEFERRED review findings (`fix/apmf-deferred-f4-2-f5-2-f5-3`)

Branch off main. One commit per finding; each gets its own Fable diff review. **Not
field-run.**

**F4-2 (SEV-3, log fidelity) -- the ch.9 redirect erase ran in the pre-`Publish()`
window.** `channels/OfferPackage.cpp`'s `Release` called `packagegate::ForgetRedirect`
inline. Release runs inside `ControlMap::Drain`'s apply loop, so the PUBLISHED snapshot
still held the claim: a combat-thread 0x49 consult in that window took the thunk's
claim-present path and re-inserted the byte-identical tuple, and a release plus a
same-form re-request inside ONE Drain then had its `[ch.9-redirect]` line eaten by the
RULE D dedup again -- the exact false negative `ForgetRedirect` was added to remove.
The erase is now POSTED through `apmf::mainthread::Post`, queued AHEAD of the release
nudge, so it runs one hop past that `Publish` and strictly before any 0x49 consult a
nudge causes. Unconditional by construction (a bare Post, never behind
`PostDeferredNudge`'s gate-1 early return). At kPreLoadGame the task is dropped by
`mainthread::Discard()` -- correct and costless: the world boundary erases wholesale via
`ForgetAllRedirects()` and the thunk's no-claim backstop covers the rest, and a missed
erase can only suppress a LOG LINE, never change what 0x49 returns.

**F5-2 (MEDIUM, dormant) -- a FromPackage heartbeat was refused as a spell change.**
`ControlMap::ApplyRequest` stores the spell it EXTRACTED from a `kCastFlag_FromPackage`
request, a FormID the client is never handed, so the only form a correct client can
heartbeat with is the PACKAGE it requested with -- and `ApplyRepoint`'s
form-change refusal fired on every such call: a loud warning about a client doing
exactly the right thing. The claim now remembers the client-named form
(`Claim::castSrcForm`, 0 for every other claim) and `ApplyRepoint` treats a heartbeat
carrying it as the same-form shape: nothing refused, nothing warned, TTL renewed. A
genuinely different form -- another package, or a bare spell -- is still refused and
still logged. Dormant today: MFO sets no FromPackage claim.

NOTE for the coordinator: `native/APMF_API.h` (byte-shared with MFO, append-only) still
documents Repoint as "a `param.form` different from the claim's current spell is
REFUSED" with no FromPackage carve-out. Correcting that comment touches a byte-shared
header and must be done in lockstep with MFO's copy -- deliberately NOT done here.

## HEAD OF WORK 2026-09-06 -- ch.9 nudge ordering fix (`fix/apmf-offerpackage-nudge-ordering`)

Branch off main. **CI-green, NOT field-run.** Fixes the bug MFO's
`Docs/DIAG-2026-09-06-loot-travel.md` diagnosed: every MFO loot dispatch claimed the
ch.9 package offer and none of them ever put the follower on the offered package.

**Root cause: right thread, wrong MOMENT.** `channels/OfferPackage.cpp`'s
`Engage`/`OnOwnerChanged`/`Release` called `packagegate::EvaluatePackage` synchronously.
All three run INSIDE `ControlMap::Drain`'s apply loop, and `Drain` only `Publish()`es the
new snapshot AFTER that loop -- while the 0x49 `CheckForCurrentAliasPackage` thunk answers
off the PUBLISHED snapshot. So an engage nudge re-evaluated while the hook still saw NO
claim (offer never adopted), and a release nudge re-evaluated while the hook still saw the
claim (offered package asserted at the instant of withdrawal). The field-proven
`AliasPkgProbe` this code was graduated from had the order right (claim first, nudge one
frame later); graduating it into the Channel lifecycle kept the call and inverted the
ordering on both edges.

**Fix (3b9b29c):** both edges post the nudge through `apmf::mainthread::Post`, which runs
one hop past that `Publish` (`Arbiter::OncePerFrame` = `Drain()` then `Pump()`) -- the same
idiom, for the same ordering reason, that ch.8b's two existing deferred teardowns already
use (INVARIANTS #20) -- the cast-claim EVICTION teardown in `ControlMap::ApplyRequest`
(`core/ControlMap.cpp:477`) and `CastComposeChannel::Release`'s proxy teardown
(`channels/CastCompose.cpp:126`). The posted nudge carries an `RE::ActorHandle`, re-reads the
now-published ch.9 claim and DROPS itself (logged, no retry) if the state moved.

**Fable diff review + the five follow-up fixes it found (this branch, second commit):**

1. **The pass criterion would have false-negatived.** `core/PackageGate.cpp`'s
   `[ch.9-redirect]` RULE D transition dedup only ever WROTE `g_redirectLast` on the
   claim-present path, so a no-claim answer was never recorded and never cleared -- and
   dispatch -> release -> the SAME dispatch again produced a byte-identical tuple, so the
   second dispatch printed NOTHING even though the redirect happened. A working deck cycle
   would have been graded a failure. The no-claim path now ERASES the actor's remembered
   answer (guarded by an atomic count so unclaimed actors never touch the mutex), so the
   next claim is a fresh insert and prints.
2. **Posted tasks crossed the load boundary.** kPreLoadGame -> `ReleaseAll` -> `Release`
   -> `Post`, and nothing `Pump()`ed until the first player Update AFTER the load, so the
   task acted on the NEW world. New `mainthread::Discard()`, called in the kPreLoadGame
   handler (after `ReleaseAll` + `castproxy::ResetAll`) and on the revert/new-game path.
   Harmless for what is queued today -- the review corrected the original commit's
   "strictly safer" to "harmless" -- but the hole is GENERAL to every future teardown Post.
3. **The commit's "the unload sweep hits gate 2" claim was FALSE.** `Actor::GetHandle`
   MINTS a fresh valid handle for an actor that has none, so the posted task's
   `handle.get()` check could not catch `ControlMap`'s unload sweep. Gate 1 is now
   `!actor || !actor->IsHandleValid()` -> NOT POSTED (checked BEFORE `GetHandle`).
4. **Doc drift inside the mechanism's own files.** `core/PackageGate.{h,cpp}` still
   asserted the RETRACTED "the lifecycle calls it directly, they already run on the game
   thread" claim. Both corrected, and the retraction is written down as a retraction.
5. **Scope trim + dependency note.** `OfferPackage.cpp`'s header no longer restates the
   field session's timestamps/DLL hashes (they rot; the DIAG owns them) -- two-line pointer
   instead, ordering rationale and re-validation contract kept. `core/Channel.h` now records
   that a channel MAY read the ControlMap back (lock-free RCU, any thread) and why, since
   OfferPackage is the first channel to do it.

**Round-2 Fable review + its four follow-ups (third commit).** Verdict SHIP WITH
FOLLOW-UP; the five fixes above were verified correct and complete, and all four new
findings were log FIDELITY, not mechanism -- the redirect works either way, but the deck
run would have been graded wrong.

1. **The erase could not fire on the path that matters.** Erasing only inside the 0x49
   no-claim consult assumed such a consult happens between a release and the next
   same-form claim. It does not have to: a release plus a re-request landing in ONE
   `ControlMap::Drain` leaves the release nudge correctly DROPPED as stale at Pump (the
   claim is already back), so 0x49 is never called with no claim, the re-engage answers
   with a byte-identical tuple, and the dedup eats the line again. New
   `packagegate::ForgetRedirect(FormID)` is called from `OfferPackageChannel::Release`
   BEFORE it posts its nudge -- the edge that actually knows. The hook-side branch stays
   as a BACKSTOP for the one path with no channel `Release` at all: `ControlMap::Clear()`
   (revert / new game). Two idempotent erase sites; erasing can only ever cause an extra
   line, never a missing one.
2. **The RULE E line cap was reachable and crossing it was silent.** Fix 1 above makes
   `[ch.9-redirect]` at least one line per dispatch, so 600 is reachable in a long
   multi-follower session -- after which every later dispatch prints nothing and the pass
   criterion false-negatives again. Cap resized to 4000 (from the real cadence, not a
   guess), and the crossing now emits a one-shot `spdlog::warn` naming itself, so the
   silence can never be misread as "no redirects" (principle 7).
3. `RedirectAnswer::claimPresent` is dead-always-true; commented as such (no-claim paths
   ERASE, never store) rather than left implying the opposite.
4. Mis-naming corrected here and in `OfferPackage.cpp`'s header: the ch.8b EVICTION
   teardown is `ControlMap::ApplyRequest`'s (`core/ControlMap.cpp:477`);
   `channels/CastCompose.cpp:126` is the channel-`Release` proxy teardown. Both use the
   idiom; neither is "CastCompose's eviction teardown".

**NEXT: a deck cycle.** Pass criterion (now that it can actually fire): a `[ch.9-redirect]`
line within one frame of every ch.9 CLAIMED, on EVERY dispatch including repeats and
including a release + same-form re-claim inside one Drain, with `[ch.9-nudge] ... FIRED
post-publish` between them. If `[ch.9-redirect] SESSION LINE CAP` ever appears, the
absence of later lines is the cap, not the mechanism.

## PROBE-GATED 2026-09-06 -- PFP Phase 0: movement-leaf OBSERVE-ONLY reporting (`feat/pfp-phase0-movement`)

ZERO new hooks: rides the act() thunk `core/ActionGate.cpp` already installs on all
70 `CombatBehaviorTreeNodeObject` leaves (`Docs/CHANNEL-MAP.md` ch.7). Classifies ~30
movement-shaped leaves (8 corroborated by CombatPathingRevolution's own
`CombatBehaviorNodesMovement.h`, ~22 HYPOTHESIS-by-naming) and, for any actor holding a
winning ch.1 `kIntent_MovementBlock` claim, logs `[pfp] mvcbt A ...` on a leaf-fire
TRANSITION -- settling the open question of whether ch.1's full block
(`channels/MovementDeny.cpp`) actually stops the combat AI's own movement branch or
just the translation underneath it. `[pfp] mvcbt H ...` heartbeats (incl. zero) print
every ~30s from `Arbiter::OncePerFrame`, so a dead anchor is never mistaken for "no
ch.1 claims this session." INI-gated `[Probe.mvcbt] Enable=0` in
`Data/SKSE/Plugins/APMF.ini`, default OFF. NOT yet field-run. From the 2026-09-06
"Progressive Facet Probe" design pass §3.1.2 -- Phase 0 (movement) only; that design's
mvpkg/tgt/cast probes are NOT part of this branch.


## CI-GREEN, NOT FIELD-RUN: `fix/apmf-claim-renew-denyhand-spellsteer` (2026-09-06)

The APMF half of the 2026-09-06 deny/heal field diagnosis (MFO
`Docs/DIAG-2026-09-06-deny-heal-failures.md`). Four fixes plus an adversarial-review
follow-up pass; nothing here has been on a deck.

- **F2 / RC2 — a cast claim's TTL is a renewable FLOOR, not a hard expiry.**
  `Repoint` on a live `kIntent_Cast` claim moves its deadline to now + the claim's own
  granted (already clamped) `ttlMs`. Before this, every claim died exactly `ttlMs`
  after `RequestCast` and the client's re-request arrived 0.26–0.61 s later — a
  recurring unclaimed gap in which foreign spells were observed equipping and charging
  (one 110 ms after an expiry). Crash-safety is unchanged: a client that STOPS asking
  still loses the claim on the same schedule. An already-lapsed claim is never
  resurrected. No ABI change.
- **F3 / RC3 — `kCastFlag_DenyHandOnly`: claim a hand purely to DENY it.** The missing
  half of the per-hand cast deny — the client drives nothing on that hand and APMF
  arms nothing, so an actor-wide cast deny is expressible again without giving up
  per-hand scoping. Bit 4 of the already-frozen `flags` word, so **no `kABIVersion`
  bump** (still 6). Ships **DORMANT**: no client sets the bit yet.
- **F5 / RC5 — the score steer is applied where SPELLS are scored.** The bias lived
  only in `WeaponScoreThunk`, whose `driven == itemForm` test compares a spell FormID
  against a WEAPON's — dead code under any INI setting. It now rides
  `CalculateScoreThunk` (the spell/staff item leaves), hand-resolved from the item's
  own `itemSlot.equipSlot`; the dead weapon branch is deleted. Still OFF by default.
- **F6 / RC7 — the cast deny is observable.** `core/CastGate.cpp` had no per-decision
  log at all, so the 2026-09-06 audit had to record its own P1 row as "DENIED (code) /
  UNOBSERVED (log)". A throttled `[t2c] ... CheckCast DENIED ...` line now names the
  actor, the spell, the hand and WHICH narrowing said no (CLAUDE.md principle 5).

**Adversarial-review follow-up (same branch).** Every finding fixed, none deferred:
`Repoint` can no longer rewrite what a cast claim drives (a deny-only claim's form
stays 0; a driving claim's form change is REFUSED loudly, since proxy/target/flags
were resolved against the original spell and `Repoint` re-runs none of that); all
SEVEN winner-selections in `core/ControlMap.cpp` share ONE comparator
(`ControlMap.h::BetterClaim` — higher basis wins, and at an equal basis a deny-only
claim loses to a driving one) so the channel owner and the per-hand gates can never
disagree; a deny-only claim is excluded from the dual-vs-single collision in BOTH
directions (a dual claim no longer evicts a floor, and a standing floor no longer
refuses a dual claim); the two Allowance readers respect an elapsed TTL instead of
denying from a lapsed claim until the sweep runs; and `Docs/DENY-COMPLETENESS-AUDIT.md`
row 8b + its new "deny-only hand claim" section describe the mechanism as it actually
is. One review hypothesis was checked and found WRONG: a client's own direct
`CastSpellImmediate` force is NOT denied by its own floor — that call is vtable slot
0x01 and never consults `CheckCast` (0x0A), field-established by MFO's own
`CasterConsent.cpp` note and by `ENGINE_NOTES` §0.9's zero-magicka infinite-cast
measurement.

**Field observables, in order:** `[ctl] ... REPOINT (... TTL renewed +N ms)` with no
`already auto-expired` churn between claims; `[t2c] ... CheckCast DENIED ...` lines
proving the deny executes; and, once a client sets the bit, no foreign spell charging
on the floored hand.

## ✅ SHIPPED 2026-09-06 -- v0.9.2. PER-HAND EQUIP DENY + THE OFFENSIVE CASTER SEAT.

*(Added 2026-09-07: the tag and the CHANGELOG entry existed since 2026-09-06 and STATUS
carried no v0.9.2 section at all — this file is the living handoff, so a shipped tag missing
from it is a defect in its own right.)*

Tag `v0.9.2`. **CI-verified only; NOT field-tested as a build** — though its two changes were
both exercised in MFO's 2026-09-06 deck session, which is where their limits were found.

- **The equip deny is PER-HAND** (`feat/deny-perhand`, `core/EquipGate.cpp` +
  `core/CastGate.cpp` + `Allowance::AllowedCastForHand`). A cast claim used to deny every
  spell and staff on BOTH hands for its whole life, which disarmed the follower; it now denies
  only the claim's own hand, read from a native per-hand signal at each seat
  (`MagicCaster::GetCastingSource()` at 0x0A; `CombatInventoryItem::itemSlot.equipSlot` at
  0x0F, compared against `BGSDefaultObjectManager`'s own left/right hand objects).
- **The engine seats cover the Offensive caster** (`feat/offense-seat-scope`,
  `CastSeats.cpp:483-500`). A claimed HOSTILE spell classifies into `CombatMagicCasterOffensive`,
  never `CombatMagicCasterRestore`, so before this a claim on an offense spell was structurally
  inert. Safe because every thunk re-tests {claim actor, claim's driven form} per call; seat
  0x07's read of `primaryAV` (a Restore-only member) is now guarded by a vtable-identity check.
- **The classify seat refuses hostile spells** — applying the heal-other classification fix to a
  hostile spell would move it to a table row the engine never uses.

**What v0.9.2 did NOT fix, learned the next day in the field:** narrowing the deny to one hand
left the OTHER hand entirely AI-governed, and nothing in the release closed the TTL gap — open
gaps 9 and 11 in `Docs/DENY-COMPLETENESS-AUDIT.md`. **Both mechanisms are now ON `main`**
(`fix/apmf-claim-renew-denyhand-spellsteer`, merged 2026-09-07 — the block above): `Repoint`
renews a cast claim's TTL, and `kCastFlag_DenyHandOnly` lets a client close the other hand. Neither
is in a tagged release yet, and F3 ships DORMANT: **gap 9 stays open in practice until a client
sets the bit**, which no client does today.

## ✅ SHIPPED 2026-09-05 -- v0.9.1 (beta prerelease). THE NPC'S OWN AI PERFORMS A CLIENT'S CAST.

**FIELD-PROVEN on the deck.** Release: https://github.com/marthofdoom/APMF/releases/tag/v0.9.1
(client: MFO v2.0.1). main = the field-proven tree. The `feat/ai-cast-seats-impl` section below
is now SHIPPED -- read this block first.

**THE MISSING LINK WAS CLASSIFICATION, NOT THE SEATS.** The five seats were correct but
unreachable: `CombatInventory::Rebuild`'s per-effect classifier keys on
`archetype<<16 | av<<8 | hostile<<1 | isSelfDelivery`, and the 23-row table has **no row for
`(Health, self=0, hostile=0)`** -- so a heal-OTHER spell mints no CombatInventoryItem, no Restore
caster is ever built, and no seat is ever called. **That is why no follower in any mod has ever
healed an ally through the game's own AI.**

**SEAT 0 CLASSIFY** (`core/CastClassify.cpp`, `CombatMagicItemData` vtable slot 1) forces
`+0x4c=1` for the claim's driven form so the effect keys into the self-heal row. Then the
**deny-complete 0x0F** (`core/EquipGate.cpp`) denies every other spell/staff item while the claim
stands, so ours is the sole survivor and takes the equip slot -- and the equip slot is what mints
the caster (`CombatBehaviorContextMagic` ctor -> item vfunc 0x15 CreateCaster). The four seats
then answer from the claim. **APMF still makes no equip, anim or cast write, and CastSpellImmediate
is gone.**

**Guards on the one raw write:** install refuses unless the disassembled RTTI name matches, plus
per-call vtable identity, an INI kill-switch (`[CastSeats] EnableSeat0Classify`), VR refused, and
**non-AE refused outright** (the SE offsets are NOT confirmed).

**DONE since this was written:** offense casts ride the same ch.8b path, and the equip deny was
narrowed from both hands to one (`feat/per-hand-cast-claims`; `Allowance::AllowedCastForHand` +
`EquipGate`'s `hasHandSeat`). The narrowing had a known cost -- an unclaimed hand is fully
AI-governed, and the field proved the AI uses it (25 Stone Runes / 6 Poison Sprays / 8 Raise
Zombies charged there under a live claim; MFO `Docs/DIAG-2026-09-06-deny-heal-failures.md`, RC3).
`kCastFlag_DenyHandOnly` (see the `fix/apmf-claim-renew-denyhand-spellsteer` section above) is the other half: a claim that closes a hand it drives nothing
on, so an actor-wide cast deny is expressible again WITHOUT giving up per-hand scoping.

**NEXT:** movement/target facet RE. Field-run `fix/apmf-claim-renew-denyhand-spellsteer` (the
renewable cast TTL, the deny-only hand claim, the spell score steer, and the `[t2c]` deny log) --
none of it has been on a deck, and the deny-only flag ships DORMANT until a client sets the bit.

Still open beyond that branch, and NOT closed by it: `Docs/DENY-COMPLETENESS-AUDIT.md` open gaps
10 (a weapon can take the claimed hand — no weapon-side admission gate exists), 12 (the
request-to-publish window) and 13 (a spell already CHARGING when the claim arrives). Gap 9 is
closable by a client now but is open until one sets the bit.

## HEAD OF THE CAST WORK: `feat/ai-cast-seats-impl` (SHIPPED in v0.9.1 and FIELD-RUN)

*(Heading corrected 2026-09-07: it said "built, CI-green, NOT field-run" for two days after the
2026-09-05 deck run proved it — see the v0.9.1 block above, and the 17 animated offense fires in
MFO's 2026-09-06 session. The body below is kept as the design record of the pass.)*

Off `observe/ai-cast-seats-split` (525daae). **The keystone changed shape: the NPC's
OWN AI now performs a claimed cast, natively, and the forced drive is deleted.**

- **THE FIVE ENGINE SEATS.** While a `kIntent_Cast` claim {actor A, spell S, target T}
  stands, APMF answers the vfunc seats the combat AI's own cast decision is built out
  of, and the AI selects, equips, charges, aims, fires and channels S at T with its own
  animation, magicka, LOS and interrupt handling. **APMF makes no `EquipSpell`,
  `CastSpell`, `CastSpellImmediate`, `NotifyAnimationGraph` or caster-state write
  anywhere.**
  | Seat | Slot | Where | Answer |
  |---|---|---|---|
  | WHICH item | `0x0F CheckShouldEquip` | `core/EquipGate.cpp` | TRUE for the claim's driven form, **without chaining** |
  | WHETHER | `0x06 CheckStartCast` | `core/CastSeats.cpp` | TRUE from the claim |
  | WHERE | `0x0A GetMagicTarget` | `core/CastSeats.cpp` | `out->handle = T`, `out->ptr = nullptr` |
  | HOW LONG | `0x07 CheckStopCast` | `core/CastSeats.cpp` | STOP on TTL / dead / unresolvable / stop-percent |
  | AIM | `0x0D SetupAimController` | `core/CastSeats.cpp` | `aim->+0x30 = T` (always written) |
- **The 0x0F seat is why this works at all, and it is the ONE non-chaining answer in
  the codebase.** The Restore ITEM templates override `CheckShouldEquip` with the static
  `0x81f7c0`, which reads its target straight off the `CombatController`
  (`kSelf ? attacker : combat TARGET`) and runs `ShouldRestore` on it — so a healthy
  follower fighting a healthy foe never lets a heal into its equipment set, and the
  other four seats are NEVER CALLED. There is no interposable seat between it and those
  fields, so chaining is not a weaker answer, it is no answer. **New INVARIANTS #20**
  states the exception, its three conditions, the scope obligation and the release
  ordering rule; **#0 gains a fourth legal channel action, (d) COMPOSE.**
- **Scope is the safety argument.** `GetMagicTarget`'s impl is the SHARED base of 13 of
  the 14 caster vtables — an unscoped redirect would aim Stagger/Disarm/Offensive
  effects at the ally. The caster seats install on `VTABLE_CombatMagicCasterRestore`
  **and `VTABLE_CombatMagicCasterOffensive`** ONLY (2 of the 14 — Offensive added in
  v0.9.2, `CastSeats.cpp:483-500`, because a claimed HOSTILE spell classifies into the
  Offensive caster and a claim on it was otherwise inert; this bullet said
  "Restore ONLY" until 2026-09-07), and every thunk re-tests {claim actor, claim's
  driven form} per call.
- **RETIRED in the same pass:** the whole `CastExecutor` phase chain
  (PhaseSelect/Rest/Drawn/Fire/Hold, ParkHand/TeardownHand, the wall-clock `Budget`
  plumbing) **and its `CastSpellImmediate` fallback** (which is what INVARIANTS #0
  forbids by name); the ch.8 `+ACT` drive opt-in (`ival` bit 2 — RESERVED and ignored,
  ABI byte-frozen); the `CombatBehaviorContextMagic` CreateContextNode act/pop deny
  (the months-live CTD seat, and now actively wrong — it would suppress the very cast
  being claimed); and `kIntent_Cast`'s implicit `kCombatActionCat_Cast` deny.
- **KEPT deliberately:** the four cast LEAVES' act/pop deny — still load-bearing for
  `kIntent_CombatAction`, a separate live intent ("this actor must not cast at all");
  the delivery-flip proxy pool, renamed `core/CastProxy.{h,cpp}`, because
  `FindTargets`' Self branch (0x5bc98a) always lands on the caster so no seat can aim a
  kSelf heal at an ally — with its `AddSpell`/`RemoveSpell` lifecycle, `ResetAll` and
  `PreSaveSweep` intact (#19); `core/MainThread`, which now carries the release hop.
- **ABI:** `APMF_CastRequest::target` is now LOAD-BEARING (it was RECORD ONLY). Stop
  percent rides `CastFlags` bits 8-15 in-place (`MakeStopPct`/`ReadStopPct`); 0 = stop
  at full restoration. No struct field added, reordered or retyped; `kABIVersion`
  stays 5.
- **CLIENT-SIDE CONSEQUENCE FOR MFO (coordinate before deploying the pair):** a claim
  that still sets the retired `+ACT` bit gets gate-only behaviour and no cast; the cast
  now goes through `RequestCast`. And MFO's own `CasterConsent` hooks slot `0x06` on 14
  caster vtables — if it installs after APMF it sits OUTER and could deny APMF's forced
  YES, so it must not deny while a ch.8b claim stands.
- **NOT FIELD-RUN.** The `AiCastSeats` passive probe stays intact and config-gated (it
  is deliberately installed BEFORE these seats, so it keeps logging the ENGINE's raw
  answer beneath them). First deck run should confirm: `[t2a] SEAT 0x0F` armed ->
  `[ch.8b seat 0x06] YES` -> `[ch.8b seat 0x0A] -> claimed target` -> a real equip +
  animated cast -> `[ch.8b seat 0x07] STOP`. The aim seat has its own kill-switch,
  `[CastSeats] EnableAimSeat=0`, if `+0x30` misbehaves (its lifecycle is the one
  NOT-CERTAIN item carried into the field).

## Earlier branch: `feat/composition-cast` (superseded in part by the seats above)

The composition rework (`Docs/SPEC-COMPOSITION-REWORK.md`). Adds the cast-EXECUTION
facet (ch.8b) as ABI v5. NOT on `main`; `main` stays v0.9.0.

- **ABI v5** (`APMF_API.h`, append-only, byte-shared with MFO): `kIntent_Cast`,
  `RequestCast`/`APMF_CastRequest{spell,proxy,target,flags,ttl}`, `kCastFlag_*`,
  `kCombatActionCat_Cast`.
- **Cast-facet claim**: a `kIntent_Cast` claim fans into the SAME three gates
  cast-select rides (0x0A CheckCast + 0x0F CheckShouldEquip via
  `Allowance::AllowedCast`; the T1 cast leaves via `kCombatActionCat_Cast`) plus a
  bounded TTL auto-release in `ControlMap::Drain`. NO engine cast call (design.md
  §1a). The CLIENT fires its own animated cast; movement untouched.
- **Deny-completeness (branch `feat/deny-completeness`, 2026-09-04)**: the cast deny
  was PARTIAL and CTD'd on the deck — it denied the cast-FIRING path but not the
  cast-CONTEXT-CREATION node, so the AI still built its magic-equip context
  (`CombatBehaviorContextMagic` CreateContextNode) and raced MFO's forced equip to a
  null-`CombatInventoryItem` AV (`call [rax+0x28]` rax=0). FIX: `core/ActionGate.cpp`
  now also denies that node's `act()` (slot 0x02, ForceFail path,
  `apmf::cbt::kCastContextNodes`), classified `Cast|Offense` — symbols verified
  present + ID-backed + RTTI-verified at install. The cast deny is now complete across
  select/fire/setup. New **INVARIANTS #18** (deny completeness) + full
  `Docs/DENY-COMPLETENESS-AUDIT.md`. Open residual: `kIntent_Equipment` weapon-vs-weapon
  (same context-node seat is the fix; scoped to a follow-up pass). CI-only, unpushed.
- **AI-cast suppression + the node-protocol fix (branch `feat/ai-cast-suppress`,
  2026-09-04, off `feat/cast-act` 1da6eac)**: the CTD above RECURRED (three deck logs
  2026-09-04) with the context-node deny live. Disassembling 1.6.1170 against the logs
  showed frame 0 is the tree-thread STEP function walking a garbage `cur_node` during an
  interrupt unwind — corrupted by APMF's own deny: a node's `act()` (slot 0x02) and
  `pop()` (slot 0x03) are a push/pop PAIR on the thread's data stack, and ForceFail'ing
  only `act()` left the node's own `pop()` to pop 0x30 (ContextMagic) for a 4-byte push,
  releasing two NiPointers out of the enclosing frame. FIX: `core/ActionGate.cpp` hooks
  slot 0x03 too and routes a denied node's next `pop()` to ForceFail's own `pop()`
  (thread-local pending record) — a denied node now runs exactly ForceFail's pair.
  SCOPE: the Cast category also arms for a ch.8 `kIntent_SelectSpell` claim carrying the
  +ACT bit, so while APMF drives a cast the AI's whole magic branch (context build,
  self-equip, fire) is silent; gate-only ch.8 unchanged. `kCastingDisabled` evaluated
  and ruled out (downstream of the context build; persisted flag) — full RE in
  `Docs/DENY-COMPLETENESS-AUDIT.md` "The node-protocol fix", INVARIANTS #18 (pair
  bullet), `core/CombatBehaviorRE.h` "The node protocol". Not deck-cycled yet: expect
  zero `[ch.7] paired-pop protocol ANOMALY` lines and no `Magic_Equip_Out` flip-flop on
  a driven actor.
- **`kCastFlag_FromPackage`**: extracts ONLY spell+target and never runs/offers/
  evaluates the package. Ships the DIRECT-form path; the package-data read is the
  §5.1/§6 fallback (refuses cleanly, client passes the spell directly) because the
  pinned CommonLib does not cleanly express it without MFO's hand offsets (#7).
- **Cast-path observer** (`core/CastObserve`, marth 2026-09-04): FULLY PASSIVE,
  always-on, per-actor rate-limited, no hotkeys. Polls loaded actors' MagicCaster
  state machine + registers a passive anim-event sink on casting NPCs, logging the
  exact cast sequence (`[castobs]`) so MFO can OBSERVE-AND-REPLICATE the real cast
  path instead of guessing a trigger.
- **`feat/alias-drive` SHELVED** (design.md §5): package SUBSTITUTION, wrong model.
  Tag `archive/alias-drive-shelved-2026-09-04` (LOCAL only pending push). Nothing
  from it is on `main`; the shipping tree carries no ESL/quest/alias-pool/
  PutCreatedPackage-write.
- **Field test**: MFO heal on Jesper (non-alias). Expect `[ch.8b] ... CLAIMED`,
  MFO's animated `[cast]` line, `[obs] ... PACKAGE STABLE`, and the follower moving
  during the cast. CI-green pending (CI-only build).

## Where we are

**Framing (marth 2026-09-02): APMF is a MODERATOR — it ARBITRATES + DENIES, it NEVER
generates behavior (design.md §1a, INVARIANTS #0).** Its only lever on the engine is
DENY (suppress the losing source at its source). It calls NO behavior-generating engine
function (`StartCombat`, `CastSpellImmediate`, movement drive, anim trigger); the CLIENT
executes behavior with its own proven mechanisms and APMF just makes it win. Once it owns
a facet, nothing else reaches it except through APMF; a re-assert loop is a FAILED block.
**ch.6 (combat-target) is ARBITRATION-ONLY** — the client commands the target; APMF only
records the claim. **ch.8 is ARBITRATION + DENY, not arbitration-only** (corrected 2026-09-07): a `kIntent_SelectSpell` claim is enforced at two gates — `core/CastGate.cpp:124` (0x0A `CheckCast`) and `core/EquipGate.cpp` (0x0F `CheckShouldEquip`) deny any spell/staff that is not the claim's `param.form` (plus its allow-list). The "arbitration-only" wording dates from the 2026-09-02 #0 correction and was never updated when the deny landed the same day. The CHANNEL still makes no engine write; the
enforcement lives one layer down in the gates. (A CTD from a ch.6
`StartCombat` executor is the cautionary case that fixed this drift — see INVARIANTS #0.)

**Phase 1 is built and on `main`: the MULTI-NPC arbiter + the real C-ABI client
API + the full documented channel catalog.** This replaces v0.1.0's single
crosshair-captured target.

- **Multi-NPC control map** (`core/ControlMap`): a hash map keyed by NPC FormID →
  that NPC's control state (engaged channels + per-channel client claims + captured
  package). Any number of NPCs controlled independently and simultaneously.
  - **Performance (#13):** the `0xAD` hook calls `OnActorUpdate` for EVERY NPC every
    frame; an uncontrolled NPC pays an `empty()` check + ONE hash lookup that misses,
    nothing else. Only a controlled NPC runs its channels (most no-op).
  - **Threading — single-writer (#12):** client `Request`/`Release` (any thread)
    only ENQUEUE a POD op under a brief lock; the map is mutated ONLY on the game
    thread — `Drain()` (once/frame, from the PlayerCharacter `0xAD` seat) applies the
    queue, `ReleaseAll()` clears. The per-NPC hot path reads lock-free.
- **Client API (Layer 2) is REAL** (`APMF_API.h` + `core/ClientAPI.cpp`): an
  inter-plugin C-ABI. A separate client DLL (MFO — soon a mandatory prerequisite)
  gets a POD struct of function pointers via the exported `APMF_GetInterface`, and
  calls `Request(actorFormID, intent, basis) -> handle` / `Release(handle)` — or
  `RequestEx(…, const APMF_Param*)` (ABI v2) to name WHICH thing (cast-select's spell,
  combat-target's target). No C++ class / STL / vtable crosses the boundary. `basis`
  arbitrates same-channel
  same-NPC (higher wins; tie → earliest); the channel stays engaged until the LAST
  claim releases. APMF holds ZERO client-specific code (#14); the header + the query
  fn are the ONLY seam. `APMF_API.h` is APPEND-ONLY forever.
- **Full movement block** (ch.1, the reference channel done right): `SetDontMove`
  alone (v0.1.0) blocked translation but not the move INTENT — run-in-place +
  teleport-snap. Now `KeepOffsetFromActor(self, offset 0)` nulls the move GOAL at the
  source (planner sees "already there", produces no locomotion) PLUS `SetDontMove`
  locks translation. Result: a clean stand-still — no walking, no run-in-place, no
  snap. Both Address-Library bound (verified IDs, #8), package left current.

Full nav: `MAP.md`. Design: `design.md` + `Docs/ARCHITECTURE.md`. Rules:
`Docs/INVARIANTS.md`. Per-channel catalog: `Docs/CHANNEL-MAP.md`.

## Built — the FULL documented catalog (first-release baseline, 13 channels)

The first release ships the full commonly-documented catalog as a baseline
benchmark (MFO will exceed it immediately). Each is a small self-registering module
exposed through an `APMF_API::Intent`. Test surface (**OPT-IN, DEFAULT OFF (2026-09-07).** No keyboard sink is registered unless `[Input] EnableTestSurface=1` in `Data/SKSE/Plugins/APMF.ini`, so in a shipped game no scancode below does anything at all (CLAUDE.md: probes are fully passive, config-gated, default OFF).): aim the
crosshair at an NPC + the key ADDS it to the controlled set; aim another + a key adds
it too; **Numpad0 releases ALL**. Logs to `Data/SKSE/Plugins/APMF.log` (`[ctl]`/`[obs]`/`[test]`/`[api]`).

| Key | Ch | Facet | Kind | Mechanism |
|-----|----|-------|------|-----------|
| Num1 | 1 | movement FULL block | source-block | `KeepOffsetFromActor(self)` + `SetDontMove` |
| Num2 | 11 | disposition (4 AVs) | source-block | aggression/confidence/assistance/morality |
| Num3 | 5 | headtrack look-up | **known-incomplete block** | own point slot; Tick re-assert (flagged) |
| Num4 | 8 | casting CLAIM | claim + T2 DENY (gated, not arbitration-only — `CastGate.cpp:124` + `EquipGate.cpp`) | records owner and denies every OTHER spell/staff at 0x0A/0x0F; CLIENT selects the spell + fires (no APMF write) |
| Num5 | 4 | weapon draw | one-shot | `DrawWeaponMagicHands` |
| Num6 | 10 | dialogue pause | one-shot | `PauseCurrentDialogue` |
| Num7 | 1a | gait scale (x0.5) | source-block | `kSpeedMult` AV (arbitrary factor) |
| Num8 | 16 | stealth (silent+keen) | source-block | `kMovementNoiseMult` + `kDetectLifeRange` |
| Num9 | 3 | sneak/crouch | one-shot promote | `NotifyAnimationGraph(SneakStart/Stop)` |
| Num- | 6 | combat-target CLAIM | arbitration-only (#0) | records owner; CLIENT commands the target (no APMF combat call) |
| Num+ | 12 | idle/animation | one-shot | `NotifyAnimationGraph(IdleForceDefaultState)` |
| Num* | 14 | shout select CLAIM | arbitration-only (#0) | records owner; CLIENT selects via its own `EquipShout` (no APMF equip call) |
| Num. | 15 | unequip weapon | source-block | `GetEquippedObject`+`Unequip/EquipObject` |

Every channel keeps the package coherent (no substitution) and restores state on
release / disengage / pre-load-game. Only Headtrack re-asserts (flagged #2). ch.2
facing is not a separate channel — it rides the movement gate.

## Fix pass (trailing review, folded in)

- **C-ABI exception guard (#14):** `APMF_Request`/`APMF_Release`/`APMF_GetInterface`
  each wrapped in `try/catch(...)` — no throw crosses into the client DLL.
- **FormID threaded through Engage/Tick/Release:** channels key per-NPC state by the
  `id` (not `actor->GetFormID()`), so a null/deleted actor still cleans + restores —
  no state-map leak, contract met.
- **KeepOffset reloc IDs VERIFIED** (36870/37894, 36871/37895) against shipping SKSE
  source with the identical signature + verbatim SetDontMove anchor (#8).
- **AV clobber guard:** the ledger stores `{prev, applied}` and restores `prev` only
  when the AV still equals `applied` (else the newer external value wins).
- **Co-save record versioning (v0.2.2):** adding `applied` changed the record layout
  12→16 B, so `kRecordVersion` is bumped to 2 and `Load` branches per version — a v1
  record reads its 12-byte entries and restores UNCONDITIONALLY (no `applied`); a v2
  record uses the clobber guard. A reader per version is kept forever (#15).
- **Log hex formatting (v0.2.2):** on the deck every `{:08X}` rendered as raw garbage
  bytes (corrupting the log to binary) while decimal/strings were clean; the same
  toolchain formats `{:X}` fine for MFO, so the trigger is APMF-build-specific and not
  statically isolable (typo/encoding/arg-type/config/formatter all ruled out). Robust
  fix (#16): all hex now formats via `apmf::log::Hex()` (manual ASCII hex, logged
  through the clean string path); no `{:X}` spec remains in any log call.
- **AvLedger hardening:** `Load(intf, version)` threads the record version; `Save`
  checks `WriteRecordData` and logs on failure.
- **Equipment save-safety:** decided + documented (#15) — only AV channels are
  co-saved; Equipment must not be held across a save (self-heals via AI re-equip).

## Save/load safety (Phase 1)

- **Persisted AV overrides are CO-SAVED (#15).** The AV channels (disposition, gait,
  detection) route every write through `core/AvLedger` (co-saved via SKSE
  serialization), so a save-while-engaged + reload restores the AV regardless of
  live engaged-state and never strands it. `kPostLoadGame` sweeps + clears;
  `OnRevert` wipes ledger + control map.
- **No stale-pointer deref:** every Release resolves the actor FRESH by FormID
  (`LookupByID`) / handle — never a cached raw pointer — so kPreLoadGame with a
  torn-down actor is safe.
- **VR:** input test surface is NOT armed on VR (hooks refuse; no drain seat).
- **Headtrack Release** now actively CLEARS the point slot
  (`ClearActionHeadtrackTarget`), not just stops re-asserting.

## Post-first-release GAP work (do NOT attempt without a live probe)

Marked GAP in `Docs/CHANNEL-MAP.md`; deliberately left for after the first release:
- Combat ACTIONS behavior tree (ch.7).
- Casting TRIGGER suppression (ch.8, no documented suppressor).
- Headtrack all-types FULL block (ch.5) — block the AI's headtrack write at the
  `0xAD` hook so the re-assert stopgap can be removed.
- Sustained package-procedure activities (ch.9).
- Facial-expression setter (ch.13, not exposed in this CommonLib build).
- Per-request FORM/target params in the API (a v2 addition): ch.8 left hand, ch.12
  a specific `TESIdleForm`, ch.14 an arbitrary shout, ch.15 an arbitrary item, ch.6
  an arbitrary target. Today those channels use a fixed demo form/target (the
  CastingSelect-Firebolt precedent); the mechanism is bound and ready.

**Movement PROMOTE is NOT a blocker.** Once the movement source is BLOCKED, driving
the walk is uncontested; wiring the promote feed (`IMovementDirectControl`) is a
driver choice for MFO integration, not a mystery.

**▶ 0x49 PACKAGE-OFFER PROBE (throwaway, on `main`; NOT wired to any client, NOT a
travel/nav build).** Demystifies the ONE intentional package-tier promote (design.md
§5a / INVARIANTS #3): hook `Actor::CheckForCurrentAliasPackage` (vfunc **0x49**) on
`VTABLE_Character` ONLY and return a client's package for a claimed actor → the engine
runs it natively. `native/core/AliasPkgProbe.{h,cpp}`; installed at kDataLoaded after
the 0xAD hook; game-thread eval pump on `Arbiter::OncePerFrame`; test hotkey DIK `0x57`
(F11) toggles a single-actor offer claim on the aimed NPC + `EvaluatePackage(true,false)`
(RELOCATION_ID 36407/37401, resetAI=false). **Phased:** Phase 0 (armed now, no claim,
no package needed) answers the make-or-break — *does 0x49 fire?* (census logs hit count
+ thread + returned pkg; **0 hits ⇒ devirtualised/inlined ⇒ mechanism DEAD, stop**);
Phases 1-3 (engage/release/save-load) now ARMED — `kProbePackageForm` is set to
`DefaultSandboxCurrentLocation256` (Skyrim.esm `0x000956B8`, a vanilla radius-256
current-location sandbox, verified by parsing Skyrim.esm's own PACK group directly,
not guessed), engage/release logs the `ExtraAliasInstanceArray` size before/after
(must be UNCHANGED), and `kPreLoadGame` now drops the claim with no engine call
(Phase 3, no latch). For marth to field-test on Cicero (owner quest 0x0009BE51) or
any generic NPC, 3 deck cycles.

**▶ T1/NATIVE-BIT PROBES (throwaway, on this branch; see `Docs/PROBE-ALLOWANCE.md` for
the full hotkey map, method, and pass/fail criteria; all probe/test hotkeys are numpad
— F-keys are occupied by the game/modlist).** T1 = combat behavior-tree leaf
`Enter`/act (slot 0x02, all 70 `VTABLE_CombatBehaviorTreeNodeObject_*` leaves,
`core/T1Probe.{h,cpp}` + the local `core/CombatBehaviorRE.h` RE:: extension since
CommonLib doesn't ship these classes) — Phase 0 OBSERVE (NumpadEnter claim, shared
with the 0x49 probe) + Phase 1 DENY the Attack leaf via a runtime-derived `SetFailed`
(disassembled from `ForceFail`'s own body, NumpadSlash toggle). Native-bit = a plain
`kAttackingDisabled`/`kCastingDisabled` toggle on the aimed NPC, no hook
(`core/NativeBitProbe.{h,cpp}`, Numpad1/Numpad2). Both field-test-first,
hotkey-driven, NOT wired to any client.

**T4 (`TESActionData::Process` body-command seat) was built, field-CRASHED, and
REMOVED (2026-09-03).** Its devirtualised fallback (`SKSE::GetTrampoline().
write_call<5>` at valhalla's known call site) collided with SCAR.dll's own hook on the
same AI attack-start path — execute-AV in ordinary combat, not even during a probe
keypress (the patch was live from `Install()`). Full crash record in
`Docs/PROBE-ALLOWANCE.md` "T4 — DEFERRED". Not a coverage dead end: T1 already covers
combat body-commands (chain-safe `write_vfunc`), so this falls through to T1 rather
than opening a gap; only the non-combat body-command slice (sneak/draw/activate/idle
OOC) still awaits a chain-safe seat. **New standing rule from this crash:**
`Docs/INVARIANTS.md` #17 — vtable hooks (`write_vfunc`) ONLY, no raw call-site patches
ever, ADAPT to a redundant alternative seat rather than degrade when a preferred one
is contested/absent/devirtualised. Also recorded in `design.md` §10.

## Allowance channels ch.7 / ch.9 graduated (2026-09-03)

The two field-proven allowance probes (`Docs/PROBE-ALLOWANCE.md` — T1 combat
behavior-tree leaf deny, and the 0x49 package-offer redirect) are now REAL,
API-driven channels; the throwaway hotkey-claim surface (`T1Probe`,
`AliasPkgProbe`, `ProbeClaimSet`) is REMOVED — never left installed alongside
the real channels on the same vtables (`Docs/INVARIANTS.md` #17).

- **ch.7 combat-action** (`native/channels/CombatAction.cpp` arbitration +
  `native/core/ActionGate.cpp` enforcement): `kIntent_CombatAction`,
  `APMF_Param.ival` = an `APMF_API::CombatActionCategory` bitmask of leaf
  categories to DENY (starts with `kCombatActionCat_Offense`, append-only for
  future categories). Reuses the proven T1 mechanism verbatim: `write_vfunc`
  slot 0x02 on all 70 `CombatBehaviorTreeNodeObject_*` leaves, the `+0x158`
  A‖B actor-resolution, and `CombatBehaviorForceFail`'s own original `act()`
  as the deny call. A leaf is denied only if its classified category bit is
  set in the winning claim's mask; unclassified leaves (movement/defense/
  utility) are never looked up at all.
- **ch.9 offer-package** (`native/channels/OfferPackage.cpp` arbitration +
  `native/core/PackageGate.cpp` enforcement): `kIntent_OfferPackage`,
  `APMF_Param.form` = the `TESPackage` FormID to offer. Reuses the proven
  0x49 mechanism verbatim: `write_vfunc` on `VTABLE_Character[0]` slot 0x49,
  never-null fallback to the engine's own answer, one
  `EvaluatePackage(true,false)` nudge on Engage/OnOwnerChanged/Release (now
  driven by the Channel lifecycle instead of a hotkey-queued op).
- Both intents (`14`/`15`) and the `CombatActionCategory` enum are
  append-only additions to `APMF_API.h` — no existing field/enum value
  changed. Neither channel needs bespoke `kPreLoadGame` handling: the generic
  `ControlMap::ReleaseAll` already calls every channel's `Release()`.
- ~~**Not yet field-tested** (built + CI-green only, same as every prior
  graduation before its own deck pass) — marth reviews the diff before Pass B.~~
  **FIELD-RUN 2026-09-06, and the result was NOT a pass (recorded 2026-09-07).** ch.7 was not
  exercised. ch.9 ran and engaged **0 of 6** dispatches: the `EvaluatePackage` nudge fires
  BEFORE the claim publishes, so the engine's 0x49 question is answered with the pre-claim
  package (MFO `Docs/DIAG-2026-09-06-loot-travel.md`). The redirect itself is sound — 16/16
  when it is asked with a published claim standing — and the engine contributes ZERO
  evaluations of its own. Fixed by `fix/apmf-offerpackage-nudge-ordering`, MERGED to `main`
  2026-09-07: the nudge is now posted one main-thread hop PAST the claim's publish. **Not
  re-run on a deck yet** -- 0-of-6 has not yet been shown to be 6-of-6.

## Client API (Layer 2) — REAL

`APMF_API.h` (the shared header) + `core/ClientAPI.cpp` (the impl). A client:
`GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface")` → `fn(kABIVersion)`
→ a `const APMF_API_v1*` (null on ABI mismatch); check `p->abiVersion` and cast up to
`APMF_API_v2*` (>=2), `APMF_API_v3*` (>=3), … up to the newest struct the client uses
→ `Request/RequestEx/Release/Repoint/SetSpellAllowList/RequestCast/GetCastProxy/
IsCastActive`. `APMF_GetInterface` returns NULL if the client asks for a version newer
than APMF implements.
`RequestEx` carries the POD `APMF_Param` (`form`/`fval`/`ival`) — cast-select reads
`param.form` as the spell (no param → Firebolt), combat-target as the target (no param
→ player). `Repoint(handle,param)` re-points a live claim in place (same handle) — the
retarget primitive (combat-target switches the held foe without release/re-request).
Forwards to `ControlMap` enqueue (the SAME path the hotkeys use — one control path).
Frozen, append-only (#14/#14a): each ABI = a prefix-extension struct. **The current
`kABIVersion` is in `native/APMF_API.h` and only there** (#14b — this line said 3 while
the header was at 6). New slot ⇒ bump; a new bit in an already-frozen word ⇒ NO bump,
because MFO calls `fn(kABIVersion)` once with no downward retry and a bump turns its
whole owned-cast model off against any older APMF.

## Build / CI

- Compile is **CI-only** (GitHub Actions, `native.yml`, on `native/**`):
  `gh run list -R marthofdoom/APMF`. Windows + vcpkg + colorglass CommonLibSSE-NG,
  same toolchain/baseline as MFO.
- CMake **GLOBs** `native/**/*.cpp` so a new channel needs no build-file edit.
- Pinned CommonLib API-surface gotchas + the verified Address-Library IDs (movement
  block, StartCombat) are in INVARIANTS #8 (verified against the fork's headers, not
  memory).

## Next

*(Rewritten 2026-09-07. The previous list was 2026-09-02 content — "field-test the 13 channels"
and "MFO integration (Phase 3)" have both been done since v0.9.0/v0.9.1, and a stale Next list in
the living handoff is how a finished item gets re-planned.)*

1. **FIELD-RUN what merged on 2026-09-07**, because none of it has been on a deck: the
   renewable cast TTL, `kCastFlag_DenyHandOnly` (dormant until a client sets the bit), the
   spell score steer, the `[t2c]` deny log, and the ch.9 nudge posted past `Publish()`. The
   ch.9 pass criterion is the one that was 0-of-6.
2. **Close the cast facet's REMAINING open gaps** (`Docs/DENY-COMPLETENESS-AUDIT.md`): gap 10
   (a weapon takes the claimed hand — no weapon-side admission gate exists), gap 12 (the
   request-to-publish window) and gap 13 (a spell already charging). Gap 9 needs a CLIENT to
   set the deny-only bit; gap 11 needs a client that RE-POINTS instead of re-requesting.
3. **Finish the double-0x0A migration**: MFO still installs its own `CheckCast` hook outside
   APMF's gate (`Docs/SPEC-GRADUATED-CAST.md` §3, `Docs/HOOK-SITE-COVERAGE.md` §6) — latent
   today, and the collapse of castLvl 1-3 under APMF has never been measured.
4. **Probe the GAP channels** (movement PROMOTE first) on a live runtime — `feat/pfp-phase0-movement`
   is the standing Phase 0 for that, INI-gated and not yet field-run.
5. **Weapon-vs-weapon admission** (open gap 1 / row 15): the weapon CONTEXT-node seat, with its
   own field test for over-suppression.
