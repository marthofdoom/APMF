# Spec: the composition rework — facets, executors, and the cast-intent API

Status: **implementation spec (2026-09-04, Fable). No code in this doc has been applied.**
Corrects the `feat/alias-drive` miss. Governed by `design.md` §1a (the binding contract) and
`Docs/INVARIANTS.md` #0; where anything below or in any older spec reads as APMF *driving*
or *delivering* a package to make an actor cast, §1a wins and that text is a stand-in.

Companion: MFO `Docs/SPEC-FORCED-CAST.md` (the client side — how MFO EXECUTES the animated
cast that this spec only moderates around).

---

## 0. What went wrong (so it is not repeated)

The recent work chain was: MFO wants an ANIMATED heal at an ally → the only thing that ever
produced that was the M9 forced-casting PACKAGE (MFO ENGINE_NOTES §0.17/§0.21) → the package
freezes the body → "APMF owns movement, so route the package through ch.9 and keep movement" →
0x49 does not reach a non-alias follower → put the follower on an APMF-owned alias ladder (ESL
claim quest, 16-slot pool, placeholder Travel package) → the placeholder still wins → push the
client package with `Actor::PutCreatedPackage` (0xDF).

Every step after the first is **PACKAGE SUBSTITUTION**, which `design.md` §5 rejects outright:
the preempted source loses the current-package slot, its procedure stops, `OnPackageEnd` fires,
the actor is torn down or frozen. The deck proved the prediction exactly: the follower ran the
placeholder Travel package (walked to the player / stood frozen) and never cast.

The category error: a HEAL is a **cast facet** (ch.8), not a package facet (ch.9). The M9 package
was only ever the CLIENT's old executor for that facet. When the executor freezes the body, the
answer is a better executor for the same facet, never "have the framework run the freezing thing
and try to steal movement back." §1a rule 2: the CLIENT brings and EXECUTES the behavior.

---

## 1. The corrected model in one paragraph

APMF is a per-facet **moderator**. For every facet it does exactly three things: ARBITRATE
(record the owner in the `ControlMap`), DENY (withhold the losing source's input at the engine's
own choke point — a vfunc gate, an AV the AI reads, a movement full-block), and, for the few
facets with no deny form, PROMOTE a bounded one-shot client-requested action at Engage/Release.
It never manufactures an AI DECISION, never sustains or boosts one, never re-asserts. **The rule
constrains INPUT, not OUTPUT (marth 2026-09-04):** APMF never fabricates input it did not receive
from the API (no invented client intent, no AI decision of its own), but it MAY fabricate OUTPUT —
translate the received intents plus the actor's own non-denied facets into whatever form the
receiving engine seat consumes, including a synthesized PROXY PACKAGE (§4). The CLIENT brings and
EXECUTES decision-shaped behavior through mechanisms it owns (the cast, §3); APMF ensures DELIVERY
by denying competitors around it. **APMF never substitutes a package at a SOURCE** — no source
(framework alias, package stack, base AI) ever sees its package lose the slot (§5 teardown). Where
a package IS the right control form (movement goals, procedure activities), APMF intercepts at the
RECEIVER — the seat between the sources and the NPC where the package is consumed — reads every
competing input, and hands that seat ONE composed amalgamation, unobserved by any source (§4).
ch.9's 0x49 offer remains the one SOURCE-tier path, for a client's real package on an actor the
alias tier already reaches. The NPC runs a **composed proxy**: its own non-denied facets plus the
client's facets, assembled per facet at that facet's receiver.

---

## 2. The facet → executor catalog (aligned to `design.md` §4b)

Column meaning: **APMF lever** = the only thing APMF does. **Client executor** = how the client
makes the behavior (documented so the boundary is visible; NOT an APMF action). **Kind**:
D = deny-only, A = arbitrate-only (no engine call at all), P = bounded one-shot promote,
N = native package offer (the §5a exception).

| ch. | Facet | APMF lever (channel + gate) | Kind | Client executor (client's own call) | Status |
|---|---|---|---|---|---|
| 1 | Movement / locomotion | `MovementDeny.cpp` full-block (`KeepOffsetFromActor(self,0)` + `SetDontMove`) — deny. **Receiver-composed goal (§4.3-A): APMF emits the client's declared goal as the movement OUTPUT at the movement receiver (`Actor::Move` 0xC8 / `ModifyMovementData` 0x11A / `MovementControllerNPC`) while the source package keeps ticking** | D / R | client declares the goal (intent), or runs its own package (ch.9) | block proven; receiver feed = RE spike (§4.6 P4) |
| 1a | Gait | `Speed.cpp` `kSpeedMult` override via `AvLedger` | D | same AV | proven |
| 3 | Stance / sneak | `Stance.cpp` `NotifyAnimationGraph SneakStart/Stop` once | P | same | proven |
| 4 | Weapon draw | `WeaponDraw.cpp` `DrawWeaponMagicHands` once | P | same | proven |
| 5 | Headtrack | `Headtrack.cpp` `SetHeadtrackTarget` — **KNOWN-INCOMPLETE (per-tick re-assert, INVARIANTS #2)** | D* | same setter | needs a real gate; out of scope here |
| 6 | Combat target | `CombatTarget.cpp` record only | A | client's `currentCombatTarget` compare-and-write (MFO `Targeting::Command`) | arbitration proven; deny of a competitor = GAP |
| 7 | Combat actions | `ActionGate.cpp` T1: deny classified behavior-tree leaves by `param.ival` category | D | loadout (ch.15) + disposition (ch.11) | T1 field-proven (offense category) |
| 8 | Cast SELECTION | `CastingSelect.cpp` + `CastGate.cpp` T2c (`MagicCaster::CheckCast` 0x0A) + `EquipGate.cpp` T2a (`CheckShouldEquip` 0x0F): exclusivity — the AI may only cast/arm the claimed spell (+ allow-set) | D | client's `selectedSpells` + own consent + Cast-biased style so the AI DECIDES (the owned cast) | proven (MFO offense path) |
| **8b** | **Cast EXECUTION (new)** | **`CastCompose.cpp` — a composite claim (`kIntent_Cast`, §3) that fans into the ch.8 gates + a ch.7 `Cast` category deny + a bounded TTL. NO engine call.** | **D** | **client fires its own animated cast through the hand `ActorMagicCaster` (MFO `SPEC-FORCED-CAST.md`)** | **new; the deny half is built from proven gates, the executor is the client's** |
| 9 | Package-procedure activity (SOURCE tier) | `OfferPackage.cpp` + `PackageGate.cpp` T3 (0x49 alias offer) | N | the client's REAL package, run natively by the engine | proven for alias-tier actors ONLY (Cicero); non-alias actors → row 9r |
| **9r** | **Package-procedure activity (RECEIVER tier, new)** | **`PackageCompose` (§4): at the package-CONSUMPTION seat, hand the procedure runner ONE amalgamation proxy = the engine's winning package minus denied facets + the client's facets; the source's package stays "current" to every source** | **R** | client declares facet intents (goal / activity); APMF composes the output | **UNBUILT; consumption seat needs RE (§4.6 P1-P3)** |
| 10 | Dialogue | `Dialogue.cpp` `PauseCurrentDialogue` once | D | `InitiateDialogue` | proven |
| 11 | Disposition AVs | `Attribute.cpp` via `AvLedger` | D | same | proven |
| 12 | Idle / anim | `Idle.cpp` one anim event | P | `PlayIdle` / graph events | proven |
| 14 | Shout / power | `ShoutPower.cpp` record only | A | client's own `EquipShout` | arbitration only |
| 15 | Equipment | `Equipment.cpp` gate-only (T2a reads it) / probe unequip | D | client's `EquipObject` etc. | gate proven with ch.8 |
| 16 | Detection | `Detection.cpp` AVs | D | same | proven |
| — | Tier C sustained activities (sandbox/guard/furniture) | row 9r amalgamation proxy (a Sandbox/UseItemAt template carrying the client's anchor + the source's non-denied flags), or ch.9 native offer on alias-tier actors | R / N | client declares the activity + anchor | design; gated on 9r |

Kind **R** (receiver-composed) is the kind this revision adds: APMF fabricates the OUTPUT a
receiving seat consumes, from inputs it received (client intents + the actor's own winning
package). It is not a decision (the goal/activity came from the client), not a source-level
substitution (no source observes it), and not a re-assert (the seat is fed once per consumption,
exactly as the engine feeds it). The cast facet (8b) is NOT kind R: a cast is a decision the
client executes, and its correct output form is the client's own hand cast, not a package.

Nothing in this table manufactures a decision. Every row's engine touch is a deny, an arbitration
record, a one-shot promote, or (kind R) a translation of received intent into the receiver's
consumption form — including 8b, whose only engine touches are the SAME three gates that already
exist (0x0A, 0x0F, T1 leaves) reading one more claim kind.

---

## 3. The cast-intent API (the keystone)

### 3.1 What a client declares

A client that is about to EXECUTE a cast (a spell the AI would not choose — a heal at an ally,
a ward on the player, a buff) tells APMF: *"for the next ≤ N ms, actor X's cast facet is mine:
spell S (and its runtime proxy P), hand H, at target T; keep the AI's own casting and re-arming
out of the way; do not touch movement."* APMF records the owner, denies the competitors at the
gates, and auto-releases at the TTL if the client forgets. APMF fires nothing.

The client may state the intent two ways:

- **Direct** — spell + target + hand in the request (the normal path).
- **From a package** — hand APMF a `TESPackage` FormID and the flag `kCastFlag_FromPackage`.
  APMF reads the cast portion OUT of the package (its `SPELL`-style input and its target) into
  the same claim shape and **never runs, offers, installs, or evaluates the package**. The freeze
  half of a UseMagic/forced-cast package is simply never applied because no package is applied.
  This satisfies marth's keystone literally: a package that would normally freeze-and-cast may be
  handed over with "this is for casting," and only the cast portion survives.

### 3.2 ABI additions (append-only, `APMF_API.h`, bump `kABIVersion` 4 → 5)

```
// Intent enum: append after kIntent_OfferPackage = 15
kIntent_Cast = 16,   // ch.8b CLAIM the cast-EXECUTION facet for a bounded window. DENY (composite).
                     // Param: see APMF_CastRequest via RequestCast; RequestEx(form=spell) is the
                     // degenerate form (no target/proxy/TTL -> default TTL).

// CombatActionCategory: append
kCombatActionCat_Cast = 1u << 1,   // CastImmediateSpell, CastConcentrationSpell, PrepareDualCast,
                                   // CastShout leaves (a leaf may carry several bits).

// Cast flags (new enum)
kCastFlag_None        = 0,
kCastFlag_FromPackage = 1u << 0,   // param.form / req.spell is a TESPackage; extract the cast portion
kCastFlag_LeftHand    = 1u << 1,   // hand hint (default right). Informational for the gates today.
kCastFlag_Concentration = 1u << 2, // client says the executed cast is a held stream (TTL floor applies)

// New POD (v5). All fields optional except actor/spell; zero = "none".
struct APMF_CastRequest {
    RE::FormID  spell;        // the spell the client will fire (or the package if FromPackage)
    RE::FormID  proxy;        // runtime FF-form proxy the client fabricated for delivery (0 if none)
    RE::FormID  target;       // intended target actor (0 = self). RECORD ONLY — APMF never aims.
    std::uint32_t flags;      // kCastFlag_*
    std::uint32_t ttlMs;      // bounded window; 0 -> kCastDefaultTtlMs (4000). Clamped to kCastMaxTtlMs (15000).
};

struct APMF_API_v5 : APMF_API_v4 {
    // Claim the cast-execution facet. Returns kInvalidHandle if extraction fails (FromPackage
    // with no readable spell), never a package run. Safe from any thread (POD captured).
    Handle (*RequestCast)(RE::FormID actor, float basis, const APMF_CastRequest* req);
};
```

`APMF_Param` gains NO new fields (its `form`/`ival` remain enough for the degenerate
`RequestEx(kIntent_Cast)` form: `form` = spell, `ival` = flags). The rich request is its own POD so
the frozen `APMF_Param` layout is untouched.

### 3.3 The claim shape inside the `ControlMap`

`ControlMap::Claim` (`native/core/ControlMap.h:143-158`) gains, at the END (RCU deep-copy stays a
POD copy):

```
RE::FormID    castProxy   = 0;   // second allowed FormID for the cast facet
RE::FormID    castTarget  = 0;   // record only
std::uint32_t castFlags   = 0;
std::uint64_t expiresMs   = 0;   // 0 = no TTL (all existing intents); nonzero = bounded claim
```

`ControlMap::EnqueueCast(actor, basis, req)` → op kind `kCast` → `ApplyRequest` path with
`intent = kIntent_Cast`, `param.form = spell`, plus the three cast fields, `expiresMs =
now + ttl`. If `kCastFlag_FromPackage`: on the WRITER thread inside `ApplyRequest` (game thread,
form lookups legal), call `castcompose::ExtractFromPackage(pkgForm, outSpell, outTarget)`; on
failure log `[ch.8b] cast-from-package: no spell input on 0x%08X -- REFUSED` and drop the op
(the handle the client got is released via the normal `ApplyRelease` path so it is never dangling).

**Extraction (`native/channels/CastCompose.cpp::ExtractFromPackage`)**: read the package's
`packageData` map for the spell-typed input (the CK "Spell"/"SPELL" input of UseMagic and of
MFO's M9 `MFO_CastPackage` procedureType 46) and the package target (`PackageTarget` with a
form or a runtime ref handle). MFO's `Packages.cpp` already contains the input-by-name +
positional-uid lookup this needs (`kInputSpell`, `Packages.cpp:98`; note MFO's own field bug:
the authored input is `"SPELL"`, MFO looked up `"Spell"`) — port that read, not its write. If
the package's target is an alias (targType alias), the extraction records `castTarget = 0`
(unknown) — the client always knows its own target anyway; `castTarget` is a record, never an aim.

### 3.4 TTL expiry — bounded, not a re-assert

`ControlMap::Drain` (writer seat, once per frame) gets one extra pass: for each claim with
`expiresMs != 0 && now >= expiresMs`, apply the same `ApplyRelease` as a client release, logging
`[ch.8b] cast claim 0x%08X expired (ttl %u ms) -- auto-released`. This is a **release**, the
opposite of a re-assert; it exists so a crashed/forgetful client can never leave a standing cast
hold (§5a guardrail 1: "never a standing hold"). Clock: `nonaliasprobe::MonotonicMs()` (already
in core) — move it to a neutral `core/Clock.h` so a probe file is not a dependency of a channel.

### 3.5 What the gates do with a `kIntent_Cast` claim (the deny half)

All three touches reuse existing thunks; none adds an engine call.

1. **`core/CastGate.cpp` (T2c, `CheckCast` 0x0A)** — today it narrows the engine's YES to NO via
   `allowance::Allowed(fid, kIntent_SelectSpell, subjectForm)`. Add: if a `kIntent_Cast` claim
   exists for the actor, the subject is allowed iff `subjectForm == claim.form ||
   subjectForm == claim.castProxy` (or in the ch.8 allow-set); otherwise deny with
   `kMultipleCast`. Implement as `allowance::AllowedCast(fid, subjectForm)` consulted AFTER the
   ch.8 check; both must pass. The client's own cast always names its spell + proxy, so the
   client's executed cast passes; the AI's own competing choice for that hand fails.
2. **`core/EquipGate.cpp` (T2a, `CheckShouldEquip` 0x0F)** — same `AllowedCast` consult: while
   the cast claim stands the AI may not re-arm that actor's hands with any other spell/staff
   (the freeze-free equivalent of "the package holds the spell in hand"). The existing
   off-hand-loan exemption applies to `claim.form`/`castProxy` too.
3. **`core/ActionGate.cpp` (T1, leaf `act()`)** — classify the four cast leaves with the new
   `kCombatActionCat_Cast` bit IN ADDITION to `kCombatActionCat_Offense` (`g_category` is a
   bitmask; the thunk already tests `(mask & leafCat) != 0`). A `kIntent_Cast` claim is treated
   by the thunk as an implicit `kIntent_CombatAction` claim with `ival = kCombatActionCat_Cast`
   for its actor: `TryGetOwningClaim(actor, kIntent_Cast, c)` → deny the cast leaves. Attack /
   block / dodge / movement leaves keep firing — the follower keeps kiting while the client's
   cast plays. (If the client ALSO holds a real `kIntent_CombatAction` claim, both masks OR.)
   The client's own trigger is NOT a tree leaf (MFO executes through the engine's body-command
   seat directly — MFO `SPEC-FORCED-CAST.md` §1.3), so this deny never touches the client's cast;
   it only removes the AI's competing decision for the window.

Nothing here reads `castTarget`. The target is recorded for arbitration/diagnostics and for a
future "two clients want different targets for the same hand" conflict rule; today basis decides.

### 3.6 Channel file

`native/channels/CastCompose.cpp` — ch.8b, `ServesIntent() = kIntent_Cast`. `Engage`/
`OnOwnerChanged`/`Release` are LOG-ONLY (`[ch.8b] 0x%08X cast-execution CLAIMED spell 0x%08X
proxy 0x%08X target 0x%08X ttl %u`), exactly like `CastingSelect.cpp`. No `Tick`. The whole
effect lives in the three gate consults of §3.5. Register with `APMF_REGISTER_CHANNEL`.

### 3.7 Explicit non-goals of the cast intent (the §1a bright line, restated for Opus)

- APMF does not call `CastSpellImmediate`, `StartCharge`/`StartCast`, `InterruptCast`,
  `NotifyAnimationGraph("MRh_…")`, `EquipSpell`, or write `selectedSpells` / `desiredTarget`.
- APMF does not claim ch.1 movement on the client's behalf. A client that wants a root during a
  ritual cast requests `kIntent_MovementBlock` itself, separately, and releases it itself.
- APMF does not offer/install/evaluate the package handed in via `kCastFlag_FromPackage`.
  `PackageGate`'s 0x49 thunk only ever reads `kIntent_OfferPackage` claims; a `kIntent_Cast`
  claim is invisible to it by construction.

---

## 4. The package / movement facets — the RECEIVER model and the amalgamation proxy

This section answers the hole the first draft flagged ("non-alias package selection needs an RE
spike") with marth's two corrections: (1) fabricating OUTPUT from received input is allowed and
intended; (2) it is safe only at the RECEIVER, never at a source. What follows is the mechanism,
then an honest ledger of what must be reversed before it can be built.

### 4.1 The input/output rule, precisely

- **Input APMF may use:** every client intent it received through the API (a travel goal, an
  activity + anchor, a spell, a deny mask), plus the actor's own current engine state that APMF
  can READ (the package the engine's own arbitration picked, its flags, its target). Nothing else.
  APMF never invents a goal, a target, or a decision.
- **Output APMF may fabricate:** whatever form the receiving seat consumes — a movement command,
  or a runtime-synthesized `TESPackage` that is the AMALGAMATION of (the engine-picked package's
  non-denied facets) + (the client's facets). This is the "proxy package of the correct
  non-denied results" the design describes. Fabricated output is session-only, never serialized,
  never enters any source's data (no alias, stack, run-once, base-AI list is written).

### 4.2 Why the receiver reconciles fabrication with §5

Skyrim's package pipeline for one actor is `SOURCES → ARBITRATION → RECEIVER → BODY`:

- **SOURCES** offer packages: base AI list (`TESAIForm::aiPackages`), quest aliases (queried
  through `Actor::CheckForCurrentAliasPackage` 0x49), the process package stacks (default /
  combat-override — ALYSLC writes `packageStackMap[kDefault]->forms[0]`, MFO ENGINE_NOTES
  §0.17:428-430), run-once (`AIProcess::SetRunOncePackage`), created/temp (`PutCreatedPackage`
  0xDF), scenes, PapyrusUtil script overrides (above all of these, §5b).
- **ARBITRATION** = `Actor::EvaluatePackage` (`RELOCATION_ID 36407/37401`, non-virtual): ranks
  the offers, and if the winner differs from `currentProcess->currentPackage.package` performs
  the switch — `OnPackageEnd` on the old, write the new, `OnPackageStart`, reset the procedure
  state. Unchanged winner = no-op (MFO §0.7, ALYSLC's `a_evaluateOnlyIfDifferent`).
- **RECEIVER** = the per-tick consumption of `currentProcess->currentPackage` by the AI tick
  under `Actor::Update` (0xAD): the procedure runner reads the current `ActorPackage` (package +
  its per-actor runtime data + procedure index) and emits body commands — movement goals into
  the `MovementControllerNPC`/`ActorMover`, a cast into the hand caster, an idle, etc.
- **BODY** = the movement virtuals (`Actor::Move` 0xC8, `ModifyMovementData` 0x11A), the
  casters, the animation graph.

Every source's OBSERVABLE state is on the left of the receiver: its alias fill, its stack entry,
`currentPackage.package` (what `GetCurrentPackage()` and the `OnPackageEnd/Start/Change` events
are computed from). `feat/alias-drive` operated on the left (an alias fill + a temp package), so
arbitration switched `currentPackage` and every source saw it — the §5 teardown. A receiver-level
interception leaves everything on the left untouched: the winner P stays `currentPackage.package`,
arbitration keeps no-op'ing on "unchanged," no event fires, no alias moves — and the runner is
handed the amalgamation Q instead. **No source can observe a substitution it has no read on.** That
is the reconciliation: §5 forbids source-level substitution; §4 composes at the receiver.

### 4.3 The two receiver seats

**A — the MOVEMENT receiver (known, virtual, chain-safe).** The movement facet's consumption
point is the movement layer itself: `Actor::Move` (vtable 0xC8), `Actor::ModifyMovementData`
(0x11A), and the `MovementControllerNPC` / `IMovementDirectControl` feed (design.md §4). P keeps
ticking as the current package, computes its own path, and its movement OUTPUT arrives at this
seat; APMF's ch.1 already denies it there (full-block). Kind R at this seat = replace P's
movement output with the client's declared goal's movement output. Because P still ticks, this is
the FULL §5 story — truthful non-arrival — with no package fabricated at all. **Loot-travel and
town-nav are movement-only intents; this is their primary seat.** What is missing is the feed
format (§4.6 P4); the hook seats are already the ones design.md names.

**B — the PACKAGE-EXECUTION receiver (the amalgamation proxy; needs RE).** For Tier C
procedure activities (sandbox-at-anchor, use-furniture, guard, eat) there is no facet-level body
seat — the behavior IS a procedure. Kind R here = hand the procedure runner a fabricated
`TESPackage` Q for the consumption window while `currentPackage.package` stays P for every
reader on the left. Q is built from received input only:

- **template**: one of the engine's shipped procedure templates (Travel `Sandbox`, `UseItemAt`,
  `Guard`… — 104 usable templates, MFO §0.17:511) cloned into a session-only pool
  (`IFormFactory` `TESPackage`; like MFO's `ConcProxy` 0xFF forms, never serialized, `Reset` on
  load/revert);
- **client facets**: the target/anchor (`PackageTarget` runtime handle — the `PackageLocation::
  data.refHandle` write MFO already proved correct on loot-travel), the activity's inputs;
- **non-denied source facets copied from P**: the package flags the engine reads per tick
  (`foFlags`/`interruptFlags`: allow-combat, weapon-drawn, hellos, headtracking, etc.), speed,
  ignore-combat — so the actor behaves as P's author intended in every facet the client did not
  claim;
- **denied source facets**: P's own goal/procedure, dropped (that is the deny).

Q never has conditions, priority, or schedule — it never enters arbitration. The runner consumes
it exactly as it would consume P.

### 4.4 The consumption window and save safety

Q must exist ONLY inside the consumption call. The discipline: the 0xAD thunk (already APMF's
seat on every NPC tick) swaps Q in for the duration of `orig(self)` and restores P after —
`currentPackage` reads P at every instant outside that window, including any SKSE `kSaveGame`
and any script `GetCurrentPackage()`. This is not a re-assert: it is a per-consumption
translation, the same cadence at which the engine itself reads the field. **Whether the window
can be the whole `orig(self)` call depends on where arbitration runs relative to consumption
(§4.6 P1)** — if `EvaluatePackage` executes INSIDE `Actor::Update`, it would compare against Q,
see a "change," and switch (teardown). Then the window must be narrowed to the runner call
itself (a downstream seat, P2), or arbitration must be given P while the runner is given Q. That
ordering question is the single fact that decides seat B's shape; it is not known today.

`ActorPackage` carries per-actor runtime state beside the package pointer (`data`, procedure
index). Swapping the pointer without its state is undefined; APMF must hold TWO complete
`ActorPackage` records (P's, engine-owned and untouched; Q's, APMF-owned and created the way the
engine creates package data on start) and swap the record, not the pointer (P3).

### 4.5 Coherence: truthful state and time-sharing, per seat

- **Seat A** — P ticks, observes truthful non-arrival (§5 verbatim). Script-tier arrival
  watchdogs still fire on their own clocks → §6 time-sharing unchanged: yield the movement
  receiver back to P on a cadence shorter than the shortest known watchdog.
- **Seat B** — P does NOT tick while Q is consumed; P's coherence is *non-observation* (no event,
  no field change, no alias change) rather than truthful non-arrival. Same watchdog exposure as
  A, so the same time-sharing: yield windows during which P is consumed again (Q not swapped in),
  so P advances/arrives on its own cadence. Q's runtime state must survive a yield (it is APMF's
  own record) — or be rebuilt on the next window if the runner resets it (P3 tells which).
- **Both seats** — reading the competing inputs is on the LEFT of the receiver and already
  visible to APMF: `currentProcess->currentPackage` (the engine's winner among unaware sources)
  at 0xAD; the alias-tier offer at the 0x49 thunk (`orig(self)`'s answer is logged today by the
  observe line); the client intents in the `ControlMap`. Composition = deny map per facet, applied
  to P's facets, unioned with the client's.

### 4.6 The honest RE ledger for the receiver (passive probes, same 0xAD seat as the cast observer)

Nothing in §4.3-B and half of §4.3-A is buildable until these are confirmed. Each is a
PASSIVE, rate-limited, config-gated observation (no hotkeys, no behavior change) that can ride the
cast-observer instrumentation the APMF Opus agent is adding at 0xAD.

- **P1 — is arbitration inside or outside `Actor::Update`?** In the 0xAD thunk set a thread-local
  `inUpdate` around `orig(self)`; in the 0x49 thunk (called from `EvaluatePackage`) record
  whether `inUpdate` is set and, if set, the elapsed µs since `orig` began. The 0x49 census (500k
  hits / 5 min) says evaluation is frequent, so the answer arrives in seconds. Result decides the
  window (§4.4): outside → the whole `orig(self)` call is the window (simple); inside → seat B
  needs the downstream runner seat (P2).
- **P2 — where is the package-execution runner, and is it virtual?** Passive candidates to
  confirm from the pinned CommonLib headers first (no runtime needed): the `Actor`/`AIProcess`
  virtuals between 0xAD and the movement virtuals that take the current package; the `TESPackage`
  virtuals the runner calls per tick (`TESPackage` has its own vtable —
  `Docs/PROBE-NONALIAS-PACKAGE.md` §4 lists `BGSProcedureTreeProcedure` slots as unreversed; a
  `TESPackage` vfunc invoked once per tick per actor with the actor as argument would be a
  chain-safe, index-stable receiver seat). Runtime passive confirmation: an observe-only
  `write_vfunc` on the candidate slot logging (actor, package, tick) — if it fires once per NPC
  tick with `package == currentPackage.package`, it is the runner's read. If no virtual exists,
  seat B is a call-site patch — FORBIDDEN by INVARIANTS #17 (the T4/SCAR lesson) — and seat B is
  shelved; Tier C activities then remain "compose from primitives or ch.9 on alias-tier actors."
- **P3 — `ActorPackage` runtime record.** From the pinned headers: fields of `RE::ActorPackage`
  (`package`, `data`, `currentProcedureIndex`, …) and how `data` is created (a `TESPackage`
  virtual at package start?). Passive runtime: at 0xAD log, per claimed actor, the record's fields
  and flag every change (does `data` get recreated on a switch; does the procedure index reset;
  does anything mutate `data` between ticks — the sign it is the runner's state). This decides
  whether Q's record can be held across yields.
- **P4 — the movement feed format (seat A).** Observe-only `write_vfunc` on `ModifyMovementData`
  0x11A (and/or `Actor::Move` 0xC8) for claimed actors only, logging the `MovementData` the
  package produced (heading / speed / goal) per tick. That is the format APMF must EMIT for the
  client's goal. Pathing for that goal is the remaining piece: the engine's own path request to a
  ref (the `PathToReference` native — a script-tier movement request) must be checked for how it
  is implemented: if it is a direct `MovementPathManager` request it is a legitimate output
  translator for seat A; if it installs a created package (visible at the existing 0xDF observe
  hook) it is source-level and rejected. Until P4 lands, ch.1 remains deny-only and loot-travel
  keeps MFO's own alias package.
- **Rejected without a probe:** `PutCreatedPackage` 0xDF (source tier, measured), alias-ladder
  fills (source tier, measured), any write to `packageStackMap` / run-once (source tier by
  definition), any raw call-site patch (#17).

Dependencies on the rest of this spec: **none.** §3 (cast) ships independently; §4 is the
foundation for loot quiet-hold / town-nav / Tier C and is gated on P1-P4.

### 4.7 The completeness principle — every combination composes

Facets are independent claims on independent receivers; a combination is the union of claims,
and the source's package stays current underneath. (M = movement deny 1, G = movement goal 1/R,
C = cast-execution 8b, S = cast-selection 8, T = combat-target 6, A = combat-action 7, P =
package offer 9, Q = amalgamation proxy 9r, E = equipment 15):

| Wanted behavior | Claims | Who executes what | Why it composes |
|---|---|---|---|
| Animated heal at an ally, follower keeps moving (THE case) | C (spell+proxy, ttl) | client fires the hand-caster cast (MFO SPEC-FORCED-CAST); AI keeps movement, blocking, attacks | C denies only 0x0A/0x0F/cast-leaves; ch.1 untouched; package untouched |
| Same, but rooted for a ritual | C + M | client casts; APMF full-blocks locomotion | M is a separate deny; released independently |
| AI-decided offense cast (owned path, today) | S (+T) | client sets `selectedSpells`, consent, target write; the AI decides | unchanged |
| Owned offense right hand + forced heal left hand (dual-hand) | S (right, allow-set) + C (left, `kCastFlag_LeftHand`) | AI casts right; client fires left | gates allow BOTH named spells; see §7 risk 3 |
| Walk to a loot item / merchant (non-alias follower, e.g. Jesper) | G | APMF emits the goal's movement at seat A; P (his follow package) keeps ticking, sees non-arrival | seat A, §5 verbatim; gated on P4 |
| Same on an alias-tier follower (Cicero) | P or G | engine runs the client's real Travel package natively (P), or seat A | P is source-tier but a sanctioned §5a offer; G needs no alias at all |
| Walk to a merchant AND heal the player on the way | G (or P) + C | movement from the goal; client fires the cast | disjoint receivers |
| Sandbox at a town anchor with the framework's own flags (Tier C) | Q | APMF hands the runner a Sandbox amalgamation (client anchor + P's flags) | seat B; gated on P1-P3 |
| Steal/sneak positioning | G + Stance + Detection + A(offense) | goal at seat A; promotes; leaf deny | all deny/promote/R |
| Hold position, stop attacking, keep looking at X | M + A(offense) + Headtrack | — | Headtrack's re-assert is the one known-incomplete row |
| Two clients, same actor, both want the cast facet | two C claims | higher basis owns; the other's `AllowedCast` fails until owner releases | ControlMap tie rules, unchanged |
| Two clients, same actor, different movement goals | two G claims | higher basis owns the receiver; time-sharing yields to P, never to the loser | basis + §6 cadence |

No combination requires a SOURCE-level package change, and no facet's deny lives in a source.
Where a package is the right output form (Q), it is fabricated at the receiver from received
input. The one thing completeness still waits on is engineering, not design: the receiver seats
(§4.6), which is precisely the §1a "one genuinely hard job," now located.

---

## 5. DECISION: SHELVE `feat/alias-drive`

**Shelve it. Do not merge.** marth pre-authorized: "if it shelves, we shelve." It is the wrong
model, and it was measured wrong:

- Both of its mechanisms are SOURCE-level package SUBSTITUTION: the alias-ladder fill makes an
  APMF quest alias (priority 90) the actor's current-package source; `PutCreatedPackage(pkg,
  temp=true, created=false)` pushes a temp package over the current one. Either way arbitration
  switches `currentPackage` and the framework's package leaves the slot → the exact `OnPackageEnd`
  teardown §5 rejects. It was the wrong LEVEL, not merely the wrong package: fabricating a proxy
  is allowed (§4.1), but only at the receiver (§4.2), where no source can see it.
- Deck: the placeholder Travel package won 91:52 over the client package, and the follower stood
  frozen / travelled to the player instead of casting. That is the design's predicted failure,
  not a tuning problem.
- It was built to deliver a CAST, which is not a package facet at all (§0). With §3 in place its
  motivating use case no longer exists.

**Concretely:**

1. Leave `feat/alias-drive` unmerged; tag its head `archive/alias-drive-shelved-2026-09-04`
   (annotated, pushed) so the 0xDF binding and the ESL generator are recoverable, then stop
   building on it. Nothing from it is on `main`; `main` needs no revert.
2. Files that stay OFF main: `native/core/AliasPool.{h,cpp}`, `Docs/SPEC-ALIAS-DRIVE.md`,
   `APMF_GenerateESP.py`, `out/APMF.esl`, `out/SEQ/APMF.seq`, the `OfferPackage.cpp` /
   `plugin.cpp` hunks that call `aliaspool::*`, INVARIANTS #3b, the MAP.md AliasPool section.
   APMF ships NO ESL, NO quest, NO alias, NO co-save for a package pool.
3. `Docs/PROBE-NONALIAS-PACKAGE.md` §3's "0xDF worth a probe" line gets a one-line verdict:
   *probed via alias-drive; it is a substitution (temp package replaces current), rejected.*
4. Retire the memory-level framing "package-driven facets must drive via an alias slot carrying
   the client package" (it was the road to this miss). The correct framing: **a package-driven
   facet is either a §5a native OFFER of a real client package (source tier, alias-tier actors),
   or a receiver-composed amalgamation (§4, any actor) — never a source-level substitution.**
5. ch.9 / `PackageGate.cpp` 0x49 stays exactly as on `main`: the ONE intentional package-tier
   path, for real native packages, on actors the alias tier already reaches.

---

## 6. What is proven, what needs an RE spike, what is version-fragile (honest ledger)

**Proven (field):** 0xAD seat multi-thread + RCU ControlMap; T1 leaf gate (offense); 0x49 offer
on alias-tier actors (Cicero, Phases 1-2); ch.1 full-block; AV channels; MFO's owned-cast via
0x0A/0x0F exclusivity (MFO's own hooks on the same slots — APMF's T2c/T2a are the same mechanism,
CI-green, field-confirmation pending per STATUS.md).

**Built but unproven:** none of §3 exists yet. After Opus builds it the field test is the MFO
heal on Jesper (the non-alias follower that exposed the miss): expect `[ch.8b] … CLAIMED`, the
MFO `[cast]` animated-path line, `[obs] pkg=0x0005C84B [PACKAGE STABLE]` (his own package never
changes), and the follower moving during the cast.

**Needs an RE spike (do not guess) — the receiver seats, §4.6 P1-P4:**
- P1 arbitration-vs-consumption ordering under 0xAD (decides the consumption window);
- P2 the package-execution runner seat and whether it is a chain-safe virtual (decides whether
  seat B — the amalgamation proxy — is buildable at all; a call-site-only runner shelves it);
- P3 the `ActorPackage` runtime record (decides how Q's state is held across yields);
- P4 the movement feed format at 0x11A/0xC8 + how the engine's own path-to-ref request is
  implemented (decides seat A — the movement goal — which loot-travel/town-nav need first).
The non-alias loot-travel / town-nav cases are NOT a package-SELECTION problem any more (the
first draft's framing) — selecting a different package for the actor at the source is exactly
what §5 forbids. They are receiver problems, and until P4 lands the client keeps its own alias
quest (MFO's loot alias works by priority today). Nothing in §3 depends on any of these.

**Version-fragile (already carried, unchanged by this spec):** ActionGate's `+0x158` attacker
walk; EquipGate's `attackerHandle` 0x28 / `<0x68` static_asserts; the MSVC RTTI walk in
`Allowance::DerivesFrom`; 30 `CombatInventoryItemMagicT` instantiations by symbol. The new code
adds only a package-data READ for `kCastFlag_FromPackage` — use CommonLib accessors on
`TESPackage::data`/`PackageTarget`, never raw offsets; if the read is not cleanly expressible in
the pinned CommonLib, ship `kIntent_Cast` with the direct form only and log the package form as
unsupported (the client always has the spell in hand anyway). Do not let the extraction
convenience block the facet.

---

## 7. Risks

1. **Double 0x0A hook with MFO** — MFO's `CasterConsent` hooks the same `CheckCast` slot. Chain
   order is install order; both are chain-safe `write_vfunc`. The MFO-side abort bug (its
   "concentration unbounded" hard-abort) lives in MFO, not here — see MFO `SPEC-FORCED-CAST.md` §4.
2. **TTL vs a long concentration stream** — `kCastFlag_Concentration` sets a floor of the
   client's declared TTL (clamped to 15 s); the client is expected to `Repoint`/re-request for a
   longer stream, which is a new bounded claim, not a re-assert of the same one.
3. **Cast-leaf deny vs the owned right-hand cast** in the dual-hand case: denying the
   `CastImmediateSpell` leaf for the window also denies the AI's own right-hand cast. First
   build: accept (the window is a few seconds). Refinement if it shows: make the cast-leaf deny
   opt-in via `kCastFlag_DenyAiCast` so a client composing S + C can leave the AI's own cast
   leaves alive and rely on 0x0A exclusivity alone.
4. **Extraction on a package without a spell input** — refused, never a package run (§3.3).
5. **ABI** — v5 is prefix-extension; `APMF_GetInterface(4)` callers see nothing new.
   `kIntent_Cast = 16` and `kCombatActionCat_Cast` are appended; nothing renumbered.

---

## 8. Files an implementer touches (Opus checklist)

- `native/APMF_API.h` — `kABIVersion=5`, `kIntent_Cast`, `kCombatActionCat_Cast`, `kCastFlag_*`,
  `APMF_CastRequest`, `APMF_API_v5::RequestCast`; field-usage table + comments.
- `native/core/ClientAPI.{h,cpp}` — `APMF_RequestCast` export body (try/catch, POD copy,
  `ControlMap::EnqueueCast`); `g_api` becomes `APMF_API_v5`.
- `native/core/ControlMap.{h,cpp}` — `Claim` cast fields + `expiresMs`; `kCast` op;
  `EnqueueCast`; `ApplyRequest` cast branch (+ FromPackage extraction on the writer thread);
  TTL expiry pass in `Drain`.
- `native/core/Allowance.{h,cpp}` — `AllowedCast(actorFid, subjectForm)`.
- `native/core/CastGate.cpp`, `native/core/EquipGate.cpp` — consult `AllowedCast` after the
  ch.8 check.
- `native/core/ActionGate.cpp` — add the `Cast` category bit to the four cast leaves; thunk
  treats a `kIntent_Cast` claim as `ival = kCombatActionCat_Cast`.
- `native/channels/CastCompose.cpp` (new) — ch.8b log-only channel + `ExtractFromPackage`.
- `native/core/Clock.h` (new, tiny) — `MonotonicMs()` moved out of `NonAliasProbe`.
- **Receiver probes (§4.6, passive, config-gated, alongside the 0xAD cast observer):** P1
  thread-local `inUpdate` flag in `Hook.cpp`'s 0xAD thunk + a counter/latency line in
  `PackageGate.cpp`'s 0x49 thunk; P3 per-claimed-actor `ActorPackage` field/change log at 0xAD
  (extend `NonAliasProbe`'s existing `[obs]` line, rate-limited); P4 observe-only `write_vfunc`
  on `Actor::ModifyMovementData` 0x11A logging `MovementData` for claimed actors; P2 is a
  headers-first task (list `TESPackage`/`AIProcess` virtuals invoked per tick with the actor)
  before any runtime probe. No probe changes behavior; none ships enabled.
- Docs: `CHANNEL-MAP.md` row 8b; `INVARIANTS.md` new invariant "a cast is never a package"
  (+ note that `kIntent_Cast` claims are TTL-bounded); `INTEGRATION.md` the v5 call;
  `STATUS.md`; `MAP.md`; `CHANGELOG.md`; `PROBE-NONALIAS-PACKAGE.md` §3 verdict line (§5.3).
- Git: tag + leave `feat/alias-drive` unmerged (§5.1).
