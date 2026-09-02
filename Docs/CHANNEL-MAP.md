# APMF Channel Map (exhaustive)

The core blueprint: for each directable AI-control channel, its source, the clean **deny** gate
(suppress at the source, no re-assert), the **promote** mechanism, version-robustness, and a
**DOCUMENTED vs GAP** verdict. Derived 2026-09-02 from CommonLibSSE-NG RE headers, the CK wiki, and
existing mods (TDM, NFF, PapyrusUtil, Puppeteer). Governed by design.md §1a: gate INPUTS at the
source, never FORCE outputs (a re-assert loop is a code smell); APMF routes, it does not manufacture.

`RE/…` = CommonLibSSE-NG headers. `// 0xNN` = in-header vfunc index (index-stable across runtimes);
struct-member offsets are accessor/Address-Library covered but more version-sensitive. Taxonomy is
OPEN (17 channels + sub-splits). Combat-target / combat-actions / casting (⭐) carry the weight.

## Table

| # | Channel | Source | DENY gate | PROMOTE | Verdict |
|---|---------|--------|-----------|---------|---------|
| 1 | **Movement / locomotion** | Package planner (`AIProcess.currentPackage`) → `MovementControllerNPC`→`ActorMover` | **FULL BLOCK (built):** `KeepOffsetFromActor(self, 0)` nulls the move GOAL at the source + `SetDontMove` locks translation → clean stand-still (no run-in-place, no teleport-snap; `SetDontMove` ALONE was one layer too shallow). Both Address-Library bound | `IMovementDirectControl` feed (unnamed `Unk_01..08`) or `Actor::Move` 0xC8 (probe-gated) | **DENY/full-block DOCUMENTED; PROMOTE feed GAP** |
| 1a | ↳ **Gait / speed** | `PreferredSpeed` walk/jog/run; `kSpeedMult` AV=30 | **TRUE:** set `kSpeedMult` AV | same AV / package speed flag | **DOCUMENTED, clean** |
| 2 | **Facing / heading** | Same planner (rotation) | rides movement gate (`SetAIDriven`) | same direct-control feed | shared w/ movement; standalone = GAP |
| 3 | **Stance / sneak** | Package flag → `actorState1.sneaking` | clean only via package flag; anim-event = additive | `NotifyAnimationGraph("SneakStart/Stop")` vfunc 01 | **DOCUMENTED (Tier A)**, re-assert caveat |
| 4 | **Weapon draw / sheathe** | `actorState2.weaponState` (sticky) | sticky, not per-frame | `DrawWeaponMagicHands(bool)` vfunc 0xA6 | **DOCUMENTED** |
| 5 | **Headtracking** | AI writes per-type `HighProcessData.headTrackTarget[]` | **TRUE:** own the slot (`SetHeadtrackTarget(type,ref)`) | same setter | **DOCUMENTED, clean gate** |
| 6 | **Combat-target** ⭐ | `AIProcess.currentCombatTarget` (+ `CombatController.targetHandle`), re-chosen each tick by threat | `StartCombat(actor,target,nullptr)` (3-arg) INITIATES; compare-and-write of `currentCombatTarget` each drift-Tick COMMANDS/HOLDS (the measured pin; clear of AE hazard) | `StartCombat` (initiate) + `currentCombatTarget` write (hold) | **HOLD = true PIN (measured, currentCombatTarget compare-and-write)** |
| 7 | **Combat ACTIONS** ⭐ | Internal **combat behavior tree** (`CombatBehaviorController`) + `CombatInventory` + `CombatState` | **NONE** (`AttackBlockHandler` is player-only) | additive anim events fight the tree; real lever = loadout (ch.15) + aggression (ch.11) | **GAP** |
| 7a | ↳ attack/block/power/bash | `ATTACK_STATE_ENUM`; `meleeAttackState`; melee-vs-ranged = equipped weapon | (share ch.7 GAP) | — | **GAP** |
| 8 | **Casting** ⭐ | SELECT: `Actor.selectedSpells[slot]`→`MagicCaster.currentSpell`. TRIGGER: internal combat-caster state machine | **SELECT = TRUE gate:** own `selectedSpells[slot]` → AI casts your spell. **TRIGGER: no suppressor** | trigger via `CastSpellImmediate` vfunc 01 (additive injection — MFO's path) | **drive DOCUMENTED; trigger-gate GAP** |
| 8a | ↳ L/R/dual/staff | `CastingSource` kLeftHand/kRightHand/kOther/kInstant; per-slot casters | own the slot's spell | `GetMagicCaster(source)`→`CastSpellImmediate` | **DOCUMENTED per-hand** |
| 9 | **Package-procedure activity** | `TESPackage.procedureType` (sandbox/patrol/guard) | **No gate** — `SetRunOncePackage` = substitution (OnPackageEnd, §5 rejects) | compose from primitives (locomotion + `PlayIdle` + `ActivateRef` + stance/headtrack) | **GAP native; DOCUMENTED workaround** |
| 10 | **Dialogue / greeting** | AI greeting/dialogue + topics | `SetDialogueWithPlayer` 0x41 / `StopCurrentDialogue` 0x4F | `InitiateDialogue` 0xD8 | **DOCUMENTED** (coarse) |
| 11 | **AI-attribute** (aggression / confidence / assistance / morality) | Dynamic AVs the engine's own combat/flee/assist decisions read (`kAggression`/`kConfidence`/`kMorality`/`kAssistance`) | **TRUE — the design's preferred model:** `ActorValueOwner::SetActorValue`/`ModActorValue` sets the input the AI itself consumes; no override | same setter | **DOCUMENTED, cleanest gate** |
| 12 | **Idle / animation** | AI idle manager | additive (no gate on AI's own idles) | `AIProcess::PlayIdle` one-shot; `NotifyAnimationGraph` | **DOCUMENTED** (one-shot) |
| 13 | **Facial expression / mood** | FaceGen emotion (`EmotionType`) | `ClearExpressionOverride` releases | **SetExpressionOverride not exposed** in this CommonLib build (raw fn needed) | **GAP** (setter) |
| 14 | **Shouts / abilities / powers** | `selectedPower`; voice slot; `GetCurrentShout` | select which power/shout occupies the slot | `EquipShout` / `AddCastPower`; trigger like casting | **DOCUMENTED select** |
| 15 | **Equipment** (equip/unequip specific item) | Combat/package equip choices — sets what ch.4/7/8 work with | own the equipped set (remove an item = deny the AI that option — clean input-gate) | `ActorEquipManager::EquipObject`/`UnequipObject`/`EquipSpell`/`EquipShout` (scar: off-main → `MainThread::Post`, MFO #62) | **DOCUMENTED — the melee-vs-ranged lever** |
| 16 | **Detection / stealth** | `sneaking` (=ch.3); `HighProcessData.detectionModifier`; AVs kDetectLifeRange/kMovementNoiseMult | **TRUE:** set the detect AVs / `detectionModifier` | same | **DOCUMENTED** (AV path clean) |

## Synthesis

**Clean source-gates ready to build now (set the input the AI reads — no re-assert):**
- **AI-attribute AVs (ch.11)** — the textbook case of the design's preferred model and the right lever
  to bias combat/flee behavior. Cleanest gate on the board.
- **Movement DENY (ch.1)** `SetAIDriven`; **Gait/speed (ch.1a)** `kSpeedMult`; **Headtrack (ch.5)** own-the-slot;
  **Detection AVs (ch.16)**; **Casting SELECTION (ch.8)** own `selectedSpells`; **Equipment (ch.15)**.
- **One-shot promotes (no loop):** weapon draw (ch.4), idle/anim (ch.12), dialogue (ch.10), shout/power select (ch.14).

**Need live probing:** Movement PROMOTE feed (`IMovementDirectControl::Unk_0N`, §9.1 — biggest unknown);
Combat-target PIN vs threat re-selector (ch.6); Combat ACTIONS behavior tree — no gate (ch.7); Casting
TRIGGER suppression (ch.8); Sustained package procedures (ch.9); Facial-expression SETTER (ch.13).

## Combat / casting verdict (the MFO headline)
- **TARGET + SPELL + AI-DISPOSITION are drivable at a DOCUMENTED level.** Gambits can steer the target
  (`StartCombat`), cast the exact spell per hand (own `selectedSpells[slot]` + `CastSpellImmediate`), and
  bias the engine's own combat decisions cleanly via aggression/confidence AVs. Casting SELECTION is even
  a clean source-gate (the AI casts your spell itself), a strict improvement over MFO's current
  force-over-the-top; only the TRIGGER stays additive (the internal combat caster has no documented gate).
- **Combat ACTIONS (attack/block/power/bash, melee micro) remain a GAP** — the combat behavior tree owns
  them with no exposed gate. But they can be SHAPED cleanly: **equipment (ch.15)** dictates melee-vs-ranged
  and the available action set, and **aggression/confidence AVs (ch.11)** bias the tree's choices. What's
  missing is only the exact per-swing action stream.
- **Net:** APMF lets MFO gambits fully PERFORM the target / spell / disposition layer of combat and SHAPE
  the melee layer (loadout + disposition), leaving only the exact melee-action stream to a future probe of
  the combat behavior tree.
