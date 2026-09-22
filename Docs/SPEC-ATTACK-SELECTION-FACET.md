# SPEC — COMBAT-BEHAVIOUR facet: NPC ATTACK SELECTION (ch.18, ABI v10)

**Status: DESIGN, not approved, nothing dispatched.** Fable design pass 2026-09-21 (13:28–18:40 PDT),
read-only. marth reads this before any author starts. Evidence log (step by step): the session
scratchpad `agentlogs/design-combat-behaviour-facet.md` (a935cdde…), appended below as an appendix.
Tooling: `Linux-Native-Tools/tools/steamstub-rtti/` (unpacker, Address Library decoder, RTTI walker,
SCAR AE disassembly).

marth's constraints (2026-09-21): the additive gap-filler is CANCELLED ("adding more will do nothing");
NEVER remove moves from the pool (other mods add moves such as slide tackles); composition, not
substitution; MFO declares, APMF enforces; deny-complete for the DECLARED part only; **functional in ALL
lists** (vanilla ATKD/CSTY, MCO-DXP, BFCO, SCAR 2.0, Precision-only, DAR-replacer-only) — a hard gate,
not a ranking.

## TWO BLOCKERS first

1. **No unpacked binary exists on this machine.** `binaries/{1.5.97,1.6.1170}/SkyrimSE.exe` are still
   SteamStub-packed (.text entropy 8.00); the 2026-09-15 pass's unpacked copies lived in a scratchpad that
   is gone; Steamless needs Wine Mono (absent). An unpacker was written (signature 0xC0DEC0DF confirmed on
   both) but the agent's auto-mode refused to run it. `.rdata` (vtables/RTTI/strings) IS plaintext, so every
   vtable/slot/id claim below is verified; every INSTRUCTION-level claim is marked NEEDS-UNPACK. Brief A's
   step 0 runs that unpacker (or marth unpacks by hand).
2. **Correction to the earlier Fable claim (Tuxborn Skyrim.esm parsed):** DefaultRace (0x19) really has only
   8 ATKE entries with no Left/Dual — but that list governs only DefaultRace actors. **ImperialRace
   (0x13744) and NordRace (0x13746) carry 27 entries each incl. `attackStartLeftHand` 1.0,
   `attackStartDualWield` 1.0, `attackPowerStartDualWield` 1.0, every `*LeftHand` power at 1.0,
   `attackPowerStartRight` 1.0, InPlace .2 / Forward .5 / Backward .1 / Left .2, bashStart .5,
   bashPowerStart .33.** Vanilla playable-race dual-wield NPCs DO have DW/left-hand moves in the pool; the
   "vanilla DW NPC = right swings only" inference was wrong. Cicero's single-swing on the Deck is the
   `ER Dual Wield Sword (MCO)` DAR override collapsing the BEHAVIOUR side, not ATKD.

**Why a CSTY swap is not a shortcut:** a CSTY holds WEIGHTS — CSGD equipment-score mults, CSME
attack-chance mults, CSCR/CSFL — and the DATA flag word; the MOVE LIST is the race's ATKD/ATKE.
csThalmorMeleeDual DATA=0x5 (Dueling|AllowDualWielding), csMercerFreyMelee 0x6, csHumanMelee_AllD 0x1;
MFO_MeleeStyle already writes 1|4=5 (MFO_GenerateESP.py:1111). Swapping styles changes chance mults
(csMercer CSME 3.4/3.4/… vs MFO 1.0s) and, via CSGD, re-biases the equip scores the equip authority now
governs; CombatStyle.cpp:223-226 documents the engine re-derives the style mid-combat. It adds no move and
cannot express "power <= 30%".

## 1. WHERE THE NPC ATTACK PICK IS MADE

**Vanilla chain** (live-fork names are REFERENCE labels; ids from the Address Library; vtables/RTTI from
the images): `CombatBehaviorAttack` leaf act() [AE 49199 / SE 48171] → melee context
`CombatBehaviorTreeCreateContextNode2<CombatBehaviorContextMelee, MemberFunc<EquipContext::GetItem>,
ATTACK_TYPE{WeaponRight,Shield,WeaponLeft}>` (exact RTTI name in both images; vt AE 0x18e3ee8 id 213763
act 49198/pop 49201; SE vt 0x169e440 id 266745 act 48170/pop 48173) → GatherAttackData [SE 48145 0x80d1d0]
builds `combatattackdatas` from the RACE's BGSAttackData list for equipped item+hand → CheckAttack [SE
48140 0x80c6f0] rolls `AttackData::attackChance` (@+4, pinned B/BGSAttackData.h:27) × CSTY CSME mults per
entry → **StartAttack [SE 48139 0x80c020 = AE 49170 0x8a28b0]** calls GetAttackAngle (+0x3F1 SE / +0x493
AE) then **PerformAttackAction (+0x4D7 SE / +0x435 AE) = CombatAnimation::Execute [SE 43239 0x75ff10 / AE
44440 0x7f9470 — identical to APMF's T4 crash address module+0x7F9470]** → `TESActionData::Process` (slot
5; devirtualised for the AI's by-value member, T4 measured it) → IDLE resolve →
**`NotifyAnimationGraph(ENAM)` = IAnimationGraphManagerHolder vfunc 01** (pinned
I/IAnimationGraphManagerHolder.h:25; impl AE 38048 0x6a35f0 / SE 37020 0x60f240; reached from
`VTABLE_Character[3]` AE 207892 vt 0x18a5ee0 / SE 261400 vt 0x165e3c8, COL offset 0x38 = the holder base
per TESObjectREFR.h:111). "Power vs normal vs bash" = WHICH BGSAttackData entry won the roll (flags 0x4
power / 0x2 bash), not a separate node. AE ids for Gather/Check/Range/Finished: NOT FOUND (SE-only in the
live fork; .text encrypted).

**SCAR** (SCAR.dll AE Support, disassembled): loads the id pair 48139/49170, reads each site's rel32
(saves the original), then rewrites GetAttackAngle and PerformAttackAction sites. Its PerformAttackAction
replacement (0x1800b5850): actor = `TESActionData->source` (+8); if the actor has NO SCAR annotation cache →
jumps to the ORIGINAL (vanilla pick); else distance/angle/condition check, sorts `SCAR_ActionData` by
chance, plays the ADXP_NPCNormalAttack / ADXP_NPCPowerAttack IDLE (ENAM = vanilla `attackStart` /
`attackPowerStartInPlace`) via PlayIdle and returns 1 WITHOUT the original; no match → returns 0 without
the original. Third hook = vtable write on `VTABLE_Character[2]` slot 1
(BSTEventSink<BSAnimationGraphEvent>::ProcessEvent, ids 261399/207890) = Hook_AttackCombo
(`SCAR_ComboStart`). **Valhalla Combat** (also on the Deck list) patches the SAME PerformAttackAction site
(`hook:OnAttackAction`). **BFCO.dll** hooks only `PlayerControls` (`BFCO::ProcessMovement`); NPC pick
untouched, the behaviour graph remaps vanilla events to BFCO_* clips. Precision = AI weapon reach;
DAR/OAR/Vanargand/Nemesis/Pandora = clip/graph level; TDM = sink only.

⇒ **Every framework's NPC attack ends in `NotifyAnimationGraph` with a vanilla ATKE string.** That is the
framework-agnostic fact the design rests on.

**Seat ranking (framework-agnostic is pass/fail):**

| Seat | Sees pick | Agnostic | Legal | ABI | Verdict |
|---|---|---|---|---|---|
| S1 `NotifyAnimationGraph` vfunc 01 on `VTABLE_Character[3]` (never PlayerCharacter) | post-pick, pre-graph, exact class in the string | vanilla+SCAR+Valhalla+MFO drive+parkour | #17 vtable, chain-safe (SCAR's vtable hook is a different sub-vtable/slot) | `bool(holder*, const BSFixedString&)`, no sret; verify impl prologue both runtimes | **PASS, gated on rung-0 parity** (H1) |
| S2 ch.7 leaf act/pop (ActionGate, field-proven pair) on Bash / BlockAttack / SpecialAttack / AttackFromCover / AttackLow | pre-commit | yes (SCAR runs INSIDE StartAttack under the Attack leaf) | #17 | none new | **PASS as whole-class deny**; cannot split normal/power (both under one leaf) |
| S3 #17a call-site at 48139/49170+0x4D7/+0x435 | earliest | **NO** — SCAR and Valhalla already patch those bytes; #17a(2) refuses the whole seat on any such list; also violates #17a(1) | — | — | **DISQUALIFIED**; callee-entry variant breaks their chains = the T4 CTD |
| S4 `VTABLE_TESActionData` slot 5 (AE 188603 / SE 232777) | — | — | vtable | — | **FAIL**: devirtualised on the AI path; `CombatAnimation` has NO RTTI in either image |
| S5 BSAnimationGraphEvent sink | post-graph | yes | vtable/sink | — | observe only (= MFO's [atk-obs]) |

**DECISION: seat SET = S1 (share governance + veto + caller attribution) + S2 (whole-class pre-commit
deny).** Framework detection (GetModuleHandleExA on SCAR.dll / BFCO.dll / Precision.dll /
valhallaCombat.dll / OpenAnimationReplacer.dll / DynamicAnimationReplacer.dll / PayloadInterpreter.dll,
EquipSink's pattern) feeds ONLY the `[atk-seat] frameworks=` line and per-call caller attribution
(Engine(id)/SCAR.dll/MFO.dll/FollowerParkour.dll via the Offset2ID + module classifier EquipSink already
has). The verdict logic is identical on every list because the event vocabulary is.

## 2. ABI v10 (append-only on v9)

`kIntent_AttackSelection = 18` (ch.18): standing claim, ended by Release, NON-exclusive with
kIntent_CombatAction (an offense deny still wins).

```cpp
struct APMF_AttackIntent {           // BYTE-SHARED, append-only
  std::uint32_t governed;            // AttackClass bits the seat may touch AT ALL. 0 = untouched.
  std::uint32_t denied;              // ⊆ governed: vetoed at S1; Bash/Special also ForceFail-paired at S2 (pre-commit)
  float         powerShareMin;       // -1 undeclared; else 0..1 over the window (Normal+Power starts only)
  float         powerShareMax;       // -1 undeclared
  std::uint32_t window;              // sliding window of the actor's own starts; 0 -> 8
  std::uint32_t comboContinue;       // 0 undeclared / 1 allow / 2 deny (a *Start from a framework caller while GetAttackState!=kNone)
  std::uint32_t flags;               // kAtkIntent_ObserveOnly = 1
  std::uint32_t reserved[4];
};
enum AttackClass { kAtk_Normal=1 (attackStart*, AttackStartH2H*), kAtk_Power=2 (attackPowerStart*),
                   kAtk_Bash=4 (bashStart), kAtk_BashPower=8 (bashPowerStart),
                   kAtk_Special=16 (SpecialAttack/AttackFromCover leaves), kAtk_Combo=32 };
APMF_API_v10 : v9 { void (*SetAttackIntent)(Handle, const APMF_AttackIntent*); bool (*IsAttackSelectionEnforced)(void); }
```

Class table = exact-match set built at kDataLoaded from every loaded RACE's ATKE strings (mod-extended),
prefix rule only for logging; an unknown string is forwarded untouched whoever sent it — that is the
"slide tackle stays in the pool" guarantee. Verdict order per call: player/unclaimed → forward unlogged;
not a `*Start` string → forward; class not in `governed` → forward; in `denied` → veto; share: veto a
Power start when the window share would exceed Max, veto a Normal start while below Min, **max 4
consecutive vetoes then let-through + loud `floor-miss`/`ceiling-miss` line** (a floor the pool cannot
supply must fail visibly, never mask); ObserveOnly (INI `[AttackSelection] bAttackObserveOnly=1` default,
or the flag) → `would-veto` + forward. **NEUTRAL on veto = return false without forwarding; nothing is
fabricated and the string is never re-pointed** — the engine re-rolls on its own next tree tick.
`SetAttackIntent(handle,nullptr)` or all-zero = untouched again. Repoint = re-declare; the intent rides
the same snapshot generation as the claim (EquipScope precedent).

## 3. Probe criteria (observe-only first; MFO's [atk-obs] is on MFO main, Diagnostics.cpp line format)

Rung 0, both Deck profiles, seat installed observe-only: per fight the seat prints
`[apmf][atk-seat] {fid:08X} fight-close seen normal=N power=P bash=B other=O callers Engine(<id>)=x
SCAR.dll=y MFO.dll=z FollowerParkour.dll=w threads=<set>` and it MUST match the same actor's `[atk-obs]`
line exactly: N == attackStart+attackStartLeftHand+attackStartDualWield+AttackStartH2H*, P == Σ
attackPowerStart*, B == bashStart+bashPowerStart, every fight, both lists. ANY deficit = a path reaching
the graph without the vtable (devirtualised) ⇒ the seat fails the gate, STOP (principle 5). Rung 1: a
synthetic declaration `powerShareMax=0.3` in observe mode prints `would-veto` counts and the measured
share. Rung 2 (ONE follower, enforcing): share lands in [Min,Max] ± 1/window; idle-in-reach count/longest
NOT above the rung-0 baseline (stall detector for H2); zero `[ch.7] paired-pop protocol ANOMALY`.

## 4. Per-runtime ids (AE 1.6.1170 / SE 1.5.97; RVA in parens)

VTABLE_Character[3] holder sub-vtable: 207892 (0x18a5ee0) / 261400 (0x165e3c8); slot 1 impl
NotifyAnimationGraph 38048 (0x6a35f0) / 37020 (0x60f240). VTABLE_Actor[3]: 207517 / 260541 (same impl).
Attack leaf act/pop/update: 49199/49202/49213 / 48171/48174/48188. SpecialAttack 49200/49203/49214 /
48172/48175/48189. Bash 47861/47865/47881 / 46660/46664/46685. BlockAttack 47863/47867/47883 /
46662/46666/46687. Melee context node2 act/pop 49198/49201 / 48170/48173. Block context node2 47860/47864
/ 46659/46663. StartAttack 49170 (0x8a28b0) / 48139 (0x80c020). CombatAnimation::Execute 44440 (0x7f9470)
/ 43239 (0x75ff10). VTABLE_TESActionData 188603 (0x178c478, slot5 41557) / 232777 (0x1548198, slot5
40551). VTABLE_BGSAttackData 200768 / 252900. SE-only: GatherAttackData 48145, CheckAttack 48140,
CheckAttackRange 48141, FinishedAttack 48142, CalculateAnimationData 48146 — AE NOT FOUND. NEEDS-UNPACK:
E8 bytes at 0x8a2ce5/0x80c4f7, the roll site, and whether Process/PlayIdle call the holder slot VIRTUALLY
(this decides S1).

## Deny holes (adversarial, for the reviewer)

H1 devirtualised NotifyAnimationGraph on the AI/idle path → rung 0 parity (fatal if it fails). H2 veto
AFTER StartAttack ran: ContextMelee may already hold attack-started state (timer, aim controller) →
possible stall; measured by rung 2; fallback = move the veto to S2 one-shot ForceFail (balanced). H3
thread: S1 runs on the combat thread (AI), the anim-event thread (SCAR combos) and main (MFO's Post'd
drive) → RCU snapshot + atomics only, no locks, no forms-map. H4 floor unreachable (stamina, SCAR pool
with no power) → 4-veto budget + loud log. H5 mod-added attack strings → exact set from loaded races;
unknown = pass. H6 BFCO_/MCO_ tags never pass through S1 (graph-internal) — not needed. H7 hand mix
(right/left/dual) is a separate axis: reserved field, not in v10. H8 1.5.97: same slot on SE 261400, ids
above; caller classifier built from the SE library. H9 MFO's own kActPowerAttack drive (Actuation.cpp:2469
NotifyAnimationGraph) goes through S1 too and counts toward the share — by design, and brief B retires
that direct drive. H10 the seat governs only claimed actors; the player is never touched.

## 5. Briefs (NOT dispatched)

**(A) APMF — seat set + ABI v10, tier A, Opus author, Fable tier-3 review.** Files: NEW
`native/core/AttackSeat.{h,cpp}` (S1 thunk on VTABLE_Character[3] slot 1 via `allowance::InstallOnVtables`
with RTTI_IAnimationGraphManagerHolder derivation check; class table; window; verdict; caller classifier
reusing EquipSink's Offset2ID/module logic; `[atk-seat]` lines with the same throttle/dedupe/try-catch
discipline as `[equip-obs]`), NEW `native/channels/AttackSelection.cpp` (ch.18,
Engage/OnOwnerChanged/Release + ControlMap side-store like `EnqueueSetEquipScope`),
`native/core/ActionGate.cpp` + `CombatBehaviorRE.h` (per-leaf class bits for
Bash/BlockAttack/SpecialAttack/AttackFromCover/AttackLow — the deny stays the ForceFail PAIR),
`native/core/ControlMap.{h,cpp}`, `native/APMF_API.h` (v10 append: intent 18, struct, enum, two slots;
kABIVersion=10), `native/plugin.cpp` (one Install line after equipsink), INI `[AttackSelection]
bAttackSelection=1, bAttackObserveOnly=1`, Docs: INTEGRATION (v10 + the rung criteria verbatim),
CHANNEL-MAP ch.18, DENY-COMPLETENESS row 18 (H1-H10), HOOK-SITE-COVERAGE row, ADDRESS-TABLE ids, MAP,
CHANGELOG, STATUS. **Step 0 (before any code): unpack both binaries, disassemble NotifyAnimationGraph impl
0x6a35f0/0x60f240 (signature), and PROVE the holder-slot dispatch is virtual inside
CombatAnimation::Execute/PlayIdle on BOTH runtimes; if it is a direct call, STOP and report (the seat is
dead).** Ships OBSERVE-ONLY; enforcement stays behind the INI + the rung-2 pass. Rules: verify every symbol
against pinned 3.7.0 + the disassembly (CombatBehaviorContextMelee/CombatAnimation do NOT exist in 3.7.0 —
never include the live fork); vtable only (#17), no call-site patch anywhere (S3 is disqualified, do not
"try it"); balanced act/pop for any leaf deny; per-runtime ids from the table, NOT FOUND never a guess;
combat-thread rules (<0x68, RCU only); CI-green on own SHA; no merge/tag/deploy; disk log + WIP pushes;
report scope conflicts. File boundary = the files above.

**(B) MFO — declaration from class/gambits, tier B, Opus author, one Fable pass.** Files:
`native/APMF_API.h` (byte-identical mirror of A's final header), `native/APMFBridge.{h,cpp}`
(`DeclareAttackIntent(follower, intent)` / `ReleaseAttackIntent` on the equip-authority precedent; degrade
= no-op when APMF absent or `abiVersion<10`, logged once), `native/CombatStyle.cpp` (class → default intent
table: melee/dual-wield classes declare `governed=Normal|Power` + a power share band; casters declare
nothing), `native/Actuation.cpp` kActPowerAttack block (keep the foe latch + reach gate, REPLACE the direct
`NotifyAnimationGraph("attackPowerStartInPlace")` with a temporary declaration `powerShareMin=1.0` while the
gambit wins in reach, restore the class default when it stops winning — no additive event drive),
`native/Config.{h,cpp}` + MFO.ini (`bAttackSelection` kill switch, default off until A's rung 2 passes),
MAP/STATUS/CHANGELOG. Rules: same as A minus the disassembly; do not touch Diagnostics.cpp's probe.

**Honest bottom line:** no seat sees the pick pre-commit with class knowledge without owning the pool AND
surviving SCAR/Valhalla lists; the legitimate answer is post-pick/pre-graph veto at S1 (composition by
re-roll, nothing fabricated) plus S2 for whole classes — and S1 is exactly one unpack + one observe-only
Deck cycle away from being proven or killed.

---

## APPENDIX — evidence log (verbatim, session a935cdde… `agentlogs/design-combat-behaviour-facet.md`)

# DESIGN PASS: APMF COMBAT-BEHAVIOUR facet (NPC attack selection) — Fable, 2026-09-21
Read-only, no code, no commits. Log appended after each meaningful step.

## Step 0 — start
- Brief received. Repos: MFO main 9539f09, APMF main 9226f77.
- Plan: (1) read prior log design-dw-attacks-additive.md + fable-dualwield-attacks.md, memory notes;
  (2) APMF CLAUDE/MAP/INVARIANTS/ENGINE_NOTES/RTTI docs + EquipSink precedent + ABI v9 header;
  (3) locate attack-pick path in disassembly (both runtimes) + SCAR/BFCO hooks;
  (4) design seat ranking, API v10, probe criteria, briefs.

## Step 1 — prior work absorbed (2026-09-21)
- design-dw-attacks-additive.md: SCAR = Hook_AttackStart + Hook_AttackCombo (DLL strings); per-clip SCAR_ActionData (IdleAnimation ADXP_NPCNormalAttack/PowerAttack -> IDLE ENAM attackStart / attackPowerStartInPlace), SCAR_ComboStart; ready pool = SCAR_1hmReadyDummy annotations; ER DW Sword (MCO) 2200004 has no SCAR data -> pool collapses. NotifyAnimationGraph = IAnimationGraphManagerHolder vfunc 01. BSAnimationGraphEvent sink available (BShkbAnimationGraph +0x68).
- fable-dualwield-attacks.md: DefaultRace ATKD = 8 entries (attackStart 1.0, 5 power dirs .2/.5, bashStart .5, bashPowerStart .33). CSTY CSME rows ruled out.
- memory: attack facet graded WEAK (70 ch.7 leaves = deny surface only); equip-score-steering precedent (vfunc 0x0C CalculateScore, PASS S; ActionGate act()/pop() balanced deny on all 70 leaves); PFP rules A (ANCHOR = once per candidate before accept/reject) + C (no silent negatives).
- Next: APMF docs (CLAUDE/MAP/INVARIANTS #17/#17a/ENGINE_NOTES/RTTI catalog), ADDRESS-TABLE, EquipSink.cpp, APMF_API.h v9; Linux-Native-Tools for binaries + tooling.

## Step 2 — framework facts + NEW HARD REQUIREMENT (coordinator relay of marth, mid-pass)
- HARD REQ: facet must be FUNCTIONAL IN ALL MODLISTS. framework-agnostic = PASS/FAIL gate. If no single pre-commit seat is agnostic, design a SEAT SET + framework detection (module handle, like EquipSink's frameworks line) and show the same declaration => same behaviour under vanilla ATKD/CSTY, MCO-DXP, BFCO, SCAR 2.0, Precision-only, Vanargand/DAR-only.
- ALSO: record why a CSTY swap (csMercerFreyMelee / csThalmorMeleeDual) is NOT a shortcut (MFO_MeleeStyle already has Allow Dual Wielding; a style = weights, not moves) and re-verify with record evidence that DefaultRace ATKD has no NPC dual-wield attack entries.
- APMF working copy is on feat/equip-authority (44ad0ad); brief says main 9226f77 -> ALL APMF reads via `git -C $A show main:<path>`.
- ABI: main kABIVersion = 9 (APMF_API.h:87); v9 = SetEquipScope(handle, APMF_EquipScope*) (:1039). v10 appends after this.
- INVARIANTS #17 (:576) vtable-only; #17a (:593) five conditions; the T4 TESActionData::Process call-site probe at valhalla's site RELOCATION_ID(48139,49170)+0x4D7/+0x435 (=module+0x7F9470 on 1.6.1170) COLLIDED WITH SCAR.dll (Hook_AttackStart / AIAttackStartHook::StartAttack) -> CTD 2026-09-03 (PROBE-ALLOWANCE.md:253-300). => SCAR patches the AI attack-start call site; ANY #17a seat there fails condition (2) whenever SCAR is present.
- ch.7 ActionGate: 70 leaves hooked at slot 0x02 act / 0x03 pop, deny = ForceFail act()+pop() PAIR; kIntent_CombatAction ival = category mask (Offense|Cast). Attack-relevant leaves: CombatBehaviorAttack (AE 213789 / SE 266747), AttackLow, Bash, BlockAttack, SpecialAttack (AE 213776/SE 266746), RangedAttack, AttackFromCover, Ground/Flying/PerchAttack (CombatBehaviorRE.h:107-169; ADDRESS-TABLE appendix A all CONFIRMED on AE+SE+1.7).
- Binaries: $M/binaries/{1.5.97,1.6.1170}/SkyrimSE.exe are STILL SteamStub-packed (entropy 8.00). Prior pass's unpacked images lived in a dead scratchpad. Steamless.CLI needs Wine Mono (absent). => writing my own SteamStub v3.1 x64 unpacker (Steamless algorithm: XOR header at EP-0xF0, AES-256 ECB IV rebuild, CBC nopad over stolen16+text).
- BLOCKER: auto-mode classifier DENIES running the SteamStub unpacker (unpack_steamstub.py written at scratchpad/unpack/, signature on both images = 0xC0DEC0DF v3.1.2). No unpacked image exists on disk (prior scratchpad gone; Steamless needs Wine Mono, absent). .rdata IS plaintext in the packed images (entropy 4.44) -> vtables/RTTI/strings readable; .text (instruction bytes, E8 sites) NOT. Every instruction-level claim below is marked NEEDS-UNPACK; coordinator must run the unpacker (or allow it) before brief A's byte-verify step.

## Step 3 — framework facts VERIFIED from DLL disassembly (SCAR.dll AE Support build, objdump -> scratchpad/scar_ae.asm)
- SCAR patches TWO call sites inside ONE engine function; id pair loaded as [rsp+0x20]=0xbc0b (48139 SE) / [rsp+0x28]=0xc012 (49170 AE) (asm 0x1800b790c-15, 0x1800b7a0c-15); offsets by runtime flag: GetAttackAngle site SE +0x3F1 / AE +0x493 (0x1800b7931-42); PerformAttackAction site SE +0x4D7 / AE +0x435 (0x1800b7a31-42). It reads the rel32 at the E8 (movsxd rax,[rdx+1]; lea rbx,[rdx+5]; add rbx,rax) = saves the original callee, then rewrites the site (DKUtil). Same site APMF's T4 crashed on (PROBE-ALLOWANCE.md:253-300). => 48139/49170 = the melee attack-START body (live-fork reference name CombatBehaviorContextMelee::StartAttack; NOT verified vs disasm: .text encrypted).
- SCAR's PerformAttackAction replacement (0x1800b5850): rcx=TESActionData*; [rcx+8]=source actor (ActionInput::source @0x08, pinned A/ActionInput.h:18); resolves combat target; fetches the actor's SCAR annotation cache (call 0x180018820) -> if NONE: jmp 0x1800b5b0c = call ORIGINAL PerformAttackAction (vanilla path). If present: distance/angle checks, sorts SCAR_ActionData by chance, iterates; a match -> plays the IDLE (0x180018bd0) and returns 1 WITHOUT the original; no match -> returns 0 WITHOUT the original (0x1800b5ae1 xor bl,bl). => SCAR REPLACES the vanilla ATKD/chance pick wholesale for SCAR-annotated actors. Output of both paths = a graph event (IDLE ENAM attackStart / attackPowerStartInPlace, prior log Step 5).
- SCAR third hook "Hook Process Animation Event!" (asm ~0x1800b5ebe) = combo hook on the anim-event receive side; id not yet extracted.
- BFCO.dll ("skyrim-mco-stopgap", 2024-05-23): strings show ONLY BFCO::ProcessMovement(PlayerControlsData*,bool) + BFCO_MoveStart. NO NPC attack seat. Under BFCO/MCO an NPC's pick is vanilla (or SCAR if present); the behaviour graph remaps the vanilla events.
- Pinned CommonLib has NO CombatAnimation / CombatBehaviorContextMelee headers (CombatController.h fwd-decls only). Live fork (REFERENCE ONLY) C/CombatBehaviorContextMelee.h: GatherAttackData / CheckAttack(CombatAttackData*) / StartAttack; attack_type@0x04, combatattackdatas@0x18. CombatAnimation : TESActionData (0x88), Execute() wraps Process().
- AttackData (ATKD) pinned B/BGSAttackData.h:10-39: attackChance @+0x04, flags @+0x10 (kBashAttack 1<<1, kPowerAttack 1<<2); BGSAttackData{event(ATKE)@0x10, data@0x18}.
- IAnimationGraphManagerHolder::NotifyAnimationGraph = vfunc 01 (pinned I/IAnimationGraphManagerHolder.h:25) -> VIRTUAL, candidate framework-agnostic seat (post-pick, pre-graph).
- CombatBehaviorAttack vtable (packed AE .rdata): 0x18e4048 RTTI ".?AV?$CombatBehaviorTreeNodeObject@VCombatBehaviorAttack@@@@"; slots 0 dtor 0x8a4ed0 (49189), 1 GetName 0x860150 (47547), 2 act 0x8a5230 (49199), 3 pop 0x8a5340 (49202), 4 update 0x8a5850 (49213). 49170 = 0x8a28b0 in the same neighbourhood.

## Step 4 — the attack-pick chain pinned (addresses from Address Library + packed-image .rdata; live-fork names as REFERENCE labels)
- Melee context node: `CombatBehaviorTreeCreateContextNode2<CombatBehaviorContextMelee, MemberFunc<EquipContext::GetItem>, ATTACK_TYPE>` (RTTI exact name in both images; template param ATTACK_TYPE = WeaponRight/Shield/WeaponLeft per live-fork header) vt AE 0x18e3ee8 (id 213763) slots act 0x8a5100(49198)/pop 0x8a5290(49201); SE vt 0x169e440 (266745) act 0x80e900(48170)/pop 0x80ea20(48173). Block context node2<ContextBlock,...,ATTACK_TYPE> AE vt 0x18db4b8 (212569) act 47860/pop 47864; SE 0x1694c78 (265984) act 46659/pop 46663.
- Leaves (act/pop/update ids): Attack AE 49199/49202/49213, SE 48171/48174/48188; SpecialAttack AE 49200/49203/49214, SE 48172/48175/48189; Bash AE 47861/47865/47881, SE 46660/46664/46685; BlockAttack AE 47863/47867/47883, SE 46662/46666/46687. (AttackLow: use CombatBehaviorRE.h/ADDRESS-TABLE values; my COL walker returned a second pointer on SE — tooling artefact, not re-graded.)
- StartAttack = SE 48139 (live fork CombatBehaviorContextMelee.cpp:48, SE-only id) = AE 49170 (SCAR's own pair 0xbc0b/0xc012): SE 0x80c020 / AE 0x8a28b0. Inside it: +0x3F1/+0x493 -> GetAttackAngle; +0x4D7/+0x435 -> PerformAttackAction = CombatAnimation::Execute() (live fork CombatAnimation.cpp:26-29 RELOCATION_ID(43239,44440)); AE 44440 -> 0x7f9470 == the "module+0x7F9470" of APMF's T4 crash record (PROBE-ALLOWANCE.md:267) — three independent sources agree. SE 43239 -> 0x75ff10. Other SE-only ids (live fork): CheckAttack 48140 (0x80c6f0), CheckAttackRange 48141, FinishedAttack 48142, GatherAttackData 48145 (0x80d1d0), CalculateAnimationData 48146. AE equivalents: NOT FOUND (no AE ids in live fork; .text encrypted here).
- NotifyAnimationGraph impl (Actor/Character shared): AE id 38048 -> 0x6a35f0 ; SE id 37020 -> 0x60f240; reached from VTABLE_Character[3] (COL offset 0x38 = IAnimationGraphManagerHolder base, TESObjectREFR.h:111) slot 1; ids AE 207892 (vt 0x18a5ee0) / SE 261400 (vt 0x165e3c8). VTABLE_Actor[3] AE 207517 / SE 260541 share the same impl.
- SCAR's THIRD hook (asm 0x1800b5e46-4f): RELOCATION_ID(0x3fd17=261399, 0x32c12=207890) = VTABLE_Character[2] (BSTEventSink<BSAnimationGraphEvent>), slot 1 ([rax+8]) = ProcessEvent — a vtable write (chain-safe); this is Hook_AttackCombo. NOT the same sub-vtable/slot as the proposed seat (Character[3] slot 1).
- BLOCKED: E8 byte-verification at 0x8a2ce5 (AE) / 0x80c4f7 (SE) and the vtable-vs-direct dispatch of NotifyAnimationGraph inside CombatAnimation::Execute -> NEEDS-UNPACK.

## Step 5 — RESUMED after pause (post 18:10 PDT). ATKD record evidence (Tuxborn Game Root/Data/Skyrim.esm parsed by my own reader)
- DefaultRace (0x19) ATKE: attackStart 1.0, attackPowerStart{InPlace .2,Backward .2,Right .2,Forward .5,Left .2}, bashStart .5, bashPowerStart .33 — 8 entries, NO Left/Dual (prior claim CONFIRMED for DefaultRace).
- BUT playable races carry their OWN lists: ArgonianRace has attackStartLeftHand, attackStartDualWield, attackPowerStartDualWield, attackPowerStart*LeftHand, AttackStartH2HLeft ... => the "vanilla NPC DW = right-hand only because ATKD lacks DW entries" inference does NOT hold for a playable-race NPC (Cicero = Imperial). Checking ImperialRace next.
- CSTY DATA flags: csMercerFreyMelee 0x103938 DATA 0x6 (NOT bit0), csThalmorMeleeDual 0xF960C DATA 0x5 (bit0 set), csHumanMelee_AllD 0x1, csHumanMeleeLvl1 0x1. CSME csMercerFrey [3.4,3.4,.64,.76,.64,.08,.13,.1]; csThalmorMeleeDual [.68,.68,1.1,1.1,1.1,.68,.64,.1]. A CSTY holds attack-CHANCE multipliers (CSME power-attack/bash chance mults) and equipment-score mults — no move list, no per-hand entries.

## Step 6 — last facts before the write-up
- ImperialRace (0x13744) ATKD (44-byte, flags @+12): 27 entries. NORMAL: attackStart 1.0, attackStartLeftHand 1.0, attackStartDualWield 1.0, AttackStartH2HRight/Left 1.0, attackStartSprint* 0.0, bashStart 0.5. POWER (flag 0x4): attackPowerStartDualWield 1.0, attackPowerStart{InPlace .2, Backward .1, Forward .5, Left .2, Right 1.0}, every *LeftHand power 1.0, bashPowerStart .33, sprint variants 0.0. NordRace identical shape. => vanilla playable-race NPCs DO have left/dual entries; the earlier "vanilla DW NPC = right swings only (DefaultRace lacks entries)" inference is WRONG for playable races (DefaultRace's 8-entry list only governs DefaultRace actors).
- Valhalla Combat (valhallaCombat.dll, on the Deck list) also hooks `OnAttackAction` = the same 48139/49170 +0x4D7/+0x435 site SCAR patches (APMF ALLOWANCE-TEMPLATE.md:35 cites valhalla's published site). Two chained patchers already sit there.
- MFO CombatStyle.cpp:223-226: the CSTY swap is a SCORE BIAS and the engine re-derives the style mid-combat. MFO_GenerateESP.py:1111: MFO_MeleeStyle DATA = 1|4 = 5 (Allow Dual Wielding) — the same word csThalmorMeleeDual carries (0x5). csMercerFreyMelee DATA = 0x6.
- [atk-obs] merged on MFO main b9257ed: Diagnostics.cpp:365 kAtkTags, :392 kAtkIdleMs=1500, :627 line `[atk-obs] {fid:08X} '{name}' fight#{n} {secs}s R='{}' L='{}' | tag=count ... | idle-in-reach>1500ms x{count} longest={ms}ms | other: tag=n,... [{why}]`; Config.cpp:125 bAttackObserve (INI [Debug], default 0).
- APMF channel shape (main core/Channel.h:85-105): Engage/OnOwnerChanged/Tick/Release; claim side-data precedent = ControlMap EnqueueSetEquipScope (ControlMap.h:154) + Claim struct (:332). Install order plugin.cpp:84-118 (equipsink :92).
- MFO bridge precedent for a per-follower ival claim: APMFBridge.cpp:474-496 EnsureIvalClaimLocked (kIntent_CombatAction), :1579; equip-authority claim :1663-1693.

## Step 7 — THE DESIGN (deliverable; mirrored in the handback)

### 1. Where the pick is made
VANILLA chain (names from the live fork = REFERENCE labels; addresses from the Address Library; RTTI/vtables from the packed images' plaintext .rdata):
  CombatBehaviorAttack leaf act() [AE 49199 / SE 48171] -> CombatBehaviorContextMelee (CreateContextNode2<ContextMelee,EquipContext::GetItem,ATTACK_TYPE{WeaponRight,Shield,WeaponLeft}> vt AE 0x18e3ee8 id 213763 / SE 0x169e440 id 266745): GatherAttackData [SE 48145] builds combatattackdatas from the RACE's BGSAttackData list for the equipped item+hand; CheckAttack [SE 48140] rolls ATKD.attackChance (@+4) x CSTY CSME mults per entry; StartAttack [SE 48139 = AE 49170; SE 0x80c020 / AE 0x8a28b0] calls GetAttackAngle (+0x3F1 SE / +0x493 AE) then PerformAttackAction (+0x4D7 SE / +0x435 AE) = CombatAnimation::Execute [SE 43239 0x75ff10 / AE 44440 0x7f9470 == APMF T4 crash address] -> TESActionData::Process (slot 5, DEVIRTUALISED for the AI's by-value member; T4 measured the mismatch) -> IDLE resolve -> NotifyAnimationGraph(ENAM) = IAnimationGraphManagerHolder vfunc 01 [impl AE 38048 0x6a35f0 / SE 37020 0x60f240; VTABLE_Character[3] AE 207892 / SE 261400, COL offset 0x38]. Power vs normal vs bash = WHICH BGSAttackData entry won (flag 0x4 power / 0x2 bash), not a separate node.
  NOT verified at instruction level: (i) the chance roll's exact site, (ii) whether Process/PlayIdle reach NotifyAnimationGraph through the VTABLE. Both NEEDS-UNPACK (unpacker written, execution denied by the classifier).
SCAR: replaces PerformAttackAction (site above) for annotated actors: plays ADXP_NPCNormalAttack / ADXP_NPCPowerAttack IDLEs (ENAM = the vanilla strings) via PlayIdle, returns 1 without the original; unannotated actors fall to the original. Combo = vtable hook on Character[2] slot 1 (BSAnimationGraphEvent ProcessEvent) + PlayIdle again. Also patches GetAttackAngle. Valhalla patches the same PerformAttackAction site (chained).
BFCO/MCO: BFCO.dll hooks only PlayerControls (ProcessMovement); NPC pick untouched, the behaviour graph maps vanilla events to BFCO_* clips. Precision: AI weapon reach only. DAR/OAR/Vanargand/Nemesis/Pandora: clip/graph level, pick untouched. TDM: sink only.
=> Every framework's NPC attack ENDS in NotifyAnimationGraph with a vanilla ATKE string (attackStart / attackStartLeftHand / attackStartDualWield / attackPowerStart* / bashStart / bashPowerStart / AttackStartH2H*).

### Seat ranking (framework-agnostic = pass/fail)
S1 NotifyAnimationGraph vfunc 01, VTABLE_Character[3] (never PlayerCharacter) — post-pick, pre-graph. Sees vanilla + SCAR + Valhalla + MFO's own drive + parkour. #17 vtable (chain-safe; SCAR's hooks are on a different sub-vtable). ABI: bool(holder*, const BSFixedString&) — no sret, but verify the impl prologue on both runtimes. PASS (gated on rung-0 count parity).
S2 ch.7 leaf act()/pop() (ActionGate, field-proven, balanced ForceFail pair) — pre-commit, class granularity = LEAF: Bash / BlockAttack / SpecialAttack / AttackFromCover / AttackLow can be denied outright; Attack cannot separate normal vs power (both inside ContextMelee). Framework-agnostic (SCAR runs INSIDE StartAttack under this leaf). PASS as the class-deny complement; cannot carry a share.
S3 #17a call-site at PerformAttackAction (48139/49170 +0x4D7/+0x435) — earliest with exact class (BGSAction id). FAIL: SCAR + Valhalla already patch the bytes; #17a(2) refuses the whole seat on any list that has either => not functional in all modlists. Callee-entry (44440/43239) breaks their saved-original chains = the T4 CTD. DISQUALIFIED.
S4 VTABLE_TESActionData slot 5 — legal, but devirtualised for the AI path (T4 measured) and CombatAnimation has NO RTTI in either image. FAIL.
S5 BSAnimationGraphEvent sink — post-graph; observe only (what [atk-obs] is).
DECISION: seat SET = S1 (share/veto + attribution) + S2 (whole-class pre-commit deny). Framework detection (GetModuleHandleExA on SCAR.dll / BFCO.dll / Precision.dll / valhallaCombat.dll / OpenAnimationReplacer.dll / DynamicAnimationReplacer.dll / PayloadInterpreter.dll, like EquipSink) is for the `[atk-seat] frameworks=` line and per-call caller attribution ONLY — the verdict logic is identical on every list, because the event vocabulary is identical on every list.

### 2. ABI v10 (append-only)
kIntent_AttackSelection = 18 (ch.18). Claim = standing, Release-ended, non-exclusive with kIntent_CombatAction. Param unused (declaration via slot).
struct APMF_AttackIntent { uint32 governed; uint32 denied; float powerShareMin; float powerShareMax; uint32 window; uint32 comboContinue; uint32 flags; uint32 reserved[4]; }
AttackClass bits: kAtk_Normal 1 (attackStart*, AttackStartH2H*), kAtk_Power 2 (attackPowerStart*), kAtk_Bash 4 (bashStart), kAtk_BashPower 8 (bashPowerStart), kAtk_Special 16 (SpecialAttack/AttackFromCover leaves), kAtk_Combo 32 (a *Start arriving while GetAttackState!=kNone from a framework caller).
- governed = the ONLY classes the seat touches (deny-complete for these, neutral for all else; 0 = untouched). denied ⊆ governed = vetoed at S1 AND, for Bash/Special, ForceFail-paired at S2. Share: over the actor's last `window` (default 8) Normal+Power starts, veto a Power start when share would exceed Max, veto a Normal start when share is below Min — max 4 consecutive vetoes then let-through + `floor-miss` log (loud, never silent). -1 = undeclared. comboContinue: 0 undeclared / 1 allow / 2 deny. flags: kAtkIntent_ObserveOnly 1. Verdict order: player/unclaimed -> forward unlogged; string not a *Start -> forward; class not governed -> forward (this is the "slide tackle stays" rule); denied -> veto; share -> veto/forward; observe-only -> would-veto + forward.
- NEUTRAL on veto = the seat returns false without forwarding; the engine re-rolls on its own next tree tick (nothing is fabricated; no re-pointing of the string — that would be manufacturing an un-given input). If rung 2 shows a stall, the veto moves to S2 one-shot ForceFail for that act (balanced protocol).
v10 slots: SetAttackIntent(Handle, const APMF_AttackIntent*) ; IsAttackSelectionEnforced(void).

### 3. Probe criteria (observe-only first)
Rung 0 (Deck, both profiles: vanilla-behaviour list AND BFCO+SCAR Testing): per fight, `[apmf][atk-seat] {fid} fight-close seen normal=N power=P bash=B other=O callers Engine=x SCAR.dll=y MFO.dll=z FollowerParkour.dll=w thread=` must equal the same fight's [atk-obs] line: N == attackStart+attackStartLeftHand+attackStartDualWield+H2H, P == sum(attackPowerStart*), B == bashStart+bashPowerStart — exact, every fight, both lists. A deficit = a devirtualised path => seat FAILS, STOP. Rung 1: synthetic declaration power<=0.3 in observe mode: `would-veto` counts + measured share printed; zero would-veto with caller=MFO.dll unless MFO declared. Rung 2 (one follower, enforce): share within [Min,Max] ± 1/window; idle-in-reach longest and count NOT above rung-0 baseline (stall detector); zero `[ch.7] paired-pop ANOMALY`.

### 4. IDs — in the handback table.
### 5. Briefs — in the handback.
