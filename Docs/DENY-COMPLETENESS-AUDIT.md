# Deny-completeness audit (INVARIANTS #18)

Per-facet enumeration of EVERY path the competing source (native combat AI, a
foreign framework, a running package) can reach a facet APMF accepts a claim on,
and whether the deny ZEROES that facet across ALL of them. Born from the cast/equip
CTD (2026-09-04): a `kIntent_Cast` claim denied the cast-FIRING path but not the
cast-CONTEXT-CREATION path. #18 makes the enumeration obligation explicit; this file
is the enumeration. The second half of that same day taught the companion rule: a
deny must honor the denied seat's OWN PROTOCOL (the act()/pop() pair) — see "The
node-protocol fix" below.

**How to read "complete?"**
- **YES (source-gate)** — APMF owns the ONE input the AI reads for that decision;
  there is no other path. Cleanest shape.
- **YES (gated)** — every enumerated AI path to the facet is denied at a hook
  (engine-answer-first, flip YES→NO only, #17); nothing the AI does reaches the
  facet while the claim stands.
- **YES (promote / no competing source)** — a one-shot promote (#0c) over a facet
  with no continuous competing write; nothing to deny.
- **PARTIAL / GAP** — a residual path leaks. Either flagged already (#2
  known-incomplete) or newly found here. Each carries its concrete fix or the RE it
  needs. A GAP is documented, never silent (#18).

## Table

| # | Intent (ch.) | Mode | Source paths to the facet | Current deny | Complete? | Gap / fix |
|---|---|---|---|---|---|---|
| 1 | `kIntent_MovementBlock` (ch.1) | DENY full-block | package planner → `MovementControllerNPC` → `ActorMover` (move goal + translation) | `KeepOffsetFromActor(self,0)` nulls the goal at source **+** `SetDontMove` locks translation (`channels/MovementDeny.cpp`) | **YES (source-gate)** — both the intent (goal) and the execution (translation) paths are zeroed; stand-still holds even on a package-locked follower (deck-proven) | — |
| 1a | `kIntent_Gait` (ch.1a) | DENY AV | `kSpeedMult` AV = the sole pace input | set `kSpeedMult` via co-saved ledger (`channels/Speed.cpp`) | **YES (source-gate)** — one input, owned | — |
| 2 | `kIntent_Disposition` (ch.11) | DENY AV | `kAggression`/`kConfidence`/`kMorality`/`kAssistance` — the AVs the engine's own combat/flee/assist decisions read | set the AVs via co-saved ledger (`channels/Attribute.cpp`) | **YES (source-gate)** — biases the AI's own decision at its only input; no other path | — |
| 16 | `kIntent_Detection` (ch.16) | DENY AV | `detectionModifier` + `kDetectLifeRange`/`kMovementNoiseMult` AVs | set the detect AVs (`channels/Detection.cpp`) | **YES (source-gate)** for the detection-AV facet | Sneaking POSTURE is a SEPARATE facet (ch.3 stance) — see row ch.3; not a leak of this facet |
| 4 | `kIntent_SelectSpell` (ch.8), BARE (gate-only) | ARBITRATE + DENY (exclusivity) | AI charges a spell (`CheckCast` 0x0A); AI equips a spell/staff to hand (`CheckShouldEquip` 0x0F) | deny any spell/staff ≠ `param.form` at BOTH gates (`core/CastGate.cpp`, `core/EquipGate.cpp` via `Allowance::Allowed`) | **YES (gated)** — this facet NARROWS the AI to one spell (it does NOT stop casting). The magic context-node BUILD is *correct* here (it builds context for the claimed spell, which the client WANTS its AI to cast), so denying it would be wrong — deliberately not denied under a bare SelectSpell | — (context-node deny is `kIntent_Cast` / ch.8 +ACT / Offense-only by design, see rows 4+ACT and 8b) |
| 4+ACT | `kIntent_SelectSpell` (ch.8) with `kActFlag_Drive` (+ACT: APMF drives the cast) | ARBITRATE + DENY + EXECUTE (`core/CastExecutor.cpp`) | (a)(b) as row 4; **(c) the AI's autonomous magic branch** — the `CombatBehaviorContextMagic` CreateContextNode (builds the magic context over the EquipContext's item, self-equips the spell, descends to the cast leaves) and the four cast leaves | (a)(b) as row 4 (the claim's spell + its delivery proxy are allowed); **(c) `core/ActionGate.cpp` reads the +ACT bit off the winning ch.8 claim and denies the ContextMagic node + the 4 cast leaves as ForceFail's act()+pop() PAIR for the WHOLE claim window** (feat/ai-cast-suppress) | **YES (gated), per-actor.** The AI never builds a magic context, never self-equips a spell against the drive's `EquipSpell`, never fires alongside the driven cast; melee/ranged/movement/targeting nodes are untouched; the drive (EquipSpell / anim events / MagicCaster) never routes through the tree, so it is unaffected. Arm = the claim's Drain publish, disarm = release publish — one RCU snapshot read, no separate flag | Per-hand at (c) is the same documented gap as row 8b (d) |
| 8b | `kIntent_Cast` (ch.8b) | ARBITRATE + DENY (execution) | (a) AI charges a competing spell (`CheckCast` 0x0A); (b) AI re-arms a competing spell/staff (`CheckShouldEquip` 0x0F); (c) AI fires — the 4 cast leaves (`CastImmediate/Concentration/PrepareDualCast/CastShout`, T1 `kCombatActionCat_Cast`); **(d) AI BUILDS its magic context** (`CombatBehaviorContextMagic` CreateContextNode — reads the EquipContext's `CombatInventoryItem` through the thread's context window, constructs the magic context, descends into the magic subtree) | (a)(b) `Allowance::AllowedCastForHand` allow only claim spell+proxy **on the claim's own hand**, PER-HAND (see below); (c) T1 cast-leaf deny, PER-ACTOR; **(d) T1 deny of `apmf::cbt::kCastContextNodes`** (`core/ActionGate.cpp`), PER-ACTOR; (c)/(d) are ForceFail's **act()+pop() PAIR** (feat/ai-cast-suppress); all TTL-bounded auto-release | **YES (gated), per-actor complete; PER-HAND at (a)/(b) only.** Path (d)'s seat has NO native hand signal (re-derived from CombatPathingRevolution's own `CombatBehaviorTreeNode`/`CombatBehaviorTreeControl` — fixed 10-vfunc interface, no per-instance hand field) — it stays PER-ACTOR by necessity, a documented gap, not an oversight | **Per-actor FIXED 2026-09-04 (seat) and RE-FIXED 2026-09-04 (protocol): the act()-only form of (c)/(d) was itself the recurring CTD — see "The node-protocol fix".** Per-hand at (a)/(b) ADDED (feat/deny-perhand): `CastGate.cpp` resolves the caller's hand via `MagicCaster::GetCastingSource()` (vtable slot 0x15); `EquipGate.cpp` via `CombatInventoryItem::itemSlot.equipSlot` vs. `BGSDefaultObjectManager`'s Left/Right Hand default objects. Both feed `Allowance::AllowedCastForHand`. (c)/(d) remain PER-ACTOR |
| 14 | `kIntent_CombatAction` (ch.7) | DENY (category) | 70 combat behavior-tree leaves' `act()`/`pop()` (slots 0x02/0x03) | ForceFail-PAIR the leaves whose classified category bit is set in `param.ival` (`core/ActionGate.cpp`); "offense" classified today | **YES (gated) for the offense category** — the deny zeroes exactly the named category's leaves; every other leaf fires natively | SCOPE (not a leak): only "offense" is classified today; defense/movement/utility categories are future work (a claim only denies what it names — CHANNEL-MAP ch.7). Also: offense leaves' CONTEXT nodes (Melee/Ranged CreateContext) are NOT yet denied — same class as the cast-context fix; harmless today (no forced melee equip), flag if a client ever force-equips a weapon under an offense claim |
| 15 | `kIntent_Equipment` (ch.15) | ARBITRATE + DENY (input-gate) | AI equips a spell/staff (`CheckShouldEquip` 0x0F, 30 magic/staff vtables); **AI equips a competing WEAPON**; **AI BUILDS a weapon-equip CONTEXT** (AcquireWeapon / ContextMelee / ContextRanged CreateContextNode) | deny spell/staff re-arm at 0x0F while the weapon-order claim holds (`core/EquipGate.cpp`) | **PARTIAL** — the spell/staff-suppression path is gated, but the weapon-vs-weapon path is **NOT**: `CombatInventoryItemMelee`/`Ranged` have NO concrete C++ class in the pinned CommonLib, so 0x0F cannot be hooked for weapons. The AI can still equip a *different weapon*; a client that force-equips a weapon concurrently is exposed to the same context-creation race class | **GAP (documented).** Fix path found + symbols verified: deny the weapon CONTEXT nodes as the same act()+pop() pair — `VTABLE_CombatBehaviorTreeCreateContextNode_CombatBehaviorContextAcquireWeapon_` (Offsets_VTABLE.h:3430), `...ContextMelee...` (3719), `...ContextRanged...` (1784), all RTTI-backed. NOTE those nodes' own push sizes must be irrelevant to the pair (they are: the pair is ForceFail's own 4/4). Scoped OUT this pass (needs a field test for over-suppression + its own `kIntent_Equipment` category semantics). Until then: an equipment claim is complete for SPELL-hand exclusivity only; do not rely on it to win weapon-vs-weapon |
| 9 | `kIntent_OfferPackage` (ch.9) | DENY (redirect offer) | alias-tier package OFFER (`CheckForCurrentAliasPackage` 0x49); non-alias / procedure-tier package selection | 0x49 returns the claimed package for the claim's actor (`core/PackageGate.cpp`), engine runs it natively; structurally BENEATH script-driven (PapyrusUtil) overrides (#3a) | **YES (gated) for the alias tier** — the enumerated path for EVERY follower in the Tuxborn audit (all alias-tier, zero PapyrusUtil overrides). Phases 1-2 field-proven | Non-alias procedure-tier (`BGSProcedureTreeProcedure` `Unk_XX`) is a FLAGGED gap needing an RE spike (HOOK-SITE-COVERAGE §5) — not a follower-relevant path today, but the one remaining package path APMF cannot yet deny. Phase 3 (save/load interplay) unexercised |
| 6 | `kIntent_CombatTarget` (ch.6) | ARBITRATE-only | `AIProcess.currentCombatTarget` (+ `CombatController.targetHandle`), re-chosen each tick by threat; a competing framework's target write | NONE built (records owner; client writes its own `currentCombatTarget`) | **PARTIAL (by design today)** — the NATIVE AI's target re-choice is handled by the CLIENT (MFO commands `currentCombatTarget` + consent), so the native path is not a "competing source" in practice. But a COMPETING FRAMEWORK writing the target is NOT denied | **GAP (already documented, CHANNEL-MAP ch.6):** the future deny is a suppression of a competing framework's target write at the hook. Single-client today (MFO), so no live race; flag before a second target-writing framework is supported |
| 12 | `kIntent_ShoutPower` (ch.14) | ARBITRATE-only | `selectedPower` / voice slot; a competing framework's `EquipShout`; AI shout leaves (`CombatBehaviorCastShout`/`EquipShout`) | records voice-slot owner; client executes its own `EquipShout` | **PARTIAL (by design today)** — mirrors ch.6/ch.8: the client selects; the native AI is made to cooperate. A competing framework's `EquipShout` is not denied | **GAP (documented, CHANNEL-MAP ch.14):** same future competing-framework suppression as ch.6. Note `CombatBehaviorCastShout` leaf IS deniable via ch.7 offense/cast today if a client also claims combat-action |
| 5 | `kIntent_Headtrack` (ch.5) | DENY (known-incomplete) | AI writes per-TYPE `headTrackTarget[]` (default/combat/dialogue/procedure); APMF owns only the point/action slot | own the point slot + `Tick` re-assert (`channels/Headtrack.cpp`) | **PARTIAL — flagged (#2)** — a package-locked follower reclaims the head via a higher-priority type; the re-assert is the SYMPTOM of a missing block | **GAP (already flagged #2):** real fix = block the AI's per-type headtrack write at the 0xAD seat. The ONE channel that overrides `Tick` |
| 3 | `kIntent_Stance` (ch.3) | PROMOTE one-shot | package flag → `actorState1.sneaking`; anim-event is additive | fire `NotifyAnimationGraph("SneakStart/Stop")` once (`channels/Stance.cpp`) | **PARTIAL** — the anim-event promote is clean per-shot, but the AI's OWN package sneak flag can re-assert; crouch got out-fought on a package-locked Cicero (#2) | **GAP (same class as ch.5):** the package's `sneaking` write is not gated. Bounded one-shot is correct for a deliberate toggle; a HELD stance against a package needs a source-gate on the package flag. Flag it, do not present held-stance as complete |
| 4c | `kIntent_WeaponDrawn` (ch.4) | PROMOTE one-shot | `actorState2.weaponState` (sticky, not per-frame) | `DrawWeaponMagicHands(bool)` once at engage (`channels/WeaponDraw.cpp`) | **YES (promote / no competing source)** — weaponState is sticky; no continuous AI write to deny for a bounded draw/sheathe (#0c) | — |
| 11 | `kIntent_Idle` (ch.12) | PROMOTE one-shot | AI idle manager (additive) | `PlayIdle` / `NotifyAnimationGraph` once (`channels/Idle.cpp`) | **YES (promote / no competing source)** — a one-shot animation has no continuous competitor to deny (#11 momentary) | — |
| 10 | `kIntent_Dialogue` (ch.10) | DENY one-shot | the actor's OWN in-progress dialogue | `PauseCurrentDialogue()` (`channels/Dialogue.cpp`) | **YES (gated, bounded)** — pauses the one source (the actor's own dialogue); no other path produces that facet | — |

## The node-protocol fix (feat/ai-cast-suppress, 2026-09-04) — the recurring CTD's root cause

**The crash, re-read from the engine.** Three deck crash logs on 2026-09-04
(16:14:16, 17:07:30, 20:41:48; `SkyrimSE.exe+085DC80 call [rax+0x28]` / `+085DC69
mov rax,[rsi]`, stack `47483 ← 47363 ← 47362 ← 33217 ← 38565 Actor::UpdateCombat ←
MFO's UpdateCombat thunk ← 46902 CombatUpdateJob`) were disassembled against the
1.6.1170 image (SteamStub-decrypted offline copy, Address Library
`versionlib-1-6-1170-0.bin`; every faulting offset matched the disassembly byte for
byte). Frame 0 (ID 47483) is the behavior-tree THREAD STEP function, not a
`CombatInventoryItem` deref: `rsi = thread->cur_node (+0x138)` was `0x100000000`
(garbage), `thread->state (+0x148) == 2` (INTERRUPTED) selected vfunc slot 5
(`on_interrupted`, `+0x28`) → `mov rax,[rsi]` → 0 → `call [rax+0x28]`. The earlier
"null `CombatInventoryItem` vfunc" reading was a misattribution of the same
instruction. The corrupt state was the THREAD's, and it was corrupted by APMF's own
deny. APMF.log for the 20:41 session shows the actor (Jesper, `0x750012C6`) under a
ch.8 +ACT heal drive with a ~150 ms internal `kIntent_Cast` window every ~1.8 s; the
crash landed 1 s after a release, ~6 min into the session.

**The protocol (measured; `core/CombatBehaviorRE.h` "The node protocol").** A node's
`act()` (slot 0x02) PUSHES its per-thread state on the thread's data stack (`33171`:
`top += align4(size)`), and the runner calls the SAME node's `pop()` (slot 0x03) in
the SAME step right after an `act()` that ascended; `pop()` pops exactly what its own
`act()` pushed. `ForceFail`: push 4 / pop 4. Leaves: push/pop `align4(sizeof T)` —
4 for Attack/Bash/AttackLow/SpecialAttack/CastShout/PrepareDualCast/selectors, 0xC for
CastImmediateSpell/CastConcentrationSpell/RangedAttack, 0x18 GroundAttack, 0x30
FlyingAttack. The ContextMagic `CreateContextNode1`: `act()` pushes 0x20 (the
`CombatBehaviorContextMagic`) + 0x10 (the saved context window) and descends;
`pop()` pops 0x30, RELEASES the two NiPointers at context+0x10/+0x18 and restores the
window from the saved copy.

**Why the old deny crashed.** `ActThunk` ran `ForceFail::act()` (push 4) in place of
the node's `act()`, then the runner ran the NODE'S OWN `pop()`. For the 4-byte leaves
that was balanced by accident (which is why the T1 Attack probe "worked"); for the
0xC/0x18/0x30 leaves it drifted the data stack; for the ContextMagic node it popped
0x30 for a 4-byte push, released two NiPointers read out of the ENCLOSING frame's live
data (a premature `CombatInventoryItem` free — the UAF class the first crash smelled
of) and restored the context window from garbage. Later pushes then overwrote the
enclosing frames' live data; the runner eventually walked a garbage `cur_node`, and
the interrupt unwind (`state==2` — the driven cast's own `InterruptCast` / an equip
change interrupts the combat threads) called slot 5 on it.

**The fix (`core/ActionGate.cpp`).** A second thunk at slot 0x03 (`pop`) on every
vtable the act-thunk is on (70 leaves + the 2 ContextMagic nodes, RTTI-verified through
the same `InstallOnVtables`). A denied `act()` records `{node, control}` in a
`thread_local` pending-pop and invokes ForceFail's original `act()`; the very next
`pop()` for that `{node, control}` on that thread runs ForceFail's ORIGINAL `pop()`
(`top -= 4`) instead of the node's own. A denied node therefore executes exactly
ForceFail's own `act()`+`pop()` pair — the engine's own failure protocol, both halves
its own compiled bodies (no hand-rolled `SetFailed`, no invented offsets, no ID hooked).
`thread_local` is exact: the runner calls `pop()` synchronously on the same OS thread
with nothing in between (checked in the step function: phase check → phase=1 →
`pop()`); a mismatched pop is counted + logged once as a protocol anomaly, never
trusted silently. Install refuses the whole deny if ForceFail's `act()` OR `pop()`
fails to resolve, and only a vtable with BOTH halves installed is ever classified.

**Scope signal — when the cast deny arms (the "under cast control" question).** The
Cast category is denied for an actor when any of these is the winning claim, all read
from the one lock-free RCU snapshot (arm = the claim's Drain publish, disarm = its
release/TTL publish; no separate flag, nothing to race on the combat thread):
- a ch.8b `kIntent_Cast` claim (the client's executed-cast window) — as before;
- a ch.7 `kIntent_CombatAction` claim naming Cast or Offense — as before;
- **NEW: a ch.8 `kIntent_SelectSpell` claim with the +ACT opt-in
  (`ival & castexec::kActFlag_Drive`)** — APMF itself drives the cast, so the AI's own
  magic branch is silent for the WHOLE claim window, not only the ~150 ms internal
  ch.8b pulse. This is what stops the AI from persistently trying to cast a held spell
  (deck case: a patcher-injected rune; the deny is spell-agnostic, it is the whole magic
  branch) while APMF owns the cast.
A BARE ch.8 claim (gate-only, MFO's offense gambit) deliberately arms nothing: the
client wants its AI to build the magic context and cast the claimed spell (row 4).

**Why the forced drive is spared.** `core/CastExecutor.cpp` equips through
`ActorEquipManager::EquipSpell`, animates through `NotifyAnimationGraph`, and fires
through the hand `MagicCaster` / `CastSpellImmediate` — none of which dispatch through
the combat behavior tree, so a denied ContextMagic node and denied cast leaves never
touch it. If anything the drive improves: the AI can no longer self-equip against the
drive's `EquipSpell` (the `Magic_Equip_Out` ping-pong in the crash-session log) or
fire its own copy of the claimed spell.

**What was ruled out, and why.**
- `kCastingDisabled` (`Actor::boolFlags` bit 21 — at **+0x204** on 1.6.1170; CommonLib's
  `GetActorRuntimeData()` relocates the block by +8 on ≥1.6.629, so the SE header's
  `0x1FC` is wrong for this runtime). Full-image scan: the bit is READ at exactly two
  functions (IDs 38599 / 38600, `Actor::CanCast`-style: graph-variable + AV + bit) whose
  only callers are the cast LEAVES' own Enter/Update (49100 ← CastImmediateSpell::act,
  49105, 49110, 49759/49764 CastShout, 49920/49928) — i.e. DOWNSTREAM of the ContextMagic
  build. Setting it makes the cast leaves fail natively but the AI still builds the
  magic context and self-equips (the equip flip-flop stays), and it is a persisted
  actor flag (needs restore + co-save discipline, #15). No engine SETTER of the bit
  exists in the image (it is script-owned), and none of the drive's calls
  (`EquipSpell`, `CheckCast`, `CastSpellImmediate`) read it — so it WOULD spare the
  forced drive, but it is the wrong seat: a downstream partial deny with save-state
  baggage. Kept as a documented native-bit fallback only (`NativeBitProbe`).
- Clearing the AI's selected spell: the AI re-selects every tick from
  `CombatInventory` (score + `CheckShouldEquip`); a write is a re-assert loop (#1).
- Hooking the context-build entry by engine ID (47483/33171/49133 …): call-site/ID
  hooks are banned (#6/#17); the vtable seat (slot 0x02 + its paired 0x03) IS the
  context-build entry — the previous pass had the right seat and half the protocol.
- A "gate the magic branch at its parent selector" deny: the parent
  (`ConditionalChildSelector`, push 4) would also need the pair, and denying it takes
  siblings with it; the ContextMagic node is the exact, minimal seat.

## The per-hand pass (feat/deny-perhand) — what's scoped, what isn't, and why

**What IS per-hand now.** Two of the three enumerated `kIntent_Cast` paths have a
real, engine-native, per-hand signal at their seat, and both now read it:
- **`core/CastGate.cpp` (0x0A `CheckCast`)**: `a_this` (`RE::MagicCaster*`) IS
  already a per-hand object — `RE::Actor` keeps one `MagicCaster` per
  `RE::MagicSystem::CastingSource`. `MagicCaster::GetCastingSource()` (vtable slot
  0x15, declared on `MagicCaster` itself, an ordinary unhooked virtual call, same
  safety class as the pre-existing `GetCasterAsActor()` call at slot 0x0C) reports
  exactly which hand THIS deliberation is for.
- **`core/EquipGate.cpp` (0x0F `CheckShouldEquip`)**: `a_this`
  (`RE::CombatInventoryItem*`) carries its OWN `itemSlot.equipSlot` (a real struct
  member, `static_assert`'d at offset 0x20 — the AI sets this to the vanilla
  hand's `BGSEquipSlot` when it builds the item for that hand), compared against
  `RE::BGSDefaultObjectManager::GetSingleton()->GetObject<RE::BGSEquipSlot>
  (kLeftHandEquip / kRightHandEquip)` — CommonLib's own version-robust default-
  object lookup (the SAME table the engine itself consults), never a hardcoded
  FormID.

Both feed `Allowance::AllowedCastForHand(actor, subjectForm, callerHand)`:
when the winning `kIntent_Cast` claim names a specific hand (its `CastFlags`'
`kCastFlag_LeftHand` bit — default right) and the caller's resolved hand is KNOWN
and DIFFERENT, it returns ALLOW without even checking `subjectForm` — that hand's
AI deliberation is untouched. `Hand::kUnknown` (a caster with `kOther`/`kInstant`
source, or an item whose slot is neither vanilla hand — e.g. `kEitherHandEquip`)
degrades exactly to the old `AllowedCast(actor, subjectForm)` actor-wide floor —
never a guess. `ControlMap::TryGetCastClaim` grew an optional `outFlags` parameter
(default `nullptr`, existing callers unaffected) to expose the claim's `CastFlags`
for this.

**What stays PER-ACTOR (documented gap, #18).** The (c) T1 cast-leaf category deny
and the (d) `ContextMagic` `CreateContextNode` deny (`core/ActionGate.cpp`) have NO
native hand signal at their seat: `CombatBehaviorTreeNode`'s fixed 10-vfunc layout
carries no per-instance data beyond the base, and the thread object
(`CombatBehaviorThread`, measured layout in `core/CombatBehaviorRE.h`) has no
hand/casting-source field either. There is no RTTI/struct-verified read available
here — inventing one would violate INVARIANTS #17/#18's "no blind slots, no invented
offsets" discipline. **Practical effect:** a `kIntent_Cast` claim on ONE hand still
denies (c)/(d) for BOTH hands' magic-offense leaves/context-build on that actor. This
is the required per-actor floor, not silently presented as per-hand. If a future
client needs a magic-hand deny that leaves the OTHER hand's magic branch genuinely
free, that needs either a version-pinned instance-layout RE spike on the thread's
context window (the built `CombatBehaviorContextMagic` DOES know its caster, but
reading it at the node seat is an offset read this pass will not invent) or leaning
on (a)/(b)'s per-hand narrowing alone.

## Open gaps carried out of this pass (documented, not silent — #18)

1. **`kIntent_Equipment` weapon-vs-weapon (row 15).** Highest-value residual. The
   weapon CONTEXT nodes (AcquireWeapon / ContextMelee / ContextRanged CreateContext)
   are the clean, RTTI-backed seat — same paired mechanism as the cast fix, symbols
   verified present. Needs its own `kIntent_Equipment` category semantics + a field
   test for over-suppression before shipping.
2. **Competing-framework suppression for arbitration-only facets (rows 6, 12).**
   `kIntent_CombatTarget` / `kIntent_ShoutPower` do not deny a SECOND framework's
   target/shout write. No live race with a single client (MFO cooperates the native
   AI); flag before a second such framework is supported. Already in CHANNEL-MAP.
3. **`kIntent_OfferPackage` non-alias procedure tier (row 9).** `BGSProcedureTreeProcedure`
   `Unk_XX` slots need an RE spike before any hook (HOOK-SITE-COVERAGE §5). Not a
   follower path today. Phase 3 (save/load) unexercised.
4. **Known-incomplete blocks (rows 5, 3).** Headtrack (#2) and held-stance both lose
   to a package's own per-type / flag write; the real fix is a source-gate at the
   0xAD seat. Already flagged; not presented as complete.
5. **`kIntent_CombatAction` category coverage (row 14).** Only "offense" classified;
   defense/movement/utility are future categories (a scope limit, not a leak).
6. **`kIntent_Cast` / ch.8 +ACT per-hand at paths (c)/(d) (rows 8b, 4+ACT).** The T1
   cast-leaf deny and the `ContextMagic` node deny are PER-ACTOR — no native hand
   signal at the tree-node/thread seat. Paths (a)/(b) ARE per-hand today.
7. **Field confirmation of the paired deny (feat/ai-cast-suppress).** The protocol is
   disassembly-measured, not yet deck-cycled: expect zero `[ch.7] paired-pop protocol
   ANOMALY` lines, no `Magic_Equip_Out` flip-flop on a driven actor, and the +ACT drive
   selecting its form instead of degrading to the fallback every pulse.
