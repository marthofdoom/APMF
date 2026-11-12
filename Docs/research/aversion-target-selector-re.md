# The standard target selector's scorer, and where ch.32 aversion sits

Read 2026-11-11 on the unpacked 1.6.1170 and 1.5.97 executables and the plaintext 1.7.104 one, for MFO class plan card H2
("less likely to be targeted"). Earlier reads this builds on: MFO `Docs/TAUNT-DESIGN.md` section 1 and MFO
`Docs/research/2026-11/class-m0-re.md` section 2. Everything below was re-read for this card. PROVEN = read in the
disassembly. INFERRED = a meaning assigned from context. Nothing here has been observed in a running game yet.

## 1. The question: is there a vanilla lever?

No. marth asked for this check first. The standard selector's score (section 3) has these inputs and no others: a
two second per-pair combat memory, the current target, a weapon-class comparison, distance, an attacker count whose
global is zero, reach and combat-area tests, and two "candidate is down" tests. No actor value is read. No perk entry
point is called. No keyword is tested. The one vanilla input that removes a candidate is DETECTION (admission needs
`detectLevel > 0`), and that hides the actor from everything that group does, which is a different facet (stealth).

## 2. Functions

| Function | 1.6.1170 (AE id, RVA) | 1.5.97 (SE id, RVA) | 1.7.104 (RVA) |
|---|---|---|---|
| `CombatTargetSelectorStandard::SelectTarget`, vtable slot 0x06 | 47191, 0x84CFE0 | 45918, 0x7B5E50 | 0x862040 |
| the scorer, `FindBest(selector*, ActorHandle* out)` | 47195, 0x84D280 | 45922, 0x7B5FF0 | 0x8622E0 |
| `IsValidTarget(selector*, Actor* attacker, Actor* candidate, zone)` | 47196, 0x84D940 | inlined in the scorer | 0x8629A0 |
| `LookupActorByHandle(const ActorHandle*, NiPointer<Actor>* out)` | 17201, 0x261E40 | 16828, 0x2130F0 | 0x2676C0 |

None of the four AE ids is in the fork's MIT id table for 1.7.104, so Harbinger places them there by raw RVA. The
1.6.1170 byte signature of each hits the 1.7.104 RVA alone (generator rows `Aversion.*`). The 1.6.1170 and 1.7.104
bodies of slot 0x06 and of the scorer are instruction-identical apart from RIP displacements. The lookup function is
the Actor instantiation the selector code calls. It is NOT CommonLib's `Offset::LookupReferenceByHandle` (AE id 12332).

Callers (rel32 and absolute-pointer scans of each image): the scorer has ONE caller per build, slot 0x06 at +0x68.
`IsValidTarget` has one caller, the scorer at +0x2AC.

## 3. What the code does (PROVEN)

**Slot 0x06.** Resolves the cached handle (selector +0x18). If the group still holds it and
`now - [+0x28] <= [+0x2C]`, it returns the cache. Otherwise `lea rdx,[rsp+0x50]; mov rcx,rdi; call FindBest`
(+0x68 on all three builds), then `mov eax,[rsp+0x50]; cmp eax,[rdi+0x18]`: a changed pick is stored with the
interval `fCombatSelectTargetSwitchUpdateTime` (3 s), an unchanged one with `fCombatSelectTargetUpdateTime` (1 s).

**The scorer.** `rcx` = selector, `rdx` = out. It reads the controller (selector +0x10), the attacker handle
(controller +0x28) and the current target handle (controller +0x2C), sets `*out` to the null handle, then walks
`combatGroup->targets` (controller +0x00 -> group; array data at group +0x08, count at +0x18, stride 0xA8).

- **Admission.** An entry is admitted when `detectLevel` (+0x04) > 0, `stealthPoints` (+0x08) <= 0 and `kTargetLost`
  (+0xA6 bit 1) is clear, or when it is the current target and carries `kTargetKnown` (+0xA6 bit 0).
- **Not admitted.** If `*out` is still null, the entry goes to a local FALLBACK list keyed by the age of
  `lastKnownTimeStamp` (+0x90).
- **Admitted.** The handle is resolved (`LookupActorByHandle`; AE +0x28F, SE +0x2C9). Then the validity gate:
  on 1.6.1170 / 1.7.104 `mov r8,rbx; mov rdx,r14; mov rcx,[rbp+0x4C0]; call IsValidTarget` at +0x2AC and
  `test al,al; je <skip>`. On 1.5.97 the gate is inline: `mov rbx,[rsp+0x58]; test rbx,rbx; je <skip>`, the
  `bEncounterZoneTargetRestrict` test, and the attacker flag (+0xE0 bit 26) with candidate vfunc 0x5E.
  `IsValidTarget` returns false for a null candidate. The 1.5.97 skip label handles a null pointer.
- **Score** (only when the group has more than one target; a single target scores 0): +1000 / +100 from the
  per-pair combat memory when it is under 2 s old, +100 for the current target, +100 when the candidate and the
  attacker are in the same weapon-class state (AE 0x853D80: the equipped object is a weapon, form type 0x29, of
  animation type 0..6; meaning INFERRED), up to +1000 for proximity (2048 units), the attacker-count global (zero in
  the file), -2000 when the reach test fails, -2500 with no path across cell spaces, -500 for vfunc 0x99
  (`IsDead(false)`: life state dead, dying, essential down or recycle, read on AE 0x674ED0), -500 for AE 0x67EFE0 =
  IS FLEEING (read 2026-11-11 in the H2-1 fix round: with a live, active controller `CombatState::isFleeing`
  `[[actor+0x160]+0x08]+0x04`, else `MiddleHighProcessData::isFleeing` `+0x32B`; the first read's "down" was a wrong
  inference). The best score writes `*out`.
- **Fallback.** After the walk: `cmp dword [out],0; jne <done>` (AE / 1.7.104 +0x4C4, SE +0x53B). Only when nothing
  was scored is the fallback list sorted and walked: each handle is resolved (`call LookupActorByHandle`,
  AE / 1.7.104 +0x52E, SE +0x5EF) and the first one that passes the inline validity test is the answer.

**Side effects.** The scorer writes its own stack, `*out` and reference counts. It does not write the group, the
selector or an actor. It can be called twice in a row with the same result.

## 4. The seats ch.32 uses

| Seat | 1.6.1170 | 1.5.97 | 1.7.104 | What the thunk does |
|---|---|---|---|---|
| C0 | slot 0x06 +0x68 (0x84D048) | +0x68 (0x7B5EB8) | +0x68 (0x8620A8) | runs the scorer; for an averted claimant runs it again with the claimant excluded |
| X | scorer +0x2AC (0x84D52C), the `IsValidTarget` call | scorer +0x2C9 (0x7B62B9), the scored path's lookup call | +0x2AC (0x86258C) | AE / 1.7.104: answers false for the excluded candidate. SE: asks the lookup for the null handle |
| F | scorer +0x52E (0x84D7AE) | scorer +0x5EF (0x7B65DF) | +0x52E (0x86280E) | records that the pass consulted the fallback list |

Byte windows the code re-reads before patching (lead, the E8, tail):

- C0, all three: `48 8D 54 24 50 48 8B CF` | E8 | `8B 44 24 50 3B 47 18`.
- X, 1.6.1170 / 1.7.104: `4C 8B C3 49 8B D6 48 8B 8D C0 04 00 00` | E8 | `84 C0 0F 84`.
- X, 1.5.97: `48 8D 54 24 58 48 8D 4C 24 38` | E8 | `90 48 8B 5C 24 58 48 85 DB 0F 84`.
- F, 1.6.1170 / 1.7.104: `48 8D 54 24 70 48 8D 4C 24 40` | E8 | `90 48 8B 5C 24 70 48 85 DB 74`.
- F, 1.5.97: `48 8D 54 24 78 48 8D 4C 24 40` | E8 | `90 48 8B 5C 24 78 48 85 DB 74`.
- The fallback gate: `83 3E 00 0F 85` (1.6.1170 / 1.7.104 +0x4C4), `83 3B 00 0F 85` (1.5.97 +0x53B).

Why 1.5.97 differs (principle 11): there is no `IsValidTarget` function to seat there, and scorer +0x2AC is the
middle of another instruction. The lookup call is the one per-candidate call on the scored path before scoring. For
the null handle the lookup function clears `*out` and returns false on both builds (`test esi,esi; je` to the
clear-and-return tail), and the caller pre-zeroes the slot.

## 5. Who else writes a foe's target

The Fixed selector (slot 0x06 AE 0x84DAF0 / SE 0x7B6920) returns the handle it was built with. A ch.20 pin replaces
slot 0x06's answer after the original ran. `CombatController::SetTarget` (AE 0x559630 / SE 0x4FDFE0) is called by
`UpdateTarget` and by one clear. Another DLL can write `currentCombatTarget` in a later `UpdateCombat` hook. A script
`StartCombat` names the first target of a new fight. The selector's cache holds a pick for 1 to 3 s.

## 6. Not observed yet

That slot 0x06 reaches the scorer in play is read from the code (its only caller), not seen in a log. The first
field run reads `[ch.32] aversion H` and the per-pair lines. The meaning of the same-state test is inferred and it is
not used by ch.32.

## 7. The penalty tests ch.32 calls (H2-1 fix round, 2026-11-11)

The scorer penalises and does not skip, so the second pass's best can be unreachable, unpathable, dead or fleeing.
ch.32 asks the scorer's own tests about that answer by calling the same functions (`core/Aversion.cpp` `UnfitReason`).
Read on the three executables; 1.6.1170 and 1.7.104 are instruction-identical at these sites apart from rel32 / RIP
displacements.

| what | signature | 1.6.1170 | 1.5.97 | 1.7.104 | site in FindBest (AE, 1.7.104 / SE) |
|---|---|---|---|---|---|
| RefLocation | `BGSWorldLocation*(TESObjectREFR* rcx, BGSWorldLocation* out rdx)`: pos +0x00, worldspace-or-cell +0x10 | id 19784 0x2EA5E0 | id 19357 0x2964A0 | 0x2F0B70 | E8 +0x35E / +0x3C9 |
| radius | `[[controller+0x10]+0x1B8]` (the inventory record's float) | load at +0x12A | load at +0x15C | +0x12A | |
| limit | `comiss radius,[rip const]`, the constant reads 1024.0 on all three (AE RVA 0x1FFEB68, SE 0x1DDB908, 1.7.104 0x20A1F48); `jb` = ask the area | +0x3CD | +0x443 | +0x3CD | |
| InCombatArea (reach, -2000) | `bool(CombatController* rcx, const BGSWorldLocation* rdx, float radius xmm2)`: currentArea (+0x98 AE, +0x90 SE; null = true) vfunc +0x28 | id 33261 0x55B310 | id 32512 0x4FFA60 | 0x563180 | E8 +0x3EF / +0x465 |
| CrossSpaceReach (path, -2500) | `bool(Actor* attacker rcx, const BGSWorldLocation* rdx)`, different space only | id 47307 0x853F10 | id 46043 0x7BCCD0 | 0x868F80 | E8 +0x40C / +0x482 |
| IsDead (-500) | candidate vfunc `+0x4C8` (slot 0x99) with edx = 0 | window +0x41D | window +0x493 | +0x41D | |
| IsFleeing (-500) | `bool(Actor* rcx)` | id 37577 0x67EFE0 | id 36575 0x5ECB70 | 0x691AA0 | E8 +0x43A / +0x4B0 |

Order in the scorer: when `radius < 1024` or the candidate is in a different space, the area test; then, different
space only, the path test; then dead; then fleeing. None of the four AE ids is in the MIT 1.7.104 table (raw RVAs;
the 1.6.1170 signatures hit those RVAs alone). NOT OBSERVED IN PLAY: which of the four fires in a real fight (the
`kept-unfit` total and its per-pair line say so).
