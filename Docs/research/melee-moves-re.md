# Melee moves (T2 batch M): reverse engineering notes (2026-11-11)

Copied from the authors' working log. PROVEN = read in the disassembly of the 1.6.1170, 1.5.97 and 1.7.104 executables as noted; nothing here has been observed in a running game. "What was built" at the end says what each item became.

- Harbinger: feat/melee-moves from origin/feat/ch8b-dead-target b96edf0, merged origin/feat/aversion c6df79b -> 20b8c52 (clean, no conflicts).
- MFO: feat/melee-moves from origin/feat/g7-reanimate 533d6ef, merged origin/feat/class-aversion a1b620f -> c2c49bb (clean).
- Read: briefs COMMON + Batch M, manifest s2/s3, Harbinger CLAUDE.md, ch.18 + ch.31 ABI blocks, core/CombatMove.{h,cpp} whole, ActionGate act thunk, MFO Fire.cpp power attack, CombatStyle.{h,cpp} ApplyTick, classes/Package.cpp.
- RE toolkit (not in the repo; in git history at 5e8a9ab under tools/re_melee/): rx.py (rt = ae|se|r17; dis/xref/vt/ptr/rip/str/rev), scan.py (byte pattern), atkd.py (race ATKD dump), cc38.py.

## FINDING item 3 (power floor): ALREADY SHIPPED, no code
- Harbinger v34 `powerShareMin` is LIVE (core/AttackSelect.cpp EvaluateRoll: `share < minShare -> active |= Normal`; not observe-only; APMF.ini [AttackSelect] bObserveOnly=0). "Normal share max X" == powerShareMin (1-X) over the same Power+Normal window.
- MFO classes/ClassTable.cpp:344/354/370 declares 0.40 for Warrior / Champion / Living Legend; apmf/AttackMix.cpp files it. The manifest row was written from the 11-08 draft ("v1 has powerShareMin observe-only"). => no ABI, docs note only.

## RE (1.6.1170; SE/1.7 twins verified where noted)
- Engine NPC block = DoAction(actor, idx) AE id 44446 0x7F9770: TESActionData{prio 2, source actor, action = DOBJ[table 0x2016198[idx]]}, NO animEvent, Process (vt+0x28). table = {49 RightAttack, 51 RightRelease, 70 RightPowerAttack, 45 LeftAttack, 47 LeftRelease, 49, 51, 52, 59, 55}.
  Block leaf (vt 0x18DB6C8): act 0x86C510 pushes 0x18 -> Enter 0x86AE30 (DoAction(actor,3) at +0x1D7, timers), pop 0x86C5F0 -> DoAction(actor,4). BlockAttack leaf (vt 0x18DB568): Enter 0x86A890 DoAction(3) at +0x1F5, pop 0x86C6C0 DoAction(4). Other idx-4 fns 47833 / 47838: no code xref found.
- Mediator post-action step AE 39004 0x6CF760: action == DOBJ 0x2D (LeftAttack) && animEvent == "blockStart" (global 404003) -> NPC: `or [actor+0xCC],0x100` (wantBlocking=1) at +0x4DF (player only: weapon type 7/9 + attack state + stamina>0 pre-tests). action == 0x2F (LeftRelease) && event == "blockStop" -> clear at +0x50E. SE 38048 0x63D0F0 +0x4A2 / +0x4E1 (actor+0xC4), 1.7.104 0x6E2747 / 0x6E2776: same logic.
  Other clearers of wantBlocking (AE): 29383+0x1B2E (dead-ish event), 38696+0x3D5 (0x6BFB10, callers 40352, 42867), 40447+0xCF6 (hit: stamina <= 0 -> clear, the engine's block-break rule).
- Post step attack state: event found in base attack map -> bash bit -> state 6; else weapon type 7/9 -> 8 (bow), else 1 (kDraw). So a POWER entry reads back attackData == entry, state kDraw.
- StartAttack: pool gather 49176 0x8A3960 takes EVERY entry of the hand/bash class (type 0 right: !bash !left; 1 bash; 2 left), no weapon filter; after the draw, a FALSE from Execute (0x7F9470 -> Process) erases the pick and redraws (0x8A2CEA test al / fallthrough to 0x8A2D53). So the engine itself tries entries until the graph accepts one.
- Vanilla NordRace power entries (flags 0x04): H2HCombo 1.0, DualWield 1.0, ForwardH2HRightHand 1.0, Forward 0.5, InPlace 0.2, Left 0.2, Right 0.2, Backward 0.1, sprint ones 0.0.
- COMBAT STYLE: controller+0x38 is a CACHE. CombatController::UpdateCombatStyle AE 33239 0x559BD0: `[cc+0x38] = Actor::GetCombatStyle(attacker)` (call at +0x70 -> 38536 0x6B5580), marks inventory dirty (+0x1C4) on change. Callers: ctor 33214+0x296, 33240+0x1C (from 39207 = package CHANGE), 418635+0x22 (from 37652), tail jmp from Actor::SetCombatStyle 38537+0x31. NOT per tick.
  Actor::GetCombatStyle 38536: ExtraCombatStyle (call 12017 0x16BFC0 at +0xD, its ONLY caller) ?: currentPackage->combatStyle (proc+0x20 -> +0x68) ?: base vfunc 0x54. 27 callers: the 0x8DCxxx style accessor family (offensive, circle, W, block ...) read the ACTOR's style, never cc+0x38. cc+0x38 readers (heuristic scan): 44889 (flags +0x9C), 45084/45085 (magic/shout/staff score mults) = combat inventory scoring only.
  => today's MFO write to cc+0x38 only biases equipment choice; class CSTY movement/attack fields are NOT applied by it. Facet design: seat B write_call at 38536+0xD (actor-wide), seat A at 33239+0x70 (controller-cache-only scope), engine's own refresh 33239 at engage/release.

## COMBAT STYLE: the per-runtime twins (item 5)
- Actor::GetCombatStyle: AE 38536 0x6B5580 / SE 37586 0x623E60 / 1.7.104 0x6C8000. Same body on all three (process at +0xF8 AE and 1.7.104, +0xF0 SE). The call to ExtraDataList::GetCombatStyle is at +0xD on all three, after `add rcx,0x70`. 27 rel32 callers on each build (the same list on AE and SE: 26416 / 25850 +0x8E, UpdateCombatStyle, and the 0x8DCxxx / 0x844xxx accessor family).
- ExtraDataList::GetCombatStyle: AE 12017 0x16BFC0 / SE 11878 0x1246E0 (takes the list's lock out of line) / 1.7.104 0x171550. ONE caller per build.
- CombatController::UpdateCombatStyle: AE 33239 0x559BD0, call at +0x70 / SE 32492 0x4FE5B0, call at +0x8A / 1.7.104 0x561A40, call at +0x70. `mov [rdi+0x38],rax` right after on all three; `[[this+0x10]+0x1C4] = 1` when the old style was non-null and differs. Callers: AE ctor 33214 +0x296, 33240 +0x1C, 418635 +0x22, tail jmp from 38537 +0x31. SE: 32493 +0x1A (32493 is called by the ctor 32467 +0x280 and 38247 +0x9B), 32499 +0x10, tail jmp from 37587 +0x31. 1.7.104: 0x560176, 0x561B2C, 0x562642, tail jmp 0x6C8091.
- Actor::SetCombatStyle: AE 38537 0x6B55E0 / SE 37587 0x623EC0 / 1.7.104 0x6C8060: sets the ExtraCombatStyle (12018 / 11879 / 0x171580), a change flag (vfunc +0x50, 0x80000000), then tail-jumps to UpdateCombatStyle with the actor's controller (+0x160 AE and 1.7.104, +0x158 SE).
- Seat byte windows, identical on the three builds: B lead `40 53 48 83 EC 20 48 8B D9 48 83 C1 70`, E8, tail `48 85 C0 75 31`; A lead `48 89 4C 24 30`, E8, tail `48 89 47 38 48 85 F6`.

## DODGE (item 4): what was read, and why no dodge facet was built
- The engine's side step is the leaf CombatBehaviorDodgeThreat (AE vtable 0x18DAFD0: act 0x86A0F0, pop 0x86A160, update 0x86A670, slot 5 0x869FC0).
- Its gate reads the style's avoidThreatChance (TESCombatStyle +0x44 = generalData +0x24) through the ACTOR's style: condition function AE 47801 0x868930 (referenced by `lea` from the tree builder 47807 +0x20B) resolves the controller's attacker, calls the accessor AE 50706 0x8DDC00 at +0xB1 (`call Actor::GetCombatStyle; movss xmm0,[rax+0x44]`; its only caller), then rolls (26618) and tests the actor's state. So the chance comes through Actor::GetCombatStyle, the function ch.33's seat B answers.
- Before ch.33 a client's style reached only controller+0x38, which this path never reads: a class style's Avoid Threat chance had no effect.
- Dodge frameworks (read for integration only, no code copied): Ultimate NPC Dodging hooks the DodgeThreat / fallback / circle / backoff path creation and sends TKDodge* animation events itself, scoring from the combat style's avoidThreat / fallback / circle / strafe fields. TK Dodge RE is player only. Simple NPC Dodge is perk / spell / OAR driven.
- Conclusion: the list-agnostic road is the combat style facet. A client that claims a style with an Avoid Threat chance gets the engine's side step where no dodge mod is loaded, and a framework that scores from the style rolls its own dodge where one is. Not verified: whether Ultimate NPC Dodging reads the style through Actor::GetCombatStyle or from the base record. Not built: a "dodge now, this way" move; it would need each framework's event names resolved at runtime and the DodgeThreat leaf's path creation read on the three builds.
- MFO side still open: its melee class styles carry avoidThreatChance 0, so the Mercenary asks for no dodge yet (MFO REVIEW-BACKLOG MFO-B806).

## What was built (feat/melee-moves)
- Item 1, ABI v43: kCombatMove_Block / kCombatMove_BlockEnd, a held guard (core/CombatMoveBlock.cpp, core/ActionGate.cpp ActThunkBody). No new address row.
- Item 2, ABI v44: kCombatMove_PowerAttack (core/CombatMove.cpp GatherPower + Run). No new address row.
- Item 3: no code. ch.18 powerShareMin (ABI v34) is the floor.
- Item 5, ABI v45: kIntent_CombatStyle = 33 (core/CombatStyle.{h,cpp}, channels/CombatStyle.cpp). Three address rows per runtime.
- Item 4: no facet, no ABI number. See DODGE above.
