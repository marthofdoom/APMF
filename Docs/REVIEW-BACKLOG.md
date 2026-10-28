# REVIEW-BACKLOG.md — deferred Fable review findings (APMF)

**What this is.** Findings a Fable review raised that were DEFERRED under `CLAUDE.md` rule 9,
not dropped. A review round returning nothing above SEV-3 ends the cycle; the SEV-4/SEV-5
remainder lands here and is drained as ONE batch at a natural boundary — before a release cut,
or before a field cycle touching that subsystem.

**Deferred is not dropped.** Each entry keeps the finding's VERBATIM text, its severity, the SHA
it was raised against, and the reviewer's reasoning. Nothing here may be closed by assertion.

**NEVER deferrable (rule 9 carve-outs), whatever the nominal severity:**
- co-save layout, threading, or ABI / byte-shared-header findings
- anything the NEXT field cycle will exercise

Distinct from `Docs/DENY-COMPLETENESS-AUDIT.md`, which tracks DESIGN gaps in facet coverage.
This file tracks REVIEW findings only.

---

## OPEN
### APMF-B75 (SEV-4) -- ch.26 RankOf excludes items that will not actually be lifted
Raised against `7cda290` (`feat/apmf-equip-preference`, weighting phase 1), tier-3 Opus review 2026-10-12, finding F5. Verbatim: "`RankOf` excludes declared or cast-driven items that will not actually be lifted (outranked, or D3 held hand with 0x0F off or ranked out), so they keep real scores and can beat the lift. Fix: apply D3 + Outranks inside RankOf." Reasoning: the rival set leaves out every item that MATCHES a preference or a cast claim, whether or not that item is lifted this pass; an unlifted match keeps its engine score in the list and can outscore a lift sized without it. Closure: run the same D3 (`HeldFor`) and `Outranks` tests inside `RankOf` (cache per call).

### APMF-B76 (SEV-4) -- CastClaimHandle ranks with the highest-basis cast claim on ANY hand
Raised against `7cda290`, same review, finding F6. Verbatim: "`CastClaimHandle` = the highest-basis cast claim on any hand. With two clients on different hands, the wrong client's priority ranks the decision. Fix: carry the claim handle through CastSeatClaim/HandHold." Reasoning: the hand block (sink and 0x0F) and the ch.8b score steer rank with `facetattr::CastClaimHandle(actor)`; with two clients holding different hands the other hand's client may be the one ranked. Closure: carry the per-hand claim handle in `CastSeatClaim` / `HandHold` (a ControlMap read change, mind the 2500-line cap).

### APMF-B77 (SEV-4) -- BestRival does not verify a_this is in its own category list
Raised against `7cda290`, same review, finding F8. Verbatim: "BestRival does not verify that `a_this` is a member of `inventoryItems[GetCategory()]`. Fix: a short scan." Reasoning: the reviewer enumerated the callers of 0x0C on all three images (only the rescore loop), so today the item is always in its list; the check would turn a future caller into an `Unreadable` instead of a lift sized against the wrong list. Closure: one membership pass in the existing loop (`seenSelf`), `Unreadable` when absent.

### APMF-B78 (SEV-4) -- force and a slot-less left-hand preference; the ~2 s force delay is not enforced
Raised against `7cda290`, same review, finding F9. Verbatim: "a slot-less one-hander preferred for the LEFT hand passes ForceRefusal, but the engine puts it in the RIGHT hand. Also, Harbinger doesn't enforce D4's ~2 s (a client can request Force at claim creation, logged at warn)." Reasoning: `ForceRefusal` passes an item preferred for any hand it competes for (a one-hander with an EitherHand / null slot competes for both), so a dagger preferred Left equipped slot-less lands in the right hand unrefused; and the bit is honoured from claim creation (documented, logged at warn). Closure: for a slot-less one-hander, refuse unless preferred for the RIGHT hand (the engine's slot-less landing); decide with marth whether Harbinger should hold the bit until ~2 s of combat after the declaration.

### APMF-B82 (SEV-4) -- held engagement: one internal-bound slot per actor, no owner token
Raised against `c557820` (`fix/apmf-engagement-held`), tier-3 Opus re-review 2026-10-12 (reviewer log `scratchpad/agentlogs/review-apmf-engagement.md`), finding R-2. Verbatim: "SEV-4: shared single internal-bound slot: hold StopDrive can clear a leash-return bound (no owner token)." Reasoning: `channels/CombatApproach.cpp` keeps ONE Harbinger-internal bound per actor; `core/ActionGate.cpp` `LeashReturnPoll` (un-held actors) and the hold's drives in `channels/CombatEntry.cpp` (held actors) are mutually exclusive by the held / un-held split, but a hold that starts or ends between the two polls can let one owner's `ClearInternalBound` drop the other's bound. It heals itself on the next poll (the owner re-sets it). Fix: `ClearInternalBound` takes the owner's token and clears only its own bound.

### APMF-B83 (SEV-4, MOOT by ruling) -- held engagement: re-pin churn during the re-engage cooldown
Raised against `c557820`, same re-review, finding R-4. Verbatim: "SEV-4: cooled pin 1-frame live window -> MFO everLive re-pin churn bounded." MOOT 2026-10-12: marth ruled "THe engage on sight is meant to be an aggressive setting. No waiting ever, engaged, or not engaged.", so the 60 s re-engage cooldown is REMOVED for every end reason (and MFO is removing its own 60 s hold). A release -> re-engage cycle now repeats freely; each end is logged every time with "End #N" for that actor -> target and the `[hold] heartbeat` counts repeat ends, so the field log shows a cycle plainly. Kept for the drain record.

### APMF-B84 (SEV-5) -- held engagement: a mid-fight group merge ends the pin
Raised against `c557820`, same re-review, F-4 residual. Verbatim (coordinator relay): "F-4 residual: a mid-fight group merge ends the pin (SEV-5), and MFO re-pins." Reasoning: `kReasonAbsent` (`channels/TargetPin.cpp`) ends a pin whose target WAS in the actor's group's targets and is not any more; a group merge mid-fight can move the actor into a group whose targets do not (yet) include the pinned target, which ends the pin. MFO re-pins on its next evaluation. Accepted residual of the (g) condition 5 "never live and inert" fix.

### APMF-B37 (SEV-5, FIXED in docs) -- census MAP rule on CensusNote order at 0x0A
Raised against `58a4438` (`fix/apmf-cast-instant-caster`), tier-A Opus 5.5 review 2026-09-29 (reviewer log `scratchpad/agentlogs/review-apmf-instant-caster.md`), finding F3. Verbatim: "F3 SEV-5: MAP says CensusNote "must stay after the seat's own answer is decided"; on 0x0A it is called before orig (harmless, but the rule as written is not what the code does)." Reasoning: the 0x0A note reads only `this`/`cc` and touches no argument, so the order is harmless; the rule was worded wrong. MAP now states the real rule (never change an answer or an argument; after the answer at 0x06, before `orig` at 0x0A). Kept here for the drain record.

### APMF-B38 (SEV-4 process, CLOSED by decision) -- the census added two new files
Raised against `58a4438`, same review, finding F4. Verbatim: "Scope: RestoreCensus.{h,cpp} are NEW files. The coordinator's brief names "a passive Restore-caster census" but no file; the design's file column lists AiCastSeats/CastSeats/CastObserve. APMF CLAUDE.md rule 1 requires a new file to be named in the brief. Separate concern, GLOB build, justified -- coordinator to confirm (F4 SEV-4 process)." Decision (coordinator, 2026-09-29): confirmed IN SCOPE; the census was requested. No code action.

### APMF-B39 (SEV-3, CLOSED 2026-09-29 on `fix/apmf-self-claim-handle`) -- a self cast claim has no target handle, so 0x0F's deny-complete is off for its hand
Raised against `58a4438`, same review, finding F5. Verbatim: "F5 SEV-3 (pre-existing, docs): the same missing handle ALSO switches off EquipGate's 0x0F deny-complete for the claimed hand (needs hasHandSeat || handDenyOnly). Under a self heal claim the combat AI may arm another spell into the claimed hand; CastGate still refuses its charge per hand, so the hand can sit holding a spell it may not cast. Add this to STATUS phase 0c / the phase-3 brief (resolving castTarget 0 to the claimant's own handle fixes both)." Reasoning: `ControlMap.cpp` resolves `castTargetHandle` only for a non-zero target; `EquipGate.cpp` `hasHandSeat = haveHandClaim && handSeat.targetHandle`; MFO sends self heals with target 0. Documented in STATUS phase 0c and MAP. **Closure:** the phase-3 brief (give a self claim the claimant's own handle), its own tier-A round. Not deferrable past phase 3: the next field cycle exercises self heal claims only if MFO phase 2 lands first.
**CLOSED 2026-09-29** (`fix/apmf-self-claim-handle`): `ControlMap::ApplyRequest` resolves a RequestCast claim's target 0 (self) to the claimant's own FormID before the handle resolution (the "A SELF CLAIM NAMES THE CLAIMANT" block), so a self claim gets the claimant's handle, every seat serves it, and 0x0F's deny-complete (`hasHandSeat`) covers its hand. Deny-only claims and the degenerate `RequestEx(kIntent_Cast)` form keep target 0. Field check: STATUS "Phase 3a" (self-heal census windows ENGAGE within about 1 s). Pending its tier-A review.
**Review round (tier A of `5250fb2`, reviewer log `scratchpad/agentlogs/review-apmf-selfclaim.md`):** F1/F2 (SEV-3, a non-kSelf spell claimed at self would be aimed AT the caster) FIXED on the branch: the self resolution now applies ONLY to a kSelf-delivery spell (`LookupByID<SpellItem>` + `GetDelivery`, the proxy mint's pair); any other delivery keeps target 0 and no handle exactly as before, with a rate-limited `[ch.8b] ... not kSelf` warning. F3 (SEV-3, carve-out b) FIXED in docs: STATUS Phase 3a FIELD CHECK rewritten around what MFO sends today (kSelf concentration self casts; MFO self heals go direct since 2026-09-21). F5 (SEV-5, the info line not rate-limited) FIXED by the same round: both lines go through `SelfClaimLogDue(actor, spell)`. F4 and F6: APMF-B41 / APMF-B42.

### APMF-B41 (SEV-5) -- the degenerate RequestEx(kIntent_Cast) claim stays handle-less; the API doc does not say so
Raised against `5250fb2` (`fix/apmf-self-claim-handle`), tier-A Opus 5.5 review 2026-09-29 (reviewer log `scratchpad/agentlogs/review-apmf-selfclaim.md`), finding F4. Verbatim: "F4 (SEV-5, answers spot 3) -- the degenerate RequestEx(kIntent_Cast) stays handle-less. Consistent with APMF_API.h:289-290 ("degenerate form (no target/proxy/TTL)"), so leaving it alone is correct and needs no ABI change. But that form now carries exactly the B39 shape on purpose: no seats, and no 0x0F deny-complete for its hand. The API doc does not say "the seats do not serve it and deny-complete is off". Backlog: add one sentence to the kIntent_Cast doc in the next header touch. That is comment-only, but it is a byte-shared header, so it must mirror MFO's copy." Reasoning: behaviour is correct and documented as "no target"; only the consequence is unstated. Closure: one comment sentence in `APMF_API.h` kIntent_Cast at the next header touch, mirrored into MFO's copy byte-for-byte.

### APMF-B42 (SEV-5, FIXED in docs) -- the CHANGELOG line over-claimed the self-heal case for MFO users
Raised against `5250fb2`, same review, finding F6. Verbatim: "F6 (SEV-5) -- CHANGELOG over-claims for MFO users. "An NPC healing themself waited..." is true for a generic client but not for MFO today (F3). Wording only. Fold it into F3's doc fix." Reasoning: current MFO main sends no self-heal claims (CastSelfDirect asks ComposedCast::Try only for non-restoration spells). FIXED in the same round as F3: the CHANGELOG line now names kSelf spells generally and says MFO's self heals reach it only once MFO's animated-heal phase 2 lands. Kept here for the drain record.

### APMF-B40 (SEV-4 x3 + SEV-5) -- [ctcensus] caster-type census probe review items
Raised against `16ac6f5` (`feat/apmf-castertype-probe`), tier-A Opus 5.5 review 2026-09-29 (verdict: nothing above
SEV-3; the one SEV-3, `HandHolds` calling the allocating `GetMagicCaster`, and SEV-5 (b), the whitespace edit, were
FIXED on the branch before merge; SEV-5 (a) is a docs correction in the MFO repo, listed below). Verbatim from the
reviewer's log (scratchpad agentlogs/review-apmf-castertype-probe.md, "Findings"):

- **SEV-4: one line budget for everything, so combat-thread "OTHER" lines can crowd out CLOSE verdicts.**
  `LineDue()` gives 60 lines per 10 s, shared by the combat-thread `OTHER FIRED` lines (`:335-342`) and by the
  Poll lines. `OTHER FIRED` is not deduplicated: it logs on every 0x0B fire of any non-driven spell by a claimed
  actor. The CLOSE and SERIES lines, which are the data the probe exists for, are flushed LAST in each Poll
  (`:774`). In a busy multi-caster fight, OTHER FIRED lines can use up the window, and the CLOSE verdicts are the
  lines that get dropped. The heartbeat reports `lines dropped`, so the loss is visible, not silent. Suggested
  fix: a separate budget for CLOSE/SERIES, or one OTHER FIRED line per (window, type) with counts kept for the CLOSE
  line.
- **SEV-4: the per-window anim counts depend on CastObserve having registered a sink.**
  `NoteAnimEvent` only receives events from CastObserve's per-actor sink. CastObserve registers that sink only
  after it has seen the actor casting (`CastObserve.cpp` Poll, `if (casting)`). So the first claimed cast of an
  actor that has not cast before can end its window with `anim SpellFire 0 BeginCast 0`, even though the hand
  animated. The verdict relies on `firstChargeMs` (the hand-state poll) for "ANIMATED", so the verdict is right.
  But the `anim SpellFire N` figure in FIRED verdicts undercounts on the first window per actor. Record this so
  the field read does not treat 0 as "did not animate".
- **SEV-4: attribution takes the first matching window.**
  In `Observe` (`:298-299`), a build or fire for an item goes to the FIRST window with that driven form. Two
  windows on one actor with the same driven form and different targets would all count on the first one. One case
  is L and R each holding a claim for the same spell on different targets. A repoint is not affected: its old
  window is closed in the same Poll that opens the new one, before any seat can see both. This only affects the
  data, not safety.
- **SEV-5 (a): stale Armor claims in docs and comments elsewhere.** (Not fixable in this repo: ENGINE_NOTES §0.28
  and `CasterConsent.cpp:372` live in the MFO repo. Open there.)
  The branch shows that VTABLE_CombatMagicCasterArmor is a real CombatMagicCaster on both runtimes. I re-verified
  this: the COL/CHD hierarchy is {Armor, CombatMagicCaster, CombatObject, NiRefObject} on AE 0x18CD4D0 and SE
  0x1687268. ENGINE_NOTES §0.28 and MFO `CasterConsent.cpp:372` ("a symbol with NO class -- excluded") still say
  the opposite. The branch records this in the spec row and STATUS only. Backlog a docs correction.
  (Line numbers above are against `16ac6f5`; the fix commit shifted `CasterTypeCensus.cpp` by +12 lines after `:415`.)

### APMF-B36 (SEV-4 / SEV-5; F2 = PHASE-2 PREREQUISITE) -- [Probe] bRangedSelect ranged-select probe review items
Raised against `827f320` (`feat/apmf-ranged-select-probe`), tier-A Opus 5.5 review 2026-09-29 (verdict: nothing above
SEV-3; F1 promoted by carve-out (b) and FIXED on the branch before merge). Verbatim from the reviewer's log
(scratchpad agentlogs/review-apmf-ranged-probe.md, "FINDINGS"):
- **FIXED (same branch, carve-out b):** F1 SEV-4: the "the engine reads the same bytes a few instructions later" argument holds on every path EXCEPT 48124's early actor-state fail (0x877DDD `cmp [actor+0xC8],0x10000000; jae` -> SetFailed+Ascend, the window is never read). In that case the probe reads the window the engine skipped. The window is set by the parent node, independent of that actor check, so in practice it is still valid; the residual is a stale/garbage ctx making the probe dereference `item` (vtable, +0x10, GetName vfunc) on the combat thread = a CTD the engine would not have had. Scenario: none observed; needs a parent that runs the Equip leaf with no context pushed for an actor failing the +0xC8 test. Fix (backlog): bound the offset by the context buffer's size before reading ctx+8, or read the window only when the leaf's own actor test would pass. Empty window: ptr null -> probe returns silently (engine would fault).
  Fix: `core/ActionGate.cpp` `RsEquipLeafWouldReadWindow` mirrors the attack-state test of all four leaf bodies (AE 48124 0x877DDD / 48126 0x878150, SE 46955 0x7E0724 / 46957 0x7E0AA0) and `RangedProbeEquipLeaf` also bounds `off + 0x10` by the window header's top (+0x0C).
- **PHASE-2 PREREQUISITE (write into the phase-2 brief):** F2 SEV-4: "189/189 + byte-identical header" does not prove slot 0x0F: slots are doc-only in the generator, the header cannot change, and SeatVerified checks the vtable address. Adequate for an observe-only chaining thunk (the Shield TASK2 precedent), but the phase-2 DENY at this seat alters the answer and must add an install-time guard (refuse unless the slot holds the disassembled base, or log-and-refuse a foreign hook), and the docs/author claim should say "vtable verified; slot 0x0F verified offline (doc row) and compared at install for the log".
  (Docs wording corrected on the branch: STATUS and the AiCastSeats.cpp seat comment.)
- **FIXED (docs, same branch):** F3 SEV-5: STATUS says "No engine call, lookup or allocation on the unclaimed path"; with any claim live the unclaimed path does 1-2 handle-table lookups per leaf act (the code comment's "no form lookup" is accurate). Wording.
- F4 SEV-5: the LEAVES summaries are outside the 6000-line cap (about 0.6 MB/h at four claimed followers in constant combat). Acceptable for a field cycle; noted.
- F5 SEV-5: RsKey = (actor<<32) ^ (kind<<28) ^ formId: the full 32-bit formId overlaps the kind bits, so EQUIP-LEAF keys for different kinds/forms can collide and share one 10 s rate slot. Logging only.
- F6 SEV-5: unreachActMs is set at act and cleared only at an observed pop; a pop on the denied-pair path would leave it stale (ran= too long). Unreachable today (CheckUnreachableTarget is never denied).


### APMF-B1 — `APMF_API.h`'s Repoint contract omits the FromPackage carve-out
- **Raised:** Fable review of `019e70e`/`22154fe`/`59764a8` (finding 2, MEDIUM). Left out of that branch deliberately and correctly — the header is byte-shared and append-only.
- **Severity:** SEV-3-equivalent as a DOCUMENTATION defect; ABI-neutral, comment-only.
- **Finding:** `native/APMF_API.h:600-602` still says *"A `param.form` different from the claim's current spell is REFUSED"*, with no `kCastFlag_FromPackage` carve-out — now imprecise after F5-2. A FromPackage client author reading it will conclude they cannot heartbeat at all (they never learn the extracted spell) and will fall back to lapse-and-re-request: the exact unclaimed-gap pattern the heartbeat exists to remove.
- **Suggested text:** *"On a `kCastFlag_FromPackage` claim, heartbeat with the PACKAGE you requested with; it is accepted as the same-form shape (the extracted spell is kept, never re-extracted)."*
- **Why it is still here:** it needs a LOCKSTEP identical edit in BOTH repos' copies, which must stay byte-identical (md5 `158acf8fc281e612f4b624656711e6d0`, `kABIVersion` 6). **Note the rule 9 carve-out: this is a byte-shared-header item, so it is NOT freely deferrable — it is open only because the MFO tree was held by another session, and it should be drained at the first opportunity.**
- **Assigned:** to ride MFO's queued native comment-only commit, or to be done as both halves at once.

### APMF-B2 — ch.15's probe re-equip is `External(APMF.dll)` on a ch.17 actor
- **Raised:** Fable tier-3 round 2 on `123d50e` (feat/equip-authority), SEV-5 #3.
- **Severity:** SEV-5.
- **Finding (verbatim):** "ch.15 probe re-equip (Equipment.cpp:90) classifies External(APMF.dll) and is denied on a ch.17 actor — document in MAP/INTEGRATION".
- **Reasoning:** `channels/Equipment.cpp`'s no-param (hotkey probe) path calls `ActorEquipManager::EquipObject` from APMF.dll itself, outside the ch.17 TLS bracket; on an actor holding a ch.17 declaration the seat classifies it `External(APMF.dll)` and refuses it unless the weapon is in the declared set. The param'd (gate-only) ch.15 form makes no equip and is unaffected. Documented in `Docs/INTEGRATION.md` ("Do not mix ch.15 with a ch.17 claim") and `MAP.md` EquipSink (10); the code is unchanged.
- **Assigned:** drain with the next ch.15 touch (either bracket the probe re-equip, or retire the hotkey probe path now that the gate-only form is the client contract).

### APMF-B3 — entry-detour target inside a trampoline page prints `detoured by ?`
- **Raised:** Fable tier-3 round 2 on `123d50e`, SEV-5 #6.
- **Severity:** SEV-5.
- **Finding (verbatim):** "detour target inside a trampoline page prints `detoured by ?` — following one FF 25 / 48 B8 hop would name the DLL".
- **Reasoning:** `core/EquipSink.cpp` `InspectEntryDetours` resolves the first hop's target to a module; a detour whose first hop lands in a trampoline page (SKSE's or the detouring plugin's) has no module, so the log names `?`. Following one further `FF 25` / `48 B8` hop from that page would usually name the DLL. Diagnosis fidelity only; the verdict (attribution degraded) is already right.
- **Assigned:** drain before the observe-mode field cycle if a `detoured by ?` line ever appears; otherwise with the next EquipSink touch.

### APMF-B4 — the `set truncated to 32` log fires at Apply, not at Enqueue
- **Raised:** Fable tier-3 round 2 on `123d50e`, SEV-5 #7.
- **Severity:** SEV-5 (accepted as-is by the reviewer: "truncation log at Apply is acceptable").
- **Finding (verbatim):** "truncation log at Apply is acceptable".
- **Reasoning:** round 1 asked for the once-per-handle truncation log at enqueue; it lives in `ControlMap::ApplySetEquipSet` because the once-per-handle state (`Claim::equipRequested`) lives on the Claim and the enqueue path is any-thread with no per-handle state. Recorded so the deviation is visible, not to be changed.
- **Assigned:** none; close when the file is next drained.

---

### APMF-B7 — the once-logged claim refusal reason can be stale after a later Install refusal
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "ControlMap.cpp:82-88 once-logged refusal reason vs a later Install refusal".
- **Reasoning:** `EnqueueRequest` logs the seat-not-installed refusal ONCE with `NotInstalledReason()` at that moment. A client that requests before kDataLoaded logs "not yet installed (Install runs at kDataLoaded)"; if `Install()` then refuses for another reason (INI off, site-verify), that reason is never printed by this line (the `[apmf][equip-sink]` install line still carries it). Fix shape: log once PER DISTINCT reason string, not once ever.
- **Assigned:** none.

- REOPENED 2026-10-06 (Opus spot-check): back to LIVE. It is the refusal logging in `ControlMap::EnqueueRequest` when the equip-sink seat is not installed: the claim/deny seat the weighting rework keeps, not the declaration road.

### APMF-B13 — ch.19 has no way for a client to learn what the claim is actually doing
- **Raised:** Fable tier-3 on `2d6108f`, SEV-4 (plus a related gap found in the same round); text as relayed by the coordinator.
- **Severity:** SEV-4.
- **Finding (verbatim, as relayed):** "observe-only ACCEPTS the claim and is inert with no way for a client to tell (ch.17 has IsEquipAuthorityEnforced; ch.19 cannot add a query without an ABI slot) PLUS the related gap that a client has NO way to learn the approach ENDED or was ABANDONED after the 120 s STUCK — record both as one entry for a later append-only query, do not add an API now".
- **Reasoning:** (a) RESOLVED 2026-09-23 by removing the mode entirely: marth ruled that an observe mode in Harbinger is a contradiction ("Harbinger accepts commands, the kill switch is not sending one"), so a claim can no longer be accepted-and-inert. (b) RESOLVED 2026-09-24 on `feat/apmf-travel-leg-state` (ABI v12): `GetTravelLegState` reports every leg end (arrived, blocked, combat, destination gone, stuck, actor gone, failed, released) with the owning claim handle. **CLOSED.**

- **Assigned:** the next ABI revision that adds a slot for another reason (append-only, never a v10 retrofit).

### APMF-B14 — the ported package accessor adds a static uid fallback MFO does not have
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "the uid-0 static fallback MFO lacks (name it in the port comment too)".
- **Reasoning:** `core/PackageData.cpp` falls back to UNAM uid 0 for the Travel template's `"Place to Travel"` when BOTH name maps miss; MFO's original carries static fallbacks only for the UseMagic template's SPELL (3) and Target (4) and none for a Location, so MFO declines where this port proceeds. Strictly additive and still guarded by the type-name check before any write, but the two implementations can behave differently on a both-maps miss, which matters when comparing their logs. The divergence is now NAMED in the port comment (done in this round); the entry stays open only for the decision of whether to converge the two.
- **Assigned:** whichever brief routes MFO's loot travel through APMF's copy (the two implementations merge there anyway).

### APMF-B16 — a leg whose ACTOR does not resolve still offers a package and nudges
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "an engagement whose ACTOR does not resolve still offers a package + nudge".
- **Reasoning:** `Engage` captures the actor handle only when `actor && actor->IsHandleValid()`, but it does NOT refuse the claim when the actor is unresolvable — so `Compose` can still point a package and file the ch.9 offer for an actor that is not there. ch.9's own `PostDeferredNudge` gate 1 then refuses to post the nudge (it logs "actor does not resolve"), so nothing reaches the engine and the per-frame monitor ends the leg on its first pass with "the actor no longer resolves". The cost is one wasted package slot for up to one poll period and two log lines, never a wrong actor being moved. Fix = refuse at `Engage` when the handle is invalid, the same shape ch.9 uses.
- **Assigned:** the ch.19 backlog drain (before the observe-only flip).

### APMF-B17 — the leg safety net is 120 s where MFO's equivalent is 60 s
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "120 s STUCK vs MFO's 60 s".
- **Reasoning:** `kLegMaxMs = 120000` was sized from "far longer than any legitimate cross-cell travel" rather than from a measurement, while MFO's loot-travel equivalent settled on 60 s after the navmesh-sticky field round (`memory/loot-multiminute-stall-navmesh-sticky`). Both are safety nets, so a too-long value costs only a slower FAILURE report, never a cut-short leg (CLAUDE.md principle 9: a floor is safe, an expiry is not) — but two numbers for the same job in two codebases is the kind of divergence that gets argued about later. Decide one from the first ACTIVE field log's real leg durations.
- **Assigned:** the first field cycle's log review.

### APMF-B18 — a package record's runtime Location handle is stale across a save/load
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "a stale PackageLocation.refHandle across save/load (same shape MFO ships)".
- **Reasoning:** `SetTravelTarget` writes a live `ObjectRefHandle` into the shipped record's `PackageLocation`. Handles do not survive a save/load, so a record saved mid-leg carries a dangling handle into the next session. In practice the leg cannot survive either — `ControlMap::Clear()` + `travel::ResetAll` drop every claim at the world boundary, so nothing offers that package again until a fresh `SetTravelTarget` overwrites the handle — and MFO ships exactly this shape in production. Still worth a deliberate decision rather than an inherited one: either clear the records' handles at the revert boundary, or write down why the overwrite-before-offer ordering makes it unreachable.
- **Assigned:** the ch.19 backlog drain (before the observe-only flip).
- **Partly addressed 2026-09-23 (`feat/apmf-position-cast-and-space-queries`):** a record that last named an APMF MARKER is pointed back at its PlayerRef placeholder at the load boundary and whenever that marker is deleted (`channels/Travel.cpp` `RestoreMarkerSlots` / `UnpointSlotsAt`). Records aimed at an ordinary reference are unchanged, so this entry stays open for them.

### APMF-B19 — the load-time marker sweep's proofs could match a different XMarker
- **Raised:** Opus 5.5 tier-A review of `ed729ec` (ABI v11 branch), finding F8(a); text as relayed by the coordinator.
- **Severity:** below SEV-3 (deferred by the coordinator as "negligible"; the relay did not restate the exact grade).
- **Finding (as relayed):** "co-save proofs matching a different XMarker, negligible".
- **Reasoning:** `SweepCarriedMarkers` deletes a recorded marker only when the recorded 0xFF FormID resolves, its base is the Skyrim.esm XMarker and it stands within 1u of the recorded position. A DIFFERENT XMarker could pass all three only if the engine recycled that exact 0xFF FormID for another runtime-created XMarker placed within 1u of the same spot, between the save and the load. Possible in principle, vanishingly unlikely, and the cost would be deleting one invisible marker another mod placed at that exact spot. A stronger proof would need a per-marker tag the engine keeps (none found) or a created-by record.
- **Assigned:** the ABI v11 backlog drain.

### APMF-B20 — a player-blamed position cast's location may shift
- **Raised:** Opus 5.5 tier-A review of `ed729ec`, finding F8(e); text as relayed by the coordinator.
- **Severity:** below SEV-3 (deferred by the coordinator; the relay did not restate the exact grade).
- **Finding (as relayed):** "player-blamed location shift".
- **Reasoning:** the cast core's Target Location arm compares the caster's OUT-ACTOR against the player and takes a crosshair pick for the player (AE `0x5bc3ec` -> `0x5c0470`). A marker caster reports no out-actor (NonActorMagicCaster slot 0x0D never writes it), which is why an NPC-blamed cast lands on the marker. Whether that still holds when the BLAME actor is the player (an API client naming the player as `actor`) was not traced, so a player-blamed position cast might land at the crosshair instead of the point. No client does this today (MFO blames followers).
- **Assigned:** the ABI v11 backlog drain; a one-line field check (position-cast with the player as actor, read where it lands) settles it.

---

### APMF-B32 (SEV-5) -- ch.23 leash over-denies back-off moves toward the anchor
Raised against `85ed6af` (`feat/apmf-pursuit-deny`), tier-A Opus 5.5 review 2026-09-25 (reviewer log `scratchpad/agentlogs/review-apmf-pursuit.md`), SEV-5 "over-deny retreat moves"; carried as closing-round F5. The leash rule compares the GOAL (the combat target) against the anchor, not the direction of the move. MaintainOptimalRange's back-off and Surround's Retreat path can move the actor away from its target and TOWARD the anchor; past the radius, with the target farther out still, they are denied too and the actor stands where it is. Reasoning: an over-deny, never a leak; the actor keeps attacking what it can reach. Documented in DENY-COMPLETENESS-AUDIT gap 14 (e) and INTEGRATION's leash limits. **Closure:** a direction-aware rule for these two leaves (read the move's destination, not the target), or measured field evidence that it does not matter.

### APMF-B33 (SEV-4 + SEV-5) -- Travel.cpp at the 2500 cap; probe 2's per-actor limit
Raised against `6883032` (`feat/apmf-gate-probe2`), tier-A Opus 5.5 review 2026-09-28 (reviewer log
`scratchpad/agentlogs/review-apmf-gate-probe2.md`). Verbatim from the reviewer log: "SEV-4: Travel.cpp
2482/2500" and "SEV-5: budget per-location x2 + vertex loop unbudgeted + per-actor (multi-follower same frame)".
- **Reasoning:** the budget half is fixed in the closing round (one triangle+vertex budget per probe). What
  remains: the file is at 2494 lines, so ANY further Travel work needs a Travel.cpp split first (its own tier-A
  brief). And the probe is rate-limited per ACTOR, so three followers blocked in one frame run three probes
  (each bounded; the field `us` figure will size whether a global limit is needed).
- **Assigned:** the Travel.cpp split brief (before further Travel work); the per-actor limit after the field
  cost numbers.
- **Update 2026-10-20 (`feat/apmf-ch19-cell-hold`):** marth relaxed the 2500 cap until the next major release
  (never split inside a task), so the cell-hold fix went into Travel.cpp unsplit. The file is now 2689 lines. The
  split is still owed as its own tier-A brief.

### APMF-B34 (SEV-4 / SEV-5) -- ch.12 idle entry confirmation, review round 1 items (deferred)
Raised against `18be6ec` (`fix/apmf-idle-confirm`), tier-A Opus 5.5 review 2026-09-29, round 1 (verdict FIX FIRST;
the SEV-2 and SEV-3 items were fixed on the same branch in `c5f435a`). Verbatim from the reviewer's log
(`scratchpad/agentlogs/review-apmf-idle-confirm.md`, "ROUND-1 FINDINGS (against 18be6ec)"):
- R1 SEV-4: carry on a same-idle re-play reports CONFIRMED (labelled carry) even if the re-play itself does nothing.
- R1 SEV-4: toNestedStateId (TransitionInfo +0x3C) ignored: entry set can be short (NO TRANSITION unaffected).
- R1 SEV-4: thin data behind 3000 ms (7 field IdleGive plays, entry time unknown in 1).
- R1 SEV-5: unhandled generator classes (BGSGamebryoSequenceGenerator, hkbReferencePoseGenerator) -> unknown -> incomplete (conservative); no SEH around engine reads (house style).
- **Fixed in round 1 (not deferred):** `Names::Get` null data check (`Names::Valid`).
- (This entry was first filed in `c5f435a` under the number APMF-B32, which collided with the existing ch.23
  entry; renumbered here.)

### APMF-B35 (SEV-3 OPEN + SEV-4 / SEV-5) -- ch.12 idle entry confirmation, review round 2 items
Raised against `c5f435a` (`fix/apmf-idle-confirm`), tier-A Opus 5.5 review 2026-09-29, round 2 (verdict MERGE:
nothing above SEV-3; R2-4 must be recorded OPEN). Verbatim from the reviewer's log, "ROUND-2 FINDINGS (against
c5f435a)":
- **OPEN (SEV-3, not fixed; the WEAK label does not close it).** R2-4 SEV-3: WEAK is a LABEL only; a confirmation on a shared event still counts as PLAYED and keeps the claim live. STATUS says both MFO idles' entry sets are entirely shared, so R1 SEV-3.2 (the false-CONFIRMED class of the 2026-09-28 bug) is narrowed and logged, not closed. Scenario: IdleLockPick accepted but not taken by the graph; within 3 s the follower leaves MT_State for another non-offset idle / furniture / dialogue idle -> IdleOffsetStop -> "ANIMATION CONFIRMED (WEAK ...)" -> MFO's lockpick claim stays live though nothing played. The code comment cites "review 18be6ec SEV-3.2" as handled, and APMF-B32 does not record it as open. Needs a backlog entry (rule 9) naming it open; a real fix would require a specific event or a state check (e.g. the graph's active state / the dest's enter event order).
  Note: the `Idle.cpp` header comment still cites "review 18be6ec SEV-3.2" next to the WEAK label; per this
  finding the label narrows and logs the false-CONFIRMED class, it does not close it.
- R2-1 SEV-4: hkbStateMachine's own event-driven fields (returnToPreviousStateEventId 0x6C, randomTransitionEventId 0x70, transitionToNextHigher/LowerStateEventId 0x74/0x78) change state on an event but are not read. Tux uses randomTransitionEventId in 4 SMs, all on events that also have TransitionInfos -> no false NO TRANSITION today. Scenario: a modded graph routes an idle's event only through such a field -> complete walk, 0 transitions -> NO TRANSITION -> claim ended. Fix: count an SM whose field equals the event as a transition (or as a blind spot).
- R2-2 SEV-4 (pre-existing design, not new in c5f435a): NO TRANSITION trusts the idle form's own animEventName. TESIdleForm kSequence / kParent / childIdles may make the engine play a child's event (SetupSpecialIdle behaviour unverified). Not hit by IdleGive/IdleLockPick (transitions exist). Fix: no NO TRANSITION for an idle with childIdles or kSequence (UNCONFIRMABLE), or verify in disasm.
- R2-3 SEV-4: the verdict does not re-check the actor is still loaded / high process / not ragdolled. Scenario: player goes through a load door 1 s after a follower's IdleGive and the follower is left behind (or unloads) -> 3 s of world time later NOT CONFIRMED "the graph never entered the idle" -> claim ends. Ending is right (the idle did not play/was cut) but the reason text misattributes; say "actor unloaded / left high process" instead.
- **FIXED (docs, same round):** R2-5 SEV-5: STATUS field check 2 says the play line shows "entry events [...]"; for IdleGive (source-exit only) EntryText prints "source-exit events [...]". STATUS field check 2 now names both forms.
- R2-6 SEV-5: Entry::weak is computed (Play) and never read: dead field.
- **FIXED (docs, same round):** R2-7 SEV-5: APMF-B32 says R1 SEV-5 items were "relayed without separate text" -> now in full above; B32 can cite this log. APMF-B34 now cites the log verbatim.

### APMF-B43 (SEV-5) -- per-classify cost of the claim gate on the NotHealShaped path (restore-serve exit logs)
Raised against `34d8a812` (`fix/apmf-restore-serve-exitlogs`), Opus 5.5 review 2026-10-01, finding F2. Reasoning (as given): the claim gate in `NotServed` does one `attackerHandle.get()` plus one RCU `TryGetCastSeatClaimForForm` on every unserved, non-heal effect classify. It is bounded and lock-free, but it does not decay for unclaimed spells, because they are never inserted into the dedup set.

### APMF-B44 (SEV-5) -- bad_alloc from the dedup insert or fmt::format on the combat thread (restore-serve exit logs)
Raised against `34d8a812`, same review, finding F3. Reasoning (as given): `std::unordered_set::insert` and `fmt::format` can throw `bad_alloc` on the combat thread. It is the same exposure as the existing `g_healLogged` insert in `ServeUnclassedHeal`, so not new in kind.

### APMF-B48 (SEV-4) -- a delivery-flip proxy may survive in a persistent follower's spell list across a load purge
Raised against `78b057c` (`fix/apmf-proxy-per-claim-refcount`), tier-A Opus 5.5 review 2026-10-05 (reviewer log `scratchpad/agentlogs/review-apmf-proxy.md`), finding F4. Text as relayed by the coordinator (the reviewer log holds only a summary, so the reviewer's own wording is not on disk): "F4 SEV-4: a proxy may survive in a persistent follower's spell list across a load purge, plus the test to verify it." Reasoning: on kPreLoadGame, `ReleaseAll` posts each claim's `castproxy::Unref`, and that post is then `Discard()`ed unrun. `castproxy::ResetAll` clears the forms' borrowed `Effect*` and nulls the slots, but it makes no engine call. So nothing un-teaches a proxy that was still taught when the load began. A persistent follower's actor survives the world swap, so its runtime spell list may still point at the 0xFF form when the load-time purge frees it. Test: hold a proxied ally-heal claim (MFO per-hand heal at an ally) on a persistent follower, then load a DIFFERENT save without saving first. Inspect that follower's spell list (ReSaver on a save taken right after the load) and check `APMF.log` for `[castproxy]` lines. A 0xFF entry, or a CTD on the follower's next spell-list walk, confirms it. Closure if confirmed: un-teach every owned slot in `ResetAll` on the kPreLoadGame path, while the outgoing actors still resolve, before the slots are nulled. (The revert path must stay call-free.)

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- one load per session in 5 sessions and no live proxy at load in any, so the purge path never ran; needed: load a save written while a delivery-flip proxy is live in a follower's spell list (no logging gap).

## DRAINED

_(none yet)_

### APMF-B21 (SEV-4) -- F1 self-check: "derived from our disassembly" is partly circular
Raised against the F1 consumer branches (MFO 8f16e69 / APMF aab20f2, fork 17184fb8), tier-A review 2026-09-24. The scratch builders (scratchpad/f1/gen/build_spec_lib.py: vtrow, fnrow) took each vtable row's RTTI name and each function row's signature at the Address Library's RVA. The committed generator re-finds every row by RTTI or unique signature before comparing with the library, and the reviewer confirmed every RTTI name matches its construct, so the table is sound; but the independence claim is weaker than written. Fix: generator asserts RTTI name == the construct's class; reword CLAUDE.md / Docs/VERIFIED-ADDRESSES.md from "derived without the library" to "confirmed against it".

### APMF-B22 (SEV-5) -- F1 review small items
Same review: (a) APMF EquipSink.Path.* rows (27/runtime) are checked but nothing consults them, and a failed row prints a misleading "those seats will not install"; (b) the fork refuses inconsistently on an unverified build (ControlMap/CombatController GetRuntimeData and ExtraDataList::GetRuntimeSize are fatal, DOBJ and GetInputContext soft), unreached today; (c) AE kFavor -> 17 rests on controlmap.txt block order, not disassembly, unused today; (d) the gate is keyed by address not by row; (e) the fork's Actor::GetGoldAmount logs an error on every call when Gold is null (unused by us).

### APMF-B23 (SEV-5 x3) -- F1b review items
Raised against fork bf7e9a5d/fde0f3ae + consumer build/commonlib-f1b, tier-A review 2026-09-24 (verdict: merge-ready, nothing above SEV-5). (1) Fork TESForm.h:215 comment says the form-map pointer is "set once at startup and cleared only at shutdown": wrong on AE, where 36601 @0x648CD0 frees and reallocates both map pointers unlocked on a full data reload (probably main-menu reload); SE frees them in the TESForm dtor when a live-form counter hits 0. Pre-existing, the engine's own lookup has the same exposure: fix the comment; exposure is an MFO worker lookup during that reset. (2) A lookup can now Sleep(0)/Sleep(1) while an engine writer holds the lock (map grow/rehash, mostly at load); at MFO APMFBridge.cpp:1107 the worker would hold g_mx while sleeping, in Loot statics a cell spinLock. Bounded and rare. Optional: move the APMFBridge lookup above scoped_lock(g_mx). (3) The two self-check rows for LockForRead/UnlockForRead are log-only; on a mismatch Runtime.h:84/91 would still print "REFUSED ... those seats will not install" though LookupByID keeps calling them. Reword for log-only rows.

### APMF-B24 (SEV-4 / SEV-5) -- ABI v12 travel leg-state review items (deferred)
Raised against `2eca3ec` (`feat/apmf-travel-leg-state`, loot round A1), tier-A Opus 5.5 review 2026-09-24 (reviewer log `scratchpad/agentlogs/review-a1.md`). Fixed in the same round: F1-F7, F11. Deferred:
- **F8 (SEV-4, benign, logged).** A live re-point that changes the gait rewrites the record's flag and speed byte while another thread could be starting that package; the two writes can tear (flag new, byte old or the reverse). Reasoning: the record is owned by one leg, a fresh leg writes before its offer, and a live gait change is already logged as not applying until the next package start; a tear only picks one of two declared-or-authored speeds for one start.
- **F9 (SEV-5).** `GateProbe` reaches `TES::GetSingleton()` (a data id) without its own verified row. Reasoning: same exposure PositionCast already has; `TES::GetCell` itself is a verified row. Fix with a data-row kind in the generator if one is ever added.
- **F10 (SEV-5, offline only).** The `Travel.ScriptEventSourceHolder.GetSingleton` signature is 150 bytes and runs past the function's end through the int3 padding into the next prologue. It is unique and verified; it is fragile only if the neighbouring function changes, which cannot happen on these two frozen builds. Fix: cap signature growth at the first `ret` + padding in `make_sig`.
- **Package-start override (SEV-5, note).** The package-start copy (1.6.1170 0x6CE2C0 / 1.5.97 0x63BD40) applies an optional override struct (rdx): when its flags carry 0x2000 it re-sets ActorPackage +0x2B from its own byte (+0xC). A caller passing such an override would replace the record's gait on that start. Not observed; `[travel-gait]` shows the running copy if it ever happens.
- **MFO-side note for M2 (not this repo).** MFO asks `APMF_GetInterface(kABIVersion)` once with no downward retry (INVARIANTS #14b), so mirroring v12 would lose all of Harbinger against a 0.9.7 APMF. M2 should request the mirrored ABI, then retry at MFO's minimum, and gate v12 features on `abiVersion`.

### APMF-B25 (SEV-4 / SEV-5) -- ABI v12 travel leg-state, review round 2 items (deferred)
Raised against `8d6cc26` (`feat/apmf-travel-leg-state`), tier-A Opus 5.5 review round 2, 2026-09-24 (verdict: merge-ready after R2-1, fixed in the same round). Deferred:
- **R2-2 (SEV-4).** A refused re-point frees the package slot one frame before the ch.9 release applies. `OnOwnerChanged`'s `refuse` calls `EndLeg` synchronously inside `Drain`; `DropOfferAndFreeSlot` posts `FreeSlot`, which runs in THIS frame's Pump, while the `EnqueueRelease` it queued lands in the NEXT Drain. For about one frame another actor's Compose can take the slot and re-point the record while the outgoing ch.9 claim still offers it. Fix later: post the refusal teardown one hop the way `Release` does (`Release` posts `DropOfferAndFreeSlot`).
- **R2-3 (SEV-4, documented this round).** A `kLeg_Failed` recorded for a refused re-point that arrived with an owner change names the PREVIOUS owner's handle, because the new claim is not yet published inside `Drain`. The header's `ownerHandle` comment and INTEGRATION now say so, as for the Pending case. A code fix would read the owner one hop later.
- **R2-4 (SEV-5).** `refuse` records two `kLeg_Failed` states back to back (EndLeg's, for the old destination, then the refused destination's), so `seq` moves twice and the log shows two `[travel-state]` lines. Harmless; the final state is the refused one.
- **R2-5 (SEV-5).** `FindBlocker` keeps a raw `RE::Actor*` (`best`) after the `ForEachHighActor` callback's `NiPointer` scope has ended. Main thread, used within the same Poll only for FormID / name / teammate reads, so the actor cannot be freed in between in practice; fix by holding a `NiPointer<Actor>`.

### APMF-B26 (SEV-4, threading carve-out, ACCEPTED by decision) -- ch.20 seat vs a cross-thread StopCombat
Raised against `5dcefa7` (`feat/apmf-target-pin`), tier-A Opus 5.5 review 2026-09-25, finding F2 (reviewer log `scratchpad/agentlogs/review-apmf-target-pin.md`). The ch.20 seats run inside the actor's own `UpdateCombat` job and read `combatController` (and, since the source-deny rework, its `combatGroup` under the group's read lock). A `StopCombat` on ANOTHER thread against the same actor in that window (a script's `Actor.StopCombat`, a package evaluation, a kill -- MFO ENGINE_NOTES 0.47 item 4) frees the controller inline, with no refcount and no lock, under the read. Decision (coordinator, 2026-09-25): accepted as a known exposure, identical to vanilla's own `UpdateCombat` body and to MFO's Targeting hook. It is written into `channels/TargetPin.cpp`'s header. **Closure:** a `StopCombat` seat (Character vtable slot 0xE5, 1.6.1170 `0x6B70A0` / 1.5.97 `0x625920`) sharing a per-actor lock with the combat seats -- assigned to the combat-substrate task, not this repo's ch.20 work. **Another reader at this seat (2026-10-19, against `3b4272aa`):** the combat-substrate bystander census (`CombatSubstrate.cpp` `BystanderCensus`, Poll on the main thread in `Arbiter::OncePerFrame`) reads `combatController`, `cc->targetHandle`, `cc->combatGroup` (by address only) and the second-level `cc->state->isFleeing` / `isDetected` of NON-teammate actors near a holder, the same exposure class as this entry and with one extra dereference (`cc->state`) and a wider scope (any actor within 2048 u). Accepted with it; passive, logs only.
**Update (fix/apmf-anchor-leak, ace1d8b):** a new game-thread reader AND writer at this seat: the anchor-leak sweep (`channels/AnchorGuard.cpp` `Sweep` / `Treat` / `AfterClose`, from `combatsubstrate::Poll` and `CloseSub`) reads every high actor's `combatController` and its `combatGroup` (targets / members under the group's read lock) every frame while a substrate is live + its tail, and calls `CombatGroup::RemoveTarget` and the ranked `StopCombat` (+ `EvaluatePackage(false,false)`) on NON-holders. Same exposure class: a cross-thread reader of that actor's controller can see it freed by the sweep's StopCombat, as with every other game-thread StopCombat here.
**Update (feat/apmf-substrate-r5-denies, review F7 of e6f5607c, SEV-5):** the substrate's aggressor clear (`channels/SubstrateAggressor.cpp` `AfterEntry`) calls the engine's own aggressor setter (AE 39145 / SE 38185 / 1.7.104 0x6E9860) on the game thread once per entry: a non-atomic read-modify-write of `AIProcess+0x136`, so an engine write of another bit of that byte on another thread at the same instant can be lost. The same exposure the engine's own StopCombat has from the main thread; one call per substrate.

### APMF-B28 (SEV-5 x2) -- ch.20 review small items (c) and (d)
Raised against `5dcefa7`, same review, SEV-5 items (c) and (d). Verbatim: (c) "The seat calls `spdlog::info` while holding the shared lock. This happens once per engagement and causes no lock inversion." (d) "Pin counters are only reported on Release. On revert, `ResetAll` prints a count only." Reviewer note on the re-review of `9afa8d4`: (c) is now MORE FREQUENT -- the not-a-combat-target line is logged up to every 5 s per pin while `g_pinMx` is held shared -- and (d) still applies. Items (a) (INVARIANTS #2 wording) and (e) ("6 conditions") were fixed in round 2.

### APMF-B29 (SEV-5) -- ch.20 seat nests the group lock inside g_pinMx
Raised against `9afa8d4` (`feat/apmf-target-pin`), tier-A Opus 5.5 re-review 2026-09-25, finding F-C. The selector seat holds `g_pinMx` SHARED while it takes the combat group's read lock (`BSReadLockGuard(group->lock)`). Reasoning: no current path takes them in the opposite order (the game thread's unique_lock sections make no engine call), so there is no inversion today, but nesting a foreign lock inside ours is avoidable. Fix: copy the handle (and target FormID) out, release `g_pinMx`, do the group read, then re-take the shared lock to update the counters after re-checking the entry.

### APMF-B30 (SEV-5) -- ch.21 "combat ended" misses a StopCombat + StartCombat inside one Poll window
Raised against `ac895c3` (`feat/apmf-combat-entry`), tier-A Opus 5.5 review 2026-09-25, finding F6 (reviewer log `scratchpad/agentlogs/review-apmf-combat-entry.md`). Verbatim as relayed by the coordinator: "a StopCombat and StartCombat inside one Poll window is missed; harmless". Reasoning: `combatentry::Poll` samples the actor's `combatController` POINTER every 250 ms; if the fight Harbinger entered is stopped and a new controller is built (by anyone) between two samples, the pointer is never seen null, so the claim is not ended with "combat ended" and stays live over a fight it did not start. No engine write depends on it (ch.21 makes no further call on its own), so the only effect is a claim that ends later than it could.

### APMF-B31 (SEV-5) -- ch.21 a brief 3D loss of the target ends the claim
Raised against `ac895c3`, same review, finding F7. Verbatim as relayed by the coordinator: "a brief 3D loss ends the claim; the same residual as ch.20". Reasoning: `combatentry::Poll` ends the claim on `!target->Is3DLoaded()` checked live on the game thread; a transient 3D rebuild (a transform, a skeleton swap, Reset3D, a script Disable+Enable) seen by one poll ends it. ch.20 has the same residual (its Poll re-checks live, which narrows but does not remove it). The client re-requests.

### APMF-B45 (SEV-4) -- FIXED in F2b (`feat/apmf-1.7.104`, 2026-10-05) -- claims are handed out on an unsupported runtime where nothing will ever drain them
Raised against 53555c9 (`fix/apmf-g1-exact-gates`, Opus tier-A review), 2026-10-05. Reviewer (verbatim): "On an unsupported runtime (VR before this change, now also any other 1.6.x/1.5.x and 1.7.104 until F2), ClientAPI still hands out claim handles for every intent. Nothing will ever Drain or Engage them, because the 0xAD Hook is refused." Fix when drained: refuse claims at the API (kInvalidHandle, as EquipAuthority already does) or expose a runtime-supported query when 0xAD is not installed. Also SEV-5 notes: the MovementDeny ch.1 Engage/Release gates are an unreachable backstop (say so in the docs), and Travel.cpp:2105 `g_gaitVerified` is now always true after the Install gate.
**Fix (F2b):** `ControlMap::EnqueueRequest` / `EnqueueCast` return kInvalidHandle (logged once) when
`allowance::RuntimeSupported()` is false or `hook::RefusedReason()` reports the 0xAD seat refused (VR, runtime,
self-check). The two SEV-5 notes stay open (ch.1's gates and `g_gaitVerified` are still unreachable backstops).

### APMF-B46 (SEV-5) -- EquipGate CallSiteName never matches a return address
Raised against 443b172 (`feat/apmf-1.7.104`, Opus tier-A review, verdict MERGE), 2026-10-05. Reviewer finding (as relayed
by the coordinator): "EquipGate.cpp:197-219 vs :269: `CallSiteName` compares a `_ReturnAddress()` RVA against
call-site/function-start literals and never matches. This predates the branch and was copied into the 1.7.104 arm. Log
labels only." Reasoning: the table holds the `call [rax+0x78]` instructions (0x813AF2 ...) and 44868's start (0x80FCD0),
not return addresses (site+3; AiCastSeats' kRetAE has the right shape), so every line logs "unknown". Nothing gates on
the label. Fix when drained: use the return addresses (site+3) on 1.6.1170 and 1.7.104 and re-derive the pre-loop entry.

### APMF-B47 (SEV-5) -- a claim made before kDataLoaded can outlive a refused 0xAD install
Raised against 443b172 (`feat/apmf-1.7.104`, Opus tier-A review, verdict MERGE), 2026-10-05. Reviewer finding (as relayed
by the coordinator): "ControlMap.cpp NoDrainReason/RefuseNoDrain (~:97-117): a client that claims before kDataLoaded
`hook::Install`, followed by a refused install, leaves a handle that never drains. Document it." Reasoning: B45's fix
refuses at the request on an unsupported runtime (known from load) and after `hook::RefusedReason()` is set; on a
SUPPORTED runtime a claim queued before kDataLoaded is accepted (it will drain once the hook installs) and, if the 0xAD
install is then refused by the self-check, stays undrained. Documented here; not fixed in F2b.

### APMF-B49 (SEV-3/4/5) -- proxy refcount round-2 review follow-ups
Raised against e2a77a8 (`fix/apmf-proxy-per-claim-refcount`, Opus tier-A re-check MERGE), 2026-10-05.
- SEV-3, pre-existing, DECK CHECK BEFORE RELEASE: PreSaveSweep likely runs too late to keep proxies out of the .ess. SKSE runs plugin save callbacks from SkyrimVM::SaveGlobalData (globalDataTable3, type 1001), which the .ess places after changeForms, so the actor change form (with the AddSpell'd 0xFF proxy) may already be captured. INVARIANTS #19's "sweep before a record is written" premise may be wrong. Verify: save with a proxied ally-heal claim live, ReSaver-dump the follower's spell list, look for a 0xFF entry. If present: the sweep needs an earlier seat (before change-form generation), its own tier-A item, before release, with APMF-B48. If clean: close.
  EXTENDED (feat/apmf-self-delivery-proxy review F1, SEV-3, raised against 2c8c151, 2026-10-06): the same save must ALSO dump the ACTIVE-EFFECT list of the caster (and of any ally that received a proxied buff). Since the self-flip proxy a buff whose `ActiveEffect::spell` is a 0xFF proxy form sits on the CASTER for its whole duration (the forward flip already left such effects on allies); `PreSaveSweep` un-teaches the spell but cannot touch an applied effect. Verify: hold a self-flip claim (e.g. MFO's Fade Other / Oakflesh-style aimed or touch buff at the follower itself) until it lands (`[castproxy] ... LANDED on the caster`), save, ReSaver-dump the follower's active effects: an effect whose spell is a 0xFF form that survives the reload (or a load error / CTD on it) confirms. Reviewer reasoning: the save path for a 0xFF unregistered spell inside an ActiveEffect record is untraced; deck check before release.
- **DRAINED (e32a999, `fix/apmf-post-0.11.0`):** SEV-4: TryGetCastSeatClaimForForm returns the first matching claim in vector order, not the BetterClaim winner. Diverges only when a later same-hand same-spell claim has a strictly higher basis (never under MFO's uniform kOwnBasis). Fix: pick the BetterClaim-best among matches.
- **OPEN, skipped in the e32a999 round (freeing a form whose FormID clashes could drop the other holder's map entry; the free API is unverified):** SEV-5: the refused-mint path (zero/duplicate FormID) drops the created form without freeing it (leaks until load purge; error-logged every time).
- **DRAINED (e32a999):** SEV-5: the F2 log says "plain deny of its hand", but AllowedCastForHand (Allowance.cpp:148) still admits the original kSelf spell on that hand, so the AI may still self-heal by its own vanilla choice. Nothing is forced. Wording only.
- Note: the save callback is main-thread synchronous per SKSE64 source (Hooks_Papyrus.cpp SaveGlobalData_Hook); the new PreSaveSweep "on the Drain thread: yes/NO" line confirms it in the field. A NO is a SEV-1 threading finding.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- 0 'minted proxy form has FormID' errors across 51 mints (no failure seen), but the drain-thread question is unanswered: PreSaveSweep prints only when swept>0 and no save had a live proxy; needed: save with a live proxy (logging gap: PreSaveSweep line should print thread id even when swept==0).
### APMF-B50 (SEV-4) -- awareness: the detection-event read vs its free on an untraced path
Raised against 6d84bab (`feat/apmf-own-los-awareness`, Opus tier-3 review, verdict MERGE), 2026-10-05. Finding (verbatim
as relayed by the coordinator): "the detection-event UAF exposure on the untraced ProcessLists path". Reasoning: SenseActor
copies `HighProcessData+0x3D8`'s 0x18-byte DetectionEvent on the main thread; the event is freed only by AE 0x6DFAF0, called
only from 41406 (high-process teardown), reached from 40744 <- 35652 / 40438 / 40745 (ProcessLists-side process-level
changes), whose thread was not traced. Fix when drained: trace 35652 / 40438 / 40745 to their threads; if any is off the
main thread, read the event under the engine's own guard for that path or drop the noise route on 1.6.1170 / 1.5.97 /
1.7.104 until it is. `core/Awareness.cpp` LookAtNoise.

### APMF-B51 (SEV-4) -- awareness: the hearing `level > 0` filter may drop hits
Raised against 6d84bab, 2026-10-05. Finding (verbatim as relayed): "the hearing `level > 0` filter (hits pass 0; confirm in
the field)". Reasoning: AE 34993 calls SetActorsDetectionEvent with the level from `0x416070(0)` (SOUND_LEVEL 0 mapped
through a global); if that maps to 0, every such noise is filtered out as "silent". Fix when drained: read the `[aware]
noise` lines of the first field run (they print the level of every new noise), then decide whether level 0 counts.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- [aware] saw 10 SenseActor calls, 'new engine noises seen 0' and 'noise lvl -1' on all 55 senses lines; needed: a session with real hearing events near a follower (logging gap: noise line prints only on a NEW stamp after a baseline, Awareness.cpp:123-148).

### APMF-B52 (SEV-4) -- SpaceQuery reads a skipped pick as "no hit"
**DRAINED (e32a999, `fix/apmf-post-0.11.0`).** `SpaceQuery.cpp` `Cast` checks `unkC0` and every query returns `kQuery_Failed` with a logged reason (no ABI change). MFO's Sightline gap is MFO-side, still open.
Raised against 6d84bab, 2026-10-05. Finding (verbatim as relayed): "PickObject's early-out already read as \"clear\" by
`SpaceQuery.cpp:71` on main". Reasoning: `PickObject` (bhkWorld slot 0x33) can return WITHOUT casting and set
`bhkPickData+0xC0` (its only writer in the function, all three builds); `spacequery::Cast` reads `rayOutput.HasHit()` only,
so a skipped walk / ground / clearance ray reads as clear (a false Ok / a false NoGround). core/Sightline.cpp already
treats it as UNAVAILABLE. Fix when drained: check `pick.unkC0` in `Cast` and fail the query (a new status is ABI: reuse
kQuery_Failed or kQuery_NoWorld with a logged reason). MFO's own Sightline has the same gap (MFO side).

### APMF-B53 (SEV-5 x6) -- own line of sight / awareness small items
Raised against 6d84bab, 2026-10-05, the review's SEV-5s (verbatim as relayed): "pin cold start, `TableFull` never emitted,
GetBound scratch statics, the stale noise baseline, the seat-coverage doc wording, and the ControlMap.cpp split proposal".
Notes: (a) pin cold start -- a kTargetPin_OwnLineOfSight pin pauses for the first frame or two until its pair's first
verdict; (b) `kLosWhy_TableFull` is declared but `GetLineOfSight` never reports it (a full probe window reads Unknown);
(c) GetBoundMin/Max (Character 0x73/0x74) write engine static scratch globals, so a concurrent engine call can tear the
height/margin read (ray endpoints only); (d) a noise baseline taken long ago stays the baseline -- an actor not queried for
up to 60 s keeps an old stamp, and a single new noise after that is heard with an age measured from Harbinger's look, not
the noise; (e) the seat-coverage wording in the docs (which caster vtables the 0x06/0x07 gate covers) should say Restore
and Offensive only; (f) ControlMap.cpp is 2250+ lines: propose its split as its own round.

### APMF-B54 (SEV-4) -- own LoS margin: hit-body identity and the BBX-contains-capsule assumption
Raised against 44ee2e2 (`feat/apmf-own-los-awareness`, Opus round-2 re-check MERGE), 2026-10-05.
- The per-target end margin (bound half-diagonal x GetBaseHeight + 16, clamp [48,1024]) ignores any obstacle within the margin of the target, and a viewer inside it reads VISIBLE through a wall. Errs toward VISIBLE only, only inside the target's own bound; humanoids keep the field-proven 48u. Proper fix: cast to the point and treat a hit as clear only if the hit body is the target's own collidable / group.
- The bound source (middleHigh->unk180, centre +0x18 / half-extent +0x24, a BSBound BBX shape) is proven; that a modded creature's char-controller capsule fits inside its BBX is not. Field check: [los] margin on giants / dragons in the open should read VISIBLE.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- margins seen 48..492u, and 0x31108656 (492u) was VISIBLE from 3 viewers at APMF.1 12:43:56-58 then UNAVAILABLE not loaded; needed: a hit-body case (logging gap: [los] lines carry no name/race/hit-body identity).

### APMF-B58 (SEV-4 x3) -- buff / summon / rowless cast seats
Raised against 3903988 (`feat/apmf-buff-summon-seats`, Opus tier-3 FIX FIRST), 2026-10-05. Ids chosen after
`feat/apmf-combat-moveto`'s APMF-B55..B57 (that branch is not merged yet): the merge must keep both sets distinct.
Verbatim:
- (a) "Reanimate hand-hold when no corpse (Q5). Unseated; target-0 claim gets 0x0F YES + deny-complete on that hand;
  Reanimate's own 0x06 corpse search says NO, so the hand sits on Raise Zombie until TTL. Bounded and correct by
  contract (WHETHER is the engine's); MFO's never-fired release covers it. Should be documented as an expected
  'BUILT NOT FIRED'." -- Documented in STATUS (the buff-seats HEAD OF WORK field list).
- (b) "Mixed-hostility spells: SpellRowless re-evaluates every effect under the current self flag
  (CastClassify.cpp:347-358) but seat 0 sets the flag per-effect as visited (:536), so an earlier hostile effect
  that natively keyed a row under self=0 can be judged rowless under self=1 -> Script could compete with the native
  row. Rare."
- (c) "Summon-plus-other-effect spells: any spell with a Summon effect makes the whole claim NativePlacement even if
  it keyed into a different caster (e.g. Armor). Mod-only." (Since the round-2 fix NativePlacement also requires
  `IsPlacementSpell`, which is the same whole-spell test, so (c) stands as written.)
Reviewer reasoning: all three are bounded and rare; none exercised by the vanilla kinds of the next field cycle.
- SEV-4 (round-2 re-check, e60f64f): a pooled proxy slot re-pointed to another source spell can leave a stale active effect whose `spell` is that same slot form (e.g. a Candlelight proxy effect still on ally A after the slot now carries Oakflesh). AlreadyApplied then answers NO for up to that effect's remaining duration; the claim stands unfired, bounded by TTL and the client's never-fired release.
- SEV-4 (round-2, e60f64f): a claimed ward omits the native Ward 0x07 no-threat grace, so it stays up while claimed and oscillates around the 25% magicka floor. Bounded by the floor and TTL.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- slot forms were reused only for the same spell (0xFF000A3D / 0x2F3B8 x2) and ctcensus claimed Ward built=0; no log names an AlreadyApplied NO and the stale-proxy/ward-oscillation case did not occur; needed: a ward re-cast on an already-warded follower (logging gap: ctcensus says only NOT BUILT, never AlreadyApplied).

### APMF-B59 (SEV-5 x3) -- buff / summon / rowless cast seats, small items
Raised against 3903988, 2026-10-05. Verbatim:
- (d) "Script caster 0x0C (AE 0x823080) writes the actor-wide Script restrict-timer blackboard key (the same key the
  Script item's 0x0F helper 0x822E50 reads) with the claimed effect's aiDelayTimer -> native Script-row spells
  delayed by that (usually 0)."
- (e) "ServeUnclassedHeal double-keep (author's out-of-brief finding): KeepBest always adds to +0x48 sum which
  becomes the item score (+0x3C via 0x8133A0..0x8133F0), so Close Greater Wounds' item score roughly doubles -- only
  for the claimed driven form whose hand is already 0x0F-YES/deny-complete. Harmless. Banner premise should be
  corrected." -- Banner corrected in round 2 (`core/CastClassify.cpp` UNCLASSED HEAL comment); the double keep stays.
- (f) "Rowless item score = sum of aiScores, 0 for many vanilla MGEFs; whether a score-0 item is ever picked is a
  field question (STATUS's 'NOT BUILT with sub[Script]' path covers it)."
- SEV-5 (round-2, e60f64f): with permanent Magicka 0, native 0x666F60 returns a constant while the copy treats it as low (unreachable: no ward at 0 max Magicka).
- SEV-5 (round-2, e60f64f): AlreadyApplied is a proven member-level copy of AE 0x81E6C0 / 0x81E400 because ids 45349 / 45344 are absent from the fork's 1.7.104 table. Adding them at the next fork table revision would let it call the engine directly.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED (part f only: works as intended) -- Cicero 'Arcane Pull' FE204ACE predicted Script, 'engineScore=0.000 STEERED->1000' at nAPMF 14:25:27.488, FIRED via Script 14:25:40 (5 FIRED total across sessions); the other parts were not exercised.
### APMF-B55 (SEV-4) -- ch.24: the pursuit-leash test is coarse
- **DECIDED + FIXED 2026-10-11 (marth: clamp a goal point that grazes the leash edge to just inside the leash):** `channels/CombatApproach.cpp` Poll moves the goal point onto the anchor -> X line, `kLeashClampMargin` (16u) inside the leash, when X itself is outside the ch.23 leash but the R-disc still touches it; `dist` and the written bound use the clamped point; the [ch.24] line says CLAMPED. ClickUp 86e3m3pzz.
Raised against 274d7e4 (`feat/apmf-combat-moveto`, Opus tier-3 review), 2026-10-06. Finding (verbatim as relayed): "F5 SEV-4
leash coarse". Reasoning: `channels/CombatApproach.cpp` Poll holds a claim as kApproachState_Leashed only when NO point within R
of X lies inside the ch.23 leash disc (dist(X, anchor) - R > leash radius). A goal disc that only grazes the leash still applies
the bound, so the engine's path back can leave the leash on the way, and the actor can stop at a point of the disc outside the
leash. Fix when drained: decide with marth whether the bound should require X itself inside the leash, or clamp the centre to
the leash disc.

### APMF-B56 (SEV-4) -- ch.24: the save-path restore thread
Raised against 274d7e4, 2026-10-06. Finding (verbatim as relayed): "F6 SEV-4 save-thread assert". Reasoning: RestoreBeforeSave
(kSaveGame) reads each actor's controller and areas array and writes area fields; the save runs on Main::Update's save path
(AE Main::Update 0x645EA0 @0x6468CF -> 36601 -> 35732 -> Save_Impl 35727), the thread of the engine's own
CombatController::SaveGame, but no job-graph barrier against the UpdateCombat jobs was traced (ENGINE_NOTES 0.47 NOT FOUND (a)).
DONE in round 2: a once-per-session warning when kSaveGame arrives off `apmf::hook::OnMainThread()`. Still open: the barrier
itself; read the field log for the warning.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- 0 [ch.24] save lines in all sessions; RestoreBeforeSave returns early when g_touchedCount==0 so the thread check only runs with a live bound at save; needed: save while a ch.24 bound is live.

### APMF-B57 (SEV-4) -- ch.24: FindWeapon looks only inside the bound
- **DECIDED + FIXED 2026-10-11 (marth: a disarmed follower's weapon search is exempt from the heal-approach bound):** an S5 deny bracket on CombatBehaviorFindWeapon act() (slot 0x02; AE 0x864040 tail-jumps 0x8610C0 which holds the two IsInCombatArea calls; SE 0x7CCFB0 -> 0x7C9A40). Caveat: the engine's PATH to the weapon stays confined by the bound, so a weapon beyond R may be found but not reached until the claim ends; field-unproven. ClickUp 86e3m3q07.
Raised against 274d7e4, 2026-10-06. Finding (verbatim as relayed): "F7 SEV-4 FindWeapon effect undocumented". Reasoning:
CombatBehaviorFindWeapon (AE 0x8610C0, IsInCombatArea x2) is the Acquire Weapon link of the engine's MOVEMENT half, so it carries
the bound: while a ch.24 claim stands a disarmed actor only considers dropped weapons within R of X. Documented in
`Docs/INTEGRATION.md` (ch.24 contract) and `Docs/DENY-COMPLETENESS-AUDIT.md` row 24 (a) in round 2. Open only if marth wants the
weapon search bracketed like the attack pick.

### APMF-B60 (SEV-4) -- ch.24: heavy slow time can stretch S1 past the stall floor
Raised against acb832b (`feat/apmf-combat-moveto`, Opus round-2 re-check), 2026-10-06. Finding (verbatim as relayed): "R2-3
(heavy slow-time stretching S1's period past the 3 s stall floor)". Reasoning: S1's 0.25 s period is GAME time; the stall
floor (kStallMs 3000) counts on the world-tick clock, which advances with real frames (clamped 100 ms each). Under a strong
slow-time effect (a slowdown shout, a mod's time scale) twelve game-time periods can exceed three world seconds and a live
claim ends kApproachState_EngineDropped falsely. Fix when drained: scale the floor by the game's time multiplier, or count
missed S1 periods in game time.

### APMF-B61 (SEV-4) -- ch.24: the F3 put-back waits on the target selector; its exclusive-lock scan
Raised against acb832b, 2026-10-06 (the re-check's spot-1 items). (a) The put-back of a finished claim's area runs only from
the CombatTargetSelectorStandard slot-6 seat, so an actor whose standard selector is not run keeps its area until the save
restore or the 60 s world-time purge. (b) While any record is marked for put-back (up to 60 s), every SelectTarget of EVERY
actor takes g_areaMx EXCLUSIVE and scans the table; bounded by the table size (a handful), but a contention point on the
combat jobs. Fix when drained: pre-filter on the controller (a lock-free set of pending controllers), shared lock for the scan.

### APMF-B62 (SEV-4 x2) -- self-flip proxy: pool pressure; the forward flip's rationale
Raised against 2c8c151 (`feat/apmf-self-delivery-proxy`, Opus tier-3 review, MERGE), 2026-10-06. Ids chosen after
APMF-B61 (other open branches may also take B62: renumber at merge if they collide). Verbatim as relayed:
- F5 (SEV-4): "self buffs now share the 8-slot pool, and overflow becomes a plain deny." Reasoning: before the
  self-flip an explicit self kTargetActor claim needed no slot; now every aimed / touch / target-actor self buff takes
  one of the 8 slots (keyed per owner, spell, hand, flip), next to every ally heal / ally buff. On overflow the claim's
  target handle is left invalid and the claim stands as a plain deny of its hand (loud `pool overflow` +
  `no self-flip proxy could be had`). Fix when drained: size the pool from the field's peak concurrent proxies, or
  per-owner slots.
- F6 (SEV-5, docs; fixed in round 2 where the reasoning is written): the forward flip's stated reason -- "a kSelf spell
  lands on the caster whatever the seats say; FindTargets' Self branch never reads desiredTarget" -- is false on all
  three builds. The Self branch (AE 0x5BC98A / SE 0x54D508 / 1.7.104 0x5CB4D1) takes MagicCaster::desiredTarget
  (+0x20) when it resolves to an Actor, else the caster; seat 0x0A feeds desiredTarget (AE 0x89EE30 calls the combat
  caster's 0x0A, resolves the ref and passes it to 0x5BB720, which calls SetDesiredTarget 0x5BE720 at 0x5BB848).
  OPEN QUESTION left for a later cycle, not answered by either round: whether an UN-proxied kSelf spell claimed at an
  ally would therefore land on the ally. The forward proxy stays because it is the field-proven road (ally heals,
  2026-09-05/06); no code depends on the false premise.
- RESIDUAL of the round-2 gate (raised against e82107b, 2026-10-06): the self-flip refuses any hostile or detrimental
  base effect and the Calm / Frenzy / Demoralize / Paralysis / Stagger archetypes. A MOD damage / debuff spell whose
  effects carry neither flag and a plain ValueModifier archetype (e.g. an unflagged Damage Health) cannot be told from
  a buff by archetype, so an explicit self claim on it would land on the caster. Closing it needs a magnitude-sign /
  AV-direction test or the client's own declaration; the client owns the consequence meanwhile.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED (pool-pressure part only) -- only owner 0x750012C6 ever minted, peak concurrent 2 (nAPMF 14:44:17, APMF.1 12:46:35) and 0 'pool overflow' lines; the forward-flip rationale question is not answerable from logs and stays open.

### APMF-B64 (SEV-4) -- hand-claim block: a refused ch.17 equip is never re-issued
**DRAINED (e32a999, `fix/apmf-post-0.11.0`).** Documented in `Docs/INTEGRATION.md` (ch.17 notes: re-declare after the cast claim releases).
Same review, finding F4. As relayed: "a refused ch.17 equip is never re-issued; document it in INTEGRATION".
Reviewer's reasoning (log `scratchpad/agentlogs/review-apmf-handblock.md`): channels/EquipAuthority.cpp has no tick;
it equips only on a declaration, so a declared weapon the hand-claim block refused while a cast claim held the hand
is not equipped again when the hand frees. The client must re-declare. Fix when drained: document it in
Docs/INTEGRATION.md (or re-issue on the hand's release).

### APMF-B65 (SEV-4/5, docs) -- hand-claim block: `bEquipDenyScript` now reaches cast-only actors
**DRAINED (e32a999, `fix/apmf-post-0.11.0`).** `Data/SKSE/Plugins/APMF.ini` comment widened to the cast-only-actor scope.
Same review, finding F5. As relayed: "`bEquipDenyScript` now reaches cast-only actors; fix the INI comment scope".
On an actor with no ch.17 claim, the sink step reads the INI `bEquipDenyScript` (the claim flags are 0), so with it
set a Papyrus / console equip into a hand a cast claim holds is refused too. The APMF.ini comment still describes it as
the ch.17 declared-set exemption only. Fix when drained: widen the comment (or scope the INI to ch.17 actors).

### APMF-B66 (SEV-5 x2) -- hand-claim block review follow-ups F9, F10
Raised against 1cc0bc9 (`fix/apmf-hand-claim-blocks-equip`, Opus tier-3 review MERGE), 2026-10-06.
- F9 (SEV-5): an extra handle lookup per combat leaf. `ActionGate.cpp:890` resolves the actor handle on every leaf `act()` of every combatant whenever any actor is watched. Cost only, no correctness problem.
- **DRAINED (e32a999):** F10 (SEV-5): comment and MAP wording. "EitherHand/null -> both" at the 0x0F seat is unreachable: CombatInventory AddItem (AE 0x811AC0) splits a multi-parent slot into per-parent items (Clone, vfunc 0x0D), so a one-hander becomes a RightHand item plus a LeftHand clone.
- DECIDED (marth 2026-10-06), review F2: a direct `EquipObject` from another SKSE DLL into a hand held by a cast claim is REFUSED. marth: "mfo asks harbinger to block it. Later its arbitrated". Papyrus / skse64 script equips, console and player-menu equips stay exempt. Becomes per-facet priority arbitration with the weighting rework (86e3jwmk7 / 86e3k3upc).

### APMF-B67 (SEV-4 x2, SEV-5 x2) -- spells-only floor (ABI v20) review: deferred F3-F6
Raised against `7bf4548` (`fix/apmf-floor-spells-only`, Opus tier-3 review, nothing above SEV-3), 2026-10-06. Text verbatim as relayed.
- **DRAINED at the v0.11.0 cut** (CHANGELOG entries moved into a `## v0.11.0` block). F3 (SEV-4): "A CHANGELOG entry for ABI v20 sits under the RELEASED `## v0.10.0` heading. v0.10.0 is ABI 17, tagged 2026-10-05. This is a pre-existing pattern for v18/v19 too. Fix at the next cut by moving all post-0.10.0 entries into an Unreleased block." Reasoning: release notes would credit v0.10.0 with features it does not ship; harmless until a cut, so drained at the cut.
- **DRAINED at the v0.11.0 cut** (`MinReleaseForAbi` 18/19/20 now return "0.11.0"). F4 (SEV-4): "The `MinReleaseForAbi` text at ClientAPI.cpp:238, \"the first release after 0.9.11\", is stale for 18/19/20. Name the real release at the cut." Reasoning: log text only (the "client too new" refusal line); the real release name is only known at the cut.
- **DRAINED (e32a999):** F5 (SEV-5): "In INVARIANTS #14b, the next bullet still says MFO calls `fn(kABIVersion)` and cites the dead `native/APMFBridge.cpp:398-403`. MFO actually asks for `kRequestAbi=10` (Bridge.cpp:922)." Reasoning: stale doc; the amended #14b text already states the current fact, the older bullet contradicts it.
- **DRAINED (e32a999, comment only: the mixed-hold label is documented in the code; no string changed):** F6 (SEV-5): "With a staff, R on a spells-only floor and L under a claim, the sink verdict reads \"hands R+L held by a cast claim\". The refusal is correct; only the label is loose." Reasoning: `core/EquipSink.cpp` step 1a picks one label for both hands; a mixed hold falls to the claim label. Log wording only.
### APMF-B68 (SEV-5) -- kQuery_Failed doc in the byte-shared header (post-0.11.0 drain review)
Raised against `407bb7a` (`fix/apmf-post-0.11.0`, Opus tier-B review, MERGE OK), 2026-10-06. Text as relayed.
- B-1 (SEV-5): "`APMF_API.h:2016` and `:2409` still document `kQuery_Failed` as \"an exception was caught; nothing was returned\". It now also means \"a ray was skipped, retry later\". Fixing that comment in a byte-shared header needs a mirrored edit in MFO's copy, so do it in a later header round." Reasoning: comment-only, but the header is byte-shared, so it waits for the next APMF_API.h round.

### Closed by triage 2026-10-06 (moved from OPEN / PENDING; each text kept verbatim, with its CLOSED line)

### APMF-B10 — `MinReleaseForAbi` v7/v8 entry must be named at the cut
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5 (release-checklist item).
- **Finding (verbatim, as relayed):** "MinReleaseForAbi \"first release after 0.9.4\" must be named at the cut (release checklist)".
- **Reasoning:** `core/ClientAPI.cpp MinReleaseForAbi` returns the placeholder "the first release after 0.9.4" for ABI v7/v8 because no release carrying them exists yet. Whoever cuts the next release replaces it with the actual version string in the same commit that bumps `project(APMF VERSION ...)`.
- **v9 (2026-09-16, `feat/equip-authority-v9`):** ABI v9 (`SetEquipScope`) joins the same placeholder entry (`case 7: case 8: case 9:`). v7, v8 and v9 all ship together in that first release; the cut names ONE version string for all three.
- **Assigned:** the next release cut.

- CLOSED 2026-10-06 (triage): OBSOLETE -- done: `core/ClientAPI.cpp` `MinReleaseForAbi` (`:226-229`) now returns "0.9.5" for ABI 7, 8, 9 and 10; the "first release after 0.9.4" placeholder is gone (and 18/19/20 name "0.11.0").

### APMF-B15 — the observe log names package slot 0 whatever slot would be used
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "observe log prints g_pkg[0] whatever slot would be used".
- **Reasoning:** In observe mode no slot is allocated (nothing is claimed), so the line had to name SOME package and named the first. Cosmetic, and the fix landed incidentally in the same round the intent was renamed — the observe line no longer names a package at all, it names the destination, the radius and the basis. Kept as a record of the finding; re-check at the drain that no observe line names a slot it did not allocate.
- **Assigned:** verify-only at the next drain.

- CLOSED 2026-10-06 (triage): OBSOLETE -- the observe mode this entry is about no longer exists: APMF-B13 (a) records marth's ruling that Harbinger has no observe mode (removed 2026-09-23), and `channels/Travel.cpp` has no observe line at all (`grep -i observe` finds none), so no observe line can name a slot it did not allocate.

### APMF-B27 (SEV-4) -- ch.20 vs MFO's own UpdateCombat target hook (hook order)
Raised against `5dcefa7`, same review, finding F3. MFO's `native/Targeting.cpp` hooks `Character::UpdateCombat` (slot 0xE4) and writes `currentCombatTarget` + `targetHandle` after the engine for any follower with a gambit latch. Both plugins install at kDataLoaded, so whichever `write_vfunc` runs last wraps the other; the order is SKSE's plugin dispatch order, not ours. Since the source-deny rework ch.20 no longer writes at 0xE4 at all (its answer lands inside `UpdateTarget`), so MFO's hook, running after the whole update, writes LAST in every order and overrides the pin for any actor MFO also latched. The ch.20 observer reports this as `OVERWRITTEN` only when MFO's hook is chained INSIDE it (MFO installed first); in the other order the observer's check runs before MFO's write and sees nothing, so the log cannot be relied on to show it. **Closure:** MFO retires its own write for any actor with a live ch.20 claim (MFO's port task).

- CLOSED 2026-10-06 (triage): OBSOLETE -- MFO retired its own UpdateCombat target write on the pin route: MFO `native/Targeting.cpp:69-72` (`anyTarget = !g_pinRoute && ...`) and `:155-158` ("MFO's own UpdateCombat target write is RETIRED" when `APMFBridge::TargetPinOffered()`); MFO's port task ClickUp 86e3eu6fd is complete. This entry's closure condition ("MFO retires its own write") is met.

### APMF-B5 — ch.17 Engage log still says "(SetEquipSet, ABI v7)"
- **Raised:** Fable tier-3 on `a369e9f` (feat/equip-authority-v8), SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** ":364 Engage log \"(SetEquipSet, ABI v7)\"".
- **Reasoning:** `channels/EquipAuthority.cpp` `Engage` names only the v7 call; v8's `SetEquipSetEx` is the other declaration path. Cosmetic log text; a reader grepping for v8 adoption would not see it here.
- **Assigned:** none; drain with the next ch.17 batch.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- log wording of ch.17's `Engage` ("SetEquipSet, ABI v7"); ch.17's declared-set path is what the rework retires ("most of the declaration path"). The wording is reviewed again only if the channel survives for the no-weighting cases.

### APMF-B6 — the hand-EQUP resolve error over-claims "every handed entry skipped"
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** ":226 error text over-claims \"every handed entry skipped\"".
- **Reasoning:** `Enforce`'s `resolveHandSlots` logs "every handed entry in this pass is skipped" when EITHER hand form fails to resolve, but the per-entry check skips only an entry whose OWN hand slot is null (`if (!equipSlot) { ++unresolved; continue; }`); with one hand resolving, entries for that hand are still equipped. Both forms come from Skyrim.esm at load index 00, so the branch is not expected to fire; the text should say "every entry declared for that hand".
- **Assigned:** none.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- error text in ch.17 `Enforce`'s hand-slot resolve (`resolveHandSlots`); the declared-set enforce pass is what the rework replaces with weighting.

### APMF-B8 — the "seat not installed" branch in Enforce is unreachable under the v8 refusal
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "the unreachable \"seat not installed\" branch in Enforce :176-186".
- **Reasoning:** since `EnqueueRequest` refuses a ch.17 claim while the seat is down, no claim exists for `Enforce` to run against when `Installed()` is false (Install runs once and never uninstalls), so the `g_seatMissing` error path is dead code. Kept as a defensive guard; its message ("declaration ... NOT enforced") describes a state that cannot arise. Either delete it or reword it as an invariant-violation log.
- **Assigned:** none.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the unreachable "seat not installed" branch is in ch.17 `Enforce` (`g_seatMissing`); `Enforce` is the declared-set pass the rework replaces.

### APMF-B9 — queued-slot carriage through the engine's deferred apply is unobserved
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5 (probe requirement added to `Docs/INTEGRATION.md` criterion 7 in the closing round).
- **Finding (verbatim, as relayed):** "queued-slot carriage unobserved (add the probe requirement: one `hand=left` equip line followed by visible dual-wield)".
- **Reasoning:** `Enforce` passes the `BGSEquipSlot` to `EquipObject(queue=true)`; the slot's survival through the AIProcess queue and the `QueuedApply` re-entry (38906/37950) is argued from the `EquipData` layout, not observed. CLAUDE.md principle 5: a path exists is not a path runs. Closed by the field log, not by code.
- **Assigned:** the first v8 deck session.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- whether the queued hand slot survives the engine's deferred apply is an `Enforce` (declared-set equip) question; if the declaration road is retired the probe requirement goes with it.

### APMF-B11 — v9 enforce pass counts an already-worn unowned item as `skipped-unowned`
- **Raised:** Fable tier-A on `3d5cab8`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "skip-before-worn miscounts an already-worn unowned item as skipped-unowned".
- **Reasoning:** `channels/EquipAuthority.cpp Enforce` runs the scope skip (`competes & denied` / `competes & ~owned`) BEFORE the inventory/worn test, so a declared item the actor already wears in an unowned category is counted and named as skipped rather than `already-worn`. The pass never equips it either way (correct); only the counter and the once-per-signature warn text are off. Fix = move the scope test after the worn test, or count worn-and-unowned separately.
- **Assigned:** the v9 backlog drain (before the observe-only flip).

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the v9 enforce pass's `skipped-unowned` counter is part of ch.17 `Enforce`'s scope skip, the declared-set pass the rework replaces.

### APMF-B12 — v9 skip warn is once-per-distinct-from-last, not once per signature
- **Raised:** Fable tier-A on `3d5cab8`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "warn is once-per-distinct-from-last, not once per signature".
- **Reasoning:** `ActorMemory::lastSkipSig` holds ONE signature, so alternating declarations A, B, A re-warn for A each time it returns; a true once-per-signature guard needs a bounded set per actor. Bounded, logged, never a pile-up; cosmetic.
- **Assigned:** the v9 backlog drain (before the observe-only flip).

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the v9 skip-warn signature guard (`ActorMemory::lastSkipSig`) is part of ch.17 `Enforce`, the declared-set pass the rework replaces.

### APMF-B63 (SEV-4, tooling) -- hand-claim block: the 1.7.104 Torch / OneHandedBlock 0x0F rows live on APMF's own idmap CSV
Raised against `1cc0bc9` (`fix/apmf-hand-claim-blocks-equip`, Opus tier-3 review, MERGE), 2026-10-06, finding F6. As
relayed: the 1.7.104 Torch row depended on an untracked `_research` CSV. Fixed on the branch by committing the evidence
as `tools/verified_addresses/idmap-1.7.104-apmf.csv` and writing the exact reproducing command into
Docs/VERIFIED-ADDRESSES.md (hand-maintained section). OPEN residual: move AE ids 45069 (Torch 0x0F, 0x819760; SE 43850)
and 45056 (OneHandedBlock 0x0F, 0x819300; SE 43837) into the CommonLibSSE-NG fork's 1.7.104 id table / idmap at the
next fork revision, then retire the APMF-local CSV rows.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwgvy -- the open residual (move AE ids 45069 Torch 0x0F and 45056 OneHandedBlock 0x0F into the CommonLibSSE-NG fork's 1.7.104 id table / idmap and retire the APMF-local CSV rows) is exactly the "move Harbinger/MFO verified engine bindings into our CommonLib fork" task (Fork F3).

### APMF-B69 (SEV-4 x3, SEV-5 x2) -- Harbinger controller 1a review, deferred items
Raised against 80b56a6 (`feat/apmf-controller-1a`, Opus tier-3 review `scratchpad/agentlogs/review-controller-1a.md`),
2026-10-11. Findings as relayed by the coordinator (the review log carries the reviewer's wording):
- (SEV-4) "GetPrivateProfileInt non-numeric -> 0=off": `core/Sources.cpp` ReadIni reads each key with
  `GetPrivateProfileIntA`, which answers 0 for a non-numeric value, so a typo becomes priority 0 = off instead of
  the default; a negative value is skipped (default applies). Fix when drained: read the string, accept only
  0..100 digits, otherwise log and keep the default.
- (SEV-4) "slider drag INI write per frame": `EnqueueSetPriority` is called on every frame the slider value changes
  and `Pump` writes the INI for each applied value. Fix: apply the atomic at once, write the INI only after the
  value has been stable for ~1 s.
- (SEV-4) "census-only sources Idle at once": a source only seen by the census (never a hit) has
  `lastLoadSeq == 0` and greys out as idle immediately. Fix: stamp the load sequence on NoteInvolved too, or
  show "never wrote" instead of idle.
- (SEV-5) "plugin.cpp comment placement": the Harbinger lines in `native/plugin.cpp` OnMessage sit inside the
  kDataLoaded install list's comment run; regroup when that file is next touched.
- (SEV-5) "thread-local frame on unwind": `channels/TargetPin.cpp` LayerBody leaves `t_frame.depth` raised if the
  chained call below it unwinds (an engine/plugin exception through UpdateCombat); the next update on that thread
  would take the nested path. Fix: an RAII guard that resets the frame.
Re-review of e63afa6 (MERGE OK), further notes deferred here:
- A writer that rewrites the target on every update (SmartNPC's post-write) makes the restore call
  `CombatController::SetTarget` on every update of a pinned actor (group bookkeeping each time). Consider a
  per-actor restore budget or a quieter path once the field log shows the cost.
- `sources::NoteClientRequest`'s first-claim reset reads the CACHED INI values (`g_iniPrio`), which a menu slider
  change does not update: a slider moved in-session on a module before that module files its first claim is
  discarded by the reset. Fix: update `g_iniPrio` in `Pump` when a slider value is applied.
- The module-range cache (`core/Sources.cpp` ModuleOf `g_ranges`) is never invalidated on DLL unload; an unloaded
  and re-mapped range would answer the old module. Fix with the 1b DLL-notification census (unload event).
Done in the same round instead of deferred (each under 20 lines): APMF's own enforce equips no longer register
`APMF.dll` as an outside equip source (`core/EquipSink.cpp`, `sources::IsSelf`), and module ranges are cached
(`core/Sources.cpp` ModuleOf) so a hot path stops calling GetModuleHandleEx / RtlPcToFileHeader per frame.

### APMF-B70 (SEV-3, SEV-4 x2, SEV-5) -- ch.25 cast guard review, deferred items
Raised against d2d591d (`feat/apmf-cast-lof-channel-bound`, Opus tier-3 review
`scratchpad/agentlogs/review-castguard.md`), 2026-10-12. The SEV-2 (EFIT area is feet) and the SEV-4 Repoint
refusal were fixed on the branch. Deferred, as relayed by the coordinator:
- (SEV-3) `IsHostileToActor` treats an aggroed but non-faction-hostile combatant as friendly -> over-holds the
  line of fire (`core/CastGuard.cpp` Friendly). FIELD-WATCH: count `[ch.25]` refusals naming a friendly that is
  itself in combat with the caster.
- (SEV-4) Self-delivered area spells (Fire Storm) are never guarded (`Offensive` excludes kSelf). Documented in
  Docs/INTEGRATION.md "Cast guards".
- (SEV-4) At 0x0A, an AI cast on the OTHER hand is judged against the claim's aim (Measure picks one aim per
  caster: the driving offensive claim's target, else the combat target).
- (SEV-5) The `g_lastLog` decision-log throttle map (`core/CastGuard.cpp` LogDue) is never cleared; clear it in
  ResetAll.

### APMF-B71 (SEV-4 x3, SEV-5 x6) -- 1b facet attribution review, deferred items
Raised against 90ea282 (`feat/apmf-1b-facet-attribution`, Opus tier-3 review
`scratchpad/agentlogs/review-1b-attr.md`), 2026-10-12. Review verdict MERGE OK, nothing above SEV-3. The tier-C
doc wording (Papyrus/console StartCombat resolves to Engine) was fixed on the branch in 6dc222d. Deferred:
- (SEV-4) Count `found=false` (Harbinger's own stack scan failed) separately from a genuinely unnamed caller in
  the Release counters; today both read as "unattributed".
- (SEV-4) Papyrus `Actor.StartCombat` and console calls run inside SkyrimSE.exe and resolve to `Engine` (20), not
  `DirectCalls` (75). Not a regression (still refused as before), but it breaks the 75 > 50 rule and the log
  names the wrong source. Telling them apart needs new verified addresses (the native's VM frame).
- (SEV-4) `Sources::ModuleOf` has no negative cache: a no-module caller costs up to 3 `GetModuleHandleExA` calls
  (loader lock under Wine) per StartCombat through the `FollowStubs` hops, while the seat holds `g_mx` shared,
  possibly inside a BSJobs job. No deadlock cycle found. Add a negative cache.
- (SEV-5) `FollowStubs` can misattribute a return address whose next instruction is a `jmp`.
- (SEV-5) ch.24 yield inference and the drift check can raise false alerts; attribution only, no behaviour.
- (SEV-5) S1 calls `Decide` on every Update per actor, so `Engine` hit counts dominate the menu (cosmetic).
- (SEV-5) The client's own DLL passes its own deny (tie goes to the source, logged). Intended; noted.
- (SEV-5) `ActionGate`'s `ClaimOutranksTree` has no try/catch, unlike ch.22 and ch.24; an allocation throw in
  `LineDue` / `Unattributed` would unwind into engine frames.
- (SEV-5) Deny-miss detector (`CombatReentryDeny.cpp`): a source pass and a real miss in the same 250 ms sample
  hide the miss.
- NEXT SLICE (not a finding): Cast-facet denies (CastGate, EquipGate, the 8b seats) consulting priorities.

### APMF-B72 (SEV-4, SEV-5 x6) -- 1b cast attribution review, deferred items
Raised against 9c00037 (`feat/apmf-1b-cast-attribution`, Opus tier-3 review
`scratchpad/agentlogs/review-1b-cast.md`), 2026-10-12. Verdict MERGE OK. The SEV-3 per-slot split, the SEV-4 try
coverage and two log-text SEV-5s were fixed on the branch in 83db3cf. Deferred:
- (SEV-4) `CastClaimHandle` is actor-wide (highest-basis cast claim), but EquipGate / CastSeats / classify decide per
  hand or per form. With two cast clients at different INI priorities, the wrong client's priority decides. Fix =
  a handle field on `CastSeatClaim` (ControlMap.cpp). Inert today: MFO is the only cast client and defaults are 50.
- (SEV-5) A client DLL hooked inside a seat ranks as Engine, so an INI cannot rank client B's hook above client
  A's claim and B's hits are booked to Engine. Refinement: map to Engine only when it is the claim's OWN client.
- (SEV-5) skse64 seen inside a cast seat gets its own `skse64_*.dll` hook row (20); a `[Cast] DirectCalls=` entry
  does not apply to it. Harmless (skse64 does not hook these vtables). Documented in INI-REFERENCE.
- (SEV-5) Engine Cast hit counts grow at roughly items x ticks (0x0F deny-complete per non-driven item); one cast is
  counted at up to three seats (classify, 0x06, 0x0F). Menu noise only.
- (SEV-5) The 0x0F census row is filed under the Equip facet (`FacetForTag("t2a")`) while its decisions are booked
  on Cast.
- (SEV-5) InsideSource could be resolved once at install to take first-sight loader-lock calls off the combat
  thread entirely.

### APMF-B73 (SEV-5 x6) -- 1b observe slice (ActorValueObserve seat) review, deferred items
Raised against df6716e / 1a93682 (`feat/apmf-1b-observe-facets`, Opus tier-3 review, three rounds,
`scratchpad/agentlogs/review-1b-observe.md`), 2026-10-12. Final verdict MERGE OK at fcd002c. The SEV-3 layered-hook
misattribution, the SEV-4 install-time null original, the SEV-4 hot-path RMW, the SEV-4 no-module CallerBelow
fault and the SEV-4 pre-installed 0x07 hook were fixed on the branch. Deferred:
- (SEV-5) A DLL that both layers over the seat AND writes actor values itself gets its own direct writes credited
  to its caller (one frame lower).
- (SEV-5) Engine-internal non-virtual AV paths (e.g. the inner restore 0x6B27A0) bypass the seat. Noted in MAP.
- (SEV-5) Docs/VERIFIED-ADDRESSES.md "1.7.104 proof" still says 241 slot functions; the 4 new pairs were compared
  identical by the reviewer. Extend the proof text.
- (SEV-5) The `Character.ActorValueOwner.SE` row has no `twin` key, so the generator does not class its 1.7.104 skip
  BY DESIGN. Add the twin.
- (SEV-5) The "Unknown sources can be overridden" checkbox has no effect on observe-only facets (ActorValue, and
  Detection for these AVs). Hide it or label it in the UI.
- (SEV-5) Papyrus SetAV/ModAV are named Engine (same limit as B71).

### APMF-B74 (SEV-5) -- heal LoS friends-pass review, deferred item
Raised against e517d44 (`fix/apmf-heal-los-friends`, Opus tier-3 review,
`scratchpad/agentlogs/review-apmf-heal-los.md`), 2026-10-12. Verdict MERGE OK. The SEV-4 once-per-claim log key
(expiresMs, moved by every client Repoint heartbeat), the SEV-5 Self-delivered-hostile classification and the
SEV-5 passFriend-when-strict-visible log were fixed on the branch. Deferred:
- (SEV-5) "strict identity: proven (strict = min over all blocking hits; pass >= strict so earlyOut only grows;
  verdict reads rayHit only). Volume/viewer diag counters drift (SEV-5)." -- with a friend set (a player /
  teammate viewer) the collector prunes at the farther friends-pass hit, so `volumes` / `viewerHits` and the
  nearest-volume note in the OCCLUDED detail line and the heartbeat count hits beyond the strict hit. Diagnostic
  only; no verdict reads them. Reviewer: inflated counters, no behaviour change.

### APMF-B79 (SEV-4) -- ch.26 "lift lost a pass" detector over-counts and misses the spell steer
Raised against 9d01cdf (`feat/apmf-equip-preference`, Opus tier-3 re-review), 2026-10-12. Reviewer (verbatim): "It only counts upward: a list rebuild (the dirty refresh) or a new fight reorders the list, and that gets counted as a lost pass. The heartbeat and menu should say \"at most\". It does not cover the ch.8b spell steer, because a cast-steered item returns before `Record`. The spell steer is the lift MFO relies on today, and it uses the same 5% step. Suggest adding this to the backlog."

### APMF-B80 (SEV-5) -- APMF-B75..B78 are labelled "Verbatim" but are paraphrases
Raised against 9d01cdf, 2026-10-12. Reviewer (verbatim): "APMF-B75..B78 match my findings in substance, but they are labelled \"Verbatim\" while being paraphrases. Rule 9 asks for the finding's exact text." The exact text is in the review log `review-apmf-equip-pref.md` (7cda290 round, F5/F6/F8/F9); paste it at the drain.

### APMF-B81 (SEV-5) -- ApplyPending logs "declared" before the supersede check; Prune scales with handles x map
Raised against 9d01cdf, 2026-10-12. Reviewer (verbatim): "`ApplyPending` logs \"declared …\" before it checks whether a newer declaration replaced this one." And: "It costs (tracked handles × the whole published map) once a second on the main thread. That is fine at MFO's scale, but would be heavy near the 4096-handle cap."

### APMF-B85 (SEV-4) -- stale ch.24 area geometry if a freed controller's address is reused
Raised against 732c859 (`fix/apmf-engagement-held`, Opus tier-3 round 3), 2026-10-12. Reviewer (verbatim): "StopCombat frees the controller. An immediate re-engage can allocate the new controller and area at the same addresses. A reactivation record for the old controller then matches the new controller and area, and writes the old area's saved geometry into the fresh area. No freed memory is touched, and the engine's next area update recomputes it. A generation or serial check would remove it."

### APMF-B86 (SEV-5) -- one-poll drive blip when the dropped entry and the pin name different targets
Raised against 732c859, 2026-10-12. Reviewer (verbatim): "If the dropped entry target differs from the pin target, `DropNotEngaged` still stops the pin hold's drive and calls `ClearInternalBound` unconditionally. The pin's own track restarts its drive on the next poll."

### APMF-B87 (SEV-5) -- `GetCommandingActor` (MiddleHighProcessData +0x218) has no verified-address row
Raised against 732c859, 2026-10-12. Reviewer (verbatim): "In the fork this is `currentProcess->middleHigh` (AIProcess +0x008) → `commandingActor` (MiddleHighProcessData +0x218, `static_assert(sizeof == 0x338)`). The header has no runtime split for it. MFO's shipped main already relies on the same accessor on both runtimes (`EngageOnSight.cpp:352`). I did not confirm +0x218 in disassembly; my quick byte-pattern scan found no reader. Add a verified row, or accept the precedent."

### APMF-B88 (SEV-5) -- stale PackageGate banner comment after the TryGetOwningClaimBasis switch
Raised against a2d914c (`fix/apmf-menu-client-hits`, Opus review), 2026-10-12. Reviewer (verbatim): "The banner comment at PackageGate.cpp:~140 still says \"same single TryGetOwningClaim call ... byte-identical\". It is stale wording only."

### APMF-B89 (SEV-5) -- g_handleMx purge holds the shared lock across an O(handles x claims) scan
Raised against a2d914c, 2026-10-12. Reviewer (verbatim): "On Windows a queued writer (`NoteClientRequest`) blocks new shared readers while the 60 s Pump purge holds the shared lock across an O(handles × claims) scan. That would briefly stall 0x49 and selector callers. It is sub-millisecond at realistic handle counts. Optional later fix: store the `SourceId` in the internal `Claim` at ApplyRequest so hot-path credits need no lock." Pre-existing, widened by the served credits.

### APMF-B90 (SEV-5) -- menu Winner() keeps naming a client that released every claim until it idles
Raised against a2d914c, 2026-10-12. Reviewer (verbatim): "A client that released everything stays \"winner\" until it goes idle after 3 loads. That is the same rule every other source already follows."

## Combat substrate S2a, tier-3 review round 1 (raised against a512763, 2026-10-13)

Source: the Opus tier-3 review of `feat/apmf-combat-substrate` @ a512763 (its log, quoted verbatim below, is
`scratchpad/agentlogs/review-substrate-s2a.md`; the full finding text went to the coordinator in the review's
handback). Fixed in the same round: F1-F6, F9, F10. F8 (the AfterLoad sweep) was APPROVED by marth 2026-10-13
and is recorded in INVARIANTS #0 (j) condition 4. MAP.md's `channels/CombatSubstrate.cpp` row cross-references
these entries.

### APMF-B91 (CLEARED, F7) -- RemoveTarget called unlocked from the game thread
Raised against a512763, 2026-10-13. Reviewer (verbatim, review log): "RemoveTarget: no lock inside (erase unlocked;
0x83DBA0 takes CombatManager+0x18 lock); engine 46875 + 40590 call it unlocked; CombatGroup::Update takes group
lock." CLEARED by that evidence: the substrate calls it the way the engine's own main-thread paths do. Kept here as
the record (the same exposure class as APMF-B26).

### APMF-B92 (SEV-4, F11) -- EquipSpell is denied by the allow-list (S4)
Raised against a512763, 2026-10-13. Reviewer (verbatim): "EquipSpell is denied by the allow-list. That conflicts with
design §4 / S4, where the engine equips the claimed spell under the claim. Record it for S4."

### APMF-B93 (RESOLVED by fix/apmf-substrate-release-close, 2026-10-26; was SEV-4, F12) -- the fire close may cut the charge
FIELD-PROVEN 2026-10-26 (LoreRim v273, 3 of 3 windows): 0x0B fired on the hand's Charged -> Casting, `CLOSE reason=fired`
16-33 ms later, StopCombat freed the controller, `anim SpellFire 0` every time. RESOLVED: 0x0B only marks the window
RELEASING; `fired` needs the holder's TESSpellCastEvent for the driven form; the hand back to None after a release state
(4-6) closes as `released-unconfirmed` (a warning, review F1 of 608df67); 3 s without either closes as
`fired-not-released` (a warning); the TTL defers while releasing. Original entry:
Raised against a512763, 2026-10-13. Reviewer (verbatim): "The "fire" close. It rides 0x0B `NotifyStartCast`, whose name
and callers (49083/49086/49101/49159) suggest the start of the cast. If so, `CloseSub`'s StopCombat would cut the
charging animation. The S2a dry run does not exercise this, but P4 (S3) must prove it."

### APMF-B94 (SEV-5, F13) -- the rank check runs once at open; the kStRank* bits are dead flags
Raised against a512763, 2026-10-13. Reviewer (verbatim): "The rank check runs once at open. That is acceptable within the
≤15 s bound, but the kStRank* bits are dead flags (always set): either re-evaluate them in `Serve` or remove them."

### APMF-B95 (SEV-5, F14) -- APMF.esl top-group order
Raised against a512763, 2026-10-13. Reviewer (verbatim): "The ESL top-group order is NPC_, PACK, CELL, which is not
canonical; Skyrim puts CELL before PACK. The HEDR record count of 100 predates this branch."

### APMF-B96 (SEV-4, F16) -- the anchor's collision is on, 72u behind the holder
Raised against a512763, 2026-10-13. Reviewer (verbatim): "Collision is on, 72u behind the holder. That is an invisible
obstacle in the player's path. P3 should log it."

### APMF-B97 (SEV-5, F15) -- a hit-sink MarkReal can land on a just-reused slot
Raised against a512763, 2026-10-13. Reviewer (verbatim): "A `MarkReal` from the hit sink can land on a slot that was just
reused. The result is benign: that holder's substrate drops."

### APMF-B98 (SEV-5) -- the DROP line's holder count
Raised against a512763, 2026-10-13. Reviewer (verbatim): "`LiveHolders()+1` in the DROP log miscounts when several holders
are flagged REAL at once."

### APMF-B100 (SEV-5) -- the SE side of the re-pick chain is not re-traced
Raised against 266769c, 2026-10-13. Reviewer (verbatim): "the SE downstream of the re-pick chain (the twins of
39108/39213/39434) is not independently re-traced; P6's attribution covers it in the field."

### APMF-B99 (SEV-4, MUST be traced before S3 relies on the allow-list) -- the function-node classes -- TRACED 2026-10-19 (equip path closed)
Raised in round 1's fix, 2026-10-13 (author, from F1's census of the node classes); raised by the round-2 reviewer
(266769c) to "must be traced before S3 relies on the allow-list" (the non-const EquipContext NodeEvaluateFunction).
`CombatBehaviorTreeNodeEvaluateFunction` and `CombatBehaviorTreeNodeUpdateFunction` are behaviour-tree nodes that
are not `NodeObject*`, so the allow-list does not see them (INVARIANTS #0 (j) 2(a)). Trace what their functions do,
the EquipContext evaluate function first, before S3 relies on the allow-list.
**TRACED 2026-10-19 (`feat/apmf-substrate-equip`; the equip question CLOSED, the class census recorded).** Six
function-node classes exist on both runtimes (RTTI). The base protocol (AE vtable 0x18DA8A8 / SE 0x1694018): act
(AE 47552) and pop (47553) are EMPTY (`ret 0`, no data-stack push), update (AE 47781 0x8686B0) calls the function at
node+0x28 on the current context (thread+0x128 window), sets the thread state to failed when it returns false, then
Ascends. (1) **EquipContext evaluate** ("Equip empty" on the Empty Sequence and "Equip potion" on the Potion
Sequence of `CombatBehaviorTreeActionEquipmentSelector`, AE 47688 / SE 46461): the function is AE 48120 0x877B70 /
SE 46952 0x7E04A0, which calls CombatInventory AE 44860 / SE 43629 (`equippedItems` +0x178 bookkeeping: entries
whose slot mask overlaps are dropped with the item's Unequip slot 0x12, the item added with its Equip slot 0x11) and
returns true. Slots 0x11 / 0x12 are empty functions on every CombatInventoryItem class except the five *Restore
Unequips (AE 0x8326B0 etc.: a blackboard timer). **No actor equip, and it does not gate the spell equip** (it is
on the Empty and Potion branches; the spell's Magic Sequence is [EquipSpell, Magic Link]). (2) ContextCover
evaluate (UseCover tree, AE 49816), ContextFlee evaluate and the two CombatState evaluates (Flee tree, AE 48563),
ContextSearch UPDATE function (Search tree, AE 49529): Movement-side trees, reached only after the Movement tree's
`Acquire Movement Resource`, which the substrate keeps denied. Their functions are not traced: they cannot run
under a substrate. Open only if a future allow-list opens the Movement branch.

### APMF-B141 (SEV-4) -- the substrate's LEAF DENIED line does not name the AcquireResource<2> instance
Raised against a0112b24, 2026-10-19 (tier-3 review of `feat/apmf-substrate-equip`). Reviewer (verbatim): "LeafDenied logs the Movement instance as 'LEAF DENIED CombatBehaviorAcquireResource<2>', which looks like 'the Action branch is still cut'; name the instance."

### APMF-B142 (SEV-4) -- "did NOT reach the hand" has three causes the log cannot tell apart
Raised against a0112b24, same review. Reviewer (verbatim): "'did NOT reach the hand' has three causes the log cannot tell apart: the claimed form not in CombatInventory (e.g. a proxy AddSpell'd after the controller was built), the item's own engine CheckShouldEquip NO (Light/Armor/Cloak conditions), or a scroll winning the claimed hand's set (scrolls are hooked by neither EquipGate nor HandBlock). Run the field test with [EquipGate] EnableEquipGateLog=1."

### APMF-B143 (SEV-5) -- a late combat-thread count can land in the next window
Raised against a0112b24, same review. Reviewer (verbatim): "NoteAllowed/Begin: a combat-thread count passing the id check just before a close and a re-Begin can land in the next window's counter (log only)."

### APMF-B144 (SEV-5) -- the "25/25" evidence wording
Raised against a0112b24, same review. Reviewer (verbatim): "Docs cite '25/25 AcquireResource denied' as proof, but the log cannot tell the two instances apart; cite the disassembly instead." FIXED in the same round (the docs and `channels/SubstrateEquip.cpp` now cite the allow-list class plus the disassembly; the log line is called consistent, not proof).

### APMF-B145 (SEV-4) -- a stowed copy without a uid comes back by form
Raised against 1f99de2b, 2026-10-19 (re-check of `feat/apmf-substrate-equip`). Reviewer (verbatim): "Restore by form when the worn copy had no uid: EquipObject(obj, nullptr) lets the engine pick any copy, which can be the plain copy instead of the tempered/enchanted one. Logged 'no uid'."

### APMF-B146 (SEV-4) -- a save inside the window keeps the stowed items unequipped (NEEDS marth)
Raised against 1f99de2b, same re-check. Reviewer (verbatim): "A save made during the window leaves the stowed items unequipped in the .ess; ResetHands drops the record with a log line. For MFO followers ch.17 enforce likely re-equips the declared loadout after the load; other clients get nothing back. Needs marth's acknowledgement (co-save record vs accept)."

### APMF-B147 (SEV-4) -- cross-repo noise during the stow window
Raised against 1f99de2b, same re-check. Reviewer (verbatim): "Cross-repo noise: ch.17 enforce re-issues EquipObject for the stowed declared weapon every 3 s during the window (EquipAuthority.cpp:553-579), each refused at step 1a with one log line; MFO PlayerGiven's sink may label a restored relic kAiEquipped. Bounded; watch in the field."

### APMF-B102 (SEV-5) -- ch.12 idle state: spdlog under g_stateMx
Raised against `d95b403` (`feat/apmf-idle-state`, ABI v24 GetIdleState), tier-3 Opus 5.5 review 2026-10-13
(verdict MERGE OK). Verbatim from the reviewer's log (`scratchpad/agentlogs/review-idle-state.md`): "F5 SEV-5
spdlog under g_stateMx". Where: `EvictForInsert` (channels/Idle.cpp) logs its once-per-session cap warning while
`g_stateMx`, documented as a LEAF lock, is held. spdlog takes its own sink lock, so the leaf claim is not strictly
true on that path. Fix: collect the victim handle under the lock and log after it is released.

### APMF-B103 (SEV-5) -- ch.12 idle state: the prune's IsClaimLive scan cost
Raised against `d95b403`, same review. Verbatim: "F6 SEV-5 IsClaimLive scan per live old record per 250ms".
`PruneStates` calls `ControlMap::IsClaimLive` (a full scan of the published map) for every record past its age
threshold, every 250 ms. Since the round-2 fix (F3) a live record without a final end cause is checked once it is
1 s old, so every live ch.12 claim with a form costs one full scan per 250 ms. Small today (a few idle claims).
Fix if it grows: one snapshot pass collecting the live handles, or a handle -> live lookup in ControlMap.

### APMF-B104 (SEV-4) -- ch.17 unequip passes the LeftHand EQUP for a shield/torch with no unequip readback
Raised against `93a7461` (Opus tier-3 review of `fix/apmf-equip-enforce-all`, F3). Verbatim: "the unequip passes the LeftHand EQUP for a shield or torch (`:80-82`), and there is no readback for unequips. Confirm on the deck that the item actually comes off; repeated `UNEQUIP` lines for the same item every 3 seconds would be the symptom." Field criterion for the next Deck run.

### APMF-B105 (SEV-4) -- ch.17 in-set test is a hand-blind FormID compare
Raised against `93a7461`, same review (F4, pre-existing, now on by default). Verbatim: "the in-set test is a FormID compare that ignores hands, at selection and at the sink, so a dual-wield hold can come out with its two weapons swapped."

### APMF-B106 (SEV-4) -- ammo at 48126 has no selection seat
Raised against `93a7461`, same review (F5, pre-existing). Verbatim: "ammo at 48126 has no selection seat. Under an Ammo-owned bow hold, if the engine's best ammo differs from the declared stack, the refusal lines repeat."

### APMF-B107 (SEV-5) -- stale texts and a hand-copied verdict after enforce-all
Raised against `93a7461`, same review (F6). Verbatim: "`plugin.cpp:245-249` still says `bEquipObserveOnly (default 1)`. The `APMF_API.h` comment 'never unequips' is stale; it is byte-shared, so it waits for the paired MFO change. `AiCastSeats.cpp:908`'s `hbRefused` labels a ch.17 selection refusal as a hand-block refusal. `ActionGate.cpp:1825 RangedProbeClassify` duplicates the verdict instead of calling `DeclaredSetVerdict`. The deny-only 'seat missing' log says '0 item(s)'." Also README.md:177/:216 and the CHANGELOG still name `bEquipEnforceArmor` (author's note; release cut).

### APMF-B108 (SEV-5) -- the stood-source map's 1024-entry clear
Raised against `a358017` (re-review R3). Verbatim: "the map only grows when a higher-ranked source equips a denied-category item and that equip stands, so it is practically unreachable. A clear makes the unequip fall back to ranking against Engine, which could strip one item a slider-raised source put on. It would only ping-pong while the map stays full."

### APMF-B109 (SEV-5) -- F2 "every candidate refused" warn can false-positive on AE's range branch
Raised against `a358017` (re-review R4). Verbatim: "AE evaluate sends some candidates down the range branch (`0x813a8a`) without calling 0x0F, so a declared item may not stamp YES in every evaluation. That can give a one-off false-positive warn. Log only."

### APMF-B110 (SEV-4) -- ch.17 uid: an instance worn in the other hand is skipped every pass; either-hand null-extra retries
Raised against `e6829829` (Opus tier-3 review of `feat/apmf-equip-instance`, F2). Verbatim: "An instance declared for one hand but worn in the other is skipped every pass with no way out until the client acts (`EquipAuthority.cpp` ~527-541). Unequipping it first would fix it, but that is new unequip scope, so documenting it is enough for now." and "when the right hand holds the form, `held` is set for an either-hand candidate. The AI can then try to put a second copy in the left hand with a null extra, and the sink refuses that each time." Hand entries only; MFO sets uids on rings/amulets only.

### APMF-B111 (SEV-5) -- ch.17 uid: RangedProbeClassify still tests membership by form
Same review (F4). Verbatim: "`ActionGate.cpp:1835` (`RangedProbeClassify`) still tests membership by form only, so for a uid-declared weapon it logs 'pass (declared item)' where the seat would refuse." Probe only.

### APMF-B112 (SEV-5) -- ch.17 uid: plain + uid entries for one form across hands
Same review (F5). Verbatim: "Suppose one form has a plain entry for one hand and a uid entry for the other, and the actor carries only the uid copy. The plain entry's null-extra equip can pick up the uid copy for the wrong hand, after which F2 applies."

### APMF-B113 (design follow-up) -- ch.17 uid: re-point EquipData.extra to the declared list on a null-extra equip
Same review (F1 optional). Verbatim: "when the engine names no copy and exactly one carried copy is declared, re-point `EquipData.extra` to the declared list. That is closer to 'enforce what was declared', but it mutates the engine's arguments, so it would be tier A." Would let the combat AI switch to uid-declared weapons again.

### APMF-B114 (SEV-5) -- menu: an own-client equip that no ranking decides lands in no counter
Raised against `5acb322` (Opus review of `feat/apmf-menu-split`, F3). Verbatim: "the deferred hit of an own-client equip is lost when no ranking decides that equip (for example an in-set item, or `dv.refuse` false). That equip then lands in no counter at all, not even Own." Also: a release race between `isOwnClient` and the ranking could count both Hit and Own for one equip (negligible).

### APMF-B115 (SEV-5) -- menu: EquipAuthority Served units differ for equips vs unequips
Same review (F5). Verbatim: "`EquipAuthority.cpp` Served uses different units: equips are credited once per pass per facet (~608-611), unequips once per item (~326)."

### APMF-B116 (SEV-5) -- menu: init log double-counts legacy [Equip] overrides; EquipArmor census shows no seats
Same review (F6). Verbatim: "The Init log 'N priority override(s) read' counts legacy `[Equip]` keys twice, because they are copied into both maps." and "The EquipSink and HandBlock seats register for the census only on EquipWeapon, so hovering an EquipArmor source shows no seats."

### APMF-B117 (SEV-4) -- queued-leg pairing: a stale record can rank a leg 2 as a requester
Raised against `34f2779` (Opus tier-3 review of `feat/apmf-queued-0x49`, round-1 spot 1). Verbatim: "Wrongful ALLOW needs all of these: a stale record (from a q=1 leg 1 the engine applied at once) and, within 5 s, a leg 2 of the same item whose leg 1 skipped the seat or happened while the actor was unclaimed, with a claim back in place by leg 2. That window is narrow, and the item is one the requester itself just equipped." Crosswise pairing of two live records only misattributes logs (SEV-5).

### APMF-B118 (SEV-5) -- 0x49 owner = orig->GetFile(0): vanilla packages in a follower alias attribute to Skyrim.esm
Same review (spot 3). A vanilla follower alias package is owned by Skyrim.esm (default 20); MFO's own .esp package would rank as a source separate from the MFO client (menu shows MFO.esp overruled). Naming the alias's quest plugin needs an ExtraAliasInstanceArray walk under its lock on the AI thread.

### APMF-B119 (SEV-5) -- procedure classifier: Acquire as auxiliary; unchecked tree pointer reads
Re-review of `ae10bf6`. Verbatim: "the vanilla `Acquire` template (Acquire, Find, Travel, Wander) classifies as STANDING because of its Wander fallback. A follower mod's own 'go pick up or steal X' package built on that template would not count as active, so at a priority tie the claim wins. This is marth's design call, not a defect." Also: "The first read of the object pointer itself, and the reads of `p->data`, `procedureTree`, `templateParent` and the branch child array in `ClassifyUncached`, are not validated. They are safe for plugin packages ... For a runtime-created package, lifetime belongs to the engine."

### APMF-B120 (SEV-4) -- hold: the 300 u "arrived but not fighting" end is not suspended while the target flies
Raised against `910c12f` (Opus review of `fix/apmf-flyer-progress`). Verbatim: "a ranged or caster follower whose cast is denied, or a melee follower under a dragon hovering at dz 250-300, could still lose the hold after 20 s while the dragon is airborne. That cuts against 'an engagement cannot be lost'. If marth wants it airtight, the fix is to also gate `arrivedSinceMs` on `!tflying`. The evidence does not show this end firing on the dragon." Also recorded: the suspension is unbounded by design, so a mod flyer that sits permanently in Cruising/Hovering never gets the no-progress end (accepted by the ruling).

### APMF-B121 (SEV-5) -- hold: flying transition cosmetics
Same review. The out-of-combat branch clears `samples` but not `targetFlying` (the "no longer flying" line can print late); after landing the window starts at the last flying sample (one poll short of 20 s). Harmless.

### APMF-B122 (SEV-4) -- substrate music deny: a non-holder member makes the group impure
Raised against `b1c78b2` by the tier-3 review of `fix/apmf-substrate-fixes`. Verbatim: "A teammate in the group makes it impure, so the deny is not written. It is logged (WAITS, then MISSED), so nothing is masked; design hole in deny-completeness. Allowing a non-holder member whose only target is the anchor would be a reasonable widening later." Reasoning: deferred by the coordinator; the WAITS / MISSED lines make every occurrence visible in the field.

### APMF-B123 (SEV-5) -- channels/CombatSubstrate.cpp size: propose a split as its own brief
Raised against `b1c78b2` by the same review: the file was 1940 lines (2103 after the review-round fixes). A split is its own brief with its own tier-3 review and split proof, never inside a feature round.

### APMF-B124 (SEV-4) -- substrate music deny: a full debt table is ignored silently
Raised against `102526c` by the tier-3 re-review (verbatim): "`DebtSet` returns false when all 32 entries are in use, and both callers ignore that. The owed 0 is then never put back and nothing is logged, which breaks principle 7. It is unlikely (16 slots, 30 s debts), but a log line is cheap. It has to be emitted after `g_debtMx` is released, because that mutex is a leaf." Where: `DebtSet` in `channels/CombatSubstrate.cpp` (~:609), callers `SeatMusicAnchor` (~:657) and `AfterLoad` (~:2087).

### APMF-B125 (SEV-4) -- substrate music deny: debt lost when a non-holder joins a denied group and the writer closes normally
Raised against `102526c` (verbatim): "Holder A denies pure group G. A non-holder later joins G (a StartCombat join or merge into G). A closes normally, and `DebtForget(A)` runs at the start of `CloseSub`. G survives with that member, still reads 0, and no debtor remains. When a real target later appears in G, nothing puts the 0 back. It is rare. A co-holder is covered, because it carries its own debt."
Narrowed by the anchor guard (ace1d8b); the debt is still lost when the ranked StopCombat refuses (pin, ch.24 or Engine outranking) and the group survives holding the 0.

### APMF-B126 (SEV-5) -- substrate music deny: an engine-decided 0 can be reset to 0xFF
Raised against `102526c` (verbatim): "When a debtor's group reads 0 and shows a real target, the 0 is reset even if the engine decided it. The engine just re-decides from the current strengths, which suits a fight that is now real. This is acceptable." Recorded for completeness; reviewer judged it benign.

### APMF-B127 (SEV-5) -- substrate music deny: the 30 s debt can expire before a real target arrives
Raised against `102526c` (verbatim): "After a hit or party drop where `StopCombatRanked` refuses to end the targetless fight, the group can outlive the 30 s debt. A real target added later then leaks the 0. It is an edge case."

### APMF-B128 (SEV-5) -- substrate seat logging runs inside the engine's group write lock
Raised against `102526c` (verbatim): "The sink is a plain file sink with `flush_on(info)`, so each seat line does a synchronous fflush while the combat job holds the group's write lock. Every line is once per event, so this is a stall, not a deadlock. The existing DetectThunk lines already do the same."

### APMF-B129 (SEV-5) -- ch.19: a re-pointed leg never logs the substrate-held line again
Raised against `3b4272aa` (`fix/apmf-ch19-substrate-pass`), Opus 5.5 review 2026-10-19. Verbatim: "Travel.cpp:~2030: a RE-POINTED leg is called 'a new leg' but does not reset substrateLogged (gaitLogged behaves the same way), so a re-pointed leg never logs the held line again. StartLeg resets it (:886)." Cross-referenced from MAP.md's ch.19 Travel and ch.27 What-breaks entries.

### APMF-B130 (SEV-5) -- substrate bystander census walks the high-process list every Poll frame
Raised against `3b4272aa`, same review. Verbatim: "The census walks ForEachHighActor on every Poll frame while its window is live (about 16 s of every 20 s with the dry run). Cheap, but PartyPoll already walks the list each frame and castobserve / castertypecensus throttle to 100-200 ms; a 100-200 ms throttle would match those probes." Cross-referenced from MAP.md's ch.27 What-breaks entry.

### APMF-B131 (SEV-5) -- substrate bystander census goes silent after 60 lines with a long-lived client substrate
Raised against `3b4272aa`, same review. Verbatim: "With a long-lived client substrate the census window never closes; after 60 lines it goes silent for the rest of the session apart from its one cap line. Telemetry loss only, no unbounded logging." Cross-referenced from MAP.md's ch.27 What-breaks entry.

### APMF-B132 (SEV-4) -- substrate probe: a client substrate never gets a pre-open movement baseline
Raised against `bbc2d29` by the tier-3 review (verbatim): "SubstrateProbe.cpp:343-348. A client substrate's window is created in BeforeEntry, and Opened runs in the same frame. So the 'before' stage of a client window is always 'speed n/a, dist n/a', and the brief's 'from 1 s before the open' holds only for the dry run. The header (.h:42-51) and the file header (:19-21) state this, so it is documented rather than hidden. It matters once MFO's S3 client opens substrates." Fix before the S3 field run.

### APMF-B133 (SEV-5) -- bSubstrateProbes defaults ON
Raised against `bbc2d29` (verbatim): "SubstrateProbe.cpp:289 and APMF.ini bSubstrateProbes=1. A default of ON contradicts the CLAUDE.md:28-29 rule ('config-gated logging only ... default OFF'). The brief explicitly asked for 1, and the bystander census set the precedent, so no change is needed now. Flip it to 0 when the substrate leaves test." Flip together with bSubstrateBystanderCensus at ship (plan L8).

### APMF-B134 (SEV-5) -- the first dry round after start has no pre-request samples
Raised against `bbc2d29` (verbatim): "SubstrateProbe.cpp:306-307 with CombatSubstrate.cpp:1475-1476. The first dry round after start (g_lastDryMs=0, so the due time is already past) never gets pre-request samples. It is logged as 'no pre-request samples', which is honest."

### APMF-B135 (SEV-5) -- OtherReset's "package change in this frame" almost always reads no
Raised against `bbc2d29` (verbatim): "SubstrateProbe.cpp:406-410. OtherReset is called only when !restart, meaning the package and start time equal the bit-write values. So 'package change in this frame' can read YES only when the previous frame's package differed from bitPkg. In practice it will almost always read 'no'. It is correct but adds little information."

### APMF-B136 (SEV-5) -- the probe's "close=" aggressor value is pre-StopCombat
Raised against `bbc2d29` (verbatim): "SubstrateProbe.cpp:236. The 'close=' aggressor value is read before StopCombat and before the ignoring bit is restored. Label it 'close (pre-StopCombat)' in the line or the INI-REFERENCE wording so a field reader does not misread it."

### APMF-B137 (SEV-5) -- anchor guard: g_leaks entries for clean members inflate the window count
Raised against `ace1d8b` by the tier-3 review of `fix/apmf-anchor-leak` (verbatim): "AnchorGuard.cpp:191, 397: g_leaks[id] is created before the clean test, so every clean member that AfterClose passes through inflates the window-end '({} actor(s))' count." Reasoning: a log count only; deferred. MAP ch.27 row (the anchor guard).

### APMF-B138 (SEV-5) -- anchor guard: a non-logged retry's outcome is dropped
Raised against `ace1d8b`, same review (verbatim): "AnchorGuard.cpp:203-250: the outcome of a non-logged retry is dropped; if the first logged attempt says 'NOT ended' and a later retry succeeds, no line ever says it ended (only the window summary counts it)." Reasoning: log completeness only; deferred. MAP ch.27 row.

### APMF-B139 (SEV-5) -- anchor guard: published group / kind pair can tear for one frame
Raised against `ace1d8b`, same review (verbatim): "AnchorGuard.cpp:439-442: kinds[i] and groups[i] are stored per slot in the published set; a concurrent reader can pair an old address with a new kind of 0 and miss it for one frame. Within the stated one-frame-old limit." Reasoning: inside the documented one-frame limit; the sweep is the backstop. MAP ch.22 / ch.27 rows.

### APMF-B140 (SEV-5) -- anchor guard: trace default ON, no sweep kill switch, INI comment gap
Raised against `ace1d8b`, same review (verbatim): "bSubstrateEntryTrace=1 defaults ON (the B133 precedent: flip at ship); the sweep has no kill switch apart from bCombatSubstrate=0; the APMF.ini comment does not say the trace needs bSubstrateJoinGuard=1 (INI-REFERENCE does)." Reasoning: the trace is on the STATUS pre-release checklist; deferred. MAP ch.27 row.

### APMF-B148 (SEV-4) -- substrate hands: script/console equips pass the hand block unless DenyScript is on
Raised against `ee8ca38` by the tier-3 closing re-check (verbatim): "Sink step 0 lets script and console equips through the hand block unless DenyScript is on (EquipSink.cpp verdict: scriptExempt = scriptPath && !denyScript). A third-party Papyrus EquipItem of a weapon during the window of at most 15 s can therefore put a weapon back in a floored hand, and EquipSpell's draw would then draw it. This was already true for client Opens. It is rare. The SubstrateEquip.cpp header comment says step 1a refuses 'a script', which is inaccurate. Correct the comment, or set DenyScript for substrate holders."

### APMF-B149 (SEV-4) -- place: a LOADED placement runs the Character branch from the player-Update pump, unobserved
Raised against `89a72848` by the tier-3 review of `feat/apmf-place-facet` (verbatim): "Place.cpp:201-202 runs the Character branch (StopInteractingQuick + process work) on a LOADED actor from the player-Update Pump seat; unobserved (the anchor precedent is a non-actor). MFO should always pass OnlyIfUnloaded; the first field run should show a loaded placement before anyone relies on one." Reasoning: MFO's errand use is the unloaded case; deferred until a field run shows a loaded placement. MAP `core/Place` entry.

### APMF-B150 (SEV-5) -- place: a Post that throws leaves a permanent Queued record
Raised against `89a72848`, same review (verbatim): "Place.cpp:327-352: if mainthread::Post throws after the record is inserted, a permanent Queued record counts toward the 128 cap until the next load." Reasoning: needs a bad_alloc in the post; the next load clears it; deferred. MAP `core/Place` entry.

### APMF-B151 (SEV-5) -- place: kIntent_Place falls to Facet::kCore in FacetMaskForIntent
Raised against `89a72848`, same review (verbatim): "Sources.cpp:634: kIntent_Place falls to Facet::kCore in FacetMaskForIntent (menu cosmetic)." Reasoning: menu attribution cosmetic only; deferred. MAP `core/Place` entry.

### APMF-B152 (SEV-3) -- place: RefuseIfPlayerPresent over-refuses exterior persistent-ref destinations
Raised against `2914e534` by the tier-3 closing re-check (verbatim): "with RefuseIfPlayerPresent set, every exterior placement to a persistent ref in the player's current worldspace is refused, whether or not it could be seen" -- GetParentCell of a persistent ref outside the loaded grid is (inferred) the worldspace persistent cell (formFlags 0x2000, GetSkyCell AE 0x30B070), which TES keeps attached (cellState 7) while the player is in that worldspace. No false pass (a visible destination refuses either way). Proper fix: when cell->formFlags & 0x2000, test the real grid cell at the point instead (verify that symbol in the fork first). Carve-out (b) if MFO's next town field cycle places at exterior destinations; interior counters are unaffected.

### APMF-B153 (SEV-4) -- APMF_API.h IsSubstrateAvailable comment still says "exactly 1.6.1170 or 1.5.97"
Raised against `f0145287` (tier-3 review): the substrate now installs on 1.7.104, but the byte-shared header comment (APMF_API.h:2546) was left unedited. Comment-only; mirror the corrected comment into MFO's copy at the next ABI bump.

### APMF-B154 (SEV-5) -- 1.7.104 RegisterCallSeat names the hold seats with the 1.6 kSiteName
Raised against `f0145287` (verbatim): "CombatEntry.cpp:1232-1233: on 1.7.104, RegisterCallSeat still names the seats with the 1.6 kSiteName. This only affects the census label."

### APMF-B155 (SEV-5) -- 1.7.104 install-time call-target checks are stricter than the 1.6 runtime rows
Raised against `f0145287` (verbatim): "The CallsTo17 and BytesAt17 checks run at kDataLoaded and also check the call target, so they are stricter than the 1.6 runtime rows. On 1.7.104 only, a third-party patch at the group update's LockForWrite call would refuse the substrate. This is informational and fails safe."

### APMF-B156 (SEV-5) -- BSMusicManager::current (+0x28) read by MusicNow is unproven by any row
Raised against `f0145287` (verbatim): "BSMusicManager::current (+0x28, read in MusicNow) is not proven by any row on any runtime. It is only used for logging." Existed before this branch; now reachable on 1.7.104.

### APMF-B157 (SEV-5) -- bark deny: ownership needs both timers to match as a pair
Raised against `e6f5607c` (tier-3 review of feat/apmf-substrate-r5-denies, review log `scratchpad/agentlogs/review-apmf-substrate-r5.md`), finding F4 (verbatim as relayed): "ownership needs both timers to match as a pair." `SubstrateBark.cpp` `HarbingerValues` treats the group's two timers as Harbinger's only when BOTH read one armed record's {clock, duration}; an engine arm of one timer alone makes the pair "not ours" (no put-back of the other). Reasoning: SEV-5, the unrestored timer runs out within TTL + 2 s. MAP `SubstrateBark.cpp` entry.

### APMF-B158 (SEV-5) -- bark deny: stale-value adopt and a false CHANGED
Raised against `e6f5607c`, finding F5 (verbatim as relayed): "stale-value adopt and false CHANGED." A second holder that arms a group whose timers still read an earlier (forgotten or expired) record's values adopts nothing (that record is gone) and saves Harbinger's values as the "before" values; and a co-holder's re-arm makes the first holder's record report CHANGED (logged as "or the group was re-armed by another holder"). Diagnostics / a put-back of Harbinger's own values only; SEV-5. MAP `SubstrateBark.cpp` entry.

### APMF-B159 (SEV-5) -- bark deny: arm TOCTOU (microseconds, logged LATE)
Raised against `e6f5607c`, finding F6 (verbatim as relayed): "arm TOCTOU, microseconds, logged LATE." Between `BeforeUpdateCombat`'s read of the controller / group and its write there is a microsecond window; a controller whose +0x35 startedCombat is already set at the write is logged LATE on the BARK DENY line. Reasoning: the seat runs on the holder's own UpdateCombat before the chain, so the window is only another thread's access, the engine's own exposure. MAP `SubstrateBark.cpp` entry.

### APMF-B160 (SEV-5) -- the substrate now depends on TargetPin's UpdateCombat layer 0
Raised against `e6f5607c`, finding F9 (verbatim as relayed): "the substrate now depends on TargetPin layer 0 (bTargetPin=0 refuses it unless bSubstrateBarkDeny=0)." The bark deny rides `TargetPin.cpp` layer 0 (`substratebark::BeforeUpdateCombat`); with `[TargetPin] bTargetPin=0` layer 0 is not installed and the bark deny's Install refuses the substrate (principle 2) unless `[CombatSubstrate] bSubstrateBarkDeny=0`. Documented in INI-REFERENCE (`bTargetPin`, `bSubstrateBarkDeny`) and FACETS ch.27. A future cleanup could give the 0xE4 layer its own switch independent of the pin. MAP `SubstrateBark.cpp` / `TargetPin.cpp` entries.

### APMF-B161 (SEV-5, CLOSED at merge) -- SubstrateBark.cpp stale header comment
Raised against `898f90c3` (verbatim): "SubstrateBark.cpp has a stale header comment. Lines 55-57 still describe a drop put-back 'on the game thread under the group's WRITE lock'. Lines 64-66 still say 1.7.104 is 'refused by name'." Fixed (comment-only) in the merge commit.

### APMF-B162 (SEV-5) -- bark debt expiry uses MonotonicMs while the timers run on the AI clock
Raised against `898f90c3` (verbatim): "Debt expiry uses MonotonicMs, but the timers run on the AI clock. If a pausing menu is open, the UNPAID line can fire while Harbinger's values are still live for about the paused time. The only effect is a few more seconds of a joining member's bark being suppressed."

### APMF-B163 (SEV-5) -- F-6: NoteDenied / Closing race can recreate a stale window
Raised against `118ea269` (tier-3 review of feat/apmf-substrate-f6-override-deny, review log `scratchpad/agentlogs/review-apmf-substrate-f6.md`), finding as relayed (verbatim): "NoteDenied/Closing race recreating a stale window." An AI-thread denial that lands after `Closing` erased the holder's window re-creates it through `g_win[a_actor]` (opened = false); it lingers until the next `Opening` resets it or `Reset` clears the map. Harmless for logic (the next Opening overwrites it), a bounded leak per holder. Reviewer's reasoning: the deny gate is `StateOf == Held`, read before the lock, so a close between the check and the lock can still count.

### APMF-B164 (SEV-5) -- F-6: the call-site row does not pin the selector's `test rax,rax; cmovne rbx,rax`
Raised against `118ea269` (verbatim as relayed): "add the `48 85 C0 48 0F 45 D8` bytes at +0xF8 to the call-site row." `Substrate.PackageSelector.CombatOverrideCall` checks only the E8 at +0xF3 (and its target); the null-keeps-the-pick contract rests on the next instruction pair (AE `0x65DF88` / SE `0x5CDA1F` / 1.7.104 `0x670978`), which the signature does not cover. Reviewer's reasoning: the deny's meaning depends on those bytes.

### APMF-B165 (SEV-5) -- F-6: S2b Reassert logs at most 30 lines per window
Raised against `118ea269` (verbatim as relayed): "Reassert 30 lines/window." marth's approval says every undone reset is logged; past 30 lines in one window the rest are only counted in the close summary (`S2b re-asserts N`). Reviewer's reasoning: a line cap is a partial log; S2b is OFF by default, so it matters only once it is switched on.

### APMF-B166 (SEV-4) -- torch seat: Install / Decide do not check equipsink::Installed()
Raised against `eb160e3` (tier-3 review of feat/apmf-torch-seat, verdict MERGE, review log `scratchpad/agentlogs/review-apmf-torch.md`), finding F1 (verbatim as relayed): "torchseat Install/Decide do not check equipsink::Installed() (bEquipAuthority=0 still denies the updater; fix = refuse seat \"equip sink not installed\")". Reviewer's reasoning (review log): "seat not gated on sink Installed (bEquipAuthority=0)". With the sink down `EnqueueRequest` refuses new ch.17 claims, so the gap is narrow, but the torch seat should not stand on its own when the facet's main seat is off. Fix: in `core/TorchSeat.cpp Install()` refuse with the reason "equip sink not installed" when `!equipsink::Installed()` (Install already runs after `equipsink::Install`).

### APMF-B167 (SEV-4) -- torch seat: no streak-ENDED line when the claim is released
Raised against `eb160e3`, finding F2 (verbatim as relayed): "streak-ENDED line missing when the claim is released (TryGetEquipSet fails before NoteVerdict, map entry lingers)". Reviewer's reasoning (review log): "no ENDED line when claim released entirely". `Thunk` returns at the no-claim branch before `NoteVerdict`, so a deny streak only ends on a later call that still finds a claim; the `g_streaks` entry stays until the bounded map clears. Fix: on the no-claim branch, end any open streak for the actor (one map lookup under `g_mx`, guarded by a relaxed "any streak open" atomic so unclaimed actors stay cheap).

### APMF-B168 (SEV-5) -- spec row label EquipSink.Path.AiCommand.39948 (+ SE twin) still the old name
Raised against `eb160e3`, finding F3 (verbatim as relayed): "spec row label EquipSink.Path.AiCommand.39948 (+SE twin) still old". Reviewer's reasoning (review log): "spec label AiCommand.39948 stale". The sink's path table now names 39948 / 38902 `TorchUpdate`, but the spec rows `EquipSink.Path.AiCommand.39948` / `.38902` (and their `construct` text) keep the old label because the torch round kept spec edits append-only. Rename both rows (and the generated header / doc) in one regeneration when the substrate branches are no longer editing the spec.

### APMF-B169 (SEV-5) -- torch seat: the stow log covers only kRun, not kWouldDeny / kStood
Raised against `eb160e3`, finding F4 (verbatim as relayed): "stow log only for kRun, not kWouldDeny/kStood". In observe-only, or when the Engine source outranks the claim, the updater runs and can stow a torch the claim owns, and no line says so (only the once-per-streak would-deny / STANDS line). Fix: read `HeldLight` before the call for every verdict except kDeny and log the stow with the verdict named.

### APMF-B170 (SEV-4) -- document that a default-scope (owned=All) claim owns Light, and a torch already in hand stays
Raised against `eb160e3`, author spot 1 (verbatim as relayed): "document in ABI/FACETS text that a default-scope (owned=All) claim owns Light and a torch already in hand stays". Reviewer's reasoning: the seat follows the sink's rule (owned, not denied, declared), so any claim on the v8 default scope with a declaration owns Light and the engine's torch updater is skipped for that actor, including its stow. A torch the actor holds when the claim publishes is therefore not put away by the engine. Today MFO claims every category, so this applies to every MFO follower with a declaration until MFO narrows Light to its torch rule. Fix: a sentence in FACETS ch.17 "Torch ownership" and in the `SetEquipScope` / `kEquipCat_Light` comment of `APMF_API.h` (byte-shared: a mirrored MFO + APMF comment-only edit).

### APMF-B171 (SEV-5) -- document that the torch seat's counters are per streak, the sink's per event
Raised against `eb160e3`, author spot 2 (verbatim as relayed): "document per-streak counter semantics differ from the sink's per-event". Reviewer's reasoning (review log): "per-streak counters". The torch seat counts the Engine source's Hit + Blocked / Passed once per deny streak (the updater runs every AI update), while the equip sink counts once per equip. A user reading the Harbinger menu sees one torch block per streak, not per skipped update. Fix: one line in FACETS ch.17 "Torch ownership" and in the menu / HARBINGER-MENU doc.

### APMF-B172 (SEV-4) -- torch: Light-owned-but-not-Left displacement and generic unequips remain open
Raised against `eb160e3`, author spot 5 / review F5 (verbatim as relayed): "Light-owned-but-not-Left displacement and generic unequips remain open (already in AUDIT row 17 -- cross-reference)". Reviewer's reasoning (review log): "holes (Left-only displacement, generic unequips)". A claim that owns Light but not Left lets an engine equip into the left hand push the held torch off, and a generic unequip of a torch (RemoveItem, outfit, script) is not seated. Both are recorded in `Docs/DENY-COMPLETENESS-AUDIT.md` row 17, open item (a). They close with the unequip twin's own #17a argument or with MFO owning Left together with Light while its torch rule holds.

### APMF-B173 (SEV-4) -- ch.19 cell hold: slot pressure
Raised against `be7d9ec` (`feat/apmf-ch19-cell-hold`), tier-3 Opus 5.5 review 2026-10-20 (reviewer log `scratchpad/agentlogs/review-apmf-ch19-cell-hold.md`), finding F4, verbatim as relayed: "slot pressure (held follower keeps 1 of 8 slots; a held re-point needs a second free slot, fails kLeg_Failed when 7-8 taken)". Reasoning: a held cell leg keeps its package record for the whole client event (`HoldAtCell`), and a Repoint of a held leg must start on a FRESH record before the held one is released (`ComposeLeg` held branch: the engine does not restart the same package in DONE). With 7 of 8 slots taken the re-point is refused loudly (`kLeg_Failed`) and the hold is let go. A party-scale client stays well under it today. More slots would need more PACK records in `Data/APMF.esl`. Cross-referenced from MAP.md ch.19 What breaks (6a).

### APMF-B174 (SEV-5) -- ch.19 cell hold: holes the hold does not cover
Same review, finding F5, verbatim as relayed: "holes the hold does not cover (PapyrusUtil/ActorUtil package overrides above the 0x49 tier; Papyrus MoveTo from SFF/AFT teleports -- logged once, MFO decides)". Reasoning: the hold is the ch.9 offer at the 0x49 alias tier, which is structurally BENEATH script-driven package overrides (INVARIANTS #3a), and a script `MoveTo` moves the actor out of the cell without asking any package. Poll logs the departure once (`HELD in cell ... but now in cell ...`) and does nothing, by design (no re-assert). The client decides whether to Repoint. Cross-referenced from MAP.md ch.19 What breaks (6a).

### APMF-B175 (SEV-5, optional) -- ch.19 cell hold: log the running package at the hold and on reload
Same review, verbatim as relayed: "optional: log the current package at HoldAtCell and on reload". Reasoning: the field proof that the held actor keeps the travel package in DONE is today read from the `[obs]` lines (ControlMap.cpp ~:1850 prints curPkg for claimed actors, unloaded included), which the reviewer judged adequate. One line naming the running package at `HoldAtCell` and when the actor loads again would make the hold self-evident in the log.

### APMF-B176 (SEV-3 ruling item) -- ch.24 releases the client's claim when the actor's 3D unloads
Same review (coordinator relay, 2026-10-20), verbatim: "ch.24 CombatApproach.cpp:1079 releases the client's claim on !Is3DLoaded(), contrary to marth's 2026-10-20 ruling that claims hold for the client's event". Reasoning: `channels/CombatApproach.cpp` `Poll` calls `End(id, kApproachState_ActorGone, "actor dead / unloaded:")`, and `End` does `EnqueueRelease` on the winning claim. marth's ruling ("mfo claims need to hold for the duration of the mfo event") means a claim ends on the actor's death or the loss of its reference, not on a 3D unload. Outside the ch.19 brief's file boundary, so not changed there. It needs its own brief and a ruling on what the combat-approach bound means for an unloaded actor. The related FACETS ch.1 line ("If the actor unloads, the claim is dropped") was corrected in the same closing round: it describes ControlMap's handle sweep only.

### APMF-B177 (SEV-3, CLOSED 2026-10-20 in 61e8805, `feat/apmf-weapon-standdown` -- marth ruled STAND DOWN, core/HandBlock.cpp Ch15Refuses) -- ch.15 weapon rule: W not carried empties the claimed hand (raised against 567850f)
Tier-3 review of feat/apmf-weapon-admission: "Every candidate for the claimed hands is refused, including bows and unarmed for a two-hand claim. The evaluate releases the picks at its start, so the slot ends up empty and the AI loses its whole melee/ranged branch. That is more than the brief's 'NOTHING else suppressed'. It is logged once after 5 s, so it is not masked." Option: stand down in the thunk when W is absent from `a_cc->inventory->inventoryItems[0..6]` (+0x08, stride 0x18, x7; confirmed 1.6.1170 at 0x80F10C / 0x80F3E2; SE and 1.7 unchecked; only at the evaluate sites, the pre-loop 0x0F site 0x80FF35 is not proven to see stable arrays), with a loud once-per-claim line. Alternative: keep as "client owns consequences" (MFO only claims a W it has just equipped).
**Resolution (61e8805):** marth ruled stand-down. While W is not among the controller's own `CombatInventory::inventoryItems[0..6]` (scanned at the evaluate's 0x0F sites only; layout measured identical on 1.6.1170 / 1.5.97 / 1.7.104; runtime `parentController` cross-check) the rule refuses nothing for that claim; one `STAND-DOWN` warn per claim, a `carried again` line on return. Non-evaluate sites reuse the last evaluate verdict for the same claim; with none the rule applies.

### APMF-B178 (SEV-4, CLOSED 2026-10-20 on `feat/apmf-weapon-standdown` (docs commit after 61e8805), Docs/DENY-COMPLETENESS-AUDIT.md row 15 residual (a)) -- state the measured pre-claim pick window (raised against 567850f)
"Residual (a) should state the measured window: <=1.0 s of AI time, collapsed by `dirty`." (CombatInventory::Update 0x80F380 sets the duration to 1.0f at 0x80F573; every pick assignment follows a 0x0F YES in the same pass.)
**Resolution:** row 15 residual (a) now states the measured window (<= 1.0 s of AI time, collapsed by `dirty`).

### APMF-B179 (SEV-4, OPEN, core/HandBlock.cpp ClaimedHands) -- ch.15 claimed hand follows where W is worn now (raised against 567850f)
"A one-hander the client wants in the LEFT hand is claimed as RIGHT until it is worn, and if W is disarmed or moved, the claim moves with it. The fix is the reserved param.ival explicit hand."

### APMF-B180 (SEV-5, lock half CLOSED 2026-10-20 in 61e8805; ranking half OPEN, core/HandBlock.cpp) -- CachedShape lock outside try; ch.15 facet ranking inconsistent with EquipGate (raised against 567850f)
"CachedShape's scoped_lock sits outside any try inside the engine's 0x0F frame (theoretical std::system_error); EquipGate's ch.15 deny ranks on Facet::kEquipWeapon, while HandBlock's uses FacetForEquipCats(a_cats), so shield and torch competitors rank on their own facet."
**Resolution (lock half, 61e8805):** `CachedShape` is `noexcept` with its lock inside a try (a failed lock reads as "not cached"). The ranking half is NOT changed in this round and stays open (the coordinator scoped the round to the lock).

### APMF-B181 (SEV-4, OPEN, core/HandBlock.cpp ScanCarried) -- ch.15 weapon rule: a staff W may stand down permanently (raised against 94093d3)
Follow-up review of feat/apmf-weapon-standdown: "EquipGate's own comment (EquipGate.cpp ~:674) says a ch.15 weapon FormID 'never matches a spell/staff subjectForm'. That suggests a staff's CombatInventoryItem stores the enchantment as its item, not the staff's weapon form. If so, the scan never finds a staff W, the rule stands down for good, and the AI could swap a held staff for a sword. It would be logged loudly, not hidden. MFO never force-holds staves (cast/Equip.cpp filters them with IsStaff()), so MFO is not affected. A third-party client could be." Confirm a staff item's form, then match on it or document staff claims as out of scope. Also: the arrays' single-writer trace was done on 1.6.1170 only (layout checked on 1.5.97 / 1.7.104).

### APMF-B182 (SEV-4, OPEN, channels/Travel.cpp ComposeLeg) -- a different-destination re-point needs two slots for one hop (raised against e9f9080)
A live leg re-pointed to a different destination holds the old slot and the new one until `DropOfferAndFreeSlot` frees the old a hop later, so at 8 concurrent legs a re-point of one of them fails loudly (kLeg_Failed) where the in-place path used to succeed. Capacity regression: document it or give it a designed loud degrade.

### APMF-B183 (SEV-5, OPEN, channels/Travel.cpp Leg::destChanged) -- sticky flag edge cases (raised against e9f9080)
An A->B->A re-point before one Compose leaves destChanged set and restarts the walk for the same destination; the StillOurs early return in Compose leaves the flag stale; the field comment wording overstates "cleared when Compose has run".

### APMF-B184 (SEV-5, OPEN, channels/Travel.cpp RE::TESPackageEvent) -- local definition may collide (raised against e9f9080)
The local `RE::TESPackageEvent` can collide if the CommonLib fork ever defines it. Move it to a project header guarded by `__has_include`, or drop it when the fork adds the real one.

### APMF-B185 (SEV-5, OPEN, core/ControlMap.cpp OnActorUpdate) -- stale "ONE reader-side mutation" comment (raised against e9f9080)
The comment in OnActorUpdate still says the obsTick fetch_add is the ONE reader-side mutation; obsLastPkg is a second.

### APMF-B191 (SEV-4, OPEN, core/Sources.cpp NoteClientRequest / CallerPriority, raised against 13231d6, Opus tier-3 review) -- a menu slider change before a source's first claim is lost
A menu slider change made before a source has filed its first claim this session is stored in `prio[]` and APMF_Sources.ini but not in `g_iniPrio`, so `NoteClientRequest` overwrites it with the stale `InitialPriority` on that first claim, and `GetClientFacetPriority` (ABI v29, `CallerPriority`) reports the stale value until then. Fix: update `g_iniPrio` from Pump's priority write, or keep `prio[]` when flipping a source to client.
### APMF-B186 (OPEN, author-raised, fix/apmf-ooc-substrate) -- a holder whose controller style gives magic no weight never selects the claimed spell
Field 2026-10-20: all 33 of Auri's spells scored exactly 0.000 under the substrate. The spell score is the live controller's combat style `magicScoreMult` (CombatController+0x38 -> TESCombatStyle+0x30) times the effect sum (AE 0x819AA0 -> 0x819C10), so a style with magicScoreMult 0 scores every spell 0 and D1 (`core/EquipPreference.cpp`: never lift an engine 0) keeps the claimed one at 0. Not a substrate input: MFO swaps that style to its cast style for a cast gambit in a real fight and applies none under a substrate. Blocks MFO's `bSubstrateCasts` default ON for such followers. Needs the MFO change (cast stance style while its substrate cast claim stands) or a marth ruling on D1 for a substrate claim. Proof line: `[substrate] 0x<f> CLAIM SCORE`. Never deferrable past the next substrate field cycle (rule 9 (b)).

### APMF-B187 (SEV-5, OPEN, author-raised, core/ActionGate.cpp MOVEMENT HOLD) -- hold records of destroyed threads linger until the close / load
A behaviour thread freed without a pop (AE 0x85D9F0 pops nothing) leaves its `{thread, node}` record until `ForgetMovementHolds` (a close that leaves no controller, or a load). Harmless by construction (any later use of the pair begins with an act(), which rewrites the record), but a table of 64 that fills logs one ERROR and the hold then degrades to the old failing behaviour for new windows.

### APMF-B188 (SEV-5, OPEN, author-raised, core/ActionGate.cpp MOVEMENT HOLD) -- the hold's release costs one tree restart
When the hold stops standing while the controller lives (real combat), the held node ends through SetFailed + Ascend: the Combat Parallel fails once and the controller rebuilds the tree, so the real fight's first behaviour frame is a restart (one frame, the same restart the old fail loop paid every frame). A success exit would leave the fight without a Movement half until the next restart, so the failure exit is deliberate.

### APMF-B189 (SEV-4, OPEN, Docs/STATUS.md + Docs/FACETS.md) -- cause 2 stated as fact (raised against df5635a, tier-3 review F3)
Reviewer: STATUS/FACETS state cause 2 as fact. Reword to "inferred; the CLAIM SCORE line decides": that magicScoreMult is 0 on Auri's controller style is an inference from 33 spells at exactly 0.000, not yet observed; the passive `[substrate] 0x<f> CLAIM SCORE` line decides it in the next field cycle.

### APMF-B190 (SEV-5, OPEN, MAP.md ActionGate entry) -- cross-reference APMF-B187 from the What-breaks entry (raised against df5635a, tier-3 review F4)
Reviewer: cross-reference APMF-B187 (stale hold records) explicitly from MAP's MOVEMENT HOLD "What breaks" entry (it currently reads "Backlog: APMF-B186..B188" only as a range).

### APMF-B192 (SEV-5, OPEN, core/ActionGate.cpp HoldUpdateThunk) -- frameEmpty is tested before the cur_node/phase anomaly check (raised against 6e5b1c5)
"In HoldUpdateThunk record path, frameEmpty() is tested before the cur_node/phase check, so the never-seen not-cur_node-in-phase-1 anomaly now drops the record instead of leaving the node untouched; the F2 guard would still release it on its next update." Fix: swap the two checks, or accept.

### APMF-B193 (SEV-3 WATCH, OPEN, channels/Travel.cpp cross-cell legs) -- the engine's Travel package may end DONE at its own radius before a MOVING destination is reached (raised against c206088, tier-3 review F2)
Reviewer (F2, observe only): with kTravel_CrossCells to a moving vendor, the engine can declare the travel package DONE at its own radius check while the vendor has walked on; the package then stands current in DONE and the actor stands until the stuck net. DONE-WHILE-WALKING already logs it. Candidate fix: a rate-limited fresh-slot restart of the leg when DONE-WHILE-WALKING fires on a flagged leg. marth decides after a field log; nothing is changed now.

### APMF-B194 (SEV-4, OPEN, channels/Travel.cpp XArrival) -- an UNLOADED actor inside the destination interior does not close the distance (raised against c206088, tier-3 review F3)
Reviewer (F3): an unloaded follower that has crossed into the destination interior may stay at the door while unloaded, so a flagged leg does not arrive until it loads (or the progress net ends it). Documented in Docs/APMF_API-REFERENCE.md#abi-v30-travel-cross-cells (use kIntent_Place). No code change.

### APMF-B195 (SEV-5, CLOSED in the F1 round, Docs/APMF_API-REFERENCE.md) -- "an older header cannot set the bit" was false (raised against c206088, tier-3 review F4)
Reviewer (F4): the claim that only v30 clients can set bit 5 is false (MFO already holds a local 1u << 5; any client can pass raw ival bits). Fixed: the reference now says the bit is honoured regardless of the client's ABI, like the v11 / v12 bits.

### APMF-B196 (SEV-5, CLOSED in the F1 round, channels/Travel.cpp Poll) -- the held-cell branch tested `leg.held` alone (raised against c206088, tier-3 review F5)
Reviewer (F5): the cell-hold branch must also test `leg.destKind == DestKind::kCell` so a held ref leg can never be looked up as a cell. Fixed in the F1 round.

### APMF-B197 (SEV-4, OPEN, channels/Travel.cpp HoldAfterFailure) -- held failures keep a package slot until the client releases / repoints (raised against 42b0e2b, tier-3 review F4)
Reviewer (F4, as relayed by the coordinator): "held failures keep a slot until the client releases/repoints (+ one extra slot for one hop on repoint; MFO loot STUCK path holds longest)". Deferred: slot pressure only (8 slots), same class as APMF-B173; no wrong behaviour while a slot is free.

### APMF-B198 (SEV-5, OPEN, channels/Travel.cpp RestartAfterDone) -- a Failed then StuckTimeout publish window (raised against 42b0e2b, tier-3 review F5)
Reviewer (F5, as relayed by the coordinator): "Failed then StuckTimeout publish window in RestartAfterDone". When a restart cannot start, StartLeg records kLeg_Failed and HoldAfterFailure records kLeg_StuckTimeout in the same poll; a client thread reading GetTravelLegState between the two sees a transient kLeg_Failed (with its own seq).

### APMF-B199 (SEV-5, OPEN, channels/Travel.cpp failure hold) -- a held BLOCKED leg that later arrives keeps reading kLeg_Blocked (raised against 42b0e2b, tier-3 review F6)
Reviewer (F6, as relayed by the coordinator): "a held BLOCKED leg that later arrives keeps reading kLeg_Blocked". The BLOCKED hold resumes the walk when the engine leaves Movement Blocked, but Poll skips a failure hold, so an arrival is never recorded until the client re-points or releases.

### APMF-B200 (SEV-4, OPEN, channels/CombatSubstrate.cpp Serve) -- a TESSpellCastEvent alone closes as fired with no 0x0B seen (raised against 608df67, tier-3 review F2)
Reviewer (F2, as relayed by the coordinator): "a TESSpellCastEvent alone closes as fired even when no 0x0B was seen (releasing false) while the census seat is installed (~1671); a non-animated cast (CastSpellImmediate) would read as an animated fire. Recommend a separate reason in that case." Deferred (SEV-4).

### APMF-B201 (SEV-4, OPEN, channels/CombatSubstrate.cpp Serve) -- the claim ending mid-release closes as "claim ended" and StopCombat cuts the cast (raised against 608df67, tier-3 review F3)
Reviewer (F3, as relayed by the coordinator): "claim ending mid-release closes as "claim ended" at info and StopCombat cuts the cast; reachable only with tree start > ~6 s or raised fSubstrateTtlSec. Recommend warn "claim ended mid-release"." Deferred (SEV-4).

### APMF-B202 (SEV-4, OPEN, channels/CombatSubstrate.cpp Serve / SpellCastSink) -- a Repoint during the release (raised against 608df67, tier-3 review F4)
Reviewer (F4, as relayed by the coordinator): "Repoint during release: flipping to concentration skips the release block and never clears releasing (TTL stays deferred); castEventForm keeps only the first event so an old-form event before a same-frame re-aim yields a false fired-not-released." Deferred (SEV-4).

### APMF-B203 (SEV-4, OPEN, channels/CombatSubstrate.cpp NoteCastBuilt) -- tree-outside-gate fires only on a caster BUILD (raised against 608df67, tier-3 review F5)
Reviewer (F5, as relayed by the coordinator): "tree-outside-gate fires only on a caster BUILD; a tree running outside the gate that never builds stays undenied until the 10 s start floor (window 2's movement started before the build at +983 ms). LEAF GATE probe diagnoses next cycle." Deferred (SEV-4); the next field cycle's LEAF GATE line is the input.

### APMF-B204 (SEV-5, OPEN, channels/CombatSubstrate.cpp Install, Docs/VERIFIED-ADDRESSES.md) -- the TESSpellCastEvent source has no verified row (raised against 608df67, tier-3 review F6)
Reviewer (F6, as relayed by the coordinator): "ScriptEventSourceHolder +0xE18 TESSpellCastEvent source has no VERIFIED-ADDRESSES row (reviewer checked 1.5.97/1.6.1170/1.7.104 destructor funclets, layout consistent; HitSink/CombatEventSink share the gap). Add a row later." Deferred (SEV-5).

### APMF-B205 (SEV-5, OPEN, channels/CombatSubstrate.cpp) -- the file is 3051 lines (raised against 608df67, tier-3 review F7)
Reviewer (F7, as relayed by the coordinator): "CombatSubstrate.cpp 3051 lines, split stays APMF-B123." Deferred (SEV-5); the split is APMF-B123's own brief.

### APMF-B206 (SEV-5, OPEN, core/CastProxy.cpp Configure) -- the proxy carries no KEYWORDS (raised by the author of fix/apmf-gate-probe-proxy-name, 2026-10-26, by the brief's direction)
The proxy now copies the source spell's name and menu display object (the equip slot stays the factory's EitherHand) (field 2026-10-26d: the nameless proxy showed generic active-effect text and MFO logged "CAST (unnamed)"), but NOT its `BGSKeywordForm` keywords. Copying them means an engine-allocated keyword array on a runtime-minted form, which INVARIANTS #19 would then need reset (freed or emptied) in `ResetAll` before the load-time purge, and a re-point must replace it. Anything that tests a spell's keywords (perk entry conditions, a magic-skill or a list's keyword reads, MFO's own classification) sees the proxy as keywordless. Deferred by the brief (marth's coordinator, 2026-10-26): its own scoped change with the #19 reset designed in.

### APMF-B207 (SEV-4, OPEN, core/ActionGate.cpp ProbeUnresolvedAct / channels/CombatSubstrate.cpp NoteCastBuilt) -- the "resolved to another id" case is not probed (raised against 6ea134d, tier-3 review F3)
Reviewer (F3, as relayed by the coordinator): "The 'resolved to another id' case is not probed: a tree whose control resolves to a non-holder id gets kNotHolder from FindSlot and logs nothing, and that is exactly the hypothesis the CAST BUILT comment raises." Also noted: the SpellItem Create default equip slot on 1.7.104 was not disassembly-checked (moot once F1 is applied: the slot is no longer touched). Deferred (SEV-4).

### APMF-B208 (SEV-4, OPEN, core/PapyrusCaller.cpp CallThunk :145-161) -- the !orig fallback calls the engine Call (raised against 7f1faa4, tier-3 review of ClickUp 86e3q082a step 1)
Reviewer, verbatim as relayed: "the !orig fallback (:145-161) calls the engine Call; wrong for a hand-written IFunction not deriving NativeFunctionBase; comment "every native's Call is it or forwards to it" too broad." Reasoning: the fallback is unreachable by construction, but if it ever runs for an IFunction whose own Call is not the engine's, the engine Call would mis-handle it. Closure: no generic fallback is valid; narrow the comment, and decide between refusing the call (it cannot be refused) or keeping the fallback only for vtables whose recorded module is SkyrimSE.exe.

### APMF-B209 (SEV-4, OPEN, core/PapyrusCaller.cpp kCap :64, :91) -- the 2048-vtable cap (raised against 7f1faa4)
Reviewer, verbatim as relayed: "2048-vtable cap (:64, :91) may be reached on large lists; kCap=16384 (256 KB) removes it; check 'distinct vtables' in the first field log." Closure: read the install line's distinct-vtable count from the first field run; raise kCap to 16384 if it is anywhere near 2048 (or unconditionally).

### APMF-B210 (SEV-4, OPEN, STEP-2 PREREQUISITE, core/PapyrusCaller.cpp Describe / ProbeEquip :446) -- misattribution trap for step 2 (raised against 7f1faa4)
Reviewer, verbatim as relayed: "misattribution trap for step 2: an equip made synchronously while an unrelated native runs on that thread is attributed to that native's script (e.g. AddItem -> container-changed sink in a C++ mod equips); step 2 must require an equip native (or a_scriptPath) before ranking, and should rank by the VMAD-owning file, not only definer (a script attached by an override on a vanilla ref shows definer=Skyrim.esm; :446)." marth's ruling 2026-10-27 is "Defining plugin"; step 2 reconciles that with the VMAD-owner point and presents it to marth before ranking.

### APMF-B211 (SEV-5, OPEN, core/PapyrusCaller.cpp CallThunk :163-171) -- thunk restore is not exception-safe (raised against 7f1faa4)
Reviewer, verbatim as relayed: "thunk restore not exception-safe (:163-171), RAII guard." Closure: restore t_cur from a scope guard's destructor.

### APMF-B212 (SEV-5, OPEN, core/PapyrusCaller.cpp CallThunk) -- g_calls is a contended counter (raised against 7f1faa4)
Reviewer, verbatim as relayed: "g_calls fetch_add contended cache line on every call." Closure: a thread_local count summed only for the install line, or drop the counter.

### APMF-B213 (SEV-5, OPEN, core/PapyrusCaller.cpp Install :403-405) -- a silent safe_write failure still counts as seated (raised against 7f1faa4)
Reviewer, verbatim as relayed: "silent safe_write failure still counted as seated (:403-405)." Closure: re-read the slot after write_vfunc; count and log a slot that does not hold the thunk.

### APMF-B214 (SEV-5, OPEN, core/PapyrusCaller.cpp Install :391-398 and the full-table error) -- two log wordings (raised against 7f1faa4)
Reviewer, verbatim as relayed: "engineForeign log wording (:391-398)." and "full-table message prints g_used of kCap while the limit is kCap/2." Closure: reword the engine-vtable line; print the real limit (kCap/2) in the full-table line.

### APMF-B215 (SEV-4, OPEN, core/BehaviorTreeCatalog.h:10 + Docs/BEHAVIOR-TREE-CATALOG.md, against 1f08dc3, Opus review of fix/apmf-cleanroom-combatbehavior) -- derivation script not committed
It says "tables regenerated by the script recorded there", but no script is committed or recorded. Docs/BEHAVIOR-TREE-CATALOG.md:30-48 gives only the method steps; line 176 mentions an unrecorded comparison script. For a clean-room provenance record, either commit the derivation script (e.g. under tools/) or reword both places to "the method below".

### APMF-B216 (SEV-5, OPEN, against 1f08dc3) -- stale kCastContextNodes references
The symbol no longer exists, but these still name it: ActionGate.cpp:1568 comment (apmf::cbt::kCastContextNodes); spec.json layout_facts[36].where = "BehaviorTreeCatalog.h kCastContextNodes"; spec.json layout_facts[19] cites "BehaviorTreeCatalog.h:70-94", a line range carried over from the old file.

### APMF-B217 (SEV-5, OPEN, against 1f08dc3) -- protocol text slightly off for some runtimes
BehaviorTreeCatalog.h:37-41 says ForceFail act pushes through the push helper (AE id 33171) on all three; SE 0x8218A0 inlines it (still a 4-byte push). The doc's SetFailed shows mov [rcx+0x148],dl; the real code is movzx eax,dl; mov [rcx+0x148],eax on AE / 1.7.104 and a setne form on SE. Values are right.

### APMF-B218 (SEV-5, OPEN, against 1f08dc3) -- Leaf.* label mismatch in 10 spec.json rows
Their Leaf.* names (and so the VerifiedAddresses.h labels and VERIFIED-ADDRESSES.md row names, e.g. Leaf.CombatBehaviorDisableAim, ...ChildSelector_ValueChildSelector_) keep the old spellings while construct uses the new ones. Labels only show in logs; align at the next regeneration. (Also: MFO's tools/splitcheck _data_diff crashes on set | tuple, MFO tools backlog.)

### APMF-B219 (SEV-5, OPEN, native/APMF_API.h v27 comment and native/plugin.cpp:309, against 4996523) -- two stale code comments
APMF_API.h v27 comment says the substrate runs only on 1.6.1170/1.5.97 (it runs on 1.7.104 since 0.14.0). plugin.cpp:309 says bPapyrusCaller defaults to 1 (code and shipped INI default 0). Closure: fix both comments (APMF_API.h is byte-shared with MFO, so mirror the edit there).

### APMF-B220 (SEV-4, OPEN, core/AttackSelectProbe.cpp, against 162a446, Opus tier-3 review of feat/apmf-attack-select-probe) -- lock + allocations on combat threads
Reviewer, verbatim as relayed: "armed path takes g_mx on combat threads with allocations under the lock (pickHist emplace_back, deque push; Heartbeat copies Stats under the same lock): acceptable for a default-OFF probe, do NOT copy into the real seat." Closure: the real ch.18 seat must not reuse this pattern (lock-free counters / per-thread buffers).

### APMF-B221 (SEV-5, OPEN, core/AttackSelectProbe.cpp StatsFor, against 162a446) -- clock read before the lock
Reviewer, verbatim as relayed: "StatsFor r.ms taken before the lock: a backwards a_now < lastRecMs wraps and opens a spurious fight (:284-288)." Closure: take the time under the lock, or treat a_now < lastRecMs as the same fight.

### APMF-B222 (SEV-5, OPEN, core/AttackSelectProbe.cpp Install, against 162a446) -- context records undrained when no leaf is watched
Reviewer, verbatim as relayed: "if leaves==0 while context seats installed, g_armed stays false and context records pile to the queue cap undrained (:506-507, :521)." Closure: drain in Heartbeat when either half is armed.

### APMF-B223 (SEV-5, OPEN, docs, against 162a446) -- CommonLib's ATKD bit 3 name is wrong
Reviewer, verbatim as relayed: "CommonLib names ATKD flag bit 3 kChargeAttack but it is Left Attack (0x08); add a line to the ENGINE notes doc; future seats must not trust that enum name." (Evidence: the melee gather AE 0x8A3ACC tests bit 3 against ATTACK_TYPE 2; CommonLib's own BGSAttackData::IsLeftAttack reads kChargeAttack.)

### APMF-B224 (SEV-5, OPEN, process, core/ActionGate.cpp, against 162a446) -- ActionGate.cpp past the split threshold
Reviewer, verbatim as relayed: "ActionGate.cpp 2342 lines, the next brief must propose the split (RangedProbe / PFP / leash seams) as its own round."

### APMF-B225 (SEV-5, OPEN, pre-existing, channels/CombatEntry.cpp hold EndHold, against 94c35f6, Opus review of fix/apmf-hold-repin-nopath) -- an ended Track under a live claim
Reviewer, verbatim as relayed: "An ended Track can stay ended under a live claim. When EndHold runs but EndClaim / EndHeld find no winning claim naming the target (the entry or pin is not the winner or is already ending), the Track is ended but the claim stays live and nothing is released. entryEnded / pinEnded only take effect on a later SetSlot, so with no Repoint the watchdogs never run again for that live claim. Rare, not introduced here."

### APMF-B226 (SEV-5, OPEN, core/EquipPreference.cpp OnScore, against 3186270, Opus tier-3 review of feat/apmf-explained-zero-lift) -- the no-rival floor also lifts tiny non-zero scores
Reviewer, as relayed by the coordinator (the review log holds no fuller text): "floor fix lifts non-zero (0, 0.01) scores to 0.01 and touches ch.26 weapon declarations, harmless." Reasoning: with no rival the target is now kEquipPrefStepAbs instead of 0, so an engine score in (0, 0.01) on a lifted item rises to 0.01. It only reorders items that already lead their category. Deferred under rule 9.

### APMF-B227 (SEV-5, OPEN, core/EquipPreference.cpp ZeroLineDue, against 3186270, Opus tier-3 review of feat/apmf-explained-zero-lift) -- the [ch.8b zero] throttle keys on a pooled form id
Reviewer, as relayed by the coordinator: "g_zeroLines throttle keyed on form id, can be a pooled 0xFF proxy, logging only." Reasoning: a delivery-flip proxy id is reused across spells by the pool, so a recycled id can suppress the first line for a different spell for up to 10 s. Logging only, no decision reads it.

### APMF-B228 (SEV-5, OPEN, core/CastClassify.cpp VerifyZeroModel, against 3186270, Opus tier-3 review of feat/apmf-explained-zero-lift) -- the self-check does not cover the per-row step or the visitor
Reviewer, as relayed by the coordinator: "install self-check proves row functions and bytes, not the per-row step or effect visitor (verified statically, exact-build gated)." Reasoning: KEEP-BEST (AE 0x81DBA0 / SE 0x7815A0 / 1.7.104 0x833090) and the visitor (AE 0x81D830) were verified by disassembly on all three builds and the seat is exact-build gated, so a drift would need a new executable, which the runtime gate already refuses.
