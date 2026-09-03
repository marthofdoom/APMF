# APMF Universal Allowance Template — engine command-layer research + architecture

Status: **authoritative design reference** (Fable deep-research spike, 2026-09-02). Read before building any
allowance channel. This is *what the engine actually exposes* and the one reusable pattern APMF uses to gate
it. Evidence: every vfunc index below is a literal `// 0xNN` in a CommonLibSSE-NG header; every reloc ID is a
literal `REL::ID` in `Offsets_VTABLE.h` or a maintained mod's source. Sources: pinned po3-dev CommonLib
headers, MFO `Docs/ENGINE_NOTES.md`, MFO `native/CombatStyle.cpp`/`CasterConsent.cpp`, installed mods
(CombatPathingRevolution, valhallaCombat, PayloadInterpreter, TDM, Precision, DAC, po3), Linux-Native-Tools.

## 0. The premise (marth)

APMF is the universal **allowance authority** over NPC actions: a client declares intent through the API;
APMF **allows** the intended action and **denies** alternatives *at the source* (the decision the actor
receives), so the engine executes only what's permitted — natively, animated, no manufacture, no re-assert.
Anything an actor does or does not do is APMF's responsibility, in ALLOWANCE not in design. A capability a
client needs that isn't reachable through the API is an APMF gap (see `gate-the-source` design memo).

## 1. The engine's commonality is a SMALL SET of generic interfaces, layered by domain (not one hook)

| Layer | Generic mechanism | Evidence |
|---|---|---|
| **Combat decisions** | ONE node interface `CombatBehaviorTreeNode { GetName 01, Enter 02, Exit 03, Update 04, Abort 05, Save 06, Load 07, Validate 08, GetType 09 }`, instantiated as **~70 leaf vtables** `VTABLE_CombatBehaviorTreeNodeObject_CombatBehavior{Attack, AttackLow, Bash, Block, BlockAttack, CastImmediateSpell, CastConcentrationSpell, CastShout, PrepareDualCast, DrinkPotion, EquipObject, EquipSpell, EquipShout, EquipRangedWeapon, Flee(+variants), DodgeThreat, Idle, Advance, Backoff, Circle, Strafe, Chase, PursueTarget, FollowPath, Reposition, Surround, Flank, Orbit, FaceAngle, TrackTarget, WatchTarget, Hide, FindCover, Search*, RangedAttack, SpecialAttack, GroundAttack, FlyingAttack, AcquireItem, FindWeapon, Pause/Repeat/Sequence/Parallel/ForceFail/ForceSuccess}` + generic selectors (Conditional/Value/WeightedRandom child selectors, Fallback, Sequence, Parallel) + ~30 named subtrees in `CombatBehaviorTreeManager::trees` (AL 32551/33306). | `C/CombatBehaviorTreeNode.h:18-28`; `C/CombatBehaviorThread.h` (stackFrame (node,u32); `currentNode` 0x138, `controller` 0x158); `C/CombatBehaviorTreeManager.h`; `Offsets_VTABLE.h:3509` Attack SE 213789 / `:10849` AE 266747, Block 212608/265987, CastImmediateSpell 213720/266705 |
| **Combat resource decisions** | ONE base `CombatObject` with per-domain "may I" bool vfuncs: `CombatInventoryItem::CheckBusy 0E / CheckShouldEquip 0F` (+ `CalculateScore 0C` = selection); `CombatMagicCaster::CheckStartCast 06 / CheckStopCast 07` (+ `CalcCastMagicChance 08`, `GetMagicTarget 0A`). | `C/CombatInventoryItem.h:65-80`, `C/CombatMagicCaster.h:26-34`; 87 `VTABLE_CombatInventoryItem*` (75 are `MagicT<item,caster>` instantiations), 16 `VTABLE_CombatMagicCaster*` |
| **Body commands (all contexts)** | ONE command object: `ActionInput{source, target, BGSAction*, Priority{kImperative,kQueue,kTry}}` + `ActionOutput{animEvent,result}` = `TESActionData`, executed by `BGSActionData::Process() // 05 -> bool`. Vocabulary = `DEFAULT_OBJECT kAction*` 43–112: Left/Right/Dual Attack/Ready/Release/Interrupt, PowerAttack, Activate, Jump, Sneak, Voice(Ready/Release/Interrupt), Idle, Draw, Sheath, BlockHit/Anticipate, Stagger, Bleedout, MoveStart/Stop, Turn*, TalkingIdle/ListenIdle, Death, Knockdown, GetUp. | `A/ActionInput.h:18-36`, `A/ActionOutput.h`, `B/BGSActionData.h:20`, `T/TESActionData.h:22`, `C/CombatAnimation.h:12-44` (`Execute(){ return actionData.Process(); }`), `B/BGSDefaultObjectManager.h:56-125`; `VTABLE_TESActionData` SE 188603 / AE 232777 |
| **Animation execution** | ONE string channel: `IAnimationGraphManagerHolder::NotifyAnimationGraph(BSFixedString&) // 01` (game→graph) and ONE sink `TESObjectREFR::ProcessEvent(BSAnimationGraphEvent*) // 01` (graph→game). Sub-vtables: `VTABLE_Character[3]` = holder, `[2]` = sink. **OUTPUT-side → OBSERVER, not an allowance point** (denying here = whack-a-mole + the MagicCaster "state 1 wedge", ENGINE_NOTES:434-441). Perfect universal passive telemetry. | `I/IAnimationGraphManagerHolder.h:26`, `T/TESObjectREFR.h:99-102,225`, `B/BShkbAnimationGraph.h:32` |

**Native deny bits (no hook, wholesale per-domain):** `Actor::BOOL_FLAGS` (`Actor.h:203-238`) `kCanSpeak 1<<13`, `kAttackOnSight 1<<15`, `kIsCommandedActor 1<<16`, `kAttackingDisabled 1<<20`, `kCastingDisabled 1<<21`, `kMovementBlocked 1<<27`; `BOOL_BITS::kProcessMe 1<<1` (po3 FreezeActor); `MovementControllerNPC::SetAIDriven 0C / SetControlsDriven 0D`; `SetDontMove`(36490/37489)+`KeepOffsetFromActor(self,0)`(36870/37894) = APMF's proven full block; detect/aggression/confidence AVs.

**NOT generic — package procedures:** `TESPackage::procedureType` has 49 values (`T/TESPackage.h:85-137`) but `IProcedureTreeItem`/`BGSProcedureTreeProcedure` expose only `Unk_XX`; no reversed per-procedure Update/Execute exists in the tree. Out-of-combat *activities* (sandbox/travel/use-item/activate-by-package/greeting) are gateable only at the package OFFER (0x49) or via flags/AVs — never per-procedure.

## 2. Ranking by closeness to a universal chokepoint WITH an allow/deny seam

1. **Combat behavior-tree leaf `Enter`/act (slot 0x02) on the ~70 leaf vtables.** One interface, one thunk, one install loop; covers every combat action. Deny = the engine's own failure protocol (`CombatBehaviorTreeControl::SetFailed(bool)` SE AL 46240; `Ascend` 46229 — CPR `src/RE/CombatBehaviorTreeControl.h`; CPR names slot 04 `on_childfailed`; `CombatBehaviorForceFail` is a shipped node doing exactly this). Precedent: CombatPathingRevolution subclasses these leaves (RTTI-resolved, SE + AE). **Closest to marth's "commonality".**
2. **`CombatObject` Check* family** — `CheckShouldEquip 0F`, `CheckBusy 0E`, `CheckStartCast 06`, `CheckStopCast 07`, plus **`MagicCaster::CheckCast 0A` on `VTABLE_ActorMagicCaster[0]`**. Finer grain (per item / per spell). Proven: MFO `CombatStyle.cpp:235-305` + `CasterConsent.cpp:658-879`, third parties DragonWar.dll (`CheckShouldEquip`) and MSCO (`RequestCastImpl 03`, ENGINE_NOTES:578-584). Field facts: `CheckStartCast 06` is **ADVISORY** (a denied spell still fired, `CasterConsent.cpp:643-645`); **`CheckCast 0A` is the HARD pre-charge gate**, fires IN and OUT of combat (`CasterConsent.cpp:834`), carries `CannotCastReason*`; `CombatMagicCasterRestore` also governs combat potion drinking (ENGINE_NOTES:1266-1269). `Actor::CheckCast // 110` (`Actor.h:476`) is the actor-level twin.
3. **`TESActionData::Process` / the attack-action callee** — the only mechanism spanning combat + non-combat + player + NPC for BODY commands. Precedent: valhallaCombat hooks `bool PerformAttackAction(TESActionData*)` by `write_call<5>` at `RELOCATION_ID(48139,49170)+0x4D7/0x435`. Caveat: `CombatAnimation::Execute` calls `actionData.Process()` on a by-value member → devirtualised, so a `VTABLE_TESActionData[0]` slot-5 hook may NOT see AI calls; the callee entry (from valhalla's site) is the real seat. Coverage across kAction* is header-unproven → PROBE.
4. **`Actor::CheckForCurrentAliasPackage // 049`** (`Actor.h:302`) — the package-tier allowance, LIVE-PROVEN by APMF Phase 0 (fires through the vtable, several threads). Covers all package-driven activity as "which package is the actor offered". See `apmf-package-deny-via-vfunc-0x49`.
5. **Native deny bits / AVs** (above) — no hook, most version-stable, wholesale not per-decision.
6. **`NotifyAnimationGraph` (`VTABLE_Character[3]:1`) + graph sink (`[2]:1`)** — universal but OUTPUT-side → **OBSERVER only** (passive telemetry of every action string per actor), never an allowance point.
7. **Targeted actor command vfuncs** — `DrawWeaponMagicHands 0A6`, `SetDialogueWithPlayer 041`(bool), `InitiateDialogue 0D8`, `PutCreatedPackage 0DF`, `Move 0C8` — per-action secondary gates.

## 3. THE UNIVERSAL ALLOWANCE TEMPLATE

One reusable shape, four attach points. Every instance: **decision vfunc → resolve the deliberating actor →
lock-free claim lookup → let the engine answer FIRST → flip YES→NO only; never invent a YES, never call
anything, no Tick, no re-assert.**

```
T(vtable_symbol[], slot, ActorResolver, ClaimFacet, DenyForm):
  install: for each concrete vtable symbol (SE/AE from Offsets_VTABLE.h; VR -> refuse):
             verify RTTI/derivation at install (the CombatMagicCasterArmor lesson, ENGINE_NOTES §0.28),
             orig[vt] = write_vfunc(slot, thunk)
  thunk(this, args...):
    o = orig[*(vt*)this]; if (!o) return DenyForm.benign      // foreign object -> do not touch members
    engineSays = o(this, args...)                             // engine's own answer first
    if (engineSays == NO) return NO                           // nothing to own
    actor = ActorResolver(this, args...)                      // FormID only, no lists
    if (!ControlMap.has(actor)) return engineSays             // ~1 relaxed load / hash, the common case
    return Claim(actor, ClaimFacet).allows(subject(args)) ? engineSays : DenyForm.no
```
The claim lookup MUST be the lock-free RCU ControlMap read (INVARIANTS #12/#13), FormID-keyed, no
follower-list access, no engine writes in the thunk. (MFO's `CheckShouldEquip` takes a mutex — APMF must NOT.)

| Instance | vtables / slot | ActorResolver | DenyForm | Covers |
|---|---|---|---|---|
| **T1 combat-action** | ~70 × `VTABLE_CombatBehaviorTreeNodeObject_*`, slot 0x02 (`Enter`/`act(CombatBehaviorThread*)`) | thread→`controller`(0x158)→`CombatBehaviorController::combatController`(0x20)→`CombatController::attackerHandle`(0x28, <0x68 AE-safe); or static `GetCurrentAttacker()` if the rev binds it (unverified — INVARIANTS #8) | do NOT call orig; `SetFailed(true)`; return thread → parent selector picks next allowed child | attack/power/bash/block, dodge/flee, cast (immediate/concentration), shout, equip spell/object/shout/ranged, potion, combat movement leaves, combat headtrack, search/cover |
| **T2 combat-resource** | 87 × `VTABLE_CombatInventoryItem*` slot 0x0F (`CheckShouldEquip`) [+ 0x0E `CheckBusy`]; 15 valid `VTABLE_CombatMagicCaster*` slot 0x06 (advisory, pacing only); **`VTABLE_ActorMagicCaster[0]` slot 0x0A (`CheckCast`, HARD)** | `CombatController*→attackerHandle` (T2a/b); `MagicCaster::GetCasterAsActor 0C` (T2c) | return false (+ `CannotCastReason` on 0x0A) | per-ITEM equip choice; per-SPELL cast in and out of combat; potions via Restore caster |
| **T3 package-offer** | `VTABLE_Character[0]` slot 0x49 | `this` | return client's `TESPackage*` (never null — §0.25 claimed-with-nothing = rooted) | all package-driven activity (travel, sandbox, use-item, activate-by-package, greeting) |
| **T4 body-command** (PROBE-gated) | callee of valhalla site `RELOCATION_ID(48139,49170)+0x4D7/0x435` (`bool f(TESActionData*)`), entry-hooked; fallback `VTABLE_TESActionData[0]` slot 0x05 if the probe shows virtual dispatch | `actionData->source` | return false without forwarding (`kTry` priority = "may fail") | draw/sheathe, sneak, activate, idle, voice, and attack/block/bash outside T1's reach (player/package-issued) |

**Native-bit tier (no hook, wholesale):** `kAttackingDisabled`/`kCastingDisabled`/`kMovementBlocked`/`kCanSpeak`, `SetDontMove`+`KeepOffsetFromActor(self,0)`, `SetActivationBlocked`, AVs. T1–T4 are the per-decision denies that make "allow only X" possible; the bits are the blunt whole-domain denies.

**Why it obeys the rule:** APMF only ever turns the engine's own YES into NO at the point the engine ASKS.
The client's intent (spell/target/package/item) is the only remaining option the engine finds, and it runs it
natively. No re-assert: a denied node/item/spell just isn't selected this deliberation; the next asks again.

## 4. Coverage map

| Action | T1 tree | T2 Check* | T3 0x49 | T4 action | Native bit | Gap |
|---|---|---|---|---|---|---|
| Cast (which spell, whether) | Cast* leaves | **`CheckCast 0A` hard**, `CheckShouldEquip 0F` | — | — | `kCastingDisabled`(?) | none in combat; OOC buff = CheckCast covers |
| Equip weapon/spell/shout/ranged | Equip* leaves | `CheckShouldEquip 0F` (Melee/Ranged/Shield classes: 0x0F inherited from base, RTTI-verify) | — | — | — | package-issued equips (UseWeapon PKDT) |
| Attack/power/bash/block | Attack/Bash/Block leaves | — | — | kAction*Attack/PowerAttack/BlockHit | `kAttackingDisabled`(?) | none in combat |
| Dodge/flee/cover/search | leaves | — | — | — | confidence AV | — |
| Combat movement | leaves (CPR precedent) | — | — | — | `SetDontMove` | — |
| Shout | CastShout/EquipShout leaves | `Shout::CheckShouldEquip 0F` | — | kActionVoice* | — | — |
| Potion | DrinkPotion leaf | `CheckStartCast 06` on Restore (must NOT blanket-deny) | — | — | — | — |
| Move/travel (package) | — | — | **0x49** | kActionMoveStart(probe) | `SetDontMove`,`kMovementBlocked` | locomotion planner has no decision vfunc → 0x49 + full-block |
| Sneak | — | — | pkg flag via 0x49 | kActionSneak(probe) | forceSneak | OOC sneak = pkg flag only |
| Draw/sheathe | — | — | — | kActionDraw/Sheath(probe); `DrawWeaponMagicHands 0A6` 2nd | sticky weaponState | — |
| Activate | — | — | pkg Activate via 0x49 | kActionActivate(probe) | `SetActivationBlocked`; `TESForm::Activate 037`(bool) | per-activator = 0x37 family/T4 |
| Dialogue/greeting | — | — | scene pkgs outrank | kActionTalkingIdle | `SetDialogueWithPlayer 041`, `kCanSpeak`, `PauseCurrentDialogue 04F` | greeting queue no vfunc |
| Idle | Idle leaf (combat) | — | sandbox idles via pkg | kActionIdle(probe) | — | OOC idle: `PlayIdle` non-virtual → GAP unless T4 |
| Headtrack | Track/Watch leaves (combat) | — | pkg headtrack | — | per-type slot (TDM hooks 5 SetHeadtrackTarget sites) | non-combat headtrack types |

**Honest gaps:** package PROCEDURES (0x49 only); OOC idle/sneak/activate if T4 fails; `Melee/Ranged/Shield/Torch`
CombatInventoryItem classes + every behavior-tree leaf/context class are vtable symbols WITHOUT header bodies
→ index inheritance must be RTTI-verified at install, never assumed; `SetFailed` has only an SE ID in public
source (AE must be derived — CPR-AE or the `ForceFail` leaf body); `kAttackingDisabled`/`kCastingDisabled`
readers unproven (probe: set the bit, observe).

## 5. Version-robustness & threading

- **Robust:** T1–T3 are vtable slots on CommonLib `VTABLE_*` symbols with SE+AE IDs in `Offsets_VTABLE.h`
  (index-stable, no call-site offsets). VR: **refuse at install** (indices unverified — MFO doctrine). RTTI
  symbols exist for every leaf (`RTTI_CombatBehaviorTreeNodeObject_*`, 172 entries) → an install-time
  derivation walk (confirm base `CombatBehaviorTreeNode`) is marth's requested built-in heuristic.
- **Less robust:** T4 is an AL function (call-site-derived entry); `SetFailed`/`Ascend` are AL functions (SE
  46240/46229) — the only non-vtable dependencies. `CombatController` members must stay `< 0x68` (§0.29).
  Two REs disagree on `CombatBehaviorThread+0x158` (CommonLib: `CombatBehaviorController*`; CPR:
  `CombatController*`) — resolve on the probe before reading through it.
- **Threads:** T2 `CheckCast` is confirmed NON-main (`STATUS.md:1148`); 0x49 fires from several threads (Phase
  0); `CheckShouldEquip`/`CheckStartCast`/T1 run inside the combat AI update. All thunks use the lock-free RCU
  ControlMap read — never a mutex (contrast MFO `CombatStyle.cpp:276`), never a follower-list touch.

## 6. Minimal set + probe plan

Smallest set, widest coverage: **T1 (one thunk, ~70 vtables) + T2c (`CheckCast 0A`, 1 vtable) + T2a
(`CheckShouldEquip 0F`, 87 vtables) + T3 (0x49, 1 vtable)** — four attach points, one template. T4 is the
fifth if its coverage probe proves it the shared body-command seat.

**3-cycle probe before building on T1/T4:**
0. Install T1 OBSERVE-only on one NPC; log `GetName()` per leaf `Enter` for a fight (proves vtable dispatch +
   derivation; maps which leaves fire).
1. Deny the `Attack` leaf via `SetFailed(true)`; confirm the tree falls back (block/circle) with no stutter,
   no re-entry storm.
2. T4: read the rel32 target at valhalla's site, compare with `VTABLE_TESActionData` slot 5; log `BGSAction`
   editorID + priority + source for one NPC across combat/sandbox/dialogue/player-command to measure coverage.

## 7. Build order (marth-approved, proven-first)

1. RCU thread-safe ControlMap — **DONE** (`fea17d2`).
2. Template infra + the **proven** attach points: **T2c `CheckCast`** (the cast allowance — deny any spell but
   the claimed one → the AI casts only ours, animated; solves the 3-cycle cast saga as the first template
   instance) + **T2a `CheckShouldEquip`** + **T3 `0x49`** (already built as the probe; fold into the template).
   MFO's owned cast becomes a pure client: declares the spell via ch.8; APMF enforces via T2. MFO drops its
   own cast-side `CasterConsent`/`CheckShouldEquip` (Option B).
3. PROBE T1 (combat-tree deny via `SetFailed`) + T4 (`TESActionData::Process`).
4. Expand to full T1/T4 coverage; migrate the rest of MFO's private control hooks to API requests.
