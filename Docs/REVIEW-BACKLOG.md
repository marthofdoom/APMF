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
