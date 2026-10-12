# Harbinger facet reference

This is the reference for every facet a mod can claim through the Harbinger (APMF) client API. Each section says what the facet is for, how to claim it, what Harbinger does and does not touch while the claim stands, how long it lives, who wins a conflict, and a short example.

The source of truth is [`native/APMF_API.h`](../native/APMF_API.h) and the code under `native/channels/` and `native/core/`. If this page and the header ever disagree, the header wins. For a getting-started guide (finding the interface, checking the ABI, the claim and heartbeat and release pattern) read [INTEGRATION.md](INTEGRATION.md) first.

Everything here is for mod authors. A player never needs it. The player-facing pieces are [INI-REFERENCE.md](INI-REFERENCE.md) and [HARBINGER-MENU.md](HARBINGER-MENU.md).

Where the code is ambiguous or this page could not confirm something, the text says `TODO(verify)`. Where a facet is built but has not been seen working in game, the text says so.

## Contents

- [How to read this page](#how-to-read-this-page)
- [Shared concepts](#shared-concepts)
- [Facet index](#facet-index)
- [Movement and body](#movement-and-body): [ch.1 Movement block](#ch1-movement-block-kintent_movementblock), [ch.1a Gait](#ch1a-gait-kintent_gait), [ch.3 Stance](#ch3-stance-kintent_stance), [ch.4 Weapon drawn](#ch4-weapon-drawn-kintent_weapondrawn), [ch.5 Headtrack](#ch5-headtrack-kintent_headtrack), [ch.10 Dialogue](#ch10-dialogue-kintent_dialogue), [ch.12 Idle](#ch12-idle-kintent_idle)
- [AI disposition and senses](#ai-disposition-and-senses): [ch.11 Disposition](#ch11-disposition-kintent_disposition), [ch.16 Detection](#ch16-detection-kintent_detection)
- [Casting](#casting): [ch.8 Select spell](#ch8-select-spell-kintent_selectspell), [ch.8b Cast](#ch8b-cast-kintent_cast), [position cast](#position-cast-kcastflag_atposition), [ch.14 Shout and power](#ch14-shout-and-power-kintent_shoutpower), [ch.25 Cast guard](#ch25-cast-guard-kintent_castguard)
- [Equipment](#equipment): [ch.15 Equipment](#ch15-equipment-kintent_equipment), [ch.17 Equip authority](#ch17-equip-authority-kintent_equipauthority)
- [Packages and travel](#packages-and-travel): [ch.9 Offer package](#ch9-offer-package-kintent_offerpackage), [ch.19 Travel](#ch19-travel-kintent_travel)
- [Combat](#combat): [ch.6 Combat target](#ch6-combat-target-kintent_combattarget), [ch.7 Combat action](#ch7-combat-action-kintent_combataction), [ch.20 Target pin](#ch20-target-pin-kintent_targetpin), [ch.21 Combat entry](#ch21-combat-entry-kintent_combatentry), [ch.22 Combat re-entry deny](#ch22-combat-re-entry-deny-kintent_combatreentrydeny), [ch.23 Pursuit leash](#ch23-pursuit-leash-kintent_pursuitleash), [ch.24 Combat approach](#ch24-combat-approach-kintent_combatapproach)
- [Read-only queries (no claim)](#read-only-queries-no-claim): [space queries](#space-queries-findemptyspace-and-findhostilesinspace), [line of sight](#line-of-sight-getlineofsight), [awareness](#awareness-senseactor)
- [Observe-only facets (no claim API)](#observe-only-facets-no-claim-api)
- [Appendix A: sources and priorities per facet](#appendix-a-sources-and-priorities-per-facet)
- [Appendix B: ABI history](#appendix-b-abi-history)
- [Appendix C: refusal cheat sheet](#appendix-c-refusal-cheat-sheet)
- [Appendix D: INI keys by facet](#appendix-d-ini-keys-by-facet)
- [Appendix E: equip authority probe criteria](#appendix-e-equip-authority-probe-criteria)

## How to read this page

Every facet section uses the same layout.

- **What it is for.** Plain language, and what a player sees.
- **How to claim it.** The exact call, every argument, units, valid values, defaults, and what 0 or null means.
- **What Harbinger enforces.** What changes on the actor while your claim stands, and what Harbinger leaves alone.
- **Lifetime.** How the claim ends, whether you must heartbeat it, and what a load, an unload or a death does to it.
- **Priority and conflicts.** Who wins when two claims or a claim and a foreign source collide.
- **Examples.** Short C++ snippets built from the real header.
- **Version and INI.** The ABI that introduced the facet, the first release that shipped it, and the `APMF.ini` keys that switch it.

Every snippet on this page assumes the following prelude. It is the same one INTEGRATION.md builds.

```cpp prelude
#include "APMF_API.h"
using namespace APMF_API;

// Bound once at startup, see INTEGRATION.md. Always test abiVersion before
// you call a slot that is newer than the ABI you asked for.
extern const APMF_API_v19* g_apmf;

// A claim's basis is yours to choose, see "Basis and priority" below.
constexpr float kBasis = 50.0f;
```

Distances are in game units unless a section says otherwise. FormIDs are plain `RE::FormID` values (`std::uint32_t`). `0x14` is the player. A FormID names a base-resolved reference, not a base object, except where a section says it is a base form (a spell, an idle, a package, an item).

One header detail to know before you copy anything. `APMF_Param` carries a world point as three floats, `posX`, `posY` and `posZ`. The header comments call it `param.pos`, but there is no `pos` member.

## Shared concepts

### A claim

A claim is a request that says "I need this facet on this actor". It has four parts: the actor, the intent (which facet), a `basis` and an optional `APMF_Param`. A successful call returns a `Handle`. You keep the handle and use it to repoint, renew or release the claim.

```cpp
Handle  Request(RE::FormID actor, Intent intent, float basis);                      // ABI 1
Handle  RequestEx(RE::FormID actor, Intent intent, float basis, const APMF_Param*); // ABI 2
Handle  RequestCast(RE::FormID actor, float basis, const APMF_CastRequest*);        // ABI 5
void    Repoint(Handle, const APMF_Param*);                                         // ABI 3
void    Release(Handle);                                                            // ABI 1
bool    IsClaimLive(Handle);                                                        // ABI 6
```

`APMF_Param` is a plain struct. Zero-initialise it (`APMF_Param p{};`) and set only what the facet reads. A field a facet does not read is accepted and ignored.

| Field | Type | Used for |
|---|---|---|
| `form` | `RE::FormID` | a spell, an actor, a package, an idle, a destination, an item. 0 means "the facet default" |
| `fval` | `float` | a scalar such as a radius or a window in seconds |
| `ival` | `std::int32_t` | a flag word or a bitmask |
| `target` | `RE::FormID` | an actor or reference the request acts on |
| `posX`, `posY`, `posZ` | `float` | a world point (ABI v11) |

A `kInvalidHandle` (0) return means the claim was refused at the call. Every refusal is logged in `APMF.log` with its reason. A refusal means "do it your own way": keep your own mechanism for that facet.

### Basis and priority

These are two different numbers and they do two different jobs.

- **Basis** ranks claims against claims on the same actor and the same intent. You choose the scale. The highest basis owns the facet. On a tie the earlier claim keeps it. The losers stay stored and the next best one takes over when the owner releases. When the last claim goes, Harbinger stops suppressing and the actor's own AI takes the facet back. MFO uses 200.
- **Priority** ranks a claim's mod against a foreign source that writes the same facet (another mod's hook, the engine, a script). It is a number from 0 to 100 per source per facet, kept in `APMF_Sources.ini` and editable on the [Harbinger menu page](HARBINGER-MENU.md). A deny stands only while the claiming mod's priority is strictly above the source it overrules. A tie goes to the source.

Defaults when `APMF_Sources.ini` is missing or does not name a source:

| Source kind | Default priority |
|---|---|
| A mod that asks Harbinger (a client, found from the DLL that called the API) | 50 |
| Another mod, not a client (a hook, a plugin) | 20 |
| Papyrus and console calls (`DirectCalls`) | 75 |
| The engine | 20 |

A change Harbinger cannot trace to a named source is never denied. It is allowed, logged as `[sources] UNATTRIBUTED <facet>` and shown as an alert on the menu page. Each facet has an `[UnknownSources]` switch that lets such a change rank at the priority of the Unattributed row instead. It is off by default, and it acts today on the `Target`, `Combat` and `Cast` facets only. See [INI-REFERENCE.md](INI-REFERENCE.md).

Your DLL registers itself. Harbinger reads the return address of `Request`, `RequestEx` and `RequestCast` to find the calling module, so the source name is your DLL file name (`MyMod.dll`). You can raise or lower your own default per facet with a line in `APMF_Sources.ini`, for example `MyMod.dll=60` under `[Cast]`.

Which Intent belongs to which sources facet is listed in [Appendix A](#appendix-a-sources-and-priorities-per-facet), together with what is enforced on each today.

### Pending claims and `IsClaimLive`

A request returns a handle at once, from any thread. Harbinger applies it on the game thread at its next drain. `IsClaimLive` reads the applied state, so it answers false for a claim that is still pending. A false answer in the first moments after the call does not mean the claim ended.

MFO's practice is to treat a handle as "never seen live" until `IsClaimLive` has answered true once, and to judge it ended only after about two seconds of running (not paused) game time with no true answer. That wait is MFO's choice and it is not a Harbinger constant. It comes from the fact that the drain runs on the player update, which does not tick under a game-pausing menu. `TODO(verify)`: the exact drain cadence under a paused menu is MFO's observation, not stated in the header.

Always `Release` a handle before you forget it. `Release` on a claim Harbinger already ended is a no-op, and `Release` on a claim that is still pending cancels it, so a wrong "never live" judgement cannot leave an orphan claim.

### Ways a claim ends

| Cause | What happens |
|---|---|
| You call `Release` | Your part ends. The next best claim takes over, or the actor's own AI does |
| A higher-basis claim on the same facet | You stay stored and non-owning. You take over again if the owner releases. (Cast claims have two exceptions, see [ch.8b](#ch8b-cast-kintent_cast)) |
| A time limit | ch.8b, ch.22 and ch.25 are bounded and release themselves |
| Harbinger ends it itself | ch.20, ch.21, ch.22, ch.24 release their own claim when their goal is over or lost. `IsClaimLive` turns false and the log names the reason |
| The actor unloads | Every claim on the actor is released and the actor is dropped from control |
| A save load or a new game | Every claim is released at `kPreLoadGame`. Claims are never saved. After a load you have to claim again |
| The actor dies | Facet specific, said in each section |

A leg or a window that ends is not always a claim that ends. ch.19 Travel never releases your claim. It drops only its own package offer and leaves the claim live. Its `GetTravelLegState` tells you why the walk stopped.

### Two refusal timings

Some requests are refused at the call (`kInvalidHandle`). Others are accepted and then found unusable one drain later, on the game thread, because deciding them needs a form lookup and the API is callable from any thread. For the second kind the handle is live and inert, the log says why, and you must `Release` it. Each facet section lists both.

Every claim is refused at the call, for every intent, in these cases: the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104, VR, the arbiter seat was refused at load, or no channel serves the intent (an older APMF). The request is also refused before `kDataLoaded` on facets that name it.

### Threading

`Request`, `RequestEx`, `RequestCast`, `Repoint`, `Release`, `SetSpellAllowList`, `SetEquipSet`, `SetEquipSetEx` and `SetEquipScope` are safe from any thread. They copy the pointer you pass before they return and queue the work for the game thread. A stack temporary is fine.

The read-only reads `IsClaimLive`, `GetCastProxy`, `GetTravelLegState`, `GetLineOfSight`, `GetCombatApproachState` and `IsEquipAuthorityEnforced` are safe from any thread.

`FindEmptySpace`, `FindHostilesInSpace` and `SenseActor` are synchronous and run on the game's main thread only (the player-update thread). From any other thread they do nothing and return `kQuery_NotMainThread`. SKSE's `AddTask` is not the main thread.

No exception crosses the boundary. A throw inside Harbinger becomes `kInvalidHandle`, a no-op or a null.

### Version gating

`APMF_GetInterface(n)` returns null if the installed Harbinger is older than `n`, and the newest interface for any `n` at or below its own. Ask for the lowest ABI you can live with, then gate every newer slot and flag on `abiVersion`. The interface object always reports Harbinger's own ABI in `abiVersion`, whatever you asked for. The struct for ABI 21 is `APMF_API_v19` because ABI 20 and 21 added only flags and one intent.

| You need | Gate | Cast to |
|---|---|---|
| `Request`, `Release` | 1 | `APMF_API_v1` |
| `RequestEx` | 2 | `APMF_API_v2` |
| `Repoint` | 3 | `APMF_API_v3` |
| `SetSpellAllowList` | 4 | `APMF_API_v4` |
| `RequestCast` | 5 | `APMF_API_v5` |
| `GetCastProxy`, `IsClaimLive` | 6 | `APMF_API_v6` |
| `SetEquipSet` | 7 | `APMF_API_v7` |
| `SetEquipSetEx`, `IsEquipAuthorityEnforced` | 8 | `APMF_API_v8` |
| `SetEquipScope` | 9 | `APMF_API_v9` |
| `FindEmptySpace`, `FindHostilesInSpace` | 11 | `APMF_API_v11` |
| `GetTravelLegState` | 12 | `APMF_API_v12` |
| `GetLineOfSight`, `SenseActor` | 18 | `APMF_API_v18` |
| `GetCombatApproachState` | 19 | `APMF_API_v19` |

A new intent or flag has the same rule. An older Harbinger either refuses an unknown intent (`kInvalidHandle`, "no channel serves intent N") or silently ignores an unknown flag. The silent ones are the dangerous ones, and each section marks them.

## Facet index

"Tier" is how far to trust the facet. **Field-proven** means a live deck run exercised it, or a real client uses it. **Built** means it compiles and its mechanism is sound but it has not had its own field run. The tiers follow the project's own record. ABI is the version that introduced the facet.

| Intent | ch. | Facet | One line | ABI | Tier |
|---|---|---|---|---|---|
| [`kIntent_MovementBlock`](#ch1-movement-block-kintent_movementblock) | 1 | Movement block | Stand the actor still | 1 | Built |
| [`kIntent_Gait`](#ch1a-gait-kintent_gait) | 1a | Gait | Halve the actor's speed | 1 | Field-proven |
| [`kIntent_Stance`](#ch3-stance-kintent_stance) | 3 | Stance | Make the actor sneak | 1 | Field-proven |
| [`kIntent_WeaponDrawn`](#ch4-weapon-drawn-kintent_weapondrawn) | 4 | Weapon drawn | Draw the weapon, sheathe on release | 1 | Built |
| [`kIntent_Headtrack`](#ch5-headtrack-kintent_headtrack) | 5 | Headtrack | Make the actor look straight up | 1 | Field-proven, known incomplete |
| [`kIntent_CombatTarget`](#ch6-combat-target-kintent_combattarget) | 6 | Combat target | Record who owns the target facet | 1 | Field-proven (arbitration only) |
| [`kIntent_CombatAction`](#ch7-combat-action-kintent_combataction) | 7 | Combat action | Deny categories of combat behaviour | 1 | Field-proven |
| [`kIntent_SelectSpell`](#ch8-select-spell-kintent_selectspell) | 8 | Select spell | The only spell the actor's AI may cast | 2 | Field-proven (single spell) |
| [`kIntent_Cast`](#ch8b-cast-kintent_cast) | 8b | Cast | Make the actor's own AI cast a spell at a target | 5 | Field-proven |
| [`kCastFlag_AtPosition`](#position-cast-kcastflag_atposition) | 8b | Position cast | Cast a Target Location spell at a point | 11 | Built, off by default |
| [`kIntent_OfferPackage`](#ch9-offer-package-kintent_offerpackage) | 9 | Offer package | Hand the actor one package | 1 | Proven when nudged |
| [`kIntent_Dialogue`](#ch10-dialogue-kintent_dialogue) | 10 | Dialogue | Pause the actor's own dialogue once | 1 | Built |
| [`kIntent_Disposition`](#ch11-disposition-kintent_disposition) | 11 | Disposition | Make the actor aggressive and loyal | 1 | Field-proven |
| [`kIntent_Idle`](#ch12-idle-kintent_idle) | 12 | Idle | Play an idle, optionally at a target | 1 (form: 17) | Built |
| [`kIntent_ShoutPower`](#ch14-shout-and-power-kintent_shoutpower) | 14 | Shout and power | Record who owns the shout facet | 1 | Built (arbitration only) |
| [`kIntent_Equipment`](#ch15-equipment-kintent_equipment) | 15 | Equipment | Gate spell and staff re-arm | 1 | Built |
| [`kIntent_Detection`](#ch16-detection-kintent_detection) | 16 | Detection | Silent movement, longer detect range | 1 | Field-proven |
| [`kIntent_EquipAuthority`](#ch17-equip-authority-kintent_equipauthority) | 17 | Equip authority | Declare what the actor wears and refuse the rest | 7 | Built, ships observe-only for weapons |
| [`kIntent_Travel`](#ch19-travel-kintent_travel) | 19 | Travel | Walk the actor to a place | 10 | In use by MFO |
| [`kIntent_TargetPin`](#ch20-target-pin-kintent_targetpin) | 20 | Target pin | Hold the engine's combat target on yours | 13 | Built, CI verified |
| [`kIntent_CombatEntry`](#ch21-combat-entry-kintent_combatentry) | 21 | Combat entry | Start a fight with one `StartCombat` | 14 | Built, CI verified |
| [`kIntent_CombatReentryDeny`](#ch22-combat-re-entry-deny-kintent_combatreentrydeny) | 22 | Combat re-entry deny | Keep the actor out of combat for a while | 15 | Built, seat not yet observed |
| [`kIntent_PursuitLeash`](#ch23-pursuit-leash-kintent_pursuitleash) | 23 | Pursuit leash | Stop the actor chasing too far from an anchor | 16 | Built, not observed |
| [`kIntent_CombatApproach`](#ch24-combat-approach-kintent_combatapproach) | 24 | Combat approach | Walk toward an actor while fighting | 19 | Built, not field-run |
| [`kIntent_CastGuard`](#ch25-cast-guard-kintent_castguard) | 25 | Cast guard | Hold attack spells through friends, bound channels | 21 | Built, not field-run |
| [Space queries](#space-queries-findemptyspace-and-findhostilesinspace) | | `FindEmptySpace`, `FindHostilesInSpace` | Ask for a spot or the hostiles in a sphere | 11 | Built |
| [Line of sight](#line-of-sight-getlineofsight) | | `GetLineOfSight` | Harbinger's own ray | 18 | Used by MFO |
| [Awareness](#awareness-senseactor) | | `SenseActor` | Does this actor sense that one | 18 | Used by MFO |

Channel 18 is skipped on purpose. The number is reserved for an attack-selection design that is not on main.

## Movement and body

These facets act on the actor's body: where it stands, how fast it moves, how it holds itself. Most of them are small. Several apply a fixed built-in effect today and read no parameter. That is stated in each section so you do not design around a field that does nothing yet.

### ch.1 Movement block (`kIntent_MovementBlock`)

**What it is for.** Stand the actor perfectly still. The player sees the follower stop walking. It does not run in place and it does not teleport back when you release.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_MovementBlock, kBasis);
// or: g_apmf->RequestEx(actor, kIntent_MovementBlock, kBasis, nullptr);
```

| Argument | Meaning |
|---|---|
| `actor` | FormID of the NPC to hold |
| `basis` | ranks you against other clients on this facet |
| `param` | none. The facet reads nothing from `APMF_Param` |

**What Harbinger enforces.** Once, when your claim becomes the owner, Harbinger parks the actor's movement goal at its own position (`KeepOffsetFromActor` on itself with offset 0) and locks the mover (`SetDontMove(true)`). The planner sees "already there" and issues no locomotion command, so there is nothing to run in place and nothing to snap back. The actor's package stays current and keeps evaluating. Harbinger never swaps it. So a travel package simply waits, and finishes by itself when you release. Casting, combat, equipment, animations and dialogue are untouched.

**Lifetime.** A standing claim with no time limit. It ends on `Release`. On release Harbinger clears the parked goal and unlocks the mover. A save load releases it. If the actor unloads, the claim is dropped and there is nothing left to undo. No heartbeat is needed. Harbinger does not re-assert.

**Priority and conflicts.** Basis decides between clients. The facet belongs to the `Core` sources facet, which Harbinger counts and shows but does not deny on.

**Examples.**

Hold a follower still while it plays a give animation, then let it go.

```cpp
struct HoldState { Handle hold = kInvalidHandle; };

bool HoldStill(RE::FormID actor, HoldState& s) {
    if (s.hold != kInvalidHandle) return true;
    s.hold = g_apmf->Request(actor, kIntent_MovementBlock, kBasis);
    return s.hold != kInvalidHandle;     // 0 means refused: play the idle anyway, without the stand-still
}

void LetGo(HoldState& s) {
    if (s.hold == kInvalidHandle) return;
    g_apmf->Release(s.hold);
    s.hold = kInvalidHandle;
}
```

**Version and INI.** ABI 1. No `APMF.ini` key. It refuses itself on any runtime other than 1.6.1170, 1.5.97 and 1.7.104.

### ch.1a Gait (`kIntent_Gait`)

**What it is for.** Slow the actor down. The player sees the follower walk at half pace.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Gait, kBasis);
```

`param` is not read. `fval` is reserved for a future per-request multiplier. Today the factor is fixed at 0.5 and your value is ignored.

**What Harbinger enforces.** It sets the actor's `SpeedMult` actor value to half of its current value, once. It is an input the movement code reads, so it holds even on an actor locked by a package. The write goes through the co-saved actor-value ledger. The value before your claim is recorded, and `Release` puts it back, but only if the value still equals the one Harbinger set. If a quest or another mod changed it in the meantime, theirs stands and Harbinger drops its stale record.

**Lifetime.** Standing, ended by `Release`. The ledger is saved with the game, so a save made while the claim was live does not strand the slow speed. After a load Harbinger restores every outstanding override whether or not a claim still exists. A save load also releases the claim itself.

**Priority and conflicts.** `ActorValue` sources facet. It is watched only: another mod, a script or the game changing `SpeedMult` while you hold it is named in the log and on the menu page, and never refused. Between clients, basis decides. Do not claim `kIntent_Gait` for the same actor from two mods and expect different values, because the ledger keeps one record per actor value.

**Examples.**

```cpp
void SlowForCarry(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Gait, kBasis);
}
void EndCarry(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }
}
```

If you want a chosen speed on a walk, use the gait bits of [ch.19 Travel](#ch19-travel-kintent_travel) instead. They are per leg and they pick Walk, Jog, Run or FastWalk.

**Version and INI.** ABI 1. `iActorValueSeat` in `[ActorValueObserve]` switches the watch only.

### ch.3 Stance (`kIntent_Stance`)

**What it is for.** Make the actor sneak. The player sees the follower crouch.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Stance, kBasis);
```

`param` is not read. `ival` is reserved.

**What Harbinger enforces.** One animation event. If the actor is not already sneaking, Harbinger sends `SneakStart`. It remembers whether it started the sneak. On `Release` it sends `SneakStop` only if it was the one who started it. There is no re-assert. The event is additive over the package's own crouch decision, so a package that forces a stance can undo it. For the common case it stands as a clean toggle.

**Lifetime.** Ended by `Release`. Nothing is saved.

**Priority and conflicts.** `Core`. Basis between clients.

**Examples.**

```cpp
void Sneak(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Stance, kBasis);
}
```

**Version and INI.** ABI 1. No key.

### ch.4 Weapon drawn (`kIntent_WeaponDrawn`)

**What it is for.** Draw the actor's weapon. The player sees the follower ready its weapon.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_WeaponDrawn, kBasis);
```

`param` is not read.

**What Harbinger enforces.** When the claim becomes the owner, Harbinger calls the engine's draw (`DrawWeaponMagicHands(true)`) once. On `Release` it calls the sheathe (`DrawWeaponMagicHands(false)`). The draw state is sticky, so there is no re-assert. The sheathe on release happens whatever the state was before your claim, so do not release a draw claim if you want the weapon to stay out.

**Lifetime.** Ended by `Release`.

**Priority and conflicts.** `Core`. Basis between clients.

**Examples.**

```cpp
void ReadyWeapon(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_WeaponDrawn, kBasis);
}
void Sheathe(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }   // the release is what sheathes
}
```

**Version and INI.** ABI 1. No key.

### ch.5 Headtrack (`kIntent_Headtrack`)

**What it is for.** Control where the actor looks. Today the facet is a test facet with one fixed behaviour: it makes the actor look straight up.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Headtrack, kBasis);
```

`param` is not read. `form` is reserved for a future look-at target.

**What Harbinger enforces.** It sets the actor's point-based headtrack target to a spot 300 units above it. This is the only channel that does per-tick work. It re-applies the target every tick. That re-assert is a known stopgap. The AI writes several headtrack types (default, combat, dialogue, procedure) and Harbinger owns only one slot, so a package or an aggressive source can win the head back. Treat this facet as a known-incomplete gate.

**Lifetime.** Ended by `Release`. Release clears the action headtrack target, and the AI resumes its own tracking.

**Priority and conflicts.** `Core`. Basis between clients.

**Examples.** There is little to build on until the channel reads a target. A claim looks like this.

```cpp
void LookUp(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Headtrack, kBasis);
}
```

**Version and INI.** ABI 1. No key. Field-proven for the look-up itself.

### ch.10 Dialogue (`kIntent_Dialogue`)

**What it is for.** Interrupt a conversation the actor is in. The player sees the NPC stop talking mid-line.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Dialogue, kBasis);
```

`param` is not read.

**What Harbinger enforces.** One call, `PauseCurrentDialogue()`, when the claim becomes the owner. There is no lasting block. The actor can start a new conversation afterwards. A persistent "this actor is unavailable for dialogue" toggle is a documented follow-up that is not built.

**Lifetime.** `Release` is a no-op for the actor. The claim only refcounts the engagement.

**Priority and conflicts.** `Core`. Basis between clients.

**Examples.**

```cpp
void ShushNpc(RE::FormID actor) {
    Handle h = g_apmf->Request(actor, kIntent_Dialogue, kBasis);
    if (h != kInvalidHandle) g_apmf->Release(h);     // the pause already happened at engage
}
```

**Version and INI.** ABI 1. No key. Built, not yet battle-tested.

### ch.12 Idle (`kIntent_Idle`)

**What it is for.** Play an animation on the actor. The player sees a follower do a gesture or a work animation. With ABI 17 you choose the idle record and a reference to play it at (hand an item to a crate, for example).

**How to claim it.**

Form-free, any ABI:

```cpp
Handle h = g_apmf->Request(actor, kIntent_Idle, kBasis);
```

With a form, ABI 17 or newer:

```cpp
APMF_Param p{};
p.form   = idleFormID;      // a TESIdleForm (an IDLE record). Required for the v2 path
p.target = targetRefID;     // optional reference to play it AT. 0 = none
Handle h = g_apmf->RequestEx(actor, kIntent_Idle, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `form` | 0 means the v1 behaviour. Non-zero must be an IDLE record |
| `target` | read only when `form` is non-zero. 0 lets the engine use the actor's own process target |
| `fval`, `ival`, `pos` | not read |

Gate on `abiVersion >= 17` before you set `form`. An older Harbinger does not refuse a form. It ignores it and plays the form-free default, so an unchecked client gets the wrong animation and no error.

**What Harbinger enforces.**

- v1 (form 0): one `IdleForceDefaultState` animation event at engage. Nothing at release.
- v2 (form set): one `PlayIdle(actor, idle, target)` call at engage, and one more on each `Repoint` or owner change. The idle's own conditions are evaluated by the engine against the actor and the target, and a failing one refuses it. The client picks the idle. Harbinger never does. At release it sends `IdleForceDefaultState` once, and only if the idle is still held (the actor's graph has not raised `IdleStop` since the call). A one-shot idle that already ended gets nothing.
- With `bIdleConfirm=1` an idle counts as played only when the actor's behaviour graph raises one of that idle's own entry events. If none arrives within 3 seconds of world time, Harbinger ends the claim and logs `ANIMATION NOT CONFIRMED`.

**Lifetime.** Standing, ended by `Release`. Harbinger also ends the claim, logs why and sets `IsClaimLive` false when: the form is not an IDLE, the target is not a loaded reference, the actor is not loaded or is dead or has no AI process, the engine refused the idle, or the owner died. A load or a new game ends it. `Repoint` with a new `form` or `target` plays again.

**Refusals.** At the call, only when `form` is non-zero: VR, an unsupported runtime, a self-check refusal, `bIdleV2=0`, before `kDataLoaded`, or `target` equal to the actor. At engage (the handle is live and inert, release it): the form is not an IDLE record or the target is not loaded.

**Priority and conflicts.** `Core`. Basis between clients. If the idle must not be undone by a walking package, hold the actor with [ch.1](#ch1-movement-block-kintent_movementblock) at the same time. They are separate claims.

**Examples.** MFO plays a give animation at a crate while it holds the follower still.

```cpp
struct GiveAnim { Handle hold = kInvalidHandle; Handle idle = kInvalidHandle; };

bool PlayGive(RE::FormID actor, RE::FormID idleForm, RE::FormID crate, GiveAnim& g) {
    if (g_apmf->abiVersion < 17) return false;             // an older Harbinger would play the wrong thing
    g.hold = g_apmf->Request(actor, kIntent_MovementBlock, kBasis);

    APMF_Param p{};
    p.form   = idleForm;
    p.target = crate;                                       // the engine tests the idle's conditions against it
    g.idle = g_apmf->RequestEx(actor, kIntent_Idle, kBasis, &p);
    return g.idle != kInvalidHandle;
}

void EndGive(GiveAnim& g) {
    if (g.idle != kInvalidHandle) g_apmf->Release(g.idle);
    if (g.hold != kInvalidHandle) g_apmf->Release(g.hold);
    g = {};
}
```

**Version and INI.** ABI 1, form and target from ABI 17 (first shipped in 0.9.10). `bIdleV2` and `bIdleConfirm` in `[Idle]`. The v2 path is built and CI verified. `TODO(verify)`: no field run of the v2 path is recorded in the project docs.

## AI disposition and senses

These two facets work by setting the actor values the AI itself reads. Harbinger gives the engine's own decisions a different input. It does not override a decision. Both go through the co-saved actor-value ledger described under [ch.1a](#ch1a-gait-kintent_gait), so a save made while a claim is live never strands the changed values.

### ch.11 Disposition (`kIntent_Disposition`)

**What it is for.** Make the actor fight on, hold its ground and help its friends. The player sees a follower that does not flee and that joins fights.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Disposition, kBasis);
```

`param` is not read. `fval` is reserved for a future bias value.

**What Harbinger enforces.** Four actor values are set to fixed values through the ledger.

| Actor value | Set to | Meaning |
|---|---|---|
| Aggression | 2 | Aggressive |
| Confidence | 4 | Foolhardy |
| Assistance | 2 | Helps friends and allies |
| Morality | 0 | Any crime |

The values are fixed. They are not a function of anything you pass. Morality 0 means the actor will commit any crime, so claim this knowing that. Harbinger restores the previous values on `Release` (only if they still equal what Harbinger set).

**Lifetime.** Standing, ended by `Release`. The ledger is saved, so a save while engaged does not strand the values. A load restores them.

**Priority and conflicts.** `ActorValue` sources facet, watched only. Between clients, basis decides.

**Examples.**

```cpp
void StandAndFight(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Disposition, kBasis);
}
```

**Version and INI.** ABI 1. `iActorValueSeat` in `[ActorValueObserve]` for the watch.

### ch.16 Detection (`kIntent_Detection`)

**What it is for.** Make the actor quiet and sharp-eyed. The player sees a follower that sneaks past sleeping enemies and spots things earlier.

**How to claim it.**

```cpp
Handle h = g_apmf->Request(actor, kIntent_Detection, kBasis);
```

`param` is not read. `fval` is reserved.

**What Harbinger enforces.** Two actor values through the ledger: `MovementNoiseMult` is set to 0 (silent movement) and `DetectLifeRange` is raised to 1.5 times its current value. The values are fixed. Release restores them, with the same "only if unchanged" guard as ch.1a.

**Lifetime.** Standing, ended by `Release`. Saved through the ledger.

**Priority and conflicts.** `Detection` sources facet. The two values are watched only. A change by another source is named and counted, never refused, at any priority.

**Examples.**

```cpp
void GoQuiet(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Detection, kBasis);
}
```

**Version and INI.** ABI 1. `iActorValueSeat` in `[ActorValueObserve]` for the watch.

## Casting

Four intents and one flag deal with casting. They work in different ways, so pick carefully.

| You want | Use |
|---|---|
| The NPC's own AI to cast spell S at target T, animated | [ch.8b `kIntent_Cast`](#ch8b-cast-kintent_cast) |
| The NPC to cast only one spell (or a short list) and nothing else | [ch.8 `kIntent_SelectSpell`](#ch8-select-spell-kintent_selectspell) |
| Close one hand to every other spell | a [deny-only hand claim](#a-hand-floor-kcastflag_denyhandonly) on ch.8b |
| An attack spell held while a friend is in the way | [ch.25 `kIntent_CastGuard`](#ch25-cast-guard-kintent_castguard) |
| No casting or attacking at all | [ch.7 `kIntent_CombatAction`](#ch7-combat-action-kintent_combataction) with `kCombatActionCat_Cast` |
| A Target Location spell dropped at a point, no animation | [position cast](#position-cast-kcastflag_atposition) |

### ch.8 Select spell (`kIntent_SelectSpell`)

**What it is for.** Narrow the spells an NPC's own AI is allowed to choose. While the claim stands, the only spell the AI may select and charge is the one you name, plus an optional short allow list. The player sees a follower that casts what you chose and nothing else. The AI still decides when to cast it. If you need the cast to happen at a target you name, use ch.8b.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = spellFormID;                                       // the SpellItem the AI may cast
Handle h = g_apmf->RequestEx(actor, kIntent_SelectSpell, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `form` | the spell FormID. 0 falls back to a built-in default. `TODO(verify)`: the header says the default is Firebolt, but no shipped path uses a zero form |
| `ival`, `target`, `pos` | retired. They were the old APMF-driven cast mode. Setting them does nothing |

`SetSpellAllowList` (ABI 4) adds a bounded extra set:

```cpp
void SetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count);
```

`forms` is copied inside the call. `count` is clamped to `kMaxSpellAllowList` (32). `count == 0` or `forms == nullptr` clears the set. A stale handle, or a handle on another intent, is a silent no-op. The set is stored whether or not the claim currently owns the facet.

**What Harbinger enforces.** It refuses every spell or staff that is not `param.form` and not in the allow list, at the two gates the AI's cast choice passes through: `CheckCast` (slot 0x0A on the hand casters) and `CheckShouldEquip` (slot 0x0F on the magic items). The refusal answers the engine's question with a no. It never casts anything and never equips anything. The actor's movement, combat and all other behaviour are untouched.

**Lifetime.** Standing, no time limit. It ends on `Release`, a load or an unload. It is not saved.

**Priority and conflicts.** `Cast` sources facet. The refusal stands only while your priority is above the source of the answer it replaces: the engine, or a mod that hooked the same engine function before Harbinger. Defaults are client 50 against engine 20, so with the defaults you win. An answer that came through a hook Harbinger cannot name is allowed and shown as an alert, unless `[UnknownSources] Cast=override` is set.

**Examples.** Let the follower cast Flames and its own healing spells, nothing else.

```cpp
Handle LimitSpells(RE::FormID actor, RE::FormID flames, const RE::FormID* heals, std::uint32_t nHeals) {
    APMF_Param p{};
    p.form = flames;
    Handle h = g_apmf->RequestEx(actor, kIntent_SelectSpell, kBasis, &p);
    if (h != kInvalidHandle && g_apmf->abiVersion >= 4)
        g_apmf->SetSpellAllowList(h, heals, nHeals);     // up to kMaxSpellAllowList (32), copied here
    return h;
}
```

**Version and INI.** `RequestEx` is ABI 2 (0.2.3). `SetSpellAllowList` is ABI 4 (0.3.0-rc.3). Field-proven for the single owned spell. The graduated allow list is a separate capability and is not yet proven in game. Related keys: `EnableEquipDenyComplete` in `[EquipGate]`, and the `[CastSeats]` keys (see [Appendix D](#appendix-d-ini-keys-by-facet)).

### ch.8b Cast (`kIntent_Cast`)

**What it is for.** Make an NPC cast a chosen spell at a chosen target with the game's own animation. This is how a follower heals a friend or buffs an ally. The vanilla combat AI cannot classify a healing spell aimed at someone else, so it never considers casting one. Harbinger supplies that one missing decision and the engine does everything else: it selects the spell, equips it, charges it, aims it, fires it and channels it. Harbinger makes no equip, cast or animation call of any kind. The player sees an ordinary, fully animated cast.

It also serves attack spells, wards, summons, cloaks, lights, bound weapons, invisibility and buffs, and spells the combat AI has no caster for (muffle, fortify, resist, cures, calm, fear, frenzy, courage, night eye, detect life). A claimed spell of that last kind is filed under the engine's general-purpose caster and cast exactly as written. Not every one of these seats has been seen in the field. The caster-type census in the log says which ones answered.

**How to claim it.** The rich call is `RequestCast` (ABI 5).

```cpp
Handle RequestCast(RE::FormID actor, float basis, const APMF_CastRequest* req);

struct APMF_CastRequest {
    RE::FormID    spell;   // the spell to cast (or the package, with kCastFlag_FromPackage)
    RE::FormID    proxy;   // a delivery proxy you made. 0 = none, Harbinger mints one if needed
    RE::FormID    target;  // the target actor. 0 = the caster itself. Load-bearing
    std::uint32_t flags;   // CastFlags, see below
    std::uint32_t ttlMs;   // the window in milliseconds. 0 = 4000. Clamped to 15000
};
```

| Argument | Meaning |
|---|---|
| `actor` | the caster |
| `basis` | ranks you against other cast claims on this actor |
| `req->spell` | required, except on a deny-only hand claim |
| `req->proxy` | leave 0. A Self-delivery spell aimed at someone else needs a delivery flip, because the engine's Self branch always lands on the caster. Harbinger mints the flipped copy itself and pools it. You can read its FormID back with `GetCastProxy(handle)` |
| `req->target` | where the cast lands. The engine's magic-target seat hands it to the AI as its target and the aim seat uses it as the aim point. 0 is self |
| `req->flags` | the `CastFlags` bits below |
| `req->ttlMs` | how long the claim lives without a renewal |

The degenerate form `RequestEx(actor, kIntent_Cast, basis, &p)` with `p.form = spell` and `p.ival = CastFlags` has no target, no proxy and the default window. Use `RequestCast`.

`req` is copied inside the call. The call is safe from any thread.

**The `CastFlags` word.** Flags are OR-ed. Gate each bit on the ABI that introduced it.

| Bit | Name | Meaning | ABI |
|---|---|---|---|
| 0 | `kCastFlag_FromPackage` | `spell` is a TESPackage. Harbinger extracts only its spell input and target and never runs, offers or evaluates the package. A package with no readable spell input is refused one drain later | 5 |
| 1 | `kCastFlag_LeftHand` | hand hint. Default is the right hand. Scopes the per-hand deny and the equip seat | 5 |
| 2 | `kCastFlag_Concentration` | the cast is a held stream. The time floor applies | 5 |
| 3 | `kCastFlag_DualCast` | a hint to cast with both hands. The engine still decides whether the second hand arms. Never set `kCastFlag_LeftHand` with it. If both are set, dual wins | 5 |
| 4 | `kCastFlag_DenyHandOnly` | [claim a hand purely to deny it](#a-hand-floor-kcastflag_denyhandonly) | 5 (bit added in place) |
| 5 | `kCastFlag_AtPosition` | the [position cast](#position-cast-kcastflag_atposition). `RequestEx` only | 11 |
| 6 | `kCastFlag_OwnLineOfSight` | [Harbinger's own ray](#own-line-of-sight-kcastflag_ownlineofsight) gates the cast | 18 |
| 7 | `kCastFlag_FloorSpellsOnly` | with a hand floor, refuse spells only | 20 |
| 8 to 15 | stop percent | `MakeStopPct(1..100)`, see below | 5 |

An older Harbinger ignores a flag bit it does not know and applies the rest. The ones that matter are `kCastFlag_DenyHandOnly` (an older build makes a no-form claim that denies nothing), `kCastFlag_AtPosition` (an older build makes an ordinary actor-target claim), `kCastFlag_OwnLineOfSight` (an older build casts with no line test) and `kCastFlag_FloorSpellsOnly` (an older build blocks weapons too). Check `abiVersion` before you set them.

**Stop percent.** For a concentration cast, `MakeStopPct(80)` sets bits 8 to 15 to 80. The channel then stops the moment the target reaches 80 percent of its permanent actor value. 0 means no threshold. The channel then runs to full restoration, to the target's death, to your release, or to the window's end. Without it the engine stops where its own combat style says, which for a heal is about a quarter-second pulse. `ReadStopPct(flags)` reads it back.

**What Harbinger enforces.** While your claim owns the cast facet on the actor, Harbinger answers these engine decisions for it.

- Seat 0 (classify): lets the engine's own spell classifier admit a beneficial spell aimed at another actor, or a spell the classifier has no row for.
- `CheckShouldEquip` (0x0F): which spell enters the hand. Competing spells and staffs on the claimed hand are denied.
- `CheckStartCast` (0x06): whether to start the cast.
- `GetMagicTarget` (0x0A on the combat casters): hands your `target` to the AI as its magic target.
- `CheckStopCast` (0x07): when a channel stops.
- `SetupAimController` (0x0D): aims at your target.
- `CheckCast` (0x0A on the hand casters): refuses competing spells on the claimed hand.

Beyond the seats, the hand block in `[HandBlock]` closes a claimed hand to weapons, shields and torches while the claim holds it (on by default, whatever `bEquipObserveOnly` says).

Harbinger leaves alone the other hand (the AI keeps governing it unless you floor it), movement, and every combat leaf. A spell already charging when your claim arrives is not interrupted. A cast is never a package and is invisible to ch.9.

If you want "no casting at all", ch.8b is the wrong tool. Claim [ch.7](#ch7-combat-action-kintent_combataction) with `kCombatActionCat_Cast`.

**Lifetime.** The claim is always bounded. It lives `ttlMs` (default 4000, at most 15000) from the last request or renewal, then Harbinger releases it and logs `cast claim ... expired`. A crashed client can never strand an actor.

- To hold a cast longer than one window, **heartbeat with `Repoint`**. `Repoint(handle, &param)` moves the deadline to now plus the same already-clamped window. A release and re-request cycle left the actor unclaimed for 0.26 to 0.61 seconds each window, and foreign spells equipped in the gap.
- `Repoint` cannot change what the claim casts. A `param.form` that differs from the claim's spell is refused and logged, and the claim keeps its spell. To cast something else, `Release` and `RequestCast` again. A same-form heartbeat is the shape to send.
- `IsClaimLive(handle)` turns false at expiry, release or eviction. Treat false like `kInvalidHandle` and re-request. It reads false while a fresh claim is still [pending](#pending-claims-and-isclaimlive).
- Claims on different hands coexist. A dual-cast claim and a single-hand claim cannot. The loser is refused outright, or evicted if it was already live, and `IsClaimLive` turns false. A deny-only claim is outside that collision in both directions.
- A save load or a new game releases every cast claim. An actor that unloads drops its claims.

**Priority and conflicts.** Among your own cast claims, the highest basis owns the hand. A tie keeps the earliest claim, with one exception: **at an equal basis a deny-only claim loses to a driving claim.** That is enforced for you, so a client that uses one basis for every claim can lay a floor first and still have its next cast take the hand. A floor placed at a basis strictly above a driving claim wins, and Harbinger logs that loudly because a follower holding a hand and casting nothing is hard to read from a log.

Against foreign sources, ch.8b sits on the `Cast` sources facet. Each answer Harbinger gives replaces the engine's, or an earlier mod's hook, and stands only while your priority is above that source's. Defaults are client 50 against engine 20. An answer through a hook Harbinger cannot name is allowed by default and shown as an alert.

**Examples.**

Keep a heal going on a wounded friend, with a heartbeat. This is the shape MFO uses: a change of spell or target is a new claim, an unchanged claim is renewed.

```cpp
struct HealClaim { Handle h = kInvalidHandle; RE::FormID spell = 0; RE::FormID target = 0; };

void KeepHeal(RE::FormID healer, RE::FormID healSpell, RE::FormID friendId, HealClaim& c) {
    if (g_apmf->abiVersion < 5) return;                       // no RequestCast on an older Harbinger

    const bool same = c.h != kInvalidHandle && c.spell == healSpell && c.target == friendId;
    if (same && g_apmf->IsClaimLive(c.h)) {
        APMF_Param p{};
        p.form = healSpell;                                   // same form: a pure heartbeat
        g_apmf->Repoint(c.h, &p);                             // renews the window
        return;
    }
    if (c.h != kInvalidHandle) g_apmf->Release(c.h);

    APMF_CastRequest req{};
    req.spell  = healSpell;
    req.target = friendId;
    req.flags  = kCastFlag_LeftHand                           // keep the weapon hand free
               | kCastFlag_Concentration
               | MakeStopPct(80);                             // stop the stream at 80 percent
    req.ttlMs  = 4000;
    c.h = g_apmf->RequestCast(healer, kBasis, &req);
    c.spell = healSpell;
    c.target = friendId;
}
```

Fire an offensive spell at a chosen foe, aimed by the follower's own AI.

```cpp
Handle CastAt(RE::FormID caster, RE::FormID spell, RE::FormID foe) {
    APMF_CastRequest req{};
    req.spell  = spell;
    req.target = foe;
    req.ttlMs  = 3000;
    return g_apmf->RequestCast(caster, kBasis, &req);
}
```

#### A hand floor (`kCastFlag_DenyHandOnly`)

A single-hand cast claim denies competing spells only on its own hand. The other hand stays AI governed, and the field showed the AI using it: Stone Runes, Poison Sprays and Raise Zombies charged on the free hand while the claim held the other. A deny-only claim fixes that. It claims a hand purely to deny it. The client drives nothing. Harbinger forces the claim's spell, proxy and target to 0, so no seat seats, drives or classifies anything for it.

- The floor stands on the hand `kCastFlag_LeftHand` selects (default right).
- It is a bounded cast claim. Same window rules, same `Repoint` renewal. A `Repoint` on it renews the window and nothing else. A form in the param is forced to 0.
- A floored hand admits **nothing**, including your own spells, because the floor names no form. Claim the hand with a driving `kIntent_Cast` to reopen it. A direct `CastSpellImmediate` force does not consult `CheckCast`, so a floor does not stop it.
- Without `kCastFlag_FloorSpellsOnly` a floor also refuses weapons, shields, torches and unarmed into that hand. With the flag (ABI 20) it refuses spells, scrolls and staffs only. A one-hander, a shield or a torch passes. A two-hander, a bow, or an item the engine has given no hand still competes for both hands and is refused while a driving claim holds the other one.
- A floor and a dual-cast claim coexist both ways. A dual claim never evicts a floor.
- An APMF older than the bit makes a no-form claim that denies nothing. Ship the pair together and gate on `abiVersion`.

Deny the left hand to every spell while a driving claim casts from the right. This is the "so a spell cannot be swapped out" case.

```cpp
Handle FloorLeftHand(RE::FormID actor) {
    if (g_apmf->abiVersion < 20) return kInvalidHandle;         // FloorSpellsOnly needs ABI 20
    APMF_CastRequest req{};
    req.spell = 0;                                              // a floor drives nothing. Harbinger zeroes it anyway
    req.flags = kCastFlag_DenyHandOnly | kCastFlag_LeftHand | kCastFlag_FloorSpellsOnly;
    req.ttlMs = 4000;
    return g_apmf->RequestCast(actor, kBasis, &req);            // heartbeat it with Repoint like any cast claim
}
```

#### Own line of sight (`kCastFlag_OwnLineOfSight`)

Without this bit the claim decides whether to cast, so a claim aimed at a target behind a wall is cast into the wall. The engine's own `HasLineOfSight` cannot be asked instead. For an NPC it returns a cached answer on the engine's own schedule, stale for targets the engine does not keep warm.

With the bit, and a target that is another actor, Harbinger judges the line with its own ray (see [line of sight](#line-of-sight-getlineofsight)).

- Seat 0x06 answers yes only while the ray's verdict for caster to target is VISIBLE and at most 1000 ms old. Otherwise it answers no. The claim stands and the next poll asks again. The first cast waits a frame or two for the first verdict.
- Seat 0x07 stops a held channel when the verdict is OCCLUDED on two consecutive measurements. UNKNOWN or UNAVAILABLE never stop a running channel.
- UNAVAILABLE holds the start like OCCLUDED and is logged. It is never treated as visible.
- A self target, and a deny-only claim, have no line to judge. The bit is ignored on them.
- `RequestCast` only. The degenerate `RequestEx` form carries no target, so the bit there is refused (`kInvalidHandle`, logged).
- Refused at the call while the line-of-sight service is not armed. Keep your own test then. An APMF older than ABI 18 ignores the bit and casts with no test.

**In main, next release:** a beneficial cast (a spell with no hostile effect) looks past the player and the player's teammates in its own line of sight at seats 0x06 and 0x07. Walls, the ground and every other NPC still block it. Offensive casts, the cast guard, `GetLineOfSight`, the target pin and `SenseActor` are unchanged and stay strict. The log names the friend that was looked past, once per spell, target and friend. This is merged but not released, and not yet tested in game.

**Version and INI.** `kIntent_Cast` and `RequestCast` are ABI 5 (0.9.1). `GetCastProxy` and `IsClaimLive` are ABI 6 (0.9.3). Keys: `EnableSeat0Classify` and `EnableAimSeat` in `[CastSeats]`, `EnableEquipDenyComplete` in `[EquipGate]`, `EnableScoreSteer` in `[AiCastSeats]`, `bHandClaimBlocksEquip` in `[HandBlock]`, `bOwnLineOfSight` in `[Sightline]`, and the census keys `bCasterTypeCensus` and `bRestoreCensus` in `[Probe]` (log only). Field-proven for heal-other and for offense. The non-Restore, non-Offensive caster seats and the general-purpose caster fallback are not yet field-run.

### Position cast (`kCastFlag_AtPosition`)

**What it is for.** Drop a Target Location spell at a point in the world, blamed on an actor. The actor does not animate and its hands are untouched. It is a stepping stone, not a finished feature, and it is **off by default**.

**How to claim it.** `RequestEx` only (ABI 11).

```cpp
APMF_Param p{};
p.form = spellFormID;                    // a SpellItem. Target Location, fire-and-forget, no projectile
p.ival = kCastFlag_AtPosition;           // this flag ALONE. Any other cast flag is refused
p.posX = x; p.posY = y; p.posZ = z;      // world point in the actor's worldspace (indoors: its cell)
Handle h = g_apmf->RequestEx(actor, kIntent_Cast, kBasis, &p);
```

`(0, 0, 0)` is a legal point when the flag is set, because the flag, not the value, says the field is in use. Gate on `abiVersion >= 11`. An older Harbinger ignores the flag and makes an ordinary actor-target claim.

**What Harbinger enforces.** On the game thread it places a non-persistent XMarker (Skyrim.esm `0x3B`) at the point, in the cell that contains it, and casts the spell from that marker, blamed on the actor. That is the same sequence the engine's Papyrus `Spell.RemoteCast` runs. The marker is deleted a frame later. Harbinger charges no magicka. A marker casts, and the actor does not, because the engine places a Target Location spell cast by an NPC at the NPC's own magic node and never reads the target it was given.

**Refused, each with one `[poscast] ... REFUSED` log line.**

- A summon (the engine only lets the caster summon).
- A spell with a projectile on any effect (a rune, a trap, a lobbed shot). The engine launches those only from the player's crosshair.
- Self, Touch, Aimed or Target Actor delivery.
- A concentration or constant-effect spell.
- A disease, ability or addiction spell type.
- Any other cast flag set alongside this one.
- A point in a cell that is not loaded and attached, or not in the actor's worldspace.
- A cast facet that is owned: a live cast claim on the actor that would outrank a driving request at this basis, a floor included. This is checked at the call. Your own driving claim at the same basis blocks your own position cast. Release it or ask at a higher basis.
- More than 16 markers alive at once.
- A `Repoint` carrying the bit on a live cast claim (nothing changes).
- Everything, when `[PositionCast] bPositionCast=0` (the default), on VR, or on an unsupported runtime.
- `RequestCast` with the bit. `APMF_CastRequest` has no point.

At the call (`kInvalidHandle`): the flag, the point, a non-finite basis and the facet check. On the game thread (a live handle is not a promise): the spell checks, the actor and cell checks, and the facet check again.

**Lifetime.** It is a one-shot, not a claim. It never enters the control map and holds no facet. The handle labels log lines and nothing else. `IsClaimLive` is false for it and `Release` and `Repoint` do nothing. You learn the outcome from `APMF.log` (`queued`, then `DELIVERED` or `REFUSED`). Every marker Harbinger places is recorded in the co-save (record `XMRK`). After a load the markers the loaded world still holds are deleted, each only after three proofs: the recorded FormID resolves, its base is XMarker, and it stands within 1 unit of the recorded spot.

**Priority and conflicts.** None, since it holds no facet. It yields to an owner of the cast facet.

**Examples.** Pick a clear spot with `FindEmptySpace`, then drop the spell there.

```cpp
bool DropSpellAhead(RE::FormID caster, RE::FormID spell) {
    if (g_apmf->abiVersion < 11) return false;

    APMF_SpaceQuery q{};
    q.size      = sizeof(q);
    q.origin    = caster;
    q.distance  = 400.0f;
    q.clearance = 96.0f;
    APMF_SpaceResult r{};
    r.size = sizeof(r);
    if (g_apmf->FindEmptySpace(&q, &r) != kQuery_Ok) return false;    // main thread only

    APMF_Param p{};
    p.form = spell;
    p.ival = kCastFlag_AtPosition;
    p.posX = r.x; p.posY = r.y; p.posZ = r.z;
    return g_apmf->RequestEx(caster, kIntent_Cast, kBasis, &p) != kInvalidHandle;
}
```

**Version and INI.** ABI 11 (0.9.8). `bPositionCast` in `[PositionCast]`, default 0. Built, CI verified, not battle-tested. `TODO(verify)`: no field run of the position cast is recorded in the project docs.

### ch.14 Shout and power (`kIntent_ShoutPower`)

**What it is for.** Record that a mod owns the shout or power selection on an actor, so Harbinger is the single arbiter of who does. Today it is **arbitration only**. Harbinger makes no equip write and denies nothing.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = shoutOrPowerFormID;                                  // recorded for observability and a future gate
Handle h = g_apmf->RequestEx(actor, kIntent_ShoutPower, kBasis, &p);
```

**What Harbinger enforces.** Nothing beyond recording the owner. You select the shout yourself with your own call (`ActorEquipManager::EquipShout`). The channel used to make that call itself and was converted to arbitration only. A future gate may deny a competing framework's own shout write.

**Lifetime.** Ended by `Release`. Nothing to undo.

**Priority and conflicts.** `Core`. Basis between clients.

**Examples.**

```cpp
Handle OwnShout(RE::FormID actor, RE::FormID shout) {
    APMF_Param p{};
    p.form = shout;
    return g_apmf->RequestEx(actor, kIntent_ShoutPower, kBasis, &p);
}
```

**Version and INI.** ABI 1. No key. Built, not yet battle-tested.

### ch.25 Cast guard (`kIntent_CastGuard`)

**What it is for.** Put a rule on an NPC's own casts. Two rules exist, and one claim can carry either or both. The player sees a follower that does not fire a fireball through the player and that does not start streams the client has not allowed. Harbinger installs no hook for it and drives nothing. It says no at the cast seats it already owns.

**How to claim it.** ABI 21, `RequestEx`.

```cpp
APMF_Param p{};
p.ival = kCastGuard_LineOfFire | kCastGuard_ChannelRight | kCastGuard_ChannelLeft;
p.fval = 6.0f;                                                // the window in SECONDS
Handle h = g_apmf->RequestEx(actor, kIntent_CastGuard, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `ival` | `CastGuardFlags`. At least one known bit, no unknown bit |
| `fval` | the window in seconds. 0 means 4 seconds. Clamped to 15. Negative or non-finite is refused |
| `form`, `target`, `pos` | not read |

| Flag | Value | Guard |
|---|---|---|
| `kCastGuard_LineOfFire` | 1 | hold an offensive cast while a friend is in the way |
| `kCastGuard_ChannelRight` | 2 | refuse the AI's own concentration cast on the right hand |
| `kCastGuard_ChannelLeft` | 4 | refuse the AI's own concentration cast on the left hand |
| `kCastGuard_All` | 7 | all three |

**What Harbinger enforces.**

*Line of fire.* An offensive cast (any effect with a hostile magic effect, not Self delivery, whether spell, scroll or staff) is held while a friendly stands in its line of fire. Friendly means any actor not hostile to the caster (the engine's own `IsHostileToActor`, asked from that actor). For a caster that is a player teammate, the player and every teammate count too. A friend is in the line of fire when:

- it is within **80 units** of the line from the caster to the aim and not read OCCLUDED from the caster by Harbinger's own ray (a wall between them means the bolt cannot reach it, and an unknown reading still counts the friend), or
- it is within the spell's **blast area plus 64 units** of the aim.

The blast area is the spell's largest effect area. The game stores it in **feet**. Harbinger converts it at **64 over 3 units per foot** (Fireball, area 15, is 320 units). Friends up to 4096 units from the aim are considered. The aim is the target of the actor's live driving `kIntent_Cast` claim when that claim's spell is itself offensive, else the actor's combat target. No aim means no line, so nothing is held.

Harbinger measures every guarded caster on the main thread every frame (at most 8 per frame, round robin), from fresh positions. The seats read the newest measurement lock-free and trust it for 250 ms.

- At `CheckStartCast` (0x06, the AI's decision to start): the answer is no unless a measurement at most 250 ms old says the line is clear. This holds claim-driven casts too, because the guard runs before the claim answers yes.
- At `CheckCast` (0x0A on the hand casters, hard): refused (`kMultipleCast`) when a measurement at most 250 ms old says blocked. The engine consults it at cast start, at release of a charged cast and on every tick of a concentration channel. A refusal at a release or a tick interrupts the cast. An unknown measurement never refuses here.
- There is no decay. Harbinger holds as long as a friend is in the line. A client that wants a decay drops the bit with a `Repoint` for a moment.

*Channel bound.* On a bounded hand the AI's own concentration spell is refused at 0x0A, at its start and on every channel tick, **unless** a live `kIntent_Cast` claim on that hand names that spell (its spell or its proxy, and a dual-cast claim names both hands). The AI can stream a concentration spell only inside the client's own claim. The instant caster and the voice caster are never touched. A client's direct stream keeps running.

It chooses no spell, aims nothing, equips nothing and fires nothing. A self-delivered area spell (Fire Storm, a cloak) is never guarded, because only a spell fired at an aim has a line of fire. It cannot reach a cast already past its release, such as a projectile in flight. Healing and other friendly spells are never held.

**Lifetime.** Bounded exactly like a cast claim. It dies at the window unless a `Repoint` renews it. A `Repoint` renews the window granted at request, and its `fval` is not re-read. Its `ival` is read, so a heartbeat can change the guards. A `Repoint` that would be refused (an unknown bit, no bit, line of fire while the measurement is not armed) is refused by name and the claim keeps its previous bits. A load releases it.

**Refused at the call** (`kInvalidHandle`, logged `[apmf][cast-guard] claim refused`): an APMF older than ABI 21 ("no channel serves intent 25"), the `CheckCast` gate not installed, no param, no guard bit or an unknown bit, a negative or non-finite `fval`, the player as the actor, and `kCastGuard_LineOfFire` while the measurement is not armed.

**Priority and conflicts.** `Cast` sources facet. The source a guard overrules is the engine's own AI. A refusal stands only while the guard claim's client outranks `Engine` there. Defaults are client 50 against engine 20. A would-be refusal that does not stand is logged "would refuse, ALLOWED". Among guard claims on one actor, the highest basis wins, a tie keeps the earliest, and **the winner's bits apply alone**. Bits are not merged across claims.

**Examples.** Hold one guard per follower and heartbeat it from your service tick. This is the pattern in MFO's design for its friendly-fire hold.

```cpp
void KeepGuard(RE::FormID follower, bool boundConcentration, Handle& h) {
    if (g_apmf->abiVersion < 21) return;

    APMF_Param p{};
    p.ival = kCastGuard_LineOfFire;
    if (boundConcentration) p.ival |= kCastGuard_ChannelRight | kCastGuard_ChannelLeft;
    p.fval = 6.0f;                                              // seconds. Repoint renews it

    if (h == kInvalidHandle) h = g_apmf->RequestEx(follower, kIntent_CastGuard, kBasis, &p);
    else                     g_apmf->Repoint(h, &p);
}

void DropGuard(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }
}
```

**Version and INI.** ABI 21 (0.12.0). No key of its own. It rides the cast gate and, for the ray, `bOwnLineOfSight` in `[Sightline]`. Built and CI verified, not yet field-run. `TODO(verify)`: the project docs describe this recipe as MFO's, but the current MFO source checkout has no `kIntent_CastGuard` caller.

## Equipment

### ch.15 Equipment (`kIntent_Equipment`)

**What it is for.** Gate the actor's spell and staff re-arm. It is the old "melee versus ranged" lever. A client that already equips and holds a weapon itself claims this with that weapon's FormID, and from then on the actor's AI cannot swap a spell or staff into its hand. The player sees a follower that keeps its weapon out.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = weaponFormID;                       // gate-only form: engages the channel, makes no engine write
Handle h = g_apmf->RequestEx(actor, kIntent_Equipment, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `form` | optional. Set: gate-only. Zero: the **probe path**, see the warning below |
| `ival` | reserved for a future hand slot |

**Warning, the probe path.** A claim with `form == 0` is the old test path. Harbinger actively unequips the actor's right-hand weapon and re-equips it on release. That changes persisted inventory state and the re-equip pointer is not saved, so it is not save-safe. Do not hold a no-param ch.15 claim across a save. Do not use the no-param form in a shipped mod. Always pass a form.

**What Harbinger enforces.** With a form set, the channel itself writes nothing to the engine. The enforcement lives in the `CheckShouldEquip` gate (the same one ch.8 and ch.8b ride). While the claim stands, the gate denies any spell or staff re-arm for the actor. Your own equip call is unaffected, and Harbinger never fights you over which weapon is in hand.

**Lifetime.** Standing, ended by `Release`. Never hold it on the same actor as a ch.17 claim. On a ch.17 actor the probe path's re-equip is an equip from outside the engine and is refused unless the weapon is in the declared set. The gate-only form is not affected.

**Priority and conflicts.** `Equip` sources facet. Equip writers are traced by a stack scan and a deny stands only against a positively identified source that is outranked. An equip Harbinger cannot attribute is never denied.

**Examples.**

```cpp
Handle KeepWeaponOut(RE::FormID actor, RE::FormID weapon) {
    APMF_Param p{};
    p.form = weapon;                          // never leave this zero in a shipped mod
    return g_apmf->RequestEx(actor, kIntent_Equipment, kBasis, &p);
}
```

**Version and INI.** ABI 1. No key of its own. `EnableEquipDenyComplete` in `[EquipGate]` affects the gate it rides. Built, not yet battle-tested.

### ch.17 Equip authority (`kIntent_EquipAuthority`)

**What it is for.** Take the whole engine-equip facet for an actor. You declare what the actor wears. Harbinger equips that set and refuses every other engine equip of armor, weapons, ammo and torches until you re-declare or release. Outfit re-apply, the AI's weapon and armor choice, combat re-arm and the engine's re-equip after a `RemoveItem` all stop reaching the actor. The player sees a follower in exactly the gear the mod chose, and the game cannot strip or replace it. The player is never subject to it.

**How to claim it.** Three calls work together. Gate everything on `abiVersion`.

```cpp
APMF_Param p{};
p.ival = kEquipAuth_None;                                     // an EquipAuthFlags word
Handle h = g_apmf->RequestEx(actor, kIntent_EquipAuthority, kBasis, &p);   // ABI 7

void SetEquipSet  (Handle, const RE::FormID* forms,           std::uint32_t count);  // ABI 7
void SetEquipSetEx(Handle, const APMF_EquipEntry* entries,    std::uint32_t count);  // ABI 8, a hand per item
void SetEquipScope(Handle, const APMF_EquipScope* scope);                            // ABI 9
bool IsEquipAuthorityEnforced();                                                     // ABI 8
```

The claim alone changes nothing. Harbinger invents no set, so an actor with a claim and no declaration passes every equip through.

`EquipAuthFlags` (in `param.ival`):

| Flag | Value | Meaning |
|---|---|---|
| `kEquipAuth_None` | 0 | defaults |
| `kEquipAuth_DenyUnequip` | 1 | reserved. Accepted and ignored, logged. Unequips are never refused |
| `kEquipAuth_DenyScript` | 2 | also refuse Papyrus `EquipItem` and console equips of off-set items. Default off |
| `kEquipAuth_ObserveOnly` | 4 | log every verdict as `would-deny` and deny nothing |
| `kEquipAuth_DenyPlayerMenu` | 8 | ABI 8. Also refuse the player's own equips through the trade or gift menu. Default off. A v7 Harbinger ignores the bit |

`SetEquipSet` declares bare base FormIDs. `forms` is copied inside the call. `count` is clamped to `kMaxEquipSet` (32). Items past that are treated as off-set and denied. `count == 0` or `forms == nullptr` clears the declaration and the claim passes everything through again, without releasing.

`SetEquipSetEx` carries one `APMF_EquipEntry` per item.

```cpp
struct APMF_EquipEntry {
    RE::FormID   form;        // the item's BASE FormID
    std::uint8_t slot;        // EquipSlot: 0 default, 1 right hand, 2 left hand
    std::uint8_t reserved[3]; // must be 0
};
```

`kEquipSlot_Default` lets the engine pick. A slot-less one-hander always lands in the right hand and displaces what is there, so a dagger meant for the off hand would evict the sword. Use `kEquipSlot_Right` and `kEquipSlot_Left` for hand-held items only (a one-hand weapon, a shield, a torch). Declare a two-hander, a bow, ammo and body armor with `kEquipSlot_Default`. The same form may appear twice, once per hand, for two identical daggers. Harbinger counts the instances it needs against the inventory and skips, with a log line, any it does not own enough of. An unknown slot value is treated as default and logged once.

`SetEquipScope` narrows the authority to categories.

```cpp
struct APMF_EquipScope {
    std::uint32_t owned;      // categories your declared set is authoritative for
    std::uint32_t denied;     // categories the actor must not equip into at all
    std::uint32_t reserved[2];// must be 0
};
```

`EquipCategory` bits: `kEquipCat_Armor` (1, every ARMO without the shield bit), `kEquipCat_Shield` (2, competes for Left too), `kEquipCat_Right` (4), `kEquipCat_Left` (8), `kEquipCat_Ammo` (16), `kEquipCat_Light` (32, competes for Left too), and `kEquipCat_All` (0x3F). The default scope is `{ owned = kEquipCat_All, denied = 0 }`, which is the whole facet. A two-hander, a bow or a crossbow competes for both hands. A one-hander with an either-hand slot, declared as default, also competes for both hands, so declare its hand when you own only one. Unknown bits are masked off, logged once per handle, never refused. `scope == nullptr` resets to the default.

**What Harbinger enforces.** When the declaring claim owns the facet:

1. The set is published to the lock-free snapshot the equip seat reads. From the next engine equip on, an item not in the set is refused for this actor (within the categories you own, path-classified and logged).
2. One main-thread pass equips each declared item the actor owns and is not wearing, through the engine's own `EquipObject` (queued, sounds on). It never unequips anything. The engine's own worker displaces whatever occupies a declared item's slot. No re-assert loop follows. The seat is what stops the engine undoing the set.

The seat's verdict for an equip on a claimed actor, in order:

| Case | Verdict |
|---|---|
| script or console equip, without `DenyScript` | allow (exempt) |
| the player's own menu equip, without `DenyPlayerMenu` | allow (player agency) |
| competes for a denied category | deny, even if in the set |
| competes for an owned category and there is no declaration or the item is in the set | allow |
| competes for an owned category and the item is off-set | deny |
| competes for no owned and no denied category | allow |

Only ARMO, WEAP, AMMO and LIGH equips are governed. A potion, food, a scroll, an ingredient or a book passes untouched, so the AI's own potion use is never refused. Do not declare one. The equip pass skips it and logs why. The deny half compares FormIDs only, so a declared item is in the set whichever hand the engine tries. The hand is what Harbinger's own equip pass enforces.

Rules of the road:

- **Declare, do not tick.** Call it when the loadout changes. Each declaration walks the inventory. An item Harbinger queued is held for 3 seconds before it is queued again.
- **The set must be wearable at once.** A two-hander and a shield make the engine displace one with the other every pass. Harbinger never picks a winner.
- **Re-declare with a removal.** If you remove or unequip a declared weapon, the engine's replacement is off-set and refused. The actor stays bare until you declare again.
- **An unloaded actor stores the declaration.** The seat denies for it, but the equip pass is skipped and logged. Declare again after it loads.
- **Bound weapons are equips.** A conjured bound weapon equips through the seat and is refused unless its form is in the set.
- **Do not mix ch.15's probe path with a ch.17 claim.**
- **A refused equip is not re-issued.** If the hand block refused a declared weapon because a cast claim held that hand, Harbinger does not equip it again when the hand frees. Declare again after the cast claim releases (backlog item APMF-B64).
- **Harbinger never adds items.** A declared item the actor does not own is skipped and logged.

**Refusal and the degrade.** The claim is refused at the call (`kInvalidHandle`) while the equip seat is not installed: `bEquipAuthority=0`, VR, an unsupported runtime, a site-verify refusal, or before `kDataLoaded`. A refused claim means keep your own equips. `IsEquipAuthorityEnforced()` separates "Harbinger holds the set" from "Harbinger is only watching". It is true only when the seat is installed and `bEquipObserveOnly` is 0. A claim's own `kEquipAuth_ObserveOnly` bit is not consulted by it.

**Lifetime.** Standing, no time limit. It ends on `Release`, a higher basis, a load or an unload. Release clears the declaration and nothing is unequipped. A declaration on a claim that does not own the facet is stored and takes effect the moment that claim wins. A win is a declaration event and runs the equip pass.

**Priority and conflicts.** `Equip` sources facet. The caller of an engine equip is recovered by a stack scan. A deny stands only against a positively identified source that your priority outranks. An equip Harbinger cannot attribute is never denied. Armor verdicts are enforced by default (`bEquipEnforceArmor=1`) even while `bEquipObserveOnly=1`. Weapons, shields, torches and ammo stay observe-only until you set `bEquipObserveOnly=0`. This is the only facet with a call-site seat. It patches the two internal call sites of the engine's equip worker, byte-verifies both before writing either, and refuses to install if any other mod touched those bytes, so a collision is a missing feature and never a crash.

While a cast claim holds a hand, `[HandBlock]` also closes that hand to weapons, shields and torches. This is on by default and independent of `bEquipObserveOnly`.

**Examples.**

Claim the authority, scope it to the hands and body, and declare a dual-wield loadout. This is the call sequence MFO uses.

```cpp
Handle ClaimEquip(RE::FormID actor) {
    if (g_apmf->abiVersion < 9) return kInvalidHandle;

    APMF_Param p{};
    p.ival = kEquipAuth_None;                         // scripts pass, unequips pass, the player menu passes
    Handle h = g_apmf->RequestEx(actor, kIntent_EquipAuthority, kBasis, &p);
    if (h == kInvalidHandle) return h;                // seat not installed: keep your own equips
    return h;
}

void DeclareLoadout(Handle h, RE::FormID sword, RE::FormID dagger, RE::FormID cuirass) {
    APMF_EquipScope scope{};
    scope.owned  = kEquipCat_Right | kEquipCat_Left | kEquipCat_Armor;   // leave ammo and torches to the engine
    scope.denied = 0;
    g_apmf->SetEquipScope(h, &scope);                 // send the scope before the set

    APMF_EquipEntry set[3] = {
        { sword,   kEquipSlot_Right,   { 0, 0, 0 } },
        { dagger,  kEquipSlot_Left,    { 0, 0, 0 } },
        { cuirass, kEquipSlot_Default, { 0, 0, 0 } },
    };
    g_apmf->SetEquipSetEx(h, set, 3);
}

void DropAuthority(Handle& h) {
    if (h == kInvalidHandle) return;
    g_apmf->SetEquipSetEx(h, nullptr, 0);             // an empty declaration clears it
    g_apmf->Release(h);                               // nothing is unequipped
    h = kInvalidHandle;
}
```

Find out whether the engine is actually refusing, or only watching.

```cpp
bool HarbingerEnforces() {
    return g_apmf->abiVersion >= 8 && g_apmf->IsEquipAuthorityEnforced();
}
```

**Version and INI.** ABI 7 (first shipped in 0.9.5), 8 adds a hand per item, `kEquipAuth_DenyPlayerMenu` and `IsEquipAuthorityEnforced`, 9 adds the scope. Keys in `[EquipAuthority]`: `bEquipAuthority` (1), `bEquipObserveOnly` (1), `bEquipEnforceArmor` (1), `bEquipDenyScript` (0). `bHandClaimBlocksEquip` in `[HandBlock]`. The seat ships observe-only for weapons until the probe criteria in [Appendix E](#appendix-e-equip-authority-probe-criteria) pass. Built, not yet battle-tested.

## Packages and travel

### ch.9 Offer package (`kIntent_OfferPackage`)

**What it is for.** Hand the actor one specific AI package, and let the engine run it natively. This is how a mod holds a travel package or a work package on a follower even when another framework offers its own. The player sees the follower do what the package says. Harbinger does not run the package. The client owns the package and whatever its target resolves to.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = packageFormID;                      // the TESPackage to offer
Handle h = g_apmf->RequestEx(actor, kIntent_OfferPackage, kBasis, &p);
```

**What Harbinger enforces.** It hooks the engine's package question (`Character` slot 0x49, `CheckForCurrentAliasPackage`). While your claim owns the facet, the answer is your package instead of the framework's own. The engine adopts it and runs it with real pathing, one `OnPackageChange` each way. If the FormID cannot be resolved, Harbinger falls back to the engine's own answer, never null. It does not pick, validate or interpret the package's content.

A nudge makes the redirect take within one evaluation. Harbinger calls `EvaluatePackage` once at engage and owner change, and once at release so the framework package resumes at once. The nudge is posted one hop past the point the claim publishes, because a nudge issued earlier re-evaluates against the old snapshot. It is not self-sustaining. If nothing nudges and the engine does not ask, nothing re-asks.

**Spectator hold-off.** While the claim wins and outranks `Engine` on the `Package` facet, the engine's runtime-created Spectator package (an out-of-combat NPC watching a fight) is not created, so a retreating follower keeps its package. The seat is `iSpectatorSeat` in `[PackageGate]` (2 on, 1 log only, 0 off). Built, not yet tested in game.

**Lifetime.** Standing, ended by `Release`. A load drops it. `TODO(verify)`: the project's own notes say save and load persistence of an engaged claim is unexercised.

**Priority and conflicts.** `Package` sources facet. Package and scene sources are **observe only**. Harbinger captures the package that runs instead of yours, names its source (the scene's plugin, else the package's plugin, else `Engine` for a runtime package), compares priorities in the log and denies nothing. The only enforcement on this facet is the Spectator hold-off. Between your own package-offer claims, basis decides. ch.19 Travel files an internal ch.9 claim at your basis, so an outbidding ch.9 claim on the same actor takes the package from your travel leg.

**Examples.**

```cpp
Handle HoldPackage(RE::FormID actor, RE::FormID package) {
    APMF_Param p{};
    p.form = package;
    return g_apmf->RequestEx(actor, kIntent_OfferPackage, kBasis, &p);
}
```

**Version and INI.** ABI 1. `EnableRedirectLog` and `iSpectatorSeat` in `[PackageGate]`. Proven when nudged: 16 of 16 deck wins, each after an explicit nudge. The first graduated field run engaged 0 of 6 because the nudge fired before the claim published, which is fixed.

### ch.19 Travel (`kIntent_Travel`)

**What it is for.** Walk an actor to a place. The player sees the follower walk to a crate, an enemy, a marker, a cell or a world point under its own travel package, and stop when it gets there, when it enters combat, or when the destination is gone. MFO uses it for loot excursions and deposit trips.

The whole contract, in the project's words: it goes to the target 50 to 100 units from it, and combat interrupts and cancels the movement. That is all.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = destinationFormID;     // a REFERENCE or a CELL. Required unless kTravel_ToPosition
p.fval = 0.0f;                  // arrival radius, game units. 0 means 75
p.ival = kTravel_ReleaseOnTargetDead;                      // a TravelFlags word
Handle h = g_apmf->RequestEx(actor, kIntent_Travel, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `form` | the destination's FormID. An object reference (REFR or ACHR: an actor, an XMarker, a container, anything) or a **cell**. The record type decides which, no flag. It need not be loaded or nearby. Anything else is refused. 0 is refused unless `kTravel_ToPosition` is set |
| `fval` | arrival radius in game units. 0 means 75. Clamped to 50 to 512, logged. Not used for a cell. Also written into the package's own stop radius |
| `ival` | `TravelFlags`, below |
| `posX`, `posY`, `posZ` | the destination point, read only with `kTravel_ToPosition` (then `form` must be 0). A non-zero point without the flag is refused by name |

`TravelFlags`:

| Flag | Bit | Meaning |
|---|---|---|
| `kTravel_ReleaseOnTargetDead` | 0 | names the default. Does not switch anything on. The leg ends if a destination that was alive when targeted dies on the way, set or not. A destination already dead when targeted (a corpse) is a valid place and never ends the leg by death |
| `kTravel_ToPosition` | 1 | ABI 11. The destination is the world point. Harbinger places its own non-persistent XMarker there and walks to it. Needs a runtime where marker placement is verified. A non-finite point is refused |
| `kTravel_SpeedSet` | 2 | ABI 12. Use the speed in bits 3 and 4 for this leg. Without it the package's authored speed (Run) applies |
| `kTravel_SpeedWalk`, `Jog`, `Run`, `FastWalk` | bits 3 to 4 = 0, 1, 2, 3 | ABI 12. The engine's `PreferredSpeed` values. Mask with `kTravel_SpeedMask` |

An older Harbinger stores the gait bits and walks at its authored speed without a word. Gate on `abiVersion >= 12`. Without `kTravel_ToPosition` support (below ABI 11) a zero form is refused, so a point request fails loudly instead of walking somewhere else.

**What Harbinger enforces.** Harbinger ships eight travel package records in `Data/APMF.esl`. It points one at your destination and offers it through an internal ch.9 claim at **your basis**. The engine runs the package natively, with real pathing. The leg ends on exactly these:

- **Arrival**: inside the radius of a reference, or the actor's parent cell is the destination cell.
- **Combat**: the actor enters combat.
- **Destination gone**: deleted, disabled, died during travel, or a cell that no longer resolves.
- **Blocked** (ABI 12): the engine held the actor in its Movement Blocked package for 3 seconds of running game time.
- **Stuck**: a two-minute safety net.

It claims nothing else for you. No combat target, no casting, no equipment, no aggression, no movement block. It enters no combat, pins no target and fakes no perception. If you want one of those, claim that intent yourself. One facet per intent.

A gait change in a `Repoint` rewrites the package record, but the running package keeps the speed it started with until the package next starts. Harbinger logs that case. Release and request again to change gait mid-walk.

**A leg ends and your claim stands.** Harbinger never releases a ch.19 claim. When a leg ends it drops only its own package offer. The claim stays live (`IsClaimLive` stays true) and does nothing until you `Repoint` or `Release`. Read `GetTravelLegState` to learn why the actor stopped, so that an ended leg is never mistaken for a package theft.

**Why the walk stopped.**

```cpp
std::uint32_t GetTravelLegState(RE::FormID actor, APMF_TravelLegInfo* out);   // ABI 12, any thread
```

Set `out->size = sizeof(APMF_TravelLegInfo)` first. The call returns a `TravelLegState` and, when `out` is non-null and `size` covers the 72-byte v12 layout, fills it. It writes nothing into a shorter struct. It is a snapshot read and safe from any thread. The state is not saved. After a load an actor reads `kLeg_None` until a claim is made again.

| State | Meaning |
|---|---|
| `kLeg_None` (0) | no state held for this actor |
| `kLeg_Pending` (1) | accepted or re-pointed. The leg starts next frame |
| `kLeg_Walking` (2) | the package is offered and the actor is travelling |
| `kLeg_Arrived` (3) | inside the radius, or inside the cell |
| `kLeg_Blocked` (4) | the engine's Movement Blocked package held the actor 3 seconds. `stallX/Y/Z` say where. `blocker` and `blockerKind` say whether an actor stood in front |
| `kLeg_CombatCancelled` (5) | the actor entered combat |
| `kLeg_DestGone` (6) | the destination is gone |
| `kLeg_StuckTimeout` (7) | the two-minute net elapsed |
| `kLeg_ActorGone` (8) | the actor unloaded, died or lost its 3D |
| `kLeg_Failed` (9) | the leg could not start or be re-pointed. A refused `Repoint` ends the previous leg. `destForm` and `destX..Z` name the refused destination |
| `kLeg_Released` (10) | the claim was released |

`APMF_TravelLegInfo` fields: `size`, `state`, `actor`, `destForm` (0 for a point leg), `destX/Y/Z`, `msInState`, `seq` (one counter that never resets and changes on every state change, so compare for inequality), `stallX/Y/Z`, `blockedMs`, `speed` (0 to 3 = the gait written, or 0xFFFFFFFF = none), `reserved`, `ownerHandle` (the winning ch.19 claim, compare with yours), `blocker`, `blockerKind` (`kBlocker_None`, `Player`, `Teammate`, `Actor`). A static block (a closed gate) reads `kBlocker_None`: treat the route as closed until the world changes. An actor block will likely clear, so take another task and come back. A read right after a `Repoint` may still show the previous leg's end for a frame, so check `ownerHandle`, `destForm` and `seq`.

**What BLOCKED means.** The engine answers "this actor cannot move along its path" by running a runtime package of type 36, Movement Blocked. A closed lever portcullis produced it for 39 to 45 seconds in the field, while a healthy leg shows it for about a second and then arrives. So Harbinger ends the leg as BLOCKED once it has held for 3 seconds of running game time, checked every 250 ms. Time spent in a menu does not count, because each poll adds at most 500 ms. One poll without Movement Blocked inside a run is tolerated. Two in a row end the run. Harbinger does not fight or deny Movement Blocked. It records the end and lets you decide. At the verdict it looks for a live actor right in front of the stalled one: within 192 units on the ground plane, less than 128 units above or below, inside a 120 degree cone around the actor's facing or around the straight line to the destination. The nearest one is reported in `blocker`.

What to do with it (marth's loot rules, and good advice for any client):

- An actor block (`blockerKind` is not `kBlocker_None`): someone is standing in the way and will probably move. Reorder, do not drop. Take another task now and come back to this one later.
- A static block (`kBlocker_None`): a closed gate, a wall or clutter. The route is closed. Park the task until the world changes (a door or gate opens, `TESOpenCloseEvent`, a lever is used, `TESActivateEvent`, or a cell attaches). Do not use a timer to retry, and do not send the actor home while other reachable tasks remain.

A BLOCKED end also runs a passive gate probe that only writes log lines (`[travel-gate]`, and `[travel-gate2]` with `bGateProbe2=1`). Nothing reads it back and a client must not parse it.

**Why a point needs a marker.** A package's location holds a form pointer or a reference handle and nothing else. There is no coordinate storage in it, which was read off both game binaries. Vanilla's answer is to place an XMarker and point at the marker's reference. You can do that yourself and pass the marker as `form`, or set `kTravel_ToPosition` and Harbinger does it. A non-zero point without the flag is refused by name rather than walking the actor somewhere you did not ask for.

**Gait timing.** Harbinger writes the speed into the leg's package record, together with the record's "Preferred Speed" flag (0x2000), which the engine requires before it honours the byte at all. The engine copies the record's speed into the actor's running-package state when the package starts on the actor. A fresh leg therefore walks at the declared gait. `GetTravelLegState`'s `speed` field reports what was written. A `[travel-gait]` log line per gait leg shows the engine's running copy once the package runs.

**Refused at the call** (`kInvalidHandle`, nothing to clean up): VR, `[Travel] bTravel=0`, `Data/APMF.esl` missing or disabled, `param.form == 0` without `kTravel_ToPosition`, `param.pos` non-zero without the flag, and with the flag a non-zero form or a non-finite point or a runtime without marker support.

**Refused at engage, one drain later** (the handle is live and inert, the log says why, **release it**): `param.form` names a record that is neither an object reference nor a cell. This one cannot be synchronous, because deciding a form's record type needs a form lookup and the API does not take the engine's form table off the game thread.

Eight concurrent legs is a hard cap. A package's destination lives on the package record, so two actors walking to two places need two records. A ninth simultaneous leg is accepted at the call, because the slot is found when the leg starts on the game thread. The log then says `[travel-leg] ... REFUSED -- all 8 package slots are in use by other legs`, the leg state reads `kLeg_Failed`, and the claim stays live and inert until you release it.

**Lifetime.** Standing, no time limit. `Repoint(handle, &param)` moves the destination in place with no release and no re-claim. A point leg's marker lives exactly as long as the leg, and is deleted when the leg ends for any reason or on a `Repoint` to a new point. Markers are co-saved, so a save made mid-leg cannot leave one behind. A load releases every ch.19 claim. If a leg ended and you want the same place again, `Repoint` it, since the state read tells you it ended.

**Priority and conflicts.** `Package` sources facet. The internal ch.9 offer is filed at your basis and re-filed if the winning ch.19 claim's basis changes. If another client outbids you on ch.9 for the same actor, your leg loses the package, exactly as if you had claimed ch.9 yourself.

**Examples.**

Send a follower to a crate at a walking pace and find out why it stopped.

```cpp
struct Trip { Handle h = kInvalidHandle; RE::FormID dest = 0; std::uint32_t seqBefore = 0; };

bool StartTrip(RE::FormID actor, RE::FormID crate, Trip& t) {
    if (g_apmf->abiVersion < 12) return false;                 // gait and the state read need ABI 12

    APMF_TravelLegInfo pre{};
    pre.size = sizeof(pre);
    g_apmf->GetTravelLegState(actor, &pre);
    t.seqBefore = pre.seq;                                      // a state with this seq is an OLD leg's end

    APMF_Param p{};
    p.form = crate;
    p.fval = 100.0f;
    p.ival = kTravel_ReleaseOnTargetDead | kTravel_SpeedSet | kTravel_SpeedWalk;
    t.h = g_apmf->RequestEx(actor, kIntent_Travel, kBasis, &p);
    t.dest = crate;
    return t.h != kInvalidHandle;                               // 0: esl missing, bTravel=0, VR. Use your own path
}

// Poll from your service tick. Returns true when the leg is over.
bool TripOver(RE::FormID actor, const Trip& t, bool& arrived) {
    APMF_TravelLegInfo info{};
    info.size = sizeof(info);
    const std::uint32_t st = g_apmf->GetTravelLegState(actor, &info);
    const bool ours = info.destForm == t.dest && info.seq != t.seqBefore &&
                      (info.ownerHandle == t.h || info.ownerHandle == 0);
    if (!ours) return false;
    arrived = (st == kLeg_Arrived);
    return st >= kLeg_Arrived;                                  // Arrived, Blocked, Combat, DestGone, Stuck, Gone, Failed, Released
}
```

Walk to a world point you picked, and move the destination in place later.

```cpp
Handle WalkToPoint(RE::FormID actor, float x, float y, float z) {
    if (g_apmf->abiVersion < 11) return kInvalidHandle;
    APMF_Param p{};
    p.form = 0;                                                 // a point, not a form
    p.ival = kTravel_ToPosition;
    p.posX = x; p.posY = y; p.posZ = z;
    return g_apmf->RequestEx(actor, kIntent_Travel, kBasis, &p);
}

void MovePoint(Handle h, float x, float y, float z) {
    APMF_Param p{};
    p.ival = kTravel_ToPosition;
    p.posX = x; p.posY = y; p.posZ = z;
    g_apmf->Repoint(h, &p);                                     // a new marker is placed, the old one deleted
}
```

Hold a travel package to a loot target, and stop when you are done.

```cpp
Handle GoToLoot(RE::FormID actor, RE::FormID lootRef) {
    APMF_Param p{};
    p.form = lootRef;
    p.fval = 80.0f;
    return g_apmf->RequestEx(actor, kIntent_Travel, kBasis, &p);
}

void StopTrip(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }   // ends the claim and the leg
}
```

**Version and INI.** ABI 10 (the intent, with `Data/APMF.esl`), 11 adds `kTravel_ToPosition`, 12 adds the gait bits, the BLOCKED end and `GetTravelLegState`. First shipped in 0.9.5, 0.9.8 and 0.9.8. Keys in `[Travel]`: `bTravel` (1) and `bGateProbe2` (0, a passive log probe). Needs `APMF.esl` enabled in the load order.

## Combat

Seven facets cover combat, and each does one job. They are designed to be claimed together, each as its own handle, because a facet never claims another facet on your behalf.

| You want | Claim |
|---|---|
| The NPC to start fighting X | [ch.21 combat entry](#ch21-combat-entry-kintent_combatentry) |
| The NPC to fight X and not something else | ch.21 and [ch.20 target pin](#ch20-target-pin-kintent_targetpin) |
| The NPC out of combat for a while | [ch.22 re-entry deny](#ch22-combat-re-entry-deny-kintent_combatreentrydeny), plus one `StopCombat` from you |
| The NPC not to chase too far from you | [ch.23 pursuit leash](#ch23-pursuit-leash-kintent_pursuitleash) |
| The NPC to close on X while fighting | [ch.24 combat approach](#ch24-combat-approach-kintent_combatapproach) |
| The NPC not to attack or cast | [ch.7 combat action](#ch7-combat-action-kintent_combataction) |
| Record who owns the target | [ch.6 combat target](#ch6-combat-target-kintent_combattarget) |

The world's reaction to a fight is yours. Crime, bounty, faction and aggression changes, allies and guards joining, and combat music all happen the way the engine does them. Nothing here undoes them.

### ch.6 Combat target (`kIntent_CombatTarget`)

**What it is for.** Record that a mod owns the combat-target facet on an actor. It is **arbitration only**. Harbinger writes no target itself. The client writes the combat target with its own mechanism, and the claim makes Harbinger the single arbiter of who owns the facet. If you want Harbinger to hold the target for you, claim ch.20 instead.

**How to claim it.**

```cpp
APMF_Param p{};
p.form = targetActorFormID;                      // recorded for observability
Handle h = g_apmf->RequestEx(actor, kIntent_CombatTarget, kBasis, &p);
```

**What Harbinger enforces.** It records the owner. The engine's target-selector seat sits above every foreign hook on the combat target. When a positively identified foreign writer that your priority outranks changes the target, Harbinger puts the target back through the engine's own `SetTarget`. A segment of the hook chain it cannot prove is logged as `ATTRIBUTION DEFECT` and never enforced. `TODO(verify)`: the project docs describe this restore for "the target facet (ch.6 and ch.20)". It is not written down which target a ch.6 claim restores, since the claim's `form` is described as recorded for observability.

**Lifetime.** Standing. Ended by `Release`. Nothing to undo, because Harbinger wrote nothing.

**Priority and conflicts.** `Target` sources facet. `[UnknownSources] Target=override` makes an unnamed write rank at the Unattributed row. When the unnamed write came from the claiming mod's own plugin, it stands for that mod's claim.

**Examples.**

```cpp
Handle OwnTarget(RE::FormID actor, RE::FormID foe) {
    APMF_Param p{};
    p.form = foe;
    return g_apmf->RequestEx(actor, kIntent_CombatTarget, kBasis, &p);
}

void ChangeTarget(Handle h, RE::FormID newFoe) {
    APMF_Param p{};
    p.form = newFoe;
    g_apmf->Repoint(h, &p);                      // same handle, no release and re-claim
}
```

**Version and INI.** ABI 1. No key. Field-proven, in production use by MFO.

### ch.7 Combat action (`kIntent_CombatAction`)

**What it is for.** Deny named categories of combat behaviour. The player sees a follower that does not attack, or does not cast, while everything else about its combat (movement, blocking, dodging, searching) runs as normal.

**How to claim it.**

```cpp
APMF_Param p{};
p.ival = kCombatActionCat_Offense;                    // a CombatActionCategory bitmask
Handle h = g_apmf->RequestEx(actor, kIntent_CombatAction, kBasis, &p);
```

| `ival` bit | Name | Leaves denied |
|---|---|---|
| 0 | `kCombatActionCat_Offense` | Attack, AttackLow, Bash, RangedAttack, SpecialAttack, GroundAttack, FlyingAttack, CastImmediateSpell, CastConcentrationSpell, CastShout, PrepareDualCast |
| 1 | `kCombatActionCat_Cast` | CastImmediateSpell, CastConcentrationSpell, PrepareDualCast, CastShout. These four carry both bits |

An all-zero mask denies nothing. It is never a blanket lock. Defense, movement and utility leaves have no category bit and are never denied.

**What Harbinger enforces.** The claim denies exactly the combat behaviour-tree leaves whose classified category bit is set in the winning claim's mask. Harbinger replaces the denied leaf's `act()` and `pop()` as a pair, so the thread's data stack stays balanced, and the tree falls back cleanly the way it does when a leaf fails. Every other leaf runs natively. The channel itself writes nothing.

`kCombatActionCat_Cast` is the "no casting at all" tool. A [ch.8b](#ch8b-cast-kintent_cast) claim does not deny the cast leaves, because they are its delivery mechanism, so do not stack a Cast deny on an actor you also want to cast through ch.8b.

**Lifetime.** Standing, ended by `Release`, a load or an unload.

**Priority and conflicts.** `CombatAction` sources facet. A deny stops a leaf the engine's combat tree chose only while your priority is above `Engine`. Defaults are client 50 against engine 20. Raise `Engine=` in `[CombatAction]` above the client to let the engine win. It is arbitrated separately from ch.23: a ch.7 claim by another mod neither replaces nor is replaced by a leash.

**Examples.**

Stop a follower attacking and casting while everything else in its combat still runs.

```cpp
Handle HoldFire(RE::FormID actor) {
    APMF_Param p{};
    p.ival = kCombatActionCat_Offense;           // the cast leaves carry the Offense bit too, see the table
    return g_apmf->RequestEx(actor, kIntent_CombatAction, kBasis, &p);
}
```

The four cast leaves carry both bits, so `kCombatActionCat_Offense` also stops spell casts, and `kCombatActionCat_Cast` alone stops only the casts. No category denies the melee leaves alone.

**Version and INI.** ABI 1. No key. Field-proven (graduated from a live deck probe).

### ch.20 Target pin (`kIntent_TargetPin`)

**What it is for.** Hold the engine's combat target on the one you name. The player sees a follower that keeps fighting the foe the mod chose, with the engine's own attacks, movement, spells and equips. It never starts a fight.

**How to claim it.** ABI 13.

```cpp
APMF_Param p{};
p.form = targetActorFormID;                          // REQUIRED. The target actor
p.ival = kTargetPin_OwnLineOfSight;                  // optional, ABI 18. 0 = none
Handle h = g_apmf->RequestEx(actor, kIntent_TargetPin, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `form` | the target actor. Required. 0, the actor itself and the player as the actor are refused |
| `ival` | `TargetPinFlags`. `kTargetPin_OwnLineOfSight` (1), ABI 18 |

`fval`, `target` and `pos` are not read. This is a different intent from `kIntent_CombatTarget`, which writes nothing.

**What Harbinger enforces.** Each combat update the engine asks its target selectors (vtable slot 6) which foe the actor should fight. While the claim stands, Harbinger answers that question with your target, so the engine's own pick never reaches the actor. The engine then fights your target with its own AI.

Only among the engine's own targets. The answer is replaced only when the engine's selector picked a target (the actor is fighting) **and** your target is one of the actor's combat group's targets. Otherwise nothing is written and the log says so. A target becomes pinnable once something, the engine or your own ch.21 entry, makes it a combat target of the actor's group. The pin **pauses**, with the claim intact, while the engine has no target or your target is not yet in the group. It resumes when both hold again.

With `kTargetPin_OwnLineOfSight` the pin also pauses while Harbinger's own ray from the actor to the pinned target is not visible and at most 1000 ms old. A bow or staff then shoots at what the actor can see, not at a pinned foe behind a wall. It never ends the pin. The first seat call registers the pair, and until its first verdict (a frame or two) the pin pauses. Refused at the call while the line-of-sight service is not armed. An APMF older than ABI 18 ignores `ival` and pins with no test.

**Lifetime.** Standing, no time limit. Harbinger **releases the claim itself** when the target is lost (the engine can no longer locate it), dead, disabled, not loaded or no longer resolves, or the actor dies. The log names the reason (`pin ended: target lost`), `IsClaimLive` turns false and the handle is dead. Pin again if you want to keep chasing. This is the only case where Harbinger releases a ch.20 claim. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

Refused at the call (`kInvalidHandle`): an APMF older than 13, VR, an unsupported runtime, `[TargetPin] bTargetPin=0`, a failed address self-check, before `kDataLoaded`, `form` 0 or equal to the actor, the player as the actor, and `kTargetPin_OwnLineOfSight` while the line-of-sight service is not armed. Refused at engage (the handle is live and inert, release it): `form` is not an Actor. A `Repoint` naming a non-actor, 0 or the actor itself leaves the claim live and inert, logged.

**Priority and conflicts.** `Target` sources facet. Another mod that writes the target after the engine's update (its own `UpdateCombat` hook) writes last and wins, which shows as `OVERWRITTEN` in the log. If your client has its own target hook, make it stand down for any actor you have pinned. Harbinger also logs `SOURCE SEAT MISSED` if the actor ends an update aimed elsewhere with the selector never asked.

**Examples.** See the combined entry and pin recipe under ch.21.

```cpp
Handle PinTarget(RE::FormID actor, RE::FormID foe) {
    if (g_apmf->abiVersion < 13) return kInvalidHandle;
    APMF_Param p{};
    p.form = foe;
    p.ival = (g_apmf->abiVersion >= 18) ? kTargetPin_OwnLineOfSight : kTargetPin_None;
    return g_apmf->RequestEx(actor, kIntent_TargetPin, kBasis, &p);
}

void RePin(Handle h, RE::FormID newFoe) {
    APMF_Param p{};
    p.form = newFoe;
    p.ival = kTargetPin_None;
    g_apmf->Repoint(h, &p);                      // moves the pin in place
}
```

**Version and INI.** ABI 13 (0.9.9), line-of-sight flag 18. `bTargetPin` in `[TargetPin]`. Built and CI verified. Nobody has watched the selector seat run in game yet. The first field log must show `FIRST SOURCE DENY` and no `SOURCE SEAT MISSED` before anything relies on it.

### ch.21 Combat entry (`kIntent_CombatEntry`)

**What it is for.** Start a fight. Harbinger calls the engine's own `Actor::StartCombat(target)` once. The player sees the NPC turn on a foe it had not noticed.

**How to claim it.** ABI 14.

```cpp
APMF_Param p{};
p.form = targetActorFormID;                          // REQUIRED. The target actor. The player may be the target
Handle h = g_apmf->RequestEx(actor, kIntent_CombatEntry, kBasis, &p);
```

Only `form` is read. 0, the actor itself and the player as the actor are refused.

**What Harbinger enforces.** On the game thread, one frame after the claim is applied, Harbinger calls `StartCombat(target)` once. If the actor was not in combat, the engine builds its combat controller and group with your target as the group's first combat target, if it accepts it. If the actor was already in combat, the engine adds your target to the group's targets and keeps its current target. Everything after that is the engine's own AI.

It does **not** choose whom the actor fights first. The engine's own selector does, among its group's targets. To make the actor fight your target, also claim ch.20 with the same target. It is **one call per declaration**. Harbinger does not re-enter when the engine ends combat, and does not keep the fight going. A `Repoint` of a live claim, or a new winning claim, makes one more call for its target. **Release stops nothing.** The fight is the engine's state, and you call `StopCombat` yourself if you want it over.

**Lifetime.** Harbinger releases the claim itself, and `IsClaimLive` turns false with the reason logged, when:

- the engine refused the entry (`StartCombat` returned false: a restrained, unconscious or dead actor, a dead target, the engine's distance test, its identity test against one engine-global actor, an engine flag),
- the entry could not be attempted (the target is not an Actor, or the actor is not loaded, has no AI process or is dead),
- the fight ended (the entry succeeded and the actor now has no combat controller, "combat ended"), or
- the target is dead, disabled, not loaded or unresolvable, or the actor dies.

After "combat ended" or a refusal, another claim on the actor naming the same target ends with the same reason. To try again, send a new `RequestEx`. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

Refused at the call: an APMF older than 14, VR, an unsupported runtime, `[CombatEntry] bCombatEntry=0`, a failed `StartCombat` self-check, before `kDataLoaded`, `form` 0 or equal to the actor, the player as the actor. A target that is not an Actor is found a frame later and ends the claim.

**Priority and conflicts.** `Combat` sources facet. Your own entry is a declared decision and passes a ch.22 deny.

**Examples.** Enter combat and pin the same target, watching both handles. A handle that has never read live is pending, not ended.

```cpp
struct Fight {
    Handle entry = kInvalidHandle, pin = kInvalidHandle;
    bool   entrySeen = false, pinSeen = false;     // has IsClaimLive ever read true for it?
};

bool Ended(Handle h, bool& seen) {
    if (h == kInvalidHandle) return false;
    const bool live = g_apmf->IsClaimLive(h);
    if (live) seen = true;
    return seen && !live;                          // it WAS live and is not any more
}

bool StartFight(RE::FormID actor, RE::FormID foe, Fight& f) {
    if (g_apmf->abiVersion < 14) return false;     // start it your own way
    APMF_Param p{};
    p.form = foe;                                  // required for both: the target actor
    f.entry = g_apmf->RequestEx(actor, kIntent_CombatEntry, kBasis, &p);
    if (f.entry == kInvalidHandle) return false;   // refused at the call: the log says why
    f.pin   = g_apmf->RequestEx(actor, kIntent_TargetPin, kBasis, &p);
    return true;
}

void StopFight(Fight& f) {
    if (f.pin   != kInvalidHandle) g_apmf->Release(f.pin);     // the engine picks its own targets again
    if (f.entry != kInvalidHandle) g_apmf->Release(f.entry);   // stops nothing. The engine ends the fight
    f = {};
}
```

**Version and INI.** ABI 14 (0.9.9). `bCombatEntry` in `[CombatEntry]`. Built and CI verified.

### ch.22 Combat re-entry deny (`kIntent_CombatReentryDeny`)

**What it is for.** Keep an NPC out of combat for a bounded window. The player sees a retreating follower that the engine cannot pull straight back into the fight.

**How to claim it.** ABI 15.

```cpp
APMF_Param p{};
p.fval = 20.0f;                                      // the window in SECONDS
Handle h = g_apmf->RequestEx(actor, kIntent_CombatReentryDeny, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `fval` | the window in seconds. 0 means 10. Above 120 is clamped to 120 and logged. Negative or NaN is refused |

`form`, `ival`, `target` and `pos` are not read.

**What Harbinger enforces.** Every way the engine puts an actor into combat (it sees an enemy, it is attacked, an ally is fighting, a script or another mod calls `StartCombat`) goes through one engine function, `Actor::StartCombat`. While the claim holds and its window runs, Harbinger makes that function refuse for this actor, every time, before it does anything (no weapon draw, no equip, no alarm). The actor keeps doing what its package says.

**It stops nothing.** An actor already in combat is not taken out of it. The recipe for a retreat is: claim this, wait for the claim to read live, then call `Actor::StopCombat` once yourself. From then on the engine cannot pull the actor back in until the window ends. Other actors may still attack it. Nothing is undone when the claim ends, because nothing was written. Your own ch.21 entry is a declared decision and is **not denied**.

**Lifetime.** The claim ends by itself and Harbinger releases it: "window elapsed", or "owner dead". The window runs from the claim's own request (or last `Repoint`). A claim that takes over from a released rival gets only what is left of its window. `Repoint(handle, &param)` restarts the window from that moment with the new `fval`. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

Refused at the call: an APMF older than 15, VR, an unsupported runtime, `[CombatReentryDeny] bCombatReentryDeny=0`, a failed self-check, before `kDataLoaded`, a negative or NaN `fval`, the player as the actor.

**Priority and conflicts.** `Combat` sources facet. The deny refuses a `StartCombat` only while your priority outranks the source that called it (the engine, or the calling mod's DLL). A vanilla Papyrus `Actor.StartCombat` or a console command runs inside the game and is named `Engine` in this release. Raise `Engine=` in `[Combat]` above the client to let the engine win. A `StartCombat` whose caller cannot be named during a deny is allowed by default. `[UnknownSources] Combat=override` makes it rank at the Unattributed row.

**Examples.** Retreat: deny re-entry first, then `StopCombat` once, on your own tick.

```cpp
struct Retreat { Handle h = kInvalidHandle; bool live = false; int pendingTicks = 0; };

void StartRetreat(RE::FormID actor, float seconds, Retreat& r) {
    if (g_apmf->abiVersion < 15) return;
    APMF_Param p{};
    p.fval = seconds;
    r.h = g_apmf->RequestEx(actor, kIntent_CombatReentryDeny, kBasis, &p);   // PENDING until the next drain
    r.live = false;
    r.pendingTicks = 0;
}

// Your own per-tick update, on the MAIN thread. Returns true when the retreat has ended.
template <class StopCombatFn>
bool TickRetreat(Retreat& r, StopCombatFn stopCombatOnce) {
    if (r.h == kInvalidHandle) return true;
    const bool liveNow = g_apmf->IsClaimLive(r.h);
    if (liveNow && !r.live) { r.live = true; stopCombatOnce(); }             // the deny is applied: end the fight ONCE
    if (!liveNow && r.live) return true;                                     // window elapsed or owner dead
    if (!r.live && ++r.pendingTicks > 120) { g_apmf->Release(r.h); return true; }   // never applied: give up
    return false;
}
```

Where the NPC goes after `StopCombat` is not decided here. If foes keep fighting nearby, the engine hands an out-of-combat NPC its own Spectator package and it stands and watches. To send it somewhere, claim [ch.9](#ch9-offer-package-kintent_offerpackage) with your own package before `StopCombat`.

**Version and INI.** ABI 15 (0.9.10). `bCombatReentryDeny` in `[CombatReentryDeny]`. Built and CI verified. The seat must be observed in the field (`[ch.22] seat OBSERVED` in the log) before a client relies on it. `[ch.22] ... DENY MISSED` means the actor entered combat anyway and should be reported.

### ch.23 Pursuit leash (`kIntent_PursuitLeash`)

**What it is for.** Stop an NPC chasing a fight away from an anchor. The player sees a follower that fights what comes to it and does not run off after a fleeing foe.

**How to claim it.** ABI 16.

```cpp
APMF_Param p{};
p.target = anchorActorFormID;                        // REQUIRED. Often the player (0x14)
p.fval   = 1500.0f;                                  // REQUIRED. The radius in game units, > 0
Handle h = g_apmf->RequestEx(actor, kIntent_PursuitLeash, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `target` | the anchor actor. 0 and the actor itself are refused |
| `fval` | the leash radius in game units. Must be a positive, finite number |

`form`, `ival` and `pos` are not read.

**What Harbinger enforces.** While the actor is farther than the radius from the anchor, the combat behaviour-tree moves that take it toward its goal are refused when the goal is farther from the anchor than the actor already is, which is to say when the move would take it farther. The goal is the combat target for the ten pursuit leaves (Advance, Chase, FindAttackLocation, Flank, FlankDistant, MaintainOptimalRange, PursueTarget, Reposition, Stalk, Surround) and the combat group's search centre for the four search leaves (Search, SearchCenter, SearchLocation, SearchWander). A leaf is refused before it starts. One already running is ended the way the engine ends a leaf whose path failed. Every other move and every attack, spell and block runs natively. The actor is never walked back.

**Lifetime.** Standing. Ended by `Release`, an outranking ch.23 claim, a load, a new game or an unload. A dead actor runs no combat tree. A dead, unloaded or other-cell anchor holds nothing back.

Refused at the call: an APMF older than 16, VR, an unsupported runtime, `[PursuitLeash] bPursuitLeash=0`, a failed self-check, before `kDataLoaded`, target 0 or the actor itself, a radius that is not a positive number, or the player as the actor. An anchor that is not a loaded Actor is found at engage: the claim stands and denies nothing, and the log says so.

**Priority and conflicts.** It is its own facet, arbitrated separately from ch.7. It sits on the `CombatAction` sources facet. A leash stops a leaf only while your priority is above `Engine`. [ch.24](#ch24-combat-approach-kintent_combatapproach) respects a leash on the same actor.

**Examples.**

```cpp
Handle LeashToPlayer(RE::FormID follower, float radius) {
    if (g_apmf->abiVersion < 16) return kInvalidHandle;
    APMF_Param p{};
    p.target = 0x14;                             // the player
    p.fval   = radius;
    return g_apmf->RequestEx(follower, kIntent_PursuitLeash, kBasis, &p);
}

void ChangeRadius(Handle h, RE::FormID anchor, float radius) {
    APMF_Param p{};
    p.target = anchor;
    p.fval   = radius;
    g_apmf->Repoint(h, &p);
}
```

**Version and INI.** ABI 16 (0.9.10). `bPursuitLeash` in `[PursuitLeash]`. Built and CI verified. Not yet observed in game. The `[ch.23] pursuit H` heartbeat counts the leaves.

### ch.24 Combat approach (`kIntent_CombatApproach`)

**What it is for.** Make an NPC close to within R of an actor while it fights. A healer uses it to walk toward the friend it wants to heal while it keeps attacking and casting. The player sees a follower that moves toward the named actor in its combat gait, with both hands still working.

**How to claim it.** ABI 19.

```cpp
APMF_Param p{};
p.target = xFormID;                                  // REQUIRED. The reference to close on
p.fval   = 400.0f;                                   // REQUIRED. R in game units, finite and > 0
p.ival   = kApproach_Hold;                           // optional CombatApproachFlags
Handle h = g_apmf->RequestEx(actor, kIntent_CombatApproach, kBasis, &p);
```

| Field | Meaning |
|---|---|
| `target` | X, the reference to close on. An Actor or any loaded reference. The player is allowed. 0 and the actor itself are refused |
| `fval` | R in game units. Must be finite and greater than 0 |
| `ival` | `CombatApproachFlags`. `kApproach_Hold` (1): keep the bound after arrival and stay within R until released. 0 ends the claim on arrival |

`form` and `pos` are not read.

**What Harbinger enforces.** The engine's combat tree runs two parallel halves, Movement and Action. The Movement half keeps the actor inside its current combat area. Outside it, the engine's own Return To Combat Area branch paths back. Inside it, every movement leaf's destination is clamped to it. While the claim is in force, Harbinger centres the actor's own standard combat area on X with radius R. So the engine walks the actor toward X in its combat gait, and once inside R it keeps moving tactically within R. The engine also reads the area outside movement (the attack and bash pick, the target selector's score, the invisibility and bound-weapon cast decisions). Every one of those is answered with the engine's own area, not the bound, so no other action is dropped or changed.

The claim yields in three cases and moves nothing:

- A package's HOLD POSITION area outranks the standard area in the engine's own pick. The state reads `kApproachState_Yielded`.
- A fleeing actor, or one that is not in high process, has no standard area: `kApproachState_Disabled`.
- A ch.23 leash on the same actor that excludes every point within R of X: `kApproachState_Leashed`.

A claim made out of combat waits (`kApproachState_Waiting`) and applies when combat starts.

**State read.**

```cpp
std::uint32_t GetCombatApproachState(RE::FormID actor, APMF_CombatApproachInfo* out);   // ABI 19, any thread
```

Set `out->size = sizeof(APMF_CombatApproachInfo)`. The 48-byte v19 layout is filled when `size` covers it. The state stays readable after the claim ends, until the actor's next ch.24 claim or a load.

| State | Meaning |
|---|---|
| `kApproachState_None` (0) | no state since the last load |
| `Waiting` (1) | the claim stands, not in combat yet |
| `Approaching` (2) | the bound is in force and the actor is outside it |
| `Holding` (3) | the bound is in force and the actor is inside it |
| `Yielded` (4) | not applied, a package's hold position wins |
| `Disabled` (5) | not applied, fleeing or not in high process |
| `Leashed` (6) | not applied, a ch.23 leash excludes the area |
| `Arrived` (7) | ENDED, the engine reports the actor inside R of X |
| `TargetLost` (8) | ENDED, X is gone, dead, disabled, unloaded, in another cell space, or never a loaded reference |
| `CombatEnded` (9) | ENDED, the actor left combat after the bound was applied |
| `EngineDropped` (10) | ENDED, the engine stopped updating the bound for 3 seconds in combat, its own path back failed. Logged |
| `ActorGone` (11) | ENDED, the actor died, unloaded or lost its 3D |
| `Released` (12) | the claim was released by the client, outranked or dropped |
| `Refused` (13) | ENDED, a `Repoint` declared target 0, the actor itself or a bad radius |

`APMF_CombatApproachInfo` fields: `size`, `state`, `actor`, `target` (X as declared), `radius`, `distance` (actor to X, 3D, at the last update, -1 unknown), `msInState`, `seq` (compare for inequality), `ownerHandle`, `flags`, `applied` (how many times the engine's own area update carried the bound, 0 means the seat never applied it), `engineInside` (0 not seen, 1 outside, 2 inside).

**Lifetime.** Unlike ch.19, a ch.24 claim that ends is **released by Harbinger**, so a lower-ranked approach claim can take over. It ends on arrival (unless `kApproach_Hold`), when X is lost, when combat ends after the area was applied, when the engine drops the bound, or when the actor is gone. A client `Release`, an outranking ch.24 claim, a load or a new game also end it. Read the state to learn why.

Refused at the call: an APMF older than 19, VR, an unsupported runtime, `[CombatApproach] bCombatApproach=0`, a failed self-check, before `kDataLoaded`, target 0 or the actor itself, a radius that is not positive and finite, the player as the actor. A target that is not a loaded reference is found at engage and the claim ends as `TargetLost`.

**Priority and conflicts.** `Combat` sources facet. The bound is written only while your priority is above the engine's own combat area. A ch.24 claim is arbitrated against other ch.24 claims by basis.

**Examples.** Walk a healer to within 300 units of a friend and ask why it stopped.

```cpp
Handle ApproachFriend(RE::FormID healer, RE::FormID friendId) {
    if (g_apmf->abiVersion < 19) return kInvalidHandle;
    APMF_Param p{};
    p.target = friendId;
    p.fval   = 300.0f;
    p.ival   = kApproach_None;                   // end on arrival
    return g_apmf->RequestEx(healer, kIntent_CombatApproach, kBasis, &p);
}

const char* WhyApproachEnded(RE::FormID healer) {
    APMF_CombatApproachInfo info{};
    info.size = sizeof(info);
    switch (g_apmf->GetCombatApproachState(healer, &info)) {
        case kApproachState_Arrived:      return "arrived";
        case kApproachState_TargetLost:   return "target lost";
        case kApproachState_CombatEnded:  return "combat ended";
        case kApproachState_EngineDropped:return "the engine dropped the bound";
        case kApproachState_ActorGone:    return "actor gone";
        default:                          return "still going, or not ended";
    }
}
```

**Version and INI.** ABI 19 (0.11.0). `bCombatApproach` in `[CombatApproach]`. Built and CI verified, not field-run. The `[ch.24]` heartbeat in the log counts the seats.

## Read-only queries (no claim)

These are questions a client asks before it declares something. They claim no facet, hold nothing, write nothing into the world or the control map, and have no handle. A query is not an intent, and there is no combined query-and-act call. Harbinger answers and the client decides.

### Space queries (`FindEmptySpace` and `FindHostilesInSpace`)

**What they are for.** `FindEmptySpace` answers "where is an empty, standable spot over there?". `FindHostilesInSpace` answers "which hostile actors are inside this sphere?". They serve a position cast, a travel point, or anything else that has to pick a point.

**How to call them.** ABI 11. **True main thread only**, the one that runs the player's `Actor::Update`. From any other thread they do nothing and return `kQuery_NotMainThread`. SKSE's `AddTask` is not that thread. A client with its own player-update pump calls straight in from it.

```cpp
std::uint32_t FindEmptySpace(const APMF_SpaceQuery* q, APMF_SpaceResult* out);
std::uint32_t FindHostilesInSpace(const APMF_HostileQuery* q, RE::FormID* outActors,
                                  std::uint32_t capacity, std::uint32_t* outCount,
                                  std::uint32_t* outTotal);
```

Every struct starts with `size`. Set it to `sizeof` the struct. Harbinger refuses an input below the v11 layout (`kQuery_BadArgs`) and never writes an output past the size you declared.

`APMF_SpaceQuery` (44 bytes):

| Field | Meaning |
|---|---|
| `size` | `sizeof(APMF_SpaceQuery)` |
| `origin` | required. A loaded reference, actor or not. Its cell supplies the havok world. If it is an actor, the rays skip its capsule |
| `originX`, `originY`, `originZ` | a feet-level point, read only with `kSpaceFlag_OriginIsPoint` |
| `heading` | radians, Skyrim's angle Z (forward is (sin h, cos h)), read only with `kSpaceFlag_UseHeading` |
| `distance` | how far along the direction, > 0. Clamped to 2048 and logged |
| `clearance` | the free radius wanted around the point, > 0. Clamped to 16 to 512 |
| `maxDrop` | how far the ground may sit below the origin's feet. 0 means 128. Clamped to 1024 |
| `flags` | `SpaceFlags`: `kSpaceFlag_OriginIsPoint` (1), `kSpaceFlag_UseHeading` (2), `kSpaceFlag_ClampToWall` (4) |
| `reserved` | must be 0 |

`APMF_SpaceResult` (24 bytes): `size` (caller sets), `status` (the same value the call returns), `x`, `y`, `z` (the ground point, valid only on `kQuery_Ok`), `detail` (the collision layer that stopped a ray, or the occupying actor's FormID for `kQuery_Occupied`, else 0).

`APMF_HostileQuery` (24 bytes): `size`, `side` (required, the loaded actor whose side we are on), `x`, `y`, `z` (the sphere's centre, in the side actor's cell or worldspace), `radius` (> 0, clamped to 4096 and logged).

`outActors` receives up to `min(capacity, 64)` FormIDs (`kMaxHostileResults`), **nearest first**. `*outCount` is how many were written. `outTotal` (may be null) is how many matched before the cap, so you can tell a full sphere from a truncated list. The test is the engine's own `Actor::IsHostileToActor`, asked from each candidate (the same call Papyrus makes). Candidates are every loaded actor plus the player, in the side actor's worldspace (indoors, its cell), alive, enabled, not the side actor, with a 3D distance to the centre of at most `radius`. Harbinger keeps no faction list.

**What FindEmptySpace does.** A ray at the origin's chest height (feet plus 64) out to the point, then a ray straight down from there to feet minus `maxDrop`. The ground must be static, terrain or ground. Then the clearance: eight level rays of `clearance` length at the point's knee height (plus 32) must hit nothing, four ground rays at the clearance ring must land within 48 of the point's height, and no live actor may stand within `clearance` plus 32. At most 14 ray casts plus one pass over the loaded actors. Every failure names itself and nothing is retried or relaxed, except that `kSpaceFlag_ClampToWall` shortens a walk that meets a wall to (hit minus clearance) instead of returning `kQuery_Blocked`.

`QueryStatus`: `kQuery_Ok` (0), `NotMainThread` (1), `BadArgs` (2), `Unsupported` (3: VR, an unverified runtime, or before `kDataLoaded`), `NoOrigin` (4), `NoWorld` (5), `Blocked` (6: geometry or an actor between the origin and the point, `detail` is the layer), `NoGround` (7), `NotGround` (8), `Occupied` (9), `NoClearance` (10), `Ledge` (11: the clearance ring is not level), `Failed` (12: an exception was caught). Every non-Ok answer is one `[space] ...` warning line naming the reason.

**Lifetime.** Nothing persists.

**Priority and conflicts.** None.

**Examples.**

```cpp
// Where can a summon-sized thing stand, 180 units in front of this follower?
bool FindSummonSpot(RE::FormID follower, float& x, float& y, float& z) {
    APMF_SpaceQuery q{};
    q.size      = sizeof(q);
    q.origin    = follower;
    q.distance  = 180.0f;
    q.clearance = 80.0f;
    APMF_SpaceResult r{};
    r.size = sizeof(r);
    if (g_apmf->FindEmptySpace(&q, &r) != kQuery_Ok) return false;    // main thread only
    x = r.x; y = r.y; z = r.z;
    return true;
}

// Which hostiles, nearest first, are inside 300 units of that point?
std::uint32_t HostilesAt(RE::FormID side, float x, float y, float z, RE::FormID (&out)[16]) {
    APMF_HostileQuery hq{};
    hq.size = sizeof(hq);
    hq.side = side;
    hq.x = x; hq.y = y; hq.z = z;
    hq.radius = 300.0f;
    std::uint32_t n = 0, total = 0;
    if (g_apmf->FindHostilesInSpace(&hq, out, 16, &n, &total) != kQuery_Ok) return 0;
    return n;                                                         // total says if more matched than 16
}
```

**Version and INI.** ABI 11 (0.9.8). No key. Built and CI verified.

### Line of sight (`GetLineOfSight`)

**What it is for.** Harbinger's own answer to "can this actor see that one?". It is a physics ray test, never the engine's cached `HasLineOfSight`, which for an NPC returns a stale or false answer for targets the engine does not keep warm (allies, the player). It is the same ray the `kCastFlag_OwnLineOfSight` and `kTargetPin_OwnLineOfSight` flags use.

**How to call it.** ABI 18. Any thread, lock-free. It never blocks and never casts a ray itself.

```cpp
std::uint32_t GetLineOfSight(RE::FormID viewer, RE::FormID target, APMF_LosInfo* out);
```

It reads the newest stored measurement and marks the pair as asked for. The main thread measures asked-for pairs. So the **first call for a pair returns `kLos_Unknown`**, and a call a frame or two later has the verdict. Keep asking while you need it. A pair nobody asks for in 2000 ms is dropped.

| Verdict | Meaning |
|---|---|
| `kLos_Unknown` (0) | never measured, or the newest measurement is older than 1000 ms. Not seen. Ask again |
| `kLos_Visible` (1) | the own ray reached the target |
| `kLos_Occluded` (2) | every ray was blocked, or the target is in another havok world |
| `kLos_Unavailable` (3) | the ray could not be cast. `why` says why |
| `kLos_Unsupported` (4) | the service is not armed (VR, an unsupported runtime, a self-check refusal, `bOwnLineOfSight=0`, or before `kDataLoaded`). Keep your own test |

`APMF_LosInfo` (24 bytes): `size` (caller sets), `verdict`, `ageMs` (milliseconds since the stored measurement, 0xFFFFFFFF = never), `occludedRun` (consecutive OCCLUDED measurements of this pair, 0 after a VISIBLE), `sample` (the clear ray: 1 feet, 2 torso, 3 head, 0 none), `why` (a `LosWhy`: `BadArgs` 1, `ViewerNotLoaded` 2, `TargetNotLoaded` 3, `NoWorld` 4, `NoController` 5, `OtherWorld` 6, `TableFull` 7, `PickSkipped` 8). Pass `out == nullptr` for a verdict only.

The ray: from the viewer's eye point (the engine's own `GetEyeVector` origin). There is **no view cone and no facing test**. A target behind the viewer is as visible as one in front. Line of sight is geometry, not attention. It goes to three points on the target: feet plus 16, 55 percent and 90 percent of its height. Each ray ends short of its point by the target's own size, so a giant's own body never blocks it. It runs on the character-controller collision layer in the viewer's own collision group. Walls, closed doors and tent walls block. An open doorway, a railing under the line or a gate's gaps do not. **Another actor's body on the line blocks that ray.** It is VISIBLE when any of the three is clear, OCCLUDED only when all three are blocked. A target in a different havok world is OCCLUDED with `kLosWhy_OtherWorld`. A ray that cannot be cast is UNAVAILABLE, never VISIBLE.

Timings: a pair is re-measured at most every 250 ms (`kLosRefreshMs`), at most 8 pairs per frame, 3 rays each. Main-thread cost is bounded whatever the clients ask.

**Probing for the service.** `GetLineOfSight(0, 0, nullptr)` returns `kLos_Unsupported` when the service is not armed, before it reads any argument, and otherwise a bad-args verdict that is not `kLos_Unsupported`. MFO uses it as a free, no-ask probe.

**Lifetime.** Nothing persists. No pair or noise baseline crosses a load.

**Priority and conflicts.** None. Heal casts look past friends in this ray: see the in-main-for-next-release note under [Own line of sight](#own-line-of-sight-kcastflag_ownlineofsight). `GetLineOfSight` itself stays strict.

**Examples.**

```cpp
enum class Seen { Yes, No, Unknown, NoService };

Seen CanSee(RE::FormID viewer, RE::FormID target) {
    if (g_apmf->abiVersion < 18) return Seen::NoService;
    APMF_LosInfo info{};
    info.size = sizeof(info);
    switch (g_apmf->GetLineOfSight(viewer, target, &info)) {
        case kLos_Visible:     return Seen::Yes;
        case kLos_Occluded:    return info.occludedRun >= 2 ? Seen::No : Seen::Unknown;   // one reading can be borderline
        case kLos_Unsupported: return Seen::NoService;
        default:               return Seen::Unknown;     // first ask, stale, or the ray could not be cast
    }
}

bool LosServiceArmed() {
    return g_apmf->abiVersion >= 18 && g_apmf->GetLineOfSight(0, 0, nullptr) != kLos_Unsupported;
}
```

**Version and INI.** ABI 18 (0.11.0). `bOwnLineOfSight` in `[Sightline]`. Used by MFO, whose Sightline recipe it is.

### Awareness (`SenseActor`)

**What it is for.** "Does this actor sense that one?" It serves an engage-on-sight gambit. It is omnidirectional (no view cone, no facing anywhere) and multi-sense, so a client can give a follower the appearance of more senses than sight. Harbinger decides nothing about hostility, faction or crime. That is the client's call. The engine's own detection (`RequestDetectionLevel`) is deliberately not a sense, because its sight half is the facing-limited test this replaces.

**How to call it.** ABI 18. **True main thread only** (it casts the sight ray). From any other thread it returns `kQuery_NotMainThread` and does nothing.

```cpp
std::uint32_t SenseActor(const APMF_AwarenessQuery* q, APMF_AwarenessResult* out);
```

`APMF_AwarenessQuery` (32 bytes):

| Field | Meaning |
|---|---|
| `size` | `sizeof(APMF_AwarenessQuery)` |
| `viewer` | required. The loaded, live actor who senses |
| `target` | required. The loaded actor sensed, not the viewer |
| `anchor` | optional second listener (0 = none), for example the player the viewer follows. Hearing and engaged also count it. Sight and proximity ignore it. Not loaded is treated as none |
| `sightRange` | game units. 0 means 4096. Clamped to 8192 |
| `hearRadius` | game units. 0 means 1536. Clamped to 8192 |
| `proximityRadius` | game units. 0 means 256. Clamped to 2048 |
| `flags` | `AwareFlags`: senses to **skip**. `kAware_NoSight` (1), `NoHearing` (2), `NoProximity` (4), `NoEngaged` (8). 0 evaluates all four |

The four senses (`AwareSense` bits in the result):

| Sense | Bit | Fires when |
|---|---|---|
| `kSense_Sight` | 1 | the own ray is VISIBLE and the target is within `sightRange` |
| `kSense_Hearing` | 2 | a new engine noise by the target in the last 3000 ms (`kAwareNoiseWindowMs`), level above 0, within `hearRadius` of the viewer or the anchor, or the target is in combat within `hearRadius` of either |
| `kSense_Proximity` | 4 | the target is within `proximityRadius` of the viewer, whatever is between them |
| `kSense_Engaged` | 8 | the target is in combat and its current combat target is the viewer or the anchor |

Hearing by noise is novelty-based. The first time a query names a target, Harbinger records its current noise as a baseline and hears nothing from it (`kAwareDetail_NoiseBaseline`). A noise counts only when Harbinger saw that record change. A target in a different cell space (interior against exterior, another interior) senses nothing and sets `kAwareDetail_OtherSpace`. The sight test measures now unless the pair was measured less than 250 ms ago (reused), or the frame's cap of 8 synchronous measurements is spent. In that case a fresh stored verdict answers, else sight is not sensed this call (`kAwareDetail_SightDeferred`) and the pair is queued.

`APMF_AwarenessResult` (48 bytes): `size` (caller sets), `status` (a `QueryStatus`, also the return), `senses` (bits that fired, 0 = not sensed), `detail` (`AwareDetail` bits: `HeardNoise` 1, `HeardCombat` 2, `NearAnchor` 4, `EngagedAnchor` 8, `SightOutOfRange` 16, `SightUnavailable` 32, `OtherSpace` 64, `NoiseBaseline` 128, `NoiseRouteOff` 256, `SightReused` 512, `SightDeferred` 1024), `distance` (viewer to target, 3D, -1 when not computed), `sightVerdict`, `sightSample`, `sightWhy`, `noiseLevel` (-1 none), `noiseAgeSec` (-1 unknown), `noiseDistance` (-1 none), `engagedWith` (the viewer or anchor the target is fighting, 0 none).

Statuses: `kQuery_Ok` (evaluated, senses may be 0), `NotMainThread`, `BadArgs` (null pointer, a size below the v18 layout, viewer or target 0, viewer equal to target, a negative or non-finite range), `Unsupported` (service not armed), `NoOrigin` (the viewer is not a loaded live actor or the target is not a loaded actor), `Failed`.

**Lifetime.** Nothing persists. No noise baseline crosses a load.

**Priority and conflicts.** None. It changes nothing in the game.

**Examples.**

```cpp
// An engage-on-sight check for a follower. Call from the main thread.
bool SensesFoe(RE::FormID follower, RE::FormID foe, bool& heard) {
    if (g_apmf->abiVersion < 18) return false;

    APMF_AwarenessQuery q{};
    q.size       = sizeof(q);
    q.viewer     = follower;
    q.target     = foe;
    q.anchor     = 0x14;                  // the player counts as a listener for hearing and engaged
    q.sightRange = 3000.0f;               // the rest stay 0: Harbinger's defaults, all four senses

    APMF_AwarenessResult r{};
    r.size = sizeof(r);
    if (g_apmf->SenseActor(&q, &r) != kQuery_Ok) return false;
    heard = (r.senses & kSense_Hearing) != 0;
    return r.senses != kSense_None;
}
```

**Version and INI.** ABI 18 (0.11.0). `bOwnLineOfSight` in `[Sightline]` (when off, `SenseActor` answers `kQuery_Unsupported`). `bHearNoise` in `[Awareness]` (when 0, the noise route is off and `kAwareDetail_NoiseRouteOff` is set, while hearing a nearby fight, sight, proximity and engaged still work). Used by MFO.

## Observe-only facets (no claim API)

These seats exist so Harbinger can name who changes a part of the AI. **There is no claim API for any of them.** You do not call anything. They matter to a mod author for two reasons: they decide what the Harbinger menu page and the log show about your mod, and they explain what you will see when something else changes a value you hold. Each one changes, denies and delays nothing. A write always runs the original with the original arguments.

### Actor-value watch (`Detection` and `ActorValue` facets)

**What it is for.** While a claim holds a follower's actor values through Harbinger, the seat names anyone else who writes them. The values are: `MovementNoiseMult` and `DetectLifeRange` (held by [ch.16](#ch16-detection-kintent_detection)), `SpeedMult` (held by [ch.1a](#ch1a-gait-kintent_gait)), and Aggression, Confidence, Assistance and Morality (held by [ch.11](#ch11-disposition-kintent_disposition)).

**How it works.** Three virtual-function seats on the `Character` actor-value owner: `SetBaseActorValue` (0x04, which `SetActorValue` tail-jumps into), `ModActorValue` (0x05) and `RestoreActorValue` (0x06). A write to a held value by anyone but Harbinger is named by its caller module and counted on the `Detection` or `ActorValue` facet.

**What the client sees.** Counters and the module name on the menu page, and log lines. If you hold an actor value and a script or another mod changes it, you can read who in the log. You are never denied and the other mod is never denied. At any priority, the change stands.

**Version and INI.** Added in 0.13.0, no ABI change. `iActorValueSeat` in `[ActorValueObserve]`: 1 watch (default), 0 off. Not yet tested in game.

### Seat census

**What it is for.** Every engine seat Harbinger hooks is re-checked at each load stage and every 60 seconds. Another mod that hooks the same seat later, on top of Harbinger or inside it, is named in the log and in the census table at the bottom of the menu page, with whether Harbinger is still outermost.

**What it does.** A seat another mod hooked over becomes a `hook` source on that facet, logged once, and Harbinger re-hooks on top where a re-hook exists. This is how a foreign hook gets a priority and an attribution. It is why your priority against that mod means anything.

**What the client sees.** Nothing to call. The census is reported on the menu page and in the log.

**Version and INI.** 0.12.0. No key.

### Package and scene source capture

**What it is for.** On the `Package` facet, Harbinger compares the package an actor is running against the one a winning ch.9 (or ch.19) claim offered. If a different package runs, an outside input exists. Harbinger names its source: the plugin of the actor's current scene, else the plugin that defines the package, else `Engine` for a runtime-created package such as Movement Blocked, combat Spectator or dialogue. A scene with no file is Unattributed. It compares that source's priority with the claim's and **logs it, but denies nothing**.

**Scene probe.** `bSceneProbe` in `[Harbinger]` (default on, a passive probe) logs which engine code asks for an actor's scene while a mod has given that actor a package. The scene hold-off it is gathering evidence for is not built.

**The one enforced exception** is the Spectator hold-off, described under [ch.9](#ch9-offer-package-kintent_offerpackage).

**Version and INI.** 0.12.0 and 0.13.0. `bSceneProbe` in `[Harbinger]`, `iSpectatorSeat` in `[PackageGate]`.

### Equip caller recovery

**What it is for.** Names the mod behind an engine equip. The caller of an engine equip is recovered by a stack scan. An equip Harbinger cannot attribute is **never denied**. Armor verdicts are enforced for clients by default.

**What the client sees.** `path=...` and source names in the `[apmf][equip-obs]` log lines, and the `Equip` table on the menu page. See [ch.17](#ch17-equip-authority-kintent_equipauthority).

**Version and INI.** 0.12.0. `bEquipEnforceArmor` and `bEquipObserveOnly` in `[EquipAuthority]`.

### Caster-type and restore census

**What it is for.** A passive diagnosis of the cast facet. For an actor with a cast claim, `bCasterTypeCensus` logs which kind of combat caster the game's AI builds for the claimed spell (Restore, Offensive, Armor, Summon and the rest), whether the engine then fired it from the hand, whether the hand charged it, and the case where it built nothing at all. It also counts, for every actor, how often each kind fires. `bRestoreCensus` logs, for a healing or restoring spell, whether the AI ever reaches its own restore caster, how long that took, and whether the hand took the spell. Lines are rate limited.

**What the client sees.** Log lines only. The first time each cast seat answers a claim, the log says so. The census heartbeat counts every answer per caster type. If your claimed spell never reaches a caster, this is where you find out.

**Version and INI.** `bCasterTypeCensus` (1) and `bRestoreCensus` (1) in `[Probe]`. Both are on while field tested and are meant to return to 0 before a release. `bRangedSelect` (0) is a similar probe for a bow or crossbow on an actor with an equip claim. Log only, no behaviour change.

### The menu page (the viewer)

The Harbinger page in the SKSE Menu Framework shows, per facet: the sources, who wins, their priorities and hit counters, alerts and the seat census. See [HARBINGER-MENU.md](HARBINGER-MENU.md). Your DLL appears there under its file name once it files a claim, and a loaded SKSE plugin appears before it has written anything.

## Appendix A: sources and priorities per facet

Every Intent maps to one sources facet. This table says which, and what Harbinger enforces against a foreign source on that facet today. "Observe" means Harbinger names and counts the source and denies nothing.

| Sources facet (`APMF_Sources.ini` section) | Intents | Enforced against a foreign source |
|---|---|---|
| `Target` | ch.6, ch.20 | Restored through the engine's own `SetTarget` when a positively identified source is outranked |
| `Package` | ch.9, ch.19 | Observe. The Spectator hold-off is the one exception, and it needs your priority above `Engine` |
| `Equip` | ch.15, ch.17, hand block | Denied only for a positively identified, outranked source. An unattributed equip is never denied |
| `CombatAction` | ch.7, ch.23 | A leaf the engine's tree chose is stopped only while your priority is above `Engine` |
| `Cast` | ch.8, ch.8b, ch.25 | A refusal or substitution stands only while your priority is above the source of the answer it replaces (the engine, or a mod hooked there first) |
| `Combat` | ch.21, ch.22, ch.24 | ch.22 refuses a `StartCombat` only while above the calling source. ch.24 writes its bound only while above the engine's combat area. ch.21 makes one call |
| `Detection` | ch.16 | Watch only. Detection values are named, never refused |
| `ActorValue` | ch.1a, ch.11 | Watch only. Speed and disposition values are named, never refused |
| `Core` | ch.1, 3, 4, 5, 10, 12, 14 and the engine update seat | Counted and shown. Nothing is denied |

Priorities run 0 to 100, with 0 meaning off. A source is denied only when another source with a higher priority wants the same facet, and a tie goes to the source whose write is already standing.

**The file.** `Data/SKSE/Plugins/APMF_Sources.ini`, optional, read once at startup. The menu page writes it when you move a slider.

```ini
[Defaults]
iUnregisteredTarget=20
iClientCast=50

[Cast]
MyMod.dll=60
SomeOtherMod.dll=20
Engine=20

[UnknownSources]
Target=override
```

`iUnregistered<Facet>` and `iClient<Facet>` change the default for a whole facet. A name in a facet section beats the default. Names are DLL or plugin names and are not case sensitive. Raise `Engine=` in `[Combat]`, `[CombatAction]` or `[Cast]` above the client to let the engine win. `[UnknownSources]` takes `allow` (default) or `override` per facet and acts today on `Target`, `Combat` and `Cast`. See [INI-REFERENCE.md](INI-REFERENCE.md) for the full format.

## Appendix B: ABI history

The ABI is append-only. A client built against an older ABI keeps working against every later Harbinger. Harbinger returns null from `APMF_GetInterface(n)` for an `n` above its own ABI, and logs which release the client needs.

| ABI | First release | Adds |
|---|---|---|
| 1 | 0.2.0 | `Request`, `Release`, the first channels |
| 2 | 0.2.3 | `RequestEx`, `APMF_Param` |
| 3 | 0.3.0-rc.1 | `Repoint` |
| 4 | 0.3.0-rc.3 | `SetSpellAllowList` |
| 5 | 0.9.1 | `RequestCast`, `kIntent_Cast`, `CastFlags`, `APMF_CastRequest` |
| 6 | 0.9.3 | `GetCastProxy`, `IsClaimLive` |
| 7 | 0.9.5 | `kIntent_EquipAuthority`, `SetEquipSet` |
| 8 | 0.9.5 | `SetEquipSetEx`, `IsEquipAuthorityEnforced`, `kEquipAuth_DenyPlayerMenu` |
| 9 | 0.9.5 | `SetEquipScope` |
| 10 | 0.9.5 | `kIntent_Travel`, `TravelFlags`, `Data/APMF.esl` |
| 11 | 0.9.8 | `FindEmptySpace`, `FindHostilesInSpace`, `kCastFlag_AtPosition`, `kTravel_ToPosition` |
| 12 | 0.9.8 | `GetTravelLegState`, the travel gait bits, the BLOCKED end |
| 13 | 0.9.9 | `kIntent_TargetPin` |
| 14 | 0.9.9 | `kIntent_CombatEntry` |
| 15 | 0.9.10 | `kIntent_CombatReentryDeny` |
| 16 | 0.9.10 | `kIntent_PursuitLeash` |
| 17 | 0.9.10 | `kIntent_Idle` with a form and a target |
| 18 | 0.11.0 | `GetLineOfSight`, `SenseActor`, `kCastFlag_OwnLineOfSight`, `kTargetPin_OwnLineOfSight` |
| 19 | 0.11.0 | `kIntent_CombatApproach`, `GetCombatApproachState` |
| 20 | 0.11.0 | `kCastFlag_FloorSpellsOnly` |
| 21 | 0.12.0 | `kIntent_CastGuard` |

The last release is 0.13.0, ABI 21. The interface struct for ABI 21 is `APMF_API_v19`, since ABI 20 and 21 added no slot.

## Appendix C: refusal cheat sheet

A refusal at the call returns `kInvalidHandle` and logs the reason in `APMF.log`. The documented degrade is to keep doing it your own way. All intents are also refused on an unsupported runtime (anything but exactly 1.6.1170, 1.5.97 and 1.7.104), on VR, and when the engine update seat was refused at load.

| Intent | Refused at the call | Accepted, then inert one drain later (release it) |
|---|---|---|
| any | no channel for the intent (a Harbinger older than the intent's ABI) | |
| `kIntent_Cast` | `kCastFlag_OwnLineOfSight` with the service not armed or on the degenerate `RequestEx` form. `kCastFlag_AtPosition` on `RequestCast`. A position cast with a non-finite basis or an owned cast facet | a package with no spell input (`kCastFlag_FromPackage`). A dual claim against a single-hand claim loses by basis |
| `kIntent_EquipAuthority` | the equip seat is not installed | |
| `kIntent_Idle` with a form | v2 path not available, `target` equal to the actor | the form is not an IDLE record, the target is not loaded |
| `kIntent_Travel` | channel not installed (`bTravel=0`, VR, `APMF.esl` missing), no destination, a point without `kTravel_ToPosition`, with it a non-zero form or a non-finite point | the form is neither a reference nor a cell. A ninth simultaneous leg (reads `kLeg_Failed`) |
| `kIntent_TargetPin` | seat not installed, `form` 0 or the actor itself, the player as the actor, line of sight flag with the service down | `form` is not an Actor |
| `kIntent_CombatEntry` | seat not installed, `form` 0 or the actor itself, the player as the actor | `form` is not an Actor (ends the claim) |
| `kIntent_CombatReentryDeny` | seat not installed, `fval` negative or NaN, the player as the actor | |
| `kIntent_PursuitLeash` | leash not armed, `target` 0 or the actor itself, `fval` not positive, the player as the actor | the anchor is not a loaded Actor |
| `kIntent_CombatApproach` | seats not installed, `target` 0 or the actor itself, `fval` not positive and finite, the player as the actor | the target is not a loaded reference (ends as `TargetLost`) |
| `kIntent_CastGuard` | gate not installed, no param, no bit or an unknown bit, bad `fval`, the player as the actor, line of fire with the measurement not armed | |

## Appendix D: INI keys by facet

Defaults are the shipped values. The full text of every key is in [INI-REFERENCE.md](INI-REFERENCE.md).

| Facet | Section | Keys (default) |
|---|---|---|
| ch.8, ch.8b cast seats | `[CastSeats]` | `EnableSeat0Classify` (1), `EnableAimSeat` (1) |
| ch.8, ch.8b, ch.15 hand gates | `[EquipGate]` | `EnableEquipDenyComplete` (1), `EnableEquipGateLog` (0), `EnableDualWieldPreference` (0) |
| ch.8b score steer | `[AiCastSeats]` | `EnableScoreSteer` (1), `EnableWeaponScoreProbe` (1), `EnableItemScoreProbe` (0), `EnableCasterSeatProbe` (0) |
| ch.17 equip authority | `[EquipAuthority]` | `bEquipAuthority` (1), `bEquipObserveOnly` (1), `bEquipEnforceArmor` (1), `bEquipDenyScript` (0) |
| ch.8b hand block | `[HandBlock]` | `bHandClaimBlocksEquip` (1) |
| ch.9 package, Spectator | `[PackageGate]` | `EnableRedirectLog` (1), `iSpectatorSeat` (2) |
| actor-value watch | `[ActorValueObserve]` | `iActorValueSeat` (1) |
| ch.19 travel | `[Travel]` | `bTravel` (1), `bGateProbe2` (0) |
| position cast | `[PositionCast]` | `bPositionCast` (0) |
| ch.20 target pin | `[TargetPin]` | `bTargetPin` (1) |
| ch.21 combat entry | `[CombatEntry]` | `bCombatEntry` (1) |
| ch.12 idle v2 | `[Idle]` | `bIdleV2` (1), `bIdleConfirm` (1) |
| ch.22 re-entry deny | `[CombatReentryDeny]` | `bCombatReentryDeny` (1) |
| ch.23 pursuit leash | `[PursuitLeash]` | `bPursuitLeash` (1) |
| ch.24 combat approach | `[CombatApproach]` | `bCombatApproach` (1) |
| line of sight, own ray | `[Sightline]` | `bOwnLineOfSight` (1) |
| awareness | `[Awareness]` | `bHearNoise` (1) |
| sources registry | `[Harbinger]` | `bSceneProbe` (1) |
| census probes | `[Probe]` | `bCasterTypeCensus` (1), `bRangedSelect` (0), `bRestoreCensus` (1) |

Facets with no key of their own: ch.1, 1a, 3, 4, 5, 6, 7, 10, 11, 14, 15, 16 and ch.25. The space queries have none either.

## Appendix E: equip authority probe criteria

Equip authority ships with weapons in observe-only (`bEquipObserveOnly=1`). Read the `[apmf][equip-obs]` lines before you switch enforcement on. Each line carries `actor= item= op=equip path= site= ret= q= f= s= a= tls= cat= owned= black= eslot= verdict= name=`. `tls` is attribution only, since an equip Harbinger issued itself runs with `tls>0`, and it never changes a verdict. `cat= owned= black= eslot=` are the scope fields. `name` is the item's display name for reading, not parsing. A field log should show all of these before enforcement is switched on.

1. Every engine re-equip of an off-set item in an owned or denied category on a claimed actor logs `verdict=would-deny` with a named path: `OutfitApply`, `AddWornOutfit`, `AiCommand`, `RemoveItemReequip`, `CombatNode`, `Script`, `Console`, `WorkerReentry`, `QueuedApply`, `DropObject`, `PickUpObject`, `BoundItem`, `ProcedureEat`, `InventoryReequip`, `StartCombat`, or `External(<dll>)`. Two engine callers stay `Unknown` on purpose: 16104 and 15864 (a re-equip helper whose second route could not be pinned) and 52410 and 51535 (an orphan with no reference in the image). Either one on a deny line is a real finding (see `DENY-COMPLETENESS-AUDIT.md`, row 17). A `PlayerMenu` off-set equip logs `verdict=allow (player agency)` unless the claim carries `kEquipAuth_DenyPlayerMenu`. A non-governed form type, such as a potion, logs no per-event line.
2. Zero `would-deny` lines with `tls>0`. Harbinger's own equip pass only issues declared items, so a would-deny inside that bracket means the set the seat reads is not the set the pass enforced.
3. Zero `path=Unknown(<id>)` on a `deny` or `would-deny` line. An unknown id there is an engine caller the path table does not name yet. Add it before enforcing. Harbinger's own queued equips come back one actor update later as `path=QueuedApply tls=0 verdict=allow`. That is the engine applying what Harbinger queued, not a leak.
4. Every item Harbinger equips shows an `[apmf][equip-auth] ... -> equip` line followed by its `[apmf][equip-obs] ... tls=1 verdict=allow` line. An actor in high or middle-high AI process gets a `path=QueuedApply ... verdict=allow` line one actor update later. An actor in low process applies directly under the seat, so no `QueuedApply` line appears, and that is not a failure.
5. At least one `[apmf][equip-obs]` line has a named engine path (not `External(...)`). If every line reads `External(<dll>)`, another plugin has inline-detoured the engine's `EquipObject` entry. The seat logs `entry <id> detoured by <dll>` at install when it can see that. Attribution is degraded and the script exemption is unavailable until that detour is accounted for.
6. `path=PlayerMenu` attributes correctly: zero such lines in a session where the player never opens a follower's inventory, and every such line coincides with a trade or gift menu interaction.
7. The hand reaches the engine: at least one `[apmf][equip-auth] ... hand=left` equip line followed by the follower visibly dual-wielding, or holding the declared shield or torch in the left hand.
8. A follower with no hold that switches to a bow in combat logs `path=CombatNode ... verdict=allow cat=Right+Left owned=0 black=0 name='... Bow'` and then visibly shoots.
9. Zero `verdict=deny` or `verdict=would-deny` lines with `owned=0 black=0`.
10. A dual-wielder with no hold logs `cat=Shield+Left owned=0 black=1 verdict=deny` (or `would-deny` in observe mode) on the shield and stays shield-less.
11. Every WEAP line's `eslot` is `R`, `L` or `E`. An `E` on a `WorkerReentry` line is the case the conservative either-hand rule guards. It is expected.
12. The scope reaches the seat: after a `SET-EQUIP-SCOPE` line with a narrowed `owned`, every line for that actor whose `cat=` has no bit in `owned` and none in `denied` reads `owned=0 black=0 verdict=allow`.
13. The deny mask fires: for a scope with a non-zero `denied`, every line whose `cat=` overlaps `denied` reads `black=1` and `verdict=would-deny` (or `deny`), even for an item in the declared set. Zero `black=1 verdict=allow` lines except on a `Script`, `Console` or `PlayerMenu` path without the matching deny bit.
14. The category map agrees with the engine. A `cat=Shield+Left` line names a shield, `cat=Ammo` names arrows or bolts, `cat=Light+Left` names a torch, and a one-handed weapon with `eslot=R` or `eslot=L` reads `cat=Right` or `cat=Left`. A `cat=Right+Left` line for a one-hander carries `eslot=E`, `eslot=none` or a non-hand slot, never `R` or `L`.
15. The equip pass honours the scope: each `enforce pass` line prints `owned=0x.. denied=0x..` matching the last `SET-EQUIP-SCOPE` for that actor, and every declared item skipped as unowned or denied appears in one `NOT equipped by this pass` warning per actor and set, with `skipped-unowned` and `skipped-denied` counted on the pass line. Zero `-> equip` lines for an item whose categories are not all owned, or any denied.
