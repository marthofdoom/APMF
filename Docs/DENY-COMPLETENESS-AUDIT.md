# Deny-completeness audit (INVARIANTS #18)

Per-facet enumeration of EVERY path the competing source (native combat AI, a
foreign framework, a running package) can reach a facet APMF accepts a claim on,
and whether the deny ZEROES that facet across ALL of them. Born from the cast/equip
CTD (2026-09-04): a `kIntent_Cast` claim denied the cast-FIRING path but not the
cast-CONTEXT-CREATION path, so the AI still built its magic-equip context and raced
MFO's forced equip to a null-vfunc AV. #18 makes the enumeration obligation explicit;
this file is the enumeration.

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
| 4 | `kIntent_SelectSpell` (ch.8) | ARBITRATE + DENY (exclusivity) | AI charges a spell (`CheckCast` 0x0A); AI equips a spell/staff to hand (`CheckShouldEquip` 0x0F) | deny any spell/staff ≠ `param.form` at BOTH gates (`core/CastGate.cpp`, `core/EquipGate.cpp` via `Allowance::Allowed`) | **YES (gated)** — this facet NARROWS the AI to one spell (it does NOT stop casting). The magic context-node BUILD is *correct* here (it builds context for the claimed spell, which is allowed), so denying it would be wrong — deliberately not denied under SelectSpell | — (context-node deny is `kIntent_Cast`-only by design, see row ch.8b) |
| 8b | `kIntent_Cast` (ch.8b) | ARBITRATE + DENY (execution) | (a) AI charges a competing spell (`CheckCast` 0x0A); (b) AI re-arms a competing spell/staff (`CheckShouldEquip` 0x0F); (c) AI fires — the 4 cast leaves (`CastImmediate/Concentration/PrepareDualCast/CastShout`, T1 `kCombatActionCat_Cast`); **(d) AI BUILDS its magic cast/equip CONTEXT** (`CombatBehaviorContextMagic` CreateContextNode `act()` — selects spell + constructs `CombatBehaviorEquipContext` over a `NiPointer<CombatInventoryItem>` and derefs it) | (a)(b) `Allowance::AllowedCast` allow only claim spell+proxy; (c) T1 cast-leaf deny; **(d) NEW: T1 deny of `apmf::cbt::kCastContextNodes` `act()` via ForceFail (`core/ActionGate.cpp`)**; all TTL-bounded auto-release | **YES (gated) — NOW COMPLETE.** Path (d) was the leak: firing was denied, SETUP was not → the AI built the equip context and raced MFO's forced equip → `EXCEPTION_ACCESS_VIOLATION call [rax+0x28]` rax=0. Fixed this pass | **FIXED 2026-09-04.** See the fix section below |
| 14 | `kIntent_CombatAction` (ch.7) | DENY (category) | 70 combat behavior-tree leaves' `act()` (slot 0x02) | ForceFail the leaves whose classified category bit is set in `param.ival` (`core/ActionGate.cpp`); "offense" classified today | **YES (gated) for the offense category** — the deny zeroes exactly the named category's leaves; every other leaf fires natively | SCOPE (not a leak): only "offense" is classified today; defense/movement/utility categories are future work (a claim only denies what it names — CHANNEL-MAP ch.7). Also: offense leaves' CONTEXT nodes (Melee/Ranged CreateContext) are NOT yet denied — same class as the cast-context fix; harmless today (no forced melee equip), flag if a client ever force-equips a weapon under an offense claim |
| 15 | `kIntent_Equipment` (ch.15) | ARBITRATE + DENY (input-gate) | AI equips a spell/staff (`CheckShouldEquip` 0x0F, 30 magic/staff vtables); **AI equips a competing WEAPON**; **AI BUILDS a weapon-equip CONTEXT** (AcquireWeapon / ContextMelee / ContextRanged CreateContextNode) | deny spell/staff re-arm at 0x0F while the weapon-order claim holds (`core/EquipGate.cpp`) | **PARTIAL** — the spell/staff-suppression path is gated, but the weapon-vs-weapon path is **NOT**: `CombatInventoryItemMelee`/`Ranged` have NO concrete C++ class in the pinned CommonLib, so 0x0F cannot be hooked for weapons. The AI can still equip a *different weapon*; a client that force-equips a weapon concurrently is exposed to the SAME context-creation race class as the cast CTD | **GAP (documented).** Fix path found + symbols verified: deny the weapon CONTEXT nodes' `act()` — `VTABLE_CombatBehaviorTreeCreateContextNode_CombatBehaviorContextAcquireWeapon_` (Offsets_VTABLE.h:3430), `...ContextMelee...` (3719), `...ContextRanged...` (1784), all RTTI-backed. Same mechanism as the cast fix. Scoped OUT this pass (needs a field test for over-suppression + its own `kIntent_Equipment` category semantics). Until then: an equipment claim is complete for SPELL-hand exclusivity only; do not rely on it to win weapon-vs-weapon |
| 9 | `kIntent_OfferPackage` (ch.9) | DENY (redirect offer) | alias-tier package OFFER (`CheckForCurrentAliasPackage` 0x49); non-alias / procedure-tier package selection | 0x49 returns the claimed package for the claim's actor (`core/PackageGate.cpp`), engine runs it natively; structurally BENEATH script-driven (PapyrusUtil) overrides (#3a) | **YES (gated) for the alias tier** — the enumerated path for EVERY follower in the Tuxborn audit (all alias-tier, zero PapyrusUtil overrides). Phases 1-2 field-proven | Non-alias procedure-tier (`BGSProcedureTreeProcedure` `Unk_XX`) is a FLAGGED gap needing an RE spike (HOOK-SITE-COVERAGE §5) — not a follower-relevant path today, but the one remaining package path APMF cannot yet deny. Phase 3 (save/load interplay) unexercised |
| 6 | `kIntent_CombatTarget` (ch.6) | ARBITRATE-only | `AIProcess.currentCombatTarget` (+ `CombatController.targetHandle`), re-chosen each tick by threat; a competing framework's target write | NONE built (records owner; client writes its own `currentCombatTarget`) | **PARTIAL (by design today)** — the NATIVE AI's target re-choice is handled by the CLIENT (MFO commands `currentCombatTarget` + consent), so the native path is not a "competing source" in practice. But a COMPETING FRAMEWORK writing the target is NOT denied | **GAP (already documented, CHANNEL-MAP ch.6):** the future deny is a suppression of a competing framework's target write at the hook. Single-client today (MFO), so no live race; flag before a second target-writing framework is supported |
| 12 | `kIntent_ShoutPower` (ch.14) | ARBITRATE-only | `selectedPower` / voice slot; a competing framework's `EquipShout`; AI shout leaves (`CombatBehaviorCastShout`/`EquipShout`) | records voice-slot owner; client executes its own `EquipShout` | **PARTIAL (by design today)** — mirrors ch.6/ch.8: the client selects; the native AI is made to cooperate. A competing framework's `EquipShout` is not denied | **GAP (documented, CHANNEL-MAP ch.14):** same future competing-framework suppression as ch.6. Note `CombatBehaviorCastShout` leaf IS deniable via ch.7 offense/cast today if a client also claims combat-action |
| 5 | `kIntent_Headtrack` (ch.5) | DENY (known-incomplete) | AI writes per-TYPE `headTrackTarget[]` (default/combat/dialogue/procedure); APMF owns only the point/action slot | own the point slot + `Tick` re-assert (`channels/Headtrack.cpp`) | **PARTIAL — flagged (#2)** — a package-locked follower reclaims the head via a higher-priority type; the re-assert is the SYMPTOM of a missing block | **GAP (already flagged #2):** real fix = block the AI's per-type headtrack write at the 0xAD seat. The ONE channel that overrides `Tick` |
| 3 | `kIntent_Stance` (ch.3) | PROMOTE one-shot | package flag → `actorState1.sneaking`; anim-event is additive | fire `NotifyAnimationGraph("SneakStart/Stop")` once (`channels/Stance.cpp`) | **PARTIAL** — the anim-event promote is clean per-shot, but the AI's OWN package sneak flag can re-assert; crouch got out-fought on a package-locked Cicero (#2) | **GAP (same class as ch.5):** the package's `sneaking` write is not gated. Bounded one-shot is correct for a deliberate toggle; a HELD stance against a package needs a source-gate on the package flag. Flag it, do not present held-stance as complete |
| 4c | `kIntent_WeaponDrawn` (ch.4) | PROMOTE one-shot | `actorState2.weaponState` (sticky, not per-frame) | `DrawWeaponMagicHands(bool)` once at engage (`channels/WeaponDraw.cpp`) | **YES (promote / no competing source)** — weaponState is sticky; no continuous AI write to deny for a bounded draw/sheathe (#0c) | — |
| 11 | `kIntent_Idle` (ch.12) | PROMOTE one-shot | AI idle manager (additive) | `PlayIdle` / `NotifyAnimationGraph` once (`channels/Idle.cpp`) | **YES (promote / no competing source)** — a one-shot animation has no continuous competitor to deny (#11 momentary) | — |
| 10 | `kIntent_Dialogue` (ch.10) | DENY one-shot | the actor's OWN in-progress dialogue | `PauseCurrentDialogue()` (`channels/Dialogue.cpp`) | **YES (gated, bounded)** — pauses the one source (the actor's own dialogue); no other path produces that facet | — |

## The cast/equip fix (row 8b path (d)) — mechanism + why it is complete

**Root cause.** The crash node is
`CombatBehaviorTreeCreateContextNode1<CombatBehaviorContextMagic, …
CombatBehaviorEquipContext, NiPointer<CombatInventoryItem>…>`. It is a
`CombatBehaviorTreeNode` subclass (same base vtable: `act()` at slot 0x02) whose
`act()` runs UPSTREAM of the cast-firing leaves to BUILD the AI's magic cast/equip
context — it selects the spell and constructs a `CombatBehaviorEquipContext` holding
a `NiPointer<CombatInventoryItem>`, then dereferences that item. `kIntent_Cast`
denied the firing leaves (T1 `kCombatActionCat_Cast`), the charge (`CheckCast`), and
the re-arm (`CheckShouldEquip`) — but NOT this context build. So with the claim held,
the combat AI (combat thread) still built the context and dereferenced the item while
MFO's forced equip (main thread, MFO #62 `MainThread::Post`) mutated the equipped
item out from under it → the item pointer went null → `call [rax+0x28]` rax=0
(null-vtable vfunc call), MFO.dll frame 5.

**Fix.** `core/ActionGate.cpp` now also `write_vfunc`-installs its existing
`ActThunk` at slot 0x02 on the ContextMagic CreateContextNode vtables
(`apmf::cbt::kCastContextNodes` in `core/CombatBehaviorRE.h`) and classifies them
`kCombatActionCat_Cast | kCombatActionCat_Offense`. While a `kIntent_Cast` (or an
offense `kIntent_CombatAction`) claim stands on the actor, the thunk denies the
node's `act()` by invoking `CombatBehaviorForceFail`'s own original `act()` (the
field-proven leaf-deny mechanism — never a hand-rolled `SetFailed`) and returns
without calling the node's own `act()`. The context is therefore NEVER built and the
item is NEVER dereferenced; the parent selector treats the magic node as failed and
falls through to a non-magic branch.

**Why it is complete.**
- **All four cast paths are now zeroed**: charge (0x0A), re-arm (0x0F), fire (T1 cast
  leaves), and — new — SETUP/context-build (T1 ContextMagic node). No enumerated AI
  path to the magic-cast facet leaks while the claim stands.
- **The race is structurally gone, not narrowed**: the AI never builds/derefs the
  `CombatInventoryItem`, so there is no concurrent read to race MFO's equip. This is
  a source-block, not a re-assert (#1) — the deny does zero per-tick work; it is a
  vtable consult.
- **Version-robust + a good citizen (#6/#17)**: the node's `VTABLE_*` and `RTTI_*`
  symbols exist and are Address-Library-ID-backed in the pinned CommonLib
  (`CharmedBaryon/CommonLibSSE-NG` @ `c4ab853d`, Offsets_VTABLE.h:3704-3705,
  Offsets_RTTI.h:4095-4096). `allowance::InstallOnVtables` RTTI-verifies each node
  derives `CombatBehaviorTreeNode` before hooking — a non-deriving symbol is SKIPPED,
  never hooked blind (#17). The hook is a chainable `write_vfunc`, VR-refused.
- **Granular (#0 / §1a rule 3)**: only the `ContextMagic` node is touched. Melee /
  ranged / movement / search context nodes keep firing, so a mid-cast follower keeps
  repositioning and (if only `kIntent_Cast` is held) keeps its melee/ranged options —
  APMF denies the AI's magic setup, it does not freeze the body.
- **Bounded (#3c)**: the `kIntent_Cast` claim is TTL-auto-released by the ControlMap
  Drain, so the context-node deny lifts with it — a crashed client cannot leave the
  AI's magic branch permanently failed.
- **No engine call added (#0)**: the fix is one more `write_vfunc` install + a
  classification bit; the thunk still only ever flips the engine's own decision to
  fail for a claimed actor. APMF fires no cast, builds no context, drives nothing.

## Open gaps carried out of this pass (documented, not silent — #18)

1. **`kIntent_Equipment` weapon-vs-weapon (row 15).** Highest-value residual. The
   weapon CONTEXT nodes (AcquireWeapon / ContextMelee / ContextRanged CreateContext)
   are the clean, RTTI-backed seat — same mechanism as the cast fix, symbols verified
   present. Needs its own `kIntent_Equipment` category semantics + a field test for
   over-suppression before shipping. Until then, an equipment claim is complete for
   spell-hand exclusivity only.
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
