# Harbinger recipes: worked scenarios

*A real follower behaviour is several facets claimed together, each as its own handle. These scenarios show the combinations that come up most, with complete code that compiles against the real header and a way to check each one in game.*

| Page | What it is for |
|---|---|
| [GUIDE.md](GUIDE.md) | the foundations these recipes assume: facets, basis and priority, the claim lifecycle, threading, binding |
| [FACETS.md](FACETS.md) | the reference for every facet a recipe claims |
| **This page, `RECIPES.md`** | worked scenarios that combine facets |
| [GLOSSARY.md](GLOSSARY.md) | every term, defined once |

Each recipe gives the goal, **Think it through** (which facets, and why those), a **Solution** that compiles, and a **Check** for the game or the log. The code uses the `TrackedClaim` and `Heartbeat` helpers from [GUIDE.md](GUIDE.md#tracking-a-claim-in-your-own-code), reproduced in the prelude below. Every recipe is written the way MFO, the reference client, does the same job, and the **Real-World Connection** callouts say where. The questions at the end are things to try, with answers.

<details>
<summary><b>The prelude every example on this page assumes</b></summary>

```cpp prelude
#include "APMF_API.h"
#include <chrono>
#include <cstdint>
using namespace APMF_API;

extern const APMF_API_v29* g_apmf;   // bound once at startup, GUIDE.md "Binding and Version Gating"
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

## Keep a healer in standoff

*A healer that stays with the party, walks toward whoever needs healing while it fights, heals them with the game's own animation, and never runs off after a fleeing foe. Four facets, four handles, one tick.*

**Think it through.** The behaviour splits into four engine decisions. Where the healer moves in combat is the combat area, so a [combat approach](FACETS.md#ch24-combat-approach-kintent_combatapproach) on the wounded friend with `kApproach_Hold` keeps it within reach and still fighting. Whether it chases is the pursuit leaves, so a [pursuit leash](FACETS.md#ch23-pursuit-leash-kintent_pursuitleash) to the player stops it leaving. What it casts is the cast facet, so a [cast claim](FACETS.md#ch8b-cast-kintent_cast) names the heal and the friend, on the left hand, as a channel that stops at 80 percent. Whether its own attack spells go through friends is the [cast guard](FACETS.md#ch25-cast-guard-kintent_castguard). Two of the four are bounded and need a heartbeat. The approach ends itself when the friend is lost or combat ends, and `Poll` reports that as `Ended`, so the tick simply reopens it while the friend still needs healing.

**Solution.**

```cpp
struct Standoff {
    TrackedClaim approach, leash, heal, guard;
    Heartbeat    healBeat, guardBeat;
    RE::FormID   healTarget = 0;
};

// Your own service tick, every 100 to 500 ms of unpaused game.
// `wounded` is the friend to heal now, or 0 when nobody needs it.
void TickStandoff(RE::FormID healer, RE::FormID healSpell, RE::FormID wounded, Standoff& s) {
    if (g_apmf->abiVersion < 21) return;                 // the guard is ABI 21, the newest piece here

    // 1. The leash to the player: a standing claim, opened once, reopened after a load.
    {
        APMF_Param p{};
        p.target = 0x14;
        p.fval   = 1500.0f;
        const ClaimState st = s.leash.Poll();
        if (st == ClaimState::None || st == ClaimState::Ended)
            s.leash.Open(healer, kIntent_PursuitLeash, kBasis, &p);
    }

    // 2. The friendly-fire guard: bounded, so heartbeat it. Six seconds, like MFO.
    {
        APMF_Param p{};
        p.ival = kCastGuard_LineOfFire;
        p.fval = 6.0f;
        switch (s.guard.Poll()) {
            case ClaimState::None:
            case ClaimState::Ended:   s.guard.Open(healer, kIntent_CastGuard, kBasis, &p); break;
            case ClaimState::Pending: break;
            case ClaimState::Live:    s.guardBeat.Tick(s.guard, &p, 6000); break;
        }
    }

    // 3. Nobody to heal: drop the approach and the heal, keep the leash and the guard.
    if (wounded == 0) {
        s.approach.Close();
        s.heal.Close();
        s.healTarget = 0;
        return;
    }

    // 4. A new patient: a new approach and a new heal claim (a cast claim cannot be re-aimed).
    if (wounded != s.healTarget) {
        s.approach.Close();
        s.heal.Close();
        s.healTarget = wounded;
    }

    // 5. The approach: Harbinger ends it itself on arrival (we hold), target lost or combat end.
    {
        APMF_Param p{};
        p.target = wounded;
        p.fval   = 400.0f;
        p.ival   = kApproach_Hold;
        const ClaimState st = s.approach.Poll();
        if (st == ClaimState::None || st == ClaimState::Ended)
            s.approach.Open(healer, kIntent_CombatApproach, kBasis, &p);
    }

    // 6. The heal: bounded, heartbeat with the same form. Reopen after a lapse or a load.
    {
        APMF_Param beat{};
        beat.form = healSpell;                               // same form: a pure renewal
        switch (s.heal.Poll()) {
            case ClaimState::None:
            case ClaimState::Ended: {
                APMF_CastRequest req{};
                req.spell  = healSpell;
                req.target = wounded;
                req.flags  = kCastFlag_LeftHand | kCastFlag_Concentration | MakeStopPct(80);
                req.ttlMs  = 4000;
                s.heal.OpenCast(healer, kBasis, &req);
                break;
            }
            case ClaimState::Pending: break;
            case ClaimState::Live:    s.healBeat.Tick(s.heal, &beat, 4000); break;
        }
    }
}

void EndStandoff(Standoff& s) {
    s.heal.Close();
    s.approach.Close();
    s.guard.Close();
    s.leash.Close();
    s.healTarget = 0;
}
```

**Check.** In a fight with a wounded friend: `[ch.24]` shows the bound applied (`applied` greater than 0 in `GetCombatApproachState`), the healer walks toward the friend in its combat gait, `[ch.8b]` shows the seats answering the heal claim and the beam lands on the friend and stops near 80 percent, `[ch.23]` counts refused pursuit leaves when a foe runs, and with the player in the line the healer holds its attack spell with a `[cast-guard]` refusal. No `expired` or `window elapsed` lines while the tick runs.

> **Real-World Connection** This is MFO's healer: `CombatApproach.cpp` for the approach, `PursuitLeash.cpp` for the leash, `CastClaims.cpp` for the per-hand heal claim with its heartbeat, and `CastGuard.cpp` for the guard, all driven from one service lap per follower.

## Hold a travel leg to loot

*Walk a follower to an item, hold it still and play the pickup animation when it arrives, then move on. When it is blocked, decide by what blocked it.*

**Think it through.** Three facets in sequence: a [travel leg](FACETS.md#ch19-travel-kintent_travel) to the item, then on arrival a [movement block](FACETS.md#ch1-movement-block-kintent_movementblock) so the animation is not walked out of, and an [idle](FACETS.md#ch12-idle-kintent_idle) at the item. The leg ends but the travel claim stays live, so the tick reads `GetTravelLegState` to learn why the walk stopped, and compares `seq` and `destForm` so an older leg's end is not mistaken for this one's. A BLOCKED end is two different situations: an actor in the way (reorder, come back later) and a static block (park until the world changes). Combat cancels the leg, and the client decides whether to resume after the fight. Since 0.15.1 a BLOCKED or STUCK end keeps the follower on the travel package for as long as the claim stands, so he does not wander back to his own package while the client decides. The client ends that hold by releasing the claim or re-pointing it.

**Solution.**

```cpp
enum class LootStep { Idle, Walking, Pickup, Done, Blocked, Cancelled };

struct LootTrip {
    LootStep      step = LootStep::Idle;
    TrackedClaim  travel, hold, idle;
    RE::FormID    item = 0;
    std::uint32_t seqBefore = 0;
    std::uint32_t pickupTicks = 0;
    bool          actorBlocked = false;      // true: someone stood in the way, retry later
};

bool StartLootTrip(RE::FormID follower, RE::FormID item, LootTrip& t) {
    if (g_apmf->abiVersion < 12) return false;           // the state read is ABI 12
    APMF_TravelLegInfo pre{};
    pre.size = sizeof(pre);
    g_apmf->GetTravelLegState(follower, &pre);
    t.seqBefore = pre.seq;

    APMF_Param p{};
    p.form = item;
    p.fval = 80.0f;
    p.ival = kTravel_SpeedSet | kTravel_SpeedJog;
    if (!t.travel.Open(follower, kIntent_Travel, kBasis, &p)) return false;   // esl missing, bTravel=0, VR
    t.item = item;
    t.step = LootStep::Walking;
    return true;
}

// Your service tick. `giveIdle` is the IDLE record to play at the item (ABI 17).
void TickLootTrip(RE::FormID follower, RE::FormID giveIdle, LootTrip& t) {
    switch (t.step) {
    case LootStep::Walking: {
        APMF_TravelLegInfo info{};
        info.size = sizeof(info);
        const std::uint32_t st = g_apmf->GetTravelLegState(follower, &info);
        const bool ours = info.destForm == t.item && info.seq != t.seqBefore &&
                          (info.ownerHandle == t.travel.h || info.ownerHandle == 0);
        if (!ours || st < kLeg_Arrived) return;          // still walking, or an older leg's state
        if (st == kLeg_Arrived) {
            APMF_Param p{};
            p.form   = giveIdle;
            p.target = t.item;
            t.hold.Open(follower, kIntent_MovementBlock, kBasis, nullptr);
            if (g_apmf->abiVersion >= 17) t.idle.Open(follower, kIntent_Idle, kBasis, &p);
            t.pickupTicks = 0;
            t.step = LootStep::Pickup;
        } else if (st == kLeg_Blocked) {
            t.actorBlocked = info.blockerKind != kBlocker_None;
            t.step = LootStep::Blocked;                  // the claim is still live: Close it when you give up
        } else {
            t.step = LootStep::Cancelled;                // combat, destination gone, stuck, actor gone, failed. A STUCK end keeps the offer: Close() the claim to hand him back
        }
        return;
    }
    case LootStep::Pickup:
        // Your own pickup call goes here, on the main thread, once the animation has had its time.
        if (++t.pickupTicks >= 8) {                      // about a second at a 133 ms tick
            t.idle.Close();
            t.hold.Close();
            t.travel.Close();
            t.step = LootStep::Done;
        }
        return;
    default:
        return;
    }
}

// After a BLOCKED end: an actor block will clear, so try again later. A static block
// stays closed until the world changes (a gate opens, a lever is used, a cell attaches).
void RetryOrPark(RE::FormID follower, LootTrip& t) {
    if (t.step != LootStep::Blocked) return;
    if (t.actorBlocked) {
        APMF_Param p{};
        p.form = t.item;
        p.fval = 80.0f;
        p.ival = kTravel_SpeedSet | kTravel_SpeedJog;
        t.travel.Repoint(&p);                            // the same place again: a Repoint starts a new leg
        t.step = LootStep::Walking;
    } else {
        t.travel.Close();                                // park the task. Do not retry on a timer
        t.step = LootStep::Idle;
    }
}
```

**Check.** `[travel-leg]` shows the leg start and its end reason. On arrival `[ctl]` shows the ch.1 engage and `[ch.12]` the `PlayIdle` call. A BLOCKED end logs the stall point and the blocker, and `blockerKind` in the state read decides the branch. If the destination is an actor who moved and the game ends the walk short, the state goes back to walking with a new `seq` (up to 3 times) before it reads stuck. A fixed item never triggers that restart. A combat cancel reads `kLeg_CombatCancelled`.

> **Real-World Connection** MFO's `Excursion.cpp` is this loop, and marth's loot rules are the BLOCKED branch: an actor block is reordered, a static block is parked until a `TESOpenCloseEvent` or `TESActivateEvent` says the world changed.

## Guard a channel against friendly fire

*Let a mage follower keep its own attack spells, but never through the player, and never a concentration stream the mod has not asked for.*

**Think it through.** This is one [cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) claim with both guards: `kCastGuard_LineOfFire` holds an offensive cast while a friend is within 80 units of the line or inside the blast, and the channel bits refuse the AI's own concentration spells on both hands unless a live cast claim names them. The guard is bounded, so it is heartbeated. Its `ival` is re-read on `Repoint`, so the bound can be switched on and off from the tick without a new claim. The line-of-fire bit is refused when the measurement is not armed, and that is a configuration fact a load does not change, so a refused guard is struck for the session rather than asked again every tick.

**Solution.**

```cpp
struct FriendlyFire {
    TrackedClaim  guard;
    Heartbeat     beat;
    std::uint32_t refused = 0;           // guard bits Harbinger refused this session
};

void TickFriendlyFire(RE::FormID mage, bool boundChannels, FriendlyFire& f) {
    if (g_apmf->abiVersion < 21) return;

    std::uint32_t want = kCastGuard_LineOfFire;
    if (boundChannels) want |= kCastGuard_ChannelRight | kCastGuard_ChannelLeft;
    want &= ~f.refused;
    if (want == 0) { f.guard.Close(); return; }         // a Repoint with no bit is refused: release instead

    APMF_Param p{};
    p.ival = static_cast<std::int32_t>(want);
    p.fval = 6.0f;

    switch (f.guard.Poll()) {
        case ClaimState::None:
        case ClaimState::Ended:
            if (!f.guard.Open(mage, kIntent_CastGuard, kBasis, &p)) {
                // Refused on configuration (gate not installed, measurement not armed).
                // Strike LineOfFire first, so the channel bound is still asked for next tick.
                f.refused |= (want & kCastGuard_LineOfFire) ? kCastGuard_LineOfFire : want;
            }
            break;
        case ClaimState::Pending:
            break;
        case ClaimState::Live:
            f.beat.Tick(f.guard, &p, 6000);               // renews the window and re-reads ival
            break;
    }
}
```

**Check.** Stand between the mage and its foe: the mage charges and holds, and `[cast-guard]` logs the refusal at 0x06 or 0x0A with your FormID and the distance from the line. Step aside and it fires. With the channels bound, the mage's own Flames or Sparks never starts, and the log shows the 0x0A refusal naming the hand. `[apmf][cast-guard] claim refused` once at startup means the measurement is not armed on this install, and the recipe stops asking.

> **Real-World Connection** This is `native/apmf/CastGuard.cpp` in MFO 2.3.0, which replaced MFO's own `CheckCast` and `CheckStartCast` hooks with this claim. MFO keeps no decay: Harbinger holds for as long as a friend is in the line, and re-measures at the release of a charged cast.

## Enter a fight and pin the target

*Make a follower turn on a foe it had not noticed, and keep it on that foe.*

**Think it through.** Two facets, two handles, one target. [Combat entry](FACETS.md#ch21-combat-entry-kintent_combatentry) makes one `StartCombat` call that puts the foe in the follower's combat group. It does not choose whom the follower fights, so a [target pin](FACETS.md#ch20-target-pin-kintent_targetpin) answers the engine's selector with the same foe. The entry claim ends itself when the fight ends or the engine refuses, and the pin ends itself when the target is lost, so both are polled and both report `Ended` rather than staying inert. Release on the entry stops nothing, which is what you want: the fight is the engine's now.

**Solution.**

```cpp
struct Engagement { TrackedClaim entry, pin; RE::FormID foe = 0; };

bool Engage(RE::FormID follower, RE::FormID foe, Engagement& e) {
    if (g_apmf->abiVersion < 14) return false;          // entry is 14, the pin is 13
    APMF_Param p{};
    p.form = foe;
    if (!e.entry.Open(follower, kIntent_CombatEntry, kBasis, &p)) return false;   // the log says why
    e.pin.Open(follower, kIntent_TargetPin, kBasis, &p);
    e.foe = foe;
    return true;
}

// Returns true once the fight with this foe is over, as far as Harbinger can tell.
bool TickEngagement(Engagement& e) {
    const ClaimState entry = e.entry.Poll();
    const ClaimState pin   = e.pin.Poll();
    if (entry == ClaimState::Ended && pin == ClaimState::Ended) {   // combat ended, or the foe is gone
        e.entry.Close();
        e.pin.Close();
        e.foe = 0;
        return true;
    }
    return false;
}

void Disengage(Engagement& e) {                           // stops nothing: call StopCombat yourself if you want it over
    e.pin.Close();
    e.entry.Close();
    e.foe = 0;
}
```

**Check.** `[ch.21]` logs the `StartCombat` call and whether the engine accepted it. `[ch.20]` logs `FIRST SOURCE DENY` once the pin replaced the engine's pick, and `pin ended: target lost` when the foe dies. `TickEngagement` returns true shortly after either.

> **Real-World Connection** MFO's `CombatEntry.cpp` carries the same pending-versus-ended tracking, because an entry that reads false one tick after the request is pending, not refused.

## Retreat

*Pull a follower out of a fight and keep the engine from pulling it straight back in, then send it somewhere.*

**Think it through.** The [re-entry deny](FACETS.md#ch22-combat-re-entry-deny-kintent_combatreentrydeny) stops nothing already running, so the recipe is: claim the deny, wait for it to read live, then call `StopCombat` once yourself. Where the follower goes afterwards is a package decision, so an [offer package](FACETS.md#ch9-offer-package-kintent_offerpackage) claim is filed before the stop, and the Spectator hold-off keeps the engine from handing it the watching package while that claim outranks the engine. The deny is bounded and restarts its window on `Repoint`, so a heartbeat keeps the retreat as long as you want it. `StopCombat` is your own main-thread engine call, passed in as a callable.

**Solution.**

```cpp
struct Retreat {
    TrackedClaim deny, package;
    Heartbeat    beat;
    bool         stopped = false;
};

bool StartRetreat(RE::FormID follower, RE::FormID retreatPackage, Retreat& r) {
    if (g_apmf->abiVersion < 15) return false;
    APMF_Param pkg{};
    pkg.form = retreatPackage;
    r.package.Open(follower, kIntent_OfferPackage, kBasis, &pkg);   // before the stop, so the engine has somewhere to go

    APMF_Param p{};
    p.fval = 20.0f;                                        // seconds. The heartbeat keeps it going
    r.stopped = false;
    return r.deny.Open(follower, kIntent_CombatReentryDeny, kBasis, &p);
}

// On the MAIN thread. Returns true when the retreat is over (the deny ended and will not be renewed).
template <class StopCombatFn>
bool TickRetreat(Retreat& r, bool keepRetreating, StopCombatFn stopCombatOnce) {
    APMF_Param p{};
    p.fval = 20.0f;
    switch (r.deny.Poll()) {
        case ClaimState::Pending:
            return false;                                  // not applied yet: StopCombat would be undone
        case ClaimState::Live:
            if (!r.stopped) { r.stopped = true; stopCombatOnce(); }   // the deny stands: end the fight ONCE
            if (keepRetreating) r.beat.Tick(r.deny, &p, 20000);       // Repoint restarts the window
            return false;
        case ClaimState::None:
        case ClaimState::Ended:
            r.deny.Close();
            r.package.Close();
            return true;
    }
    return true;
}
```

**Check.** `[ch.22] seat OBSERVED`, then one refused `StartCombat` per attempt to pull the follower back, each with its caller named. The follower runs the retreat package and does not stand watching the fight (`iSpectatorSeat=2`). A `DENY MISSED` line means the actor entered combat anyway and should be reported.

> **Real-World Connection** MFO's `ReentryDeny.cpp` is this shape, and the 2026-09-28 field run saw every deny logged and none missed.

## Declare a loadout

*Put a follower in exactly the gear the mod chose and keep the game from stripping or replacing it.*

**Think it through.** One [equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) claim per follower, scoped to what the mod owns (the hands and the body here, ammo left to the engine), then one declaration per loadout change with a hand per item. The declaration is an event, not a tick. A removal makes the engine's replacement off-set, so the mod re-declares whenever its own loadout changes. `IsEquipAuthorityEnforced` says whether Harbinger enforces at least armor, which with the defaults is true while weapons are still only observed, so a client that turns its own equips off reads it as "armor is held" and nothing more.

**Solution.**

```cpp
struct Loadout { RE::FormID right = 0, left = 0, body = 0; };

struct EquipHold { TrackedClaim auth; Loadout declared; };

bool ClaimLoadout(RE::FormID follower, EquipHold& e) {
    if (g_apmf->abiVersion < 9) return false;            // the scope is ABI 9
    APMF_Param p{};
    p.ival = kEquipAuth_None;                            // scripts pass, the player's menu passes
    if (!e.auth.Open(follower, kIntent_EquipAuthority, kBasis, &p)) return false;   // seat not installed: keep your own equips

    APMF_EquipScope scope{};
    scope.owned  = kEquipCat_Right | kEquipCat_Left | kEquipCat_Armor;
    scope.denied = 0;
    g_apmf->SetEquipScope(e.auth.h, &scope);
    return true;
}

// Call when the loadout CHANGES, never per tick. Entries with a 0 form are left out.
void Declare(EquipHold& e, const Loadout& l) {
    if (e.auth.h == kInvalidHandle) return;
    APMF_EquipEntry set[3]{};
    std::uint32_t n = 0;
    if (l.right) set[n++] = { l.right, kEquipSlot_Right,   { 0, 0, 0 } };
    if (l.left)  set[n++] = { l.left,  kEquipSlot_Left,    { 0, 0, 0 } };
    if (l.body)  set[n++] = { l.body,  kEquipSlot_Default, { 0, 0, 0 } };
    g_apmf->SetEquipSetEx(e.auth.h, n ? set : nullptr, n);   // an empty set clears the declaration
    e.declared = l;
}

bool ArmorIsHeld() {
    return g_apmf->abiVersion >= 8 && g_apmf->IsEquipAuthorityEnforced();
}

void ReleaseLoadout(EquipHold& e) {
    if (e.auth.h != kInvalidHandle) g_apmf->SetEquipSetEx(e.auth.h, nullptr, 0);
    e.auth.Close();                                      // nothing is unequipped
    e.declared = {};
}
```

**Check.** `SET-EQUIP-SCOPE`, then an `enforce pass` line, then one `-> equip` line per item not yet worn and its `tls=1 verdict=allow` echo. An outfit refresh afterwards logs `would-deny` for off-set items (or `deny` with `bEquipObserveOnly=0`). [FACETS.md Appendix E](FACETS.md#appendix-e-equip-authority-probe-criteria) is the full checklist before you switch enforcement on.

> **Real-World Connection** MFO's `Equip.cpp` declares with `SetEquipSetEx` so a dagger in the off hand does not evict the sword, which a slot-less declaration did before ABI 8.

## Engage on sight

*A follower that attacks a known enemy the moment it sees, hears or is near it, without the engine's facing-limited detection.*

**Think it through.** [`SenseActor`](FACETS.md#awareness-senseactor) answers whether the follower senses the target through four senses, omnidirectional, with the player as a second listener. It decides nothing about hostility, so the client decides that first (its own list, or [`FindHostilesInSpace`](FACETS.md#space-queries-findemptyspace-and-findhostilesinspace)). Both queries are main-thread only, so this runs from a main-thread hop. Once sensed, the fight is the entry-and-pin recipe above.

**Solution.**

```cpp
// MAIN THREAD ONLY: SenseActor casts the sight ray. `foe` is already known to be hostile.
bool TrySenseAndEngage(RE::FormID follower, RE::FormID foe, Engagement& e) {
    if (g_apmf->abiVersion < 18) return false;
    if (e.foe != 0) return false;                        // already fighting someone

    APMF_AwarenessQuery q{};
    q.size       = sizeof(q);
    q.viewer     = follower;
    q.target     = foe;
    q.anchor     = 0x14;                                 // the player counts for hearing and engaged
    q.sightRange = 3000.0f;                              // the other ranges stay at Harbinger's defaults
    APMF_AwarenessResult r{};
    r.size = sizeof(r);
    if (g_apmf->SenseActor(&q, &r) != kQuery_Ok) return false;   // NotMainThread, Unsupported, NoOrigin: the log says
    if (r.senses == kSense_None) return false;           // not sensed this call. Ask again next tick

    return Engage(follower, foe, e);                     // entry + pin, from the recipe above
}
```

**Check.** `r.detail` says which route fired (`HeardCombat`, `HeardNoise`, `SightReused`). The first call for a target sets `NoiseBaseline` and hears nothing from it, so a target that is only making noise is engaged on the second call at the earliest. Then the entry and pin lines from the recipe above.

> **Real-World Connection** MFO's `Los.cpp` wraps `GetLineOfSight` and `SenseActor` and feeds its engage-on-sight gambit, with the player as the anchor.

## A service tick that owns many claims

*One place that owns every handle a follower has, drives them from one tick, and forgets them all on a load.*

**Think it through.** Handles do not survive a load, so the one rule that matters is: forget everything at `kPreLoadGame`, and let the tick reopen what is still wanted once the game runs. Every claim is a `TrackedClaim`, so the tick reads `Poll` and reopens on `Ended`. Bounded claims get a `Heartbeat`. A follower that unloads has its claims dropped by Harbinger's sweep, and `Poll` reports that as `Ended` too, so the tick handles it the same way. The per-follower record below is the shape MFO's bridge keeps, made small.

**Solution.**

```cpp
#include <unordered_map>

struct FollowerClaims {
    Standoff     standoff;       // from the first recipe
    FriendlyFire guard;          // from the third
    EquipHold    equip;          // from the sixth
};

struct ClaimService {
    std::unordered_map<RE::FormID, FollowerClaims> byFollower;

    // On SKSE kPreLoadGame, and on a new game: every handle is dead. Forget them without Release.
    // Release would be a harmless no-op on a dead handle, but there is nothing to release.
    void ForgetAll() { byFollower.clear(); }

    // A follower dismissed or dead: release what it had.
    void Drop(RE::FormID follower) {
        auto it = byFollower.find(follower);
        if (it == byFollower.end()) return;
        EndStandoff(it->second.standoff);
        it->second.guard.guard.Close();
        ReleaseLoadout(it->second.equip);
        byFollower.erase(it);
    }

    // Your own tick, every 100 to 500 ms of unpaused game, per active follower.
    void Tick(RE::FormID follower, RE::FormID healSpell, RE::FormID wounded, bool boundChannels) {
        if (!g_apmf) return;                                  // Harbinger absent: your own paths run
        auto& c = byFollower[follower];
        TickStandoff(follower, healSpell, wounded, c.standoff);
        TickFriendlyFire(follower, boundChannels, c.guard);
        if (c.equip.auth.Poll() == ClaimState::Ended) {       // dropped by a load or an unload: claim again
            ClaimLoadout(follower, c.equip);
            Declare(c.equip, c.equip.declared);
        }
    }
};
```

**Check.** After a load, `APMF.log` shows a fresh `ENGAGED` line for every claim the tick wants, and no `Repoint` on a dead handle (those are silent no-ops, so their absence is the check). On a dismissed follower the `[ctl] ... released` lines appear once.

> **Real-World Connection** MFO's `Bridge.cpp` holds the per-follower record, forgets it at `kPreLoadGame`, and drives every claim from the same 133 ms service lap.

## Check Your Understanding: Recipes

Try these against the recipes above.

1. In the standoff recipe, why does a change of patient close the heal claim instead of re-pointing it?
2. In the loot recipe, the leg ends `kLeg_Blocked` with `blockerKind == kBlocker_Teammate`. What does `RetryOrPark` do, and why?
3. In the retreat recipe, what goes wrong if `stopCombatOnce` runs while the deny is still pending?
4. In the service tick, why is `ForgetAll` a clear and not a loop of `Close` calls?
5. In the engage-on-sight recipe, a target is standing still and silent behind the follower, 500 units away, in the open. Which sense fires?

## Selected Answers

1. A cast claim's `Repoint` cannot change what it casts or whom it targets. A different spell is refused and the target is resolved at request. A new patient is a new claim.
2. It re-points the travel claim at the same item, which starts a new leg. A teammate stood in front of the follower and will move, so the task is retried rather than parked.
3. The deny is not applied yet, so the engine pulls the follower straight back into combat after `StopCombat`. The recipe waits for `Poll` to read `Live` first.
4. At `kPreLoadGame` Harbinger has already released every claim through its channel, and on a new game it has cleared the map. The handles are dead numbers. `Release` on them would be a harmless no-op, but there is nothing to release, so the record is simply forgotten.
5. Sight. The own ray has no view cone and no facing test, the target is within `sightRange`, and nothing blocks the line. Hearing does not fire (no noise, no combat), proximity does not (500 is over the 256 default), engaged does not (it is not fighting).
