# APMF Channel Map

The core blueprint: for each directable AI-control channel, its source, the clean **deny** gate
(suppress at the source, no re-assert), the **promote** mechanism (hand authority to a chosen
channel), version-robustness, and a **DOCUMENTED vs GAP** verdict. Derived 2026-09-02 from
CommonLibSSE-NG RE headers, the CK wiki, and existing mods (TDM, NFF, PapyrusUtil, Puppeteer).
Governed by design.md §1a: gate INPUTS at the source, never FORCE outputs (a re-assert loop is a
code smell); APMF routes channels, it does not manufacture behavior.

`RE/…` = CommonLibSSE-NG headers. Vfunc indices (`// 0xNN`) are index-stable across runtimes;
struct-member offsets are accessor/Address-Library covered but more version-sensitive.

## Table

| # | Channel | Source | DENY gate (suppress at source) | PROMOTE | Verdict |
|---|---------|--------|--------------------------------|---------|---------|
| 1 | **Movement / locomotion** | Package procedure planner (`AIProcess.currentPackage`) → `MovementControllerNPC`→`ActorMover` | **TRUE gate:** `MovementControllerNPC::SetAIDriven`/`SetControlsDriven` vfunc 0x0C/0x0D — flips the planner off, no re-assert | `IMovementDirectControl` feed (unnamed `Unk_01..08`) or `Actor::Move` 0xC8 override. `KeepOffsetFromActor` = manufacture (stand-in only) | **DENY documented; PROMOTE feed = GAP** |
| 2 | **Facing / heading** | Same planner (heading) | Rides the movement gate (`SetAIDriven` suspends heading too) | Same direct-control feed writes heading | **Documented (shared w/ movement)**; standalone = same GAP |
| 3 | **Stance / sneak** | Package sneak flag → `actorState1.sneaking` | Clean gate only via owning the package sneak flag; anim-event assert is ADDITIVE (re-assert smell) | `NotifyAnimationGraph("SneakStart/Stop")` (IAGMH vfunc 01) | **Documented (Tier A)**, re-assert caveat |
| 4 | **Weapon draw / sheathe** | Combat/package → `actorState2.weaponState` (sticky) | weaponState is sticky, not per-frame re-derived | `Actor::DrawWeaponMagicHands(bool)` vfunc 0xA6 — clean one-shot | **Documented** |
| 5 | **Headtracking / look-at** | AI writes per-type `HighProcessData.headTrackTarget[]` (kDefault/kCombat/kDialogue/kProcedure) | **TRUE gate:** own the slot — `AIProcess/HighProcessData::SetHeadtrackTarget(type,ref)` / `ClearHeadtrackTarget`. You write the exact input the engine reads | Same setter | **Documented, clean source-gate** |
| 6 | **Combat-target selection** ⭐ | `CombatGroup.targets[]`; `CombatController.targetHandle`, re-chosen each tick by the threat system | No per-source suppressor; `StartCombat(target)` seeds the target, `StopCombat` 0xE5 / `SetCombatGroup` 0xD5 manage engagement. Raw `targetHandle` write = override (offset-fragile) | `StartCombat` with the chosen target | **Documented to STEER; GAP on PINNING** vs the threat re-selector (probe) |
| 7 | **Combat ACTIONS** ⭐ (attack/block/power, melee-vs-ranged) | Internal **combat behavior tree** (`CombatBehaviorController`/`Thread/Tree/Node`) + `CombatInventory` + `CombatState` | **NONE exposed** — no per-input gate on the tree's action choice (`AttackBlockHandler` is PLAYER-only) | Only additive anim events (fight the tree). Melee-vs-ranged set indirectly by equipped loadout | **GAP** — no clean gate; bias via loadout + aggression AV |
| 8 | **Casting** ⭐ (spell SELECT + TRIGGER) | SELECT: `Actor.selectedSpells[slot]` → `MagicCaster.currentSpell` (L/R/Other/Instant). TRIGGER: internal combat magic-caster state machine | **SELECT = TRUE gate:** own `selectedSpells[slot]` / `MagicCaster::SetCurrentSpell` → AI casts YOUR spell, no re-assert. **TRIGGER has no documented suppressor** (internal caster) | Trigger via `MagicCaster::CastSpellImmediate` vfunc 01 (MFO's current path — additive/injection) | **Documented to DRIVE (select+fire); GAP** for a pure trigger source-gate |
| 9 | **Package-procedure activity** (sandbox/travel/guard/furniture) | `TESPackage.procedureType` | **No clean gate** — only starter `SetRunOncePackage` IS substitution (fires OnPackageEnd, §5 rejects) | Compose from primitives: locomotion + `PlayIdle` + `ActivateRef` + stance/headtrack (§4b Tier C) | **GAP** for native; **documented workaround** (compose) |
| 10 | **Dialogue / greeting** | AI greeting/dialogue package + topic system | `SetDialogueWithPlayer` 0x41 / `StopCurrentDialogue` 0x4F | `InitiateDialogue` 0xD8 | **Documented** (Tier A toggles) |

**Not yet fully rowed (add in a follow-up pass):** aggression / confidence / assistance / morality
(dynamic ActorValue overrides — a clean "set the input the AI reads" bias channel, mentioned as the
combat-actions lever); idle/animation playback (`PlayIdle`); facial expression / mood; gait/speed
(sub-channel of movement); shouts / abilities / powers (casting sibling); equipment (equip/unequip a
specific weapon/item); detection/stealth state. Sub-splits: combat actions = attack/block/power/bash;
casting = L-hand/R-hand/dual/staff.

## Synthesis

**Ready to gate now (clean source-gate, no re-assert):**
- **Movement DENY** — `SetAIDriven`/`SetControlsDriven` flips the planner off at its root.
- **Headtracking** — own the per-type target slot (write the exact input the engine reads).
- **Weapon draw** — sticky one-shot vfunc.
- **Dialogue** — vfunc toggles.
- **Casting SELECTION** — own `selectedSpells[slot]`; the AI then casts your spell.

**Need live probing before committing:**
- **Movement PROMOTE feed** — `IMovementDirectControl::Unk_01..08` unnamed (design §9.1); biggest engineering unknown. `Move`/`ModifyMovementData` override is the fallback.
- **Combat-target PIN** — whether `StartCombat` holds the target or the threat re-selector overwrites it each tick.
- **Combat ACTIONS** — no documented gate; whether a `CombatBehaviorThread` can be paused/steered is unknown.
- **Casting TRIGGER suppression** — no documented way to silence the AI's own combat caster.
- **Sustained package procedures** — the one package-entangled facet.

## Combat / casting verdict (the MFO headline)
- **TARGET + SPELL are drivable at a DOCUMENTED level.** Gambits can steer combat to the exact target
  (`StartCombat`) and cast the exact spell (own `selectedSpells` for selection, `CastSpellImmediate`
  for the trigger). The core of a caster gambit is achievable today, and the *selection* half is a
  clean source-gate, better than MFO's current force-over-the-top.
- **Casting: source-gate is cleaner for SELECTION, only additive for the TRIGGER.** Owning the
  selected spell beats forcing (the AI produces the right cast itself). The fire still rides
  `CastSpellImmediate` as injection; no documented gate silences the AI's internal caster, so a fully
  source-gated "no competing cast" is a GAP (probe).
- **Combat ACTIONS (melee attack/block/power stream) are a GAP.** The combat behavior tree owns them
  with no exposed gate. Gambits can BIAS via loadout + aggression/confidence AVs, but cannot cleanly
  PERFORM the exact melee-action stream the way they can drive target and spell.

**Bottom line:** MFO gambits can fully PERFORM the caster/target layer of combat through APMF, but only
NUDGE the fine melee-action layer until (and unless) live probing finds a lever into the combat
behavior tree.
