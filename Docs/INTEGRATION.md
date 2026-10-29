# Integrating with Harbinger (APMF)

A getting-started guide for SKSE plugin authors who want their mod to share NPC control with other mods instead of fighting them for it.

This page covers the part every client needs: finding the interface, checking the version, becoming a source, and the claim, heartbeat and release pattern. It does not describe individual facets. The model behind all of it (facets, basis and priority, the claim lifecycle, threading) is in **[GUIDE.md](GUIDE.md)**. Every facet, with its arguments, rules and examples, is in **[FACETS.md](FACETS.md)**. Scenarios that combine facets, with complete code, are in **[RECIPES.md](RECIPES.md)**, and every term is defined in **[GLOSSARY.md](GLOSSARY.md)**.

## What Harbinger is

Harbinger (the project is APMF, the AI Package Management Framework) is a per-facet arbitration layer for NPC control. You claim one facet of an actor at a priority you choose. Harbinger routes that facet to you and denies whoever else was driving it. Everything else about the actor keeps running exactly as it was: its movement, its other packages, the rest of its AI. When you release the claim, Harbinger stops suppressing and the actor's own AI, or the next best claim, takes the facet back.

Harbinger is a moderator. It does not generate decisions. For most facets you bring the behaviour with your own proven mechanism and Harbinger makes sure it reaches the actor. For the cast facet you declare what to cast and at whom, and the NPC's own combat AI does the rest, fully animated. For the cast facet Harbinger makes no equip, cast or animation call of its own.

Some other facets do make a small, bounded engine call because you declared it: `EquipObject` for the equip authority's declared set, `PlayIdle` for an idle, `DrawWeaponMagicHands` for a weapon draw, the `SneakStart` and `SneakStop` events for a stance, `SetDontMove` and a parked goal for a movement block, `StartCombat` for a combat entry, an `EvaluatePackage` nudge for a package offer or a travel leg, and `CastSpellImmediate` from a marker for a position cast. A combat re-entry deny calls nothing. It makes the engine's own `StartCombat` refuse. Each facet section in [FACETS.md](FACETS.md) names the calls it makes.

## Before you start

- You build against one file, [`native/APMF_API.h`](../native/APMF_API.h). It is a plain C-ABI header with no CommonLib dependency. Copy it byte for byte. It is append-only, so a client built against any older copy keeps working against every later Harbinger. The header keeps a short contract comment on every declaration. Its long explanations are in [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md) and its historical notes in [APMF_API-HISTORY.md](APMF_API-HISTORY.md); the header points to each one.
- Harbinger runs on Skyrim AE 1.6.1170, SE 1.5.97 and 1.7.104, and refuses every other build and VR by name. See the [README](../README.md) for requirements.
- Read [GUIDE.md](GUIDE.md) once. It explains facets, basis and priority, the claim lifecycle (pending, live, the drain, refusal timings) and threading, and this page assumes it.

## 1. Find the interface and check the version

Harbinger exports one undecorated C function, `APMF_GetInterface`. It returns a pointer to a static POD struct of function pointers. You never free it. Fetch it once, after SKSE has loaded (`kPostLoad` or `kDataLoaded`), and keep it.

There is no separate `Version()` call. The version is the `abiVersion` field, the first member of every interface struct. `APMF_GetInterface(n)` returns null if the installed Harbinger is older than `n`, and the newest interface for any `n` at or below its own ABI. The interface it hands back reports Harbinger's own ABI in `abiVersion`, whatever you asked for.

```cpp
#include <Windows.h>
#include "APMF_API.h"

namespace MyMod {

    // The newest struct (APMF_API_v29, ABI 29. Harbinger reports ABI 30 because ABI 30 added a
    // flag and no slot). Reading a slot past the end of an older Harbinger's interface
    // is undefined, so gate EVERY call on abiVersion (see Has below).
    const APMF_API::APMF_API_v29* g_apmf = nullptr;

    // Ask for the LOWEST ABI you can live with. GetInterface returns null for anything
    // above the installed Harbinger's own, with no downward retry.
    constexpr std::uint32_t kMinAbi = 5;

    bool Has(std::uint32_t abi) { return g_apmf && g_apmf->abiVersion >= abi; }

    void BindHarbinger() {
        HMODULE h = GetModuleHandleA("APMF.dll");
        if (!h) return;                                   // not installed: run your own path

        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) return;

        const APMF_API::APMF_API_v1* base = fn(kMinAbi);
        if (!base || base->abiVersion < kMinAbi) return;  // installed Harbinger is too old

        g_apmf = reinterpret_cast<const APMF_API::APMF_API_v29*>(base);
    }

}
```

Then every call site reads like this.

```cpp
if (MyMod::Has(6) && MyMod::g_apmf->IsClaimLive(handle)) { /* ... */ }   // IsClaimLive is ABI 6
```

Which ABI introduced which slot, intent and flag is in [GUIDE.md "Gating on the ABI"](GUIDE.md#gating-on-the-abi) and [FACETS.md Appendix B](FACETS.md#appendix-b-abi-history). The newest ones are listed under [What is new from ABI 22 to 30](#what-is-new-from-abi-22-to-30) below. Two things catch people out. An unknown intent is refused (`kInvalidHandle`), which is a loud failure. An unknown flag bit is ignored without a word, which is a quiet one. Check `abiVersion` before you set any flag a facet section marks as newer than ABI 5.

Harbinger logs `APMF_GetInterface` either way. If a client asks for an ABI the installed Harbinger does not have, the log names the release that does.

## 2. You are a source

You do not register anything. Harbinger reads the return address of `Request`, `RequestEx` and `RequestCast` and finds the module that called. The source name is your DLL file name. Make the calls from your own DLL, not through a helper that lives in another one.

Each source has a priority from 0 to 100 on each facet. Your default is 50. A player or you can change it in `Data/SKSE/Plugins/APMF_Sources.ini`, and the [Harbinger menu page](HARBINGER-MENU.md) edits it live.

```ini
[Cast]
MyMod.dll=60
```

Priority is how your claim ranks against a foreign source (another mod's hook, the engine, a script). The `basis` you pass with each request is how your claim ranks against other clients' claims. They are different numbers. [GUIDE.md "Sources, Priority and Arbitration"](GUIDE.md#sources-priority-and-arbitration) has the rules, the defaults, and what happens to a change Harbinger cannot trace.

## 3. The claim pattern

Every facet follows the same steps. Claim, wait for it to apply, keep it alive if it is bounded, release it.

**Claim.** Pick the intent, fill an `APMF_Param` (zero-initialised, set only what the facet reads), and call `RequestEx`. For a cast use `RequestCast`. The call is safe from any thread. It returns a `Handle` at once, or `kInvalidHandle` if Harbinger refuses.

**A refusal means do it your own way.** The seat is down (VR, an unsupported runtime, the facet switched off in `APMF.ini`) or a parameter is wrong. The reason is in `APMF.log`. Harbinger refuses rather than accepting a claim that would silently do nothing, so your degrade path actually runs. Most refusals are synchronous, but a few can only be found one frame later on the game thread, and then the handle is live and inert. [FACETS.md Appendix C](FACETS.md#appendix-c-refusal-cheat-sheet) lists both for every intent.

**Wait for it.** The request is applied on the game thread at Harbinger's next drain. `IsClaimLive` (ABI 6) reads the applied state, so it is false for a claim that is still pending. A handle that has never read live is pending, not ended. Only a handle that was live and no longer is has ended. Set your own bound on how long you wait, and do not count paused-menu time, because the drain runs on the player update.

**Keep it alive, if it is bounded.** Three facets are bounded and release themselves: the cast claim (ch.8b), the combat re-entry deny (ch.22) and the cast guard (ch.25). The combat substrate (ch.27) is bounded too, but it is not renewed. Harbinger releases the substrate claim itself when the window closes (on its cast, its time limit, the cast claim's end or a real fight), and you file it again for the next cast. To hold a cast or a guard longer than one window, send a `Repoint` before it lapses. A renewal moves the deadline to now plus the window the claim was granted. Do not release and re-request, because the gap leaves the actor unclaimed. Every other claim is standing and needs no heartbeat. A claim that is not bounded can still be ended by Harbinger itself when its goal is over, for example a target pin whose target died. Poll `IsClaimLive` for those.

**Release it.** `Release(handle)` when you are done with the facet. Release a handle even if you think it ended. It is a no-op on a claim Harbinger already released. A release queued behind a pending request is applied after it in the same drain, so the claim engages and is then released. Nothing is cancelled. Reserve `Release` for being done. Use `Repoint` for a mere change of target.

**After a load.** Every claim is released at `kPreLoadGame`. Claims are never saved. Forget your handles on a load and claim again once the game is running.

Here is the pattern as one reusable helper. It tracks pending, live and ended, and always releases.

```cpp
enum class ClaimState { None, Pending, Live, Ended };

struct TrackedClaim {
    APMF_API::Handle h        = APMF_API::kInvalidHandle;
    bool             everLive = false;
    std::uint32_t    waited   = 0;

    // How many polls to wait for the first live read before calling it ended.
    // Your own choice. MFO polls every 133 ms and waits 15 unpaused polls.
    static constexpr std::uint32_t kMaxPending = 15;

    // Any thread. False means refused: do it your own way.
    bool Open(RE::FormID actor, APMF_API::Intent intent, float basis, const APMF_API::APMF_Param& p) {
        Close();
        h = MyMod::g_apmf->RequestEx(actor, intent, basis, &p);
        everLive = false;
        waited   = 0;
        return h != APMF_API::kInvalidHandle;
    }

    // Call from your own tick. Needs ABI 6.
    ClaimState Poll() {
        if (h == APMF_API::kInvalidHandle) return ClaimState::None;
        if (MyMod::g_apmf->IsClaimLive(h)) { everLive = true; return ClaimState::Live; }
        if (!everLive && ++waited < kMaxPending) return ClaimState::Pending;
        return ClaimState::Ended;               // was live and is not, or never applied
    }

    // Move a held claim without releasing it. Needs ABI 3.
    void Repoint(const APMF_API::APMF_Param& p) {
        if (h != APMF_API::kInvalidHandle) MyMod::g_apmf->Repoint(h, &p);
    }

    void Close() {
        if (h != APMF_API::kInvalidHandle) MyMod::g_apmf->Release(h);   // safe on an ended claim
        h = APMF_API::kInvalidHandle;
    }
};
```

And a heartbeat for a bounded claim, sent from the same tick.

```cpp
#include <chrono>

struct Heartbeat {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    // windowMs is the window you asked for. Renew well before it lapses.
    void Tick(TrackedClaim& c, const APMF_API::APMF_Param& p, std::uint32_t windowMs) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(windowMs / 3)) return;
        last = now;
        c.Repoint(p);                           // renews the window. A cast claim cannot change its spell this way
    }
};
```

A cast guard claim built with them is the smallest complete example. It renews while the claim is live and asks again once it has ended, which is what happens after the window lapses or a load drops it.

```cpp
void KeepFriendlyFireHold(RE::FormID follower, TrackedClaim& guard, Heartbeat& beat) {
    if (!MyMod::Has(21)) return;                // kIntent_CastGuard is ABI 21

    APMF_API::APMF_Param p{};
    p.ival = APMF_API::kCastGuard_LineOfFire;
    p.fval = 6.0f;                              // the window in seconds

    switch (guard.Poll()) {
        case ClaimState::None:
            guard.Open(follower, APMF_API::kIntent_CastGuard, 50.0f, p);
            break;
        case ClaimState::Pending:
            break;                              // wait for the first live read
        case ClaimState::Live:
            beat.Tick(guard, p, 6000);          // renew before the window lapses
            break;
        case ClaimState::Ended:
            guard.Open(follower, APMF_API::kIntent_CastGuard, 50.0f, p);   // lapsed or dropped: ask again
            break;
    }
}
```

## 4. Threading

`Request`, `RequestEx`, `RequestCast`, `Repoint`, `Release` and the declaration calls copy what you pass and queue the work, so any thread may call them. The read-only state reads are any-thread too. Three queries are synchronous and run on the true main thread only (the player-update thread): `FindEmptySpace`, `FindHostilesInSpace` and `SenseActor`. SKSE's `AddTask` is not that thread. No exception ever crosses the boundary. [GUIDE.md "Threading"](GUIDE.md#threading) has the full list.

## 5. Coexistence

This is what makes Harbinger safe to install beside mods that know nothing about it.

- **Chainable hooks only.** Harbinger never overwrites another mod's hook. Every deny gate calls through to whatever answered before it and only turns a yes into a no for the one thing it denies. The single exception is the equip authority seat (ch.17), which has no virtual function on its path. It patches two internal call sites, byte-verifies both first, and refuses to install if anyone else touched them, so a collision is a missing feature and never a crash.
- **Engine answer first.** A deny gate lets the engine, or the next hook in the chain, answer first. Harbinger never invents a yes. (The cast facet has one bounded, documented exception: one equip seat that has to be answered from the claim.)
- **One input, not a whole system.** A claim covers exactly one facet. An actor whose cast choice is claimed keeps moving, keeps fighting and keeps reacting under its own AI.
- **Reversible on release.** When the last claim on a facet releases, the suppression stops. There is no re-assert loop and no lingering override to clean up.
- **Never deny what it cannot name.** Harbinger enforces only against a positively identified source that has been outranked. A write it cannot attribute is allowed, logged as `[sources] UNATTRIBUTED <facet>` and shown as an alert.

If you want Harbinger to hold something for you, say so with a claim, and stop doing that thing yourself while the claim is live. `IsEquipAuthorityEnforced` is true when the equip seat is installed and `bEquipObserveOnly` is 0, the default since 2026-10-13: every category you declare (weapons, shields, torches, ammo, armor) is enforced, in and out of combat. Two consequences for an equip claim. Your claim binds you: your own direct equip of an item your declaration refuses is refused too, so equip what you declared. And a worn item in a category you DENY is taken off by Harbinger when the declaration lands. A client that turns its own equips off on a successful equip claim should read FACETS.md ch.17 first.

## 6. Debugging

- **`APMF.log`.** Every refusal, every end and every attribution is logged with its reason. Search for `REFUSED`, `ended`, `UNATTRIBUTED` and `ATTRIBUTION DEFECT`. If a claim did not do what you expected, read the log line before assuming a bug. Facet tags are `[ch.N]`.
- **The menu page.** The Harbinger page in the SKSE Menu Framework shows, per facet, the sources, the winner, their priorities, five counters (Served, Hits, Blocked, Passed, Observed), alerts and the seat census. Served counts the times Harbinger enforced your own claim. Blocked and Passed count other sources' writes against your claim. See [HARBINGER-MENU.md](HARBINGER-MENU.md). Your DLL appears there under its file name once it files a claim.
- **Substrate and travel lines.** `[substrate]` lines say how a substrate window opened and closed (`CLOSE reason=fired`, `fired-not-released`, `released-unconfirmed` or `tree-outside-gate`). `LEAF GATE`, `GATE UNRESOLVED` and `CAST BUILT` are passive. `[travel-leg]` and `[travel-xcell]` say how a leg started, restarted and ended.
- **Settings.** `APMF.ini` switches facets and probes, and `APMF_Sources.ini` holds priorities. See [INI-REFERENCE.md](INI-REFERENCE.md).
- **Observe-only seats.** Some seats only name who changes a value and never deny. They are in [FACETS.md "Observe-only facets"](FACETS.md#observe-only-facets-no-claim-api).

## 7. The worked example

[MFO (marth's Follower Overhaul)](https://github.com/marthofdoom/MFO) is Harbinger's first client. If you want to see a real mod claim facets, drive them and release them cleanly, read `native/apmf/` in that repo.

| File | What it shows |
|---|---|
| `Bridge.cpp` | binding (`APMF_GetInterface` at the lowest ABI MFO needs), the cast claim create-or-refresh, `Repoint` heartbeats |
| `CastClaims.cpp` | offense and heal cast claims, per hand |
| `SpellAllowList.cpp` | ch.8 select-spell and `SetSpellAllowList` |
| `Equip.cpp` | ch.17: claim, `SetEquipScope`, `SetEquipSetEx` |
| `Excursion.cpp`, `Deposit.cpp`, `ErrandClaims.cpp` | ch.19 travel, ch.20 pins, ch.1 hold and ch.12 idle |
| `Substrate.cpp` | ch.27: the substrate claim and the cast claim filed as a pair, `GetSubstrateState` and `IsSubstrateAvailable` |
| `EquipPreference.cpp` | ch.26: a declaration sent only when it changes |
| `Place.cpp` | `kIntent_Place` and `GetPlaceState` |
| `CastGuard.cpp` | ch.25: a 6 second window, a `Repoint` heartbeat, and a fresh request when the claim has gone stale |
| `CombatEntry.cpp`, `ReentryDeny.cpp` | ch.21 and ch.22 with pending-versus-ended tracking |
| `PursuitLeash.cpp`, `CombatApproach.cpp`, `OffenseApproach.cpp` | ch.23 and ch.24 |
| `Los.cpp` | `GetLineOfSight` and `SenseActor` |

MFO files every claim with a basis of 200.

## Equip preference (ch.26, ABI v22)

New in 0.13.1, not yet tested in game. The full reference is [ch.26 Equip preference](FACETS.md#ch26-equip-preference-kintent_equippreference). Since the same ABI every equip refusal (the ch.17 set, the hand block, the ch.15 refusal) follows the `Equip` priorities in `APMF_Sources.ini`.

## What is new from ABI 22 to 31

A client built against an older header keeps working. Each item below needs its ABI check before you use it. Everything here is also in [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md) and [FACETS.md](FACETS.md), and the header wins over both.

| ABI | Release | What a client gets | Gate |
|---|---|---|---|
| 22 | 0.13.1 | `kIntent_EquipPreference` with `SetEquipPreference` and `GetEquipPreferenceState` | `abiVersion >= 22` |
| 23 | 0.13.1 | `kIntent_CombatSubstrate`, filed beside a cast claim through `RequestEx` | `abiVersion >= 23` |
| 24 | 0.13.1 | `GetIdleState` | `abiVersion >= 24` |
| 25 | 0.14.0 | an equip entry can name one item copy by its unique id | `abiVersion >= 25` |
| 26 | 0.14.0 | `GetSubstrateState(actor)` says none, held, real or anchor | `abiVersion >= 26` |
| 27 | 0.14.0 | `IsSubstrateAvailable()` says whether the substrate installed on this runtime | `abiVersion >= 27` |
| 28 | 0.14.0 | `kIntent_Place` and `GetPlaceState(handle, &info)` | `abiVersion >= 28` |
| 29 | 0.15.0 | `GetClientFacetPriority(facet)` | `abiVersion >= 29` |
| 30 | 0.15.0 | the `kTravel_CrossCells` bit on a travel claim to a reference. No new slot | `abiVersion >= 30` |
| 31 | 0.15.2 | `GetClaimEndReason(handle)` says why Harbinger ended a ch.20 pin or ch.21 entry claim | `abiVersion >= 31` |

### Place (ABI 28)

`kIntent_Place` moves an actor beside a reference or to a point in an interior cell, once, through the game's own move call. It is not a claim. `IsClaimLive` is false for it, and there is nothing to release or renew. Put the destination in `param.form` and the offset in `param.pos`. Read the outcome with `GetPlaceState(handle, &info)` after you set `info.size = sizeof(info)`. The state is `Placed`, `Refused` or `Failed`. A failure is never reported as a success. It is refused when another client's travel or hold owns the actor, and your own claim never blocks it. A finished state is kept until you have read it and 30 seconds have passed. Details: [FACETS.md "Place"](FACETS.md#place-kintent_place) and [APMF_API-REFERENCE.md](APMF_API-REFERENCE.md#abi-v28-place).

### The combat substrate (ch.27, ABI 23, 26 and 27)

An animated cast is the follower's own combat AI casting, and that AI only exists in combat. For a cast out of combat, file a `kIntent_CombatSubstrate` claim and your cast claim together. Harbinger puts the follower into combat against its own invisible stand-in, lets only the claimed spell through and gives the follower back when the cast leaves. It ships on (`bCombatSubstrate=1`).

- Ask `IsSubstrateAvailable` first (ABI 27). False means the capability is absent on this install, which is a degrade you can plan for. A refusal on one request is a failure to log, not to fall back from.
- Ask `GetSubstrateState(actor)` (ABI 26) before you treat an actor's combat as a fight. A held substrate is not a fight.
- The window closes as fired only on the game's own spell cast event for the spell. A release that never confirms closes after 3 seconds as `fired-not-released`. A hand that goes empty after a release with no cast event closes as `released-unconfirmed`. A combat caster built before the follower's tree runs under the gate closes as `tree-outside-gate`. In each case Harbinger releases the substrate claim (`IsClaimLive` turns false) and the reason is in `APMF.log`. The API does not return the reason.
- The window runs from the tree's first run, not from your request, and waits a fixed 10 seconds for it to start. Size your own cast claim's window for that.
- A cast proxy for a buff carries the spell's real name and menu icon.
- A real fight starts: the substrate is dropped at once and your cast claim runs as an in-combat claim.

The full contract is [FACETS.md "Combat substrate"](FACETS.md#combat-substrate-kintent_combatsubstrate).

### Travel legs: restarts and keeping control (ch.19)

Two behaviours changed for a travel claim, with no API change, and one flag was added.

- **A walk the game ends short is started again.** If the game ends a reference leg's package while the follower is still outside the arrival radius and the destination has moved, Harbinger starts the leg again on a fresh package slot, up to 3 times. After that the leg reads `kLeg_StuckTimeout` at once, instead of after the 2 minute net. `GetTravelLegState` goes back to walking with a new `seq` at each restart, so compare `seq` and not just the state.
- **BLOCKED and STUCK keep the follower.** While your claim stands, Harbinger keeps its travel package on the follower after either end. The state reads as before. The follower no longer drifts back to its own package. A blocked follower carries on when the way clears. A stuck one stands where the walk ended. Only your `Release` hands the follower back, and a `Repoint` starts a new leg on a fresh slot. The held failure occupies one of the 8 package slots until then, so release it when you give up.
- **Across cells (ABI 30).** Set `kTravel_CrossCells` on a leg to a reference and the follower walks through doors, the game's own way, and follows the NPC if it moves. The leg counts as arrived only in the same interior cell as the destination, or outdoors in the same worldspace, and inside the radius. A destination outdoors that is not loaded yet is waited for. Arrival holds the follower until you release or re-point, like a cell leg. Do not set the bit unless you mean it, because an older Harbinger drops it silently.

### Asking what the player allowed (ABI 29)

The player ranks every mod on every facet, and 0 means off. A claim of yours on a facet at 0 never wins. `GetClientFacetPriority(facet)` returns your own number, so you can plan around it. Pass a `ClientFacet` value such as `kFacet_EquipWeapon` or `kFacet_EquipArmor`. It returns 0 to 100, or `kFacetPriorityUnknown` (-1) for a facet it does not know. A client that has not filed a claim yet gets the priority its first claim will rank at. Any thread, read only.

```cpp
// MFO's use: with EquipWeapon at 0 the follower will not wear a looted upgrade, so sell it.
bool CanEquipWeapons() {
    if (!MyMod::Has(29)) return true;                   // older Harbinger: behave as before
    return MyMod::g_apmf->GetClientFacetPriority(APMF_API::kFacet_EquipWeapon) != 0;
}
```

### Why a claim ended (ABI 31)

When `IsClaimLive` turns false on a ch.20 pin or a ch.21 entry, `GetClaimEndReason(handle)` says why Harbinger ended it. It returns a `ClaimEndReason`.

| Value | Meaning |
|---|---|
| `kClaimEnd_None` (0) | no record: still live, released by you, dropped by an unload or a load, another intent, or an unknown handle |
| `kClaimEnd_NotEngaged` (1) | held engagement: the target is beyond your engage range (`param.fval`) and not attacking the party. The actor's combat was ended |
| `kClaimEnd_NoPathProgress` (2) | held engagement: 20 s with no path progress toward the target. This counts as not engaged, so the actor's combat was ended too |
| `kClaimEnd_ArrivedIdle` (3) | held engagement: 20 s within reach with no attack or cast on the target |
| `kClaimEnd_CombatEnded` (4) | ch.21: the game ended the actor's fight |
| `kClaimEnd_EntryRefused` (5) | ch.21: `StartCombat` returned false |
| `kClaimEnd_NotAttempted` (6) | ch.21: the actor or the target could not be used, so no call was made |
| `kClaimEnd_OwnerDead` (7) | the actor died |
| `kClaimEnd_TargetDead` (8) | the target died |
| `kClaimEnd_TargetGone` (9) | the target is disabled, unloaded or no longer resolves |
| `kClaimEnd_TargetLost` (10) | ch.20 only with the held engagement off: the game lost the target |
| `kClaimEnd_NotATarget` (11) | ch.20: the target left the actor's group targets after it was one |

The two not engaged reasons mean the follower cannot or should not fight that target now. marth's rule for MFO: give that target up instead of engaging it again at once. Harbinger itself has no cooldown, so a client that engages again gets a new engagement. The reason is recorded in the same step that queues the release, so it can be readable a frame before `IsClaimLive` turns false. A claim you release at the same moment Harbinger ends it can still carry a reason. Records are dropped on a load or a new game, and only the newest 256 are kept. Any thread, read only. An older Harbinger has no slot.

```cpp
// After a held engagement ends: give the target up if Harbinger dropped it as not engaged.
bool GaveUp(APMF_API::Handle h) {
    if (!MyMod::Has(31)) return false;                  // older Harbinger: no reason to read
    const auto* api = reinterpret_cast<const APMF_API::APMF_API_v31*>(MyMod::g_apmf);
    const auto  why = api->GetClaimEndReason(h);
    return why == APMF_API::kClaimEnd_NotEngaged || why == APMF_API::kClaimEnd_NoPathProgress;
}
```

### Equip is two facets

On the page and in `APMF_Sources.ini` equip ranks as `EquipWeapon` (weapons, shields, torches, ammo) and `EquipArmor`. Your priority is separate on each. An old `[Equip]` section is still read where the new ones have no value.

## Idle state (ch.12, ABI v24)

New in 0.13.1, not yet tested in game. `APMF_API_v24::GetIdleState(handle, &info)` says what became of a ch.12 idle claim's `PlayIdle` call: `Pending`, `Accepted`, `Confirmed`, `ConfirmedWeak`, `NotConfirmed`, `NoTransition`, `Refused`, `NotPlayed`, `Unconfirmable` (with the reason) or `Carried` (no evidence, only no `IdleStop` since the same claim's previous play), and `endCause` says how the claim ended. Gate on `abiVersion >= 24`. If your mod must act only after the animation really played, wait for `Confirmed` instead of timing Harbinger's 3 second window yourself. Any thread. An ended claim's state stays readable for 10 seconds of world time. The full table is in [ch.12 Idle](FACETS.md#reading-what-happened-abi-24).

## Sections that moved

Older comments in `APMF_API.h` and the source tree cite sections of this page by name. Those sections now live in [FACETS.md](FACETS.md) and [GUIDE.md](GUIDE.md).

| Old name | Now |
|---|---|
| "Spells-only floor" | [A hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly) |
| "Cast guards" | [ch.25 Cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) |
| "enter combat + pin" | the example under [ch.21 Combat entry](FACETS.md#ch21-combat-entry-kintent_combatentry) |
| "the fight cannot be lost; give it a range" | ch.21 and ch.20 claims are HELD engagements (2026-10-12, `Docs/INVARIANTS.md` #0 (i)). A ch.21 claim may set `param.fval` to its engage range in game units (0 = none): the moment the target is beyond it and not attacking the party, it is dropped, the actor's combat is ended (unless another foe is attacking the party) and its prior behaviour restored. Since 2026-10-27 a held engagement with no path progress for 20 s is dropped the same way, and `GetClaimEndReason` (ABI 31) says which end it was. There is no cooldown after any end: the same target may be engaged again at once. `APMF_API.h`'s "No other field is read" for kIntent_CombatEntry is out of date until the next mirrored header edit. |
| "retreat: StopCombat once + deny re-entry" | the example under [ch.22 Combat re-entry deny](FACETS.md#ch22-combat-re-entry-deny-kintent_combatreentrydeny) |
| "Making an NPC cast", "Casting at a point" | [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast), [Position cast](FACETS.md#position-cast-kcastflag_atposition) |
| "Declaring what an NPC wears", "Scoping the authority", the probe criteria | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority), [Appendix E](FACETS.md#appendix-e-equip-authority-probe-criteria) |
| "Walking an NPC somewhere" | [ch.19 Travel](FACETS.md#ch19-travel-kintent_travel) |
| "The Harbinger controller" | [GUIDE.md "Sources, Priority and Arbitration"](GUIDE.md#sources-priority-and-arbitration), [FACETS.md Appendix A](FACETS.md#appendix-a-sources-and-priorities-per-facet) |
| "The facet table" | [Facet index](FACETS.md#facet-index) |
| a section named for an ABI ("ABI v8", "ABI v9", "ABI v11 queries", and so on) | the facet that ABI introduced, listed in [Appendix B](FACETS.md#appendix-b-abi-history) |
| "The probe criteria" | [Appendix E](FACETS.md#appendix-e-equip-authority-probe-criteria) |

## Where next

- **[GUIDE.md](GUIDE.md)** is the model: facets, basis and priority, the claim lifecycle, threading, binding and how to read the log.
- **[FACETS.md](FACETS.md)** is the reference: every facet, its arguments, what Harbinger enforces, its lifetime, its priority rules and examples.
- **[RECIPES.md](RECIPES.md)** is the scenarios: a healer in standoff, a loot trip, a retreat, a loadout, engage on sight, a service tick, with complete code.
- **[GLOSSARY.md](GLOSSARY.md)** defines every term once.
- **[CHANNEL-MAP.md](CHANNEL-MAP.md)** maps each channel to its hook site, with the research behind it.
- **[INI-REFERENCE.md](INI-REFERENCE.md)** documents every key in `APMF.ini` and the format of `APMF_Sources.ini`.
- **[native/APMF_API.h](../native/APMF_API.h)** is the contract, and it wins over any page here.
