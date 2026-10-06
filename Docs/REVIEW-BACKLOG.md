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

### APMF-B5 — ch.17 Engage log still says "(SetEquipSet, ABI v7)"
- **Raised:** Fable tier-3 on `a369e9f` (feat/equip-authority-v8), SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** ":364 Engage log \"(SetEquipSet, ABI v7)\"".
- **Reasoning:** `channels/EquipAuthority.cpp` `Engage` names only the v7 call; v8's `SetEquipSetEx` is the other declaration path. Cosmetic log text; a reader grepping for v8 adoption would not see it here.
- **Assigned:** none; drain with the next ch.17 batch.

### APMF-B6 — the hand-EQUP resolve error over-claims "every handed entry skipped"
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** ":226 error text over-claims \"every handed entry skipped\"".
- **Reasoning:** `Enforce`'s `resolveHandSlots` logs "every handed entry in this pass is skipped" when EITHER hand form fails to resolve, but the per-entry check skips only an entry whose OWN hand slot is null (`if (!equipSlot) { ++unresolved; continue; }`); with one hand resolving, entries for that hand are still equipped. Both forms come from Skyrim.esm at load index 00, so the branch is not expected to fire; the text should say "every entry declared for that hand".
- **Assigned:** none.

### APMF-B7 — the once-logged claim refusal reason can be stale after a later Install refusal
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "ControlMap.cpp:82-88 once-logged refusal reason vs a later Install refusal".
- **Reasoning:** `EnqueueRequest` logs the seat-not-installed refusal ONCE with `NotInstalledReason()` at that moment. A client that requests before kDataLoaded logs "not yet installed (Install runs at kDataLoaded)"; if `Install()` then refuses for another reason (INI off, site-verify), that reason is never printed by this line (the `[apmf][equip-sink]` install line still carries it). Fix shape: log once PER DISTINCT reason string, not once ever.
- **Assigned:** none.

### APMF-B8 — the "seat not installed" branch in Enforce is unreachable under the v8 refusal
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "the unreachable \"seat not installed\" branch in Enforce :176-186".
- **Reasoning:** since `EnqueueRequest` refuses a ch.17 claim while the seat is down, no claim exists for `Enforce` to run against when `Installed()` is false (Install runs once and never uninstalls), so the `g_seatMissing` error path is dead code. Kept as a defensive guard; its message ("declaration ... NOT enforced") describes a state that cannot arise. Either delete it or reword it as an invariant-violation log.
- **Assigned:** none.

### APMF-B9 — queued-slot carriage through the engine's deferred apply is unobserved
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5 (probe requirement added to `Docs/INTEGRATION.md` criterion 7 in the closing round).
- **Finding (verbatim, as relayed):** "queued-slot carriage unobserved (add the probe requirement: one `hand=left` equip line followed by visible dual-wield)".
- **Reasoning:** `Enforce` passes the `BGSEquipSlot` to `EquipObject(queue=true)`; the slot's survival through the AIProcess queue and the `QueuedApply` re-entry (38906/37950) is argued from the `EquipData` layout, not observed. CLAUDE.md principle 5: a path exists is not a path runs. Closed by the field log, not by code.
- **Assigned:** the first v8 deck session.

### APMF-B10 — `MinReleaseForAbi` v7/v8 entry must be named at the cut
- **Raised:** Fable tier-3 on `a369e9f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5 (release-checklist item).
- **Finding (verbatim, as relayed):** "MinReleaseForAbi \"first release after 0.9.4\" must be named at the cut (release checklist)".
- **Reasoning:** `core/ClientAPI.cpp MinReleaseForAbi` returns the placeholder "the first release after 0.9.4" for ABI v7/v8 because no release carrying them exists yet. Whoever cuts the next release replaces it with the actual version string in the same commit that bumps `project(APMF VERSION ...)`.
- **v9 (2026-09-16, `feat/equip-authority-v9`):** ABI v9 (`SetEquipScope`) joins the same placeholder entry (`case 7: case 8: case 9:`). v7, v8 and v9 all ship together in that first release; the cut names ONE version string for all three.
- **Assigned:** the next release cut.

### APMF-B11 — v9 enforce pass counts an already-worn unowned item as `skipped-unowned`
- **Raised:** Fable tier-A on `3d5cab8`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "skip-before-worn miscounts an already-worn unowned item as skipped-unowned".
- **Reasoning:** `channels/EquipAuthority.cpp Enforce` runs the scope skip (`competes & denied` / `competes & ~owned`) BEFORE the inventory/worn test, so a declared item the actor already wears in an unowned category is counted and named as skipped rather than `already-worn`. The pass never equips it either way (correct); only the counter and the once-per-signature warn text are off. Fix = move the scope test after the worn test, or count worn-and-unowned separately.
- **Assigned:** the v9 backlog drain (before the observe-only flip).

### APMF-B12 — v9 skip warn is once-per-distinct-from-last, not once per signature
- **Raised:** Fable tier-A on `3d5cab8`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "warn is once-per-distinct-from-last, not once per signature".
- **Reasoning:** `ActorMemory::lastSkipSig` holds ONE signature, so alternating declarations A, B, A re-warn for A each time it returns; a true once-per-signature guard needs a bounded set per actor. Bounded, logged, never a pile-up; cosmetic.
- **Assigned:** the v9 backlog drain (before the observe-only flip).

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

### APMF-B15 — the observe log names package slot 0 whatever slot would be used
- **Raised:** Fable tier-3 on `2d6108f`, SEV-5; text as relayed by the coordinator.
- **Severity:** SEV-5.
- **Finding (verbatim, as relayed):** "observe log prints g_pkg[0] whatever slot would be used".
- **Reasoning:** In observe mode no slot is allocated (nothing is claimed), so the line had to name SOME package and named the first. Cosmetic, and the fix landed incidentally in the same round the intent was renamed — the observe line no longer names a package at all, it names the destination, the radius and the basis. Kept as a record of the finding; re-check at the drain that no observe line names a slot it did not allocate.
- **Assigned:** verify-only at the next drain.

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
Raised against `5dcefa7` (`feat/apmf-target-pin`), tier-A Opus 5.5 review 2026-09-25, finding F2 (reviewer log `scratchpad/agentlogs/review-apmf-target-pin.md`). The ch.20 seats run inside the actor's own `UpdateCombat` job and read `combatController` (and, since the source-deny rework, its `combatGroup` under the group's read lock). A `StopCombat` on ANOTHER thread against the same actor in that window (a script's `Actor.StopCombat`, a package evaluation, a kill -- MFO ENGINE_NOTES 0.47 item 4) frees the controller inline, with no refcount and no lock, under the read. Decision (coordinator, 2026-09-25): accepted as a known exposure, identical to vanilla's own `UpdateCombat` body and to MFO's Targeting hook. It is written into `channels/TargetPin.cpp`'s header. **Closure:** a `StopCombat` seat (Character vtable slot 0xE5, 1.6.1170 `0x6B70A0` / 1.5.97 `0x625920`) sharing a per-actor lock with the combat seats -- assigned to the combat-substrate task, not this repo's ch.20 work.

### APMF-B27 (SEV-4) -- ch.20 vs MFO's own UpdateCombat target hook (hook order)
Raised against `5dcefa7`, same review, finding F3. MFO's `native/Targeting.cpp` hooks `Character::UpdateCombat` (slot 0xE4) and writes `currentCombatTarget` + `targetHandle` after the engine for any follower with a gambit latch. Both plugins install at kDataLoaded, so whichever `write_vfunc` runs last wraps the other; the order is SKSE's plugin dispatch order, not ours. Since the source-deny rework ch.20 no longer writes at 0xE4 at all (its answer lands inside `UpdateTarget`), so MFO's hook, running after the whole update, writes LAST in every order and overrides the pin for any actor MFO also latched. The ch.20 observer reports this as `OVERWRITTEN` only when MFO's hook is chained INSIDE it (MFO installed first); in the other order the observer's check runs before MFO's write and sees nothing, so the log cannot be relied on to show it. **Closure:** MFO retires its own write for any actor with a live ch.20 claim (MFO's port task).

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
- SEV-4: TryGetCastSeatClaimForForm returns the first matching claim in vector order, not the BetterClaim winner. Diverges only when a later same-hand same-spell claim has a strictly higher basis (never under MFO's uniform kOwnBasis). Fix: pick the BetterClaim-best among matches.
- SEV-5: the refused-mint path (zero/duplicate FormID) drops the created form without freeing it (leaks until load purge; error-logged every time).
- SEV-5: the F2 log says "plain deny of its hand", but AllowedCastForHand (Allowance.cpp:148) still admits the original kSelf spell on that hand, so the AI may still self-heal by its own vanilla choice. Nothing is forced. Wording only.
- Note: the save callback is main-thread synchronous per SKSE64 source (Hooks_Papyrus.cpp SaveGlobalData_Hook); the new PreSaveSweep "on the Drain thread: yes/NO" line confirms it in the field. A NO is a SEV-1 threading finding.
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

### APMF-B52 (SEV-4) -- SpaceQuery reads a skipped pick as "no hit"
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
### APMF-B55 (SEV-4) -- ch.24: the pursuit-leash test is coarse
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

### APMF-B57 (SEV-4) -- ch.24: FindWeapon looks only inside the bound
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

### APMF-B63 (SEV-4, tooling) -- hand-claim block: the 1.7.104 Torch / OneHandedBlock 0x0F rows live on APMF's own idmap CSV
Raised against `1cc0bc9` (`fix/apmf-hand-claim-blocks-equip`, Opus tier-3 review, MERGE), 2026-10-06, finding F6. As
relayed: the 1.7.104 Torch row depended on an untracked `_research` CSV. Fixed on the branch by committing the evidence
as `tools/verified_addresses/idmap-1.7.104-apmf.csv` and writing the exact reproducing command into
Docs/VERIFIED-ADDRESSES.md (hand-maintained section). OPEN residual: move AE ids 45069 (Torch 0x0F, 0x819760; SE 43850)
and 45056 (OneHandedBlock 0x0F, 0x819300; SE 43837) into the CommonLibSSE-NG fork's 1.7.104 id table / idmap at the
next fork revision, then retire the APMF-local CSV rows.

### APMF-B64 (SEV-4) -- hand-claim block: a refused ch.17 equip is never re-issued
Same review, finding F4. As relayed: "a refused ch.17 equip is never re-issued; document it in INTEGRATION".
Reviewer's reasoning (log `scratchpad/agentlogs/review-apmf-handblock.md`): channels/EquipAuthority.cpp has no tick;
it equips only on a declaration, so a declared weapon the hand-claim block refused while a cast claim held the hand
is not equipped again when the hand frees. The client must re-declare. Fix when drained: document it in
Docs/INTEGRATION.md (or re-issue on the hand's release).

### APMF-B65 (SEV-4/5, docs) -- hand-claim block: `bEquipDenyScript` now reaches cast-only actors
Same review, finding F5. As relayed: "`bEquipDenyScript` now reaches cast-only actors; fix the INI comment scope".
On an actor with no ch.17 claim, the sink step reads the INI `bEquipDenyScript` (the claim flags are 0), so with it
set a Papyrus / console equip into a hand a cast claim holds is refused too. The APMF.ini comment still describes it as
the ch.17 declared-set exemption only. Fix when drained: widen the comment (or scope the INI to ch.17 actors).

### APMF-B66 (SEV-5 x2) -- hand-claim block review follow-ups F9, F10
Raised against 1cc0bc9 (`fix/apmf-hand-claim-blocks-equip`, Opus tier-3 review MERGE), 2026-10-06.
- F9 (SEV-5): an extra handle lookup per combat leaf. `ActionGate.cpp:890` resolves the actor handle on every leaf `act()` of every combatant whenever any actor is watched. Cost only, no correctness problem.
- F10 (SEV-5): comment and MAP wording. "EitherHand/null -> both" at the 0x0F seat is unreachable: CombatInventory AddItem (AE 0x811AC0) splits a multi-parent slot into per-parent items (Clone, vfunc 0x0D), so a one-hander becomes a RightHand item plus a LeftHand clone.
- DECIDED (marth 2026-10-06), review F2: a direct `EquipObject` from another SKSE DLL into a hand held by a cast claim is REFUSED. marth: "mfo asks harbinger to block it. Later its arbitrated". Papyrus / skse64 script equips, console and player-menu equips stay exempt. Becomes per-facet priority arbitration with the weighting rework (86e3jwmk7 / 86e3k3upc).

### APMF-B67 (SEV-4 x2, SEV-5 x2) -- spells-only floor (ABI v20) review: deferred F3-F6
Raised against `7bf4548` (`fix/apmf-floor-spells-only`, Opus tier-3 review, nothing above SEV-3), 2026-10-06. Text verbatim as relayed.
- F3 (SEV-4): "A CHANGELOG entry for ABI v20 sits under the RELEASED `## v0.10.0` heading. v0.10.0 is ABI 17, tagged 2026-10-05. This is a pre-existing pattern for v18/v19 too. Fix at the next cut by moving all post-0.10.0 entries into an Unreleased block." Reasoning: release notes would credit v0.10.0 with features it does not ship; harmless until a cut, so drained at the cut.
- F4 (SEV-4): "The `MinReleaseForAbi` text at ClientAPI.cpp:238, \"the first release after 0.9.11\", is stale for 18/19/20. Name the real release at the cut." Reasoning: log text only (the "client too new" refusal line); the real release name is only known at the cut.
- F5 (SEV-5): "In INVARIANTS #14b, the next bullet still says MFO calls `fn(kABIVersion)` and cites the dead `native/APMFBridge.cpp:398-403`. MFO actually asks for `kRequestAbi=10` (Bridge.cpp:922)." Reasoning: stale doc; the amended #14b text already states the current fact, the older bullet contradicts it.
- F6 (SEV-5): "With a staff, R on a spells-only floor and L under a claim, the sink verdict reads \"hands R+L held by a cast claim\". The refusal is correct; only the label is loose." Reasoning: `core/EquipSink.cpp` step 1a picks one label for both hands; a mixed hold falls to the claim label. Log wording only.
