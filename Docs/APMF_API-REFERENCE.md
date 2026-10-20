# APMF_API.h reference

Long-form explanation moved out of `native/APMF_API.h` on 2026-10-12, verbatim, anchored per symbol (the companion-reference convention of a public C header: the header keeps a concise contract comment on every declaration and points here). The full facet reference is [FACETS.md](FACETS.md); history is in [APMF_API-HISTORY.md](APMF_API-HISTORY.md).

<a id="kintent-travel-destinations"></a>
## kIntent_Travel (ABI v10/v11): destinations and world positions

Moved verbatim from `native/APMF_API.h` (line 373 at 9d01cdf). The header keeps a pointer to this anchor.

```text
A DESTINATION IS A REFERENCE OR A CELL, and the
FormID's own record type decides which -- no flag,
no second field, no ambiguity:
  * an object REFERENCE (REFR/ACHR: an actor, an
    XMarker, a container, anything loaded). Arrival
    = distance to it <= the radius.
  * a CELL. Arrival = the actor's PARENT CELL is that
    cell; distance has no meaning for a cell, so the
    radius is not consulted for this kind.
Anything else is REFUSED and logged, never
reinterpreted -- but read the two refusal TIMINGS
below, because they are not the same and a client
that assumes they are will leak a claim.

A WORLD POSITION (ABI v11): set kTravel_ToPosition in
ival, leave form = 0, put the point in param.pos. An
AI package's location still carries a form or a handle
and NO coordinates, so APMF does what vanilla does: it
places its OWN XMarker at the point and the leg's
destination is that marker. Everything else is the
reference path above, unchanged (radius, combat,
the 2-minute stuck end), except that a marker never
"dies". APMF deletes the marker when the leg ends for
ANY reason and replaces it on a Repoint to a new
point (see kTravel_ToPosition). WITHOUT the flag a
non-zero param.pos is still REFUSED by name, never
silently ignored.
```

<a id="kintent-travel-ends"></a>
## kIntent_Travel: when a leg ends

Moved verbatim from `native/APMF_API.h` (line 401 at 9d01cdf). The header keeps a pointer to this anchor.

```text
THE WHOLE CONTRACT, in marth's words: "it goes to
the target 50-100u from it. And combat interrupts
and cancels the movement. That's all." The leg ENDS
on ARRIVAL (inside the radius), on the ACTOR ENTERING
COMBAT (`Actor::IsInCombat`), or on the destination
dying / being disabled / unloading. Nothing else.
(ABI v12 adds one engine-reported end: BLOCKED, the
engine holding the actor in its Movement Blocked
package for 3 s. The 2-minute stuck end is the
safety net. APMF_API_v12::GetTravelLegState says
which end it was.)
```

<a id="kintent-travel-one-facet"></a>
## kIntent_Travel: one intent, one facet

Moved verbatim from `native/APMF_API.h` (line 413 at 9d01cdf). The header keeps a pointer to this anchor.

```text
ONE INTENT, ONE FACET. ch.19 claims NOTHING on your
behalf -- no combat target (ch.6), no attack
selection, no casting, no equip (ch.17), no hands, no
aggression (ch.11), no movement block (ch.1). It
enters no combat, pins no target, and fakes no
perception (there is no line-of-sight or detection
test anywhere in it). A client that wants any of
those claims that intent ITSELF, separately, and the
```

<a id="kintent-travel-refusal-timing"></a>
## kIntent_Travel: why the engage-time refusal is not synchronous

Moved verbatim from `native/APMF_API.h` (line 447 at 9d01cdf). The header keeps a pointer to this anchor.

```text
Why not synchronous: RequestEx is callable FROM ANY
  THREAD and its whole contract is that it copies
  POD and enqueues (see the Threading note at the
  top). Deciding a form's record type needs a form
  lookup, and APMF does not take the engine's form
  table off the game thread for an API call --
  kIntent_Cast's own FromPackage extraction defers
  for exactly the same reason. Every refusal is
  logged with its reason either way.
```

<a id="kintent-targetpin"></a>
## kIntent_TargetPin (ABI v13): what the pin does, pauses and ends

Moved verbatim from `native/APMF_API.h` (line 466 at 9d01cdf). The header keeps a pointer to this anchor.

```text
WHAT IT DOES. Each combat update the engine asks its
target selectors which foe this actor should fight.
While the claim stands, APMF answers that question
with your target, so the engine's own pick never
reaches the actor. The engine then fights your
target with its own AI: its own attacks, movement,
spells and equips.

ONLY AMONG THE ENGINE'S OWN COMBAT TARGETS. The
answer is replaced only when the engine's selector
picked a target (the actor is fighting) AND your
target is one of the actor's combat group's targets.
Otherwise nothing is written and the log says so.
It does NOT start combat and does not make anyone a
target: an actor becomes pinnable once something (the
engine, or your own combat entry, a separate concern)
makes it a combat target of the actor's group. Combat
entry is never this intent (INVARIANTS #0).
It claims nothing else: no attack selection, no
casting, no equip, no movement, no aggression. One
intent, one facet (the kIntent_Travel rule).
It is a different intent from kIntent_CombatTarget
(ch.6), which stays ARBITRATION-ONLY and writes
nothing: claim this one to have APMF hold the target.

THE PIN PAUSES -- the engine's own pick stands for
that update -- while the engine has no target or your
target is not (yet) one of the group's combat targets.
The claim stays; it resumes when both hold again.

THE PIN ENDS, AND APMF RELEASES THE CLAIM ITSELF
(marth: "If APMF can no longer track the target, it's
lost, and dropped"), when the target is LOST (the
engine can no longer locate it), dead, disabled, not
loaded or no longer resolves, or the actor itself
dies. The log names the reason ("pin ended: target
lost"), IsClaimLive(handle) turns false (ABI v6), and
the handle is dead: a mod that wants to keep chasing
must pin again. This is the ONLY case in which APMF
releases a client's kIntent_TargetPin claim. Your own
Release, an outranking claim, a save load, a new game
or the actor unloading also end it (never saved).

The world's reaction to the fight is YOURS (CLAUDE.md
principle 2): crime, bounty, faction and aggression
consequences happen as the engine does them, and
Release does not undo them.
```

<a id="abi-v18-own-ray"></a>
## ABI v18 line of sight: the own ray, threading and cost, who keeps a pair measured

Moved verbatim from `native/APMF_API.h` (line 1975 at a512763). The header keeps a pointer to this anchor.

```text
THE OWN RAY. Harbinger's line of sight from a VIEWER actor to a TARGET actor is a
physics ray test, never the engine's cached Actor::HasLineOfSight answer:
  * FROM the viewer's eye point (the engine's own Actor::GetEyeVector origin). There
    is NO view cone and NO facing test: a target behind the viewer is as visible as
    one in front. Line of sight is geometry, not attention.
  * TO three points on the target: its feet + 16 units, 55% and 90% of its height
    (its bound height times its scale; 120 units when that reads as nothing). Each
    ray ends short of its point by the TARGET's own size -- the horizontal half-diagonal
    of its bound box times its scale, plus 16 units, never less than 48 (a humanoid's
    value) and never more than 1024 -- so the target's own body (a giant's, a mammoth's
    or a dragon's included) is never the thing that blocks it. A target closer than
    that margin is VISIBLE.
  * ON the character-controller collision layer, in the viewer's own collision group
    (its own capsule is skipped): "could a walking body travel this line". Walls,
    closed doors and tent walls block; an open doorway, a railing under the line or
    a gate's gaps do not. ANOTHER ACTOR'S BODY on the line blocks that ray.
  * VISIBLE when ANY of the three rays is clear (a target one stair up, or behind a
    low sill, is seen); OCCLUDED only when all three are blocked.
  * A target in a different havok world (an interior seen from outside) is
    OCCLUDED with kLosWhy_OtherWorld.
  * The ray cannot be cast (no attached cell or havok world, a viewer without a
    character controller, an unloaded, disabled or deleted actor, a dead viewer, or
    the engine's pick returning without casting): the verdict is UNAVAILABLE with its
    reason. It is never reported as VISIBLE.

THREADING AND COST. Every ray runs on the game's MAIN thread (the player-Update
thread APMF drains its own queue on). Each ray is one bhkWorld::PickObject call,
which takes the havok world's READ lock itself around the cast (disassembly of all
three builds: AE 0xE86560, SE 0xDA7580, 1.7.104 0x104BE80); Harbinger holds no lock
of its own across a ray and takes no outer world lock. At most 3 rays per pair
measurement. Cached pairs are re-measured at most once every kLosRefreshMs and at
most kLosMaxPairsPerFrame pairs per frame (oldest first). SenseActor's synchronous
sight test has its OWN per-frame cap, kLosMaxSyncPairsPerFrame: past it, the query
answers from the pair's stored verdict (fresh only) or not at all
(kAwareDetail_SightDeferred), and the pair is queued for the pump. So the main-thread
cost is bounded at 3 x (kLosMaxPairsPerFrame + kLosMaxSyncPairsPerFrame) rays per frame
whatever the clients ask.

WHO KEEPS A PAIR MEASURED. A pair is measured while something ASKS for it: a
GetLineOfSight call, a cast seat or target-pin seat reading it for a claim with an
own-line-of-sight bit, or a SenseActor sight test. A pair nobody has asked for in
kLosInterestMs is dropped. GetLineOfSight is a cheap lock-free read, so a client
asks every time it needs the answer, and the asking is what keeps it warm.
```

<a id="senseactor-senses"></a>
## ABI v18 SenseActor: what each sense means

Moved verbatim from `native/APMF_API.h` (line 2086 at a512763). The header keeps a pointer to this anchor.

```text
"Does this actor SENSE that one?" -- for an engage-on-sight gambit. OMNIDIRECTIONAL
(no view cone, no facing anywhere) and MULTI-SENSE: each sense below is evaluated on
its own and reported as a bit, so a client decides which ones count for it.

  kSense_Sight      The OWN RAY (above) from the viewer to the target is VISIBLE and the
                    target is within sightRange. Measured now, unless this pair was
                    measured less than kLosRefreshMs ago (that verdict is reused), or this
                    frame's kLosMaxSyncPairsPerFrame is spent (kAwareDetail_SightDeferred:
                    a fresh stored verdict answers, else sight is not sensed this call). It
                    also refreshes the pair for GetLineOfSight.
  kSense_Hearing    Either route (AwareDetail says which):
                    NOISE: the target made a NEW engine noise in the last
                    kAwareNoiseWindowMs, of a level above 0, at a point within
                    hearRadius of the viewer or of the anchor. The noise is the engine's
                    own "detection event", the one record the engine keeps per actor of
                    the last noise it made (written by AIProcess::SetActorsDetectionEvent:
                    a spell's release, a projectile fired or landing, a hit, and the
                    other engine noise sources; its level is the engine's own value).
                    "New" means Harbinger saw that record change: the FIRST time a query
                    names a target, Harbinger records its current noise as a baseline and
                    hears nothing from it (a noise made before Harbinger ever looked is
                    not heard; kAwareDetail_NoiseBaseline marks that call).
                    COMBAT: the target is in combat (Actor::IsInCombat) within hearRadius
                    of the viewer or of the anchor -- a fight is loud.
  kSense_Proximity  The target is within proximityRadius of the viewer (3D distance),
                    whatever is between them.
  kSense_Engaged    The target is in combat and its current combat target IS the viewer
                    or the anchor: it is fighting you.
Nothing here decides hostility, faction or crime: that is the client's call. A target in
a different cell space (interior vs exterior, another interior) senses nothing and sets
kAwareDetail_OtherSpace. The engine's own detection (Actor::RequestDetectionLevel) is
deliberately NOT a sense: its sight half is the facing, cone-limited test this replaces.
```

<a id="abi-v22-equip-preference"></a>
## ABI v22 equip preference (ch.26): what a declaration does, never lifted, the escalation

Moved verbatim from `native/APMF_API.h` (line 2318 at a512763). The header keeps a pointer to this anchor.

```text
Where the engine decides by a SCORE, Harbinger enforces a wish only by BIASING that score.
In combat the engine rescores every item about once a second with CombatInventoryItem::
CalculateScore (vfunc 0x0C), sorts each engine CATEGORY and equips the top of each while a
hand is free. Melee and ranged weapons and OFFENSIVE spells and staves share category 0;
shields and torches 3; Restore 1; Ward 2 or 3; buff and utility spells 4 or 5.

WHAT A DECLARATION DOES. While the claim OWNS ch.26 on the actor and the actor is in combat,
each time the engine scores a declared item Harbinger returns the engine's OWN score lifted to
max(own, best * (1 + kEquipPrefStepRel) + kEquipPrefStepAbs), where "best" is the highest score
the engine holds for any other item of the same category list. Items this actor's preferences
or live cast claims lift at an equal or better rank are left out of "best". A lower `rank` is
lifted above a higher one; an item that already leads is returned unchanged. Weapons, shields
and torches are lifted at the weapon-class seats, a staff at the spell/staff seat. A SPELL is
not declarable here (it is ignored): a kIntent_Cast claim's driven spell is lifted the same
way at the spell/staff seat. The ENGINE STILL DECIDES: its range gate, its order table (a
Restore, Ward or buff spell takes its hand before any weapon) and its hand occupancy apply.
Out of combat the scorer does not run and a preference does nothing.
  * NEVER LIFTED: an item the engine scored 0 or less (a bow with no arrows); an item for a
    hand a live kIntent_Cast claim holds (that hand carries only the spell); any item while a
    source INSIDE the score seat outranks this claim on the Equip facet (APMF_Sources.ini; the
    engine itself ranks at "Engine", default 20, a client at 50).

ESCALATION (kEquipPref_Force). DISCOURAGED. Set it (Repoint with param.ival |= kEquipPref_Force)
only after an OBSERVED miss: the preferred item not in its hand after about two rescore passes
(about 2 s) in combat. Harbinger does not time this; the bit is honoured when set. With it
Harbinger ALSO refuses, at the engine's equip choke point, every engine equip of a weapon,
shield or torch into a hand this claim prefers something for, unless the item is preferred for
a hand it competes for -- RANKED like every other deny: a source that outranks the claim keeps
its equip, and that is a FORCE ANOMALY (warn line, counted, APMF_EquipPrefInfo::forceAnomalies).
The claim's OWN client's equip ties with the claim and stands; that is counted apart, not an
anomaly. The player's own trade/gift menu equips always pass. An unattributed equip passes
unless APMF_Sources.ini says [UnknownSources] Equip=override. Harbinger EQUIPS NOTHING for a
preference: place the item yourself (or declare it with kIntent_EquipAuthority).
```

<a id="abi-v25-equip-instance"></a>
## ABI v25 equip instance (ch.17): naming one copy of an item

A base FormID cannot say which copy of an item the actor should wear. A gem-socketed ring and a plain ring of the same base form read the same, and so do a player-enchanted sword and a plain one. From ABI v25 a `SetEquipSetEx` entry can name ONE inventory instance by its `ExtraUniqueID` uniqueID, the u16 the engine keeps on an item's extra data list. MEO identifies item instances the same way.

**Layout.** `APMF_EquipEntry` does not change. It is still 8 bytes, and the instance rides its three reserved bytes.

| Byte | Meaning |
|---|---|
| `reserved[0]` | flags. Bit 0 is `kEquipEntry_HasUid`. Bits 1 to 7 are reserved and must be 0 |
| `reserved[1]` | the uid, low byte. 0 without the flag |
| `reserved[2]` | the uid, high byte. 0 without the flag |

`SetEquipEntryUid(entry, uid)` writes the three bytes (uid 0 clears them) and `EquipEntryUid(entry)` reads the uid back (0 when the entry names no instance). Harbinger reads an entry as naming an instance only when `reserved[0]` is exactly `kEquipEntry_HasUid` and the uid is not 0. All-zero bytes are a plain entry. Anything else (the flag with uid 0, another flag bit, uid bytes without the flag) is logged once per handle and read as a plain entry.

**What the seat does.** For a form the set declares ONLY by instance, the in-set test needs the instance an equip names. That is the `a_extraData` argument of `ActorEquipManager::EquipObject`, which both worker call sites store as the first field of the worker's argument block. Harbinger reads its `ExtraUniqueID` there.

| The equip names | Verdict for a form declared only by uid |
|---|---|
| the declared copy (same uid) | in the set |
| another copy, or a copy with no `ExtraUniqueID` | off the set |
| no copy at all (a null extra list) | off the set |

A null extra list is what the combat AI's own equip passes (AE 48124 and 48126, SE 46955 and 46957, 1.7.104 `0x88CFF0` and `0x88D360` all clear `r9`), and so do the worker's second-hand re-entry (AE 38913, SE 37957) and a plain script `EquipItem`. The engine has not named a copy, so the seat cannot prove it is the declared one, and refuses it in an owned category. An outfit refresh or the AI cannot swap the declared copy for another. A queued equip comes back to the seat through QueuedApply (AE 38906, SE 37950) with the extra list it was queued with, so Harbinger's own queued equip of the declared copy passes.

If another entry declares the same form with no uid, the form is admitted by base form as in v8, whatever copy the equip names.

**The selection seat.** The combat AI's `CheckShouldEquip` (0x0F) candidate carries the form only (`CombatInventoryItem::item` at +0x10, no extra list). For a form declared only by uid the candidate is admitted while a hand it competes for already holds that form (keeping it equips nothing new) and refused otherwise, because the equip it would drive names no copy and the sink would refuse it. That keeps the AI from planning around an equip the sink refuses. A consequence to choose on purpose: a uid on a HAND entry (a weapon, a shield, a torch) takes that item out of the combat AI's weapon switching. The selection seat refuses it unless the hand already holds it, so only Harbinger's equip pass (once per declaration) or a re-declaration puts it back on. Set a uid on a hand entry only when that is what you want.

**The equip pass.** For an entry with a uid, Harbinger finds the extra data list with that uid in the actor's inventory and hands it to `EquipObject`, so the engine equips exactly that copy. "Not worn" means that list is not worn: no `ExtraWorn` or `ExtraWornLeft` for a default entry, no `ExtraWorn` for a right-hand entry, no `ExtraWornLeft` for a left-hand entry. A copy the actor does not carry is skipped and logged (`instance-missing` on the pass line). Another copy is never put on instead. A copy worn in the OTHER hand is skipped and logged (`instance-other-hand`), because the engine ignores an equip of a list that is already worn (`EquipObject` returns before its worker). Entries without a uid count copies among themselves, exactly as in v8.

**What does not change.** The unequip of a worn item in a DENIED category reads only the category, never the set. The ranking of every deny, the exemptions and observe-only work as before.

**Version.** A client must see `abiVersion >= 25` before it sets the flag. An older Harbinger ignores the reserved bytes without a word: the entry reads as its base form, any copy satisfies it, and its equip pass equips whichever copy the engine picks.

**Verified on** 1.6.1170, 1.5.97 and 1.7.104 (the three runtimes the equip seat installs on), from the disassembly: `EquipObject` stores `r9` at the argument block's first field (AE and 1.7.104 `+0x113`, SE `+0x87`), the list sibling stores the first unworn extra list there, the engine read-locks the extra list on the same thread just before (`+0x18` on 1.6.1170 and 1.7.104, `+0x10` on 1.5.97, the offsets CommonLib's `ExtraDataList::GetLock` uses), and `ExtraUniqueID` is type `0x9F` with the uid at `+0x14` (its own `IsNotEqual` on all three binaries).

<a id="abi-v26-substrate-state"></a>
## ABI v26 substrate state (ch.27): is this combat a fight?

The [combat substrate](FACETS.md#combat-substrate-kintent_combatsubstrate) puts an out-of-combat actor into combat against Harbinger's own anchor so the engine's animated cast road exists. To the engine, and to any mod reading `IsInCombat` or the combat controller, that actor is in combat. It is not in a fight. marth, 2026-10-13: "Certainly not. It only exists to facilitate actions." From ABI v26 a client can ask.

```cpp
std::uint32_t (*GetSubstrateState)(RE::FormID actor);   // APMF_API_v26, gate on abiVersion >= 26
```

| Value | Name | Meaning | What a client does |
|---|---|---|---|
| 0 | `kSubstrateState_None` | No substrate on this actor. Also every actor before Harbinger installed, and every answer of an APMF older than v26 (no slot) | Its combat, if any, is real |
| 1 | `kSubstrateState_Held` | A live substrate holder whose real-combat trigger has not fired. Set from the substrate's open (before its entry `StartCombat`) to its close (after its `StopCombat`) | Its combat is SUBSTRATE-ONLY: not party combat, no combat HUD, no combat style, no combat decisions, nothing interrupted |
| 2 | `kSubstrateState_Real` | A holder whose real-combat trigger fired (a foe in its group, a hit from outside the party, the party in combat). The substrate stands down for it at once and is dropped on the next game-thread pass, after which the actor reads `None` | Its combat COUNTS |
| 3 | `kSubstrateState_Anchor` | The actor IS the anchor (`APMF.esl` 0x80A, a ghost Nazeem copy) | Never a foe, a follower or a target |

**Threading and cost.** Any thread. The call reads Harbinger's fixed 16-slot holder table (atomics, an id re-check around the state read) and the anchor's FormID: no lock, no game call, no allocation. It is cheap enough to ask for every actor on every tick. A throw inside returns `None`.

**Why `Real` flips before the drop.** The real-combat triggers run on whatever thread sees them (the combat thread for a new group target, the event thread for a hit). Each sets the slot's REAL bit first, the same bit every substrate deny reads, so the actor's combat counts from that instant. The anchor stays in the group until the next game-thread pass removes it, so for at most one frame a `Real` actor may still list the anchor as a target.

**Older APMF.** An APMF before v26 has no slot. A client treats every actor as `None`, which is today's behaviour: a v23 to v25 APMF with the substrate on shows a substrate as ordinary combat.

**Not in the query.** Whether battle music plays is not a client concern. The same build denies it inside Harbinger: a group whose members are all live holders and whose only target is the anchor has its music decision written from "undecided" to "none" before the engine decides it (FACETS ch.27 "Battle music").

<a id="abi-v27-substrate-available"></a>
## ABI v27 substrate availability (ch.27): can this install open a substrate at all?

```cpp
bool (*IsSubstrateAvailable)(void);   // APMF_API_v27, gate on abiVersion >= 27
```

TRUE only when the [combat substrate](FACETS.md#combat-substrate-kintent_combatsubstrate) INSTALLED and is usable on this runtime:

- `APMF.ini` `[CombatSubstrate] bCombatSubstrate` is not 0,
- the runtime is exactly 1.6.1170 or 1.5.97 (not VR; 1.7.104 is refused because the held engagement's `StopCombat` is not placed there),
- every seat it rides is installed (ch.22's slot-0x99 seat and its anchor-target site, the held engagement's `StopCombat`, the 95-node leaf allow-list, the music deny's seat unless `bSubstrateMusicDeny=0`),
- every address row it calls verified (`[allowance] SeatVerified`), and its `APMF.esl` records (0x808 to 0x80B) resolved.

It is the same flag that makes a `kIntent_CombatSubstrate` request refused as unavailable: TRUE here means such a request is not refused for that reason (it can still be refused for the player, actor 0 or a bad TTL, and an accepted claim can still fail to OPEN, which ends the claim and is logged).

**Why a client needs it.** "The capability is absent on this install" and "this one open was refused" are different answers. The first is a declared degrade a client may plan around (MFO: its legacy direct cast road out of combat, logged once per session). The second is a failure: log it, never fall back from it (principle 7).

**Threading and cost.** Any thread, one atomic load, no game call, no lock. FALSE before `kDataLoaded`; it may change once at `kDataLoaded` and never afterwards. A throw inside returns FALSE. The reason for a FALSE is logged once in `APMF.log`: `[substrate] combat substrate NOT installed -- <why>`.

**Older APMF.** An APMF before v27 has no slot. A v23 to v26 APMF may still accept a substrate claim; a client that needs the answer treats a missing slot as "unknown", not as "available".


<a id="abi-v28-place"></a>
## ABI v28 place (kIntent_Place): put an actor somewhere, loaded or not

```cpp
// kIntent_Place = 28, through RequestEx. Gate on abiVersion >= 28.
std::uint32_t (*GetPlaceState)(Handle handle, APMF_PlaceStateInfo* out);   // APMF_API_v28
```

A [placement](FACETS.md#place-kintent_place) is ONE engine move, `TESObjectREFR::MoveTo_Impl` (AE id 56626 at 0xA447F0, SE id 56227 at 0x9AE5C0, 1.7.104 at 0xA5D370, verified row `Place.TESObjectREFR.MoveTo`), on the game thread. That is the body Papyrus `ObjectReference.MoveTo` runs: the Papyrus native only queues a task on the script VM, and the task's run (AE 56101, SE 55572) calls `MoveTo_Impl` with the target's cell, its worldspace, the point and the rotation. For a `Character` the body calls `StopInteractingQuick(true)` and then the actor relocation (AE 37313) with the point, rotation Z, the cell and the worldspace. The handle argument is read only on the player branch, so Harbinger passes an empty one and can name a bare interior cell. It works on an unloaded persistent actor and into an unloaded cell.

**Never a claim.** `ClientAPI` diverts the request before the control map, the same shape as a position cast. The handle comes from the claim counter and only labels the request: `IsClaimLive` is false for it, `Release` and `Repoint` do nothing to it.

**The destination.** `param.form` is a REFERENCE (the actor lands at its position plus `param.pos`, in its cell and worldspace: exactly Papyrus `MoveTo(akTarget, x, y, z)`), or an INTERIOR CELL (the actor lands at `param.pos` in it). An exterior cell is refused: name a reference in that worldspace and an offset.

**The flags** (`param.ival`, `PlaceFlags`): `kPlace_OnlyIfUnloaded` (no visible pop: refused when the actor has 3D or stands in an attached interior cell, and outdoors only his 3D counts), `kPlace_RefuseIfPlayerPresent` (refused when the player's cell is the actor's cell or the destination cell), `kPlace_SetYaw` (face `param.fval` radians, else keep his heading). An unknown bit is refused at the call.

**Arbitration.** Refused (`kPlaceWhy_TravelOwned` / `kPlaceWhy_HoldOwned`, `blocker` = that claim's handle) when the winning ch.19 travel or ch.1 movement block claim on the actor belongs to another client, or to no client Harbinger can name. The client is taken from the calling DLL at the call, the same attribution every claim gets. Your own claim does not block a placement and Harbinger does not end it.

**`GetPlaceState`.** Any thread, a copy under a leaf lock, no game call. `APMF_PlaceStateInfo` is 64 bytes, every field 4 bytes, the size rule of `APMF_TravelLegInfo`: set `size = sizeof(APMF_PlaceStateInfo)`. It fills nothing into a smaller struct.

| Offset | Field | Meaning |
|---|---|---|
| +0 | `size` | the caller sets it |
| +4 | `state` | `PlaceState`: Unknown 0, Queued 1, Placed 2, Refused 3, Failed 4 |
| +8 | `handle` | the handle asked about |
| +12 | `actor` | the actor |
| +16 | `dest` | `param.form` |
| +20 | `why` | `PlaceWhy` (FACETS table 20c) |
| +24 | `blocker` | the owning ch.19 / ch.1 claim on TravelOwned / HoldOwned |
| +28 | `cell` | Placed / Failed: the actor's parent cell read back after the move |
| +32 | `x`, `y`, `z` | the world point he was sent to |
| +44 | `yaw` | the world Z rotation handed to the engine |
| +48 | `flags` | the request's `PlaceFlags` |
| +52 | `info` | `kPlaceInfo_WasLoaded`, `kPlaceInfo_FromCell`, `kPlaceInfo_CellDiffers` |
| +56 | `msSinceChange` | wall ms since the record last changed |
| +60 | `seq` | changes on every change of the record |

**Placed is read back, not assumed.** After the call Harbinger reads the actor's position. Within 1 unit of the point: `Placed`. Otherwise `Failed` with `kPlaceWhy_NotMoved` and an ERROR line naming where he reads back. `kPlaceInfo_CellDiffers` says the parent cell he reads back in is not the cell handed to the engine (it does not fail the placement, and it is logged).

**Lifetime.** A finished record stays 30 s of wall time (`kPlaceStateGraceMs`), then reads `Unknown`. A load or a new game drops every record, and a placement still queued then never runs. Nothing is saved.

**Older APMF.** An APMF before v28 has no channel for intent 28 and refuses the request ("no channel serves intent 28"), and has no `GetPlaceState` slot.
