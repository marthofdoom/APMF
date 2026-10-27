# Combat behaviour tree catalog: provenance and evidence

Provenance: derived from disassembly of SkyrimSE.exe 1.6.1170, 1.5.97 and 1.7.104, clean room, 2026-10-27.

This is the evidence for every value in `native/core/BehaviorTreeCatalog.h` (`apmf::cbt`). That header replaces
`native/core/CombatBehaviorRE.h`, removed the same day because its own comment said it was copied from a
GPL-3.0-or-later CommonLibSSE-NG fork; Harbinger is MIT. marth (2026-10-27) chose a clean-room re-derivation.
Older docs (STATUS history, the probe write-ups, HOOK-SITE-COVERAGE, ADDRESS-TABLE-2026-09-15) still cite
`core/CombatBehaviorRE.h` as a record of what existed then; the current file is `core/BehaviorTreeCatalog.h`.

## Clean-room method

- The author did not open the old header (nor any alandtse source) before the new header was written and
  committed. What the header must provide was taken from its consumers only (`core/ActionGate.{h,cpp}`,
  `core/CastObserve.cpp`, `channels/CombatApproach.cpp`, `channels/CombatSubstrate.cpp`,
  `channels/SubstrateEquip.cpp`): the identifiers they use, the table sizes they assert (70 / 4 / 21 = 95),
  the names they look up and the classification they describe.
- Exposure record (stated so nobody has to guess): before the header was written, three things outside the
  rule's letter crossed the author's screen and are disclosed here: (a) one grep over `*.h` printed a single
  COMMENT line of the old header (line 344, wording only, no value); (b) two `tools/verified_addresses/spec.json`
  rows (our own generated table: the Advance and Attack vtable ids) were read while locating the tooling;
  (c) a `Docs/HOOK-SITE-COVERAGE.md` line quoting the Attack triple from the pinned MIT CommonLib's
  `Offsets_VTABLE.h`. No value was taken from any of them: every value comes out of the scripted derivation
  below, which never reads them.
- Inputs: our own executables (`binaries/1.6.1170/SkyrimSE.unpacked.exe`, `binaries/1.5.97/SkyrimSE.unpacked.exe`,
  `binaries/1.7.104/SkyrimSE.exe`, see `binaries/README.md`); the Address Library files
  `versionlib-1-6-1170-0.bin` and `version-1-5-97-0.bin` used ONLY to name the id of an RVA already derived
  (an inverse lookup: RVA -> id); the fork's published MIT id table `data/mit-idtable-v1-1-7-104-0.bin`, used
  only to CHECK that each AE id resolves on 1.7.104 to the vtable we derived there.
- Tooling: the RTTI helpers of `tools/verified_addresses/gen_verified_addresses.py` (`Image`,
  `vtable_entries`, `td_rvas`, `load_addrlib`, `load_mit_table`) and capstone for the disassembly.

### The catalog (95 vtables)

1. Every RTTI TypeDescriptor in `.data` whose name starts `.?AV?$CombatBehaviorTreeNodeObject@`,
   `...Object1@`, `...Object2@` or `...Object3@` (the class templates every acting node is an instance of;
   `NodeObjectBase<X>` is their non-leaf base and is excluded). The same 95 names on all three executables.
2. For each: the CompleteObjectLocators that point at it (signature 1, self-RVA check) with COL offset 0, and
   the one `.rdata` vtable whose slot -1 points at that locator. Exactly one per class per runtime.
3. Checks per class per runtime: its Class Hierarchy Descriptor's base-class array contains
   `.?AVCombatBehaviorTreeNode@@`; slots 0x00-0x05 all point into `.text`.
4. Id: the 1.6.1170 and 1.5.97 vtable RVA looked up in the respective Address Library (exactly one id each).
   1.7.104: the MIT table's row for the AE id equals the 1.7.104 vtable from step 2 (true for all 95).
5. Split: the 70 `NodeObject<X>` classes are `kLeaves`; the four `NodeObject1<Search*, COMBAT_SEARCH_PRIORITY>`
   are `kSearchLeaves`; the other 21 are `kParamLeaves`. The `ParamKind` of each parameterised node follows
   the allow-list the substrate documents (channels/CombatSubstrate.cpp file header): the two `<bool>` cast
   variants are `kParamCast`; Repeat (three), SpawnParallel, DynamicSelector and the two Idle waits are
   `kParamStructural`; the other twelve are `kParamDeny`.
6. `CombatBehaviorAcquireResource<2>` is the `NodeObject2<AcquireResource, BSFixedString, int>` class: both
   engine constructions of the node (AE 0x86537B "Action" and 0x8ADEEF "Movement") load vtable 0x18DA380
   (`lea rax,[rip+...]`), then write the name at +0x28 and `mov dword [rbx+0x30], 0x64`.

### Thread, controller and group fields

| Value | Derivation (1.6.1170 / 1.5.97 / 1.7.104) |
|---|---|
| `kThreadCurNode` 0x138, `kThreadPhase` 0x14C | Ascend, AE id 47484 0x85DD70 / SE 46229 0x7C69D0 / 0x872FA0: `mov rax,[rcx+0x138]; mov rax,[rax+0x10]; mov [rcx+0x138],rax; mov dword [rcx+0x14C],0`. The runner (AE id 47483 0x85DBA0 / SE 46228 0x7C6800 / 0x872DD0) tests `[rdi+0x14C]` against 2, 1 and 0 before calling slots 0x02, 0x04/0x05 and 0x03 of the node at `[rdi+0x138]`. |
| `kThreadResult` 0x148 | SetFailed, AE id 47496 0x85E0A0 / SE 46240 0x7C6D30 / 0x8732D0: `cmp dword [rcx+0x148],1; ja ret; mov [rcx+0x148],dl`. |
| `kThreadRanNode` 0x140, `kThreadWait` 0x150, `kThreadParent` 0x160 | Runner AE 0x85DCE3 `mov [rdi+0x140],rsi`; 0x85DC60 `test byte [rdi+0x150],3`; 0x85DD07 `mov rcx,[rdi+0x160]` then clears that thread's wait bit. |
| `TreeControl::master_controller` 0x158 | Runner AE 0x85DD2B `mov rcx,[rcx+0x158]` (the parent's owner, called with the parent thread); every leaf: `mov rax,[thread+0x158]; mov rcx,[rax+0x20]` = the CombatController (EquipSpell Enter AE 0x87829F; SearchCenter Enter AE 0x8B2A07 / SE 0x81BCE7 / 0x8C7F97). |
| Act/pop data-stack pair | Push helper AE id 33171 0x5572A0: `lea esi,[r8+3]; and esi,-4; add esi,[rcx+0xC]`. ForceFail act AE 0x8B9D90 / SE 0x8218A0 / 0x8CF370 (push 1, SetFailed, jmp Ascend), pop AE 0x8B9E50 / SE 0x821950 / 0x8CF430 `add dword [rdx+0xC],-4`. |
| Runner thread == TLS thread | Stepping loop AE id 47363 0x858B10: 0x858B56 `mov [rsi],rdx` (rsi = TLS block + 0x6A0), 0x858B60 `mov rcx,rbx` (rbx = rdx), call runner. |
| `ControllerMini::combatGroup` 0x00 | SearchCenter Enter AE 0x8B2A63 / SE 0x81BD5A / 0x8C7FF3 `mov rax,[rcx]` on the controller, then reads the group's search fields. |
| `ControllerMini::attackerHandle` 0x28 | The inlined attacker read in the same Enters: AE 0x8B2A27 / SE 0x81BD10 / 0x8C7FB7 `mov eax,[reg+0x28]` (else the cached pointer). |
| `ControllerMini::targetHandle` 0x2C | CombatController::SetTarget AE id 33235 0x559630 (0x5596AA `mov eax,[rdi+0x2C]`, the old target), SE id 32488 (0x4FE07D), 1.7.104 0x5614A0 (0x56151A). |
| `kGroupTargetToSearchFor` 0xFC | The group's search-target setter AE id 44767 0x809B20 `mov [rcx+0xFC],eax` (the actor's handle); SE id 43541 0x76FEB0 the same; 1.7.104 0x81EA00, the only hit of the AE byte signature (which carries the literal 0xFC). |
| `kGroupSearchLoc` 0x100, `kGroupSearchLocSpace` 0x110 | SearchCenter Enter reads `[group+0x100/0x104/0x108]` (three floats) then `[group+0x110]` and skips the centre when it is null: AE 0x8B2A66-0x8B2A8D / SE 0x81BD5D-0x81BD84 / 0x8C7FF6-0x8C801D. |
| `RTTI_CombatBehaviorTreeNode` | TypeDescriptor `.?AVCombatBehaviorTreeNode@@`: 1.6.1170 0x205E120 = id 394204; 1.5.97 0x1E37020 = id 686393; 1.7.104 0x2101980 = the MIT row for 394204. |
| Slot typedefs | `Act_t (node, thread)` slot 0x02 and `Update_t (node, thread)` slot 0x04: the runner calls `[vtbl+0x10]` / `[vtbl+0x20]` with rcx = node, rdx = thread and ignores the result. `SetFailed_t (thread, bool)`, `Ascend_t (thread)` as disassembled above. |

The VR column of every `REL::VariantID` is 0: there is no VR executable to derive from, and every consumer
refuses VR before resolving one.

## The 95 vtables

| # | Table | Name (ours) | RTTI type descriptor | 1.6.1170 vtable (AE id) | 1.5.97 vtable (SE id) | 1.7.104 vtable | MIT[AE id] = RTTI vtable |
|---|---|---|---|---|---|---|---|
| 0 | kLeaves | `CombatBehaviorAdvance` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorAdvance@@@@` | 0x18DC6C0 (212694) | 0x16960B0 (266096) | 0x195AC88 | yes |
| 1 | kLeaves | `CombatBehaviorAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorAttack@@@@` | 0x18E4048 (213789) | 0x169E5A0 (266747) | 0x1962610 | yes |
| 2 | kLeaves | `CombatBehaviorAttackFromCover` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorAttackFromCover@@@@` | 0x18E8C80 (214395) | 0x16A37A8 (267194) | 0x1967270 | yes |
| 3 | kLeaves | `CombatBehaviorAttackLow` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorAttackLow@@@@` | 0x18E2DA8 (213640) | 0x169D210 (266654) | 0x1961370 | yes |
| 4 | kLeaves | `CombatBehaviorBackoff` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorBackoff@@@@` | 0x18DCB90 (212785) | 0x1696580 (266103) | 0x195B158 | yes |
| 5 | kLeaves | `CombatBehaviorBash` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorBash@@@@` | 0x18DB618 (212595) | 0x1694DD8 (265986) | 0x1959BE0 | yes |
| 6 | kLeaves | `CombatBehaviorBlock` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorBlock@@@@` | 0x18DB6C8 (212608) | 0x1694E88 (265987) | 0x1959C90 | yes |
| 7 | kLeaves | `CombatBehaviorBlockAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorBlockAttack@@@@` | 0x18DB568 (212582) | 0x1694D28 (265985) | 0x1959B30 | yes |
| 8 | kLeaves | `CombatBehaviorCastConcentrationSpell` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCastConcentrationSpell@@@@` | 0x18E3868 (213735) | 0x169DD98 (266707) | 0x1961E30 | yes |
| 9 | kLeaves | `CombatBehaviorCastImmediateSpell` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCastImmediateSpell@@@@` | 0x18E3760 (213720) | 0x169DC90 (266705) | 0x1961D28 | yes |
| 10 | kLeaves | `CombatBehaviorCastShout` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCastShout@@@@` | 0x18E7E60 (214280) | 0x16A28E0 (267116) | 0x1966450 | yes |
| 11 | kLeaves | `CombatBehaviorChase` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorChase@@@@` | 0x18DF408 (213109) | 0x16991F0 (266336) | 0x195D9D0 | yes |
| 12 | kLeaves | `CombatBehaviorCheckUnreachableTarget` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCheckUnreachableTarget@@@@` | 0x18E5410 (213917) | 0x169F9C8 (266866) | 0x19639D8 | yes |
| 13 | kLeaves | `CombatBehaviorChildSelector_ConditionalChildSelector` | `.?AV?$CombatBehaviorTreeNodeObject@V?$CombatBehaviorChildSelector@VConditionalChildSelector@@@@@@` | 0x18DC560 (212668) | 0x1695F50 (266094) | 0x195AB28 | yes |
| 14 | kLeaves | `CombatBehaviorChildSelector_RandomValueChildSelector` | `.?AV?$CombatBehaviorTreeNodeObject@V?$CombatBehaviorChildSelector@VRandomValueChildSelector@@@@@@` | 0x18DC980 (212746) | 0x1696370 (266100) | 0x195AF48 | yes |
| 15 | kLeaves | `CombatBehaviorChildSelector_ValueChildSelector` | `.?AV?$CombatBehaviorTreeNodeObject@V?$CombatBehaviorChildSelector@VValueChildSelector@@@@@@` | 0x18DA640 (212424) | 0x1693DB0 (265919) | 0x1958C08 | yes |
| 16 | kLeaves | `CombatBehaviorCircle` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCircle@@@@` | 0x18DCAE0 (212772) | 0x16964D0 (266102) | 0x195B0A8 | yes |
| 17 | kLeaves | `CombatBehaviorCircleDistant` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorCircleDistant@@@@` | 0x18DC820 (212720) | 0x1696210 (266098) | 0x195ADE8 | yes |
| 18 | kLeaves | `CombatBehaviorDiveBomb` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorDiveBomb@@@@` | 0x18E25B8 (213551) | 0x169C930 (266624) | 0x1960B80 | yes |
| 19 | kLeaves | `CombatBehaviorDodgeThreat` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorDodgeThreat@@@@` | 0x18DAFD0 (212546) | 0x16947B8 (265954) | 0x1959598 | yes |
| 20 | kLeaves | `CombatBehaviorDrinkPotion` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorDrinkPotion@@@@` | 0x18E8F80 (214430) | 0x16A3B40 (267213) | 0x1967570 | yes |
| 21 | kLeaves | `CombatBehaviorDynamicConditionalNode` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorDynamicConditionalNode@@@@` | 0x18DA590 (212411) | 0x1693D00 (265918) | 0x1958B58 | yes |
| 22 | kLeaves | `CombatBehaviorEquipObject` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorEquipObject@@@@` | 0x18DA6F0 (212437) | 0x1693E60 (265920) | 0x1958CB8 | yes |
| 23 | kLeaves | `CombatBehaviorEquipRangedWeapon` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorEquipRangedWeapon@@@@` | 0x18DA7A0 (212450) | 0x1693F10 (265921) | 0x1958D68 | yes |
| 24 | kLeaves | `CombatBehaviorEquipShout` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorEquipShout@@@@` | 0x18DA958 (212478) | 0x16940C8 (265924) | 0x1958F20 | yes |
| 25 | kLeaves | `CombatBehaviorEquipSpell` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorEquipSpell@@@@` | 0x18DA850 (212463) | 0x1693FC0 (265922) | 0x1958E18 | yes |
| 26 | kLeaves | `CombatBehaviorExitWater` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorExitWater@@@@` | 0x18E5570 (213943) | 0x169FB28 (266868) | 0x1963B38 | yes |
| 27 | kLeaves | `CombatBehaviorFallback` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFallback@@@@` | 0x18DCA30 (212759) | 0x1696420 (266101) | 0x195AFF8 | yes |
| 28 | kLeaves | `CombatBehaviorFallbackSelector_NextChildSelector` | `.?AV?$CombatBehaviorTreeNodeObject@V?$CombatBehaviorFallbackSelector@VNextChildSelector@@@@@@` | 0x1873240 (205291) | 0x162FBD0 (256698) | 0x18ED320 | yes |
| 29 | kLeaves | `CombatBehaviorFallbackSelector_WeightedRandomChildSelector` | `.?AV?$CombatBehaviorTreeNodeObject@V?$CombatBehaviorFallbackSelector@VWeightedRandomChildSelector@@@@@@` | 0x18E23A8 (213512) | 0x169C720 (266621) | 0x1960970 | yes |
| 30 | kLeaves | `CombatBehaviorFallbackToRanged` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFallbackToRanged@@@@` | 0x18DC610 (212681) | 0x1696000 (266095) | 0x195ABD8 | yes |
| 31 | kLeaves | `CombatBehaviorFindAllyAttackLocation` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFindAllyAttackLocation@@@@` | 0x18DDDB0 (212925) | 0x16978C0 (266196) | 0x195C378 | yes |
| 32 | kLeaves | `CombatBehaviorFindAttackLocation` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFindAttackLocation@@@@` | 0x18DDD00 (212912) | 0x1697810 (266195) | 0x195C2C8 | yes |
| 33 | kLeaves | `CombatBehaviorFindCover` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFindCover@@@@` | 0x18E8A70 (214365) | 0x16A3598 (267190) | 0x1967060 | yes |
| 34 | kLeaves | `CombatBehaviorFindLateralAttackLocation` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFindLateralAttackLocation@@@@` | 0x18DDC50 (212899) | 0x1697760 (266194) | 0x195C218 | yes |
| 35 | kLeaves | `CombatBehaviorFindWeapon` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFindWeapon@@@@` | 0x18D99D0 (212262) | 0x1692F60 (265838) | 0x1957F98 | yes |
| 36 | kLeaves | `CombatBehaviorFlank` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFlank@@@@` | 0x18DF358 (213096) | 0x1699140 (266335) | 0x195D920 | yes |
| 37 | kLeaves | `CombatBehaviorFlankDistant` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFlankDistant@@@@` | 0x18DF4B8 (213122) | 0x16992A0 (266337) | 0x195DA80 | yes |
| 38 | kLeaves | `CombatBehaviorFlee` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFlee@@@@` | 0x18E0EA0 (213321) | 0x169AEB8 (266497) | 0x195F468 | yes |
| 39 | kLeaves | `CombatBehaviorFleeThroughDoor` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFleeThroughDoor@@@@` | 0x18E0D40 (213295) | 0x169AD58 (266495) | 0x195F308 | yes |
| 40 | kLeaves | `CombatBehaviorFleeToAlly` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFleeToAlly@@@@` | 0x18E0C90 (213282) | 0x169ACA8 (266494) | 0x195F258 | yes |
| 41 | kLeaves | `CombatBehaviorFleeToCover` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFleeToCover@@@@` | 0x18E0DF0 (213308) | 0x169AE08 (266496) | 0x195F3B8 | yes |
| 42 | kLeaves | `CombatBehaviorFlyingAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorFlyingAttack@@@@` | 0x18E2718 (213577) | 0x169CA90 (266626) | 0x1960CE0 | yes |
| 43 | kLeaves | `CombatBehaviorForceFail` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorForceFail@@@@` | 0x18E7928 (214228) | 0x16A22A0 (267085) | 0x1965F18 | yes |
| 44 | kLeaves | `CombatBehaviorForceSuccess` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorForceSuccess@@@@` | 0x18E13C8 (213405) | 0x169B3E0 (266506) | 0x195F990 | yes |
| 45 | kLeaves | `CombatBehaviorGroundAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorGroundAttack@@@@` | 0x18E2458 (213525) | 0x169C7D0 (266622) | 0x1960A20 | yes |
| 46 | kLeaves | `CombatBehaviorHide` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorHide@@@@` | 0x18E1160 (213364) | 0x169B178 (266502) | 0x195F728 | yes |
| 47 | kLeaves | `CombatBehaviorHover` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorHover@@@@` | 0x18E2508 (213538) | 0x169C880 (266623) | 0x1960AD0 | yes |
| 48 | kLeaves | `CombatBehaviorLand` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorLand@@@@` | 0x18E2980 (213609) | 0x169CCF8 (266631) | 0x1960F48 | yes |
| 49 | kLeaves | `CombatBehaviorMaintainOptimalRange` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorMaintainOptimalRange@@@@` | 0x18E5E58 (214013) | 0x16A0630 (266933) | 0x1964420 | yes |
| 50 | kLeaves | `CombatBehaviorOrbit` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorOrbit@@@@` | 0x18E22F8 (213499) | 0x169C670 (266620) | 0x19608C0 | yes |
| 51 | kLeaves | `CombatBehaviorOrbitDistant` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorOrbitDistant@@@@` | 0x18E2248 (213486) | 0x169C5C0 (266619) | 0x1960810 | yes |
| 52 | kLeaves | `CombatBehaviorParallel` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorParallel@@@@` | 0x18D9068 (212197) | 0x1692468 (265772) | 0x1957630 | yes |
| 53 | kLeaves | `CombatBehaviorPause` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorPause@@@@` | 0x18DAA08 (212491) | 0x1694178 (265925) | 0x1958FD0 | yes |
| 54 | kLeaves | `CombatBehaviorPerchAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorPerchAttack@@@@` | 0x18E2668 (213564) | 0x169C9E0 (266625) | 0x1960C30 | yes |
| 55 | kLeaves | `CombatBehaviorPrepareDualCast` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorPrepareDualCast@@@@` | 0x18E3658 (213705) | 0x169DB88 (266703) | 0x1961C20 | yes |
| 56 | kLeaves | `CombatBehaviorPursueTarget` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorPursueTarget@@@@` | 0x18E2E58 (213653) | 0x169D2C0 (266655) | 0x1961420 | yes |
| 57 | kLeaves | `CombatBehaviorRangedAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorRangedAttack@@@@` | 0x18732F0 (205304) | 0x162FC80 (256699) | 0x18ED3D0 | yes |
| 58 | kLeaves | `CombatBehaviorRepeat` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorRepeat@@@@` | 0x1873190 (205278) | 0x162FB20 (256697) | 0x18ED270 | yes |
| 59 | kLeaves | `CombatBehaviorReposition` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorReposition@@@@` | 0x18DC8D0 (212733) | 0x16962C0 (266099) | 0x195AE98 | yes |
| 60 | kLeaves | `CombatBehaviorReturnToCombatArea` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorReturnToCombatArea@@@@` | 0x18E54C0 (213930) | 0x169FA78 (266867) | 0x1963A88 | yes |
| 61 | kLeaves | `CombatBehaviorSearchInvestigateDoor` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorSearchInvestigateDoor@@@@` | 0x18E79D8 (214241) | 0x16A2350 (267086) | 0x1965FC8 | yes |
| 62 | kLeaves | `CombatBehaviorSequence` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorSequence@@@@` | 0x18D9920 (212249) | 0x1692EB0 (265837) | 0x1957EE8 | yes |
| 63 | kLeaves | `CombatBehaviorSpecialAttack` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorSpecialAttack@@@@` | 0x18E3F98 (213776) | 0x169E4F0 (266746) | 0x1962560 | yes |
| 64 | kLeaves | `CombatBehaviorStalk` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorStalk@@@@` | 0x18DF2A8 (213083) | 0x1699090 (266334) | 0x195D870 | yes |
| 65 | kLeaves | `CombatBehaviorStrafe` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorStrafe@@@@` | 0x18E5F08 (214026) | 0x16A06E0 (266934) | 0x19644D0 | yes |
| 66 | kLeaves | `CombatBehaviorSurround` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorSurround@@@@` | 0x18DC770 (212707) | 0x1696160 (266097) | 0x195AD38 | yes |
| 67 | kLeaves | `CombatBehaviorTakeoff` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorTakeoff@@@@` | 0x18E20E8 (213460) | 0x169C460 (266617) | 0x19606B0 | yes |
| 68 | kLeaves | `CombatBehaviorTrackTarget` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorTrackTarget@@@@` | 0x18E1000 (213338) | 0x169B018 (266500) | 0x195F5C8 | yes |
| 69 | kLeaves | `CombatBehaviorWaitBehindCover` | `.?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorWaitBehindCover@@@@` | 0x18E8BD0 (214382) | 0x16A36F8 (267193) | 0x19671C0 | yes |
| 70 | kSearchLeaves | `CombatBehaviorSearch` | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorSearch@@W4COMBAT_SEARCH_PRIORITY@@@@` | 0x18E7718 (214189) | 0x16A2090 (267082) | 0x1965D08 | yes |
| 71 | kSearchLeaves | `CombatBehaviorSearchCenter` | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorSearchCenter@@W4COMBAT_SEARCH_PRIORITY@@@@` | 0x18E7878 (214215) | 0x16A21F0 (267084) | 0x1965E68 | yes |
| 72 | kSearchLeaves | `CombatBehaviorSearchLocation` | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorSearchLocation@@W4COMBAT_SEARCH_PRIORITY@@@@` | 0x18E7668 (214176) | 0x16A1FE0 (267081) | 0x1965C58 | yes |
| 73 | kSearchLeaves | `CombatBehaviorSearchWander` | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorSearchWander@@W4COMBAT_SEARCH_PRIORITY@@@@` | 0x18E77C8 (214202) | 0x16A2140 (267083) | 0x1965DB8 | yes |
| 74 | kParamLeaves | `CombatBehaviorAcquireItem<AcquireWeapon>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorAcquireItem@@V?$CombatBehaviorExpression@V?$CombatBehaviorMemberFunc@VCombatBehaviorContextAcquireWeapon@@P81@EBAAEBVCombatAcquireItem@@XZ@@@@@@` | 0x18D9C90 (212314) | 0x1693220 (265842) | 0x1958258 | yes |
| 75 | kParamLeaves | `CombatBehaviorAcquireResource<2>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject2@VCombatBehaviorAcquireResource@@VBSFixedString@@H@@` | 0x18DA380 (212372) | 0x1693AF0 (265915) | 0x1958948 | yes |
| 76 | kParamLeaves | `CombatBehaviorAcquireResource<3>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject3@VCombatBehaviorAcquireResource@@VBSFixedString@@HW4FLAG@1@@@` | 0x18D9A80 (212275) | 0x1693010 (265839) | 0x1958048 | yes |
| 77 | kParamLeaves | `CombatBehaviorCastConcentrationSpell<bool>` (kParamCast) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorCastConcentrationSpell@@_N@@` | 0x18E3810 (213733) | 0x169DD40 (266706) | 0x1961DD8 | yes |
| 78 | kParamLeaves | `CombatBehaviorCastImmediateSpell<bool>` (kParamCast) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorCastImmediateSpell@@_N@@` | 0x18E3708 (213718) | 0x169DC38 (266704) | 0x1961CD0 | yes |
| 79 | kParamLeaves | `CombatBehaviorDisableAim<PRIORITY>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorDisableAim@@W4PRIORITY@CombatAimController@@@@` | 0x18D9B30 (212288) | 0x16930C0 (265840) | 0x19580F8 | yes |
| 80 | kParamLeaves | `CombatBehaviorDynamicSelector_ConditionalChildSelector<float>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject1@V?$CombatBehaviorDynamicSelector@VConditionalChildSelector@@@@M@@` | 0x18E2198 (213473) | 0x169C510 (266618) | 0x1960760 | yes |
| 81 | kParamLeaves | `CombatBehaviorEnableBlackboardFlag<CombatBlackboardFlag*>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorEnableBlackboardFlag@@PEAVCombatBlackboardFlag@@@@` | 0x18E1318 (213392) | 0x169B330 (266505) | 0x195F8E0 | yes |
| 82 | kParamLeaves | `CombatBehaviorFaceAngle<float,bool>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject2@VCombatBehaviorFaceAngle@@M_N@@` | 0x18E10B0 (213351) | 0x169B0C8 (266501) | 0x195F678 | yes |
| 83 | kParamLeaves | `CombatBehaviorFlee<float>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorFlee@@M@@` | 0x18E0EF8 (213323) | 0x169AF10 (266498) | 0x195F4C0 | yes |
| 84 | kParamLeaves | `CombatBehaviorFollowPath<AcquireWeapon>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorFollowPath@@V?$CombatBehaviorExpression@V?$CombatBehaviorMemberFunc@VCombatBehaviorContextAcquireWeapon@@P81@EAA?AV?$NiPointer@VCombatPath@@@@XZ@@@@@@` | 0x18D9BE0 (212301) | 0x1693170 (265841) | 0x19581A8 | yes |
| 85 | kParamLeaves | `CombatBehaviorIdle<GameSetting>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorIdle@@V?$CombatBehaviorExpression@V?$CombatBehaviorFunc1@P6AMPEAV?$SettingT@VGameSettingCollection@@@@@ZPEAV1@@@@@@@` | 0x18E7560 (214161) | 0x16A1ED8 (267079) | 0x1965B50 | yes |
| 86 | kParamLeaves | `CombatBehaviorIdle<float>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorIdle@@M@@` | 0x18733B0 (205318) | 0x162FD40 (256701) | 0x18ED490 | yes |
| 87 | kParamLeaves | `CombatBehaviorLand<float>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorLand@@M@@` | 0x18E2878 (213594) | 0x169CBF0 (266629) | 0x1960E40 | yes |
| 88 | kParamLeaves | `CombatBehaviorLandNearby<float>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorLandNearby@@M@@` | 0x18E2928 (213607) | 0x169CCA0 (266630) | 0x1960EF0 | yes |
| 89 | kParamLeaves | `CombatBehaviorRepeat<FLAGS>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorRepeat@@W4FLAGS@1@@@` | 0x18E2770 (213579) | 0x169CAE8 (266627) | 0x1960D38 | yes |
| 90 | kParamLeaves | `CombatBehaviorRepeat<float,FLAGS>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject2@VCombatBehaviorRepeat@@MW4FLAGS@1@@@` | 0x18E8AC8 (214367) | 0x16A35F0 (267191) | 0x19670B8 | yes |
| 91 | kParamLeaves | `CombatBehaviorRepeat<float>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorRepeat@@M@@` | 0x18D97C0 (212223) | 0x1692D50 (265835) | 0x1957D88 | yes |
| 92 | kParamLeaves | `CombatBehaviorSetBlackboardData<AITimeStamp>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject2@VCombatBehaviorSetBlackboardData@@PEAV?$CombatBlackboardKey@VAITimeStamp@@@@V?$CombatBehaviorExpression@V?$CombatBehaviorFunc@P6AMXZ@@@@@@` | 0x18DDE60 (212938) | 0x1697970 (266197) | 0x195C428 | yes |
| 93 | kParamLeaves | `CombatBehaviorSpawnParallel<uint,FLAGS>` (kParamStructural) | `.?AV?$CombatBehaviorTreeNodeObject2@VCombatBehaviorSpawnParallel@@IW4FLAGS@CombatBehaviorParallel@@@@` | 0x18DA430 (212385) | 0x1693BA0 (265916) | 0x19589F8 | yes |
| 94 | kParamLeaves | `CombatBehaviorWatchTarget<float>` (kParamDeny) | `.?AV?$CombatBehaviorTreeNodeObject1@VCombatBehaviorWatchTarget@@M@@` | 0x18DF568 (213135) | 0x1699350 (266338) | 0x195DB30 | yes |
