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
