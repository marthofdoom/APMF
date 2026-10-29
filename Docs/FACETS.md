# Harbinger facet reference

This is the reference for every facet a mod can claim through the Harbinger (APMF) client API, one section per facet. Each section says what the facet is for, every argument it takes, what Harbinger enforces and leaves alone while the claim stands, how long it lives, who wins a conflict, how it is refused, and how far it has been proven in the field, with worked examples that compile against the real header.

| Page | What it is for |
|---|---|
| [GUIDE.md](GUIDE.md) | the foundations this page assumes: facets, basis and priority, the claim lifecycle, threading, binding |
| **This page, `FACETS.md`** | the reference, one section per facet |
| [RECIPES.md](RECIPES.md) | worked scenarios that combine facets |
| [GLOSSARY.md](GLOSSARY.md) | every term, defined once |

The source of truth is [`native/APMF_API.h`](../native/APMF_API.h) and the code under `native/channels/` and `native/core/`. If this page and the header ever disagree, the header wins. Text moved out of the header lives in [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md) (long explanations) and [APMF_API-HISTORY.md](APMF_API-HISTORY.md) (historical notes), each anchored per symbol. [INTEGRATION.md](INTEGRATION.md) is the quickstart. The player-facing pages are [INI-REFERENCE.md](INI-REFERENCE.md) and [HARBINGER-MENU.md](HARBINGER-MENU.md).

**How a facet section is laid out.** Each opens with a short summary in italics, then: **What it is for** (and what the player sees), **Arguments** (every field, with type, units, range, default and what 0 means), **What Harbinger enforces** (the mechanism, and what it leaves alone), **Lifetime**, **Priority and conflicts**, **Refusals** (at the call and at the drain, see [The two refusal timings](GUIDE.md#the-two-refusal-timings)), **Worked examples** (goal, Think it through, Solution, Check), **Version, INI and field status**, and **Check Your Understanding**, two or three questions to try, with the odd-numbered answers in [Selected Answers](#selected-answers). Callouts are **Key Concept**, **Tip**, **Caution**, **Real-World Connection** (how MFO uses it) and **Did You Know?** (the engine fact behind a rule). Where a facet is built but has not been seen working in game, the text says so.

**Conventions.** Distances are in game units unless a section says otherwise (one foot is 64 over 3 units). Windows are milliseconds in `APMF_CastRequest::ttlMs` and seconds in `APMF_Param::fval`. FormIDs are plain `RE::FormID` values. `0x14` is the player. A FormID names an object reference except where a section says it is a base form. `APMF_Param` carries a world point as `posX`, `posY` and `posZ`. Older header comments call it `param.pos`, and there is no `pos` member. "ch.N" is Harbinger's channel number, the log tag.

<details>
<summary><b>The prelude every example on this page assumes</b> (the binding from GUIDE.md and its tracked-claim helper)</summary>

```cpp prelude
#include "APMF_API.h"
#include <chrono>
#include <cstdint>
using namespace APMF_API;

extern const APMF_API_v19* g_apmf;   // bound once at startup, GUIDE.md "Binding and Version Gating"
constexpr float kBasis = 50.0f;      // your choice of basis. MFO uses 200

// GUIDE.md "Tracking a claim in your own code". Poll needs ABI 6.
enum class ClaimState { None, Pending, Live, Ended };

struct TrackedClaim {
    Handle        h        = kInvalidHandle;
    bool          everLive = false;
    std::uint32_t waited   = 0;
    static constexpr std::uint32_t kMaxPending = 15;   // unpaused polls before "never applied"

    bool Open(RE::FormID actor, Intent intent, float basis, const APMF_Param* p) {
        Close();
        h = g_apmf->RequestEx(actor, intent, basis, p);
        everLive = false; waited = 0;
        return h != kInvalidHandle;
    }
    bool OpenCast(RE::FormID actor, float basis, const APMF_CastRequest* req) {
        Close();
        h = g_apmf->RequestCast(actor, basis, req);
        everLive = false; waited = 0;
        return h != kInvalidHandle;
    }
    ClaimState Poll() {
        if (h == kInvalidHandle) return ClaimState::None;
        if (g_apmf->IsClaimLive(h)) { everLive = true; waited = 0; return ClaimState::Live; }
        if (!everLive && ++waited < kMaxPending) return ClaimState::Pending;
        return ClaimState::Ended;
    }
    void Repoint(const APMF_Param* p) { if (h != kInvalidHandle) g_apmf->Repoint(h, p); }
    void Close() {
        if (h != kInvalidHandle) g_apmf->Release(h);
        h = kInvalidHandle; everLive = false; waited = 0;
    }
};

struct Heartbeat {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    void Tick(TrackedClaim& c, const APMF_Param* p, std::uint32_t windowMs) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(windowMs / 3)) return;
        last = now;
        c.Repoint(p);
    }
};
```

</details>

## Contents

- [Facet index](#facet-index)
- [Movement and body](#movement-and-body): [ch.1 Movement block](#ch1-movement-block-kintent_movementblock), [ch.1a Gait](#ch1a-gait-kintent_gait), [ch.3 Stance](#ch3-stance-kintent_stance), [ch.4 Weapon drawn](#ch4-weapon-drawn-kintent_weapondrawn), [ch.5 Headtrack](#ch5-headtrack-kintent_headtrack), [ch.10 Dialogue](#ch10-dialogue-kintent_dialogue), [ch.12 Idle](#ch12-idle-kintent_idle)
- [AI disposition and senses](#ai-disposition-and-senses): [ch.11 Disposition](#ch11-disposition-kintent_disposition), [ch.16 Detection](#ch16-detection-kintent_detection)
- [Casting](#casting): [ch.8 Select spell](#ch8-select-spell-kintent_selectspell), [ch.8b Cast](#ch8b-cast-kintent_cast), [a hand floor](#a-hand-floor-kcastflag_denyhandonly), [own line of sight](#own-line-of-sight-kcastflag_ownlineofsight), [position cast](#position-cast-kcastflag_atposition), [ch.14 Shout and power](#ch14-shout-and-power-kintent_shoutpower), [ch.25 Cast guard](#ch25-cast-guard-kintent_castguard)
- [Equipment](#equipment): [ch.15 Equipment](#ch15-equipment-kintent_equipment), [ch.17 Equip authority](#ch17-equip-authority-kintent_equipauthority), [ch.26 Equip preference](#ch26-equip-preference-kintent_equippreference)
- [Packages and travel](#packages-and-travel): [ch.9 Offer package](#ch9-offer-package-kintent_offerpackage), [ch.19 Travel](#ch19-travel-kintent_travel), [Place](#place-kintent_place)
- [Combat](#combat): [ch.6 Combat target](#ch6-combat-target-kintent_combattarget), [ch.7 Combat action](#ch7-combat-action-kintent_combataction), [ch.20 Target pin](#ch20-target-pin-kintent_targetpin), [ch.21 Combat entry](#ch21-combat-entry-kintent_combatentry), [ch.22 Combat re-entry deny](#ch22-combat-re-entry-deny-kintent_combatreentrydeny), [ch.23 Pursuit leash](#ch23-pursuit-leash-kintent_pursuitleash), [ch.24 Combat approach](#ch24-combat-approach-kintent_combatapproach), [Combat substrate](#combat-substrate-kintent_combatsubstrate)
- [Read-only queries (no claim)](#read-only-queries-no-claim): [space queries](#space-queries-findemptyspace-and-findhostilesinspace), [line of sight](#line-of-sight-getlineofsight), [awareness](#awareness-senseactor)
- [Observe-only facets (no claim API)](#observe-only-facets-no-claim-api)
- [Appendix A: sources and priorities per facet](#appendix-a-sources-and-priorities-per-facet), [Appendix B: ABI history](#appendix-b-abi-history), [Appendix C: refusal cheat sheet](#appendix-c-refusal-cheat-sheet), [Appendix D: INI keys by facet](#appendix-d-ini-keys-by-facet), [Appendix E: equip authority probe criteria](#appendix-e-equip-authority-probe-criteria)
- [Selected Answers](#selected-answers)

## Facet index

"Tier" is how far to trust the facet ([The field tiers](GUIDE.md#the-field-tiers)). **Field-proven** means a live deck run exercised it, or a real client uses it. **Built** means it compiles and its mechanism is sound but it has not had its own field run. ABI is the version that introduced the facet.

**Table 1. The facet index**

| Intent | ch. | Facet | One line | ABI | Tier |
|---|---|---|---|---|---|
| [`kIntent_MovementBlock`](#ch1-movement-block-kintent_movementblock) | 1 | Movement block | Stand the actor still | 1 | Field-proven (in use by MFO) |
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
| [`kIntent_Idle`](#ch12-idle-kintent_idle) | 12 | Idle | Play an idle, optionally at a target | 1 (form: 17) | Field-proven (in use by MFO) |
| [`kIntent_ShoutPower`](#ch14-shout-and-power-kintent_shoutpower) | 14 | Shout and power | Record who owns the shout facet | 1 | Built (arbitration only) |
| [`kIntent_Equipment`](#ch15-equipment-kintent_equipment) | 15 | Equipment | Gate spell and staff re-arm | 1 | Field-proven (in use by MFO) |
| [`kIntent_Detection`](#ch16-detection-kintent_detection) | 16 | Detection | Silent movement, longer detect range | 1 | Field-proven |
| [`kIntent_EquipAuthority`](#ch17-equip-authority-kintent_equipauthority) | 17 | Equip authority | Declare what the actor wears and refuse the rest | 7 | Field-proven (in use by MFO), every category enforced by default since 0.14.0 |
| [`kIntent_Travel`](#ch19-travel-kintent_travel) | 19 | Travel | Walk the actor to a place | 10 | In use by MFO |
| [`kIntent_TargetPin`](#ch20-target-pin-kintent_targetpin) | 20 | Target pin | Hold the engine's combat target on yours | 13 | Field-proven (in use by MFO) |
| [`kIntent_CombatEntry`](#ch21-combat-entry-kintent_combatentry) | 21 | Combat entry | Start a fight with one `StartCombat` | 14 | Field-proven (in use by MFO) |
| [`kIntent_CombatReentryDeny`](#ch22-combat-re-entry-deny-kintent_combatreentrydeny) | 22 | Combat re-entry deny | Keep the actor out of combat for a while | 15 | Field-proven (in use by MFO, run 2026-09-28) |
| [`kIntent_PursuitLeash`](#ch23-pursuit-leash-kintent_pursuitleash) | 23 | Pursuit leash | Stop the actor chasing too far from an anchor | 16 | Field-proven (in use by MFO) |
| [`kIntent_CombatApproach`](#ch24-combat-approach-kintent_combatapproach) | 24 | Combat approach | Walk toward an actor while fighting | 19 | Field-proven (in use by MFO) |
| [`kIntent_CastGuard`](#ch25-cast-guard-kintent_castguard) | 25 | Cast guard | Hold attack spells through friends, bound channels | 21 | Field-proven (in use by MFO) |
| [`kIntent_EquipPreference`](#ch26-equip-preference-kintent_equippreference) | 26 | Equip preference | Lift a preferred weapon's combat score so the AI picks it | 22 | Built, not yet tested in game |
| [`kIntent_CombatSubstrate`](#combat-substrate-kintent_combatsubstrate) | 27 | Combat substrate | Combat as a substrate so an out-of-combat cast is animated | 23 | Field tested on LoreRim in 0.15.1 for out of combat casts, on by default |
| [`kIntent_Place`](#place-kintent_place) | 28 | Place | Put an actor, loaded or not, beside a reference or at a point in an interior cell, once | 28 | Built, not yet tested in game |
| [Space queries](#space-queries-findemptyspace-and-findhostilesinspace) | | `FindEmptySpace`, `FindHostilesInSpace` | Ask for a spot or the hostiles in a sphere | 11 | Built |
| [Line of sight](#line-of-sight-getlineofsight) | | `GetLineOfSight` | Harbinger's own ray | 18 | Used by MFO |
| [Awareness](#awareness-senseactor) | | `SenseActor` | Does this actor sense that one | 18 | Used by MFO |

Channel 18 is skipped on purpose. The number is reserved for an attack-selection design that is not on main.

## Movement and body

These facets act on the actor's body: where it stands, how fast it moves, how it holds itself. Most of them are small. Several apply a fixed built-in effect today and read no parameter, and each section says so, so that nobody designs around a field that does nothing yet.

### ch.1 Movement block (`kIntent_MovementBlock`)

*Park the actor's own movement goal where it already stands, and the planner has nowhere to go. The actor stands still without a package swap, without running in place, and without snapping back on release.*

#### What it is for

Stand the actor perfectly still. The player sees the follower stop walking. It does not run in place and it does not teleport back when you release.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_MovementBlock, kBasis);
// or: g_apmf->RequestEx(actor, kIntent_MovementBlock, kBasis, nullptr);
```

**Table 2. ch.1 arguments**

| Argument | Type | Range | Default | 0 or null means |
|---|---|---|---|---|
| `actor` | `RE::FormID`, a loaded actor | any NPC | required | refused as "actor not found" at the drain (ignored, no claim) |
| `basis` | `float` | any finite value | your choice | a legal basis, ranks below any positive one |
| `param` | `const APMF_Param*` | not read | `nullptr` | the same as `Request` |

#### What Harbinger enforces

> **Key Concept 1 (a parked goal)** Once, when the claim becomes the owner, Harbinger parks the actor's movement goal at its own position (`KeepOffsetFromActor` on itself with offset 0) and locks the mover (`SetDontMove(true)`). The planner sees "already there" and issues no locomotion command.

So there is nothing to run in place and nothing to snap back. The actor's package stays current and keeps evaluating. Harbinger never swaps it. A travel package simply waits, and finishes by itself when you release. Casting, combat, equipment, animations and dialogue are untouched.

> **Did You Know?** The engine's package system treats a goal it has reached as satisfied, not as blocked. That is why a parked goal is invisible to a package: the package waits exactly as it would if the walk were slow, and nothing fires `OnPackageEnd`.

#### Lifetime

A standing claim with no time limit. It ends on `Release`. On release Harbinger clears the parked goal and unlocks the mover. A load releases it through the channel. If the actor's reference leaves memory (it is deleted, or a non-persistent actor's cell unloads it), ControlMap's sweep drops the claim and there is nothing left to undo. A 3D unload alone does not drop it. No heartbeat is needed. Harbinger does not re-assert.

#### Priority and conflicts

Basis decides between clients. The facet belongs to the `Core` sources facet, which Harbinger counts and shows but does not deny on.

#### Refusals

At the call: only the ones every intent shares (an unsupported runtime, VR, the arbiter seat refused). At the drain: an actor that is not loaded is ignored with a log line and no claim is registered.

#### Worked example

**Example 1: Hold a follower still while it plays a give animation**

**Think it through.** The hold is a standing claim, so one request at the start and one release at the end. A refused hold (0) should not cancel the animation. The animation is a separate facet ([ch.12 Idle](#ch12-idle-kintent_idle)), claimed on its own.

**Solution.**

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

**Check.** The log shows `[ctl] 0x... + ch.1 MovementBlock ENGAGED` and the follower stops within a frame. On release the follower's package resumes and the log shows the release. If the follower keeps walking, another claim at a higher basis owns ch.1 on that actor and the log's `additional claim` line says so.

#### Version, INI and field status

ABI 1. In use by MFO. No `APMF.ini` key. It refuses itself on any runtime other than 1.6.1170, 1.5.97 and 1.7.104.

**Check Your Understanding: ch.1**

1. Why does the actor not run in place while held?
2. A quest package is driving the follower when you claim ch.1. Does the package end?
3. What happens to the hold if the follower's cell unloads?

### ch.1a Gait (`kIntent_Gait`)

*Speed is an actor value the movement code reads, so halving it through the co-saved ledger slows the actor even inside a package that locks everything else.*

#### What it is for

Slow the actor down. The player sees the follower walk at half pace.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Gait, kBasis);
```

**Table 3. ch.1a arguments**

| Argument | Type | Range | Default | 0 or null means |
|---|---|---|---|---|
| `actor` | `RE::FormID`, a loaded actor | any NPC | required | ignored at the drain |
| `basis` | `float` | any finite value | your choice | |
| `param.fval` | `float` | reserved for a future per-request multiplier | not read | the fixed factor, 0.5, applies whatever you pass |

#### What Harbinger enforces

It sets the actor's `SpeedMult` actor value to half of its current value, once. It is an input the movement code reads, so it holds even on an actor locked by a package.

> **Key Concept 2 (the actor-value ledger)** The write goes through Harbinger's co-saved actor-value ledger. The value before the claim is recorded. `Release` puts it back only if the value still equals the one Harbinger set. If a quest or another mod changed it in the meantime, theirs stands and Harbinger drops its stale record.

The same ledger serves [ch.11 Disposition](#ch11-disposition-kintent_disposition) and [ch.16 Detection](#ch16-detection-kintent_detection).

#### Lifetime

Standing, ended by `Release`. The ledger is saved with the game, so a save made while the claim was live does not strand the slow speed. After a load Harbinger restores every outstanding override whether or not a claim still exists. A load also releases the claim itself.

#### Priority and conflicts

`ActorValue` sources facet. It is watched only: another mod, a script or the game changing `SpeedMult` while you hold it is named in the log and on the menu page, and never refused. Between clients, basis decides.

> **Caution** Do not claim `kIntent_Gait` for the same actor from two mods and expect different values. The ledger keeps one record per actor value.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 2: Slow a follower while it carries something**

**Think it through.** A standing claim with no parameter. Open once, release when the carry ends. If a chosen speed on a walk is what you want, the travel facet's gait bits are per leg and pick Walk, Jog, Run or FastWalk ([ch.19 Travel](#ch19-travel-kintent_travel)).

**Solution.**

```cpp
void SlowForCarry(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Gait, kBasis);
}
void EndCarry(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }
}
```

**Check.** The follower walks at half pace within a frame. With `iActorValueSeat=1` the log names any other writer of `SpeedMult` while the claim stands.

#### Version, INI and field status

ABI 1. `iActorValueSeat` in `[ActorValueObserve]` switches the watch only. Field-proven.

**Check Your Understanding: ch.1a**

1. A script sets the follower's `SpeedMult` to 120 while your gait claim holds it at 50. What happens at your release?
2. What does `fval` do on this claim?
3. The player saves while the claim is live and loads that save later. Is the follower stuck at half speed?

### ch.3 Stance (`kIntent_Stance`)

*A sneak is one animation event each way, and Harbinger only sends the second if it sent the first.*

#### What it is for

Make the actor sneak. The player sees the follower crouch.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Stance, kBasis);
```

`param` is not read. `ival` is reserved.

#### What Harbinger enforces

One animation event. If the actor is not already sneaking, Harbinger sends `SneakStart`. It remembers whether it started the sneak. On `Release` it sends `SneakStop` only if it was the one who started it. There is no re-assert. The event is additive over the package's own crouch decision, so a package that forces a stance can undo it. For the common case it stands as a clean toggle.

#### Lifetime

Ended by `Release`. Nothing is saved.

#### Priority and conflicts

`Core`. Basis between clients.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 3: Sneak**

**Solution.**

```cpp
void Sneak(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Stance, kBasis);
}
```

**Check.** The follower crouches at engage and stands at release. If it stands up on its own while the claim is live, the running package forced its stance, which this facet does not deny.

#### Version, INI and field status

ABI 1. No key. Field-proven.

**Check Your Understanding: ch.3**

1. The follower was already sneaking when you claimed ch.3. What happens at your release?
2. Is a sneak re-asserted if a package stands the actor up?

### ch.4 Weapon drawn (`kIntent_WeaponDrawn`)

*The draw state is sticky, so one call draws and one call sheathes. The sheathe at release does not look at what the state was before you.*

#### What it is for

Draw the actor's weapon. The player sees the follower ready its weapon.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_WeaponDrawn, kBasis);
```

`param` is not read.

#### What Harbinger enforces

When the claim becomes the owner, Harbinger calls the engine's draw (`DrawWeaponMagicHands(true)`) once. On `Release` it calls the sheathe (`DrawWeaponMagicHands(false)`). The draw state is sticky, so there is no re-assert.

> **Caution** The sheathe on release happens whatever the state was before your claim. Do not release a draw claim on an actor you want to stay armed. Hold it until the actor should sheathe.

#### Lifetime

Ended by `Release`.

#### Priority and conflicts

`Core`. Basis between clients.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 4: Ready a weapon for a cutscene, and put it away**

**Solution.**

```cpp
void ReadyWeapon(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_WeaponDrawn, kBasis);
}
void Sheathe(Handle& h) {
    if (h != kInvalidHandle) { g_apmf->Release(h); h = kInvalidHandle; }   // the release is what sheathes
}
```

**Check.** The weapon comes out at engage and goes away at release.

#### Version, INI and field status

ABI 1. No key. Built, not yet battle-tested.

**Check Your Understanding: ch.4**

1. The follower drew its weapon on its own, then you claim and release ch.4. What is the weapon's state after the release?
2. Does Harbinger redraw the weapon if the engine sheathes it while the claim stands?

### ch.5 Headtrack (`kIntent_Headtrack`)

*A test facet with one fixed behaviour, and the only channel that re-asserts every tick. It is marked incomplete on purpose.*

#### What it is for

Control where the actor looks. Today the facet is a test facet with one fixed behaviour: it makes the actor look straight up.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Headtrack, kBasis);
```

`param` is not read. `form` is reserved for a future look-at target.

#### What Harbinger enforces

It sets the actor's point-based headtrack target to a spot 300 units above it. This is the only channel that does per-tick work. It re-applies the target every tick. That re-assert is a known stopgap. The AI writes several headtrack types (default, combat, dialogue, procedure) and Harbinger owns only one slot, so a package or an aggressive source can win the head back. Treat this facet as a known-incomplete gate.

#### Lifetime

Ended by `Release`. Release clears the action headtrack target, and the AI resumes its own tracking.

#### Priority and conflicts

`Core`. Basis between clients.

#### Refusals

Only the ones every intent shares.

#### Worked example

There is little to build on until the channel reads a target. A claim looks like this.

```cpp
void LookUp(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Headtrack, kBasis);
}
```

#### Version, INI and field status

ABI 1. No key. Field-proven for the look-up itself.

**Check Your Understanding: ch.5**

1. Which rule from GUIDE.md does this channel break, and why is that stated rather than hidden?
2. What does `form` do on this claim today?

### ch.10 Dialogue (`kIntent_Dialogue`)

*One `PauseCurrentDialogue` call at engage. The claim is a refcount around a one-shot, and release undoes nothing.*

#### What it is for

Interrupt a conversation the actor is in. The player sees the NPC stop talking mid-line.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Dialogue, kBasis);
```

`param` is not read.

#### What Harbinger enforces

One call, `PauseCurrentDialogue()`, when the claim becomes the owner. There is no lasting block. The actor can start a new conversation afterwards. A persistent "this actor is unavailable for dialogue" toggle is a documented follow-up that is not built.

#### Lifetime

`Release` is a no-op for the actor. The claim only refcounts the engagement.

#### Priority and conflicts

`Core`. Basis between clients.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 5: Shush an NPC**

**Think it through.** The pause happens at engage, so the claim can be released at once. Releasing a pending claim is applied after the engage in the same drain (GUIDE.md, [The drain](GUIDE.md#the-drain)), so the pause still happens.

**Solution.**

```cpp
void ShushNpc(RE::FormID actor) {
    Handle h = g_apmf->Request(actor, kIntent_Dialogue, kBasis);
    if (h != kInvalidHandle) g_apmf->Release(h);     // the pause already happened at engage
}
```

**Check.** The NPC's line stops at the next drain. A new line can start afterwards.

#### Version, INI and field status

ABI 1. No key. Built, not yet battle-tested.

**Check Your Understanding: ch.10**

1. Why is it safe to release the handle in the same function that requested it?
2. Does the claim stop the NPC starting a new conversation?

### ch.12 Idle (`kIntent_Idle`)

*You pick the idle record and the reference to play it at. Harbinger makes the one `PlayIdle` call and the engine evaluates the idle's own conditions. A one-shot idle that ends by itself is left alone at release.*

#### What it is for

Play an animation on the actor. The player sees a follower do a gesture or a work animation. With ABI 17 you choose the idle record and a reference to play it at (hand an item to a crate, for example).

#### Arguments

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

**Table 4. ch.12 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a base IDLE record | any IDLE | 0 | the v1 behaviour, one `IdleForceDefaultState` |
| `target` | `RE::FormID`, a loaded reference | any loaded reference, not the actor | 0 | the engine uses the actor's own process target. Read only when `form` is non-zero |
| `fval`, `ival`, `posX/Y/Z` | | not read | | |

> **Caution** Gate on `abiVersion >= 17` before you set `form`. An older Harbinger does not refuse a form. It ignores it and plays the form-free default, so an unchecked client gets the wrong animation and no error.

#### What Harbinger enforces

- v1 (form 0): one `IdleForceDefaultState` animation event at engage. Nothing at release.
- v2 (form set): one `PlayIdle(actor, idle, target)` call at engage, and one more on each `Repoint` or owner change. The idle's own conditions are evaluated by the engine against the actor and the target, and a failing one refuses it. The client picks the idle. Harbinger never does. At release it sends `IdleForceDefaultState` once, and only if the idle is still held (the actor's graph has not raised `IdleStop` since the call). A one-shot idle that already ended gets nothing.
- With `bIdleConfirm=1` an idle counts as played only when the actor's behaviour graph raises one of that idle's own entry events. If none arrives within 3 seconds of world time, Harbinger ends the claim and logs `ANIMATION NOT CONFIRMED`.

#### Reading what happened (ABI 24)

`IsClaimLive` alone cannot tell an idle that played from one nobody can confirm, because both stay live. From ABI 24, `GetIdleState(handle, &info)` says what became of the `PlayIdle` call, per claim handle. Do not time the 3 second window yourself. Harbinger judges it on world time at its Poll, about every 250 ms, and the state says the verdict.

**Table 4a. ch.12 idle states (`IdleState`)**

| State | Set when | The claim |
|---|---|---|
| `Unknown` (0) | no record: not a ch.12 claim with a form, a form-free claim, or the grace ran out | |
| `Pending` (1) | at `RequestEx` and at each `Repoint` to a form, until `PlayIdle` is called (not drained yet, not the claim that owns ch.12, or the play task has not run) | live |
| `Accepted` (2) | `PlayIdle` returned true and no entry event has arrived yet | live, the window runs |
| `Confirmed` (3) | Poll saw an entry event no other transition raises. The idle played | live |
| `ConfirmedWeak` (4) | Poll saw an entry event other transitions raise too. Weak evidence (REVIEW-BACKLOG APMF-B35) | live |
| `NotConfirmed` (5) | no entry event in 3000 ms of world time (and 30 frames) | Harbinger releases it |
| `NoTransition` (6) | at the call: a complete graph walk found no transition on the idle's event | Harbinger releases it at the next Poll |
| `Refused` (7) | at the call: `PlayIdle` returned false | Harbinger releases it |
| `Unconfirmable` (8) | at the call: `PlayIdle` returned true but no entry event can be resolved. `unconfirmableWhy` says why: confirmation off (`bIdleConfirm=0`), not armed (a self-check refusal), no entry events, an incomplete walk, an unreadable graph, or a broken event id space | live. Whether it played is unknown |
| `NotPlayed` (9) | before the call: idle v2 is not available (a `Repoint` with a form is not refused at the call), the form is not an IDLE, the target is not a loaded reference, or the actor is not loaded, dead or out of high process. No engine call was made | Harbinger releases it |
| `Carried` (10) | at the call: the same handle re-played the same idle and no `IdleStop` has been seen since its previous play's entry. This is an inference, not evidence. A fresh entry event within the same 3 second window a fresh play gets upgrades it to `Confirmed` or `ConfirmedWeak`. A later one does not count. An `IdleStop` with no fresh entry sets `kIdleInfo_CarryEnded`: the inherited idle ended | live, as a carry always was |

`endCause` says how the claim ended:

- `None`: it has not ended.
- `Harbinger`: Harbinger released it, and the state says why.
- `ActorDied`: Harbinger released it because the actor died (before or after the play).
- `Released`: you released it, or, while it owned ch.12, the unload sweep released the facet.
- `Superseded`: the claim is still live but another claim owns ch.12 now, or you re-pointed it to form 0. Its idle is no longer played. If it wins back it plays again and the record restarts at `Pending`.
- `Gone`: the claim stopped being live with no end Harbinger saw, for example it was released before its play ran, or the unload sweep dropped it while it did not own ch.12. Harbinger stamps it within about 1.25 seconds, and the grace runs from the stamp. `msSincePlay` and `msSinceEnd` are world milliseconds, which a menu pause does not advance. `msToEntry` is the wall time from the call to the entry event. `seq` changes on every change of the record. `kIdleInfo_Carried` in `flags` marks a play that was armed with a carry: the same handle re-played the same idle and no `IdleStop` had been seen since the previous play's entry. A carry never crosses handles. `kIdleInfo_CarryEnded` marks a carried play whose inherited idle raised `IdleStop` with no fresh entry from this play. The call reads a copy under Harbinger's own lock, so any thread may call it.

```cpp
if (g_apmf->abiVersion >= 24) {
    auto* v24 = reinterpret_cast<const APMF_API_v24*>(g_apmf);
    APMF_IdleStateInfo info{};
    info.size = sizeof(info);
    const std::uint32_t st = v24->GetIdleState(h, &info);
    if (info.endCause == kIdleEnd_Superseded) {
        // Another claim plays its idle on this actor now. Ours is live but not played.
        // Wait for it to win back (the record restarts at Pending), or Release it and stop.
    } else if (st == kIdleState_Confirmed || st == kIdleState_ConfirmedWeak) {
        // It played (Weak: the entry event is shared with other transitions; your policy).
        // Act on it. If endCause is not None the claim has already ended since.
    } else if (info.endCause != kIdleEnd_None) {
        // Ended without playing: NotConfirmed, NoTransition, Refused, NotPlayed, or the
        // claim went (Released, Gone, ActorDied). Do not act. Release is a no-op now.
    } else if (st == kIdleState_Unknown) {
        // No record: not an idle claim with a form, or its grace ran out. Treat as not played.
    } else if (st == kIdleState_Unconfirmable || st == kIdleState_Carried) {
        // Live, but no evidence for this play. Your call: never act on it as played.
    } else {
        // Pending or Accepted: wait. Harbinger judges the window, do not time it.
    }
}
```

#### Lifetime

Standing, ended by `Release`. When an idle ends, `IsClaimLive` reads false. From ABI 24 the idle state of an ended claim stays readable for 10 seconds of world time (`kIdleStateGraceMs`), so you see `NotConfirmed` or `Refused` and not `Unknown`. That is 40 of Harbinger's Polls and 25 of MFO's slowest ticks. A record stays as long as its claim is live. Harbinger ends the claim itself, logs why and does this when: the form is not an IDLE, the target is not a loaded reference, the actor is not loaded or is dead or has no AI process, the engine refused the idle, or the owner died. A load or a new game ends it. `Repoint` with a new `form` or `target` plays again.

#### Priority and conflicts

`Core`. Basis between clients. If the idle must not be undone by a walking package, hold the actor with [ch.1](#ch1-movement-block-kintent_movementblock) at the same time. They are separate claims.

#### Refusals

At the call, only when `form` is non-zero: VR, an unsupported runtime, a self-check refusal, `bIdleV2=0`, before `kDataLoaded`, or `target` equal to the actor. At the drain (the handle is live and inert, release it): the form is not an IDLE record or the target is not loaded.

#### Worked example

**Example 6: Play a give animation at a crate while the follower stands still**

**Think it through.** Two facets, two claims: a movement block so the walk does not undo the pose, and the idle itself. The idle needs ABI 17, so check first. If the idle is refused, do not leave the follower held. Release both at the end. Harbinger may end the idle claim itself (a one-shot that finished, an engine refusal), and `Release` on an ended claim is a no-op, so the end is the same either way.

**Solution.**

```cpp
struct GiveAnim { Handle hold = kInvalidHandle; Handle idle = kInvalidHandle; };

bool PlayGive(RE::FormID actor, RE::FormID idleForm, RE::FormID crate, GiveAnim& g) {
    if (g_apmf->abiVersion < 17) return false;             // an older Harbinger would play the wrong thing
    g.hold = g_apmf->Request(actor, kIntent_MovementBlock, kBasis);

    APMF_Param p{};
    p.form   = idleForm;
    p.target = crate;                                       // the engine tests the idle's conditions against it
    g.idle = g_apmf->RequestEx(actor, kIntent_Idle, kBasis, &p);
    if (g.idle == kInvalidHandle) {                         // refused: do not leave the follower held
        if (g.hold != kInvalidHandle) g_apmf->Release(g.hold);
        g = {};
        return false;
    }
    return true;
}

void EndGive(GiveAnim& g) {
    if (g.idle != kInvalidHandle) g_apmf->Release(g.idle);
    if (g.hold != kInvalidHandle) g_apmf->Release(g.hold);
    g = {};
}
```

**Check.** The log shows the ch.1 engage, then `[ch.12]` lines for the `PlayIdle` call and, with `bIdleConfirm=1`, the entry event that confirmed it. `ANIMATION NOT CONFIRMED` means the engine never played it: the idle's conditions failed against the actor and the crate.

> **Real-World Connection** This is MFO's deposit animation (`Deposit.cpp`): hold, play the give idle at the container, release both. IdleGive was field-confirmed on 2026-09-28.

#### Version, INI and field status

ABI 1, form and target from ABI 17 (first shipped in 0.9.10), `GetIdleState` from ABI 24 (struct `APMF_API_v24`, 0.13.1, CI verified only). `bIdleV2` and `bIdleConfirm` in `[Idle]`. In use by MFO. IdleGive was field-confirmed on 2026-09-28. The entry-event confirmation (`bIdleConfirm`) has only been CI verified.

**Check Your Understanding: ch.12**

1. What does an ABI 16 Harbinger do with a claim that carries a `form`?
2. A one-shot idle finished on its own. What does Harbinger send at your release?
3. Which other claim do you usually pair an idle with, and why?

## AI disposition and senses

These two facets work by setting the actor values the AI itself reads. Harbinger gives the engine's own decisions a different input. It does not override a decision. Both go through the co-saved actor-value ledger described under [ch.1a](#ch1a-gait-kintent_gait), so a save made while a claim is live never strands the changed values.

### ch.11 Disposition (`kIntent_Disposition`)

*Four actor values, set to fixed values through the ledger, make the actor fight on, hold its ground and help its friends.*

#### What it is for

Make the actor fight on, hold its ground and help its friends. The player sees a follower that does not flee and that joins fights.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Disposition, kBasis);
```

`param` is not read. `fval` is reserved for a future bias value.

#### What Harbinger enforces

Four actor values are set to fixed values through the ledger.

**Table 5. ch.11 values**

| Actor value | Set to | Meaning |
|---|---|---|
| Aggression | 2 | Aggressive |
| Confidence | 4 | Foolhardy |
| Assistance | 2 | Helps friends and allies |
| Morality | 0 | Any crime |

The values are fixed. They are not a function of anything you pass. Harbinger restores the previous values on `Release` (only if they still equal what Harbinger set).

> **Caution** Morality 0 means the actor will commit any crime. Claim this knowing that.

#### Lifetime

Standing, ended by `Release`. The ledger is saved, so a save while engaged does not strand the values. A load restores them.

#### Priority and conflicts

`ActorValue` sources facet, watched only. Between clients, basis decides.

#### Refusals

Only the ones every intent shares.

#### Worked example

```cpp
void StandAndFight(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Disposition, kBasis);
}
```

**Check.** The follower stops fleeing at low health and joins fights it would have watched. With `iActorValueSeat=1` any other writer of the four values is named in the log.

#### Version, INI and field status

ABI 1. `iActorValueSeat` in `[ActorValueObserve]` for the watch. Field-proven.

**Check Your Understanding: ch.11**

1. Can you ask for Confidence 3 instead of 4?
2. What does the claim do to the actor's willingness to steal?

### ch.16 Detection (`kIntent_Detection`)

*Silent feet and a longer reach for the eyes, through two actor values the detection code reads.*

#### What it is for

Make the actor quiet and sharp-eyed. The player sees a follower that sneaks past sleeping enemies and spots things earlier.

#### Arguments

```cpp
Handle h = g_apmf->Request(actor, kIntent_Detection, kBasis);
```

`param` is not read. `fval` is reserved.

#### What Harbinger enforces

Two actor values through the ledger: `MovementNoiseMult` is set to 0 (silent movement) and `DetectLifeRange` is raised to 1.5 times its current value. The values are fixed. Release restores them, with the same "only if unchanged" guard as ch.1a.

#### Lifetime

Standing, ended by `Release`. Saved through the ledger.

#### Priority and conflicts

`Detection` sources facet. The two values are watched only. A change by another source is named and counted, never refused, at any priority.

#### Refusals

Only the ones every intent shares.

#### Worked example

```cpp
void GoQuiet(RE::FormID actor, Handle& h) {
    if (h == kInvalidHandle) h = g_apmf->Request(actor, kIntent_Detection, kBasis);
}
```

**Check.** Sleeping enemies stay asleep as the follower passes. The log names any other writer of the two values.

#### Version, INI and field status

ABI 1. `iActorValueSeat` in `[ActorValueObserve]` for the watch. Field-proven.

**Check Your Understanding: ch.16**

1. Which two actor values does the claim set, and to what?
2. Is a change to `DetectLifeRange` by a script refused while the claim stands?

## Casting

Four intents and one flag deal with casting. They work in different ways, so pick carefully.

**Table 6. Which casting facet to claim**

| You want | Use |
|---|---|
| The NPC's own AI to cast spell S at target T, animated | [ch.8b `kIntent_Cast`](#ch8b-cast-kintent_cast) |
| The NPC to cast only one spell (or a short list) and nothing else | [ch.8 `kIntent_SelectSpell`](#ch8-select-spell-kintent_selectspell) |
| Close one hand to every other spell | a [deny-only hand claim](#a-hand-floor-kcastflag_denyhandonly) on ch.8b |
| An attack spell held while a friend is in the way | [ch.25 `kIntent_CastGuard`](#ch25-cast-guard-kintent_castguard) |
| No casting or attacking at all | [ch.7 `kIntent_CombatAction`](#ch7-combat-action-kintent_combataction) with `kCombatActionCat_Cast` |
| A Target Location spell dropped at a point, no animation | [position cast](#position-cast-kcastflag_atposition) |

### ch.8 Select spell (`kIntent_SelectSpell`)

*Narrow what the AI may cast to one spell plus a short list, by answering no at the two gates every AI cast choice passes through. The AI still decides when.*

#### What it is for

Narrow the spells an NPC's own AI is allowed to choose. While the claim stands, the only spell the AI may select and charge is the one you name, plus an optional short allow list. The player sees a follower that casts what you chose and nothing else. The AI still decides when to cast it. If you need the cast to happen at a target you name, use ch.8b.

#### Arguments

```cpp
APMF_Param p{};
p.form = spellFormID;                                       // the SpellItem the AI may cast
Handle h = g_apmf->RequestEx(actor, kIntent_SelectSpell, kBasis, &p);
```

**Table 7. ch.8 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a base SpellItem | any spell | required for a deny | names no spell, so with no allow list nothing is denied. There is no built-in default spell |
| `ival`, `target`, `posX/Y/Z` | | retired. They were the old APMF-driven cast mode. Setting them does nothing | | |

`SetSpellAllowList` (ABI 4) adds a bounded extra set:

```cpp
void SetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count);
```

`forms` is copied inside the call. `count` is clamped to `kMaxSpellAllowList` (32). `count == 0` or `forms == nullptr` clears the set. A stale handle, or a handle on another intent, is a silent no-op. The set is stored whether or not the claim currently owns the facet.

#### What Harbinger enforces

> **Key Concept 3 (gate-only)** Harbinger refuses every spell or staff that is not `param.form` and not in the allow list, at the two gates the AI's cast choice passes through: `CheckCast` (slot 0x0A on the hand casters) and `CheckShouldEquip` (slot 0x0F on the magic items). The refusal answers the engine's question with a no. It never casts anything and never equips anything.

The actor's movement, combat and all other behaviour are untouched.

#### Lifetime

Standing, no time limit. It ends on `Release`, a load or an unload. It is not saved.

#### Priority and conflicts

`Cast` sources facet. The refusal stands only while your priority is above the source of the answer it replaces: the engine, or a mod that hooked the same engine function before Harbinger. Defaults are client 50 against engine 20, so with the defaults you win. An answer that came through a hook Harbinger cannot name is allowed and shown as an alert, unless `[UnknownSources] Cast=override` is set.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 7: Let a follower cast Flames and its own heals, nothing else**

**Think it through.** The primary form is Flames. The heals go in the allow list, which needs ABI 4. The list is copied at the call, so a local array is fine.

**Solution.**

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

**Check.** With `EnableEquipGateLog=1` the log shows each refused spell at 0x0F and 0x0A, naming the claim. The follower casts Flames and its heals and no other spell.

#### Version, INI and field status

`RequestEx` is ABI 2 (0.2.3). `SetSpellAllowList` is ABI 4 (0.3.0-rc.3). Field-proven for the single owned spell. The graduated allow list is a separate capability and is not yet proven in game. Related keys: `EnableEquipDenyComplete` in `[EquipGate]`, and the `[CastSeats]` keys ([Appendix D](#appendix-d-ini-keys-by-facet)).

**Check Your Understanding: ch.8**

1. A ch.8 claim with `form = 0` and no allow list: what is denied?
2. Does ch.8 make the actor cast the named spell?
3. What happens to the 33rd spell in an allow list?

### ch.8b Cast (`kIntent_Cast`)

*The NPC's own combat AI can cast anything, except that it never considers a beneficial spell aimed at someone else. Harbinger supplies that one missing decision and answers the engine's own cast questions for the claim. The engine selects, equips, charges, aims, fires and channels. Harbinger casts nothing.*

#### What it is for

Make an NPC cast a chosen spell at a chosen target with the game's own animation. This is how a follower heals a friend or buffs an ally. The vanilla combat AI cannot classify a healing spell aimed at someone else, so it never considers casting one. Harbinger supplies that one missing decision and the engine does everything else: it selects the spell, equips it, charges it, aims it, fires it and channels it. Harbinger makes no equip, cast or animation call of any kind. The player sees an ordinary, fully animated cast.

It also serves attack spells, wards, summons, cloaks, lights, bound weapons, invisibility and buffs, and spells the combat AI has no caster for (muffle, fortify, resist, cures, calm, fear, frenzy, courage, night eye, detect life). A claimed spell of that last kind is filed under the engine's general-purpose caster and cast exactly as written. Not every one of these seats has been seen in the field. The caster-type census in the log says which ones answered.

> **Did You Know?** The engine's `CombatMagicCaster` classifier files each spell under a caster type (Offensive, Restore, Armor, Summon and so on) and builds no candidate for a beneficial spell aimed at anyone but the caster. Seat 0 of the cast facet is the one place Harbinger answers a classification the engine would have left empty.

#### Arguments

The rich call is `RequestCast` (ABI 5).

```cpp
Handle RequestCast(RE::FormID actor, float basis, const APMF_CastRequest* req);

struct APMF_CastRequest {
    RE::FormID    spell;   // the spell to cast (or the package, with kCastFlag_FromPackage)
    RE::FormID    proxy;   // a delivery proxy you made. 0 = none, Harbinger mints one if needed
    RE::FormID    target;  // the target actor. 0 = self for Self delivery and summon placement, else the AI's own combat target
    std::uint32_t flags;   // CastFlags, see below
    std::uint32_t ttlMs;   // the window in milliseconds. 0 = 4000. Clamped to 15000
};
```

**Table 8. `RequestCast` arguments**

| Argument | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `actor` | `RE::FormID`, a loaded actor | any NPC | required | ignored at the drain |
| `basis` | `float` | any finite value | your choice | |
| `req->spell` | `RE::FormID`, a base SpellItem (or a TESPackage with `kCastFlag_FromPackage`) | any spell | required, except on a deny-only claim | nothing to drive. Only a deny-only floor accepts it |
| `req->proxy` | `RE::FormID` | leave 0 | 0 | Harbinger mints a delivery-flip copy itself when a Self-delivery spell is aimed at someone else, and pools it. Read it back with `GetCastProxy(handle)` (ABI 6) |
| `req->target` | `RE::FormID`, a loaded actor | any actor | 0 | self, for a Self-delivery spell and for the placement of a summon or reanimate. For any other spell the AI aims at its own combat target |
| `req->flags` | `std::uint32_t`, `CastFlags` | the bits below | 0 | right hand, no hints, no stop percent |
| `req->ttlMs` | `std::uint32_t`, milliseconds | 1 to 15000 | 0 | 4000. Any value is clamped to 15000 |

The degenerate form `RequestEx(actor, kIntent_Cast, basis, &p)` with `p.form = spell` and `p.ival = CastFlags` has no target, no proxy and the default window. Use `RequestCast`. `req` is copied inside the call. The call is safe from any thread.

**The `CastFlags` word.** Flags are OR-ed. Gate each bit on the ABI that introduced it.

**Table 9. `CastFlags`**

| Bit | Name | Meaning | ABI |
|---|---|---|---|
| 0 | `kCastFlag_FromPackage` | `spell` is a TESPackage. Harbinger extracts only its spell input and target and never runs, offers or evaluates the package. A package with no readable spell input is refused at the drain | 5 |
| 1 | `kCastFlag_LeftHand` | hand hint. Default is the right hand. Scopes the per-hand deny and the equip seat | 5 |
| 2 | `kCastFlag_Concentration` | a hint that the cast is a held stream. It sets no time floor and changes no behaviour. It feeds logs and the caster census only. Use `MakeStopPct` to shape a channel | 5 |
| 3 | `kCastFlag_DualCast` | a hint to cast with both hands. The engine still decides whether the second hand arms. Never set `kCastFlag_LeftHand` with it. If both are set, dual wins | 5 |
| 4 | `kCastFlag_DenyHandOnly` | [claim a hand purely to deny it](#a-hand-floor-kcastflag_denyhandonly) | 5 (bit added in place) |
| 5 | `kCastFlag_AtPosition` | the [position cast](#position-cast-kcastflag_atposition). `RequestEx` only | 11 |
| 6 | `kCastFlag_OwnLineOfSight` | [Harbinger's own ray](#own-line-of-sight-kcastflag_ownlineofsight) gates the cast | 18 |
| 7 | `kCastFlag_FloorSpellsOnly` | with a hand floor, refuse spells only | 20 |
| 8 to 15 | stop percent | `MakeStopPct(1..100)`, see below | 5 |

> **Caution** An older Harbinger ignores a flag bit it does not know and applies the rest. `kCastFlag_DenyHandOnly` then makes a no-form claim that denies nothing, `kCastFlag_AtPosition` makes an ordinary actor-target claim, `kCastFlag_OwnLineOfSight` casts with no line test, and `kCastFlag_FloorSpellsOnly` blocks weapons too. Check `abiVersion` before you set them.

**Stop percent.** For a heal or restore channel, `MakeStopPct(80)` sets bits 8 to 15 to 80. The percent applies only to heal and restore channels. It is ignored on any other cast. The channel then stops the moment the target reaches 80 percent of its permanent actor value. 0 means no threshold. The channel then runs to full restoration, to the target's death, to your release, or to the window's end. Without it the engine stops where its own combat style says, which for a heal is about a quarter-second pulse. `ReadStopPct(flags)` reads it back.

#### What Harbinger enforces

> **Key Concept 4 (the seats)** While your claim owns the cast facet on the actor, Harbinger answers these engine decisions for it, and no others.

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

#### Lifetime

> **Key Concept 5 (always bounded)** A cast claim lives `ttlMs` (default 4000, at most 15000) from the last request or renewal, then Harbinger releases it and logs `cast claim ... expired`. A crashed client can never strand an actor.

- To hold a cast longer than one window, **heartbeat with `Repoint`**. `Repoint(handle, &param)` moves the deadline to now plus the same already-clamped window. A release and re-request cycle left the actor unclaimed for 0.26 to 0.61 seconds each window, and foreign spells equipped in the gap.
- `Repoint` cannot change what the claim casts. A `param.form` that differs from the claim's spell is refused and logged, and the claim keeps its spell. To cast something else, `Release` and `RequestCast` again. A same-form heartbeat is the shape to send.
- `IsClaimLive(handle)` turns false at expiry, release or eviction. Treat false like `kInvalidHandle` and re-request. It reads false while a fresh claim is still [pending](GUIDE.md#pending-and-live).
- Claims on different hands coexist. A dual-cast claim and a single-hand claim cannot. The loser is refused outright, or evicted if it was already live, and `IsClaimLive` turns false. A deny-only claim is outside that collision in both directions.
- A load or a new game releases every cast claim. An actor that unloads drops its claims.

#### Priority and conflicts

Among your own cast claims, the highest basis owns the hand. A tie keeps the earliest claim, with one exception: **at an equal basis a deny-only claim loses to a driving claim.** That is enforced for you, so a client that uses one basis for every claim can lay a floor first and still have its next cast take the hand. A floor placed at a basis strictly above a driving claim wins, and Harbinger logs that loudly because a follower holding a hand and casting nothing is hard to read from a log.

Against foreign sources, ch.8b sits on the `Cast` sources facet. Each answer Harbinger gives replaces the engine's, or an earlier mod's hook, and stands only while your priority is above that source's. Defaults are client 50 against engine 20. An answer through a hook Harbinger cannot name is allowed by default and shown as an alert.

#### Refusals

At the call: `kCastFlag_OwnLineOfSight` with the line-of-sight service not armed, or on the degenerate `RequestEx` form. `kCastFlag_AtPosition` on `RequestCast`. At the drain: a package with no spell input (`kCastFlag_FromPackage`) leaves the handle live and inert. A dual claim against a single-hand claim (or the reverse) at or below the incumbent's basis is dropped at the drain and `IsClaimLive` is false for it from the start.

#### Worked examples

**Example 8: Keep a heal going on a wounded friend, with a heartbeat**

**Think it through.** This is the shape MFO uses: a change of spell or target is a new claim, an unchanged claim is renewed. A claim that has not yet read live is left alone, because a pending claim released by the next tick would never engage. A pending claim that never applies lapses by itself inside `ttlMs`, so keep your tick slower than the drain (one per frame) and the claim inside its window. The heal goes on the left hand to keep the weapon hand free, as a concentration stream that stops at 80 percent.

**Solution.**

```cpp
struct HealClaim { Handle h = kInvalidHandle; bool seen = false; RE::FormID spell = 0; RE::FormID target = 0; };

void KeepHeal(RE::FormID healer, RE::FormID healSpell, RE::FormID friendId, HealClaim& c) {
    if (g_apmf->abiVersion < 6) return;                       // RequestCast is ABI 5, IsClaimLive is 6

    const bool same = c.h != kInvalidHandle && c.spell == healSpell && c.target == friendId;
    if (same) {
        if (g_apmf->IsClaimLive(c.h)) {
            c.seen = true;
            APMF_Param p{};
            p.form = healSpell;                               // same form: a pure heartbeat
            g_apmf->Repoint(c.h, &p);                         // renews the window
            return;
        }
        if (!c.seen) return;                                  // still pending: the next tick must not release it
    }
    if (c.h != kInvalidHandle) g_apmf->Release(c.h);          // changed, or it lapsed after being live

    APMF_CastRequest req{};
    req.spell  = healSpell;
    req.target = friendId;
    req.flags  = kCastFlag_LeftHand                           // keep the weapon hand free
               | kCastFlag_Concentration
               | MakeStopPct(80);                             // stop the stream at 80 percent
    req.ttlMs  = 4000;
    c.h = g_apmf->RequestCast(healer, kBasis, &req);
    c.seen = false;
    c.spell = healSpell;
    c.target = friendId;
}
```

**Check.** The log shows `[ch.8b] ... ENGAGED`, then the seat lines (`0x0F`, `0x06`, `0x0A`, `0x0D`) answering the claim, then `REPOINT ... TTL renewed` each tick. The follower raises its left hand, the heal beam lands on the friend and stops near 80 percent. `cast claim ... expired` while the tick runs means the tick is slower than the window.

> **Real-World Connection** MFO's `CastClaims.cpp` runs this shape per hand, with offense on one hand and heals on the other, and reads `GetCastProxy` so it can recognise its own proxy's cast in the engine's events.

**Example 9: Fire an offensive spell at a chosen foe, aimed by the follower's own AI**

**Think it through.** An attack spell needs no proxy and no stop percent. The target is load-bearing: seat 0x0A hands it to the AI and 0x0D aims at it. A short window is enough for one volley. Renew it if the fight goes on, exactly as in Example 8.

**Solution.**

```cpp
Handle CastAt(RE::FormID caster, RE::FormID spell, RE::FormID foe) {
    if (g_apmf->abiVersion < 5) return kInvalidHandle;       // no RequestCast on an older Harbinger
    APMF_CastRequest req{};
    req.spell  = spell;
    req.target = foe;
    req.ttlMs  = 3000;
    return g_apmf->RequestCast(caster, kBasis, &req);        // heartbeat it with Repoint to keep it past 3 s
}
```

**Check.** The follower turns to the foe and casts the named spell. The caster-type census (`bCasterTypeCensus=1`) logs which caster the AI built for it and whether the hand fired it.

#### Version, INI and field status

`kIntent_Cast` and `RequestCast` are ABI 5 (0.9.1). `GetCastProxy` and `IsClaimLive` are ABI 6 (0.9.3). Keys: `EnableSeat0Classify` and `EnableAimSeat` in `[CastSeats]`, `EnableEquipDenyComplete` in `[EquipGate]`, `EnableScoreSteer` in `[AiCastSeats]` (default 1 since ABI 22: the claimed spell's combat score is lifted one step above its best rival, see [ch.26](#ch26-equip-preference-kintent_equippreference)), `bHandClaimBlocksEquip` in `[HandBlock]`, `bOwnLineOfSight` in `[Sightline]`, and the census keys `bCasterTypeCensus` and `bRestoreCensus` in `[Probe]` (log only). Field-proven for heal-other and for offense. The non-Restore, non-Offensive caster seats and the general-purpose caster fallback are not yet field-run.

**Check Your Understanding: ch.8b**

1. Which engine call does Harbinger make to cast the spell?
2. A client wants a 20-second heal. It sets `ttlMs = 20000`. What window does it get, and how does it hold the heal for 20 seconds?
3. What does `kCastFlag_Concentration` change about the cast?

### A hand floor (`kCastFlag_DenyHandOnly`)

*A single-hand claim denies only its own hand. A floor claims the other hand purely to deny it, drives nothing, and loses a tie to any driving claim so your own next cast can take the hand over it.*

#### What it is for

A single-hand cast claim denies competing spells only on its own hand. The other hand stays AI governed, and the field showed the AI using it: Stone Runes, Poison Sprays and Raise Zombies charged on the free hand while the claim held the other. A deny-only claim fixes that. It claims a hand purely to deny it. The client drives nothing. Harbinger forces the claim's spell, proxy and target to 0, so no seat seats, drives or classifies anything for it.

#### Arguments

A `RequestCast` with `kCastFlag_DenyHandOnly` in `flags`. `spell`, `proxy` and `target` are forced to 0. `kCastFlag_LeftHand` picks the hand (default right). `ttlMs` is the window. `kCastFlag_FloorSpellsOnly` (ABI 20) narrows the floor to spells.

#### What Harbinger enforces

- The floor stands on the hand `kCastFlag_LeftHand` selects (default right).
- It is a bounded cast claim. Same window rules, same `Repoint` renewal. A `Repoint` on it renews the window and nothing else. A form in the param is forced to 0.
- A floored hand admits **nothing**, including your own spells, because the floor names no form. Claim the hand with a driving `kIntent_Cast` to reopen it. A direct `CastSpellImmediate` force does not consult `CheckCast`, so a floor does not stop it.
- Without `kCastFlag_FloorSpellsOnly` a floor also refuses weapons, shields, torches and unarmed into that hand. With the flag (ABI 20) it refuses spells, scrolls and staffs only. A one-hander, a shield or a torch passes. A two-hander, a bow, or an item the engine has given no hand still competes for both hands and is refused while a driving claim holds the other one.
- A floor and a dual-cast claim coexist both ways. A dual claim never evicts a floor.

> **Key Concept 6 (the floor tie)** At an equal basis a deny-only claim loses to a driving cast claim. A client that files every claim at one basis can lay the floor first and its next driving claim takes the hand the instant it publishes, with the floor still standing underneath when that claim ends.

> **Caution** An APMF older than the bit makes a no-form claim that denies nothing. Ship the pair together and gate on `abiVersion`.

#### Worked example

**Example 10: Deny the left hand to every spell while a driving claim casts from the right**

**Think it through.** The floor should let a shield or a torch into the left hand, so `kCastFlag_FloorSpellsOnly` is the right shape, and it needs ABI 20. The floor is bounded like any cast claim, so it is heartbeated with `Repoint` from the same tick that renews the driving claim.

**Solution.**

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

**Check.** With `EnableEquipGateLog=1` the log shows every spell refused into the left hand, and a shield passing. The follower never charges a spell on the left while the floor stands. A `DENY-ONLY FLOOR OUTRANKS A LIVE CAST` warning means the floor's basis is above your driving claim's, and the hand is shut to you too.

#### Version, INI and field status

The bit was added in place at ABI 5 (2026-09-06). `kCastFlag_FloorSpellsOnly` is ABI 20 (0.11.0). In use by MFO.

**Check Your Understanding: A hand floor**

1. Can the client's own `RequestCast` on the floored hand reopen it?
2. A floor at basis 200 and a driving claim at basis 200: who owns the hand?
3. What does an ABI 19 Harbinger do with `kCastFlag_FloorSpellsOnly`?

### Own line of sight (`kCastFlag_OwnLineOfSight`)

*Without a line test the claim casts into a wall. The engine's cached answer cannot be asked for an NPC, so Harbinger judges the line with its own ray and holds the start until it is clear.*

#### What it is for

Without this bit the claim decides whether to cast, so a claim aimed at a target behind a wall is cast into the wall. The engine's own `HasLineOfSight` cannot be asked instead. For an NPC it returns a cached answer on the engine's own schedule, stale for targets the engine does not keep warm. With the bit, and a target that is another actor, Harbinger judges the line with its own ray ([line of sight](#line-of-sight-getlineofsight)).

#### Arguments

`kCastFlag_OwnLineOfSight` in `APMF_CastRequest::flags`, with `target` set. `RequestCast` only.

#### What Harbinger enforces

- Seat 0x06 answers yes only while the ray's verdict for caster to target is VISIBLE and at most 1000 ms old. Otherwise it answers no. The claim stands and the next poll asks again. The first cast waits a frame or two for the first verdict.
- Seat 0x07 stops a held channel when the verdict is OCCLUDED on two consecutive measurements. UNKNOWN or UNAVAILABLE never stop a running channel.
- UNAVAILABLE holds the start like OCCLUDED and is logged. It is never treated as visible.
- A self target, and a deny-only claim, have no line to judge. The bit is ignored on them.

**In main, next release:** a beneficial cast (a spell with no hostile effect) looks past the player and the player's teammates in its own line of sight at seats 0x06 and 0x07. Walls, the ground and every other NPC still block it. Offensive casts, the cast guard, `GetLineOfSight`, the target pin and `SenseActor` are unchanged and stay strict. The log names the friend that was looked past, once per spell, target and friend. This is merged but not released, and not yet tested in game.

#### Refusals

At the call: the degenerate `RequestEx` form carries no target, so the bit there is refused (`kInvalidHandle`, logged). Refused at the call while the line-of-sight service is not armed. Keep your own test then. An APMF older than ABI 18 ignores the bit and casts with no test.

#### Worked example

**Example 11: Cast only when the follower can see the target**

**Think it through.** Probe the service once (`GetLineOfSight(0, 0, nullptr)` answers `kLos_Unsupported` when it is not armed), set the bit only when it is, and otherwise cast without the test or keep your own.

**Solution.**

```cpp
Handle CastIfSeen(RE::FormID caster, RE::FormID spell, RE::FormID foe) {
    if (g_apmf->abiVersion < 18) return kInvalidHandle;
    const bool losArmed = g_apmf->GetLineOfSight(0, 0, nullptr) != kLos_Unsupported;   // a free probe
    APMF_CastRequest req{};
    req.spell  = spell;
    req.target = foe;
    req.flags  = losArmed ? kCastFlag_OwnLineOfSight : kCastFlag_None;   // the bit is refused when unarmed
    req.ttlMs  = 4000;
    return g_apmf->RequestCast(caster, kBasis, &req);
}
```

**Check.** With the foe behind a wall the follower holds the cast and the log shows seat 0x06 answering no with the ray's verdict. It fires when the foe steps out.

#### Version, INI and field status

ABI 18 (0.11.0). `bOwnLineOfSight` in `[Sightline]`. Used by MFO.

**Check Your Understanding: Own line of sight**

1. What does seat 0x07 do when the verdict turns UNKNOWN during a heal beam?
2. Why can the engine's own `HasLineOfSight` not be used instead?

### Position cast (`kCastFlag_AtPosition`)

*A Target Location spell cast by an NPC lands at the NPC's own hand, so Harbinger casts it from a temporary marker at the point instead. No animation, off by default, and a one-shot rather than a claim.*

#### What it is for

Drop a Target Location spell at a point in the world, blamed on an actor. The actor does not animate and its hands are untouched. It is a stepping stone, not a finished feature, and it is **off by default**.

#### Arguments

`RequestEx` only (ABI 11).

```cpp
APMF_Param p{};
p.form = spellFormID;                    // a SpellItem. Target Location, fire-and-forget, no projectile
p.ival = kCastFlag_AtPosition;           // this flag ALONE. Any other cast flag is refused
p.posX = x; p.posY = y; p.posZ = z;      // world point in the actor's worldspace (indoors: its cell)
Handle h = g_apmf->RequestEx(actor, kIntent_Cast, kBasis, &p);
```

**Table 10. Position cast arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a base SpellItem | Target Location delivery, fire-and-forget, no projectile, no summon | required | refused at the drain |
| `ival` | `std::int32_t` | exactly `kCastFlag_AtPosition` | required | an ordinary cast claim |
| `posX/Y/Z` | `float`, game units | a point in a loaded, attached cell of the actor's worldspace | required | `(0, 0, 0)` is a legal point when the flag is set |
| `basis` | `float` | finite | | a non-finite basis is refused at the call |

Gate on `abiVersion >= 11`. An older Harbinger ignores the flag and makes an ordinary actor-target claim.

#### What Harbinger enforces

On the game thread it places a non-persistent XMarker (Skyrim.esm `0x3B`) at the point, in the cell that contains it, and casts the spell from that marker, blamed on the actor. That is the same sequence the engine's Papyrus `Spell.RemoteCast` runs. The marker is deleted a frame later. Harbinger charges no magicka.

> **Did You Know?** A marker casts, and the actor does not, because the engine places a Target Location spell cast by an NPC at the NPC's own magic node and never reads the target it was given. Read off both game binaries.

#### Refusals

Each with one `[poscast] ... REFUSED` log line.

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

#### Lifetime

It is a one-shot, not a claim. It never enters the control map and holds no facet. The handle labels log lines and nothing else. `IsClaimLive` is false for it and `Release` and `Repoint` do nothing. You learn the outcome from `APMF.log` (`queued`, then `DELIVERED` or `REFUSED`). Every marker Harbinger places is recorded in the co-save (record `XMRK`). After a load the markers the loaded world still holds are deleted, each only after three proofs: the recorded FormID resolves, its base is XMarker, and it stands within 1 unit of the recorded spot.

#### Priority and conflicts

None, since it holds no facet. It yields to an owner of the cast facet.

#### Worked example

**Example 12: Pick a clear spot with `FindEmptySpace`, then drop the spell there**

**Think it through.** The query is main-thread only, so this function runs from your main-thread hop. The query's point is the ground point, which is what a Target Location spell wants.

**Solution.**

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

**Check.** `[poscast] ... queued` then `DELIVERED` in the log, and the spell's effect appears at the point with no animation on the actor. `REFUSED` names which rule the spell broke.

#### Version, INI and field status

ABI 11 (0.9.8). `bPositionCast` in `[PositionCast]`, default 0. Built, CI verified, not battle-tested. No field run of the position cast is recorded.

**Check Your Understanding: Position cast**

1. Why does `IsClaimLive` read false for a position cast that was delivered?
2. A rune spell is sent as a position cast. What happens?

### ch.14 Shout and power (`kIntent_ShoutPower`)

*Arbitration only. Harbinger records who owns the shout facet and makes no equip write.*

#### What it is for

Record that a mod owns the shout or power selection on an actor, so Harbinger is the single arbiter of who does. Today it is **arbitration only**. Harbinger makes no equip write and denies nothing.

#### Arguments

```cpp
APMF_Param p{};
p.form = shoutOrPowerFormID;                                  // recorded for observability and a future gate
Handle h = g_apmf->RequestEx(actor, kIntent_ShoutPower, kBasis, &p);
```

`form` is recorded. 0 records nothing.

#### What Harbinger enforces

Nothing beyond recording the owner. You select the shout yourself with your own call (`ActorEquipManager::EquipShout`). The channel used to make that call itself and was converted to arbitration only. A future gate may deny a competing framework's own shout write.

#### Lifetime

Ended by `Release`. Nothing to undo.

#### Priority and conflicts

`Core`. Basis between clients.

#### Worked example

```cpp
Handle OwnShout(RE::FormID actor, RE::FormID shout) {
    APMF_Param p{};
    p.form = shout;
    return g_apmf->RequestEx(actor, kIntent_ShoutPower, kBasis, &p);
}
```

#### Version, INI and field status

ABI 1. No key. Built, not yet battle-tested.

**Check Your Understanding: ch.14**

1. Does a ch.14 claim equip the shout?
2. What is the claim for, then?

### ch.25 Cast guard (`kIntent_CastGuard`)

*Two rules on an NPC's own casts, enforced at the cast seats Harbinger already owns: hold an attack spell while a friend is in the line of fire, and keep the AI's own concentration spells off a hand unless a claim names them.*

#### What it is for

Put a rule on an NPC's own casts. Two rules exist, and one claim can carry either or both. The player sees a follower that does not fire a fireball through the player and that does not start streams the client has not allowed. Harbinger installs no hook for it and drives nothing. It says no at the cast seats it already owns.

#### Arguments

ABI 21, `RequestEx`.

```cpp
APMF_Param p{};
p.ival = kCastGuard_LineOfFire | kCastGuard_ChannelRight | kCastGuard_ChannelLeft;
p.fval = 6.0f;                                                // the window in SECONDS
Handle h = g_apmf->RequestEx(actor, kIntent_CastGuard, kBasis, &p);
```

**Table 11. ch.25 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `ival` | `std::int32_t`, `CastGuardFlags` | at least one known bit, no unknown bit | required | refused at the call |
| `fval` | `float`, seconds | 0 to 15. Negative or non-finite is refused | 0 | 4 seconds. Above 15 is clamped to 15 |
| `form`, `target`, `posX/Y/Z` | | not read | | |

**Table 12. `CastGuardFlags`**

| Flag | Value | Guard |
|---|---|---|
| `kCastGuard_LineOfFire` | 1 | hold an offensive cast while a friend is in the way |
| `kCastGuard_ChannelRight` | 2 | refuse the AI's own concentration cast on the right hand |
| `kCastGuard_ChannelLeft` | 4 | refuse the AI's own concentration cast on the left hand |
| `kCastGuard_All` | 7 | all three |

#### What Harbinger enforces

*Line of fire.* An offensive cast (any effect with a hostile magic effect, not Self delivery, whether spell, scroll or staff) is held while a friendly stands in its line of fire. Friendly means any actor not hostile to the caster (the engine's own `IsHostileToActor`, asked from that actor). For a caster that is a player teammate, the player and every teammate count too.

> **Key Concept 7 (in the line of fire)** A friend is in the line of fire when it is within 80 units of the line from the caster to the aim and not read OCCLUDED from the caster by Harbinger's own ray (a wall between them means the bolt cannot reach it, and an unknown reading still counts the friend), or when it is within the spell's blast area plus 64 units of the aim.

The blast area is the spell's largest effect area. The game stores it in **feet**. Harbinger converts it at 64 over 3 units per foot (Fireball, area 15, is 320 units). Friends up to 4096 units from the aim are considered. The aim is the target of the actor's live driving `kIntent_Cast` claim when that claim's spell is itself offensive, else the actor's combat target. No aim means no line, so nothing is held.

Harbinger measures every guarded caster on the main thread every frame (at most 8 per frame, round robin), from fresh positions. The seats read the newest measurement lock-free and trust it for 250 ms.

- At `CheckStartCast` (0x06, the AI's decision to start): the answer is no unless a measurement at most 250 ms old says the line is clear. This holds claim-driven casts too, because the guard runs before the claim answers yes.
- At `CheckCast` (0x0A on the hand casters, hard): refused (`kMultipleCast`) when a measurement at most 250 ms old says blocked. The engine consults it at cast start, at release of a charged cast and on every tick of a concentration channel. A refusal at a release or a tick interrupts the cast. An unknown measurement never refuses here.
- There is no decay. Harbinger holds as long as a friend is in the line. A client that wants a decay drops the bit with a `Repoint` for a moment.

*Channel bound.* On a bounded hand the AI's own concentration spell is refused at 0x0A, at its start and on every channel tick, **unless** a live `kIntent_Cast` claim on that hand names that spell (its spell or its proxy, and a dual-cast claim names both hands). The AI can stream a concentration spell only inside the client's own claim. The instant caster and the voice caster are never touched. A client's direct stream keeps running.

It chooses no spell, aims nothing, equips nothing and fires nothing. A self-delivered area spell (Fire Storm, a cloak) is never guarded, because only a spell fired at an aim has a line of fire. It cannot reach a cast already past its release, such as a projectile in flight. Healing and other friendly spells are never held.

#### Lifetime

Bounded exactly like a cast claim. It dies at the window unless a `Repoint` renews it. A `Repoint` renews the window granted at request, and its `fval` is not re-read. Its `ival` is read, so a heartbeat can change the guards. A `Repoint` that would be refused (an unknown bit, no bit, line of fire while the measurement is not armed) is refused by name and the claim keeps its previous bits. A load releases it.

#### Priority and conflicts

`Cast` sources facet. The source a guard overrules is the engine's own AI. A refusal stands only while the guard claim's client outranks `Engine` there. Defaults are client 50 against engine 20. A would-be refusal that does not stand is logged "would refuse, ALLOWED". Among guard claims on one actor, the highest basis wins, a tie keeps the earliest, and **the winner's bits apply alone**. Bits are not merged across claims.

#### Refusals

At the call (`kInvalidHandle`, logged `[apmf][cast-guard] claim refused`): an APMF older than ABI 21 ("no channel serves intent 25"), the `CheckCast` gate not installed, no param, no guard bit or an unknown bit, a negative or non-finite `fval`, the player as the actor, and `kCastGuard_LineOfFire` while the measurement is not armed. Nothing is refused at the drain.

#### Worked example

**Example 13: Hold one guard per follower and heartbeat it from your service tick**

**Think it through.** This is the pattern in MFO's `CastGuard.cpp`: renew while the claim is live, and ask again once it has gone. The claim is pending until its first live read, so a false read before that is not an end. After a lapse or a load the handle is dead, and `Release` on it is a harmless no-op before the new request.

**Solution.**

```cpp
struct GuardClaim { Handle h = kInvalidHandle; bool seen = false; };

void KeepGuard(RE::FormID follower, bool boundConcentration, GuardClaim& g) {
    if (g_apmf->abiVersion < 21) return;

    APMF_Param p{};
    p.ival = kCastGuard_LineOfFire;
    if (boundConcentration) p.ival |= kCastGuard_ChannelRight | kCastGuard_ChannelLeft;
    p.fval = 6.0f;                                              // seconds. Repoint renews it

    if (g.h != kInvalidHandle) {
        if (g_apmf->IsClaimLive(g.h)) {                         // ABI 6
            g.seen = true;
            g_apmf->Repoint(g.h, &p);                           // heartbeat
            return;
        }
        if (!g.seen) return;                                    // still pending: wait for the first live read
        g_apmf->Release(g.h);                                   // it lapsed or a load dropped it: ask again
        g.h = kInvalidHandle;
    }
    g.h = g_apmf->RequestEx(follower, kIntent_CastGuard, kBasis, &p);
    g.seen = false;
}

void DropGuard(GuardClaim& g) {
    if (g.h != kInvalidHandle) { g_apmf->Release(g.h); g.h = kInvalidHandle; }
    g.seen = false;
}
```

**Check.** Stand between the follower and its foe. The follower charges its attack spell and holds it, and the log shows the 0x06 or 0x0A refusal with the friend's FormID and the distance from the line. Step aside and it fires. With the channel bound, the follower's own Flames never starts while no cast claim names it.

> **Real-World Connection** MFO replaced its own `CheckCast` and `CheckStartCast` hooks with this one claim per follower (MFO 2.3.0, `native/apmf/CastGuard.cpp`): a 6 second window, a `Repoint` heartbeat every service lap, and a fresh request when the handle has gone stale. A guard Harbinger refuses on configuration is struck for the session, since a load does not change the reason.

#### Version, INI and field status

ABI 21 (0.12.0). No key of its own. It rides the cast gate and, for the ray, `bOwnLineOfSight` in `[Sightline]`. In use by MFO since 2.3.0.

**Check Your Understanding: ch.25**

1. A friend stands 60 units from the line, behind a pillar that blocks the ray from the caster. Is the cast held?
2. Two guard claims on one actor: one with `LineOfFire`, one with `ChannelRight` at a higher basis. Which bits apply?
3. Why can the guard not stop a fireball already in flight?

## Equipment

### ch.15 Equipment (`kIntent_Equipment`)

*A client that already holds a weapon in an actor's hand claims this with that weapon's FormID, and the AI can no longer swap a spell or staff into the hand. The channel writes nothing. The gate does the work.*

#### What it is for

Gate the actor's spell and staff re-arm. It is the old "melee versus ranged" lever. A client that already equips and holds a weapon itself claims this with that weapon's FormID, and from then on the actor's AI cannot swap a spell or staff into its hand. The player sees a follower that keeps its weapon out.

#### Arguments

```cpp
APMF_Param p{};
p.form = weaponFormID;                       // gate-only form: engages the channel, makes no engine write
Handle h = g_apmf->RequestEx(actor, kIntent_Equipment, kBasis, &p);
```

**Table 13. ch.15 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a base weapon | any weapon the actor holds | set it | the **probe path**, see the caution |
| `ival` | | reserved for a future hand slot | | |

> **Caution** A claim with `form == 0` is the old test path. Harbinger actively unequips the actor's right-hand weapon and re-equips it on release. That changes persisted inventory state and the re-equip pointer is not saved, so it is not save-safe. Do not hold a no-param ch.15 claim across a save. Do not use the no-param form in a shipped mod. Always pass a form.

#### What Harbinger enforces

With a form set, the channel itself writes nothing to the engine. The enforcement lives in the `CheckShouldEquip` gate (the same one ch.8 and ch.8b ride). While the claim stands, the gate denies any spell or staff re-arm for the actor. Your own equip call is unaffected, and Harbinger never fights you over which weapon is in hand.

**Weapon admission (2026-10-20, ClickUp 86e394134).** While the claim stands, the combat AI also cannot pick a DIFFERENT weapon, shield or torch for a hand your weapon holds. Harbinger answers NO at the same `CheckShouldEquip` seat on the weapon, shield, torch and unarmed-block items (`core/HandBlock.cpp`), so CombatInventory's evaluate never selects the competitor and the AI keeps your weapon itself. Nothing else is suppressed: your weapon is always admitted, a candidate that competes only for a hand your weapon does not hold is untouched, and attacks, blocks and movement are not decisions this seat makes.

**The hand your claim holds** is derived from the weapon, because `ival` stays reserved for an explicit hand discriminator later (no ABI change):

| Claimed form | Hand(s) held |
|---|---|
| two-handed sword or axe, bow, crossbow | both hands |
| one-handed weapon (a staff included) | the hand(s) the actor wears it in now; not worn: the right hand |
| shield, torch | the left hand |
| anything else | none: the weapon rule does nothing |

So a one-hander in the right hand leaves the left hand to the AI (a dual-wield off-hand swap, a shield or a torch still happen there), while a two-hander also keeps a shield or a torch out. Harbinger resolves the claimed form once, on the game thread, when the claim wins; the combat-thread seat reads the worn object first.

Why this seat and not the weapon context nodes the old audit named: the combat tree equips a weapon in the "Equip weapon" leaf of the Melee/Ranged sequence, BEFORE the "Melee Context" node, which wraps only the attacks. A deny there would cost attacks and still let the swap through (Docs/DENY-COMPLETENESS-AUDIT.md row 15).

Not covered: a pick made before the claim was published is still carried out until CombatInventory's next evaluate (ch.15 has no equip-sink step; a ch.17 declaration does); the AI picking a weapon up off the ground (the `AcquireWeapon` subtree, the disarmed path) and the engine's own pick-up auto-equip; any equip from outside the combat AI (a script, the player, another mod). If the actor does not carry your weapon (it is not among the actor's combat inventory candidates), the weapon rule STANDS DOWN for your claim and refuses nothing, so the AI is never left with an empty hand: one `[ch.15 0x0F] ... STAND-DOWN` warning per claim, and a `carried again` line when it comes back (marth's ruling, APMF-B177, 2026-10-20). The check runs at CombatInventory's evaluate.

#### Lifetime

Standing, ended by `Release`. Never hold it on the same actor as a ch.17 claim. On a ch.17 actor the probe path's re-equip is an equip from outside the engine and is refused unless the weapon is in the declared set. The gate-only form is not affected.

#### Priority and conflicts

`Equip` sources facet. Since ABI 22 the gate's refusal is ranked like every other equip deny: it stands only while your priority on `Equip` is above the source beneath the gate (the engine at 20 by default, or a mod hooked there first). With the default priorities nothing changes.

#### Refusals

Only the ones every intent shares.

#### Worked example

```cpp
Handle KeepWeaponOut(RE::FormID actor, RE::FormID weapon) {
    APMF_Param p{};
    p.form = weapon;                          // never leave this zero in a shipped mod
    return g_apmf->RequestEx(actor, kIntent_Equipment, kBasis, &p);
}
```

**Check.** The follower keeps the weapon out through combat. With `EnableEquipGateLog=1` each refused spell or staff re-arm is logged at 0x0F. Each weapon decision is logged as `[ch.15 0x0F] <actor> <class> item=<form> ... claimed=<weapon> hands=<R|L> (<how>) -> NO` / `ADMIT (the claimed weapon)` / `UNTOUCHED (competes only for a hand the claim does not hold)` (per actor and item every 1.5 s, 100 lines/s cap). A claimed hand that refused candidates and was never offered the claimed weapon for 5 s logs one `[ch.15 0x0F] ... hand: ... the claimed weapon itself was NOT offered` warning per actor, hand and claim: that is the over-suppression signal. With `bEquipmentWeaponObserveOnly=1` the refusals are logged `WOULD REFUSE` and the engine's answer stands.

#### Version, INI and field status

ABI 1. Keys in `[HandBlock]`: `bEquipmentWeaponAdmission` (1, the weapon rule) and `bEquipmentWeaponObserveOnly` (0). `EnableEquipDenyComplete` in `[EquipGate]` affects the spell/staff gate it rides. In use by MFO. The weapon rule is built, CI-verified, not yet field-run.

**Check Your Understanding: ch.15**

1. What does a ch.15 claim with a form write to the engine?
2. Why is the no-form path not save-safe?

### ch.17 Equip authority (`kIntent_EquipAuthority`)

*You declare what the actor wears. Harbinger equips the declared set once and refuses every other engine equip of armor, weapons, ammo and torches until you re-declare or release. The player is never subject to it.*

#### What it is for

Take the whole engine-equip facet for an actor. You declare what the actor wears. Harbinger equips that set and refuses every other engine equip of armor, weapons, ammo and torches until you re-declare or release. Outfit re-apply, the AI's weapon and armor choice, combat re-arm and the engine's re-equip after a `RemoveItem` all stop reaching the actor. The player sees a follower in exactly the gear the mod chose, and the game cannot strip or replace it. The player is never subject to it.

#### Arguments

Three calls work together. Gate everything on `abiVersion`.

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

**Table 14. `EquipAuthFlags` (in `param.ival`)**

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
    std::uint8_t reserved[3]; // 0, or (ABI 25) an instance: see "Instances" below
};
```

`kEquipSlot_Default` lets the engine pick. A slot-less one-hander always lands in the right hand and displaces what is there, so a dagger meant for the off hand would evict the sword. Use `kEquipSlot_Right` and `kEquipSlot_Left` for hand-held items only (a one-hand weapon, a shield, a torch). Declare a two-hander, a bow, ammo and body armor with `kEquipSlot_Default`. The same form may appear twice, once per hand, for two identical daggers. Harbinger counts the instances it needs against the inventory and skips, with a log line, any it does not own enough of. An unknown slot value is treated as default and logged once.

**Instances (ABI 25).** An entry can name ONE copy of its item, so a gem-socketed ring is worn rather than a plain ring of the same base form. Write the copy's `ExtraUniqueID` uniqueID into the entry with `SetEquipEntryUid(entry, uid)`. With a uid, the seat admits only that copy. An equip of another copy, a copy with no uid, or no named copy at all (the combat AI and an outfit refresh name none) is off the set, unless another entry declares the same form with no uid. The combat AI's `CheckShouldEquip` admits such a form only while the hand already holds it. Harbinger's equip pass equips exactly that copy, and skips and logs one the actor does not carry (`instance-missing`) or wears in the other hand (`instance-other-hand`). It never puts on another copy instead. Check `abiVersion >= 25` first: an older Harbinger ignores the bytes without a word and any copy satisfies the entry. [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md#abi-v25-equip-instance) has the byte layout and the disassembly evidence.

```cpp
APMF_EquipEntry ring{ ringForm, kEquipSlot_Default, { 0, 0, 0 } };
if (g_apmf->abiVersion >= 25) SetEquipEntryUid(ring, socketedUid);   // the copy MEO socketed
```

`SetEquipScope` narrows the authority to categories.

```cpp
struct APMF_EquipScope {
    std::uint32_t owned;      // categories your declared set is authoritative for
    std::uint32_t denied;     // categories the actor must not equip into at all
    std::uint32_t reserved[2];// must be 0
};
```

**Table 15. `EquipCategory` bits**

| Bit | Value | Items that compete for it |
|---|---|---|
| `kEquipCat_Armor` | 1 | every ARMO without the shield bit |
| `kEquipCat_Shield` | 2 | an ARMO with the shield bit (competes for Left too) |
| `kEquipCat_Right` | 4 | the right hand |
| `kEquipCat_Left` | 8 | the left hand |
| `kEquipCat_Ammo` | 16 | arrows and bolts |
| `kEquipCat_Light` | 32 | a torch (competes for Left too) |
| `kEquipCat_All` | 0x3F | every category |

The default scope is `{ owned = kEquipCat_All, denied = 0 }`, which is the whole facet. A two-hander, a bow or a crossbow competes for both hands. A one-hander with an either-hand slot, declared as default, also competes for both hands, so declare its hand when you own only one. Unknown bits are masked off, logged once per handle, never refused. `scope == nullptr` resets to the default.

#### What Harbinger enforces

When the declaring claim owns the facet:

1. The set is published to the lock-free snapshot the equip seat reads. From the next engine equip on, an item not in the set is refused for this actor (within the categories you own, path-classified and logged).
2. One main-thread pass equips each declared item the actor owns and is not wearing, through the engine's own `EquipObject` (queued, sounds on). It never unequips anything. The engine's own worker displaces whatever occupies a declared item's slot. No re-assert loop follows. The seat is what stops the engine undoing the set.

**Table 16. The seat's verdict for an equip on a claimed actor, in order**

| Case | Verdict |
|---|---|
| competes for a denied category, from ANY route (the player's trade or gift menu, Papyrus or console, an unattributed caller included) | deny, even if in the set. Since 2026-10-13: the player may still trade the item, the follower never equips it. Still ranked: a source you raised above the claim keeps its equip, but only for an equip applied at once. A QUEUED equip (the usual `EquipObject` default) comes back to the seat one actor update later as `path=QueuedApply` and ranks as Engine there, so it is refused even for a raised source. Stricter than the rule, known, not yet fixed |
| script or console equip, without `DenyScript`, not in a denied category | allow (exempt) |
| the player's own menu equip, without `DenyPlayerMenu`, not in a denied category | allow (player agency) |
| competes for an owned category and there is no declaration or the item is in the set | allow |
| competes for an owned category and the item is off-set | deny |
| competes for no owned and no denied category | allow |
| the form is not ARMO, WEAP, AMMO or LIGH | allow (not governed) |
| observe-only mode (`bEquipObserveOnly=1`, an opt-in, or the claim's own bit) | every deny above is logged as `would-deny` and nothing is refused |

Since 2026-10-13 every declared category is enforced by default: weapons, shields, torches, ammo and armor alike, in and out of combat. The same verdict runs in three places. The equip seat refuses an engine equip. The combat AI's `CheckShouldEquip` seat (on the Melee, Ranged, Shield, Torch and OneHandedBlock items) answers NO for a candidate the declaration refuses, so the AI never plans around an item it cannot equip. Out of combat there is no selection seat, and the equip seat alone refuses (outfit re-apply, the AI's own equip commands, `RemoveItem` re-equip, other mods). The equip pass takes off a worn item in a DENIED category (below), unless the source that put it on outranks the claim (the equip seat let that equip stand and remembers its source; an item with no record is ranked as an Engine equip). All three call one function, so they cannot disagree.

Only ARMO, WEAP, AMMO and LIGH equips are governed. A potion, food, a scroll, an ingredient or a book passes untouched, so the AI's own potion use is never refused. Do not declare one. The equip pass skips it and logs why. The deny half compares FormIDs (and, for an entry with a uid, the copy), so a declared item is in the set whichever hand the engine tries. The hand is what Harbinger's own equip pass enforces.

> **Key Concept 8 (declare, do not tick)** Call the declaration when the loadout changes. Each declaration walks the inventory. An item Harbinger queued is held for 3 seconds before it is queued again.

Rules of the road:

- **The set must be wearable at once.** A two-hander and a shield make the engine displace one with the other every pass. Harbinger never picks a winner.
- **Re-declare with a removal.** If you remove or unequip a declared weapon, the engine's replacement is off-set and refused. The actor stays bare until you declare again.
- **An unloaded actor stores the declaration.** The seat denies for it, but the equip pass is skipped and logged. Declare again after it loads.
- **Bound weapons are equips.** A conjured bound weapon equips through the seat and is refused unless its form is in the set.
- **Do not mix ch.15's probe path with a ch.17 claim.**
- **A refused equip is not re-issued.** If the hand block refused a declared weapon because a cast claim held that hand, Harbinger does not equip it again when the hand frees. Declare again after the cast claim releases (backlog item APMF-B64).
- **Harbinger never adds items.** A declared item the actor does not own is skipped and logged.
- **A worn item in a denied category comes off.** Each declaration event (a scope change too) runs the equip pass, and the pass unequips every worn item that competes for a category you deny, in or out of combat. It stays in the inventory. Each removal logs an `UNEQUIP` line. An off-set item in an OWNED category is not unequipped: your declared item displaces it. Observe-only logs `would-unequip` instead.
- **Your claim binds you too.** Your own direct equip of an item your declaration refuses is refused, logged `deny (the claim binds its own client)`. Equip what you declared, or declare first.

> **Key Concept 9 (what `IsEquipAuthorityEnforced` says)** It is true when the seat is installed and `bEquipObserveOnly` is 0, the default. Then every category you declare is enforced. A user who opts in with `bEquipObserveOnly=1` makes it false. A claim's own `kEquipAuth_ObserveOnly` bit is not consulted by it. (`bEquipEnforceArmor`, the 2026-10-06 armor-only switch, is retired and ignored.)

#### Torch ownership (2026-10-20)

The engine decides torches outside combat in one function, the torch updater (AE id 39948, SE 38902, 1.7.104 the same body as AE). The per-actor AI update calls it on every update of every NPC, from one call site. It does two things:

- **The stow.** On every update, while the actor sits, sleeps, uses furniture or has an interaction, it takes a held torch off.
- **The evaluator.** Out of combat, every 5 seconds, it asks whether the light calls for a torch and equips one or takes it off.

The equip seat sees the evaluator's equip (`path=TorchUpdate`) but neither take-off, because unequips are not seated (`kEquipAuth_DenyUnequip` is RESERVED).

marth's ruling: while your claim owns Light, Harbinger blocks every other torch source, the stow included. So for an actor whose winning ch.17 claim owns `kEquipCat_Light`, the torch seat (`core/TorchSeat.cpp`) skips the torch updater entirely. Both the stow and the evaluator stop for that actor. Harbinger equips nothing there. You declared the torch, and the equip pass above equips it.

"Owns Light" means all of these:

- Light is in your OWNED mask and not in your DENIED mask.
- The claim has a declaration (`count != 0`).
- Your priority on `EquipWeapon` is above the Engine source's (20 by default). If the user raised the Engine above you, the updater runs and a `STANDS` line is logged once.
- Observe-only (`bEquipObserveOnly=1` or the claim's own bit) logs `would-deny` once and lets the updater run.

Rules of the road for a torch:

- **Gate the declaration, not the seat.** Declare a torch and own Light only while your torch rule holds. Do not declare one while the actor sits, sleeps or uses furniture. Then no claim owns Light and the engine stows the torch the way it always does.
- **Not owning Light leaves the engine alone.** The updater runs untouched. On an actor with a ch.17 claim, a torch it takes off is logged once per actor per 10 s (at most 30 lines a minute): `engine torch updater UNEQUIPPED`.
- **The log is per streak.** A deny streak logs one line when it starts and one when it ends, with the number of updates it skipped.
- **In combat** the updater only stows. The combat AI's torch choice goes through the `CheckShouldEquip` seat on the Torch item, as before.
- `[EquipAuthority] bTorchDeny=0` removes the seat. The engine then stows and evaluates torches for every actor, and only its torch equips are refused by the equip seat.

#### Lifetime

Standing, no time limit. It ends on `Release`, a higher basis, a load or an unload. Release clears the declaration. The release unequips nothing and re-equips nothing. A declaration on a claim that does not own the facet is stored and takes effect the moment that claim wins. A win is a declaration event and runs the equip pass.

#### Priority and conflicts

`Equip` sources facet. The caller of an engine equip is recovered by a stack scan. Since ABI 22 every equip deny is ranked by priority: an off-set equip is refused only while your priority on `Equip` is above its source. The engine's own paths (outfit re-apply, the combat AI, `RemoveItem` re-equip) and other mods start at 20 and lose to a client at 50. Another client ties at 50 and its equip stands. Your own direct equip does NOT tie with your own claim: since 2026-10-13 a claim binds its owner on this facet, so your own off-set or denied equip is refused like any other. (Only ch.17. Other facets keep the tie.) A Papyrus or console equip under `kEquipAuth_DenyScript` ranks as DirectCalls (75) and stands unless you raise your mod above it. An equip Harbinger cannot attribute is allowed unless `[UnknownSources] Equip=override`. A source that wins logs a `STANDS` line and counts as stood on the menu. Every category is enforced by default (`bEquipObserveOnly=0`).

> **Did You Know?** This is the only facet with a call-site seat. Every engine equip funnels through one non-virtual worker, so there is no vtable slot to chain on. Harbinger patches the worker's two internal call sites, byte-verifies both before writing either, and refuses to install if any other mod touched those bytes. A collision is a missing feature and never a crash.

While a cast claim holds a hand, `[HandBlock]` also closes that hand to weapons, shields and torches. This is on by default and independent of `bEquipObserveOnly`. Since ABI 22 the hand block is ranked the same way: it stands only while the cast claim's client outranks the equip's source on `Equip`.

#### Refusals

At the call (`kInvalidHandle`) while the equip seat is not installed: `bEquipAuthority=0`, VR, an unsupported runtime, a site-verify refusal, or before `kDataLoaded`. A refused claim means keep your own equips. Nothing is refused at the drain.

#### Worked examples

**Example 14: Claim the authority, scope it to the hands and body, and declare a dual-wield loadout**

**Think it through.** This is the call sequence MFO uses. The scope goes before the set so the first declaration already runs under it. A torch or a shield competes for Left, so it is denied unless declared. Ammo is left to the engine. Each hand-held item names its hand, the cuirass is default. The authority needs ABI 9 for the scope.

**Solution.**

```cpp
Handle ClaimEquip(RE::FormID actor) {
    if (g_apmf->abiVersion < 9) return kInvalidHandle;

    APMF_Param p{};
    p.ival = kEquipAuth_None;                         // scripts pass, unequips pass, the player menu passes
    return g_apmf->RequestEx(actor, kIntent_EquipAuthority, kBasis, &p);   // kInvalidHandle: seat not installed, keep your own equips
}

void DeclareLoadout(Handle h, RE::FormID sword, RE::FormID dagger, RE::FormID cuirass) {
    APMF_EquipScope scope{};
    scope.owned  = kEquipCat_Right | kEquipCat_Left | kEquipCat_Armor;   // a torch or a shield competes for Left, so it is denied unless declared. Ammo is left to the engine
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

**Check.** The log shows `SET-EQUIP-SCOPE`, then an `enforce pass` line with `owned=0x..`, then one `[apmf][equip-auth] ... -> equip` line per item not yet worn, each followed by its `[apmf][equip-obs] ... tls=1 verdict=allow`. The follower dual-wields. An outfit refresh afterwards logs `verdict=would-deny` (or `deny` with `bEquipObserveOnly=0`) for the off-set items. [Appendix E](#appendix-e-equip-authority-probe-criteria) is the full checklist.

**Example 15: Find out whether Harbinger enforces at least armor**

**Solution.**

```cpp
bool HarbingerEnforces() {
    return g_apmf->abiVersion >= 8 && g_apmf->IsEquipAuthorityEnforced();
}
```

**Check.** True with the shipped defaults, even while weapons are only observed. Do not read it as "weapons are enforced".

> **Real-World Connection** MFO's `Equip.cpp` claims the authority per follower, scopes it to what MFO's own equip logic owns, and declares with `SetEquipSetEx` so a dagger in the off hand does not evict the sword. Before ABI 8 that was impossible: a slot-less one-hander always landed in the right hand.

#### Version, INI and field status

ABI 7 (first shipped in 0.9.5), 8 adds a hand per item, `kEquipAuth_DenyPlayerMenu` and `IsEquipAuthorityEnforced`, 9 adds the scope, 25 adds an instance per item. Keys in `[EquipAuthority]`: `bEquipAuthority` (1), `bEquipObserveOnly` (0 since 2026-10-13), `bEquipDenyScript` (0), `bTorchDeny` (1, the torch seat, 2026-10-20). `bEquipEnforceArmor` is retired. `bHandClaimBlocksEquip` in `[HandBlock]`. In use by MFO. Every declared category is enforced by default. [Appendix E](#appendix-e-equip-authority-probe-criteria) is now a field check, read with `bEquipObserveOnly=1` if you want it without refusals. Built, not yet battle-tested.

**Check Your Understanding: ch.17**

1. With the shipped defaults, what does `IsEquipAuthorityEnforced` return, and what does that mean for a declared sword?
2. You declare a two-hander and a shield in one set. What happens?
3. A declared dagger was refused by the hand block while a cast claim held the left hand. The cast claim ends. Does the dagger equip?

### ch.26 Equip preference (`kIntent_EquipPreference`)

*A client names the weapons, shields, torches or staves it wants in an actor's hands in combat. Harbinger lifts their score in the game's own combat scoring, so the AI picks them by its own rules.*

#### What it is for

Prefer the items in an actor's hands during combat without fighting the AI. In combat the game scores every weapon, shield, torch and spell about once a second, sorts each group and equips the best of each group while a hand is free. Melee and ranged weapons and attack spells and staves share one group. Shields and torches share another. Harbinger lifts a preferred item's score one step above the best other item of its group, so the AI equips it. The player sees a follower that fights with the weapon the client chose, and that still switches to its bow at range when the bow is what the client prefers.

#### Arguments

ABI 22. `RequestEx`, then `SetEquipPreference` on the handle.

```cpp
APMF_Param p{};
p.ival = 0;                                                    // kEquipPref_Force only after a miss
Handle h = g_apmf->RequestEx(actor, kIntent_EquipPreference, kBasis, &p);

APMF_EquipPrefEntry e[2]{};
e[0] = { swordForm,  kEquipSlot_Right, 0, {} };
e[1] = { daggerForm, kEquipSlot_Left,  0, {} };
g_apmf22->SetEquipPreference(h, e, 2);
```

**Table 14a. ch.26 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `ival` | `EquipPrefFlags` | `kEquipPref_Force` or 0. Unknown bits are logged and ignored | 0 | score lift only |
| `entries[i].form` | base FormID | a weapon (a staff included), a shield or a torch. Anything else is ignored and named in the log | required | skipped |
| `entries[i].slot` | `EquipSlot` | Right, Left, or Default for either hand (a two-hander, a bow) | Default | either hand |
| `entries[i].rank` | `u8` | 0 is the most preferred | 0 | top rank |
| `count` | | up to 8 | | clears the declaration |

A spell is not declared here. A `kIntent_Cast` claim's spell is lifted the same way.

#### What Harbinger enforces

Nothing is denied and nothing is equipped. Each time the game scores a declared item in combat, Harbinger returns the game's own score lifted to 5 percent plus 0.01 above the best other item of its group. An item that already leads keeps its own score. The game still applies its own range check, its own order (a Restore, Ward or buff spell takes its hand before any weapon) and its own hand rules. An item scored 0, such as a bow with no arrows, is never lifted. A hand a cast claim holds keeps only the spell. Out of combat the scoring does not run, so the preference does nothing there.

**Escalation.** Set `kEquipPref_Force` with `Repoint` only after you saw a miss for about 2 seconds in combat. Harbinger does not time it for you. With the bit set, Harbinger also refuses the game's other equips of a weapon, shield or torch into a preferred hand. That refusal is ranked like every equip deny. A source that outranks you keeps its equip, and that is logged at warn and counted as a FORCE ANOMALY. Your own equip into your own forced hand stands too, counted apart and logged at info. Harbinger still equips nothing, so place the item yourself or with ch.17. Clear the bit once the item holds.

#### Lifetime

Standing, ended by `Release`. The declaration goes with the claim however it ends: your `Release`, Harbinger releasing or sweeping the claim, an unload or a load. A declaration on a claim that does not own the facet is stored and applies when that claim wins. A declaration reaches the score seats on the next game frame.

#### Priority and conflicts

`Equip` sources facet. The lift replaces the answer of whatever sits beneath the score seat, the engine at 20 by default or a mod hooked there first, so it applies only while your priority is above that source. The force refusal is ranked against the equip's source. See [Appendix A](#appendix-a-sources-and-priorities-per-facet).

#### Refusals

`kInvalidHandle` with a logged reason: an APMF older than ABI 22, actor 0, the player, a request before the game data loaded, `EnableScoreSteer=0`, or no score seat installed on this runtime.

#### Worked example

```cpp
void OnLoadoutChanged(Handle h, RE::FormID mainHand, RE::FormID offHand) {
    APMF_EquipPrefEntry e[2]{};
    std::uint32_t n = 0;
    if (mainHand) e[n++] = { mainHand, kEquipSlot_Right, 0, {} };
    if (offHand)  e[n++] = { offHand,  kEquipSlot_Left,  0, {} };
    g_apmf22->SetEquipPreference(h, e, n);                     // declare on change, never per tick
}
```

**Check.** In combat the log shows `[ch.26] ... LIFTED item=... best-competitor=... -> ...`, and the `WEAPON-SCORE` line names the lift. `GetEquipPreferenceState` reads `kEquipPrefState_Lifted`. The 30 second `[ch.26] HEARTBEAT` counts lifts, lifts that lost the next pass, and force results.

#### Version, INI and field status

ABI 22. `EnableScoreSteer` (1) and `EnableWeaponScoreProbe` (1) in `[AiCastSeats]`. Built, not yet tested in game.

**Check Your Understanding: ch.26**

1. Your preferred bow is not equipped while the follower stands next to its foe. Is that a miss to escalate?
2. Why does a preferred item with an engine score of 0 never get lifted?

## Packages and travel

### ch.9 Offer package (`kIntent_OfferPackage`)

*The engine asks one question for the actor's package. While the claim owns the facet, Harbinger answers it with your package, and the engine runs it natively.*

#### What it is for

Hand the actor one specific AI package, and let the engine run it natively. This is how a mod holds a travel package or a work package on a follower even when another framework offers its own. The player sees the follower do what the package says. Harbinger does not run the package. The client owns the package and whatever its target resolves to.

#### Arguments

```cpp
APMF_Param p{};
p.form = packageFormID;                      // the TESPackage to offer
Handle h = g_apmf->RequestEx(actor, kIntent_OfferPackage, kBasis, &p);
```

**Table 17. ch.9 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a base TESPackage | any package | required | nothing to offer. Harbinger falls back to the engine's own answer |

#### What Harbinger enforces

It hooks the engine's package question (`Character` slot 0x49, `CheckForCurrentAliasPackage`). The engine's own answer to that question is the first package from the actor's quest aliases whose conditions and schedule pass right now, or nothing. While your claim owns the facet, Harbinger ranks your claim against that answer:

- **The mod is idle.** The engine's answer is nothing (no alias package of any mod is valid for the actor now), or it already is your package. There is nothing to rank against, and the answer is your package.
- **The mod is only following.** The engine's answer is another package, but everything it does is standing companion behaviour (Table 17a). Plain following is not active. Your package is served and nothing is counted against the mod. The `[ch.9-rank]` line names the mod and the kind, for example `owner=018Auri.esp kind=follow -> idle`.
- **The mod is active.** The engine's answer is another package that does something more. Harbinger names its owner: the plugin the package comes from, registered as a `Package` source if it is new, or `Engine` for a package created at runtime. Your package replaces it only when your mod's `Package` priority is strictly higher than that plugin's. Otherwise the plugin's package stands and your claim yields for that evaluation. A tie keeps the follower mod's package. So a follower mod at the same priority keeps the follower only while it is running something beyond plain following. When it goes back to following, waiting, sandboxing or standing guard, or its packages stop being valid, the next evaluation gives the actor your package.
- **Your own package.** When the active package's plugin is your own mod's source, your package is served.

**Table 17a. What counts as active at the 0x49 seat.** Harbinger reads what the package does from its own record: the procedures in its procedure tree (its template's tree when it has none of its own), or its package type when it has no tree. Each procedure is standing, auxiliary or active.

| The package's procedures | Kind in the log | Role |
|---|---|---|
| Follow, Follow To | `follow` | standing |
| Wait, Hold Position | `wait` | standing |
| Sandbox | `sandbox` | standing |
| Sit, Sleep, Eat, Use Idle Marker | `sit`, `sleep`, `eat`, `idle marker` | standing |
| Wander, Keep An Eye On, Done | `wander`, `keep an eye on`, `done` | standing |
| Guard, Patrol | `guard`, `patrol` | standing (a routine post, like wait and sandbox) |
| Travel, Lock Doors, Unlock Doors, Find, Acquire | `travel`, `lock doors`, `unlock doors`, `find`, `acquire` | auxiliary (getting there and opening the way) |
| Escort, Flee, Pursue | as named | active |
| Dialogue, Force Greet, Say, Shout | `dialogue`, `force greet`, `say`, `shout` | active |
| Activate, Use Magic, Use Weapon | as named | active |
| Hover, Orbit, Flight Grab | as named | active |
| A procedure or tree node Harbinger does not know, or a tree it cannot read | `unknown` | active |

How the roles decide the package:

- Any active procedure anywhere in the tree makes the package **active**.
- Otherwise, any standing procedure makes it **standing**. Its auxiliary procedures only carry it there, so the vanilla follower template (Follow, Travel, Wait), FollowPlayer (Follow, Sandbox, Travel), Sandbox (Sandbox, Travel, Unlock Doors) and a package that walks home and then sandboxes all count as standing.
- A tree of auxiliary procedures only, such as a plain Travel package, is **active**: the mod is taking the follower somewhere.
- A package with no tree decides by its type: Follow, Accompany, Eat, Sleep, Wander, Sandbox, Patrol, Guard and Do Nothing are standing. Any other type is active (`other type`).

A scene is not seen here: scenes run through their own seat and are only observed (see the source capture below).

The engine adopts the answer and runs it with real pathing, one `OnPackageChange` each way. If your FormID cannot be resolved, Harbinger falls back to the engine's own answer, never null. It does not pick, validate or interpret the package's content. Each change of the outcome for an actor is logged once as a `[ch.9-rank]` line (idle, standing, active and outranked, active and the claim yields). The source counters on the Packages table (Hits, Blocked, Passed) move once per such change, not on every evaluation, and the package capture below leaves these packages to this seat. The 30 second `[ch.9-redirect] H` heartbeat carries `rankOutranks` and `rankYields` totals.

> **Key Concept 10 (the nudge)** A nudge makes the redirect take within one evaluation. Harbinger calls `EvaluatePackage` once at engage and owner change, and once at release so the framework package resumes at once. The nudge is posted one hop past the point the claim publishes, because a nudge issued earlier re-evaluates against the old snapshot. It is not self-sustaining. If nothing nudges and the engine does not ask, nothing re-asks.

**Spectator hold-off.** While the claim wins and outranks `Engine` on the `Package` facet, the engine's runtime-created Spectator package (an out-of-combat NPC watching a fight) is not created, so a retreating follower keeps its package. The seat is `iSpectatorSeat` in `[PackageGate]` (2 on, 1 log only, 0 off). Built, not yet tested in game.

#### Lifetime

Standing, ended by `Release`. A ch.9 claim never survives a load, since every claim is released at `kPreLoadGame`. What is untested is the engine-side package persistence: how the engine's own package state behaves across a save and load.

#### Priority and conflicts

`Package` sources facet. The 0x49 answer is **enforced by priority** (above): an active alias package is Blocked when your claim outranks its plugin and Passed when it does not. A tie keeps the other mod's package. Scenes stay **observe only**: Harbinger captures a package or scene that runs instead of yours, names its source (the scene's plugin, else the package's plugin, else `Engine` for a runtime package), compares priorities in the log and holds nothing off. The Spectator hold-off is the other enforcement on this facet. Between your own package-offer claims, basis decides. ch.19 Travel files an internal ch.9 claim at your basis, so an outbidding ch.9 claim on the same actor takes the package from your travel leg.

#### Refusals

Only the ones every intent shares.

#### Worked example

```cpp
Handle HoldPackage(RE::FormID actor, RE::FormID package) {
    APMF_Param p{};
    p.form = package;
    return g_apmf->RequestEx(actor, kIntent_OfferPackage, kBasis, &p);
}
```

**Check.** `EnableRedirectLog=1` logs each 0x49 answer that was redirected to your package. If the package the actor runs is not yours, the `[sources]` capture names which plugin's package or scene won, with both priorities.

#### Version, INI and field status

ABI 1. `EnableRedirectLog` and `iSpectatorSeat` in `[PackageGate]`. Proven when nudged: 16 of 16 deck wins, each after an explicit nudge. The first graduated field run engaged 0 of 6 because the nudge fired before the claim published, which is fixed.

**Check Your Understanding: ch.9**

1. What does Harbinger answer at 0x49 when your package FormID does not resolve?
2. Another mod's scene gives the actor a different package while your claim stands. Is it denied?
4. A follower mod's alias package is valid for the actor and both mods sit at `Package` priority 50. Whose package runs when that package is a plain follow, and whose when it is an escort?
3. Why is the nudge posted one hop after publish?

### ch.19 Travel (`kIntent_Travel`)

*Harbinger ships its own travel packages, points one at your destination and offers it through an internal package claim. The engine walks the actor. A leg ends on arrival, combat, a lost destination, a block or a timeout, and the claim stays live so you can read why. A cell leg that arrives keeps holding the actor in the cell until you release or re-point.*

#### What it is for

Walk an actor to a place. The player sees the follower walk to a crate, an enemy, a marker, a cell or a world point under its own travel package, and stop when it gets there, when it enters combat, or when the destination is gone. Sent to a cell, the follower stays in that cell for as long as your claim stands. MFO uses it for loot excursions, deposit trips and town errands.

The whole contract, in the project's words: it goes to the target 50 to 100 units from it, and combat interrupts and cancels the movement. That is all.

#### Arguments

```cpp
APMF_Param p{};
p.form = destinationFormID;     // a REFERENCE or a CELL. Required unless kTravel_ToPosition
p.fval = 0.0f;                  // arrival radius, game units. 0 means 75
p.ival = kTravel_ReleaseOnTargetDead;                      // a TravelFlags word
Handle h = g_apmf->RequestEx(actor, kIntent_Travel, kBasis, &p);
```

**Table 18. ch.19 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, an object reference (REFR or ACHR) or a cell. The record type decides which | need not be loaded or nearby | required | refused at the call, unless `kTravel_ToPosition` is set |
| `fval` | `float`, game units | 50 to 512, clamped and logged. Not used for a cell | 0 | 75 |
| `ival` | `std::int32_t`, `TravelFlags` | the bits below | 0 | the defaults: the death rule, the authored speed |
| `posX/Y/Z` | `float`, game units | read only with `kTravel_ToPosition` (then `form` must be 0) | 0 | a non-zero point without the flag is refused by name |

**Table 19. `TravelFlags`**

| Flag | Bit | Meaning |
|---|---|---|
| `kTravel_ReleaseOnTargetDead` | 0 | names the default. Does not switch anything on. The leg ends if a destination that was alive when targeted dies on the way, set or not. A destination already dead when targeted (a corpse) is a valid place and never ends the leg by death |
| `kTravel_ToPosition` | 1 | ABI 11. The destination is the world point. Harbinger places its own non-persistent XMarker there and walks to it. Needs a runtime where marker placement is verified. A non-finite point is refused |
| `kTravel_SpeedSet` | 2 | ABI 12. Use the speed in bits 3 and 4 for this leg. Without it the package's authored speed (Run) applies |
| `kTravel_SpeedWalk`, `Jog`, `Run`, `FastWalk` | bits 3 to 4 = 0, 1, 2, 3 | ABI 12. The engine's `PreferredSpeed` values. Mask with `kTravel_SpeedMask` |

> **Caution** An older Harbinger stores the gait bits and walks at its authored speed without a word. Gate on `abiVersion >= 12`. Without `kTravel_ToPosition` support (below ABI 11) a zero form is refused, so a point request fails loudly instead of walking somewhere else.

#### What Harbinger enforces

Harbinger ships eight travel package records in `Data/APMF.esl`. It points one at your destination and offers it through an internal ch.9 claim at **your basis**. The engine runs the package natively, with real pathing.

> **Key Concept 11 (how a leg ends)** A leg ends on exactly these: **arrival** (inside the radius of a reference, or the actor's parent cell is the destination cell), **combat** (the actor enters combat), **destination gone** (deleted, disabled, died during travel, or a cell that no longer resolves), **blocked** (ABI 12, the engine held the actor in its Movement Blocked package for 3 seconds of running game time), **stuck** (a two-minute safety net), and **actor gone** (the actor died, or its reference no longer resolves at all).

> **Key Concept 11a (a cell leg holds after arrival, 2026-10-20)** Arrival in a destination CELL does not drop the package. Harbinger keeps offering the same travel package, which stands at its destination, and the actor stays in the cell until you `Release` or `Repoint`. The state reads `kLeg_Arrived` for the whole hold. Combat, Movement Blocked and the safety net do not end a hold. Only the actor's death or loss (`kLeg_ActorGone`), the cell no longer resolving (`kLeg_DestGone`), your `Release` or your `Repoint` do. A reference leg is unchanged: it ends on arrival and the actor goes back to his own package. If a held actor is moved out of the cell by something else, Harbinger logs it once and does nothing (the package has finished its walk and does not walk him back). `Repoint` to the same cell to walk him back. **A held leg outranks your own later package offer.** The hold's internal ch.9 offer is filed at your basis when the leg starts, and at an equal basis ControlMap keeps the EARLIEST claim. So a ch.9 offer your mod files later at the same basis for the same actor (for example a work package at the destination) loses to the held travel package and is never adopted. Release or `Repoint` the ch.19 claim first, then offer your own package.

> **Key Concept 11b (an unloaded actor is not gone, 2026-10-20)** A living actor whose reference still resolves keeps his leg when his 3D unloads. That is what happens the moment he walks through a load door into an interior the player is not in, so a cell leg into an interior arrives (and holds) even though the actor is never seen inside. While he is unloaded, arrival, destination, combat and the safety net are still decided. Movement Blocked is not (an unloaded actor has no movement to block).

> **Key Concept 11c (a blocked or stuck leg keeps the actor, 2026-10-26)** A BLOCKED or STUCK end is still reported as `kLeg_Blocked` / `kLeg_StuckTimeout`, but while your claim stands Harbinger keeps offering the travel package, so the actor is never handed back to his own package. `Repoint` (the same destination included) starts a new leg on a fresh package slot. `Release` hands him back. A stuck actor stands. A blocked one stays on the walk and carries on when the engine lets him: claim ch.1 if you want him still. And when the engine ends a reference leg's walk outside the radius, Harbinger restarts it on a fresh slot when the destination has moved, up to 3 times, and otherwise reports STUCK at once. See [the API reference](APMF_API-REFERENCE.md#kintent-travel-failure-hold).

It claims nothing else for you. No combat target, no casting, no equipment, no aggression, no movement block. It enters no combat, pins no target and fakes no perception. If you want one of those, claim that intent yourself. One facet per intent.

A gait change in a `Repoint` rewrites the package record, but the running package keeps the speed it started with until the package next starts. Harbinger logs that case. Release and request again to change gait mid-walk.

> **Key Concept 12 (a leg ends and the claim stands)** Harbinger never releases a ch.19 claim. When a leg ends it drops only its own package offer (a held cell leg keeps even that, see Key Concept 11a). The claim stays live (`IsClaimLive` stays true) and does nothing until you `Repoint` or `Release`. Read `GetTravelLegState` to learn why the actor stopped, so that an ended leg is never mistaken for a package theft.

**Why the walk stopped.**

```cpp
std::uint32_t GetTravelLegState(RE::FormID actor, APMF_TravelLegInfo* out);   // ABI 12, any thread
```

Set `out->size = sizeof(APMF_TravelLegInfo)` first. The call returns a `TravelLegState` and, when `out` is non-null and `size` covers the 72-byte v12 layout, fills it. It writes nothing into a shorter struct. It is a snapshot read and safe from any thread. The state is not saved. After a load an actor reads `kLeg_None` until a claim is made again.

**Table 20. `TravelLegState`**

| State | Meaning |
|---|---|
| `kLeg_None` (0) | no state held for this actor |
| `kLeg_Pending` (1) | accepted or re-pointed. The leg starts next frame |
| `kLeg_Walking` (2) | the package is offered and the actor is travelling |
| `kLeg_Arrived` (3) | inside the radius (the leg is over and the offer dropped), or inside the cell (the offer is KEPT and the actor is held in the cell until you release or re-point, Key Concept 11a) |
| `kLeg_Blocked` (4) | the engine's Movement Blocked package held the actor 3 seconds. `stallX/Y/Z` say where. `blocker` and `blockerKind` say whether an actor stood in front |
| `kLeg_CombatCancelled` (5) | the actor entered combat |
| `kLeg_DestGone` (6) | the destination is gone |
| `kLeg_StuckTimeout` (7) | the two-minute net elapsed |
| `kLeg_ActorGone` (8) | the actor died or its reference no longer resolves. Before 2026-10-20 an unloaded actor (no 3D) also ended here. It no longer does (Key Concept 11b) |
| `kLeg_Failed` (9) | the leg could not start or be re-pointed. A refused `Repoint` ends the previous leg. `destForm` and `destX..Z` name the refused destination |
| `kLeg_Released` (10) | the claim was released |

`APMF_TravelLegInfo` fields: `size`, `state`, `actor`, `destForm` (0 for a point leg), `destX/Y/Z`, `msInState`, `seq` (one counter that never resets and changes on every state change, so compare for inequality), `stallX/Y/Z`, `blockedMs`, `speed` (0 to 3 = the gait written, or 0xFFFFFFFF = none), `reserved`, `ownerHandle` (the winning ch.19 claim, compare with yours), `blocker`, `blockerKind` (`kBlocker_None`, `Player`, `Teammate`, `Actor`). A read right after a `Repoint` may still show the previous leg's end for a frame, so check `ownerHandle`, `destForm` and `seq`.

**What BLOCKED means.** The engine answers "this actor cannot move along its path" by running a runtime package of type 36, Movement Blocked. A closed lever portcullis produced it for 39 to 45 seconds in the field, while a healthy leg shows it for about a second and then arrives. So Harbinger ends the leg as BLOCKED once it has held for 3 seconds of running game time, checked every 250 ms. Time spent in a menu does not count, because each poll adds at most 500 ms. One poll without Movement Blocked inside a run is tolerated. Two in a row end the run. Harbinger does not fight or deny Movement Blocked. It records the end and lets you decide. At the verdict it looks for a live actor right in front of the stalled one: within 192 units on the ground plane, less than 128 units above or below, inside a 120 degree cone around the actor's facing or around the straight line to the destination. The nearest one is reported in `blocker`.

> **Tip** What to do with a BLOCKED end (marth's loot rules, and good advice for any client). An actor block (`blockerKind` is not `kBlocker_None`): someone is standing in the way and will probably move. Reorder, do not drop. Take another task now and come back to this one later. A static block (`kBlocker_None`): a closed gate, a wall or clutter. The route is closed. Park the task until the world changes (a door or gate opens, `TESOpenCloseEvent`, a lever is used, `TESActivateEvent`, or a cell attaches). Do not use a timer to retry, and do not send the actor home while other reachable tasks remain.

A BLOCKED end also runs a passive gate probe that only writes log lines (`[travel-gate]`, and `[travel-gate2]` with `bGateProbe2=1`). Nothing reads it back and a client must not parse it.

> **Did You Know?** A package's location holds a form pointer or a reference handle and nothing else. There is no coordinate storage in it, which was read off both game binaries. Vanilla's answer is to place an XMarker and point at the marker's reference. You can do that yourself and pass the marker as `form`, or set `kTravel_ToPosition` and Harbinger does it.

**Gait timing.** Harbinger writes the speed into the leg's package record, together with the record's "Preferred Speed" flag (0x2000), which the engine requires before it honours the byte at all. The engine copies the record's speed into the actor's running-package state when the package starts on the actor. A fresh leg therefore walks at the declared gait. `GetTravelLegState`'s `speed` field reports what was written. A `[travel-gait]` log line per gait leg shows the engine's running copy once the package runs.

#### Lifetime

Standing, no time limit. `Repoint(handle, &param)` moves the destination in place with no release and no re-claim. A point leg's marker lives exactly as long as the leg, and is deleted when the leg ends for any reason or on a `Repoint` to a new point. Markers are co-saved, so a save made mid-leg cannot leave one behind. A load releases every ch.19 claim. If a leg ended and you want the same place again, `Repoint` it, since the state read tells you it ended.

A cell leg that arrived HOLDS (Key Concept 11a): its package offer and one of the eight package slots stay taken until you `Release` or `Repoint`. Release it when your event in that cell is over, and the actor goes back to his own package (a follower walks back to the player). A `Repoint` of a held leg starts the new leg on another package record and only then lets go of the held one, so the actor is never handed back to his own package in between. With all eight slots taken that `Repoint` fails (`kLeg_Failed`) and the hold is let go. The claim itself survives the actor unloading: only his death, his reference leaving memory, your `Release` or a load ends it.

#### Priority and conflicts

`Package` sources facet. The internal ch.9 offer is filed at your basis and re-filed if the winning ch.19 claim's basis changes. If another client outbids you on ch.9 for the same actor, your leg loses the package, exactly as if you had claimed ch.9 yourself.

#### Refusals

At the call (`kInvalidHandle`, nothing to clean up): VR, `[Travel] bTravel=0`, `Data/APMF.esl` missing or disabled, `param.form == 0` without `kTravel_ToPosition`, a non-zero point without the flag, and with the flag a non-zero form or a non-finite point or a runtime without marker support.

At the drain (the handle is live and inert, the log says why, **release it**): `param.form` names a record that is neither an object reference nor a cell. This one cannot be synchronous, because deciding a form's record type needs a form lookup and the API does not take the engine's form table off the game thread.

Eight concurrent legs is a hard cap. A package's destination lives on the package record, so two actors walking to two places need two records. A ninth simultaneous leg is accepted at the call, because the slot is found when the leg starts on the game thread. The log then says `[travel-leg] ... REFUSED -- all 8 package slots are in use by other legs`, the leg state reads `kLeg_Failed`, and the claim stays live and inert until you release it.

#### Worked examples

**Example 16: Send a follower to a crate at a walking pace and find out why it stopped**

**Think it through.** Read the leg state before the request and keep its `seq`, so a state with that `seq` is an old leg's end and not this one's. The gait and the state read need ABI 12. Poll from your tick and treat any state at or past `kLeg_Arrived` as the end of this leg, after checking it is this leg (`destForm`, `seq`, `ownerHandle`).

**Solution.**

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

**Check.** `[travel-leg]` lines show the leg start, the slot taken and the end reason. `[travel-gait]` shows the engine's running speed copy. The follower walks, not runs, and `TripOver` reports `arrived` within 100 units of the crate.

**Example 17: Walk to a world point you picked, and move the destination in place later**

**Think it through.** A point leg needs ABI 11, a zero form and the flag. A `Repoint` to a new point places the new marker, re-points the package, then deletes the old marker, so there is no gap.

**Solution.**

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

**Check.** The log shows the marker placed and the leg started, and on `MovePoint` a second marker placed before the first is deleted.

**Example 18: Hold a travel package to a loot target, and stop when you are done**

**Solution.**

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

> **Real-World Connection** MFO's `Excursion.cpp` runs loot trips on this facet: one leg per item, a hold and an idle at the pickup, and the BLOCKED rules above decide whether to reorder or park the task.

#### Version, INI and field status

ABI 10 (the intent, with `Data/APMF.esl`), 11 adds `kTravel_ToPosition`, 12 adds the gait bits, the BLOCKED end and `GetTravelLegState`. First shipped in 0.9.5, 0.9.8 and 0.9.8. Keys in `[Travel]`: `bTravel` (1) and `bGateProbe2` (0, a passive log probe). Needs `APMF.esl` enabled in the load order. In use by MFO. The cell hold and the unloaded-actor rule (Key Concepts 11a and 11b) came after the 2026-10-20 field run in Markarth, where followers sent to an inn walked through the door and straight back out: the leg ended as soon as the door unloaded them, and the dropped offer handed them back to their follow package. Built, CI verified, not yet tested in game. The engine side of the hold (the travel package stays current in its DONE procedure, one `OnPackageEnd`, no restart while the same package is chosen) was read off the disassembly, see `channels/Travel.cpp` `HoldAtCell`.

**Check Your Understanding: ch.19**

1. The leg reads `kLeg_Arrived`. What does `IsClaimLive` say for the travel claim?
2. A ninth leg is requested while eight are walking. What does the call return, and what does the leg state read?
3. The follower is held by a closed portcullis. Which `blockerKind` is reported, and what should the client do?
4. A cell leg reads `kLeg_Arrived` and the player is outside. What keeps the follower in the cell, and what lets him go?

### Place (`kIntent_Place`)

*Put an actor beside a reference, or at a point in an interior cell, with the engine's own move. Once. It works on an actor nobody can see. A one-shot rather than a claim.*

#### What it is for

An off-screen follower should be found where the client says he went: at the shop counter when the player walks in. Walking him there (ch.19) only works while he is loaded. A placement moves him with the move Papyrus `ObjectReference.MoveTo` makes, so it works when he is unloaded and the destination is unloaded too. The client never calls `MoveTo` itself (marth: everything is ported to Harbinger).

#### Arguments

`RequestEx` only (ABI 28). `Request` with this intent is refused (it carries no destination).

```cpp
APMF_Param p{};
p.form = counterMarkerRef;                // a REFERENCE (the actor lands at its position + pos)
                                          //   or an INTERIOR CELL (the actor lands at pos in it)
p.posX = 40.0f; p.posY = 0.0f; p.posZ = 0.0f;   // an offset from the reference, or the point in the cell
p.ival = kPlace_OnlyIfUnloaded | kPlace_RefuseIfPlayerPresent | kPlace_SetYaw;
p.fval = 3.14159f;                        // the yaw in radians, read only with kPlace_SetYaw
Handle h = g_apmf->RequestEx(follower, kIntent_Place, 0.0f, &p);   // basis is not read
```

**Table 20a. Place arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID` | a reference (any kind, an NPC included, not the actor), or an INTERIOR cell | required | refused at the call |
| `posX/Y/Z` | `float`, game units | finite | `(0, 0, 0)` | at the reference itself, or the cell's origin |
| `ival` | `PlaceFlags` | any of the three bits below | 0 | move whatever the player can see, keep the actor's heading |
| `fval` | `float`, radians | finite, read only with `kPlace_SetYaw` | | yaw 0 (north) when the flag is set |
| `basis`, `target` | | | | not read |

**Table 20b. `PlaceFlags`**

| Bit | Name | Effect |
|---|---|---|
| 0 | `kPlace_OnlyIfUnloaded` | Refused (`kPlaceWhy_ActorLoaded`) when the actor has 3D or stands in an attached interior cell (outdoors only his 3D counts). No visible pop |
| 1 | `kPlace_RefuseIfPlayerPresent` | Refused (`kPlaceWhy_PlayerPresent`) when the player's cell is the actor's cell. Indoors also when the destination cell is the player's cell. Outdoors also when the destination is LOADED: its cell attached (anywhere in the player's loaded grid, one cell over included) or the destination reference has 3D. No visible pop at the far end |
| 2 | `kPlace_SetYaw` | Face `fval`. Without it the actor keeps his own heading |

Gate on `abiVersion >= 28`. An older Harbinger refuses the intent ("no channel serves intent 28").

An exterior position is named by a reference in that worldspace plus an offset, never by an exterior cell (refused, `kPlaceWhy_DestExterior`). A reference destination is exactly Papyrus `MoveTo(akTarget, x, y, z)`: the actor goes into the reference's cell and worldspace.

#### What Harbinger enforces

Nothing is switched on, denied or held. On the game thread Harbinger resolves the destination, runs the checks below and calls `TESObjectREFR::MoveTo_Impl` once (verified row `Place.TESObjectREFR.MoveTo`, AE id 56626 / SE 56227, the same body Papyrus `MoveTo`'s queued task runs). For an actor the engine first releases any furniture he is using, then relocates him at the point and heading, in that cell. Then Harbinger reads the actor's position back. Within 1 unit of the point, and for an interior destination in the destination cell, the placement is `Placed`. Anything else is `Failed` (`kPlaceWhy_NotMoved`), logged as an ERROR and never reported as placed. Outdoors a different parent cell is only the `kPlaceInfo_CellDiffers` bit (an unloaded actor may sit in the worldspace's persistent cell).

Under INVARIANTS #0 this is action (k), PLACE AN ACTOR, adopted by marth on 2026-10-19 ("Remember we are porting everything to harbinger, so having MFO do something will just need redoing later."): one engine move for the spot the client declared, on the game thread, never a claim, reported by read-back. It switches nothing on, so there is no deny-completeness exposure (`DENY-COMPLETENESS-AUDIT.md`). What the actor does at his new spot is his own package's business.

> **Did You Know?** Papyrus `MoveTo` does not move anything when it is called. It queues a task on the script VM, and the task's run calls `MoveTo_Impl` with the target's cell and worldspace. For a `Character` the handle argument is never read, which is why Harbinger can name a bare interior cell. Read off all three game binaries.

#### Refusals

At the call (`kInvalidHandle`, one `[place] request REFUSED` line): not installed (VR, a runtime other than exactly 1.6.1170, 1.5.97 or 1.7.104, the self-check row refused), `Request` (no param), actor 0, the player, `form` 0, a non-finite `pos`, `kPlace_SetYaw` with a non-finite `fval`, an unknown flag bit, 128 placements already queued.

On the game thread (state `Refused`, `why` says which, one `[place] ... REFUSED` line):

**Table 20c. `PlaceWhy`**

| Value | Name | Why |
|---|---|---|
| 1 | `kPlaceWhy_ActorGone` | the actor FormID is not an actor in memory |
| 2 | `kPlaceWhy_DestGone` | the destination FormID resolves to nothing |
| 3 | `kPlaceWhy_DestInvalid` | neither a reference nor a cell, a deleted reference, a reference in no cell, or the actor himself |
| 4 | `kPlaceWhy_DestExterior` | an exterior cell |
| 5 | `kPlaceWhy_ActorLoaded` | `kPlace_OnlyIfUnloaded` and he is loaded |
| 6 | `kPlaceWhy_PlayerPresent` | `kPlace_RefuseIfPlayerPresent` and the player is in the actor's cell, in an interior destination cell, or an exterior destination is loaded |
| 7 | `kPlaceWhy_TravelOwned` | another client's ch.19 travel claim owns him (`blocker` = its handle) |
| 8 | `kPlaceWhy_HoldOwned` | another client's ch.1 movement block owns him (`blocker` = its handle) |
| 9 | `kPlaceWhy_NotMoved` | state `Failed`: the move ran, the position did not read back at the point, or an interior placement reads back in another cell |
| 10 | `kPlaceWhy_Error` | state `Failed`: an exception in Harbinger's step |

#### Lifetime

A one-shot. It never enters the control map and holds no facet. The handle labels the request: `IsClaimLive` is false for it, and `Release` and `Repoint` do nothing to it (a queued placement still runs). The outcome is `GetPlaceState(handle, &info)` (`APMF_API_v28`, any thread): `Queued` until the next game-thread pump, then `Placed`, `Refused` or `Failed`. A finished record stays until `GetPlaceState` has returned it once or 30 s of wall time (`kPlaceStateGraceMs`) have passed, whichever is later, then reads `Unknown` (the 128-record cap can still evict the oldest finished one). A load or a new game drops every record, and a placement still queued at that moment never runs. Nothing is saved: the engine saves the actor where he now is.

#### Priority and conflicts

A placement competes with two facets that also decide where the actor is:

- **ch.19 travel** and **ch.1 movement block.** When the winning claim of either on the actor belongs to ANOTHER client, or to no client Harbinger can name, the placement is refused and the log names both clients. Your OWN winning claim does not block it, and Harbinger does not end it: a travel leg of yours carries on from the new spot (release it first if he should stay), a hold of yours holds him there. Basis plays no part.
- Everything else is untouched. A placement in the middle of a fight, a scene or a substrate is the client's decision and the client's consequence.

#### Worked example

**Example 18a: Have an errand follower found at the counter**

**Think it through.** The follower left on foot (ch.19) and is now out of the loaded area. The client wants him at the counter before the player can see in, so it asks only while he is unloaded and the player is elsewhere, and it releases its own travel claim first so the leg does not walk him off again.

**Solution.**

```cpp
Handle PlaceAtCounter(RE::FormID follower, RE::FormID counterMarker, Handle myTravel) {
    if (g_apmf->abiVersion < 28) return kInvalidHandle;          // older Harbinger: no placement
    if (myTravel) g_apmf->Release(myTravel);                      // he stays where he is put
    APMF_Param p{};
    p.form = counterMarker;
    p.posY = -48.0f;                                              // a step behind the marker
    p.ival = kPlace_OnlyIfUnloaded | kPlace_RefuseIfPlayerPresent | kPlace_SetYaw;
    p.fval = 0.0f;                                                // face north, toward the counter
    return g_apmf->RequestEx(follower, kIntent_Place, 0.0f, &p);
}

// Later, from any thread:
APMF_PlaceStateInfo info{};
info.size = sizeof(info);
auto* v28 = reinterpret_cast<const APMF_API_v28*>(g_apmf);
switch (v28->GetPlaceState(h, &info)) {
case kPlaceState_Placed:  /* he is at info.x/y/z in info.cell */ break;
case kPlaceState_Refused: /* info.why; a TravelOwned blocker is info.blocker */ break;
case kPlaceState_Failed:  /* loud in APMF.log; do not pretend he arrived */ break;
default: break;                                               // Queued, or Unknown after the grace
}
```

**Check.** `[place] <h> queued: actor 0x... -> dest 0x...`, then `[place] <h> PLACED actor 0x... at x,y,z yaw ... in cell 0x... (dest 0x..., a reference; was unloaded in cell 0x...)`. A refusal prints `REFUSED` with the reason, a failure prints `FAILED` with where he reads back.

#### Version, INI and field status

ABI 28 (the release after 0.13.1). No INI key. Exactly 1.6.1170, 1.5.97 and 1.7.104, VR refused. Built, CI verified, not yet tested in game. The same engine call is the one that takes the substrate anchor out of its holding cell and home again. Moving an unloaded actor into an unloaded cell, and the read-back right after, are what the first field run must show.

**Check Your Understanding: Place**

1. A placement reads `Placed`, and the client's own travel claim is still live. Where does the actor go next?
2. Why is an exterior destination named by a reference and not by a cell?

## Combat

Seven facets cover combat, and each does one job. They are designed to be claimed together, each as its own handle, because a facet never claims another facet on your behalf.

**Table 21. Which combat facet to claim**

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

*Arbitration only. Harbinger records who owns the combat-target facet and writes nothing. To have Harbinger hold the target for you, claim ch.20.*

#### What it is for

Record that a mod owns the combat-target facet on an actor. It is **arbitration only**. Harbinger writes no target itself. The client writes the combat target with its own mechanism, and the claim makes Harbinger the single arbiter of who owns the facet. If you want Harbinger to hold the target for you, claim ch.20 instead.

#### Arguments

```cpp
APMF_Param p{};
p.form = targetActorFormID;                      // recorded for observability
Handle h = g_apmf->RequestEx(actor, kIntent_CombatTarget, kBasis, &p);
```

`form` is recorded. 0 records nothing.

#### What Harbinger enforces

Nothing. It records the owner and writes no target. A ch.6 claim restores nothing. The target restore belongs to [ch.20](#ch20-target-pin-kintent_targetpin) only.

#### Lifetime

Standing. Ended by `Release`. Nothing to undo, because Harbinger wrote nothing.

#### Priority and conflicts

`Target` sources facet, as a record only. Basis decides between ch.6 claims. Nothing is enforced against a foreign writer for a ch.6 claim.

#### Refusals

Only the ones every intent shares.

#### Worked example

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

#### Version, INI and field status

ABI 1. No key. Field-proven, in production use by MFO.

**Check Your Understanding: ch.6**

1. Does a ch.6 claim put the target back when another mod overwrites it?
2. Which claim does?

### ch.7 Combat action (`kIntent_CombatAction`)

*Deny whole categories of combat behaviour-tree leaves. The tree falls back the way it does when a leaf fails, and everything else in the fight runs natively.*

#### What it is for

Deny named categories of combat behaviour. The player sees a follower that does not attack, or does not cast, while everything else about its combat (movement, blocking, dodging, searching) runs as normal.

#### Arguments

```cpp
APMF_Param p{};
p.ival = kCombatActionCat_Offense;                    // a CombatActionCategory bitmask
Handle h = g_apmf->RequestEx(actor, kIntent_CombatAction, kBasis, &p);
```

**Table 22. `CombatActionCategory`**

| `ival` bit | Name | Leaves denied |
|---|---|---|
| 0 | `kCombatActionCat_Offense` | Attack, AttackLow, Bash, RangedAttack, SpecialAttack, GroundAttack, FlyingAttack, CastImmediateSpell, CastConcentrationSpell, CastShout, PrepareDualCast |
| 1 | `kCombatActionCat_Cast` | CastImmediateSpell, CastConcentrationSpell, PrepareDualCast, CastShout. These four carry both bits |

An all-zero mask denies nothing. It is never a blanket lock. Defense, movement and utility leaves have no category bit and are never denied.

#### What Harbinger enforces

The claim denies exactly the combat behaviour-tree leaves whose classified category bit is set in the winning claim's mask. Harbinger replaces the denied leaf's `act()` and `pop()` as a pair, so the thread's data stack stays balanced, and the tree falls back cleanly the way it does when a leaf fails. Every other leaf runs natively. The channel itself writes nothing.

`kCombatActionCat_Cast` is the "no casting at all" tool. A [ch.8b](#ch8b-cast-kintent_cast) claim does not deny the cast leaves, because they are its delivery mechanism, so do not stack a Cast deny on an actor you also want to cast through ch.8b.

#### Lifetime

Standing, ended by `Release`, a load or an unload.

#### Priority and conflicts

`CombatAction` sources facet. A deny stops a leaf the engine's combat tree chose only while your priority is above `Engine`. Defaults are client 50 against engine 20. Raise `Engine=` in `[CombatAction]` above the client to let the engine win. It is arbitrated separately from ch.23: a ch.7 claim by another mod neither replaces nor is replaced by a leash.

#### Refusals

Only the ones every intent shares.

#### Worked example

**Example 19: Stop a follower attacking and casting while everything else in its combat still runs**

**Solution.**

```cpp
Handle HoldFire(RE::FormID actor) {
    APMF_Param p{};
    p.ival = kCombatActionCat_Offense;           // the cast leaves carry the Offense bit too, see the table
    return g_apmf->RequestEx(actor, kIntent_CombatAction, kBasis, &p);
}
```

**Check.** The follower keeps moving, blocking and searching in the fight and never swings or casts. `[ch.7]` lines count the denied leaves.

The four cast leaves carry both bits, so `kCombatActionCat_Offense` also stops spell casts, and `kCombatActionCat_Cast` alone stops only the casts. No category denies the melee leaves alone.

#### Version, INI and field status

ABI 1. No key. Field-proven (graduated from a live deck probe).

**Check Your Understanding: ch.7**

1. Which mask stops casting and nothing else?
2. Can a ch.7 claim stop the follower from blocking?
3. You hold a ch.8b heal claim and a ch.7 Cast deny on the same actor. What happens to the heal?

### ch.20 Target pin (`kIntent_TargetPin`)

*Each combat update the engine asks its target selectors whom to fight. While the pin stands, Harbinger answers with your target, among the targets the engine already has. The engine fights it with its own AI.*

#### What it is for

Hold the engine's combat target on the one you name. The player sees a follower that keeps fighting the foe the mod chose, with the engine's own attacks, movement, spells and equips. It never starts a fight.

#### Arguments

ABI 13.

```cpp
APMF_Param p{};
p.form = targetActorFormID;                          // REQUIRED. The target actor
p.ival = kTargetPin_OwnLineOfSight;                  // optional, ABI 18. 0 = none
Handle h = g_apmf->RequestEx(actor, kIntent_TargetPin, kBasis, &p);
```

**Table 23. ch.20 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a loaded actor | any actor other than the claimant | required | refused at the call |
| `ival` | `std::int32_t`, `TargetPinFlags` | `kTargetPin_OwnLineOfSight` (1), ABI 18 | 0 | no line test |
| `fval`, `target`, `posX/Y/Z` | | not read | | |

This is a different intent from `kIntent_CombatTarget`, which writes nothing.

#### What Harbinger enforces

> **Key Concept 13 (the pin)** Each combat update the engine asks its target selectors (vtable slot 6) which foe the actor should fight. While the claim stands, Harbinger answers that question with your target, so the engine's own pick never reaches the actor. The answer is replaced only when the engine's selector picked a target (the actor is fighting) **and** your target is one of the actor's combat group's targets.

Otherwise nothing is written and the log says so. A target becomes pinnable once something, the engine or your own ch.21 entry, makes it a combat target of the actor's group. The pin **pauses**, with the claim intact, while the engine has no target or your target is not yet in the group. It resumes when both hold again.

With `kTargetPin_OwnLineOfSight` the pin also pauses while Harbinger's own ray from the actor to the pinned target is not visible and at most 1000 ms old. A bow or staff then shoots at what the actor can see, not at a pinned foe behind a wall. It never ends the pin. The first seat call registers the pair, and until its first verdict (a frame or two) the pin pauses. Refused at the call while the line-of-sight service is not armed. An APMF older than ABI 18 ignores `ival` and pins with no test.

#### Lifetime

Standing, no time limit. Harbinger **releases the claim itself** when the target is dead, disabled, not loaded or no longer resolves, when it was one of the actor's group's targets and is not any more, or when the actor dies. The log names the reason (`pin ended: target dead`), `IsClaimLive` turns false and the handle is dead. Pin again if you want to keep chasing. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

A pin is a **held engagement** (2026-10-12, INVARIANTS #0 (i)). The game losing track of the target no longer ends it: Harbinger keeps the group's own knowledge of where the target is, so the actor chases it the game's way. A held engagement also ends, logged as an `[hold] ANOMALY`, after 20 s with no path progress toward the target (suspended while the target is flying, for example a dragon in the air), or after 20 s within reach without an attack or a cast on it. Since 2026-10-27 the no path progress end counts as not engaged: the actor's combat is ended through the same ranked `StopCombat` as ch.21's range rule and its usual behaviour comes back. `GetClaimEndReason` (ABI 31) says which end it was. With `[EngagementHold] bEngagementHold=0` (or the knowledge seats not installed) the older contract applies and a pin ends on `target lost`.

#### Priority and conflicts

`Target` sources facet. When the selector seat answered with your target and a later write changed it (logged `OVERWRITTEN`), Harbinger names the writer. If the writer is positively identified and your priority is above it, Harbinger puts your pin target back through the engine's own `SetTarget`. A writer that outranks you, or one Harbinger cannot name, stands. `[UnknownSources] Target=override` makes an unnamed write rank at the Unattributed row, and an unnamed write from the claiming mod's own plugin stands for that mod's claim. A segment of the hook chain Harbinger cannot prove is logged `ATTRIBUTION DEFECT` and never enforced. If your client has its own target hook, make it stand down for any actor you have pinned. Harbinger also logs `SOURCE SEAT MISSED` if the actor ends an update aimed elsewhere with the selector never asked.

#### Refusals

At the call (`kInvalidHandle`): an APMF older than 13, VR, an unsupported runtime, `[TargetPin] bTargetPin=0`, a failed address self-check, before `kDataLoaded`, `form` 0 or equal to the actor, the player as the actor, and `kTargetPin_OwnLineOfSight` while the line-of-sight service is not armed. At the drain (the handle is live and inert, release it): `form` is not an Actor. A `Repoint` naming a non-actor, 0 or the actor itself leaves the claim live and inert, logged.

#### Worked example

**Example 20: Pin a foe, with the own-line-of-sight pause when it is available**

**Think it through.** The pin needs ABI 13, the flag ABI 18. The flag is refused when the service is not armed, so set it only when a probe says the service is up. Re-pin in place with `Repoint` when the foe changes.

**Solution.**

```cpp
Handle PinTarget(RE::FormID actor, RE::FormID foe) {
    if (g_apmf->abiVersion < 13) return kInvalidHandle;
    const bool los = g_apmf->abiVersion >= 18 && g_apmf->GetLineOfSight(0, 0, nullptr) != kLos_Unsupported;
    APMF_Param p{};
    p.form = foe;
    p.ival = los ? kTargetPin_OwnLineOfSight : kTargetPin_None;
    return g_apmf->RequestEx(actor, kIntent_TargetPin, kBasis, &p);
}

void RePin(Handle h, RE::FormID newFoe) {
    APMF_Param p{};
    p.form = newFoe;
    p.ival = kTargetPin_None;
    g_apmf->Repoint(h, &p);                      // moves the pin in place
}
```

**Check.** The log shows `FIRST SOURCE DENY` when the pin first replaced the engine's pick, and no `SOURCE SEAT MISSED`. The follower keeps fighting the pinned foe. `pin ended: target lost` means the foe died or unloaded and the handle is dead.

The combined entry and pin recipe is under [ch.21](#ch21-combat-entry-kintent_combatentry).

#### Pinning a hostile NPC (e.g. a taunt)

The actor that holds a ch.20 or ch.21 claim does not have to be a follower. To make a foe fight your follower, claim on the **foe** and name the **follower** as the target. Harbinger serves that claim exactly as it serves a follower's: nothing in the pin seat, the held engagement, the watchdogs, the release paths or the menu asks whether the holder is a teammate (audited 2026-10-12 for MFO's taunt). The foe then fights your follower with its own AI: its own attacks, movement and spells.

**Table 23a. ch.20 and ch.21 when the holder is a hostile NPC**

| Mechanism | What happens for a hostile holder |
|---|---|
| The pin seat | The same. It answers for the foe while the foe is fighting and your follower is one of the foe's group's targets. |
| Follower not a target of the foe's group | The pin pauses. A ch.21 claim on the foe naming the follower, `fval` 0, adds it. On a foe that is already fighting, the entry adds the follower and keeps the foe's current target, so it needs the pin too. |
| The held knowledge | While the pin stands, the foe's whole combat group keeps knowing where your follower is, not only the pinned foe. That is the game's own per-group entry. |
| No progress, arrived but not fighting | The same 20 s windows. A pin released before 20 s never reaches them. A foe that cannot reach the follower for 20 s ends the pin early, logged as `[hold] ANOMALY`, and `IsClaimLive` turns false. |
| Bleed-out drive | If your follower goes down while it is the pinned target, Harbinger walks the foe to it. Release the pin when your follower is downed. |
| Search drive and leash return | Only for an actor with a ch.23 leash. A foe you have not leashed gets neither. |
| The range rule (ch.21 `fval`) | Its "attacking the party" test means the holder, its own group, the player, the player's teammates and actors any of those command, whoever the holder is. For a hostile holder the player half points the wrong way. Leave `fval` at 0 on a hostile holder. |
| `StopCombat` | Only the range rule calls it. With `fval` 0 Harbinger never ends the foe's fight. |
| Release, unload, load, new game | The same as for a follower. Nothing is undone. |
| Ranking | A normal claim on the `Target` facet (ch.20) and the `Combat` facet (ch.21). Who wins against another target mod is the user's `APMF_Sources.ini` setting. |
| The Harbinger page | A served pin credits your mod, like any claim. |
| A mid-fight group merge | It can end the pin (`target no longer a combat target`): see `Docs/REVIEW-BACKLOG.md` APMF-B84. Taunt again if you still want it. |
| Many foes on one follower | Each foe is its own claim. There is no per-client claim limit. The knowledge is kept in every group a holder belongs to, however many foes hold the same follower. |

**Example 20a: A taunt that pins a foe to the taunter for a set time**

**Think it through.** The claims go on the foe, so the handles are keyed by foe. One handle per foe: a second taunter repoints it instead of stacking a new claim. The entry is needed only when the taunter is not yet a target of the foe's group, and its `fval` stays 0. Harbinger ends the pin itself when the foe or the taunter dies or unloads, or on an anomaly. Your tick ends it at the time limit and when the taunter is downed, because a downed pinned target draws the foe to it.

**Solution.**

```cpp
struct Taunt {
    Handle     pin = kInvalidHandle, entry = kInvalidHandle;
    bool       pinSeen = false, entrySeen = false;   // has IsClaimLive ever read true for it?
    RE::FormID taunter = 0;                          // whom the foe is pinned to now
    std::uint64_t untilMs = 0;
};
std::unordered_map<RE::FormID, Taunt> g_taunts;    // keyed by FOE

// A handle that WAS live and is not any more has ended (Harbinger ended it, or a load did).
// One that has never read live is pending, not ended: it may still be repointed.
bool Ended(Handle h, bool& seen) {
    if (h == kInvalidHandle) return false;
    const bool live = g_apmf->IsClaimLive(h);
    if (live) seen = true;
    return seen && !live;
}

bool TauntFoe(RE::FormID foe, RE::FormID taunter, bool taunterInFoeGroup, std::uint64_t nowMs, float seconds) {
    if (g_apmf->abiVersion < 14) return false;
    APMF_Param p{};
    p.form = taunter;                              // the foe's new target
    p.fval = 0.0f;                                 // no engage range on a hostile holder
    auto& t = g_taunts[foe];
    // An ended handle is never repointed: release it and request a fresh claim below.
    if (Ended(t.entry, t.entrySeen)) { g_apmf->Release(t.entry); t.entry = kInvalidHandle; t.entrySeen = false; }
    if (Ended(t.pin, t.pinSeen))     { g_apmf->Release(t.pin);   t.pin   = kInvalidHandle; t.pinSeen   = false; }

    if (t.entry != kInvalidHandle) g_apmf->Repoint(t.entry, &p);    // one more StartCombat, for this taunter
    else if (!taunterInFoeGroup) t.entry = g_apmf->RequestEx(foe, kIntent_CombatEntry, kBasis, &p);

    if (t.pin != kInvalidHandle) {
        g_apmf->Repoint(t.pin, &p);                // already taunted: move it to this taunter
    } else {
        t.pin = g_apmf->RequestEx(foe, kIntent_TargetPin, kBasis, &p);
        if (t.pin == kInvalidHandle) {             // refused at the call: the log says why
            if (t.entry != kInvalidHandle) g_apmf->Release(t.entry);
            g_taunts.erase(foe);
            return false;
        }
    }
    t.taunter = taunter;
    t.untilMs = nowMs + static_cast<std::uint64_t>(seconds * 1000.0f);
    return true;
}

// isDown(taunter): bleeding out, essential-down or dead -- the client's own test.
void TauntTick(std::uint64_t nowMs, bool (*isDown)(RE::FormID taunter)) {
    for (auto it = g_taunts.begin(); it != g_taunts.end();) {
        Taunt& t = it->second;
        if (Ended(t.pin, t.pinSeen) || nowMs >= t.untilMs || isDown(t.taunter)) {
            g_apmf->Release(t.pin);                         // the foe's own selector answers again
            if (t.entry != kInvalidHandle) g_apmf->Release(t.entry);   // stops nothing
            it = g_taunts.erase(it);
        } else {
            ++it;
        }
    }
}
```

**Check.** `[ch.20] 0x<foe> FIRST SOURCE DENY` names the FOE's form id, with no `SOURCE SEAT MISSED`. `[hold] 0x<foe> HELD engagement on 0x<taunter> (via ch.20 pin)` shows the hold. On your release, `[ch.20] 0x<foe> target-pin released (by the client ...)`. A `pin is NOT a combat target of this actor's group` line means the entry is needed.

> **Real-World Connection** MFO's taunt (design `Docs/TAUNT-DESIGN.md` in MFO) is this recipe: one pin per foe for 15 s, the entry only when its probe finds the taunter absent from the foe's group, and a release in the same tick the taunter is downed.


#### Version, INI and field status

ABI 13 (0.9.9), line-of-sight flag 18. `bTargetPin` in `[TargetPin]`. In use by MFO. A field log should show `FIRST SOURCE DENY` and no `SOURCE SEAT MISSED` before anything relies on it.

**Check Your Understanding: ch.20**

1. The actor is not in combat when you pin a foe. What does the pin do, and what is the claim's state?
2. The pinned target dies. What happens to the handle?
3. A script writes a different target while the pin stands. When does Harbinger put yours back?

### ch.21 Combat entry (`kIntent_CombatEntry`)

*One `StartCombat(target)` call, made by Harbinger on the game thread, one frame after the claim applies. It puts the target in the group and nothing more. The engine chooses whom to fight first, and release stops nothing.*

#### What it is for

Start a fight. Harbinger calls the engine's own `Actor::StartCombat(target)` once. The player sees the NPC turn on a foe it had not noticed.

#### Arguments

ABI 14.

```cpp
APMF_Param p{};
p.form = targetActorFormID;                          // REQUIRED. The target actor. The player may be the target
Handle h = g_apmf->RequestEx(actor, kIntent_CombatEntry, kBasis, &p);
```

**Table 24. ch.21 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `form` | `RE::FormID`, a loaded actor | any actor other than the claimant. The player may be the target | required | refused at the call |
| `fval` | `float`, game units | >= 0 | 0 | no engage range |
| every other field | | not read | | |

#### What Harbinger enforces

On the game thread, one frame after the claim is applied, Harbinger calls `StartCombat(target)` once. If the actor was not in combat, the engine builds its combat controller and group with your target as the group's first combat target, if it accepts it. If the actor was already in combat, the engine adds your target to the group's targets and keeps its current target. Everything after that is the engine's own AI.

> **Key Concept 14 (one call, no choice, no stop)** It does not choose whom the actor fights first. The engine's own selector does, among its group's targets. To make the actor fight your target, also claim ch.20 with the same target. It is one call per declaration: Harbinger does not re-enter when the engine ends combat, and does not keep the fight going. A `Repoint` of a live claim, or a new winning claim, makes one more call for its target. Release stops nothing. The fight is the engine's state, and you call `StopCombat` yourself if you want it over.

#### Lifetime

Harbinger releases the claim itself, and `IsClaimLive` turns false with the reason logged, when:

- the engine refused the entry (`StartCombat` returned false: a restrained, unconscious or dead actor, a dead target, the engine's distance test, its identity test against one engine-global actor, an engine flag),
- the entry could not be attempted (the target is not an Actor, or the actor is not loaded, has no AI process or is dead),
- the fight ended (the entry succeeded and the actor now has no combat controller, "combat ended"), or
- the target is dead, disabled, not loaded or unresolvable, or the actor dies, or
- the held engagement ended it (2026-10-12, INVARIANTS #0 (i)): 20 s with no path progress (suspended while the target is flying), 20 s within reach without fighting the target, or, with `fval` above 0, the target farther than `fval` while it is not attacking the party. The range end and, since 2026-10-27, the no path progress end count as not engaged and also end the actor's fight through the game's own `StopCombat`, unless another enemy of the group is still attacking the party, a winning ch.20 pin or ch.21 entry on the actor names a different target, another client's ch.24 claim outranks the entry on the `Combat` facet, or Engine outranks the entry on the `Combat` facet. Then only the claims are released and the fight stays the engine's.

After "combat ended" or a refusal, another claim on the actor naming the same target ends with the same reason. To try again, send a new `RequestEx`. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

#### Priority and conflicts

`Combat` sources facet. Your own entry is a declared decision and passes a ch.22 deny.

#### Refusals

At the call: an APMF older than 14, VR, an unsupported runtime, `[CombatEntry] bCombatEntry=0`, a failed `StartCombat` self-check, before `kDataLoaded`, `form` 0 or equal to the actor, the player as the actor. A target that is not an Actor is found a frame later and ends the claim (not inert: ended).

#### Worked example

**Example 21: Enter combat and pin the same target, watching both handles**

**Think it through.** Two intents, two handles, one target. A handle that has never read live is pending, not ended, so track a `seen` flag per handle. The entry claim ends itself when the fight ends, and release on it stops nothing, so `StopFight` releases both and leaves the fight to the engine.

**Solution.**

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

**Check.** `[ch.21]` logs the `StartCombat` call and its result. `[ch.20]` logs `FIRST SOURCE DENY` once the pin answers. `Ended(f.entry, f.entrySeen)` turns true at "combat ended" or an engine refusal, and the log line names which.

> **Real-World Connection** MFO's `CombatEntry.cpp` and `ReentryDeny.cpp` carry exactly this pending-versus-ended tracking, because an entry that reads false one tick after the request is pending, not refused.

#### Version, INI and field status

ABI 14 (0.9.9). `bCombatEntry` in `[CombatEntry]`. In use by MFO.

**Check Your Understanding: ch.21**

1. The actor is already fighting a wolf when you declare a bandit. Whom does it fight next?
2. You release the entry claim. Does the fight stop?
3. `StartCombat` returned false because the target is too far. What is the handle's state?

### ch.22 Combat re-entry deny (`kIntent_CombatReentryDeny`)

*Every road into combat passes through one engine function. While the window runs, Harbinger makes that function refuse for this actor, before it does anything. It stops nothing already running, so a retreat is this claim plus one `StopCombat` from you.*

#### What it is for

Keep an NPC out of combat for a bounded window. The player sees a retreating follower that the engine cannot pull straight back into the fight.

#### Arguments

ABI 15.

```cpp
APMF_Param p{};
p.fval = 20.0f;                                      // the window in SECONDS
Handle h = g_apmf->RequestEx(actor, kIntent_CombatReentryDeny, kBasis, &p);
```

**Table 25. ch.22 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `fval` | `float`, seconds | 0 to 120. Above 120 is clamped to 120 and logged. Negative or NaN is refused | 0 | 10 seconds |
| `form`, `ival`, `target`, `posX/Y/Z` | | not read | | |

#### What Harbinger enforces

Every way the engine puts an actor into combat (it sees an enemy, it is attacked, an ally is fighting, a script or another mod calls `StartCombat`) goes through one engine function, `Actor::StartCombat`. While the claim holds and its window runs, Harbinger makes that function refuse for this actor, every time, before it does anything (no weapon draw, no equip, no alarm). The actor keeps doing what its package says.

> **Key Concept 15 (it stops nothing)** An actor already in combat is not taken out of it. The recipe for a retreat is: claim this, wait for the claim to read live, then call `Actor::StopCombat` once yourself. From then on the engine cannot pull the actor back in until the window ends. Other actors may still attack it. Nothing is undone when the claim ends, because nothing was written. Your own ch.21 entry is a declared decision and is not denied.

#### Lifetime

The claim ends by itself and Harbinger releases it: "window elapsed", or "owner dead". The window runs from the claim's own request (or last `Repoint`). A claim that takes over from a released rival gets only what is left of its window. `Repoint(handle, &param)` restarts the window from that moment with the new `fval`. Your own `Release`, an outranking claim, a load, a new game or an unload also end it.

#### Priority and conflicts

`Combat` sources facet. The deny refuses a `StartCombat` only while your priority outranks the source that called it (the engine, or the calling mod's DLL). A vanilla Papyrus `Actor.StartCombat` or a console command runs inside the game and is named `Engine` in this release. Raise `Engine=` in `[Combat]` above the client to let the engine win. A `StartCombat` whose caller cannot be named during a deny is allowed by default. `[UnknownSources] Combat=override` makes it rank at the Unattributed row.

#### Refusals

At the call: an APMF older than 15, VR, an unsupported runtime, `[CombatReentryDeny] bCombatReentryDeny=0`, a failed self-check, before `kDataLoaded`, a negative or NaN `fval`, the player as the actor. Nothing at the drain.

#### Worked example

**Example 22: Retreat: deny re-entry first, then `StopCombat` once, on your own tick**

**Think it through.** The deny is pending until the next drain. `StopCombat` must come after the deny is live, or the engine pulls the actor straight back in. So the tick watches for the first true `IsClaimLive`, calls `StopCombat` once, and reports the retreat over when the claim ends. A claim that never applies within a bounded wait is released. The engine call is passed in as a callable because `StopCombat` is the client's own, main-thread call and not part of this API.

**Solution.**

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

**Check.** `[ch.22] seat OBSERVED` shows the seat ran. Each refused `StartCombat` is logged with its caller. `[ch.22] ... DENY MISSED` means the actor entered combat anyway and should be reported.

Where the NPC goes after `StopCombat` is not decided here. If foes keep fighting nearby, the engine hands an out-of-combat NPC its own Spectator package and it stands and watches. To send it somewhere, claim [ch.9](#ch9-offer-package-kintent_offerpackage) with your own package before `StopCombat`, and the Spectator hold-off keeps the engine from handing it the watching package while your claim outranks `Engine`.

#### Version, INI and field status

ABI 15 (0.9.10). `bCombatReentryDeny` in `[CombatReentryDeny]`. In use by MFO, and run in the 2026-09-28 field session (the seat was seen, every deny was logged and none was missed).

**Check Your Understanding: ch.22**

1. You call `StopCombat` in the same tick as the request. What goes wrong?
2. A Papyrus script calls `StartCombat` on the actor during the window. With the defaults, is it refused?
3. The actor's ally is attacked during the window. Does the actor join?

### ch.23 Pursuit leash (`kIntent_PursuitLeash`)

*While the actor is farther than the radius from an anchor, every combat move that would take it farther is refused before it starts. The actor fights what comes to it and is never walked back.*

#### What it is for

Stop an NPC chasing a fight away from an anchor. The player sees a follower that fights what comes to it and does not run off after a fleeing foe.

#### Arguments

ABI 16.

```cpp
APMF_Param p{};
p.target = anchorActorFormID;                        // REQUIRED. Often the player (0x14)
p.fval   = 1500.0f;                                  // REQUIRED. The radius in game units, > 0
Handle h = g_apmf->RequestEx(actor, kIntent_PursuitLeash, kBasis, &p);
```

**Table 26. ch.23 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `target` | `RE::FormID`, a loaded actor | any actor other than the claimant | required | refused at the call |
| `fval` | `float`, game units | positive and finite | required | refused at the call |
| `form`, `ival`, `posX/Y/Z` | | not read | | |

#### What Harbinger enforces

> **Key Concept 16 (the leash test)** While the actor is farther than the radius from the anchor, a combat behaviour-tree move toward its goal is refused when the goal is farther from the anchor than the actor already is, which is to say when the move would take it farther.

The goal is the combat target for the ten pursuit leaves (Advance, Chase, FindAttackLocation, Flank, FlankDistant, MaintainOptimalRange, PursueTarget, Reposition, Stalk, Surround) and the combat group's search centre for the four search leaves (Search, SearchCenter, SearchLocation, SearchWander). A leaf is refused before it starts. One already running is ended the way the engine ends a leaf whose path failed. Every other move and every attack, spell and block runs natively. The actor is never walked back.

#### Lifetime

Standing. Ended by `Release`, an outranking ch.23 claim, a load, a new game or an unload. A dead actor runs no combat tree. A dead, unloaded or other-cell anchor holds nothing back.

#### Priority and conflicts

It is its own facet, arbitrated separately from ch.7. It sits on the `CombatAction` sources facet. A leash stops a leaf only while your priority is above `Engine`. [ch.24](#ch24-combat-approach-kintent_combatapproach) respects a leash on the same actor.

#### Refusals

At the call: an APMF older than 16, VR, an unsupported runtime, `[PursuitLeash] bPursuitLeash=0`, a failed self-check, before `kDataLoaded`, target 0 or the actor itself, a radius that is not a positive number, or the player as the actor. At the drain: an anchor that is not a loaded Actor. The claim stands and denies nothing, and the log says so.

#### Worked example

**Example 23: Leash a follower to the player**

**Solution.**

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

**Check.** The `[ch.23] pursuit H` heartbeat counts the refused leaves. A follower outside the radius stops chasing a fleeing foe and keeps fighting whatever closes on it.

#### Version, INI and field status

ABI 16 (0.9.10). `bPursuitLeash` in `[PursuitLeash]`. In use by MFO.

**Check Your Understanding: ch.23**

1. The follower is 2000 units from the player with a 1500-unit leash, and its target stands 1800 units from the player. Is the Advance leaf refused?
2. Does a leash walk the follower back inside the radius?
3. Can another mod's ch.7 claim replace your leash?

### ch.24 Combat approach (`kIntent_CombatApproach`)

*The engine's combat movement keeps the actor inside its combat area. Centre that area on X with radius R and the engine walks the actor toward X in its combat gait, fighting all the way. Every non-movement read of the area still sees the engine's own.*

#### What it is for

Make an NPC close to within R of an actor while it fights. A healer uses it to walk toward the friend it wants to heal while it keeps attacking and casting. The player sees a follower that moves toward the named actor in its combat gait, with both hands still working.

#### Arguments

ABI 19.

```cpp
APMF_Param p{};
p.target = xFormID;                                  // REQUIRED. The reference to close on
p.fval   = 400.0f;                                   // REQUIRED. R in game units, finite and > 0
p.ival   = kApproach_Hold;                           // optional CombatApproachFlags
Handle h = g_apmf->RequestEx(actor, kIntent_CombatApproach, kBasis, &p);
```

**Table 27. ch.24 arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `target` | `RE::FormID`, a loaded reference. An actor or any loaded reference. The player is allowed | not the claimant | required | refused at the call |
| `fval` | `float`, game units | finite and greater than 0 | required | refused at the call |
| `ival` | `std::int32_t`, `CombatApproachFlags` | `kApproach_Hold` (1): keep the bound after arrival and stay within R until released | 0 | the claim ends on arrival |
| `form`, `posX/Y/Z` | | not read | | |

#### What Harbinger enforces

The engine's combat tree runs two parallel halves, Movement and Action. The Movement half keeps the actor inside its current combat area. Outside it, the engine's own Return To Combat Area branch paths back. Inside it, every movement leaf's destination is clamped to it.

> **Key Concept 17 (the bound)** While the claim is in force, Harbinger centres the actor's own standard combat area on X with radius R. The engine walks the actor toward X in its combat gait, and once inside R it keeps moving tactically within R. The engine also reads the area outside movement (the attack and bash pick, the target selector's score, the invisibility and bound-weapon cast decisions). Every one of those is answered with the engine's own area, not the bound, so no other action is dropped or changed.

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

**Table 28. `CombatApproachState`**

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

#### Lifetime

Unlike ch.19, a ch.24 claim that ends is **released by Harbinger**, so a lower-ranked approach claim can take over. It ends on arrival (unless `kApproach_Hold`), when X is lost, when combat ends after the area was applied, when the engine drops the bound, or when the actor is gone. A client `Release`, an outranking ch.24 claim, a load or a new game also end it. Read the state to learn why.

#### Priority and conflicts

`Combat` sources facet. The bound is written only while your priority is above the engine's own combat area. A ch.24 claim is arbitrated against other ch.24 claims by basis.

#### Refusals

At the call: an APMF older than 19, VR, an unsupported runtime, `[CombatApproach] bCombatApproach=0`, a failed self-check, before `kDataLoaded`, target 0 or the actor itself, a radius that is not positive and finite, the player as the actor. At the drain: a target that is not a loaded reference ends the claim as `TargetLost`.

#### Worked example

**Example 24: Walk a healer to within 300 units of a friend and ask why it stopped**

**Solution.**

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

**Check.** `applied` in the state read is greater than 0 once the engine's area update has carried the bound. The `[ch.24]` heartbeat counts the seats. The healer walks toward the friend in its combat gait and keeps casting on the way.

> **Real-World Connection** MFO's `CombatApproach.cpp` and `OffenseApproach.cpp` use this facet for the healer's approach and for closing on an offense target, and both read `Leashed` as "the leash wins, do not fight it".

#### Version, INI and field status

ABI 19 (0.11.0). `bCombatApproach` in `[CombatApproach]`. In use by MFO.

**Check Your Understanding: ch.24**

1. The claim is made while the actor is out of combat. What state does it read, and what happens when combat starts?
2. Which reads of the combat area still see the engine's own area while the bound is in force?
3. The actor arrives inside R without `kApproach_Hold`. Who releases the claim?

### Combat substrate (`kIntent_CombatSubstrate`)

*An animated cast is the follower's own combat AI casting, and that AI only exists in combat. For an out-of-combat cast Harbinger forces combat as a SUBSTRATE: a controller whose only target is Harbinger's own anchor actor, with every facet that combat switches on denied except the cast leaves. The follower keeps walking under its own package. A real fight drops it at once.*

#### What it is for

marth, 2026-10-12: "Getting animations for everything is non negotiable." Every ch.8b claim seat is a vfunc of a `CombatMagicCaster`, which the engine builds only inside `CombatController::Update`. Out of combat there is no controller, so there is no animated road for a heal on the walk, a buff before a door or a summon. This facet gives the client's [ch.8b](#ch8b-cast-kintent_cast) claim a controller to ride. It is the CLAUDE.md principle "think with portals": combat as a substrate, one facet let through, every other one denied. The design is MFO's `Docs/COMBAT-SUBSTRATE-DESIGN.md`; the legality is `INVARIANTS.md` #0 (j).

#### Arguments

ABI 23. File it beside the cast claim, at any basis (the cast claim's basis decides the cast as always).

```cpp
APMF_Param p{};
p.fval = 6.0f;                                       // TTL in SECONDS (0 = the INI default)
Handle sub = g_apmf->RequestEx(actor, kIntent_CombatSubstrate, kBasis, &p);
Handle cast = g_apmf->RequestCast(actor, kBasis, &req);   // the spell, target, hand: ch.8b as usual
```

**Table 27a. Combat substrate arguments**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `fval` | `float`, seconds | above 0. Above 15 is clamped to 15. Negative or NaN is refused | 0 | `[CombatSubstrate] fSubstrateTtlSec` (6) |
| `form`, `ival`, `target`, `posX/Y/Z` | | not read | | |

#### What Harbinger enforces

The OPEN, in order, on the game thread:

1. The actor must be loaded, alive, enabled, high process, OUT of combat, in no scene, with no dialogue menu open and the player out of combat. The client must outrank `Engine` on CombatAction, Combat, EquipWeapon, Detection and Cast in `APMF_Sources.ini` (every deny below ranks as the client; a deny that would not stand means a facet switched on undenied, so the substrate does not open).
2. The actor enters Harbinger's holder set. The ANCHOR comes out of its holding cell to the actor on the first holder. The anchor is one persistent actor in `APMF.esl`: a field-by-field copy of Nazeem (`Skyrim.esm` `00013BBF`) with no factions, packages, outfit, inventory or script, not Unique, essential and a ghost. Harbinger turns its AI off and makes it invisible, and keeps it a step behind the follower (its collision is left on: the only way to switch it off is an engine flag that has not been verified).
3. Two hand FLOORS: internal deny-only `kIntent_Cast` claims on both hands at the lowest basis, so your cast claim wins its hand and the other hand takes nothing, no weapon, shield, torch or unclaimed spell. They are full floors, not `kCastFlag_FloorSpellsOnly`.
4. Once both floors are published (the next drain), ONE `Actor::StartCombat(anchor)`, the call ch.21 makes. A refusal ends the substrate.
5. The package is re-picked once, and once the game has run that re-pick (a frame or two), the process ignoring-combat bit (`AIProcess+0x139`) is set once. The controller is left unpaused. The actor's own package keeps moving it; the cast is cast from the hands. The order matters: the re-pick restarts the package, and a package start copies the package's own ignore-combat flag (off for a follow package) into the bit.

While it is open:

- **Only the cast leaves and the spell equip run.** Every combat behaviour-tree leaf is force-failed (ch.7's act()/pop() pair) except Cast Immediate, Cast Concentration, Prepare Dual Cast and the two dual-cast variants, and the SPELL EQUIP below, and those only while a DRIVING cast claim stands. The tree's structural nodes (selectors, sequences, repeats, spawn-parallel) pass, because the cast leaves sit beneath them. All 95 node classes the game has are covered, including the parameterised ones (acquire resource, flee, face angle, watch target, follow path, acquire item, land). The two idle waits pass: they decide nothing. Attacks, blocks, potions, every other equip, every movement leaf (close, flank, strafe, flee, search, return to area) are denied.
- **The Movement half is HELD, not failed** (2026-10-25, `core/ActionGate.cpp` "MOVEMENT HOLD"; marth 2026-10-20: "Harbinger should feed it true data and not let it know it was blocked."). The combat tree's root is a Parallel, `Combat Parallel` {Movement, Action}, and its Movement half is a second Parallel, `Movement Parallel`. A Parallel fails as soon as one child fails, and then stops every other child, so a Movement half whose leaves were force-failed killed the Action half on every frame: on 2026-10-20 no substrate spell was ever equipped and no cast leaf ever ran. Now, for a held holder, `Movement Parallel` is entered as an empty Parallel (its own data push, not its start: none of its movement leaves ever enters) and stays RUNNING: Harbinger's seat on its update does nothing while the substrate holds. The Combat Parallel sees a running Movement half and keeps the Action half (the spell equip, then the cast leaves). When the hold ends while the controller lives (a real fight), that node fails once through its own failure exit and the engine rebuilds the tree as it would for any fight. Lines: `[substrate] 0x<f> MOVEMENT HELD (+<ms> ms after the open)`, and at each close `[substrate] 0x<f> WINDOW (<reason>): tree started +<ms> ms after the open, TTL <ms> ran from there (used <ms>) | movement half held <n> time(s), <n> held update(s), released <n> | Combat Parallel entered <n> time(s)` (1 = it kept running). `[CombatSubstrate] bSubstrateMovementHold=0` restores the old failing behaviour (every OPEN logs the hole); a hold that did not arm refuses the substrate. 1.6.1170, 1.5.97 and 1.7.104.
- **The engine's own AI puts the claimed spell in the claimed hand** (2026-10-19, `channels/SubstrateEquip.cpp`; MFO does not equip and neither does Harbinger: no `EquipSpell` call is made). The combat tree reaches a spell cast only through its Action branch: `Acquire Action Resource` > one thread per equipment slot > that slot's equip context (its item is the CombatInventory's selected item for the slot) > `Equip magic` (the EquipSpell leaf) > the Magic tree > the cast leaves. The EquipSpell leaf picks nothing: it equips the context's item into that item's own hand. So two nodes run while a DRIVING cast claim stands: the EquipSpell leaf and the `Acquire Action Resource` instance of AcquireResource (its `Acquire Movement Resource` twin stays denied: the body stays the package's). Without them every holder's Action branch was cut at the resource (the allow-list denied every AcquireResource<2>, and the disassembly puts the "Action" instance at the head of the branch), so no substrate cast could be reached at all. What the AI may select is gated per hand at 0x0F, as before: on the claimed hand only the claim's driven form (its proxy, else the spell) survives; the floored hand admits no spell or staff, and no weapon, shield or torch (the hand-claim block). Every other equip leaf stays denied (weapon, ranged weapon, shield, staff, scroll and shout equips use EquipObject / EquipRangedWeapon / EquipShout), as do the equip pause and drinking. The leaf also draws BOTH hands (the engine's `DrawWeaponMagicHands(true)`), so a client substrate STOWS the hands first: before its entry, Harbinger takes every weapon, shield and torch off both hands (the ch.17 equip road: a queued `UnequipObject` on the main thread) and records each copy by its ExtraUniqueID uid (by form when the copy has none), and the entry waits until they are off (1.5 s, else it does not open). The hand floors keep them off for the window (the equip sink refuses any equip into a held hand, a client's ch.17 re-equip included). At the close, or at once on a real-fight drop, each copy goes back (a queued `EquipObject` of that exact copy) as soon as no cast claim or floor holds its hand: the floored hand on the next frame, the claimed hand when your cast claim ends. Lines: `[substrate] 0x<f> HANDS stowed: L=<form/uid> R=<form/uid>`, `HANDS restored (...)` or `HANDS restore FAILED (...): <why>` (verified worn within 3 s). Dead: dropped with a line (the copies stay in the inventory); unloaded: restored when loaded again; a load or revert drops the record with a line (the copies are in the saved inventory). A client claim that takes over an open dry-run substrate gets the same steps in place (the checks below, the stow, the wait) before the spell equip may run; a dry run never runs it. The substrate does not open when the spell equip could not stand: the 0x0F deny-complete refusal off (`[EquipGate] EnableEquipDenyComplete=0`) or not fully installed, the hand-claim block off, the equip sink not installed, or `Engine` outranking your mod on the Cast facet. Per window: `[substrate] 0x<f> EQUIP allowed: <node> for claimed spell 0x<s> hand <L/R>` once per node, and at the close `EQUIP window end (<reason>): ... REACHED the hand at +<ms>` or a warning `... did NOT reach the hand` with what is in each hand and which of the two nodes ever ran. `[CombatSubstrate] bSubstrateEquip=0` keeps both denied (the substrate then never reaches its cast). Exactly 1.6.1170, 1.5.97 and 1.7.104 (1.7.104 since 2026-10-20: `EquipSpell`'s enter and act and both `AcquireResource<2>` constructions are instruction-identical there).
- **Nobody else can fight the anchor.** Three layers (the SEV-1 of 2026-10-19 added the last two: a teammate who was not a holder fought the anchor in Riften, with no `StartCombat(anchor)` refused).
  - A `StartCombat` against the anchor by anyone but a holder is refused at StartCombat's own target check (the ch.22 seat).
  - **The null-target group join.** A `StartCombat` with NO target and a combat group skips that target check and joins the group, taking its targets. In a substrate group that is the anchor. One engine caller makes exactly this call (1.6.1170 `0x14071C2DD` in `0x14071C020`, 1.5.97 `0x14068848E` in `0x140688270`: no target means it passes its group). At StartCombat's own self check the same seat now refuses a null-target entry into a SUBSTRATE group by an actor that is not a live holder (`[substrate] ... NULL-TARGET GROUP JOIN REFUSED`). A substrate group is one with no target but the anchor that a held holder is in or that lists the anchor; the seat compares the group's ADDRESS with the set the sweep published that frame and never reads the group (it has no reference count and the seat runs on any thread). A join that names a real target is not refused: a real target makes the holder's fight real by design. Self-check row `AnchorGuard.StartCombat.TargetGroupRegs`; 1.6.1170, 1.5.97 and 1.7.104 (StartCombat is instruction-identical there, so the AE offset +0x23 holds; 2026-10-20); `[CombatSubstrate] bSubstrateJoinGuard=0` switches it off.
  - **The sweep** (route-independent: it catches any road, known or not). Every frame while a substrate is open and 10 s after the last one closes, on the game thread, every high-process actor that is not a live holder and whose combat controller targets the anchor, or whose group lists it, is a LEAK: an ERROR line (`[substrate] ANCHOR LEAK`, never hidden), then the anchor is taken out of its group with the engine's `CombatGroup::RemoveTarget` when no held holder shares that group, then the held engagement's ranked `StopCombat` when nothing but the anchor is left to fight. In a group a held holder shares, the anchor stays (removing it would end that holder's substrate) and the leaked actor leaves the group through the ranked `StopCombat`. A real target is never fought for: while one is in the group no `StopCombat` is called. A group with no holder in it still loses the anchor entry (that ends no fight); in a group a held holder shares, the holder drops on real combat and its drop removes the anchor. When the ranked `StopCombat` ends a fight, the package is re-picked as a close does. A leak seen keeps the sweep running. The player is logged, never acted on. A CLOSE runs the same on every non-holder still in the closing holder's group, right after the holder's `StopCombat`.
- **The anchor is never lost.** Its entry in the holder's group stays known (the held engagement's write), so the group never searches.
- **The anchor stands at the holder** whose claimed spell has the shortest delivery (self, then touch, then aimed). A holder farther than its own spell's reach logs a "waits" line; its claim is not cancelled.
- **No battle music** (the music deny, ClickUp 86e3pu939; marth's field report 2026-10-13: battle music started each time the substrate opened and cycled back to exploration as it ended). A combat group decides ONCE whether it can drive battle music: its `CombatGroup+0x15C` is born 0xFF (undecided); the group update, which runs under the group's write lock, writes 0 one second after its first update and then 1 when its strength ratio against its targets is in band (0.5 to 2.0), and never again. CombatManager plays battle music for a group that does not target the player only while that byte is 1. Harbinger writes it INSIDE the engine's own locked update of the group (a combat group has no reference count, so the game thread cannot hold one alive): the held engagement's knowledge seat runs in the group detection pass, which the update calls after taking its write lock and before deciding the music, and the seat acts only when its own thread holds that lock. The pass first runs half a second after the group is built, before the one-second decision. If EVERY member is a live holder and EVERY target is the anchor and the byte is still 0xFF, it writes 0 (`MUSIC DENIED`). A group with any other member or target is never touched (a `music deny WAITS` line, retried on every pass while undecided); a group the engine decided first is never overwritten (a `music deny MISSED` line); a holder that joins a group already denied shares its 0. **The 0 is owed back per holder:** the engine's group merge (StartCombat's join path) copies a decided value into the joining fight's group, so a holder pulled into an ally's real fight carries the 0 with it. Whenever the seat sees a real (non-anchor) target in a group whose member owes a 0 and the group reads 0, it puts 0xFF back (`MUSIC RESTORED`) and the engine decides that fight's music in the same update. A debt lasts while the substrate is open and 30 s after a drop; a normal close forgets it. The byte is SAVED with the group: a group saved mid-substrate that still had a real target is owed back after the load (the anchor is removed and a pure group is ended, as before). Proven on 1.6.1170 and 1.5.97 by six self-check rows (`Substrate.CombatGroup.MusicWriteLock`, `Substrate.CombatGroup.DetectionPassInUpdate`, `Substrate.CombatGroup.MusicUndecided`, `Substrate.CombatManager.MusicPlayTest`, and `CommonLib.BSReadWriteLock.LockForWrite` / `UnlockForWrite` for the lock layout) and the knowledge seat. On 1.7.104 (2026-10-20) the four sites have no id-table row, so they are raw-RVA rows (`0x8185A0`, `0x8185DD`, `0x8187AC`, `0x852FD9`, each `....1_7_104`; the two functions are instruction-identical to 1.6.1170), and Harbinger re-reads their bytes and call targets at install. `[CombatSubstrate] bSubstrateMusicDeny=0` switches it off (every OPEN then logs the hole); a failed row or a missing knowledge seat refuses the substrate.
- **The package it had before stays in charge** (F-6, marth 2026-10-20: "The combat packages movement is blocked and the regular ooc package that they had before remains in charge via harbingers allow deny."; `channels/SubstratePackage.cpp`). A follower whose quest alias (or NPC record) carries an enter-combat override package, every vanilla follower (Sven: `0x00109124` PlayerFollowerCombatOverridePackageExterior), would otherwise be switched onto that package the moment the substrate's controller exists: the engine's package selector swaps its pick for the override after asking slot 0x49, so ch.9 cannot stop it. Harbinger seats the selector's own call (1.6.1170 `0x14065DF83`, 1.5.97 `0x1405CDA1A`, 1.7.104 `0x140670973`) and answers "no override" for a HELD holder only: the selector keeps its own pick, the package recorded just before the entry (or a client's ch.9 offer). A REAL holder and every other actor get the engine's answer, and a holder dropped on real combat is re-picked once so the fight gets the override. Log: `[substrate] 0x<f> combat-override package 0x<p> DENIED -- pre-substrate package was 0x<prev>`, once per package per window, and at each close `package hold ... pre-substrate package 0x<prev> STAYED IN CHARGE` (or what runs instead). A failed address row refuses the substrate; `[CombatSubstrate] bSubstrateOverrideDeny=0` lets the swap run and logs the hole per open.
- **The ignoring-combat bit re-assert (slice S2b) is built but OFF** (`[CombatSubstrate] bSubstrateBitReassert=0`): with it on, each engine reset of `AIProcess+0x139` while held (a package restart, or a step inside the package's procedure tree) is written back and logged (`S2b re-asserted ...`) until 15 s after the open. It switches on only after P6 passes and marth's go.

> **Key Concept 20 (a real fight wins at once)** A target other than the anchor in the holder's group, a hit on the holder by an actor outside the party, or the party in combat is REAL combat. The moment any of them is seen, on whatever thread, every deny above stands down for that holder. The next game-thread pass restores the bit, removes the anchor from the group with the engine's own `CombatGroup::RemoveTarget`, releases the floors and the anchor, and puts the group's music decision back from Harbinger's 0 to 0xFF (only its own 0; the engine then decides the real fight's music as it always does). While the real foe is in the follower's group it calls NO `StopCombat`: the fight goes on and your cast claim is kept. If the group has no target left (the hit or the party's fight came from outside it), that empty fight is ended once through the same ranked `StopCombat` the held engagement uses (marth, 2026-10-13). Harbinger then releases the substrate claim.

#### Lifetime

Harbinger ends the substrate and releases the claim on: the cast claim's first FIRE (fire-and-forget; concentration ends with the claim), the cast claim's end (released, expired, its stop percent), your `Release`, the TTL, the actor's death or unload, and real combat (above). **The TTL runs from the moment the holder's behaviour tree first runs under the substrate** (2026-10-25: the field measured that start 2.7 to 5.1 s after the open, so a TTL counted from the open left 1 to 3 s for the equip and the cast). Until it starts the substrate waits a fixed 10 s floor and then closes (`the behaviour tree never ran within the start floor`). Size your own cast claim's `ttlMs` for the start too: a ch.8b claim's window still runs from its filing, and its end closes the substrate. A close calls `StopCombat` once (the controller is the substrate's), cleans any non-holder still in the holder's group (the sweep's treatment, above), restores the bit, releases the floors, sends the anchor home on the last holder and re-picks the package. Nothing is saved. A load or a new game forgets every substrate; the first frame after a load sends the anchor home and cleans the anchor out of any combat group the save carried.

#### Priority and conflicts

The substrate is an ordinary ranked claim (marth: "Nothing escapes Harbinger"). Its denies rank as its client on CombatAction (the leaves), Combat (the anchor refusal), EquipWeapon (the floors), Detection (the anchor's knowledge) and Cast (the floored hand's spell refusal at 0x0F and the 0x0A cast refusal, load-bearing since the spell equip, review F3 of a0112b24), and it opens only while the client outranks `Engine` on all five. A [ch.24](#ch24-combat-approach-kintent_combatapproach) claim waits through a substrate (it is not a fight), and a [ch.19](#ch19-travel-kintent_travel) walk continues through a HELD substrate (its combat-cancel is denied for `kSubstrateState_Held`; a REAL fight, `kSubstrateState_Real`, still ends the leg with `kLeg_CombatCancelled`). The dry run only opens a substrate on a truly idle teammate: no ch.19 / ch.9 / ch.1 / ch.12 claim from any client, not sitting or in furniture, not in dialogue with the player (not NPC-to-NPC). A [ch.22](#ch22-combat-re-entry-deny-kintent_combatreentrydeny) deny on the actor does not block the substrate's own entry.

#### Refusals

At the call: an APMF older than 23, `[CombatSubstrate] bCombatSubstrate=0` (set by hand, the default is 1), an unsupported runtime (anything but exactly 1.6.1170, 1.5.97 or 1.7.104; 1.7.104 runs it since 2026-10-20 through raw-RVA rows, see `Docs/VERIFIED-ADDRESSES.md` "Combat substrate on 1.7.104"), the ch.22 seat or its target site not installed, fewer than all 95 behaviour-tree node classes with the deny pair (`[PursuitLeash] bPursuitLeash=0` refuses it too, its search leaves are among them), the held engagement's StopCombat not available (`[EngagementHold] bEngagementHold=0`), the anchor records missing, a failed self-check row (the music deny's five rows included, unless `bSubstrateMusicDeny=0`), the player as the actor, a negative or NaN `fval`. At the open (the claim is released, `[substrate] ... NOT OPENED: <why>`): every condition of step 1, or all 16 holder slots in use.

#### Worked example

**Example 29: An animated heal out of combat**

**Think it through.** The substrate gives the cast claim a controller. The cast claim is the same one you would file in combat: the spell, the recipient, the hand. File both; the substrate opens on the next frame, enters one frame later, and closes itself on the fire. Watch `IsClaimLive(sub)`: false means it closed (or dropped for a real fight, in which case your cast claim runs as an in-combat claim).

**Solution.**

```cpp
bool HealOnTheWalk(RE::FormID healer, RE::FormID friendId, RE::FormID heal) {
    if (g_apmf->abiVersion < 23) return false;                     // no substrate: the direct road
    APMF_Param p{};
    p.fval = 0.0f;                                                 // the INI TTL
    const Handle sub = g_apmf->RequestEx(healer, kIntent_CombatSubstrate, kBasis, &p);
    if (sub == kInvalidHandle) return false;                       // off, or refused: logged
    APMF_CastRequest req{};
    req.spell  = heal;
    req.target = friendId;
    req.flags  = kCastFlag_LeftHand;
    const Handle cast = g_apmf->RequestCast(healer, kBasis, &req);
    return cast != kInvalidHandle;
}
```

**Check.** `[substrate] 0x<f> OPEN anchor=0x<ref> entry=true`, the census `[ctcensus] ... VERDICT: FIRED via Restore (the hand charged: ANIMATED`, `[substrate] 0x<f> RELEASING 0x<form>` (0x0B, the release request; since fix/apmf-substrate-release-close the close waits for real release evidence, APMF-B93), then `[substrate] 0x<f> CLOSE reason=fired controller-null=y anchor-home=y` and a `WINDOW` line `closed by the TESSpellCastEvent` (a `RELEASED-UNCONFIRMED` or `FIRED-NOT-RELEASED` warning is a failed window).

#### Is this actor's combat a substrate? (ABI v26)

A substrate holder is in combat as far as the engine and every other mod can see (`IsInCombat`, a combat controller, a combat group). It is not in a fight, and no client may treat it as one: marth, 2026-10-13, "Certainly not. It only exists to facilitate actions." `APMF_API_v26::GetSubstrateState(actor)` answers from any thread, lock-free:

| Value | Meaning |
|---|---|
| `kSubstrateState_None` (0) | no substrate: its combat, if any, is real (also every answer of an APMF older than v26) |
| `kSubstrateState_Held` (1) | its combat is SUBSTRATE-ONLY: not party combat, no combat HUD, no combat style, no combat decisions |
| `kSubstrateState_Real` (2) | its real-combat trigger fired: its combat COUNTS (the substrate drops this frame) |
| `kSubstrateState_Anchor` (3) | this is the anchor itself: never a foe or a target |

Details: [APMF_API-REFERENCE.md#abi-v26-substrate-state](APMF_API-REFERENCE.md#abi-v26-substrate-state).

#### Is the substrate available on this install? (ABI v27)

`APMF_API_v27::IsSubstrateAvailable()` is TRUE only when the substrate installed and is usable here: `bCombatSubstrate` not 0, exactly 1.6.1170, 1.5.97 or 1.7.104 (1.7.104 since 2026-10-20; the API header's comment still says 1.6.1170 or 1.5.97, a byte-shared file this change does not touch), every seat and verified row it needs present, `APMF.esl`'s anchor records resolved. Any thread, one load, settled at `kDataLoaded`. It separates "absent here" (a declared degrade a client may plan around) from "this open was refused" (a logged failure, never a fallback). Details: [APMF_API-REFERENCE.md#abi-v27-substrate-available](APMF_API-REFERENCE.md#abi-v27-substrate-available).

#### Version, INI and field status

ABI 23 (0.13.1); the state query is ABI 26, the availability query ABI 27. `[CombatSubstrate]`: `bCombatSubstrate` (1), `bSubstrateDryRun` (0), `fSubstrateDryRunPeriodSec` (20), `fSubstrateTtlSec` (6), `bSubstrateMoveTrace` (0), `bSubstrateMusicDeny` (1), `bSubstrateBarkDeny` (1, the bark deny below), `bSubstrateAggressorClear` (1, the aggressor clear below), `bSubstrateEquip` (1, the spell equip above), `bSubstrateMovementHold` (1, the movement hold above; 2026-10-25), `bSubstrateProbes` (1, passive log only, round R1: per window a `MOVE-BASELINE` line with speed and distance to the player from 1 s before a dry-run pick, while open and 3 s after the close; a `PROBE after-close` line with the aggressor bit before the entry, at the close, +1 s and +5 s and how long the hands stay drawn; a `PROBE re-pick NOT run` context line; a `PROBE other ignoring-bit reset` line per OTHER reset, at most 8 per window; round R5 adds the running package's procedure fields at each OTHER reset and whether a procedure node began in that frame, a `PROBE package SWAP while held` line per package change while open (at most 4, the procedure type named: R2 found the engine swapping a vanilla follower onto its combat-override package), the pause state and the open menus on the re-pick line, the anchor's collision flag at the open, and one `PROBE TESCombatEvent sink order` line at load), `bSubstrateBystanderCensus` (1, passive log only: while a substrate is open and 10 s after, change-only `[substrate] CENSUS` lines for actors that are not the player, the anchor or a live holder (non-holder teammates included since 2026-10-19) within 2048 u of a holder: combat controller, target, target-is-anchor, in a holder's group, fleeing, combat-detected, aggressor bit, alarm package; an actor whose controller targets the anchor is logged at any distance and keeps the window open; capped per window), `bSubstrateJoinGuard` (1, the null-target join refusal above), `bSubstrateEntryTrace` (1 while testing, passive log only: while a substrate is open, a change-only `[substrate] ENTRY StartCombat` line per actor for every StartCombat the self-check seat sees, with StartCombat's caller, its target and group, and whether that group is a substrate group; 40 lines per 10 s). **Music probe (ClickUp 86e3pu939):** every BSMusicManager track change is logged (`[substrate] MUSIC 0x<old> -> 0x<new>`) while a substrate is open and for 15 s after the last one closes, and each substrate logs the FIRST decided state of its group's music byte and who decided it (`group music state DECIDED = ... by the ENGINE / Harbinger's music deny`); the OBSERVED line carries the track at open and the number of changes. Slice S2a: built, NOT yet tested in game, off by default. The field order is fixed: the P3 dry run (the deny table with numbers), P6 (the package moves the body while the substrate is open), then the client (MFO slice S3) and P4 / P5. Slice S2b (re-asserting the bit every frame) is built only if P6 passes. **2026-10-20 (F-6):** the combat-override deny is built (`bSubstrateOverrideDeny`, 1); S2b is built and OFF (`bSubstrateBitReassert`, 0) until P6 passes.

**The combat-start bark is denied (round R5, 2026-10-20, marth's ruling of 2026-10-19):** the line a follower says as it enters combat ("NormalToCombat" or "NormalToAlert") is requested on the new controller's first update. The engine itself skips that line while the group's timer for it is running, so Harbinger sets the holder group's two timers to run for the substrate's TTL plus 2 seconds, in the holder's own combat update just before that first update. When a real target shows up in the group, Harbinger puts the timers back if they still hold its values. Each close logs a `BARK DENY result` line: when the timers were set, what the first detection pass read, and whether the engine changed them (which would mean the line was said). What earlier versions of this page called the "combat-start topic" (StartCombat's own block, which runs because the bit is off at entry) says no line: it notifies the actor's scene and clears a head-track slot, and the substrate never opens on an actor in a scene. **The aggressor bit is cleared (round R5):** StartCombat marks the holder as the one who attacked (`AIProcess+0x136` bit 2), which bystanders' help and murder decisions read. Harbinger clears it once, through the engine's own setter, right after the entry. It never clears it again: a later 1 comes from a real event (a script starting combat, or a join), and the bit is then the truth. `[CombatSubstrate] bSubstrateBarkDeny=0` / `bSubstrateAggressorClear=0` turn either one off (each OPEN logs the hole). On 1.7.104 both run through byte-verified raw rows. If the holder is dropped into a real fight before Harbinger could put the timers back, the put-back is owed and paid inside the game's own locked update of the group; a debt that runs out unpaid is logged (at most the TTL plus 2 seconds). **The bark deny rides TargetPin's combat-update layer**, so `[TargetPin] bTargetPin=0` refuses the substrate unless `bSubstrateBarkDeny=0`.

**Known hole, approved by marth 2026-10-12** (logged once per substrate): the combat state event (`OnCombatStateChanged` still fires for other mods' scripts). Round R5 builds no filter for it, only a passive probe that logs the event's listener order at load (a filter would have to sit ahead of the Papyrus VM's listener). The group JOIN against the anchor (StartCombat's third argument), once recorded here as not caught, is closed since 2026-10-19 (the null-target join refusal and the sweep, above). **Not instrumented:** the design's P3 field `ignoring-readers=` (who else reads the ignoring-combat bit: eight engine call sites per runtime). Counting them needs eight raw call-site patches per runtime, which `INVARIANTS.md` #17 forbids; the OBSERVED line says so. It needs an amendment or the RS2 research pass.

**Check Your Understanding: Combat substrate**

1. You file the substrate but no cast claim. What does the follower cast?
2. A wolf attacks the follower while its substrate is open. Which deny stands down first, and is `StopCombat` called?
3. Why does the substrate refuse to open when `Engine` outranks your mod on the EquipWeapon facet?

## Read-only queries (no claim)

*A query is a question, not a claim. Harbinger answers and the client decides. Nothing is held, nothing is written, and there is no handle.*

These are questions a client asks before it declares something. They claim no facet, hold nothing, write nothing into the world or the control map, and have no handle. A query is not an intent, and there is no combined query-and-act call.

### Space queries (`FindEmptySpace` and `FindHostilesInSpace`)

#### What they are for

`FindEmptySpace` answers "where is an empty, standable spot over there?". `FindHostilesInSpace` answers "which hostile actors are inside this sphere?". They serve a position cast, a travel point, or anything else that has to pick a point.

#### Arguments

ABI 11. **True main thread only**, the one that runs the player's `Actor::Update`. From any other thread they do nothing and return `kQuery_NotMainThread`. SKSE's `AddTask` is not that thread.

```cpp
std::uint32_t FindEmptySpace(const APMF_SpaceQuery* q, APMF_SpaceResult* out);
std::uint32_t FindHostilesInSpace(const APMF_HostileQuery* q, RE::FormID* outActors,
                                  std::uint32_t capacity, std::uint32_t* outCount,
                                  std::uint32_t* outTotal);
```

> **Key Concept 18 (the size rule)** Every query struct starts with `size`. Set it to `sizeof` the struct. Harbinger refuses an input below the layout it knows (`kQuery_BadArgs`) and never writes an output past the size you declared. A later ABI may append fields and an older caller keeps working.

**Table 29. `APMF_SpaceQuery` (44 bytes)**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `size` | `std::uint32_t` | `sizeof(APMF_SpaceQuery)` | required | refused, `kQuery_BadArgs` |
| `origin` | `RE::FormID`, a loaded reference, actor or not | its cell supplies the havok world. If it is an actor, the rays skip its capsule | required | `kQuery_NoOrigin` |
| `originX/Y/Z` | `float`, game units | a feet-level point, read only with `kSpaceFlag_OriginIsPoint` | 0 | the reference's own position |
| `heading` | `float`, radians, Skyrim's angle Z (forward is (sin h, cos h)) | read only with `kSpaceFlag_UseHeading` | 0 | the reference's facing |
| `distance` | `float`, game units | greater than 0. Clamped to 2048 and logged | required | `kQuery_BadArgs` |
| `clearance` | `float`, game units | 16 to 512, clamped | required | `kQuery_BadArgs` |
| `maxDrop` | `float`, game units | up to 1024, clamped | 0 | 128 |
| `flags` | `SpaceFlags` | `kSpaceFlag_OriginIsPoint` (1), `kSpaceFlag_UseHeading` (2), `kSpaceFlag_ClampToWall` (4) | 0 | none |
| `reserved` | | must be 0 | | |

`APMF_SpaceResult` (24 bytes): `size` (caller sets), `status` (the same value the call returns), `x`, `y`, `z` (the ground point, valid only on `kQuery_Ok`), `detail` (the collision layer that stopped a ray, or the occupying actor's FormID for `kQuery_Occupied`, else 0).

**Table 30. `APMF_HostileQuery` (24 bytes)**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `size` | `std::uint32_t` | `sizeof(APMF_HostileQuery)` | required | refused |
| `side` | `RE::FormID`, a loaded actor | the actor whose side we are on | required | `kQuery_NoOrigin` |
| `x`, `y`, `z` | `float`, game units | the sphere's centre, in the side actor's cell or worldspace | required | the origin of the world, which is not what you want |
| `radius` | `float`, game units | greater than 0. Clamped to 4096 and logged | required | `kQuery_BadArgs` |

`outActors` receives up to `min(capacity, 64)` FormIDs (`kMaxHostileResults`), **nearest first**. `*outCount` is how many were written. `outTotal` (may be null) is how many matched before the cap, so you can tell a full sphere from a truncated list. The test is the engine's own `Actor::IsHostileToActor`, asked from each candidate (the same call Papyrus makes). Candidates are every loaded actor plus the player, in the side actor's worldspace (indoors, its cell), alive, enabled, not the side actor, with a 3D distance to the centre of at most `radius`. Harbinger keeps no faction list.

#### What `FindEmptySpace` does

A ray at the origin's chest height (feet plus 64) out to the point, then a ray straight down from there to feet minus `maxDrop`. The ground must be static, terrain or ground. Then the clearance: eight level rays of `clearance` length at the point's knee height (plus 32) must hit nothing, four ground rays at the clearance ring must land within 48 of the point's height, and no live actor may stand within `clearance` plus 32. At most 14 ray casts plus one pass over the loaded actors. Every failure names itself and nothing is retried or relaxed, except that `kSpaceFlag_ClampToWall` shortens a walk that meets a wall to (hit minus clearance) instead of returning `kQuery_Blocked`.

**Table 31. `QueryStatus`**

| Status | Value | Meaning |
|---|---|---|
| `kQuery_Ok` | 0 | answered |
| `kQuery_NotMainThread` | 1 | called off the main thread. Nothing was done |
| `kQuery_BadArgs` | 2 | a null pointer, a short `size`, or a non-positive distance or radius |
| `kQuery_Unsupported` | 3 | VR, an unverified runtime, or before `kDataLoaded` |
| `kQuery_NoOrigin` | 4 | the origin or side is not a loaded reference (an actor, for side) |
| `kQuery_NoWorld` | 5 | the origin has no attached cell or no havok world |
| `kQuery_Blocked` | 6 | geometry or an actor between the origin and the point. `detail` is the layer |
| `kQuery_NoGround` | 7 | nothing to stand on within `maxDrop` under the point |
| `kQuery_NotGround` | 8 | the ground ray hit something that is not static, terrain or ground |
| `kQuery_Occupied` | 9 | a live actor stands inside the clearance. `detail` is its FormID |
| `kQuery_NoClearance` | 10 | geometry inside the clearance radius |
| `kQuery_Ledge` | 11 | the clearance ring is not level |
| `kQuery_Failed` | 12 | an exception was caught. Nothing was returned |

Every non-Ok answer is one `[space] ...` warning line naming the reason.

#### Lifetime, priority and conflicts

Nothing persists. None.

#### Worked examples

**Example 25: Where can a summon-sized thing stand, 180 units in front of this follower?**

**Solution.**

```cpp
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
```

**Example 26: Which hostiles, nearest first, are inside 300 units of that point?**

**Solution.**

```cpp
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

**Check.** A non-Ok status is one `[space]` warning in the log naming the reason. `kQuery_NotMainThread` means the call ran on a worker, and the fix is a main-thread hop, not a retry.

#### Version, INI and field status

ABI 11 (0.9.8). No key. Built and CI verified.

**Check Your Understanding: Space queries**

1. What does `FindHostilesInSpace` return in `*outCount` and `*outTotal` when 70 hostiles match and capacity is 16?
2. What does `kSpaceFlag_ClampToWall` relax, and what does it not?

### Line of sight (`GetLineOfSight`)

#### What it is for

Harbinger's own answer to "can this actor see that one?". It is a physics ray test, never the engine's cached `HasLineOfSight`, which for an NPC returns a stale or false answer for targets the engine does not keep warm (allies, the player). It is the same ray the `kCastFlag_OwnLineOfSight` and `kTargetPin_OwnLineOfSight` flags use.

#### Arguments

ABI 18. Any thread, lock-free. It never blocks and never casts a ray itself.

```cpp
std::uint32_t GetLineOfSight(RE::FormID viewer, RE::FormID target, APMF_LosInfo* out);
```

> **Key Concept 19 (asking keeps it warm)** The call reads the newest stored measurement and marks the pair as asked for. The main thread measures asked-for pairs. So the first call for a pair returns `kLos_Unknown`, and a call a frame or two later has the verdict. Keep asking while you need it. A pair nobody asks for in 2000 ms is dropped.

**Table 32. `LosVerdict`**

| Verdict | Meaning |
|---|---|
| `kLos_Unknown` (0) | never measured, or the newest measurement is older than 1000 ms. Not seen. Ask again |
| `kLos_Visible` (1) | the own ray reached the target |
| `kLos_Occluded` (2) | every ray was blocked, or the target is in another havok world |
| `kLos_Unavailable` (3) | the ray could not be cast. `why` says why |
| `kLos_Unsupported` (4) | the service is not armed (VR, an unsupported runtime, a self-check refusal, `bOwnLineOfSight=0`, or before `kDataLoaded`). Keep your own test |

`APMF_LosInfo` (24 bytes): `size` (caller sets), `verdict`, `ageMs` (milliseconds since the stored measurement, 0xFFFFFFFF = never), `occludedRun` (consecutive OCCLUDED measurements of this pair, 0 after a VISIBLE), `sample` (the clear ray: 1 feet, 2 torso, 3 head, 0 none), `why` (a `LosWhy`: `BadArgs` 1, `ViewerNotLoaded` 2, `TargetNotLoaded` 3, `NoWorld` 4, `NoController` 5, `OtherWorld` 6, `TableFull` 7, `PickSkipped` 8). Pass `out == nullptr` for a verdict only.

#### What the ray is

From the viewer's eye point (the engine's own `GetEyeVector` origin). There is **no view cone and no facing test**. A target behind the viewer is as visible as one in front. Line of sight is geometry, not attention. It goes to three points on the target: feet plus 16, 55 percent and 90 percent of its height. Each ray ends short of its point by the target's own size, so a giant's own body never blocks it. It runs on the character-controller collision layer in the viewer's own collision group. Walls, closed doors and tent walls block. An open doorway, a railing under the line or a gate's gaps do not. **Another actor's body on the line blocks that ray.** It is VISIBLE when any of the three is clear, OCCLUDED only when all three are blocked. A target in a different havok world is OCCLUDED with `kLosWhy_OtherWorld`. A ray that cannot be cast is UNAVAILABLE, never VISIBLE.

Timings: a pair is re-measured at most every 250 ms (`kLosRefreshMs`), at most 8 pairs per frame, 3 rays each. Main-thread cost is bounded whatever the clients ask.

> **Tip** `GetLineOfSight(0, 0, nullptr)` returns `kLos_Unsupported` when the service is not armed, before it reads any argument, and otherwise a bad-args verdict that is not `kLos_Unsupported`. MFO uses it as a free, no-ask probe.

#### Lifetime, priority and conflicts

Nothing persists. No pair or noise baseline crosses a load. None. Heal casts look past friends in this ray in the next release, see the note under [own line of sight](#own-line-of-sight-kcastflag_ownlineofsight). `GetLineOfSight` itself stays strict.

#### Worked example

**Example 27: A tri-state "can see" with a borderline guard**

**Think it through.** One OCCLUDED reading can be a borderline frame. Two in a row (`occludedRun >= 2`) is a no. The first ask is `kLos_Unknown` and should be treated as unknown, not as no.

**Solution.**

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

**Check.** The first call for a pair returns `Unknown`, the next frame's call has the verdict. `ageMs` in the info tells you how fresh it is.

#### Version, INI and field status

ABI 18 (0.11.0). `bOwnLineOfSight` in `[Sightline]`. Used by MFO, whose Sightline recipe it is.

**Check Your Understanding: Line of sight**

1. Why does the first call for a pair return `kLos_Unknown`?
2. The target stands behind the viewer. Is it visible?
3. Which threads may call `GetLineOfSight`?

### Awareness (`SenseActor`)

#### What it is for

"Does this actor sense that one?" It serves an engage-on-sight gambit. It is omnidirectional (no view cone, no facing anywhere) and multi-sense, so a client can give a follower the appearance of more senses than sight. Harbinger decides nothing about hostility, faction or crime. That is the client's call. The engine's own detection (`RequestDetectionLevel`) is deliberately not a sense, because its sight half is the facing-limited test this replaces.

#### Arguments

ABI 18. **True main thread only** (it casts the sight ray). From any other thread it returns `kQuery_NotMainThread` and does nothing.

```cpp
std::uint32_t SenseActor(const APMF_AwarenessQuery* q, APMF_AwarenessResult* out);
```

**Table 33. `APMF_AwarenessQuery` (32 bytes)**

| Field | Type | Range | Default | 0 means |
|---|---|---|---|---|
| `size` | `std::uint32_t` | `sizeof(APMF_AwarenessQuery)` | required | `kQuery_BadArgs` |
| `viewer` | `RE::FormID`, a loaded live actor | the one who senses | required | `kQuery_BadArgs` |
| `target` | `RE::FormID`, a loaded actor | not the viewer | required | `kQuery_BadArgs` |
| `anchor` | `RE::FormID`, a loaded actor | an optional second listener, for example the player the viewer follows. Hearing and engaged also count it. Sight and proximity ignore it. Not loaded is treated as none | 0 | none |
| `sightRange` | `float`, game units | up to 8192, clamped | 0 | 4096 |
| `hearRadius` | `float`, game units | up to 8192, clamped | 0 | 1536 |
| `proximityRadius` | `float`, game units | up to 2048, clamped | 0 | 256 |
| `flags` | `AwareFlags`, senses to **skip** | `kAware_NoSight` (1), `NoHearing` (2), `NoProximity` (4), `NoEngaged` (8) | 0 | evaluate all four |

**Table 34. The four senses (`AwareSense` bits in the result)**

| Sense | Bit | Fires when |
|---|---|---|
| `kSense_Sight` | 1 | the own ray is VISIBLE and the target is within `sightRange` |
| `kSense_Hearing` | 2 | a new engine noise by the target in the last 3000 ms (`kAwareNoiseWindowMs`), level above 0, within `hearRadius` of the viewer or the anchor, or the target is in combat within `hearRadius` of either |
| `kSense_Proximity` | 4 | the target is within `proximityRadius` of the viewer, whatever is between them |
| `kSense_Engaged` | 8 | the target is in combat and its current combat target is the viewer or the anchor |

Hearing by noise is novelty-based. The first time a query names a target, Harbinger records its current noise as a baseline and hears nothing from it (`kAwareDetail_NoiseBaseline`). A noise counts only when Harbinger saw that record change. A target in a different cell space (interior against exterior, another interior) senses nothing and sets `kAwareDetail_OtherSpace`. The sight test measures now unless the pair was measured less than 250 ms ago (reused), or the frame's cap of 8 synchronous measurements is spent. In that case a fresh stored verdict answers, else sight is not sensed this call (`kAwareDetail_SightDeferred`) and the pair is queued.

`APMF_AwarenessResult` (48 bytes): `size` (caller sets), `status` (a `QueryStatus`, also the return), `senses` (bits that fired, 0 = not sensed), `detail` (`AwareDetail` bits: `HeardNoise` 1, `HeardCombat` 2, `NearAnchor` 4, `EngagedAnchor` 8, `SightOutOfRange` 16, `SightUnavailable` 32, `OtherSpace` 64, `NoiseBaseline` 128, `NoiseRouteOff` 256, `SightReused` 512, `SightDeferred` 1024), `distance` (viewer to target, 3D, -1 when not computed), `sightVerdict`, `sightSample`, `sightWhy`, `noiseLevel` (-1 none), `noiseAgeSec` (-1 unknown), `noiseDistance` (-1 none), `engagedWith` (the viewer or anchor the target is fighting, 0 none).

Statuses: `kQuery_Ok` (evaluated, senses may be 0), `NotMainThread`, `BadArgs` (null pointer, a size below the v18 layout, viewer or target 0, viewer equal to target, a negative or non-finite range), `Unsupported` (service not armed), `NoOrigin` (the viewer is not a loaded live actor or the target is not a loaded actor), `Failed`.

> **Did You Know?** The engine keeps one record per actor of the last noise it made (written by `AIProcess::SetActorsDetectionEvent` on a spell release, a projectile, a hit and the other engine noise sources). Harbinger's hearing is that record, read for novelty. It never adds a noise of its own.

#### Lifetime, priority and conflicts

Nothing persists. No noise baseline crosses a load. None. It changes nothing in the game.

#### Worked example

**Example 28: An engage-on-sight check for a follower**

**Solution.**

```cpp
// Call from the main thread.
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

**Check.** `r.detail` says which route fired (`HeardNoise`, `HeardCombat`, `SightReused`, `SightDeferred`). The first call for a target sets `NoiseBaseline` and hears nothing from it.

#### Version, INI and field status

ABI 18 (0.11.0). `bOwnLineOfSight` in `[Sightline]` (when off, `SenseActor` answers `kQuery_Unsupported`). `bHearNoise` in `[Awareness]` (when 0, the noise route is off and `kAwareDetail_NoiseRouteOff` is set, while hearing a nearby fight, sight, proximity and engaged still work). Used by MFO.

**Check Your Understanding: Awareness**

1. Why does the first `SenseActor` for a target never hear its noise?
2. Does `kSense_Proximity` care about walls?
3. Which sense counts the anchor, and which do not?

## Observe-only facets (no claim API)

*These seats exist so Harbinger can name who changes a part of the AI. There is no claim API for any of them. They decide what the menu page and the log show about your mod, and they explain what you see when something else changes a value you hold.*

You do not call anything. Each one changes, denies and delays nothing. A write always runs the original with the original arguments.

### Actor-value watch (`Detection` and `ActorValue` facets)

**What it is for.** While a claim holds a follower's actor values through Harbinger, the seat names anyone else who writes them. The values are: `MovementNoiseMult` and `DetectLifeRange` (held by [ch.16](#ch16-detection-kintent_detection)), `SpeedMult` (held by [ch.1a](#ch1a-gait-kintent_gait)), and Aggression, Confidence, Assistance and Morality (held by [ch.11](#ch11-disposition-kintent_disposition)).

**How it works.** Three virtual-function seats on the `Character` actor-value owner: `SetBaseActorValue` (0x04, which `SetActorValue` tail-jumps into), `ModActorValue` (0x05) and `RestoreActorValue` (0x06). A write to a held value by anyone but Harbinger is named by its caller module and counted on the `Detection` or `ActorValue` facet.

**What the client sees.** Counters and the module name on the menu page, and log lines. If you hold an actor value and a script or another mod changes it, you can read who in the log. You are never denied and the other mod is never denied. At any priority, the change stands.

**Version and INI.** Added in 0.13.0, no ABI change. `iActorValueSeat` in `[ActorValueObserve]`: 1 watch (default), 0 off. Not yet tested in game.

### Seat census

**What it is for.** Every engine seat Harbinger hooks is re-checked at each load stage and every 60 seconds. Another mod that hooks the same seat later, on top of Harbinger or inside it, is named in the log and in the census table at the bottom of the menu page, with whether Harbinger is still outermost.

**What it does.** A seat another mod hooked over becomes a `hook` source on that facet, logged once, and Harbinger re-hooks on top where a re-hook exists. This is how a foreign hook gets a priority and an attribution. It is why your priority against that mod means anything.

**Version and INI.** 0.12.0. No key.

### Package and scene source capture

**What it is for.** On the `Package` facet, Harbinger compares the package an actor is running against the one a winning ch.9 (or ch.19) claim offered. If a different package runs, an outside input exists. Harbinger names its source: the plugin of the actor's current scene, else the plugin that defines the package, else `Engine` for a runtime-created package such as Movement Blocked, combat Spectator or dialogue. A scene with no file is Unattributed. It compares that source's priority with the claim's and **logs it, but denies nothing** here. The 0x49 answer itself is ranked and enforced (see [ch.9](#ch9-offer-package-kintent_offerpackage)): this capture reports what still runs anyway, a scene included.

**Scene probe.** `bSceneProbe` in `[Harbinger]` (default on, a passive probe) logs which engine code asks for an actor's scene while a mod has given that actor a package. The scene hold-off it is gathering evidence for is not built.

**The enforced exceptions** are the ranked 0x49 answer and the Spectator hold-off, both described under [ch.9](#ch9-offer-package-kintent_offerpackage).

**Version and INI.** 0.12.0 and 0.13.0. `bSceneProbe` in `[Harbinger]`, `iSpectatorSeat` in `[PackageGate]`.

### Equip caller recovery

**What it is for.** Names the mod behind an engine equip. The caller of an engine equip is recovered by a stack scan. An equip Harbinger cannot attribute is **never denied**. Armor verdicts are enforced for clients by default.

**What the client sees.** `path=...` and source names in the `[apmf][equip-obs]` log lines, and the `Equip` table on the menu page. See [ch.17](#ch17-equip-authority-kintent_equipauthority).

**Version and INI.** 0.12.0. `bEquipObserveOnly` in `[EquipAuthority]` (`bEquipEnforceArmor` retired 2026-10-13).

### Caster-type and restore census

**What it is for.** A passive diagnosis of the cast facet. For an actor with a cast claim, `bCasterTypeCensus` logs which kind of combat caster the game's AI builds for the claimed spell (Restore, Offensive, Armor, Summon and the rest), whether the engine then fired it from the hand, whether the hand charged it, and the case where it built nothing at all. It also counts, for every actor, how often each kind fires. `bRestoreCensus` logs, for a healing or restoring spell, whether the AI ever reaches its own restore caster, how long that took, and whether the hand took the spell. Lines are rate limited.

**What the client sees.** Log lines only. The first time each cast seat answers a claim, the log says so. The census heartbeat counts every answer per caster type. If your claimed spell never reaches a caster, this is where you find out.

**Version and INI.** `bCasterTypeCensus` (1) and `bRestoreCensus` (1) in `[Probe]`. Both are on while field tested and are meant to return to 0 before a release. `bRangedSelect` (0) is a similar probe for a bow or crossbow on an actor with an equip claim. Log only, no behaviour change.

### The menu page (the viewer)

The Harbinger page in the SKSE Menu Framework shows, per facet: the sources, who wins, their priorities and hit counters, alerts and the seat census. See [HARBINGER-MENU.md](HARBINGER-MENU.md). Your DLL appears there under its file name once it files a claim, and a loaded SKSE plugin appears before it has written anything.

**Check Your Understanding: Observe-only facets**

1. A script changes a value your ch.11 claim holds. What does the watch do, and does the change stand?
2. How does another mod's hook on a Harbinger seat get a priority?
3. Which two probes ship on and are meant to return to 0 before a release?

## Appendix A: sources and priorities per facet

Every Intent maps to one sources facet. This table says which, and what Harbinger enforces against a foreign source on that facet today. "Observe" means Harbinger names and counts the source and denies nothing. The rules are under [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority).

**Table 35. Sources facets and what is enforced**

| Sources facet (`APMF_Sources.ini` section) | Intents | Enforced against a foreign source |
|---|---|---|
| `Target` | ch.6, ch.20 | ch.20 only. Restored through the engine's own `SetTarget` when a positively identified writer is outranked. ch.6 restores nothing |
| `Package` | ch.9, ch.19 | The 0x49 answer: an active alias package is replaced only when your priority is strictly above its plugin's (a tie keeps the other mod's package). An idle mod, or one only following, waiting or sandboxing, has nothing to rank. The Spectator hold-off needs your priority above `Engine`. Scenes: observe |
| `EquipWeapon` | ch.15, ch.17, ch.26, hand block | Weapons, shields, torches and ammo (any item with a Right, Left, Ammo or Light category bit). Ranked since ABI 22. Every equip deny (ch.15, the ch.17 set, the hand block, the ch.26 force) stands only while your priority is above the equip's source. The ch.26 score lift applies only while your priority is above the source beneath the score seat. An unattributed equip is allowed unless `EquipWeapon=override` |
| `EquipArmor` | ch.17 | Armor-only items. The ch.17 set and denied-category refusals stand only while your priority is above the equip's source. The facet was one `Equip` facet before 2026-10-18, and an old `[Equip]` section is still read as the fallback for both |
| `CombatAction` | ch.7, ch.23 | A leaf the engine's tree chose is stopped only while your priority is above `Engine` |
| `Cast` | ch.8, ch.8b, ch.25 | A refusal or substitution stands only while your priority is above the source of the answer it replaces (the engine, or a mod hooked there first). The ch.8b score lift follows the same rule |
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

**The menu counters.** Each facet table has five counters. **Served** counts the times Harbinger enforced that source's own claim: the engine's pick overridden by a pin, a foreign write restored, an equip refused or made for the claim, a package held, a travel leg started, a cast or action decision, a score lift, a combat entry. **Hits** counts outside writes Harbinger saw that source make to the facet. A mod acting on its own claim (the claim binds its own client, or its own equip ties with its own hand claim) is not an outside write: it is counted apart as "own" in the hover details and is not a Hit, Blocked or Passed. **Blocked** counts the outside writes Harbinger denied because a claim outranked the source. **Passed** counts the ones that stood because the source outranked the claim or tied. **Observed** counts writes seen at a seat that only watches, where nothing was decided. A mod that only files claims has Served and few or no Hits, and it is not greyed as idle while it holds a claim. The facet header names as winner the highest-priority source that was active on that facet in the last 60 seconds. `Equip` is split in two facets, weapon and armor, and a legacy `[Equip]` section is the per-key fallback for both.

`iUnregistered<Facet>` and `iClient<Facet>` change the default for a whole facet. A name in a facet section beats the default. Names are DLL or plugin names and are not case sensitive. Raise `Engine=` in `[Combat]`, `[CombatAction]` or `[Cast]` above the client to let the engine win. `[UnknownSources]` takes `allow` (default) or `override` per facet and is consulted wherever a write is attributed through the shared attribution code (any facet attributed that way), including `Package`, `Target`, `Combat`, `Cast`, `EquipWeapon` and `EquipArmor`. The menu shows the checkbox on every facet. See [INI-REFERENCE.md](INI-REFERENCE.md) for the full format.

## Appendix B: ABI history

The ABI is append-only. A client built against an older ABI keeps working against every later Harbinger. Harbinger returns null from `APMF_GetInterface(n)` for an `n` above its own ABI, and logs which release the client needs. The gating rules are under [Gating on the ABI](GUIDE.md#gating-on-the-abi).

**Table 36. ABI history**

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
| 22 | 0.13.1 | `kIntent_EquipPreference`, `EquipPrefFlags`, `APMF_EquipPrefEntry`, `APMF_EquipPrefInfo`, `SetEquipPreference`, `GetEquipPreferenceState` (struct `APMF_API_v22`) |
| 23 | 0.13.1 | `kIntent_CombatSubstrate` (no struct, no slot: rides `RequestEx` / `Release`), `APMF.esl` anchor records |
| 24 | 0.13.1 | `GetIdleState`, `IdleState`, `IdleUnconfirmable`, `IdleEnd`, `APMF_IdleStateInfo` (struct `APMF_API_v24`) |
| 25 | 0.14.0 | `kEquipEntry_HasUid`, `SetEquipEntryUid`, `EquipEntryUid`: a ch.17 entry names one item instance in its reserved bytes (no struct, no slot) |
| 26 | 0.14.0 | `GetSubstrateState`, `SubstrateState` (struct `APMF_API_v26`) |
| 27 | 0.14.0 | `IsSubstrateAvailable` (struct `APMF_API_v27`) |
| 28 | 0.14.0 | `kIntent_Place`, `PlaceFlags`, `PlaceState`, `PlaceWhy`, `APMF_PlaceStateInfo`, `GetPlaceState` (struct `APMF_API_v28`) |
| 29 | 0.15.0 | `GetClientFacetPriority`, `ClientFacet`, `kFacetPriorityUnknown` (struct `APMF_API_v29`) |
| 30 | 0.15.0 | `kTravel_CrossCells`, a `TravelFlags` bit read through the existing `RequestEx` and `Repoint` (no struct, no slot) |
| 31 | 0.15.2 | `GetClaimEndReason`, `ClaimEndReason` (struct `APMF_API_v31`) |

The last release is 0.15.2, ABI 31, interface struct `APMF_API_v31`. In 0.15.1 the interface struct was `APMF_API_v29`, since ABI 30 added a flag and no slot. Before that `APMF_API_v19` served ABI 19 to 21, and ABI 23 and 25 also added no slot.

## Appendix C: refusal cheat sheet

A refusal at the call returns `kInvalidHandle` and logs the reason in `APMF.log`. The documented degrade is to keep doing it your own way. All intents are also refused on an unsupported runtime (anything but exactly 1.6.1170, 1.5.97 and 1.7.104), on VR, and when the engine update seat was refused at load. The two timings are explained under [The two refusal timings](GUIDE.md#the-two-refusal-timings).

**Table 37. Refusals by intent**

| Intent | Refused at the call | Accepted, then inert one drain later (release it) |
|---|---|---|
| any | no channel for the intent (a Harbinger older than the intent's ABI) | |
| `kIntent_Cast` | `kCastFlag_OwnLineOfSight` with the service not armed or on the degenerate `RequestEx` form. `kCastFlag_AtPosition` on `RequestCast`. A position cast with a non-finite basis or an owned cast facet | a package with no spell input (`kCastFlag_FromPackage`). A dual claim against a single-hand claim loses by basis (dropped, never registered) |
| `kIntent_EquipAuthority` | the equip seat is not installed | |
| `kIntent_Idle` with a form | v2 path not available, `target` equal to the actor | the form is not an IDLE record, the target is not loaded. When an idle ends, `IsClaimLive` reads false |
| `kIntent_Travel` | channel not installed (`bTravel=0`, VR, `APMF.esl` missing), no destination, a point without `kTravel_ToPosition`, with it a non-zero form or a non-finite point | the form is neither a reference nor a cell. A ninth simultaneous leg (reads `kLeg_Failed`) |
| `kIntent_TargetPin` | seat not installed, `form` 0 or the actor itself, the player as the actor, line of sight flag with the service down | `form` is not an Actor |
| `kIntent_CombatEntry` | seat not installed, `form` 0 or the actor itself, the player as the actor | `form` is not an Actor (ends the claim) |
| `kIntent_CombatReentryDeny` | seat not installed, `fval` negative or NaN, the player as the actor | |
| `kIntent_PursuitLeash` | leash not armed, `target` 0 or the actor itself, `fval` not positive, the player as the actor | the anchor is not a loaded Actor |
| `kIntent_CombatApproach` | seats not installed, `target` 0 or the actor itself, `fval` not positive and finite, the player as the actor | the target is not a loaded reference (ends as `TargetLost`) |
| `kIntent_CastGuard` | gate not installed, no param, no bit or an unknown bit, bad `fval`, the player as the actor, line of fire with the measurement not armed | |
| `kIntent_CombatSubstrate` | not installed (`bCombatSubstrate=0`, the ch.22 target site, the 95-node pair, the anchor records, a self-check row), the player as the actor, `fval` negative or NaN | not out of combat, unloaded, low process, in a scene or dialogue, the party in combat, Engine outranking the client on CombatAction / Combat / Equip / Detection, no free slot, the floors not published, the engine refusing the entry (the claim is released) |
| `kIntent_Place` | not installed (VR, an unsupported runtime, the row refused), `Request` (no param), actor 0, the player, `form` 0, a non-finite `pos` or yaw, an unknown flag bit, 128 queued | it is a one-shot, not a claim: `GetPlaceState` reads `Refused` with a `PlaceWhy` (destination gone or not a reference / interior cell, actor loaded or the player present under their flags, another client's ch.19 / ch.1 owner) or `Failed` |

## Appendix D: INI keys by facet

Defaults are the shipped values. The full text of every key is in [INI-REFERENCE.md](INI-REFERENCE.md).

**Table 38. INI keys by facet**

| Facet | Section | Keys (default) |
|---|---|---|
| ch.8, ch.8b cast seats | `[CastSeats]` | `EnableSeat0Classify` (1), `EnableAimSeat` (1) |
| ch.8, ch.8b, ch.15 hand gates | `[EquipGate]` | `EnableEquipDenyComplete` (1), `EnableEquipGateLog` (0), `EnableDualWieldPreference` (0) |
| ch.8b score steer, ch.26 equip preference | `[AiCastSeats]` | `EnableScoreSteer` (1, the compiled default since ABI 22), `EnableWeaponScoreProbe` (1, also the weapon score seat ch.26 lifts through), `EnableItemScoreProbe` (0), `EnableCasterSeatProbe` (0) |
| ch.17 equip authority | `[EquipAuthority]` | `bEquipAuthority` (1), `bEquipObserveOnly` (0), `bEquipDenyScript` (0) |
| ch.8b hand block | `[HandBlock]` | `bHandClaimBlocksEquip` (1) |
| ch.15 weapon admission | `[HandBlock]` | `bEquipmentWeaponAdmission` (1), `bEquipmentWeaponObserveOnly` (0) |
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
| combat substrate | `[CombatSubstrate]` | `bCombatSubstrate` (1), `bSubstrateDryRun` (0), `fSubstrateDryRunPeriodSec` (20), `fSubstrateTtlSec` (6), `bSubstrateMoveTrace` (0), `bSubstrateMusicDeny` (1), `bSubstrateBystanderCensus` (1) |
| line of sight, own ray | `[Sightline]` | `bOwnLineOfSight` (1) |
| awareness | `[Awareness]` | `bHearNoise` (1) |
| sources registry | `[Harbinger]` | `bSceneProbe` (1) |
| census probes | `[Probe]` | `bCasterTypeCensus` (1), `bRangedSelect` (0), `bRestoreCensus` (1) |

Facets with no key of their own: ch.1, 1a, 3, 4, 5, 6, 7, 10, 11, 14, 15, 16 and ch.25. The space queries have none either.

## Appendix E: equip authority probe criteria

Equip authority enforces every declared category by default (2026-10-13). To read what it would refuse without refusing, set `bEquipObserveOnly=1` and read the `[apmf][equip-obs]` lines. The criteria below are what a clean log shows (in enforce mode `would-deny` reads `deny`). Each line carries `actor= item= op=equip path= site= ret= q= f= s= a= tls= cat= owned= black= eslot= verdict= name=`. `tls` is attribution only, since an equip Harbinger issued itself runs with `tls>0`, and it never changes a verdict. `cat= owned= black= eslot=` are the scope fields. `name` is the item's display name for reading, not parsing. A field log should show all of these before enforcement is switched on.

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

## Selected Answers

Answers to the odd-numbered questions on this page.

**ch.1.** 1. The movement goal is parked at the actor's own position, so the planner sees "already there" and issues no locomotion command. There is no walk to animate. 3. The drain's sweep releases every claim on the actor and drops it from control. Nothing is left to undo.

**ch.1a.** 1. Nothing is restored. The value no longer equals what Harbinger set, so theirs stands and Harbinger drops its stale record. 3. No. The ledger is co-saved and the load restores every outstanding override, then releases the claim.

**ch.3.** 1. Nothing is sent. Harbinger only sends `SneakStop` if it was the one that sent `SneakStart`.

**ch.4.** 1. Sheathed. The sheathe on release does not look at the state before the claim.

**ch.5.** 1. Reversible on release and no re-assert (the headtrack channel re-applies every tick). It is stated because a hidden exception to a safety rule is worse than a known one, and because the single slot it owns can be taken back by the AI at any time.

**ch.10.** 1. The release is queued behind the request and applied after it in the same drain, so the engage, and its one `PauseCurrentDialogue` call, still happen.

**ch.12.** 1. It ignores the form and plays the form-free `IdleForceDefaultState`, with no refusal and no log line. 3. A movement block (ch.1), so a walking package does not undo the pose. They are separate claims.

**ch.11.** 1. No. The four values are fixed and `fval` is reserved.

**ch.16.** 1. `MovementNoiseMult` to 0 and `DetectLifeRange` to 1.5 times its current value.

**ch.8.** 1. Nothing. A zero form names no spell and there is no built-in default. 3. It is treated as not in the set and denied. `count` is clamped to 32.

**ch.8b.** 1. None. Harbinger answers the engine's own cast-decision seats and the engine casts. 3. Nothing. It is a hint for logs and the caster census. Shape a channel with `MakeStopPct`.

**A hand floor.** 1. Yes. A driving `kIntent_Cast` claim on that hand takes it over the floor (at an equal basis by the tie rule, at a higher basis by rank), and the floor stands again when it ends. 3. Ignores it, so the floor blocks weapons, shields and torches too. Gate on ABI 20.

**Own line of sight.** 1. Nothing. Only two consecutive OCCLUDED measurements stop a held channel. UNKNOWN and UNAVAILABLE never do.

**Position cast.** 1. It is a one-shot, not a claim. It never enters the control map, and the handle labels log lines only.

**ch.14.** 1. No. It records the owner and makes no equip write. You call `EquipShout` yourself.

**ch.25.** 1. No. A friend read OCCLUDED from the caster by Harbinger's own ray is not in the line of fire, because the bolt cannot reach it. (An unknown reading still counts the friend.) 3. The guard answers at `CheckStartCast` and `CheckCast`, which the engine consults at start, at release and on channel ticks. A projectile already launched has passed every one of those.

**ch.15.** 1. Nothing. The channel is gate-only with a form. The `CheckShouldEquip` gate carries the deny.

**ch.17.** 1. True, because `bEquipObserveOnly=0` by default. A declared sword is equipped by the pass, the combat AI does not select an off-set weapon, and an engine equip of one is refused. 3. No. A refused equip is not re-issued (backlog APMF-B64). Declare again after the cast claim releases.

**ch.9.** 1. The engine's own answer, never null. 3. A nudge issued earlier re-evaluates against the old snapshot, before the claim is published, and the engine keeps its previous package.

**ch.19.** 1. True. Harbinger never releases a travel claim. Only the leg ended. 3. `kBlocker_None`, a static block. Park the task until the world changes (a gate opens, a lever is used, a cell attaches). Do not retry on a timer.

**ch.6.** 1. No. A ch.6 claim restores nothing. The restore through `SetTarget` belongs to ch.20.

**ch.7.** 1. `kCombatActionCat_Cast`. 3. The heal never fires. ch.7 denies the cast leaves, which are the cast claim's delivery mechanism.

**ch.20.** 1. The pin pauses, with the claim live, until the engine has a target and yours is in the group. 3. When the writer is positively identified and your priority on `Target` is above its priority. An unnamed writer stands unless `[UnknownSources] Target=override`.

**ch.21.** 1. The engine's own selector picks, among the group's targets, which now include the bandit. To make it the bandit, pin it with ch.20. 3. Ended. Harbinger released the claim itself with "engine refused entry" in the log. Send a new `RequestEx` to try again.

**ch.22.** 1. The deny is pending until the next drain, so the engine pulls the actor straight back into combat after `StopCombat`. Wait for `IsClaimLive` to read true first. 3. No. The ally's fight goes through `StartCombat` for the actor, and the deny refuses it while the window runs.

**ch.23.** 1. No. The actor is outside the radius (2000 against 1500), so the leash is testing, but the goal is 1800 from the anchor and the actor is 2000 from it. The move brings the actor closer, so it is allowed. A leaf is refused only when its goal is farther from the anchor than the actor already is. 3. No. The leash is its own facet, arbitrated separately from ch.7.

**ch.24.** 1. `kApproachState_Waiting`. When combat starts the bound is applied and the state moves to `Approaching` or `Holding`. 3. Harbinger. The claim ends as `kApproachState_Arrived` and is released, so a lower-ranked approach claim can take over.

**Space queries.** 1. `*outCount` is 16 (the nearest 16) and `*outTotal` is 70.

**Line of sight.** 1. The call reads a stored measurement and marks the pair as asked for. No measurement exists yet, so it reads `kLos_Unknown`, and the main thread measures the pair within a frame or two. 3. Any thread. It is a lock-free read and never casts a ray.

**Awareness.** 1. The first query records the target's current noise as a baseline and sets `kAwareDetail_NoiseBaseline`. Only a change seen after that counts. 3. Hearing and engaged count the anchor. Sight and proximity ignore it.

**Observe-only facets.** 1. It names the writer's module in the log and on the menu page and counts it. The change stands at any priority. 3. `bCasterTypeCensus` and `bRestoreCensus` in `[Probe]`.
