# Integrating with APMF

A quickstart for SKSE plugin authors who want their mod to share NPC control
with other mods, instead of fighting them for it.

## What APMF is

APMF (AI Package Management Framework, "Harbinger") is a per-facet arbitration
layer for NPC control. You claim one facet of an actor (its combat target, its
cast selection, whether it's sneaking, and so on) at a priority you choose, and
APMF routes that one facet to you while denying whoever else was driving it.
Everything else about that actor, movement, other packages, the rest of its
AI, keeps running exactly as it was. When you release the claim, APMF stops
suppressing and the actor's own AI (or the next-highest claim) takes it back.

APMF never calls `StartCombat`, `CastSpellImmediate`, or any other
decision-making engine function on its own initiative. It arbitrates who owns a
facet and denies the losers. The two exceptions are calls YOU declare, made once
per declaration: a cast at a point (ABI v11) and a combat entry (ABI v14,
`kIntent_CombatEntry`, one `StartCombat` against the target you name).

For most facets you bring the behavior with your own proven mechanism (your own
package, your own `currentCombatTarget` write) and APMF makes sure it reaches
the actor.

**The cast facet works differently, and this is the point of ABI v5.** You do
not bring a cast mechanism at all. You declare *what* to cast and *at whom*, and
APMF answers the engine's own cast-decision points so that **the NPC's own combat
AI** selects, equips, charges, aims, fires and channels it. APMF still calls no
cast, equip or animation function. The animation is the game's own because the
game is the one casting. See "Making an NPC cast" below.

## Quickstart

### 1. Get the interface

Fetch the interface pointer once, after SKSE has loaded (`kPostLoad` or
`kDataLoaded` is a good spot), and keep it. APMF exports one C function,
`APMF_GetInterface`, returning a static POD struct you never free.

```cpp
#include "APMF_API.h"

namespace MyMod {

    const APMF_API::APMF_API_v6* g_apmf = nullptr;   // pick the newest struct you use
                                                     // (v6 is current -- APMF_API.h
                                                     // is the only canonical statement
                                                     // of kABIVersion, INVARIANTS #14b)

    void TryBindAPMF() {
        HMODULE h = GetModuleHandleA("APMF.dll");
        if (!h) return;   // APMF not installed. Degrade gracefully.

        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) return;

        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) return;   // The installed APMF is OLDER than the header you built
                             // against, so it refused outright. This one call has no
                             // downward retry -- ask for the LOWEST version you can
                             // work with if you want to keep supporting older APMFs.

        if (base->abiVersion >= 6)
            g_apmf = reinterpret_cast<const APMF_API::APMF_API_v6*>(base);
        // A client that only needs v2 checks `>= 2` and casts to APMF_API_v2*.
        // Guard every call site on g_apmf being non-null either way: APMF may be
        // absent, or older than the ABI version you want.
    }

}
```

### 2. Claim a facet, release it when done

A claim is a request ("I need this facet on this actor") plus a `basis`, a
float you choose as your own priority. Higher basis wins the facet. On a tie,
the earlier claim wins.

```cpp
using namespace APMF_API;

Handle g_targetClaim = kInvalidHandle;

void ClaimCombatTarget(RE::FormID actor, RE::FormID target) {
    if (!g_apmf) return;   // APMF absent or too old. Fall back to your own logic.

    APMF_Param param{};
    param.form = target;
    g_targetClaim = g_apmf->RequestEx(actor, kIntent_CombatTarget, /*basis=*/50.0f, &param);
    if (g_targetClaim == kInvalidHandle) return;   // lost arbitration, or no channel serves this intent

    // You now own the combat-target facet for `actor`. APMF has denied every
    // other claimant. Drive the behavior yourself, with your own mechanism
    // (your own currentCombatTarget write, your own targeting AI). APMF does
    // not do this part for you.
}

void ReleaseCombatTarget() {
    if (g_targetClaim == kInvalidHandle) return;
    g_apmf->Release(g_targetClaim);
    g_targetClaim = kInvalidHandle;
}
```

### 3. Repoint an existing claim

If a held claim's subject changes (the target switches, the selected spell
changes), use `Repoint` instead of releasing and re-requesting. The handle
stays the same, and the channel updates in place with no release/re-engage
churn.

```cpp
void RetargetWhileHeld(RE::FormID newTarget) {
    if (g_targetClaim == kInvalidHandle) return;
    APMF_Param param{};
    param.form = newTarget;
    g_apmf->Repoint(g_targetClaim, &param);
}
```

Reserve `Release` for when you are genuinely done with the facet (combat
ended, the cast finished), not for a routine retarget.


## Making an NPC cast (ABI v5, `kIntent_Cast`)

Requires `abiVersion >= 5` and `APMF_API_v5`.

```cpp
APMF_API::APMF_CastRequest req{};
req.spell  = spellFormID;     // the spell to cast
req.proxy  = 0;               // 0 = APMF mints a delivery-flip proxy if the spell is Self-delivery
req.target = targetActorID;   // 0 = self. Load-bearing: this is where the cast lands.
req.flags  = APMF_API::kCastFlag_LeftHand          // which hand
           | APMF_API::kCastFlag_Concentration     // set for a held/channelled stream
           | APMF_API::MakeStopPct(80);            // stop a channel at 80% of the target's AV
req.ttlMs  = 4000;                                 // 0 -> default; always clamped, never unbounded

APMF_API::Handle h = g_apmf->RequestCast(actorFormID, basis, &req);
if (h == APMF_API::kInvalidHandle) { /* lost arbitration, or channel not registered */ }
```

`req` is copied synchronously, so a stack temporary is fine, and the call is safe
from any thread. Re-request while you still want the cast; `Release(h)` when you do
not. The claim is always TTL-bounded so a crashed client cannot strand an NPC.

**What this unlocks.** The vanilla combat AI cannot classify a healing spell aimed
at someone *else*, so it never builds one as a candidate and never considers casting
it. That is why NPCs have only ever healed themselves through the game's AI. APMF
supplies the one missing classification decision and the engine does the rest.

**Practical notes.**
- If the NPC holds a weapon from another source, use the **left** hand. Contesting the
  weapon hand means that source re-equips over you and the cast dies.
- A `kSelf` spell applied at an ally lands on the caster. Leave `proxy = 0` and APMF
  mints a delivery-flipped copy.
- Left at `stopPct = 0`, a channel runs to full restoration. Without it the engine stops
  where its combat style says, which for a heal is roughly a quarter-second pulse.
- A refused cast (no magicka, hand busy, spell unknown) visibly does not happen. Nothing
  is faked. Check `APMF.log`.
- **AE 1.6.1170 only.** The cast path refuses to install on other runtimes and on VR
  rather than guessing at offsets. Kill switches live in `Data/SKSE/Plugins/APMF.ini`.


## Casting at a point (ABI v11, `kCastFlag_AtPosition`)

**Off by default, and not an endpoint.** The position cast is INVARIANTS #0 action (e),
adopted by marth with a standing condition: the actor does not animate, and proper animations
are required for ALL actions. Do not ship a user-facing action on this alone. The goal is an
animated path (the actor's own AI cast aimed at the point through the engine's seats); this is
a stepping stone or the delivery half of that. `[PositionCast] bPositionCast` defaults to 0,
and every request is refused by name (`[poscast] request REFUSED ... position cast not
installed`) until a user sets it to 1. Travel to a point does not depend on it.

Requires `abiVersion >= 11`. Check it before you set the bit: an older APMF ignores
`kCastFlag_AtPosition` and turns the request into an ordinary cast claim aimed at
`param.target` (0 = the caster itself).

```cpp
APMF_API::APMF_Param p{};
p.form = spellID;                           // a SpellItem: Target Location, fire-and-forget, no summon, NO projectile
p.ival = APMF_API::kCastFlag_AtPosition;    // the flag ALONE; any other cast flag is refused
p.posX = pt.x; p.posY = pt.y; p.posZ = pt.z; // world point, in the actor's worldspace (indoors: its cell)
APMF_API::Handle h = g_apmf->RequestEx(actorFormID, APMF_API::kIntent_Cast, basis, &p);
if (h == APMF_API::kInvalidHandle) { /* refused at the call: see APMF.log */ }
```

**What happens.** On the next main-thread pump APMF places a non-persistent XMarker
(Skyrim.esm `0x3B`) at the point and casts the spell FROM it, blamed on the actor:
`InterruptCast(false)` then `CastSpellImmediate(spell, false, none, 1.0, false, 0.0, actor)`
on the marker's instant caster. That is the exact sequence the game's own Papyrus
`Spell.RemoteCast` runs. The actor does not animate, its hands are not touched, and
APMF charges no magicka (resource policy is yours). The marker is deleted one frame later.

**Why not "the actor casts at a marker".** The game places a Target Location spell cast
by an NPC at the NPC's own magic node and never reads the target it was handed, on
both runtimes (`Docs/ADDRESS-TABLE-2026-09-15.md`, ADDENDUM 2026-09-23). That call
lands at the actor's hand. A marker has no magic node, so a spell it casts lands on it.

**What is refused, each with one `[poscast] ... REFUSED` line naming why:**
- a spell with a **projectile** on any effect: a rune, a trap, a lobbed shot. The engine
  launches a Target Location projectile only from the player's crosshair pick, never for a
  marker caster (both runtimes, `Docs/ADDRESS-TABLE-2026-09-15.md` ADDENDUM 2026-09-23), so it
  would silently place nothing. Refused on the game thread (logged). What remains is Target Location effects
  applied at the spot directly.
- a **summon**. The game applies a summon effect only to the actor that cast it, so a
  marker cannot summon. An NPC's own summon already lands in front of it: the engine's
  `SummonCreatureEffect` picks that spot itself. Summon through an ordinary cast.
- Self, Touch, Aimed or Target Actor delivery. Use `RequestCast` with an actor target.
- concentration and constant-effect spells.
- disease, ability and addiction spell types (`RemoteCast` refuses the same three).
- any other cast flag alongside `kCastFlag_AtPosition`.
- `RequestCast` with the bit: `APMF_CastRequest` has no position field.
- **the actor's cast facet is owned**: a live cast claim on that actor would outrank a
  driving request at your basis (a higher basis, a `kCastFlag_DenyHandOnly` floor included,
  or an equal basis that drives something). Refused at the call, naming the claim.
- a point outdoors in a cell that is not loaded and attached, or not in the actor's
  worldspace (the marker goes in the cell that CONTAINS the point, found with
  `TES::GetCell`; indoors it is the actor's cell).
- a `Repoint` carrying `kCastFlag_AtPosition` on a live cast claim (nothing changes).
- a dead or unloaded actor, a cell that is not attached, or more than 16 markers alive
  at once (they live one frame, so that is a burst of 16+ casts in one frame).
AT THE CALL (`kInvalidHandle`): the flag, the point, a non-finite basis, and the facet check.
ON THE GAME THREAD, one `[poscast] request N REFUSED ...` line: the spell checks (APMF does not
look forms up off the main thread: the pinned CommonLib's `LookupByID` takes no lock), the actor
and cell checks, and the facet check again (a claim may publish in between). So a live handle
does not mean the cast will happen; read the log.
**Same-basis note:** APMF has no client identity, so your OWN driving cast claim at the same
basis (or higher) blocks your own position cast. Release it, or ask at a higher basis.
- the whole feature when `[PositionCast] bPositionCast=0` (the default), on VR, or on a runtime other
  than 1.6.1170 / 1.5.97.

**It is a one-shot, not a claim.** It never enters the control map, so no engine seat
can read it as an actor target and it holds no facet. The handle labels its log lines
and nothing else: `IsClaimLive(h)` is false, `Repoint`/`Release` on it do nothing. You
learn the outcome from `APMF.log` (`queued`, then `DELIVERED` or `REFUSED`).

**Picking the point** is your job (declare, then APMF enforces). `FindEmptySpace` below
answers "where is a clear, standable spot over there" if you want the help.

### Save behaviour of APMF markers (ABI v11)

Every XMarker APMF places (position casts and position travel legs) is recorded until APMF
deletes it, and that record is co-saved (record `'XMRK'` v1 under APMF's `'APMF'` co-save).
Loading a save deletes, on the first main-thread pump after the load, every recorded marker the loaded world still holds,
and only one that passes all three proofs: the recorded runtime (0xFF) FormID resolves, its
base is Skyrim.esm's XMarker, and it stands within 1u of the recorded spot. Anything that
fails a proof is forgotten and never touched. A recorded marker whose cell is not loaded at
that moment is kept in the record and deleted by a later load that finds it (at most 64 are
carried). A save from Harbinger 0.9.7 or older has no such record and sweeps nothing. The log
line is `[marker] kPostLoadGame sweep -- N APMF XMarker(s) deleted, F forgotten, K not loaded`.
Nothing of APMF's names a marker when it is deleted: no leg or cast survives a load, and every
package record that last aimed at one was pointed back at its placeholder before the load.

## Asking about space (ABI v11 queries)

Requires `abiVersion >= 11` and `APMF_API_v11`. Two questions. They answer and change
nothing: no claim, no facet, no handle, no world write.

**Threading: synchronous, main thread only.** The main thread is the one that runs the
player's `Actor::Update` (APMF drains its own queue there). Anywhere else a query does
nothing and returns `kQuery_NotMainThread`. SKSE's `AddTask` is NOT that thread. A client
with its own player-Update pump (MFO's `MainThread::Post`) calls straight in from it.

**Sizes.** Every v11 struct starts with `size`. Set it to `sizeof(struct)`. APMF refuses
an input below the v11 layout and never writes an output past the size you declared.

```cpp
// Where can a summon-sized thing stand, 180u in front of this follower?
APMF_API::APMF_SpaceQuery q{};
q.size      = sizeof(q);
q.origin    = followerID;     // REQUIRED: a loaded reference; its cell gives the world
q.distance  = 180.0f;
q.clearance = 80.0f;          // free radius wanted around the point
APMF_API::APMF_SpaceResult r{};
r.size = sizeof(r);
if (g_apmf11->FindEmptySpace(&q, &r) == APMF_API::kQuery_Ok) { /* r.x, r.y, r.z */ }

// Which hostiles to this follower stand inside 300u of that point?
APMF_API::APMF_HostileQuery hq{};
hq.size = sizeof(hq); hq.side = followerID; hq.x = r.x; hq.y = r.y; hq.z = r.z; hq.radius = 300.0f;
RE::FormID foes[16]; std::uint32_t n = 0, total = 0;
g_apmf11->FindHostilesInSpace(&hq, foes, 16, &n, &total);   // nearest first; total = before the cap
```

| Query | Answers | Cost |
|---|---|---|
| `FindEmptySpace` | A ground point `distance` along the heading (the origin's facing, or `heading` with `kSpaceFlag_UseHeading`) from the origin (its position, or `originX/Y/Z` with `kSpaceFlag_OriginIsPoint`) with `clearance` free around it. Fails by name: `Blocked` (a wall or an actor on the walk; `kSpaceFlag_ClampToWall` shortens the walk instead), `NoGround` (nothing within `maxDrop`, default 128u, below the origin's feet), `NotGround` (the ground ray hit something other than static/terrain/ground/stairs; `detail` = the layer), `Occupied` (a live actor inside the clearance, including one whose capsule the ground ray hit; `detail` = its FormID), `NoClearance` (geometry within the clearance), `Ledge` (the clearance ring is not within 48u of level) | At most 14 ray casts (walk, ground, 8 clearance, 4 ring) under the havok world's read lock, plus one pass over the high actors |
| `FindHostilesInSpace` | Live actors within `radius` of the point, in the side actor's worldspace (or cell indoors), for which `candidate->IsHostileToActor(side)` is true. That is the engine's own test, the one Papyrus `Actor.IsHostileToActor` calls. Nearest first, at most `min(capacity, 64)` written | One pass over the high actors (a distance each), one engine hostility test per actor inside the sphere, one sort |

Statuses (`QueryStatus`): `Ok`, `NotMainThread`, `BadArgs`, `Unsupported` (VR, an
unverified runtime, before kDataLoaded), `NoOrigin`, `NoWorld`, `Blocked`, `NoGround`,
`NotGround`, `Occupied`, `NoClearance`, `Ledge`, `Failed` (an exception was caught).
Every non-`Ok` answer is one `[space] ...` WARN line naming the reason.

## Declaring what an NPC wears (ABI v7, `kIntent_EquipAuthority`)

Requires `abiVersion >= 7` and `APMF_API_v7`. This facet is the engine-equip
facet taken whole: you declare the worn set, APMF equips it and refuses every
other engine equip on that actor until you re-declare or release. Outfit
re-apply, the AI's own weapon and armor choice, combat re-arm and RemoveItem's
re-equip all stop reaching the actor. The player is never subject to it.

```cpp
APMF_API::APMF_Param p{};
p.ival = 0;   // or APMF_API::kEquipAuth_DenyScript to refuse Papyrus/console equips too
APMF_API::Handle h = g_apmf->RequestEx(actorFormID, APMF_API::kIntent_EquipAuthority, basis, &p);
// The claim alone changes nothing. Now declare the set (base FormIDs: weapons,
// armor, jewelry). Everything not in it is refused from here on; everything in
// it that the actor owns and is not wearing is equipped once, on the main thread.
RE::FormID worn[] = { swordID, shieldID, cuirassID, bootsID, gauntletsID, helmetID, ringID };
g_apmf->SetEquipSet(h, worn, 7);
// ...later, a different loadout: declare again. Items dropped from the set are
// not unequipped by APMF; the engine's own worker displaces them when a declared
// item takes their slot, and the seat refuses any attempt to put them back.
g_apmf->SetEquipSet(h, worn2, n2);
// Done: the engine owns the worn set again. Nothing is unequipped.
g_apmf->Release(h);
```

Rules of the road:

- **Standing, not bounded.** There is no TTL on this claim. It ends only with
  `Release` (or by losing arbitration to a higher basis). Release before a
  save if you do not want the declaration to be the thing the player loads
  back into, though what they load into is exactly what you declared.
- **Declare, do not tick.** Call `SetEquipSet` when your loadout changes, not
  every frame. Each call on the owning claim runs one equip pass. Re-issuing
  the same set is a legitimate way to ask for an item back after a script
  unequipped it, and it is honoured at once: every declaration walks the
  inventory and equips what is missing. What is bounded is the re-issue: an
  item APMF queued is held for 3 s before it is queued again. A
  per-tick client therefore pays one inventory walk per tick and at most one
  re-issue per item per 3 s, all logged. That is bounded and cannot pile up
  the engine's equip queue, but it is a client-driven re-issue every 3 s for
  any item the engine keeps taking back off, which is exactly the kind of
  churn the log will show you. Declare on change.
- **The set must be simultaneously wearable.** A two-hander and a shield, or
  two items for one slot, make the engine displace one with the other on
  every pass. APMF never picks which one wins. Declare one loadout.
- **The engine's replacement equip is refused.** If you `RemoveItem` or
  unequip a declared weapon, the engine's own re-equip of whatever it would
  pick next is an off-set equip and is refused: the follower stays unarmed
  until you re-declare. Re-declare in the same breath as the removal.
- **An unloaded actor is stored, not equipped.** A declaration for an actor
  whose 3D is not loaded is kept and the seat denies for it, but the equip
  pass is skipped (logged). Re-declare once the actor is loaded.
- **`SetOutfit` is an engine path.** A Papyrus `SetOutfit` ends in the engine's
  own outfit apply (`OutfitApply`), not in the script equip path, so it is
  refused even when scripts are exempt.
- **Bound weapons are equips too.** A conjured bound weapon equips itself
  through the seat (`path=BoundItem`). Under enforcement that is an off-set
  equip and is refused unless the bound weapon form is in your declared set.
  A follower that casts Bound Sword needs `BoundSword` (WEAP) declared, or
  will cast and hold nothing.
- **Do not mix `kIntent_Equipment` (ch.15) with a ch.17 claim on one actor.**
  Ch.15's no-param probe path re-equips the weapon it removed on release,
  from inside APMF.dll. On a ch.17 actor that re-equip is an equip from
  outside the engine (`External(APMF.dll)`) and is refused unless the weapon
  is in the declared set. Ch.15's param form (gate only) makes no equip and
  is unaffected.
- **APMF never adds items.** A declared item the actor does not own is skipped
  and logged. Give it to them first.
- **Unequips are not refused.** `kEquipAuth_DenyUnequip` is reserved and a
  claim carrying it is accepted with the bit ignored. If something unequips a
  declared item, re-declare.
- **An empty declaration clears.** `SetEquipSet(h, nullptr, 0)` returns the
  actor to pass-through without releasing the claim.
- **Observe-only first.** The first field build ships with
  `[EquipAuthority] bEquipObserveOnly=1`: every verdict is logged as
  `would-deny` and nothing is refused. It is switched to enforcing once the
  probe criteria below pass. A client can also set `kEquipAuth_ObserveOnly`
  on its own claim.
- **Only ARMO, WEAP, AMMO and LIGH are governed (ABI v8).** See the v8
  section below. A potion, food, a scroll, an ingredient or a book passes the
  seat untouched, so `DrinkPotion` and the AI's own potion use are never
  refused. Do not declare one: the equip pass skips it and logs why.
- **The player's own equips pass (ABI v8).** The player dressing the follower
  through the trade or gift menu (`path=PlayerMenu`) is allowed by default.
  Set `kEquipAuth_DenyPlayerMenu` for the strict form.
- **The authority can be scoped (ABI v9).** By default the claim holds the
  whole facet, as above. `SetEquipScope` narrows it to the categories you own
  (hands, armor, shield, ammo, light) and can refuse a category outright. See
  the v9 section below.

## Declaring hands (ABI v8, `SetEquipSetEx`)

Requires `abiVersion >= 8` and `APMF_API_v8`. The v7 `SetEquipSet` carries bare
FormIDs and APMF equips them without a slot, so the engine picks the hand. A
slot-less one-hander always lands in the RIGHT hand and evicts whatever was
there (MFO measured it, 2026-09-15). Two hand-held items therefore cannot be
declared through v7: a dagger meant for the off-hand evicts the sword.

`SetEquipSetEx` takes one `APMF_EquipEntry` per item, with the hand:

```cpp
// Layout, byte-shared and static_assert-pinned in APMF_API.h:
//   +0 form (u32)  +4 slot (u8: kEquipSlot_Default 0 / kEquipSlot_Right 1 / kEquipSlot_Left 2)
//   +5 reserved[3] (must be 0)   size 8, align 4
APMF_API::APMF_EquipEntry worn[] = {
    { swordID,    APMF_API::kEquipSlot_Right,   {} },
    { daggerID,   APMF_API::kEquipSlot_Left,    {} },   // dual wield: the off-hand
    { cuirassID,  APMF_API::kEquipSlot_Default, {} },   // body armor: the engine picks
    { arrowsID,   APMF_API::kEquipSlot_Default, {} },
};
g_apmf->SetEquipSetEx(h, worn, 4);
```

Rules of the road for hands:

- **`kEquipSlot_Default` is v7 verbatim.** No slot is passed, the engine picks,
  and "worn" means any worn instance. Use it for a two-hander, a bow, ammo and
  every armor piece that is not a shield. `SetEquipSet` (v7) is implemented as
  `SetEquipSetEx` with every slot Default.
- **`kEquipSlot_Right` / `kEquipSlot_Left` name the hand.** APMF hands the
  engine's own RightHand / LeftHand equip slot (Skyrim.esm EQUP `0x13F42` /
  `0x13F43`, resolved by FormID) to `EquipObject`. Use them ONLY for hand-held
  items: a one-hand weapon, a shield, a torch.
- **"Not worn" for a handed entry means "not in THAT hand".** APMF reads what
  the actor holds in each hand and compares. A sword the actor holds in the
  right hand, declared Left, is re-equipped into the left hand.
- **The same form may appear once per hand.** Two identical daggers, one per
  hand, is a legal declaration. APMF counts the instances it needs per form
  against the inventory count and skips (logged) any instance the actor does
  not own enough copies of. It never adds items.
- **The deny half does not know hands.** The seat compares FormIDs only. A
  declared item is in-set whichever hand the engine tries to put it in. The
  hand is what APMF's own equip pass enforces.
- **A slot value outside 0..2 is treated as Default** and logged once per
  handle. The reserved bytes are ignored by this APMF and must be 0.
- **Log lines.** Each APMF equip prints `hand=right|left|default`. The
  `SET-EQUIP-SET` line prints how many entries carry a hand. The pass summary
  gained `not-governed=N` for declared items of a type the seat does not
  govern.

### The governed form types (ABI v8)

`EquipObject` is also how a potion is drunk, food eaten, a scroll read, an
ingredient tasted and a book read. Those are not the worn set. The seat
governs only these form types, on both runtimes:

| Governed | Passes untouched |
|---|---|
| ARMO (armor, shields, jewelry) | ALCH (potions, food) |
| WEAP (weapons) | SCRL (scrolls) |
| AMMO (arrows, bolts) | INGR (ingredients) |
| LIGH (torches) | BOOK, and every other type |

A non-governed equip on a claimed actor is allowed and logged ONCE per actor
and form type at debug level (`verdict=allow (not a governed type...)`), never
per event. APMF's equip pass applies the same set: a declared item of any
other type is skipped and logged, because equipping it would consume it.

### A refused claim means keep your own equips (ABI v8)

A `Request`/`RequestEx` for `kIntent_EquipAuthority` returns `kInvalidHandle`
while the equip seat is NOT installed: `[EquipAuthority] bEquipAuthority=0`, VR,
a runtime other than 1.6.1170 / 1.5.97, a site-verify refusal (another patcher
at one of the worker's call sites), or a request made before kDataLoaded. The
v7 build accepted the claim and enforced nothing, which left a client that had
turned its own equips off on a successful claim equipping nothing all session.
APMF logs once: `[apmf][equip-auth] claim refused: seat not installed (<reason>)`.
Treat a refused claim exactly like an absent APMF: run your own equip path.

For an ACCEPTED claim, `IsEquipAuthorityEnforced()` (v8) tells observe from
enforce: true only when the seat is installed AND `bEquipObserveOnly=0`. In
observe mode APMF equips your declared set but refuses nothing, so the engine
can still take it back off. Your claim's own `kEquipAuth_ObserveOnly` bit is
not consulted by that call.

```cpp
if (h == APMF_API::kInvalidHandle) { /* seat not installed: keep your own equips */ }
else if (!g_apmf->IsEquipAuthorityEnforced()) { /* observe mode: APMF equips, engine may undo */ }
```

### The player menu policy (ABI v8)

The player dressing a follower by hand through the trade or gift menu
(`path=PlayerMenu`) is a deliberate act and is ALLOWED by default, even for an
off-set item. The log line reads `path=PlayerMenu verdict=allow (player
agency)`. A client that wants the strict form sets `kEquipAuth_DenyPlayerMenu`
on its claim and the path is then refused like any other engine equip. There
is no INI twin for this bit. The allow is at the seat only. The next enforce pass re-equips any declared item the player's equip displaced. A client honouring player agency must observe the change (the `path=PlayerMenu verdict=allow (player agency)` line, or its own inventory read) and fold the player's choice into its next declaration. A v7 APMF ignores the bit, so a v8 client running
against a v7 APMF gets the v7 behaviour (PlayerMenu refused).

## Scoping the authority (ABI v9, `SetEquipScope`)

Requires `abiVersion >= 9` and `APMF_API_v9`. v7 and v8 take the facet whole: a
declaration refuses every off-set engine equip of a governed type on the actor.
A client that only wants to own the hands must then also declare the body
armor, the shield, the arrows and the torch, or watch the engine's own outfit
refresh and combat re-arm get refused for them. The first v8 field run showed
exactly that: 117 `CombatNode` and `OutfitApply` would-denies, every one
against a hand-only intent.

v9 scopes the claim. Each governed item COMPETES for one or more categories,
and the claim says which categories it OWNS and which it DENIES:

```cpp
// Layout, byte-shared and static_assert-pinned in APMF_API.h:
//   +0 owned (u32)  +4 denied (u32)  +8 reserved[2] (u32, must be 0)   size 16, align 4
APMF_API::APMF_EquipScope scope{};
scope.owned  = APMF_API::kEquipCat_Armor | APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left;
scope.denied = APMF_API::kEquipCat_Shield;   // this follower never raises a shield
g_apmf->SetEquipScope(h, &scope);
g_apmf->SetEquipScope(h, nullptr);           // reset to the default {kEquipCat_All, 0}
```

The category map is ONE function in APMF (`apmf::equipsink::Categorize`), run
by the seat with the engine's resolved slot and by the equip pass with the
hand you declared, so the two halves can never disagree:

| Item | Competes for |
|---|---|
| ARMO without the shield biped bit (or with no bits) | `Armor` |
| ARMO with `BipedObjectSlot::kShield` only | `Shield` + `Left` |
| ARMO with `kShield` AND any other biped bit (a modded shield-on-back piece) | `Shield` + `Left` + `Armor` (both kinds of bits, both categories) |
| WEAP two-handed sword, two-handed axe, bow, crossbow | `Right` + `Left` |
| WEAP one-handed (incl. staff), equip slot LeftHand `0x13F43` | `Left` |
| WEAP one-handed (incl. staff), equip slot RightHand `0x13F42` | `Right` |
| WEAP one-handed, any other slot or none (EitherHand) | `Right` + `Left` |
| AMMO | `Ammo` |
| LIGH (torch) | `Light` + `Left` |

The either-hand row is conservative on purpose: when the engine has not picked
a hand yet the item competes for both, so a claim that owns only one hand
still holds it. The same rule applies to your own declaration: a one-hander
declared with `kEquipSlot_Default` competes for BOTH hands and is equipped
only when both are owned. Declare the hand with `SetEquipSetEx` when you own
one.

The seat's verdict for a governed equip on a claimed actor, in order:

| Step | Condition | Verdict |
|---|---|---|
| 0 | Script or console path, claim without `kEquipAuth_DenyScript` | allow (exempt, above both masks) |
| 1 | `PlayerMenu` path, claim without `kEquipAuth_DenyPlayerMenu` | allow (player agency, above both masks) |
| 2 | item competes for a DENIED category | deny, even if the item is in the set |
| 3 | item competes for an OWNED category, no declaration yet | allow (declare then enforce) |
| 4 | item competes for an OWNED category, item is in the set | allow |
| 5 | item competes for an OWNED category, item is off-set | deny |
| 6 | item competes for nothing the claim owns or denies | allow (`owned=0`) |

Observe-only turns each deny into `would-deny`. The default scope
`{kEquipCat_All, 0}` makes steps 2 and 6 unreachable, so a v7 or v8 client
sees the v8 verdict table unchanged. One label differs from v8: a `PlayerMenu`
equip of an IN-SET item now logs `verdict=allow (player agency)` (step 1 runs
before the in-set test), where v8 logged plain `verdict=allow`. The verdict is
the same, only the label moved.

Rules of the road for the scope:

- **`nullptr` resets** the claim to `{kEquipCat_All, 0}`. Bits outside
  `kEquipCat_All` are masked off and logged once per handle, never refused: a
  client built against a later APMF degrades to what this one knows.
- **A stale handle is a silent no-op**, like `SetEquipSet`.
- **The scope is stored whether or not the claim owns the channel** and rides
  the same snapshot read as the set. The seat never sees a set from one
  generation and a scope from another.
- **A changed scope on the owning claim is a declaration event.** One enforce
  hop runs after Publish, exactly as after `SetEquipSetEx`. An unchanged
  re-send fires nothing. The log line reads
  `[ctl] ... SET-EQUIP-SCOPE (h=N, owned=0x.., denied=0x.., changed|unchanged, owner|not owner)`.
- **The equip pass equips a declared item only when every category it
  competes for is owned and none is denied.** The rest are skipped and counted
  on the pass line as `skipped-unowned=N skipped-denied=N`, and named once per
  actor and set signature at warn level. APMF never equips or evicts into an
  unowned category and still never unequips anything.
- **Log lines.** Each `[apmf][equip-obs]` line now carries, after `tls=`:
  `cat=<Armor|Shield|Right|Left|Ammo|Light, +-joined>` (what the item competes
  for), `owned=<0|1>` (any competed category owned), `black=<0|1>` (any
  competed category denied) and `eslot=<none|R|L|E|hex>` (the engine's
  resolved slot: none, RightHand, LeftHand, EitherHand, or the slot's FormID).

### The probe criteria (what a field log must show before enforcement is switched on)

Read `[apmf][equip-obs]` lines. Each carries `actor= item= op=equip path=
site= ret= q= f= s= a= tls= cat= owned= black= eslot= verdict= name=`. `tls` is
attribution only (an equip APMF issued itself runs with `tls>0`); it never
changes a verdict. `cat= owned= black= eslot=` are the v9 scope fields (see
the v9 section). The `name='...'` field is the item's display name, for
reading, not for parsing.

1. Every engine re-equip of an off-set item in an owned or denied category on
   a claimed actor logs `verdict=would-deny` with a NAMED path (`OutfitApply`, `AddWornOutfit`,
   `AiCommand`, `RemoveItemReequip`, `CombatNode`, `Script`, `Console`,
   `WorkerReentry`, `QueuedApply`, `DropObject`, `PickUpObject`, `BoundItem`,
   `ProcedureEat`, `InventoryReequip`, `StartCombat`, or `External(<dll>)`).
   Two engine callers stay `Unknown` on purpose: 16104/15864 (a re-equip
   helper whose second route could not be pinned) and 52410/51535 (an
   orphan with no reference in the image). Either on a deny line is a real
   finding; see `Docs/DENY-COMPLETENESS-AUDIT.md` row 17. A `PlayerMenu`
   off-set equip logs `verdict=allow (player agency)` unless the claim carries
   `kEquipAuth_DenyPlayerMenu`, and a non-governed form type (a potion) logs
   no per-event line at all.
2. ZERO `would-deny` lines with `tls>0`. APMF's own equip pass only ever issues
   declared items, so a would-deny inside the bracket means the set the seat
   reads is not the set the pass enforced.
3. ZERO `path=Unknown(<id>)` on a `deny` or `would-deny` line. An unknown id
   there is an engine caller the path table does not name yet; add it before
   enforcing. (An `Unknown` on an `allow` line is a caller that only ever
   equips declared items; name it when convenient.) Note that APMF's own
   queued equips come back one actor update later as `path=QueuedApply tls=0
   verdict=allow`; that is the engine applying what APMF queued, not a leak.
4. Every item APMF equips shows up as an `[apmf][equip-auth] ... -> equip` line
   followed by its `[apmf][equip-obs] ... tls=1 verdict=allow` line. For an
   actor in high or middle-high AI process (a follower near the player) a
   `path=QueuedApply ... verdict=allow` apply line follows one actor update
   later: the engine queues the equip and applies it from its AI process. A
   loaded actor in low process applies directly beneath the seat, so no
   `QueuedApply` line appears for it. Its absence on a low-process actor is
   not a failure.
5. At least one engine id attributes: the log must contain at least one
   `[apmf][equip-obs]` line whose `path` is a named engine path (not
   `External(...)`). If every line reads `External(<dll>)`, another plugin
   has inline-detoured the engine's `EquipObject` entry and every caller now
   returns through it; the seat logs `entry <id> detoured by <dll>` at install
   when it can see that. Attribution is degraded and the script exemption is
   unavailable until that detour is accounted for.

6. `path=PlayerMenu` attributes correctly: ZERO `path=PlayerMenu` lines in a
   session where the player never opens a follower's inventory, and every
   `path=PlayerMenu` line coincides with a trade or gift menu interaction. The
   six PlayerMenu ids come from prior enumeration, not from a field proof; the
   log is the proof.
7. The hand reaches the engine: at least one `[apmf][equip-auth] ... hand=left`
   equip line followed by the follower visibly dual-wielding (or holding the
   declared shield or torch in the left hand). The queued equip carries the
   slot through the engine's deferred apply on paper only until a session shows
   it.

8. A follower with NO hold switches to a bow in combat: `path=CombatNode ...
   verdict=allow cat=Right+Left owned=0 black=0 name='... Bow'` followed by
   the follower visibly shooting. (The unowned hands are the engine's.)
9. ZERO `verdict=deny` or `verdict=would-deny` lines with `owned=0 black=0`.
10. A dual-wielder (offHand==2) with no hold logs `cat=Shield+Left owned=0
    black=1 verdict=deny` (or `would-deny` in observe mode) on the shield AND
    stays shield-less.
11. Every WEAP line's `eslot` is `R`, `L` or `E` (histogram over the session).
    An `E` on a `WorkerReentry` line is the case the conservative either-hand
    rule guards; it is expected, not a failure.
12. The scope reaches the seat: after a `SET-EQUIP-SCOPE` line with a narrowed
    `owned`, every `[apmf][equip-obs]` line for that actor whose `cat=` has no
    bit in `owned` and none in `denied` reads `owned=0 black=0 verdict=allow`.
13. The deny mask fires: for a scope with a non-zero `denied`, every
    `[apmf][equip-obs]` line whose `cat=` overlaps `denied` reads `black=1` and
    `verdict=would-deny` (or `deny`), including an item that is in the declared
    set. ZERO `black=1 verdict=allow` lines except on a `Script`, `Console` or
    `PlayerMenu` path without the matching deny bit.
14. The category map agrees with the engine: every `cat=Shield+Left` line
    names a shield, every `cat=Ammo` line names arrows or bolts, every
    `cat=Light+Left` line names a torch, and a one-handed weapon with
    `eslot=R` or `eslot=L` reads `cat=Right` or `cat=Left` respectively. A
    `cat=Right+Left` line for a one-hander carries `eslot=E`, `eslot=none` or a
    non-hand `eslot`, never `R` or `L`. A `cat=Armor+Shield+Left` line (the log
    prints categories in bit order) names a piece that carries both the shield
    bit and another biped bit.
15. The equip pass honours the scope: every `enforce pass` line prints
    `owned=0x.. denied=0x..` matching the last `SET-EQUIP-SCOPE` for that
    actor, and every declared item skipped as unowned or denied appears in ONE
    `NOT equipped by this pass` warn per (actor, set) with `skipped-unowned` /
    `skipped-denied` counted on the pass line. ZERO `-> equip` lines for an
    item whose categories are not all owned, or any denied.

Only after all fifteen hold on a real session (8-11 are the behavioural scope criteria from the v9 design, 12-15 are log-invariant checks) is `bEquipObserveOnly` flipped to 0.

## Walking an NPC somewhere (ABI v10, `kIntent_Travel`)

Move-to-a-place is a staple. Before v10 the public API could not express it: `ch.6
kIntent_CombatTarget` records who owns the combat-target facet but makes no engine
call, and `ch.9 kIntent_OfferPackage` delivers a package the CLIENT has to ship in
its own plugin. A real third-party client that wanted to send followers at an enemy
tried to fill the gap with `StartCombat` on a timer and got alert-and-search pacing
instead of a charge, because `StartCombat` does not path an actor to a foe it has
never detected.

`kIntent_Travel` is the missing verb. You pass an actor and a destination; APMF
walks the actor there. Move-to-a-place is a staple — sending followers at an enemy
is just one caller of it.

### The contract, in one sentence

**ch.19 `kIntent_Travel` walks the claimed actor to a reference or a cell and
cancels the moment that actor is in combat.** It does not claim the target, does not
enter combat, does not pin a target, and does not fake perception.

The leg ends on exactly three things: **ARRIVAL**, the **ACTOR ENTERING COMBAT**
(`Actor::IsInCombat`), or the **destination going away** (disabled, deleted, or
dying DURING travel). ABI v12 adds one engine-reported end, **BLOCKED**: the engine
held the actor in its own Movement Blocked package for 3 s (see "When the walk is
blocked" below). The two-minute stuck end stays the safety net. Every end is readable
with `GetTravelLegState` (ABI v12), because your claim outlives the leg. A destination that was already dead when you targeted it is a
corpse to walk to, and its deadness never ends the leg. A live one that dies on the
way does. There is no line-of-sight test and no detection test anywhere in it.

**The destination does not have to be loaded, or nearby.** An actor will path across
cells to somewhere it cannot see, which is what a vanilla travel package does. What
bounds a hopeless leg is the combat cancel, your own `Release`, and a two-minute
safety net that reports a leg that got nowhere as a failure instead of holding a
package slot for ever.

### One intent, one facet

ch.19 claims **nothing** on your behalf. No combat target, no attack selection, no
casting, no equipment, no hands, no aggression, no movement block. If you want the
combat-target facet arbitrated, claim `kIntent_CombatTarget` yourself — the two
claims are independent and arbitrate separately.

That is deliberate (marth: *"We should avoid combining intents anyway"*), and it is
not the final word: convenience **combo intents** — one call that bundles, say,
travel plus combat target plus combat entry — are a recognised and reasonable idea,
**deferred until after the full MFO port to APMF is complete**. They are not being
refused on principle. Until then: one facet, one intent, and a client that wants two
behaviours makes two claims.

### The recipe

```cpp
using namespace APMF_API;

// One claim per actor you are moving. Keep the handle -- Repoint moves the
// destination without any release/re-claim churn.
std::unordered_map<RE::FormID, Handle> g_travel;

void SendTo(RE::FormID actor, RE::FormID destination) {
    if (!g_apmf || g_apmf->abiVersion < 10) return;   // APMF absent or pre-v10: your own path

    APMF_Param p{};
    p.form = destination;                  // REQUIRED. Any loaded reference. 0 is refused.
    p.fval = 0.0f;                         // arrival radius in units; 0 => 75u default
    p.ival = kTravel_ReleaseOnTargetDead;  // names the default; see below

    if (auto it = g_travel.find(actor); it != g_travel.end()) {
        g_apmf->Repoint(it->second, &p);   // move the destination in place
        return;
    }

    const Handle h = g_apmf->RequestEx(actor, kIntent_Travel, /*basis=*/50.0f, &p);
    if (h == kInvalidHandle) return;       // refused -- see "When a claim is refused"
    g_travel[actor] = h;
}

void StopTravel(RE::FormID actor) {
    auto it = g_travel.find(actor);
    if (it == g_travel.end()) return;
    g_apmf->Release(it->second);           // ends the claim and the leg
    g_travel.erase(it);
}
```

That is the whole client side. No tick, no re-assert, no package to ship, no
`StartCombat`, no detection poke, no `currentCombatTarget` write. If your client is
doing any of those to get an NPC to walk somewhere, delete them — every one of them
fights the engine instead of using it.

For the "hotkey, send my followers at that enemy" case, that is one `RequestEx` per
follower with the enemy as the destination. The follower runs at the enemy and
Harbinger lets go the instant the fight starts. If you also want APMF to arbitrate
who owns that follower's combat target, add your own `kIntent_CombatTarget` claim
beside it.

### The parameter fields

| field | meaning |
|---|---|
| `param.form` | **REQUIRED.** The DESTINATION's FormID — an object **REFERENCE** (REFR/ACHR: an actor, an XMarker, a container, anything loaded) or a **CELL**. The record type decides which; there is no flag to set. A zero form, or any other record type, is refused synchronously (`kInvalidHandle`) so you find out at the call. |
| `param.fval` | Arrival radius in game units. `0` means the 75u default. Anything outside **[50, 512]** is CLAMPED and the clamp is logged — never silently reinterpreted. Whatever value ends up in force is also written into the package's own stop radius for that leg, so the engine's idea of "arrived" and APMF's cannot drift apart. **Not consulted when the destination is a cell.** |
| `param.ival` | A `TravelFlags` bitmask. ABI v12 adds the gait bits (`kTravel_SpeedSet` + a 2-bit speed); see "Gait". |
| `param.posX/Y/Z` | **ABI v11:** the destination POINT when `param.ival` has `kTravel_ToPosition` (then `param.form` must be 0). Without that flag it **must be zero**: a non-zero position REFUSES the claim, with the reason in the log. See below. |

`kTravel_ReleaseOnTargetDead` **names the v1 default, it does not switch it on.**
APMF always ends the leg when a destination that was alive when targeted dies during
travel, or the destination is disabled or deleted, set or not. A destination already
dead when targeted (a corpse) is walked to, and does not end the leg by being dead. An
unloaded-but-alive destination is NOT "gone" and does not end the leg. The bit exists so
a later ABI can add its inverse without you having to guess which way the default
ran.

### What "arrived" means, per destination kind

* **A reference:** the actor is within the radius of it. Straight-line distance,
  tested on the same number the package's own stop radius was set to, so the
  engine parking the actor and APMF ending the leg happen at the same place.
* **A cell:** the actor's parent cell IS that cell. Distance is not used and the
  radius is ignored, because a cell has no single position to measure against — an
  interior is a volume and an exterior cell is a 4096-unit tile. "In it or not" is
  also exactly what the engine's own in-cell package path steers towards, so the
  two agree by construction.

### Walking to a world position (ABI v11, `kTravel_ToPosition`)

Requires `abiVersion >= 11`. Set `kTravel_ToPosition`, leave `param.form = 0`, and put
the point in `param.pos` (in the actor's worldspace; indoors, in its cell). An older APMF refuses a
zero form, so the request fails loudly on it instead of walking somewhere else.

```cpp
APMF_API::APMF_Param p{};
p.ival = APMF_API::kTravel_ToPosition;
p.posX = pt.x; p.posY = pt.y; p.posZ = pt.z;   // e.g. a FindEmptySpace result
p.fval = 0.0f;                                  // arrival radius, 0 => 75u
APMF_API::Handle h = g_apmf->RequestEx(followerID, APMF_API::kIntent_Travel, basis, &p);
```

APMF does what vanilla does with a spot: it places its OWN non-persistent XMarker
(Skyrim.esm `0x3B`) at the point, one hop after the claim publishes, and the leg is an
ordinary reference leg to that marker. Arrival radius, the combat cancel and the
2-minute stuck end are unchanged. The death rule never applies (a marker cannot die).
APMF does not pick the point and never adjusts it: call `FindEmptySpace` first if you
want a clear, standable one.

**The marker lives exactly as long as the leg.** It is deleted, by its tracked handle
with the FormID and base re-checked, when the leg ends for any reason: arrival, combat,
`Release`, a `Repoint` to a different point or to a form, the stuck end, or the actor
unloading, dying or losing its 3D. A `Repoint` to a new point places the new marker,
re-points the package, then deletes the old one. If the claim is still held after the
leg ended, a later `Repoint` places a fresh marker. At most one marker exists per
package slot (8).

**Across a load.** Legs are never restored across a load (that was already true for
every destination kind). At the load boundary APMF forgets its markers without touching
them and points every package record that last aimed at one back at its authored
placeholder, so no record carries a marker handle into the next world. A save taken
mid-leg records its markers in APMF's co-save, and loading it DELETES them (see "Save
behaviour of APMF markers" below).

**Far destinations: walk in hops.** The marker can only be placed in a LOADED cell: outdoors
the point must fall in the loaded grid (an attached cell of the actor's worldspace, found with
`TES::GetCell`), indoors it is the actor's cell. A point outside the loaded grid is refused at
Compose (`[travel] ... REFUSED -- no destination marker ... no loaded exterior cell contains the
point`), and the claim then stands doing nothing until you Release or Repoint it. To send a
follower somewhere far, walk it in hops inside the loaded area, or pass an existing REFERENCE
(or a cell) as `param.form`, which needs no marker and can be anywhere.

**Refused, by name:** a non-zero `param.form` with the flag (a form OR a point, never
both), a non-finite point, VR or a runtime other than 1.6.1170 / 1.5.97 (all
synchronous, `kInvalidHandle`), and a marker that cannot be placed because the actor's
cell is not attached (at Compose, logged, the claim stands doing nothing: Release it).

### Why the walk stopped (ABI v12, `GetTravelLegState`)

Requires `abiVersion >= 12` and `APMF_API_v12`.

**APMF ends a leg but never releases your claim.** When the actor arrives, enters
combat, loses its destination, is blocked or times out, APMF drops only its own
internal package offer. Your `kIntent_Travel` claim stays live: `IsClaimLive` stays
true and the claim does nothing until you `Repoint` or `Release` it. Before v12 a
client could not tell an ended leg from a package another mod took away. Now it asks.

```cpp
APMF_API::APMF_TravelLegInfo info{};
info.size = sizeof(info);
const auto state = g_apmf12->GetTravelLegState(followerID, &info);
if (info.ownerHandle != myHandle) { /* another client's leg, or an older claim of yours */ }
switch (state) {
case APMF_API::kLeg_Walking:  /* still going */ break;
case APMF_API::kLeg_Arrived:  /* next item */ break;
case APMF_API::kLeg_Blocked:  /* see "When the walk is blocked": actor block => reorder,
                                  static block => park the item until the world changes */ break;
case APMF_API::kLeg_DestGone: /* next item */ break;
default: break;
}
```

| state | meaning |
|---|---|
| `kLeg_None` | APMF holds nothing for this actor (never claimed since the last load, or ch.19 is off). |
| `kLeg_Pending` | The claim was accepted or re-pointed. The leg starts on the next frame. |
| `kLeg_Walking` | The package is offered and the actor is travelling. |
| `kLeg_Arrived` | Inside the arrival radius (a cell: inside the cell). |
| `kLeg_Blocked` | The engine held the actor in Movement Blocked for 3 s of running game time. `stallX/Y/Z` and `blockedMs` say where and how long; `blocker` / `blockerKind` say whether an actor stood in front. |
| `kLeg_CombatCancelled` | The actor entered combat. |
| `kLeg_DestGone` | The destination was deleted, disabled, died during travel, or (a cell) no longer resolves. |
| `kLeg_StuckTimeout` | The two-minute safety net elapsed. |
| `kLeg_ActorGone` | The actor unloaded, died or lost its 3D. |
| `kLeg_Failed` | The leg could not start, or a `Repoint` was refused (a zero form, a record that is not a reference or a cell, a reference that no longer resolves, a bad point). `destForm` / `destX/Y/Z` name the REFUSED destination and the log says why. **A refused Repoint ends the previous leg**: the actor does not keep walking to a destination your claim no longer declares. Repoint to a valid destination to start again. |
| `kLeg_Released` | The claim was released. |

How to read it:

* **Any thread.** It copies a small per-actor record under a mutex, like `IsClaimLive`.
  It holds nothing and changes nothing.
* **Match the claim.** The state is the actor's leg, which is the leg of the WINNING
  travel claim. `ownerHandle` is that claim's handle: compare it with yours. It is 0
  while a fresh claim is still Pending. During a Pending re-point, and on a `kLeg_Failed`
  for a refused re-point that came with an owner change, it still names the previous
  owner. `destForm` (or `destX/Y/Z` for a point leg) says which
  destination the state is about. After a `Repoint` the old leg's end can still show for
  up to a frame. Wait for your destination to read `kLeg_Pending` or later.
* **`seq`** is a stamp from one counter APMF never resets while the game runs. It changes
  on every state change and never repeats, across save loads too, so a cached `seq` can
  never match a new end by accident. Compare for inequality, not for +1.
  `msInState` is how long the current state has held.
* **After a save load** every actor reads `kLeg_None` until its claim is made again. The
  state is not saved, and neither are travel claims.
* **`size`.** Set `info.size = sizeof(info)` (72 bytes in v12). APMF fills the v12 fields
  when `size` is at least `kTravelLegInfoV12Size` (72, frozen) and writes nothing into a
  shorter struct. A field a later ABI appends is written only if it fits inside your
  `size`, so a v12 build keeps working against every later APMF. APMF never writes past
  `size`. `out` may be null for a state-only read.

| field | offset | meaning |
|---|---|---|
| `size` | 0 | you set it |
| `state` | 4 | a `TravelLegState` |
| `actor` | 8 | the actor asked about |
| `destForm` | 12 | the destination FormID (0 for a point leg) |
| `destX/Y/Z` | 16/20/24 | a point leg's declared point |
| `msInState` | 28 | ms since the state began |
| `seq` | 32 | the change stamp |
| `stallX/Y/Z` | 36/40/44 | `kLeg_Blocked`: where the actor stood |
| `blockedMs` | 48 | `kLeg_Blocked`: how long it was blocked |
| `speed` | 52 | the gait written (0..3), `0xFFFFFFFF` = none |
| `reserved` | 56 | 0 |
| `ownerHandle` | 60 | the travel claim this leg belongs to |
| `blocker` | 64 | `kLeg_Blocked`: the actor in front, 0 for a static block |
| `blockerKind` | 68 | a `TravelBlocker`: None / Player / Teammate / Actor |

### When the walk is blocked (ABI v12)

The engine answers "this actor cannot move along its path" by running a runtime
package of type 36, **Movement Blocked**. It is the engine's own collision answer
(`[obs]` prints it as `(Movement Blocked)`). A closed lever portcullis produced it
for 39-45 s in the field, while healthy legs showed it for about one second and
then arrived. So ch.19 ends the leg as **BLOCKED** once it has held for **3 s of
running game time** (checked every 250 ms). Time spent in a menu does not count:
each poll adds at most 500 ms, so a pause cannot turn a short bump into a BLOCKED
end. One poll without Movement Blocked inside a run is tolerated, so a brief flicker
does not restart the clock. Two in a row end the run.

APMF does not fight or deny Movement Blocked. It records the end and lets you
decide. At the verdict it looks for a live actor right in front of the stalled one:
within 192u on the ground plane, less than 128u above or below, inside a 120-degree
cone around the actor's facing or around the straight line to the destination. The
nearest one is reported in `blocker`, with `blockerKind` = the player, a teammate
(a follower) or another actor.

What to do with it (marth's loot rules, and good advice for any client):

* **An actor block** (`blockerKind != kBlocker_None`): someone is standing in the way
  and will probably move. **Reorder, do not drop.** Take another item now and come back
  to this one later.
* **A static block** (`kBlocker_None`): a closed gate, a wall or clutter. The route is
  closed. Park the item until the world actually changes: a door or gate opens
  (`TESOpenCloseEvent`), a lever is used (`TESActivateEvent`), or a cell attaches.
  Do not use a timer to retry it, and do not send the actor home while other reachable
  items remain.

A BLOCKED end also runs a **passive gate probe** (log only). It lists the DOOR and
ACTIVATOR references within 1024u of where the actor stalled, with their base form,
record flags (Obstacle, NavMesh filter) and `GetOpenState`, then logs OPEN-CLOSE and
ACTIVATE events near the stall for ten minutes, with the gates' open state read again
right after the event and 4 s later. Lines are tagged `[travel-gate]`.

### Gait (ABI v12)

Requires `abiVersion >= 12`. An older APMF stores the bits and walks at its authored
speed (Run) without a word, so check the version first.

```cpp
p.ival = APMF_API::kTravel_SpeedSet | APMF_API::kTravel_SpeedWalk;   // or Jog / Run / FastWalk
```

The four values are the engine's own `PreferredSpeed` enum. APMF writes the speed
into the leg's package record, with the record's "Preferred Speed" flag (0x2000)
that the engine requires before it honours the speed at all. Without
`kTravel_SpeedSet` the leg runs at the record's authored speed, even if the previous
leg on the same record declared a different one.

**It takes effect when the package starts.** The engine copies the record's speed
into the actor's running-package state when the package starts on the actor, and
movement reads that copy (read on both 1.6.1170 and 1.5.97). APMF writes the record
before it offers the package, so a fresh leg walks at the declared gait. A `Repoint`
that changes the gait of a leg already walking rewrites the record, but the running
package keeps its old speed until it next starts. APMF logs a warning for that case.
Release and re-request to change gait mid-walk. `GetTravelLegState`'s `speed` field
reports what was written (`0xFFFFFFFF` = nothing written). A `[travel-gait]` line
per gait leg shows the engine's running copy once the package runs.

### Why a package cannot carry a world position

Because the engine cannot carry one. An AI package's location holds a form pointer
or a reference handle and nothing else — there is no coordinate storage in it at
runtime, the on-disk record is twelve bytes of type/payload/radius, and none of the
engine's own location kinds reads coordinates out of it. That was read off both
game binaries rather than assumed.

Vanilla's answer is to **place an XMarker and point at the marker's reference.** You
can do that yourself (case one above), or set `kTravel_ToPosition` and APMF does it
for you (the section above). A claim with a non-zero `param.pos` and no
`kTravel_ToPosition` is refused and says so, rather than quietly walking the actor
somewhere you did not ask for.

### When a claim is refused — and the two timings, which are not the same

**Synchronously.** `RequestEx` returns `kInvalidHandle` before anything is queued
when:

* the runtime is VR (the 0x49 package seat travel rides is SE/AE only),
* `[Travel] bTravel=0` in `Data/SKSE/Plugins/APMF.ini`,
* `Data/APMF.esl` is missing or disabled, so no travel package resolved,
* `param.form` is 0,
* `param.posX/posY/posZ` is non-zero.

There is nothing to clean up after one of these. A synchronous refusal means **run
your own path**: APMF is telling you it will do nothing, rather than accepting a
claim that silently does nothing.

**At engage, one frame later.** One refusal cannot be synchronous: `param.form`
naming a record that is **neither an object reference nor a cell**. `RequestEx` has
already handed you a LIVE HANDLE by then; the log says why the claim will do
nothing, and **the handle stays live until you `Release` it.** So release it.

Why it works that way, since the asymmetry is otherwise just annoying: `RequestEx`
is callable from any thread, and its contract is that it copies POD and enqueues —
nothing more. Deciding whether a FormID is a reference or a cell needs a form
lookup, and APMF will not reach into the engine's form table off the game thread to
satisfy an API call. `kIntent_Cast` defers its own package extraction for exactly
the same reason. Pass a reference or a cell and the question never arises.

### Installing `Data/APMF.esl`

Harbinger ships one plugin, `Data/APMF.esl`, in the same archive as `APMF.dll`,
beside `Data/SKSE/Plugins/APMF.ini`. It holds eight travel package records and
nothing else. It is **ESL-flagged**, so it takes no regular load-order slot (it uses
one of the 4096 light-plugin slots), it has **one master (Skyrim.esm)**, it
**overrides nothing**, and it is about 2.5 KB. Install the archive and enable the
plugin as usual; there is no further step.

If it is missing or disabled, ch.19 refuses every `kIntent_Travel` claim and names
the reason once in the log. Nothing else in Harbinger depends on it, so an absent
ESL costs exactly this one facet.

Why a plugin at all: a package's destination lives on the package RECORD, not on the
actor, so borrowing a vanilla travel package would re-point it for every actor in the
game that runs it — hijacking whatever quest owns it — and would cap Harbinger at one
travel leg globally. Building one at runtime instead needs `IPackageData` wrapper
objects the pinned CommonLib does not expose, and a dynamic form does not survive a
save. One tiny generated record set is the honest answer.

### Observe-only, and what a good log looks like

Harbinger receives commands. There is no observe mode and there never will be: the
off switch is a mod not sending a claim. If you want to watch without moving anyone,
do not claim. A claim that was accepted always moves the actor, so a live handle
means the follower is walking.

A good active session reads like this:

The line that proves the mechanism is `curPkg now 0x<pkg>` matching the package
travel pointed: that is the engine having actually adopted the package. A
`[travel-leg] ... ABANDONED` line is a FAILURE report, not noise — it means the actor
never arrived, never entered combat and never lost its destination inside the safety
net. It is logged loudly and nothing is retried.

### Limits worth knowing

* **Eight concurrent legs.** A package's destination lives on the package RECORD, so
  two actors walking to two different places need two records; APMF ships eight. A
  ninth simultaneous leg is REFUSED and logged. This is a real cap, not a soft one —
  it is never worked around by sharing a record, which would send both actors to one
  destination.
* **Combat cancels the movement, and the package does not ignore combat.** If a fight
  starts en route the engine takes the actor and the leg ends. "Walk past whoever is
  hitting you" is not on offer.
* **Arbitration is honest.** Travel's internal package offer is filed at YOUR basis,
  and it is re-filed if the winning travel claim's basis changes. If another client
  outbids you on the package-offer facet, your leg loses the package — exactly as if
  you had claimed `ch.9` yourself.


## Pinning a combat target (ABI v13, `kIntent_TargetPin`)

`kIntent_CombatTarget` (ch.6) only records who owns the combat-target facet. It makes no
engine call, so until v13 a client that wanted an NPC to fight ONE particular foe had to
hook the engine itself. `kIntent_TargetPin` (ch.20) does it inside Harbinger, at the engine's
own target selector.

### The contract

**While the NPC is fighting, the engine's own target choice is replaced with yours.** Every
combat update the engine asks its target selectors which foe the actor should fight. Harbinger
answers that question with your target, so the engine's own pick never reaches the actor. The
NPC's own AI then does the fighting: its own attacks, movement, spells and equips.

**Only among the engine's own combat targets.** The answer is replaced only when the engine
picked a target (the actor is fighting) and your target is one of the actor's combat group's
targets. If your target is not a combat target of that group, nothing is written, the engine's
pick stands, and the log says "not a combat target". Harbinger never makes anyone a target. An
actor becomes pinnable once the engine holds it as a combat target, for example after your own
combat entry (a separate concern) puts the actor in a fight with it.

**It never starts a fight.** If the engine holds no target at all (the actor is not in combat,
or the fight ended), nothing is written and the pin waits. That is the only state that leaves it
waiting: a pinned target the engine has LOST ends the pin (see below). Getting the actor into
combat is a separate concern and this intent does not do it.

**It claims nothing else.** No attack selection, no casting, no equip, no movement, no
aggression, no perception. One intent, one facet.

**It pauses while it cannot apply.** While the engine has no target, or your target is not (yet)
one of the group's combat targets, Harbinger writes nothing and the engine picks. The claim stays,
and the pin applies again as soon as both hold.

**It ends by itself when Harbinger can no longer track the target.** When the target is **lost**
(the engine can no longer locate it: its entry in the group's target list is flagged lost),
**dead**, **disabled**, **not loaded**, or no longer resolves, or when the pinned actor itself
dies, Harbinger releases your claim for you. The log says `pin ended: <reason>` and the Release
line repeats the reason. `IsClaimLive(handle)` (ABI v6) turns false and the handle is dead. This
is the only case in which Harbinger releases a pin claim on its own.

### How the pin ends, and what to do about it

* **Poll `IsClaimLive(handle)`** (ABI v6) if you need to know. A false answer on a pin you did not
  release means Harbinger ended it: the target was lost, died, was disabled or unloaded, or your
  actor died. The log has the reason.
* **Want to keep chasing?** Pin again. A lost target is one the engine cannot locate, so a new pin
  only takes effect once the engine holds and can locate it again (until then it pauses).
* **MFO**, once it is a ch.20 client (its port is a separate task), re-evaluates its gambits on its
  own cadence: its next pass picks a new target and pins
  it, or releases. It never re-pins a target Harbinger dropped without choosing it again.
* **Another mod** should treat an ended pin like any lost target: pick again, pin again if it still
  wants that actor, or let the engine choose.

### The recipe

```cpp
using namespace APMF_API;

std::unordered_map<RE::FormID, Handle> g_pin;   // one claim per actor

void FightThis(RE::FormID actor, RE::FormID target) {
    if (!g_apmf || g_apmf->abiVersion < 13) return;   // absent or older than v13: your own targeting

    APMF_Param p{};
    p.form = target;                       // REQUIRED: the target ACTOR. 0 or the actor itself is refused.

    if (auto it = g_pin.find(actor); it != g_pin.end()) {
        g_apmf->Repoint(it->second, &p);   // move the pin in place
        return;
    }
    const Handle h = g_apmf->RequestEx(actor, kIntent_TargetPin, /*basis=*/50.0f, &p);
    if (h == kInvalidHandle) return;       // refused: keep your own targeting (the log says why)
    g_pin[actor] = h;
}

void StopPin(RE::FormID actor) {
    if (auto it = g_pin.find(actor); it != g_pin.end()) {
        g_apmf->Release(it->second);       // the engine picks its own targets again
        g_pin.erase(it);
    }
}
```

No tick, no hook, no `currentCombatTarget` write of your own. If your client already has
its own target hook, make it stand down for any actor you have pinned through Harbinger: a hook
that writes the target after the engine's update overrides the pin.

### Parameter fields

| field | meaning |
|---|---|
| `param.form` | **REQUIRED.** The target ACTOR's FormID. 0 and the claimed actor itself are refused at the call. A form that is not an Actor is refused at Engage (see below). |
| everything else | Not read. |

### When a claim is refused

* **At the call (`kInvalidHandle`, logged):** Harbinger older than v13 ("no channel serves
  intent 20"), VR, a runtime other than exactly 1.6.1170 or 1.5.97, `[TargetPin]
  bTargetPin=0` in `APMF.ini`, the address self-check refused a seat, before kDataLoaded,
  `param.form` 0, `param.form` equal to the actor, or the actor is the player.
* **At Engage (one frame later):** `param.form` is not an Actor. `RequestEx` already
  returned a LIVE handle; it pins nothing and the log says why. Release it or Repoint it.

### Older and newer builds

* **A client built against v12 or older** never sees intent 20 and is unaffected.
* **A v13 client on an older Harbinger:** check `abiVersion >= 13` first. If you ask anyway,
  the older Harbinger has no channel for intent 20 and refuses the request. Either way the
  answer is the same: keep your own targeting.

### What is yours

The world's reaction to the fight. Crime, bounty, faction and aggression consequences happen the
way the engine does them. Harbinger does not stop them and Release does not undo them.

### Limits worth knowing

* **The deny is at the source, and it is checked.** Harbinger answers the engine's target
  selector itself, so there is no update where the engine's own pick is used first. Harbinger also
  watches the result of each update for a pinned actor. If the actor ends an update aimed at
  someone else and the selector was never asked, the log says `SOURCE SEAT MISSED`. If the
  selector was answered with your target and something still changed it afterwards, the log says
  `OVERWRITTEN`. The Release line reports how many times the selector was asked, how often the
  engine's pick was replaced, and each reason it was left alone.
* **Another mod writing the same target is not denied.** A mod that writes the target after the
  engine's update (its own `UpdateCombat` hook) writes last and wins. That shows up as
  `OVERWRITTEN` when Harbinger can see it.
* **Not saved.** A save load, a new game or the actor unloading drops the claim. Claim again.
* **Pin a loaded target.** A target that is not loaded ends the pin at the next check (about a
  quarter second), so pin an actor that is in the loaded area.
* **Built, CI-verified, not yet field-run.** Nobody has watched the selector seat run in a game
  yet. The first field log must show the `FIRST SOURCE DENY` line and no `SOURCE SEAT MISSED`
  before anything relies on it.

## Starting a fight (ABI v14, `kIntent_CombatEntry`)

`kIntent_CombatEntry` (ch.21) asks the engine, once, to put an NPC into combat against a target you
name. Harbinger calls the engine's own entry function, `Actor::StartCombat(target)`, on the main
thread one frame after your claim is applied. That is all it does. What the NPC does next is the
engine's AI.

### The contract

* **One call per declaration.** One call when the claim engages, one more each time you Repoint a
  live claim (or a different claim becomes the winner). No tick, no watch, no retry. Harbinger never
  starts the fight again on its own.
* **Not in combat:** the engine builds the NPC's combat controller and group with your target as the
  group's first combat target. If the engine will not take your target, it builds nothing and the
  call is a refusal. **Already in combat:** the engine is asked to add your target to the group's
  combat targets and keeps fighting whoever it was fighting.
* **Release stops nothing.** Releasing ends Harbinger's part. It calls no `StopCombat` and undoes
  nothing: the fight is the engine's now. If you want it over, call `Actor::StopCombat` yourself.
* **Entry does not choose whom the NPC fights first.** The engine's own target selector does, among
  the group's targets. To make it fight YOUR target, also claim `kIntent_TargetPin` with the same
  target (the recipe below).

### How combat entry ends

A combat-entry claim is never left live and doing nothing. Harbinger releases it itself, the log
says `entry ended: <reason>`, and `IsClaimLive(handle)` turns false, when:

| reason in the log | what happened |
|---|---|
| `engine refused entry: <cause>` | `StartCombat` returned false. The cause is named when Harbinger can see it (restrained, unconscious, dead actor, dead target); otherwise "cause not reported by the engine": its distance test, its identity test against one engine-global actor (AE id 401069 / SE id 514905), or one of its actor / process flags. |
| `entry not attempted: <why>` | The target is not an Actor, or at call time the NPC was not loaded, had no AI process or was dead, or the target was not loaded, disabled or dead. No engine call was made. |
| `combat ended` | The entry succeeded and the NPC is no longer in combat (it has no combat controller). The engine ended the fight: the foe got away, was calmed, someone called `StopCombat`. |
| `owner dead`, `target dead`, `target disabled`, `target unloaded`, `target unresolvable` | Checked about four times a second while the claim stands. |

Your own Release, an outranking claim, a save load, a new game or the NPC unloading also end it.
**To try again, send a NEW `RequestEx`.** An ended handle is dead; Repointing it does nothing.

**The engine giving up is final for that target.** If a claim ended with `combat ended` or `engine
refused entry`, and another claim on the same NPC (yours or another mod's) was waiting under it and
names the SAME target, that claim does not start the fight again: it ends with the same reason. A
waiting claim naming a different target gets its one call. The record lasts until the NPC has no
combat-entry claim left, so once every claim on it is released, a new request starts fresh.

### The recipe: enter combat + pin

Two intents, two handles. Entry puts your target into the NPC's combat group; the pin answers the
engine's target selector with it on every combat update. Claim both together: the pin WAITS (it does
not end) while your target is not yet one of the group's combat targets, and takes hold on the first
combat update after the entry put it there. Your code never needs to look at the NPC's combat state
itself; watch the two handles with `IsClaimLive`.

```cpp
using namespace APMF_API;

struct Fight { Handle entry = kInvalidHandle; Handle pin = kInvalidHandle; };
std::unordered_map<RE::FormID, Fight> g_fight;   // one per actor

void StopFight(RE::FormID actor) {
    if (auto it = g_fight.find(actor); it != g_fight.end()) {
        g_apmf->Release(it->second.pin);    // the engine picks its own targets again
        g_apmf->Release(it->second.entry);  // stops nothing: the engine ends the fight its own way
        g_fight.erase(it);
    }
}

void FightThis(RE::FormID actor, RE::FormID target) {
    if (!g_apmf || g_apmf->abiVersion < 14) return;   // absent or older than v14: start it your own way
    StopFight(actor);                                  // one fight per actor; a new target = new requests
    APMF_Param p{};
    p.form = target;                                   // REQUIRED for both: the target ACTOR
    Fight f;
    f.entry = g_apmf->RequestEx(actor, kIntent_CombatEntry, /*basis=*/50.0f, &p);
    if (f.entry == kInvalidHandle) return;             // refused at the call (the log says why)
    f.pin = g_apmf->RequestEx(actor, kIntent_TargetPin, /*basis=*/50.0f, &p);
    g_fight[actor] = f;                                // a refused pin (kInvalidHandle) just means no pin
}

// Your own per-frame (or per-tick) update.
void TickFights() {
    for (auto it = g_fight.begin(); it != g_fight.end();) {
        const auto& f = it->second;
        // Harbinger ended one of them (the log says why): drop both. To try again, call
        // FightThis() again -- that is a NEW request.
        if (!g_apmf->IsClaimLive(f.entry) || (f.pin != kInvalidHandle && !g_apmf->IsClaimLive(f.pin))) {
            g_apmf->Release(f.pin);
            g_apmf->Release(f.entry);
            it = g_fight.erase(it);
            continue;
        }
        ++it;
    }
}
```

`IsClaimLive` is ABI v6. The pin ends by itself on the same kinds of reason, plus a target the
engine has LOST.

**Watch the pin's "NOT a combat target" line when the NPC was already fighting.** On the
already-in-combat path the engine's `StartCombat` ignores whether it managed to add your target and
reports success either way, so the entry line says `ENTERED` and cannot tell you. If the target was
not added, the pin waits and logs `[ch.20] ... pin 0x... is NOT a combat target of this actor's
group` about every five seconds while the NPC keeps fighting its own foes. That line is how you find
out; your mod can release both handles and try again later with new requests.

### Parameter fields

| field | meaning |
|---|---|
| `param.form` | **REQUIRED.** The target ACTOR's FormID. 0 and the claimed actor itself are refused at the call. The player may be the target. A form that is not an Actor ends the claim one frame later. |
| everything else | Not read. |

### When a claim is refused

* **At the call (`kInvalidHandle`, logged):** Harbinger older than v14 ("no channel serves
  intent 21"), VR, a runtime other than exactly 1.6.1170 or 1.5.97, `[CombatEntry]
  bCombatEntry=0` in `APMF.ini`, the address self-check refused `StartCombat`, before
  kDataLoaded, `param.form` 0, `param.form` equal to the actor, or the actor is the player.
* **Everything later ENDS the claim** with a reason (the table above).

### What is yours

Everything the world does about the fight. Crime, bounty, faction and aggression changes, allies
and guards joining, combat music. marth: "if a user uses it to cause chaos, chaos ensues."
Harbinger does not stop any of it and Release does not undo it.

### What a good log looks like

```
[ch.21] 0x... combat-entry ENGAGED -> target 0x...: ONE Actor::StartCombat call is queued ...
[ch.20] 0x... target-pin ENGAGED -> target 0x... (same frame: the pin waits for the entry)
[ch.21] 0x... entry ENGAGED against 0x...: REQUESTED -> the engine ENTERED (was not in combat: new
        controller and group). Target is a combat-group target: yes (...). Pin: a ch.20 claim names
        this target.
[ch.20] 0x... FIRST SOURCE DENY: ...
...
[ch.21] 0x... entry ended: combat ended (target 0x..., claim h=...). ...
```

On the already-in-combat path the entry line says `not read` for group membership: the engine does
not report whether it added the target, and Harbinger does not read the group itself. The ch.20
pin's own log (`FIRST SOURCE DENY`, or `NOT a combat target of this actor's group`) answers it.
Entry lines are rate-limited to one per NPC every two seconds; the next line counts the ones held
back. Endings are never rate-limited.

### Limits worth knowing

* **Not saved.** A save load, a new game or the NPC unloading drops the claim. The fight itself is
  the engine's and is saved like any fight.
* **Loaded actors only.** Both the NPC and the target must be in the loaded area.
* **Built, CI-verified, not yet field-run.** The first field log must show `ENTERED` with `Target is
  a combat-group target: yes` before anything relies on it.

## The facet table

Every facet is one `Intent` value in `native/APMF_API.h`. The proof tier says
how much to trust a facet before you build a release around it.

- **Field-proven**: a live deck run exercised the mechanism (engage, deny,
  release) and it held up, or the facet is in active production use by a real
  client.
- **Built, not yet battle-tested**: the code compiles and the mechanism is
  sound (often reusing a mechanism already proven elsewhere in APMF), but
  this specific facet hasn't had its own field run yet.

Tested and "reads its param" are different questions. A crouch toggle can be
field-proven and working while it still applies a fixed built-in value and
ignores whatever you pass in `APMF_Param`, a plain toggle has nothing to
parametrize. The Param column is a separate note on which facets actually
consume a per-request value today versus accept-and-ignore it. Check both
columns, one doesn't imply the other.

| Intent | Facet | Param field | Proof tier |
|---|---|---|---|
| `kIntent_MovementBlock` (ch.1) | Full stand-still | none | Built, not yet battle-tested |
| `kIntent_Gait` (ch.1a) | Movement speed scale | `fval` (reserved, not yet read) | **Field-proven.** An actor-value source-block, deck-tested to hold even on a package-locked actor. |
| `kIntent_Stance` (ch.3) | Sneak/crouch toggle | `ival` (reserved, not yet read) | **Field-proven.** Deck-tested as a crouch toggle, confirmed working. |
| `kIntent_WeaponDrawn` (ch.4) | Draw/sheathe | none | Built, not yet battle-tested |
| `kIntent_Headtrack` (ch.5) | Look-at target | `form` (reserved, not yet read) | **Field-proven** for the look-up behavior itself (deck-tested). Known-incomplete as a deny gate: the AI writes several headtrack slots and APMF owns only one, so an aggressive competing source can still win the head back. |
| `kIntent_CombatTarget` (ch.6) | Claim the combat-target facet | `form` (the target actor) | **Field-proven, in active production use.** MFO drives its combat targeting through this facet every fight. Arbitration only: APMF records the owner and the client writes the target itself. Denying a competing framework's own target write is still a future gap. |
| `kIntent_CombatAction` (ch.7) | Deny named combat behavior-tree leaf categories (attack, bash, ranged attack, cast leaves, and more, grouped by category) | `ival` (a `CombatActionCategory` bitmask) | **Field-proven.** Graduated from a live deck probe: the deny fired, the tree fell back cleanly, no crash. |
| `kIntent_SelectSpell` (ch.8) | Claim the casting facet | `form` (the spell FormID) | **Field-proven, for the owned/exact cast.** A follower AI-fired an animated spell allowed only through APMF's cast-check gate, live in combat, no whack-a-mole, no crash. This covers the single claimed spell as the actor's only castable choice. The graduated multi-spell allow list (`SetSpellAllowList`, v4) is a separate capability and is not yet proven. Denying a competing framework's own spell selection is also still a future gap. |
| `kIntent_Cast` (ch.8b) | **Make the NPC's own AI cast a chosen spell at a chosen target.** ABI v5, use `RequestCast` | `APMF_CastRequest` (spell, proxy, target, flags, ttlMs) | **Field-proven** for heal-other on a follower AND for offense (17 animated offense fires in the 2026-09-06 deck session; the Offensive caster seat shipped in v0.9.2 — the "still being ported" note here was stale, corrected 2026-09-07). APMF makes no equip, anim or cast call. **Know what a claim does NOT cover, and what you must do about it:** it governs ONE hand, so the other hand keeps casting whatever the NPC's own AI picks — claim that hand too, with `kCastFlag_DenyHandOnly`, if you want the actor closed (a deny-only claim drives nothing and admits nothing, including your own spells). It expires at `ttlMs`: `Repoint` the live claim to RENEW the deadline rather than releasing and re-requesting, or foreign spells equip and charge in the gap between the two. And a spell already charging when your claim arrives is not interrupted. Both mechanisms landed on `main` 2026-09-07 and are not in a tagged release yet. See `Docs/DENY-COMPLETENESS-AUDIT.md` open gaps 9-13. |
| `kIntent_OfferPackage` (ch.9) | Claim the package-offer facet | `form` (the TESPackage FormID) | **Proven WHEN NUDGED (corrected 2026-09-07).** The 0x49 redirect answers whenever the engine asks while a published claim stands — 16/16 deck wins, every one of them following an explicit `EvaluatePackage` nudge, with ZERO hits from the engine's own evaluation cadence over ~75 s. It is not self-sustaining: if nothing nudges, nothing re-asks. The earlier "field-proven for engage/release" credit belonged to the retired PROBE; the graduated channel's own first field run engaged **0 of 6** dispatches because its nudge fired before the claim published. Fixed on `main` as of 2026-09-07 (the nudge is posted one hop past the claim's publish); untagged, and not yet re-run on a deck. Save/load persistence of an engaged claim is still unexercised. |
| `kIntent_Dialogue` (ch.10) | Pause the actor's own in-progress dialogue | none | Built, not yet battle-tested |
| `kIntent_Disposition` (ch.11) | Aggression / confidence / assistance / morality bias | `fval` (reserved, not yet read) | **Field-proven.** An actor-value source-block, deck-tested to hold even on a package-locked actor. |
| `kIntent_Idle` (ch.12) | One-shot idle/animation | none | Built, not yet battle-tested |
| `kIntent_ShoutPower` (ch.14) | Claim the shout/power selection facet | `form` (the shout/power FormID) | Built, not yet battle-tested. Arbitration only today, the same shape as ch.6. |
| `kIntent_Equipment` (ch.15) | Unequip/equip a worn item, and (with a param) gate re-equip of a spell/staff while the claim stands | `form` (optional) | Built, not yet battle-tested. The most recently landed facet in the catalog. |
| `kIntent_Detection` (ch.16) | Silent movement + reduced detection range | `fval` (reserved, not yet read) | **Field-proven.** An actor-value source-block, deck-tested to hold even on a package-locked actor. |
| `kIntent_EquipAuthority` (ch.17) | **Declare what the NPC wears; APMF equips it and refuses every other engine equip of a governed type (ARMO/WEAP/AMMO/LIGH) in the categories the claim owns.** ABI v7, declare with `SetEquipSet`; ABI v8 `SetEquipSetEx` adds a hand per item; ABI v9 `SetEquipScope` scopes the claim to owned/denied categories (default: all owned) | `ival` (an `EquipAuthFlags` bitmask); the set itself via `SetEquipSet` / `SetEquipSetEx`; the scope via `SetEquipScope` | Built, not yet battle-tested. Ships OBSERVE-ONLY (`[EquipAuthority] bEquipObserveOnly=1`) until the probe criteria above pass. The only call-site seat in APMF, under `Docs/INVARIANTS.md` #17a. Player-menu equips pass by default (v8). |
| `kIntent_Cast` + `kCastFlag_AtPosition` (ABI v11) | **Cast a Target Location spell at a world point.** A one-shot remote cast from an APMF XMarker, blamed on the actor. Not a claim: no facet held, never seen by the cast seats | `form` (the spell), `ival` (the flag alone), `pos` (the point) | Built, not yet battle-tested. CI verified only. Summons refused by name (the engine only lets the caster summon). |
| `kIntent_Travel` (ch.19) | **Walk this actor to a destination** (ABI v11: or to a world point, `kTravel_ToPosition`, via an APMF-owned XMarker). APMF points its own travel package at it and offers that package through an internal ch.9 claim at YOUR basis. The leg ends on arrival, on the actor entering combat, on the destination being gone, or (ABI v12) BLOCKED. ABI v12: `GetTravelLegState` says which, and `kTravel_SpeedSet` sets the gait | `form` (the DESTINATION, REQUIRED -- an object REFERENCE or a CELL, and it need not be loaded or nearby), `fval` (arrival radius, 0 => 75u, clamped 50-512, not used for a cell), `ival` (a `TravelFlags` bitmask) |a refused claim means the channel is off in APMF.ini, VR, the esl is missing, or eight legs are already running|
| `kIntent_TargetPin` (ch.20, ABI v13) | **Pin the actor's combat target.** Harbinger answers the engine's own target selector with your target while the actor is fighting and your target is one of its group's combat targets. Never starts combat | `form` (the target ACTOR, REQUIRED) | Built, not yet battle-tested. CI verified only. The selector seat has not been observed running in a game yet. |
| `kIntent_CombatEntry` (ch.21, ABI v14) | **Start a fight.** Harbinger calls the engine's own `StartCombat` once against your target (once more per Repoint). Never re-enters, never stops the fight; the claim ENDS on a refusal or when the fight ends. Pin the same target after it entered to make it fight THAT one | `form` (the target ACTOR, REQUIRED) | Built, not yet battle-tested. CI verified only. |

Where a field is marked "reserved, not yet read", the channel currently
applies a fixed built-in behavior and ignores whatever you pass in that field.
The ABI accepts the value today so a later APMF release can start reading it
without an ABI break. Don't design a release around a reserved field doing
anything yet.

Read `Docs/CHANNEL-MAP.md` for the full research behind every row (the exact
hook, the exact vfunc, the version-robustness notes) and `Docs/archive/ROADMAP.md` (archived) /
`Docs/STATUS.md` for what's next.

## Threading

- `Request`, `RequestEx`, `Repoint`, `Release`, and `SetSpellAllowList` are
  safe to call from any thread. They enqueue the work, APMF applies it on the
  game thread. That includes a `kCastFlag_AtPosition` RequestEx (ABI v11).
- **The exception (ABI v11): `FindEmptySpace` and `FindHostilesInSpace` are
  synchronous and run ONLY on the main (player-Update) thread.** Elsewhere they
  return `kQuery_NotMainThread` and do nothing. See "Asking about space".
- `GetTravelLegState` (ABI v12) is a read-only snapshot and is safe from any thread.
- The `APMF_Param` (or the `forms` array for `SetSpellAllowList`) you pass is
  read and copied synchronously inside the call. APMF never retains your
  pointer, so a stack temporary or a local array is fine.
- No exception ever crosses the DLL boundary in either direction. A throw
  inside APMF degrades to a safe no-op or `kInvalidHandle`, never an unwind
  into your code.

## The arbitration model

Every claim carries a `basis`, a float that is entirely yours to define (your
own priority scale, your own policy). On a channel with more than one
outstanding claim, the highest basis owns it. On a tie, the earlier claim
owns it. The channel stays engaged as long as any claim exists on it. When the
current owner releases, the next-highest claim (if any) takes over
automatically. When the last claim releases, APMF stops suppressing and the
actor's own AI resumes.

## Coexistence guarantees

This is the part that makes APMF safe to install alongside mods that know
nothing about it:

- **Chainable hooks only.** APMF never overwrites another mod's hook. Every
  deny gate calls through to whatever answered before it and only turns a YES
  into a NO for the one thing it's denying. The one exception is the equip
  authority seat (ch.17): the engine-equip facet has no virtual function on
  its path, so APMF patches the two internal call sites of the engine's own
  equip worker. It byte-verifies both before writing either; if any other
  mod has touched those bytes the whole seat refuses to install and says so
  in the log, so a collision is a missing feature, never a crash.
- **Engine-answer-first.** A deny gate always lets the engine (or the next
  hook in the chain) answer first. APMF never invents a YES on its own, it
  only ever suppresses one.
- **Withholds one input, not a whole system.** A claim covers exactly one
  facet. An actor whose cast selection is claimed keeps moving, keeps
  fighting, keeps reacting to everything else under its own AI. Nothing about
  the rest of the actor is touched.
- **Reversible on release.** When the last claim on a facet releases, the
  suppression stops immediately. There's no re-assert loop left running and
  no lingering override to clean up.

## MFO: the worked example

[MFO (marth's Follower Overhaul)](https://github.com/marthofdoom/MFO) is
APMF's first client and its worked reference integration. If you want to see
a real mod claim facets, drive them with its own mechanisms, and release them
cleanly, that's the codebase to read. MFO's `feat/apmf-cast` branch is where
that integration lives while it's being field-proven.
