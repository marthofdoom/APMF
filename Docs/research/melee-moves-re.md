# Melee moves (T2 batch M): reverse engineering notes and resume state (WIP, 2026-11-11)

Copied from the author's working log so it survives a /tmp wipe. PROVEN = read in the disassembly; nothing here has been observed in a running game. The CHECKPOINT section says what is written, what is not, and the design already decided for the remaining items.

- Harbinger: feat/melee-moves from origin/feat/ch8b-dead-target b96edf0, merged origin/feat/aversion c6df79b -> 20b8c52 (clean, no conflicts).
- MFO: feat/melee-moves from origin/feat/g7-reanimate 533d6ef, merged origin/feat/class-aversion a1b620f -> c2c49bb (clean).
- Read: briefs COMMON + Batch M, manifest s2/s3, Harbinger CLAUDE.md, ch.18 + ch.31 ABI blocks, core/CombatMove.{h,cpp} whole, ActionGate act thunk, MFO Fire.cpp power attack, CombatStyle.{h,cpp} ApplyTick, classes/Package.cpp.
- RE toolkit: scratchpad/melee/rx.py (rt = ae|se|r17; dis/xref/vt/ptr/rip/str/rev), scan.py (byte pattern), atkd.py (race ATKD dump), cc38.py.

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
- DODGE: deferred until items 1,2,3,5 pushed (brief allows). Note: class CSTY avoidThreat only becomes effective through the style facet (seat B).

## PLAN / ABI
- v43 Block (kCombatMove_Block built + kCombatMove_BlockEnd), v44 PowerAttack, v45 CombatStyle (kIntent_CombatStyle=33), v46 Dodge. Item 3: none.

## CHECKPOINT (token limit hit mid item 1) -- READ THIS FIRST IF RESUMED
STATE: item 1 (Block, ABI v43) Harbinger side WRITTEN, NOT YET BUILD-VERIFIED. First local build was still compiling
(log: scratchpad/melee/apmf-build.log, build dir _toolchains/build-melee-apmf). Nothing in MFO written yet beyond the base merge.
Harbinger WIP commit pushed on feat/melee-moves (see `git log`), message starts "WIP(ch31-block)". NOT reviewed, NOT green.

Harbinger files touched for v43 (all in worktree /tmp/claude-1000/worktrees/melee-apmf):
- native/APMF_API.h: kABIVersion 43; kCombatMove_Block built + kCombatMove_BlockEnd=4; kCombatMoveState_Ended=5; whys 13..20;
  info bits HoldLive(1<<5) BlockedAHit(1<<6) ReRaised(1<<7); kCombatBlock* constants; "ABI v43: BLOCK" block at the end; param table fval row.
- native/core/CombatMove_internal.h (NEW): detail:: Sent, Finish, MarkInfo, EndHold, Gen, MoveName, WhyText, ProcessAction, ActionLeftAttack/Release, RunBlock, RunBlockEnd, ResetHolds, NoteBlockedHit.
- native/core/CombatMove.cpp: shared helpers moved into namespace detail; Terminal(rec) treats a live hold as non-terminal; Deliver dispatches Block/BlockEnd;
  Enqueue accepts them (fval window, clamp); Install resolves Left Attack / Left Release DOBJ + computes g_blockWhy once ([CombatMove] bBlock, ActionGate arm); hit sink -> NoteBlockedHit; ResetAll -> ResetHolds.
- native/core/CombatMove.h: BlockNotAvailableReason, AnyBlockHeld, BlockHeld, NoteHoldDeny, HoldPoll.
- native/core/CombatMoveBlock.cpp (NEW): hold table + lock-free mirror g_heldFid[16], HandsWouldNotBlock, RunBlock (renew / raise / read-back wantBlocking), RunBlockEnd, HoldPoll (expiry -> Left Release, broken tests, re-raise every 250 ms), heartbeat.
- native/core/ActionGate.{h,cpp}: g_blockVt / g_blockAttackVt (paired), g_blockHoldArmed, BlockHoldDenyArmed(); ActThunkBody fails Offense + Block + BlockAttack leaves for a held actor BEFORE the leafCat==0 gate. FILE IS NOW 2484 LINES (backstop 2500): REPORT IT, add nothing more there.
- native/core/Arbiter.cpp: HoldPoll() call after the aversion heartbeat.
STILL TO DO for item 1 (Harbinger): get the build green; INI key [CombatMove] bBlock in Data/SKSE/Plugins/APMF.ini + Docs/INI-REFERENCE.md; Docs: FACETS ch.31 (block section), APMF_API-HISTORY v43, APMF_API-REFERENCE, INTEGRATION, CHANNEL-MAP row 31, DENY-COMPLETENESS-AUDIT row (open paths: equip leaves can swap the shield; a Block leaf already running at hold start releases once then re-raise; engine clearers 29383 / 38696 / 40447-stamina; other clients; 1.7.104 refused with the rest of ch.31), INVARIANTS #0 (l) amendment (a move with a duration + internal leaf deny), MAP.md, STATUS.md, CHANGELOG, REVIEW-BACKLOG (ch.31 on 1.7.104 not verified; ActionGate.cpp at 2484 lines needs a split brief).
STILL TO DO for item 1 (MFO): mirror APMF_API.h byte-identical (md5 both); Vocabulary.h APPEND kActBlock = "act.block"; Board_internal.h action table + CooldownExposed; i18n/Strings_keys.h MFO_STR(Act_Block,"Block",0);
  new apmf/BlockClaims.cpp (BlockSupported abi>=43, ServiceBlockRule: MFO-side hands/weapon-drawn pre-gate, request kCombatMove_Block with fval = [Combat] fBlockHoldSeconds default 2.5, skip re-request while own hold has >40% left, backoff 2 s after refusal; PollBlock logs outcome once; ReleaseBlock on dismissal (kCombatMove_BlockEnd); ClearBlockHolds at load);
  cast/Fire.cpp handler (Raised -> Fired opaque, Held -> NoOp opaque, failure -> transparent); CombatScan.cpp PollBlock next to PollCombatMove (line ~1330); Followers.cpp ReleaseBlock; Bridge.cpp ClearBlockHolds;
  classes/ClassTable.cpp Knight (and Great Knight) default row G(kCondFoeAttackingMeMelee, kActBlock, 0, None, kSelf, 4.0f) -> Q-melee-1 (hold 2.5 s / cooldown 4 s default); docs GAMBITS, MAP, STATUS, INI-REFERENCE, in-game-test-list T2 steps with sDevClassEquip lines; run tools/iaus_harness/run.sh + tools/iaus_sim/run.sh.

DESIGN DECISIONS ALREADY TAKEN FOR THE REST (do not re-derive):
- Item 2 PowerAttack = ABI v44, kCombatMove_PowerAttack = 5, same action road as the bash (Right Attack action + the entry's event) in core/CombatMove.cpp Run():
  candidates = actor base attack entries with Power, !Bash, !Left, attackChance > 0, tried in DESCENDING attackChance (ties: lower event string), up to 8, until Process() returns true
  (this mirrors StartAttack's own erase-and-redraw on a refused Execute, AE 0x8A2CEA..0x8A2D53; sword users end on attackPowerStartForward 0.5). Read-back: attackData == entry AND attack state kDraw(1) or kSwing(2)
  (post step AE 39004 +0x13E..: non-bash entry -> state 1). Hit sink: kPowerAttack flag && !kBashAttack marks HitSeen. Refusals as bash (OffenseDenied applies).
  MFO: Fire.cpp:471 keeps NotifyAnimationGraph("attackPowerStartInPlace") as the OLD road; new INI bPowerAttackEngineRoad (default 1) picks Harbinger when abi >= 44; log "road=harbinger|graph" on every power attack; RequestBash generalised to a move kind; PollCombatMove names it.
- Item 3: NO CODE (see FINDING above). Write it into STATUS / the report / open-questions as Q-melee-N ("manifest row G-H2 was stale").
- Item 5 CombatStyle = ABI v45, kIntent_CombatStyle = 33, new channel channels/CombatStyle.cpp + core/CombatStyle.{h,cpp} (model: channels/Aversion.cpp + core/Aversion.cpp).
  param.form = TESCombatStyle FormID (REQUIRED), param.ival flags: kCombatStyle_EquipChoiceOnly = 1<<0.
  SEAT B (actor-wide, default): write_call<5> at Actor::GetCombatStyle (AE id 38536 0x6B5580) +0xD, the ONE call to ExtraDataList::GetCombatStyle (AE 12017 0x16BFC0; the engine's own per-reference override slot). Thunk(ExtraDataList* l): actor = l - 0x70 (TESObjectREFR::extraList), claim wins -> return claimed style, else orig.
  SEAT A (EquipChoiceOnly scope): write_call<5> at CombatController::UpdateCombatStyle (AE 33239 0x559BD0) +0x70 (call 38536); returns the claimed style into the controller cache only.
  Engage / Release / owner change (game thread): call the engine's own refresh 33239(actor->combatController) exactly as Actor::SetCombatStyle's tail does (AE 38537 0x6B55E0 +0x31).
  Lock-free slot table {fid, style*, flags} published on the game thread; ControlledCount()==0 fast path; seqlock re-check of fid. Rank vs Engine on Facet::kCombatAction.
  NEED: SE + 1.7.104 twins of 38536 / 33239 / 12017 / 38537 (not yet located), VerifiedAddresses rows via tools/verified_addresses/spec.json + generator, SeatVerified guards, INVARIANTS #17a amendment (two more call-site seats), a passive probe line.
  MFO: new apmf/StyleClaims.cpp; CombatStyle::Want keeps the desire, ApplyTick's cc+0x38 write becomes the Harbinger-ABSENT degrade only; class package style (tree ON) -> actor-wide claim (strict adherence); MFO_*Style stance without a class -> EquipChoiceOnly (same reach as today's write). Report the finding that today's write never applied the class CSTY movement fields.
- Item 4 Dodge = ABI v46, RE NOT STARTED beyond: vanilla leaf CombatBehaviorDodgeThreat (AE vt 0x18DAFD0: act 0x86A0F0, pop 0x86A160, update 0x86A670, slot5 0x869FC0) gated by the style's avoidThreatChance (only effective through seat B);
  UND (LoreRim) hooks the DodgeThreat / fallback / circle / backoff createPath and sends TKDodge* events itself; TK Dodge RE is player only; Simple NPC Dodge is perk/spell/OAR. Per the brief: push items 1, 2, 3, 5 first, then dodge, and say so in the report.
