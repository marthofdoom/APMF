# APMF_API.h history notes

Historical notes moved out of `native/APMF_API.h` on 2026-10-12 (marth: "Any historical note should be added to a note file"). Each section is the original comment text, verbatim, grouped by ABI version and symbol. The header is the contract; this file is the record of how it got there. Long-form explanation lives in [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md).

<a id="append-only-discipline"></a>
## Header intro: the append-only contract, a closing aside

Moved verbatim from `native/APMF_API.h` (line 19 at 9d01cdf). The header keeps a pointer to this anchor.

```text
(The same discipline any byte-shared C-ABI header between two separately
compiled DLLs needs.)
```

<a id="transport-choice"></a>
## Header intro: why an exported query function (ABI v1)

Moved verbatim from `native/APMF_API.h` (line 53 at 9d01cdf). The header keeps a pointer to this anchor.

```text
(An exported query fn was chosen over the SKSE-messaging handshake MEO uses: it
is synchronous, has no message-ordering or sender/receiver routing subtlety, and
hands over a POD struct with no vtable. The struct-of-fn-pointers shape is the
ABI contract; the transport is just how you get the pointer.)
```

<a id="kintent-selectspell-retired"></a>
## kIntent_SelectSpell (ABI v2): the retired +ACT ival bits

Moved verbatim from `native/APMF_API.h` (line 216 at 9d01cdf). The header keeps a pointer to this anchor.

```text
RETIRED (feat/ai-cast-seats-impl): `ival` bits 0-1
(the +ACT hand mode) and bit 2 (kActFlag_Drive, the
+ACT drive opt-in) NO LONGER MEAN ANYTHING. The
forced cast drive they selected is gone; a cast that
must actually HAPPEN is now a kIntent_Cast (ch.8b)
claim, which makes the NPC's own AI cast natively
through the engine seats (see kIntent_Cast below).
```

<a id="api-v6-preamble"></a>
## APMF_API_v6 (ABI v6): the blind spot it closed

Moved verbatim from `native/APMF_API.h` (line 1793 at 9d01cdf). The header keeps a pointer to this anchor.

```text
The v6 interface: APMF_API_v5's members verbatim (via prefix EXTENSION, same
shape as every prior revision), then two appended cast-claim OBSERVABILITY
slots. Neither changes what APMF does -- both are read-only queries closing
an MFO<->APMF blind spot diagnosed 2026-09-06: a client that called
RequestCast had no way to learn (a) the delivery-flip proxy FormID APMF
minted internally for that claim, so it could only ever match the ORIGINAL
spell and never recognised its own proxy's cast, and (b) whether its claim
had already auto-expired at the TTL (design.md §5a), so it kept believing a
long-dead handle was still live. This header is BYTE-SHARED with MFO: the
declaration below is authoritative and must be mirrored byte-identically on
the client side.
```

<a id="kcastflag-denyhandonly-problem"></a>
## kCastFlag_DenyHandOnly (2026-09-06): the field problem it solves

Moved verbatim from `native/APMF_API.h` (line 1211 at 9d01cdf). The header keeps a pointer to this anchor.

```text
THE PROBLEM IT SOLVES. A single-hand kIntent_Cast claim
scopes its deny to its OWN hand by design, so the other
hand stays fully AI-governed -- and the field showed the
AI using it: 25 Stone Runes, 6 Poison Sprays and 8 Raise
Zombies all charged on the UNCLAIMED hand while the claim
held the other one (2026-09-06 diagnosis, RC3 / audit rows
P3-P4). A client that wants the follower to cast ONLY what
it asked for had no way to say so, because there is no
spell it wants on that second hand -- only silence.
```

<a id="kcastflag-denyhandonly-evidence"></a>
## kCastFlag_DenyHandOnly (2026-09-06): the floor does not close the client's direct force

Moved verbatim from `native/APMF_API.h` (line 1298 at 9d01cdf). The header keeps a pointer to this anchor.

```text
VERIFIED, 2026-09-06, because the natural worry is that a
client's own DIRECT force-cast would be denied by its own
floor: it is NOT. A direct
`GetMagicCaster(kInstant)->CastSpellImmediate(...)` force
does not consult CheckCast at all, so this gate cannot see
it -- MagicCaster::CastSpellImmediate is vtable slot 0x01
and CheckCast is 0x0A, and the engine's CastSpellImmediate
never calls it. Two independent pieces of field evidence,
not a reading of the header: (1) MFO hooks CheckCast on the
SAME ActorMagicCaster vtable[0] slot 0x0A and records, in
its own source, that "the kInstant ConcProxy direct force
(CastSpellImmediate) does NOT deliberate through these
hooks, so it is never vetoed and needs no bound"
(MFO CasterConsent.cpp); (2) MFO's ENGINE_NOTES 0.9
measured a follower with ZERO magicka casting through
CastSpellImmediate forever -- a CheckCast consult would
have refused it with kMagickaInsufficient on the first
try. A floor therefore closes the hand to the AI's
deliberation, which is exactly and only what it claims to
do; it does not close the client's own direct force.
```

<a id="isequipauthorityenforced-why"></a>
## APMF_API_v8::IsEquipAuthorityEnforced (ABI v8): why it exists

Moved verbatim from `native/APMF_API.h` (line 1933 at 9d01cdf). The header keeps a pointer to this anchor.

```text
WHY (ABI v8, MFO wiring review SEV-2 F1): a client that turns its OWN
equips off on a successful kIntent_EquipAuthority claim must be able to
tell "APMF holds the set" from "APMF is only watching". Paired with the
v8 rule that a kIntent_EquipAuthority Request/RequestEx returns
kInvalidHandle while the seat is NOT installed (so the client's documented
degrade path runs), this call distinguishes the remaining case: claimed
and installed, but observe-only.
```

**2026-10-13, no ABI change (ClickUp 86e3pyhxv).** What `IsEquipAuthorityEnforced` returns changed in practice:
`bEquipObserveOnly` now defaults to 0 and `bEquipEnforceArmor` is retired, so with the shipped defaults it is true and
every declared category is enforced. Two ch.17 behaviours a client sees: a claim binds its owner (the client's own
direct equip of an item its declaration refuses is refused; it no longer ties with its own claim), and a worn item in
a DENIED category is unequipped by the enforce pass. The header comments that still say "APMF never unequips" are
stale until the paired MFO header change.

<a id="abi-history"></a>
## ABI v10 to v29: what each version added

Moved verbatim from `native/APMF_API.h` (line 96 at 9d01cdf). The header keeps a pointer to this anchor.

```text
ABI v10 (2026-09-22) adds kIntent_Travel (ch.19) + TravelFlags and NOTHING
else: ch.19 rides the EXISTING Request/RequestEx/Repoint/Release slots, so
there is no APMF_API_v10 struct and no new function pointer. A v9 client is
byte-unaffected and keeps working against this build unchanged.

ABI v11 (2026-09-23) adds APMF_API_v11: two read-only SPACE QUERIES
(FindEmptySpace, FindHostilesInSpace) and their POD structs, plus the
kCastFlag_AtPosition bit that lets a kIntent_Cast RequestEx carry a world
POINT (APMF_Param.pos), and the kTravel_ToPosition bit that lets kIntent_Travel
walk to one. A client must see abiVersion >= 11 before it calls a v11 slot or
sets either bit: an older APMF ignores kCastFlag_AtPosition and would treat the
request as an ordinary actor-target cast claim (it refuses a zero-form travel).

ABI v12 (2026-09-24) adds APMF_API_v12: ONE read-only slot, GetTravelLegState,
and its POD struct APMF_TravelLegInfo, so a client can read WHY its travel leg
ended (APMF ends a leg but never releases the client's claim, so without it an
ended leg is indistinguishable from a package theft). It also adds the travel
GAIT bits (kTravel_SpeedSet + a 2-bit speed) and the BLOCKED leg end. A client
must see abiVersion >= 12 before it calls the slot or sets a gait bit: an older
APMF stores the unknown bits and walks at its authored speed without a word.

ABI v13..v16 (2026-09-25) each add ONE intent and NOTHING else (no struct, no slot, no
APMF_Param field; each rides RequestEx/Repoint/Release, a v(N-1) client is byte-unaffected):
v13 kIntent_TargetPin (ch.20), v14 kIntent_CombatEntry (ch.21), v15 kIntent_CombatReentryDeny
(ch.22, param.fval = the window in seconds), v16 kIntent_PursuitLeash (ch.23, param.target =
the anchor, param.fval = the radius). A client must see abiVersion >= N before it asks for
that intent: an OLDER APMF has no channel for it and REFUSES the request (kInvalidHandle, "no
channel serves intent <n>" in its log) -- the documented degrade: do it your own way. (A
withdrawn, never-released v16 draft carried the leash as ch.7 bit kCombatActionCat_Pursuit.)

ABI v17 (2026-09-25) gives kIntent_Idle (ch.12) a FORM and a TARGET and NOTHING else: no
new intent, no struct, no function-pointer slot, no APMF_Param field. The idle rides the
EXISTING param.form (the TESIdleForm to play) and param.target (the reference to play it
at), which v1..v16 accept and ignore for this intent. That is why no field is added: the
two values are a form and a ref, exactly what those fields already carry for every other
intent, and a zero form still means the v1 behaviour. There is no APMF_API_v17 struct and
a v16 client is byte-unaffected. A client must see abiVersion >= 17 before it sets
param.form on kIntent_Idle: an OLDER APMF does NOT refuse such a claim -- it ignores the
form and plays its form-free IdleForceDefaultState -- so an unchecked client gets the
wrong animation, silently. That is the degrade to avoid; check the version.

ABI v18 (2026-10-05, ClickUp 86e3h6qj9) adds Harbinger's OWN LINE OF SIGHT and an
AWARENESS query. Two new flag bits ride existing words: kCastFlag_OwnLineOfSight
(CastFlags bit 6) and kTargetPin_OwnLineOfSight (kIntent_TargetPin's param.ival,
bit 0, which v13..v17 accept and ignore). APMF_API_v18 appends TWO slots,
GetLineOfSight (any thread) and SenseActor (main thread only), with their POD
structs. A client must see abiVersion >= 18 before it calls either slot or sets
either bit: an OLDER APMF ignores both bits WITHOUT a word (the cast fires, and the
pin holds, with no line-of-sight test at all). See "ABI v18: LINE OF SIGHT AND
AWARENESS" at the end of this header.

ABI v19 (2026-10-05, batch A) adds kIntent_CombatApproach (ch.24): the IN-COMBAT
movement goal "stay within R of actor X while fighting". It rides the EXISTING
RequestEx/Repoint/Release slots and reads the EXISTING param.target (X), param.fval
(R) and param.ival (CombatApproachFlags). APMF_API_v19 appends ONE read-only slot,
GetCombatApproachState, with its POD struct APMF_CombatApproachInfo. A client must see
abiVersion >= 19 before it asks for the intent or calls the slot: an OLDER APMF has no
channel for intent 24 and REFUSES the request (kInvalidHandle, "no channel serves
intent 24" in its log) -- the documented degrade. See "ABI v19: COMBAT APPROACH" at
the end of this header.
ABI v20 (2026-10-06): kCastFlag_FloorSpellsOnly (CastFlags bit 7) only, no struct/slot.
Gate on abiVersion >= 20 (Docs/INTEGRATION.md "Spells-only floor" has the why).
ABI v21 (2026-10-12): kIntent_CastGuard (ch.25) + CastGuardFlags only, no struct/slot. An older
APMF REFUSES intent 25 ("no channel serves intent 25"). Docs/INTEGRATION.md "Cast guards".
ABI v22 (2026-10-12, weighting phase 1): kIntent_EquipPreference (ch.26) + EquipPrefFlags, the
POD structs APMF_EquipPrefEntry / APMF_EquipPrefInfo, and APMF_API_v22 with TWO appended slots
(SetEquipPreference, GetEquipPreferenceState). A client must see abiVersion >= 22 before it asks
for the intent or calls a slot: an older APMF REFUSES intent 26 ("no channel serves intent 26").
See "ABI v22: EQUIP PREFERENCE" at the end of this header.
ABI v23 (2026-10-13, combat substrate slice S2a): kIntent_CombatSubstrate (intent 27) only, no struct
and no slot (it rides RequestEx / Repoint / Release; param.fval = TTL seconds). An older APMF REFUSES
intent 27 ("no channel serves intent 27"); a v23 APMF with [CombatSubstrate] bCombatSubstrate=0 (the
default) refuses it by name. GetSubstrateState is NOT in v23: its consumer is MFO's slice S3. Docs/FACETS.md
"Combat substrate", INVARIANTS #0 (j).
ABI v24 (2026-10-13, ch.12 idle state): APMF_API_v24 appends ONE read-only slot, GetIdleState(handle,
APMF_IdleStateInfo*), with IdleState / IdleUnconfirmable / IdleEnd and the 60-byte size-rule struct. A
client can tell an idle that PLAYED from one Harbinger cannot confirm (both keep the claim live). A client
must see abiVersion >= 24 before it calls the slot: an older APMF has no query, only IsClaimLive.
See "ABI v24: IDLE STATE" at the end of this header and Docs/FACETS.md ch.12.
ABI v25 (2026-10-14, ch.17 entry instance): an APMF_EquipEntry may name ONE inventory instance by its
ExtraUniqueID uniqueID. No struct change, no slot, no intent: the instance rides the entry's three
reserved bytes (reserved[0] = kEquipEntry_HasUid, reserved[1..2] = the uid, little-endian), with the
helpers SetEquipEntryUid / EquipEntryUid. With a uid the seat admits only that copy (a copy named
otherwise, or no copy named at all, is off-set unless a plain entry declares the same form) and the
equip pass equips exactly that copy. A client must see abiVersion >= 25 before it sets the flag: an
OLDER APMF ignores the reserved bytes WITHOUT a word (it never read them since v8), so the entry reads
as its base form and any copy satisfies it. A v24 client sends zeros and is byte-unaffected.
Docs/APMF_API-REFERENCE.md#abi-v25-equip-instance, Docs/FACETS.md ch.17 "Instances".
ABI v26 (2026-10-19, ch.27 substrate state, ClickUp 86e3pu939): APMF_API_v26 appends ONE read-only slot,
GetSubstrateState(actor) -> SubstrateState (None 0 / Held 1 / Real 2 / Anchor 3), ANY thread, lock-free.
marth 2026-10-13: substrate combat "only exists to facilitate actions", so no client may treat a Held
actor's combat as a fight. A client must see abiVersion >= 26 before it calls the slot: an older APMF has
no query, and a v23..v25 APMF with the substrate on reads as ordinary combat (today's behaviour). No struct,
no intent, no change to any existing slot. The same build adds the substrate's music deny (no ABI surface).
Docs/APMF_API-REFERENCE.md#abi-v26-substrate-state, Docs/FACETS.md "Combat substrate".
ABI v27 (2026-10-19, ch.27 substrate availability, substrate round R1): APMF_API_v27 appends ONE read-only
slot, IsSubstrateAvailable() -> bool, ANY thread (one atomic load, no game call). TRUE only when the
substrate INSTALLED on this runtime: [CombatSubstrate] bCombatSubstrate not 0, exactly 1.6.1170 or 1.5.97
(not VR), every seat it rides installed, every address row it calls verified, its APMF.esl records
resolved; FALSE before kDataLoaded and after any refusal. It lets a client tell "the capability is absent
here" (a declared degrade it may plan for) from "this one open was refused" (a logged failure, never a
fallback). A client must see abiVersion >= 27 before it calls the slot: an older APMF has no query. No
struct, no intent, no change to any existing slot.
NUMBERING NOTE: v27 had been left free for the ch.17 per-item equip deny (ClickUp 86e3t8w23). The substrate
availability query took v27 (MFO's OOC substrate client needs it first), so the per-item equip deny moves to
v28.
Docs/APMF_API-REFERENCE.md#abi-v27-substrate-available, Docs/FACETS.md "Combat substrate".
ABI v28 (2026-10-19, kIntent_Place, MFO town phase 2): kIntent_Place (intent 28) rides the EXISTING RequestEx
slot (param.form = the destination, a reference or an interior cell; param.pos = the offset or the point;
param.ival = PlaceFlags; param.fval = the yaw with kPlace_SetYaw). It is a ONE-SHOT, never a claim: ONE
TESObjectREFR::MoveTo_Impl on the game thread (the body Papyrus ObjectReference.MoveTo runs), for an actor that
may be unloaded. APMF_API_v28 appends ONE read-only slot, GetPlaceState(handle, APMF_PlaceStateInfo*) ->
PlaceState (Unknown 0 / Queued 1 / Placed 2 / Refused 3 / Failed 4), ANY thread, with PlaceWhy and the 64-byte
size-rule struct. No other slot, no APMF_Param field, no change to any existing slot. A client must see
abiVersion >= 28 before it asks for the intent or calls the slot: an older APMF REFUSES intent 28 ("no channel
serves intent 28").
NUMBERING NOTE: v28 had been named for the ch.17 per-item equip deny (ClickUp 86e3t8w23) when v27 went to the
substrate availability query. The placement facet took v28 (MFO town phase 2 needs it first), so the per-item
equip deny moves to v29.
Docs/APMF_API-REFERENCE.md#abi-v28-place, Docs/FACETS.md "Place".
ABI v29 (2026-10-25, client facet priority, ClickUp 1dfcuabbh1e): ClientFacet (kFacet_Target 0 .. kFacet_EquipArmor
9, the Harbinger page / APMF_Sources.ini facets) and kFacetPriorityUnknown (-1). APMF_API_v29 appends ONE read-only
slot, GetClientFacetPriority(facet) -> std::int32_t, the CALLING client's priority 0..100 (0 = off) on that facet,
attributed from the calling module like a claim. ANY thread, no game call. No intent, no APMF_Param field, no
change to any existing slot. A client gates on abiVersion >= 29; an older APMF has no query.
NUMBERING NOTE: v29 had been named for the ch.17 per-item equip deny (ClickUp 86e3t8w23). The priority query took
v29 (MFO's sell classification needs it first), so the per-item equip deny moves to v30.
Docs/APMF_API-REFERENCE.md#abi-v29-client-facet-priority.
```

<a id="intent-18"></a>
## Intent 18: the draft that reserved it

Moved verbatim from `native/APMF_API.h` (line 358 at 9d01cdf). The header keeps a pointer to this anchor.

```text
(That design's own draft calls itself "ABI v10" -- it will have to become
v11, since ch.19 shipped v10 first.)
```

<a id="kintent-travel-no-combos"></a>
## kIntent_Travel (ABI v10): why no combo intents

Moved verbatim from `native/APMF_API.h` (line 421 at 9d01cdf). The header keeps a pointer to this anchor.

```text
two arbitrate independently. That orthogonality is
deliberate: marth, 2026-09-22, "We should avoid
combining intents anyway."
Convenience COMBO intents (one call that bundles,
say, travel + combat target + combat entry) are a
recognised and reasonable idea, DEFERRED until after
the full MFO port to APMF is complete -- not refused
on principle. Until then: one facet, one intent.
```

<a id="abi-v18-los-origin"></a>
## ABI v18 line of sight: the requests that started it

Moved verbatim from `native/APMF_API.h` (line 2163 at 9d01cdf). The header keeps a pointer to this anchor.

```text
marth 2026-09-30: "this really should be a flag sent to harbinger." marth
2026-10-05: "We also want harbinger line of sight for attack on sight ... we want
the appearance of more senses than sight."
```

<a id="abi-v18-los-recipe"></a>
## ABI v18 line of sight: where the recipe came from

Moved verbatim from `native/APMF_API.h` (line 2191 at 9d01cdf). The header keeps a pointer to this anchor.

```text
This is MFO's field-proven Sightline own-ray recipe (2026-09-30), now Harbinger's.
```
