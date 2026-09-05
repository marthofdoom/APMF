# APMF Channel Map (exhaustive)

The core blueprint: for each directable AI-control facet, its source, the clean **APMF DENY gate**
(APMF's ONLY lever — suppress the losing source, no re-assert), **how the CLIENT drives the facet**
(the client's concern, via its own proven mechanisms — NEVER an APMF action), version-robustness, and
a **DOCUMENTED vs GAP** verdict. Derived 2026-09-02 from CommonLibSSE-NG RE headers, the CK wiki, and
existing mods (TDM, NFF, PapyrusUtil, Puppeteer).

**Governed by design.md §1a + INVARIANTS #0: APMF MODERATES (arbitrate + DENY + a narrow, sanctioned
bounded-promote); it NEVER manufactures or sustains an AI decision.** The "how the client drives it"
column below is documentation of the CLIENT's execution path for arbitration-only facets — it is NOT a
list of things APMF does for those rows. APMF calls no decision-generating engine function
(`StartCombat`, `CastSpellImmediate`, movement drive-feed, a `selectedSpells`/`EquipShout`-style
selection write). Legitimate APMF actions are: arbitrate the claim; DENY at the source (AV writes that
gate the input the AI reads, movement full-block, detection AVs, package yield, pausing an actor's own
in-progress dialogue); and, for a facet with no meaningful deny form and no AI decision to arbitrate
around, PROMOTE a single bounded one-shot client-requested action (#0c — weapon draw ch.4, stance
toggle ch.3, idle/anim ch.12). (Earlier revisions of this file framed a "PROMOTE" column as APMF
DRIVING every facet, including decision-selection ones; that executor framing licensed a channel that
called StartCombat and CTD'd — it is corrected here. ch.14 shout-power previously called `EquipShout`
directly, the same anti-pattern in miniature; it is now arbitration-only, mirroring ch.6/ch.8.)

`RE/…` = CommonLibSSE-NG headers. `// 0xNN` = in-header vfunc index (index-stable across runtimes);
struct-member offsets are accessor/Address-Library covered but more version-sensitive. Taxonomy is
OPEN (17 channels + sub-splits). Combat-target / combat-actions / casting (⭐) carry the weight.

## Table

| # | Channel | Source | APMF DENY gate (APMF's only lever) | How the CLIENT drives it (NOT an APMF action) | Verdict |
|---|---------|--------|-----------|---------|---------|
| 1 | **Movement / locomotion** | Package planner (`AIProcess.currentPackage`) → `MovementControllerNPC`→`ActorMover` | **FULL BLOCK (built):** `KeepOffsetFromActor(self, 0)` nulls the move GOAL at the source + `SetDontMove` locks translation → clean stand-still (no run-in-place, no teleport-snap; `SetDontMove` ALONE was one layer too shallow). Both Address-Library bound | `IMovementDirectControl` feed (unnamed `Unk_01..08`) or `Actor::Move` 0xC8 (probe-gated) | **DENY/full-block DOCUMENTED; PROMOTE feed GAP** |
| 1a | ↳ **Gait / speed** | `PreferredSpeed` walk/jog/run; `kSpeedMult` AV=30 | **TRUE:** set `kSpeedMult` AV | same AV / package speed flag | **DOCUMENTED, clean** |
| 2 | **Facing / heading** | Same planner (rotation) | rides movement gate (`SetAIDriven`) | same direct-control feed | shared w/ movement; standalone = GAP |
| 3 | **Stance / sneak** | Package flag → `actorState1.sneaking` | clean only via package flag; anim-event = additive | `NotifyAnimationGraph("SneakStart/Stop")` vfunc 01 | **DOCUMENTED (Tier A), bounded one-shot promote — #0c** |
| 4 | **Weapon draw / sheathe** | `actorState2.weaponState` (sticky) | sticky, not per-frame | `DrawWeaponMagicHands(bool)` vfunc 0xA6 | **DOCUMENTED, bounded one-shot promote — #0c** |
| 5 | **Headtracking** | AI writes per-type `HighProcessData.headTrackTarget[]` | **TRUE:** own the slot (`SetHeadtrackTarget(type,ref)`) | same setter | **DOCUMENTED, clean gate** |
| 6 | **Combat-target** ⭐ | `AIProcess.currentCombatTarget` (+ `CombatController.targetHandle`), re-chosen each tick by threat | ARBITRATION-ONLY today (record owner). FUTURE deny: suppress a competing framework's target write at the hook (GAP) | CLIENT commands it: compare-and-write of `currentCombatTarget` (MFO's `Targeting::Command` + its UpdateCombat hook re-assert; StartCombat to initiate) — APMF makes NO combat call | **APMF arbitration-only; client executes the command** |
| 7 | **Combat ACTIONS** ⭐ | Internal **combat behavior tree** (`CombatBehaviorController`) + `CombatInventory` + `CombatState` | **TRUE (graduated 2026-09-03):** `write_vfunc` slot 0x02 on all 70 `CombatBehaviorTreeNodeObject_*` leaves (`core/ActionGate.cpp`, T1) — a `kIntent_CombatAction` claim denies exactly the leaves whose classified category bit is set in `APMF_Param.ival` (starts with `kCombatActionCat_Offense`); invokes `CombatBehaviorForceFail`'s own original `act()`, never a hand-reconstructed `SetFailed` call | `channels/CombatAction.cpp` claims the facet (arbitration-only, names the deny mask); real lever otherwise = loadout (ch.15) + aggression (ch.11) | **DOCUMENTED, clean gate (graduated from the field-proven T1 probe)** |
| 7a | ↳ attack/block/power/bash | `ATTACK_STATE_ENUM`; `meleeAttackState`; melee-vs-ranged = equipped weapon | (share ch.7 gate above; Attack/AttackLow/Bash leaves classified "offense") | — | **DOCUMENTED (ch.7)** |
| 8 | **Casting** ⭐ | SELECT: `Actor.selectedSpells[slot]`→`MagicCaster.currentSpell`. TRIGGER: internal combat-caster state machine | ARBITRATION-ONLY today (record owner). FUTURE deny: suppress a COMPETING framework's cast selection (GAP) — NOT applied on the owned-cast path (the client WANTS its AI to cast) | CLIENT makes a REAL animated cast: equip spell + write own `selectedSpells[slot]` + grant own AI consent (deny competing spells) + a Cast-biased combat style so the AI DECIDES to cast it — full animation, mobile, NO force, NO package. APMF makes NO cast write | **APMF arbitration-only; client executes the AI-decided cast** |
| 8+ACT | ↳ **Casting +ACT mode** (feat/cast-act, new) | Same claim (`RequestEx(kIntent_SelectSpell, {form=spell, ival=HAND})`), `ival` (0 auto/1 right/2 left/3 dual) now read | **APMF-EXECUTED, not arbitration-only** — a deliberate, scoped exception to the "client executes" rule above (MFO's own equivalent client-side execution is being REMOVED for this facet; `core/CastExecutor.cpp`) | APMF equips the resolved hand(s) (`ActorEquipManager::EquipSpell`, protected by feat/deny-perhand's per-hand ch.8b claim on that SAME hand), drives the OBSERVED animated sequence (`core/CastObserve.h`'s captured BeginCast→Charging→Charged→SpellFire→teardown), and falls back to `CastSpellImmediate` (guaranteed delivery) if the drive can't animate (VR / never selects / never charges). A self-delivery spell aimed at an actor named by a winning ch.6 `kIntent_CombatTarget` claim gets a delivery-flipped `kTargetActor` proxy (never AddSpell'd) so both placement and animation match | **Client declares (spell+hand); APMF owns execution end-to-end. `Release` restores the hand(s).** |
| 8a | ↳ L/R/dual/staff | `CastingSource` kLeftHand/kRightHand/kOther/kInstant; per-slot casters | (arbitration-only) | CLIENT owns the slot's spell via its own `selectedSpells` | **client per-hand** |
| **8b** | **Cast EXECUTION** (new) | Same three ch.8/ch.7 gates + the magic CONTEXT node, reading `kIntent_Cast` | **COMPOSITE DENY (built; deny-completeness closed 2026-09-04):** a `kIntent_Cast` claim (`channels/CastCompose.cpp`, `RequestCast` / ABI v5) fans into 0x0A CheckCast + 0x0F CheckShouldEquip (`Allowance::AllowedCast` — allow ONLY the claim's spell+proxy) + the T1 cast leaves (`kCombatActionCat_Cast` — deny CastImmediate/CastConcentration/PrepareDualCast/CastShout) + **the T1 magic cast/equip CONTEXT-CREATION node** (`apmf::cbt::kCastContextNodes`, `CombatBehaviorContextMagic` CreateContextNode `act()` — the node that BUILT the equip context and raced a forced equip to a null-item AV; INVARIANTS #18), leaving attack/ranged/movement firing, plus a bounded TTL auto-release (`ControlMap` Drain). **NO engine cast call.** `kCastFlag_FromPackage` extracts ONLY spell+target out of a package and NEVER runs/offers/evaluates it (design.md §3.7; direct-form fallback per §5.1/§6) | CLIENT fires its own animated cast through the hand `ActorMagicCaster` (MFO SPEC-FORCED-CAST.md); movement stays the AI's | **DOCUMENTED; deny now COMPLETE across select/fire/setup (INVARIANTS #18, Docs/DENY-COMPLETENESS-AUDIT.md). Executor is the client's.** |
| 9 | **Package-procedure activity** | `TESPackage.procedureType` (sandbox/patrol/guard) | **TRUE (graduated 2026-09-03):** `write_vfunc` on `VTABLE_Character[0]` slot 0x49 `CheckForCurrentAliasPackage` (`core/PackageGate.cpp`, T3) — a `kIntent_OfferPackage` claim returns the `TESPackage` named by `APMF_Param.form` instead of the framework's own answer; never-null fallback to the engine's answer on an unresolvable FormID | `channels/OfferPackage.cpp` claims the facet (arbitration-only, names the package); CLIENT owns the package's own runtime target (e.g. a targType-0 handle) | **DOCUMENTED (graduated from the field-proven 0x49 probe, PROVEN Phases 1-2)** |
| 10 | **Dialogue / greeting** | AI greeting/dialogue + topics | `SetDialogueWithPlayer` 0x41 / `StopCurrentDialogue` 0x4F | `InitiateDialogue` 0xD8 | **DOCUMENTED** (coarse) |
| 11 | **AI-attribute** (aggression / confidence / assistance / morality) | Dynamic AVs the engine's own combat/flee/assist decisions read (`kAggression`/`kConfidence`/`kMorality`/`kAssistance`) | **TRUE — the design's preferred model:** `ActorValueOwner::SetActorValue`/`ModActorValue` sets the input the AI itself consumes; no override | same setter | **DOCUMENTED, cleanest gate** |
| 12 | **Idle / animation** | AI idle manager | additive (no gate on AI's own idles) | `AIProcess::PlayIdle` one-shot; `NotifyAnimationGraph` | **DOCUMENTED, bounded one-shot promote — #0c** |
| 13 | **Facial expression / mood** | FaceGen emotion (`EmotionType`) | `ClearExpressionOverride` releases | **SetExpressionOverride not exposed** in this CommonLib build (raw fn needed) | **GAP** (setter) |
| 14 | **Shouts / abilities / powers** | `selectedPower`; voice slot; `GetCurrentShout` | ARBITRATION-ONLY (record voice-slot owner). FUTURE deny: suppress a competing framework's own `EquipShout` write (GAP) | CLIENT selects it: its own `ActorEquipManager::EquipShout` (sticky select; AI still triggers it) — APMF makes NO equip write | **APMF arbitration-only; client executes the select — mirrors ch.8** |
| 15 | **Equipment** (equip/unequip specific item) | Combat/package equip choices — sets what ch.4/7/8 work with | own the equipped set (remove an item = deny the AI that option — clean input-gate) | `ActorEquipManager::EquipObject`/`UnequipObject`/`EquipSpell`/`EquipShout` (scar: off-main → `MainThread::Post`, MFO #62) | **DOCUMENTED — the melee-vs-ranged lever.** GATE-ONLY facet (2026-09-03, `param.form` set): T2a's `EquipGate.cpp` `CheckShouldEquip` hook (ch.8's SAME hook) now also reads a `kIntent_Equipment` claim and denies any spell/staff re-arm while it stands (the AI's weapon-order gate, mirrors ch.8's arbitration-only shape — `channels/Equipment.cpp` makes no engine write for a param'd claim). The no-param hotkey probe (active unequip/re-equip) is unchanged. |
| 16 | **Detection / stealth** | `sneaking` (=ch.3); `HighProcessData.detectionModifier`; AVs kDetectLifeRange/kMovementNoiseMult | **TRUE:** set the detect AVs / `detectionModifier` | same | **DOCUMENTED** (AV path clean) |

## Synthesis

**APMF's legit DENY/suppress gates (APMF's own actions — set the input the AI reads, no re-assert):**
- **AI-attribute AVs (ch.11)** — bias the engine's own combat/flee decisions; cleanest gate on the board.
- **Movement DENY / full-block (ch.1)**; **Gait/speed (ch.1a)** `kSpeedMult`; **Detection AVs (ch.16)**;
  **Equipment (ch.15)** (remove an item = deny the AI that option). These SUPPRESS; they do not drive.

**Behavior the CLIENT executes (APMF only arbitrates the facet, never does these):** combat-target
command (ch.6, client's `currentCombatTarget` write), casting (ch.8, client's `selectedSpells` + consent
+ Cast-style so the AI decides), and shout/power select (ch.14, client's own `EquipShout` — converted to
arbitration-only, mirroring ch.6/ch.8).

**APMF's sanctioned bounded one-shot promotes (#0c — no deny form, no AI decision to arbitrate
around, a single deterministic call at Engage/Release, no `Tick`, no re-assert):** weapon draw (ch.4,
`DrawWeaponMagicHands`), stance toggle (ch.3, `NotifyAnimationGraph SneakStart/Stop`), idle/anim
(ch.12, `NotifyAnimationGraph IdleForceDefaultState`). These are formally permitted, not pending
conversion — see INVARIANTS #0(c).

**Need live probing:** Movement PROMOTE feed (`IMovementDirectControl::Unk_0N`, §9.1 — biggest unknown);
the DENY of a competing framework's combat-target/cast/shout selection at the hook (ch.6/ch.8/ch.14 —
the future suppression gate); Facial-expression SETTER (ch.13). Combat ACTIONS (ch.7) and package
procedures (ch.9) are now gated — see rows above; ch.7's category classification currently covers only
"offense" and ch.9's field pass covered Phases 1-2 only (save/load, Phase 3, unexercised). **The
Cicero/travel "starve an outranking framework's package" case is what ch.9 was graduated to solve:**
`core/PackageGate.cpp`'s 0x49 hook returns the claimed client's package for a claimed actor regardless
of what any other framework's own package logic would otherwise offer — see design.md §1a.

## Combat / casting verdict (the MFO headline)
- **The client makes the behavior; APMF arbitrates the facet.** MFO steers the target with its own
  `currentCombatTarget` write (`Targeting`), makes its follower's own AI DECIDE to cast the chosen spell
  via `selectedSpells` + `CasterConsent` (permit wanted, deny competing) + a Cast-biased combat style,
  and biases combat/flee via aggression/confidence AVs. The cast is REAL and fully ANIMATED (the vanilla
  AI path) and stays MOBILE — no `CastSpellImmediate` force, no rooting UseMagic package. APMF only claims
  the cast + combat-target facets (arbitration), leaving movement untouched. APMF calls neither
  `StartCombat` nor `CastSpellImmediate` (INVARIANTS #0).
- **Combat ACTIONS (attack/block/power/bash, melee micro) are now GATED (ch.7, graduated 2026-09-03)** — a
  `kIntent_CombatAction` claim denies the "offense" category (Attack/AttackLow/Bash/RangedAttack/
  SpecialAttack/GroundAttack/FlyingAttack/CastImmediateSpell/CastConcentrationSpell/CastShout/
  PrepareDualCast leaves) for its actor; every other leaf (movement, block, dodge, cover, search, ...)
  still fires natively. They can ALSO be SHAPED, as before: **equipment (ch.15)** dictates melee-vs-ranged
  and the available action set, and **aggression/confidence AVs (ch.11)** bias the tree's choices.
- **Net:** APMF lets MFO gambits fully PERFORM the target / spell / disposition layer of combat, DENY the
  offense category of the melee layer outright, and SHAPE what remains (loadout + disposition) — only a
  finer-grained melee-action category split (defense/movement/utility) is still future work.
