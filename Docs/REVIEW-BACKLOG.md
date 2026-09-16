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
- **Assigned:** the next release cut.

---

## DRAINED

_(none yet)_
