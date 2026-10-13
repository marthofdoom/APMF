# Integrating with Harbinger (APMF)

A getting-started guide for SKSE plugin authors who want their mod to share NPC control with other mods instead of fighting them for it.

This page covers the part every client needs: finding the interface, checking the version, becoming a source, and the claim, heartbeat and release pattern. It does not describe individual facets. The model behind all of it (facets, basis and priority, the claim lifecycle, threading) is in **[GUIDE.md](GUIDE.md)**. Every facet, with its arguments, rules and examples, is in **[FACETS.md](FACETS.md)**. Scenarios that combine facets, with complete code, are in **[RECIPES.md](RECIPES.md)**, and every term is defined in **[GLOSSARY.md](GLOSSARY.md)**.

## What Harbinger is

Harbinger (the project is APMF, the AI Package Management Framework) is a per-facet arbitration layer for NPC control. You claim one facet of an actor at a priority you choose. Harbinger routes that facet to you and denies whoever else was driving it. Everything else about the actor keeps running exactly as it was: its movement, its other packages, the rest of its AI. When you release the claim, Harbinger stops suppressing and the actor's own AI, or the next best claim, takes the facet back.

Harbinger is a moderator. It does not generate decisions. For most facets you bring the behaviour with your own proven mechanism and Harbinger makes sure it reaches the actor. For the cast facet you declare what to cast and at whom, and the NPC's own combat AI does the rest, fully animated. For the cast facet Harbinger makes no equip, cast or animation call of its own.

Some other facets do make a small, bounded engine call because you declared it: `EquipObject` for the equip authority's declared set, `PlayIdle` for an idle, `DrawWeaponMagicHands` for a weapon draw, the `SneakStart` and `SneakStop` events for a stance, `SetDontMove` and a parked goal for a movement block, `StartCombat` for a combat entry, an `EvaluatePackage` nudge for a package offer or a travel leg, and `CastSpellImmediate` from a marker for a position cast. A combat re-entry deny calls nothing. It makes the engine's own `StartCombat` refuse. Each facet section in [FACETS.md](FACETS.md) names the calls it makes.

## Before you start

- You build against one file, [`native/APMF_API.h`](../native/APMF_API.h). It is a plain C-ABI header with no CommonLib dependency. Copy it byte for byte. It is append-only, so a client built against any older copy keeps working against every later Harbinger.
- Harbinger runs on Skyrim AE 1.6.1170, SE 1.5.97 and 1.7.104, and refuses every other build and VR by name. See the [README](../README.md) for requirements.
- Read [GUIDE.md](GUIDE.md) once. It explains facets, basis and priority, the claim lifecycle (pending, live, the drain, refusal timings) and threading, and this page assumes it.

## 1. Find the interface and check the version

Harbinger exports one undecorated C function, `APMF_GetInterface`. It returns a pointer to a static POD struct of function pointers. You never free it. Fetch it once, after SKSE has loaded (`kPostLoad` or `kDataLoaded`), and keep it.

There is no separate `Version()` call. The version is the `abiVersion` field, the first member of every interface struct. `APMF_GetInterface(n)` returns null if the installed Harbinger is older than `n`, and the newest interface for any `n` at or below its own ABI. The interface it hands back reports Harbinger's own ABI in `abiVersion`, whatever you asked for.

```cpp
#include <Windows.h>
#include "APMF_API.h"

namespace MyMod {

    // The newest struct. Reading a slot past the end of an older Harbinger's interface
    // is undefined, so gate EVERY call on abiVersion (see Has below).
    const APMF_API::APMF_API_v19* g_apmf = nullptr;

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

        g_apmf = reinterpret_cast<const APMF_API::APMF_API_v19*>(base);
    }

}
```

Then every call site reads like this.

```cpp
if (MyMod::Has(6) && MyMod::g_apmf->IsClaimLive(handle)) { /* ... */ }   // IsClaimLive is ABI 6
```

Which ABI introduced which slot, intent and flag is in [GUIDE.md "Gating on the ABI"](GUIDE.md#gating-on-the-abi) and [FACETS.md Appendix B](FACETS.md#appendix-b-abi-history). Two things catch people out. An unknown intent is refused (`kInvalidHandle`), which is a loud failure. An unknown flag bit is ignored without a word, which is a quiet one. Check `abiVersion` before you set any flag a facet section marks as newer than ABI 5.

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

**Keep it alive, if it is bounded.** Three facets are bounded and release themselves: the cast claim (ch.8b), the combat re-entry deny (ch.22) and the cast guard (ch.25). To hold a cast or a guard longer than one window, send a `Repoint` before it lapses. A renewal moves the deadline to now plus the window the claim was granted. Do not release and re-request, because the gap leaves the actor unclaimed. Every other claim is standing and needs no heartbeat. A claim that is not bounded can still be ended by Harbinger itself when its goal is over, for example a target pin whose target died. Poll `IsClaimLive` for those.

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

If you want Harbinger to hold something for you, say so with a claim, and stop doing that thing yourself while the claim is live. `IsEquipAuthorityEnforced` is true when the equip seat is installed and either `bEquipObserveOnly` is 0 or `bEquipEnforceArmor` is 1, so with the defaults it is true while weapons are still only observed. A client that turns its own equips off on a successful equip claim should read FACETS.md ch.17 first.

## 6. Debugging

- **`APMF.log`.** Every refusal, every end and every attribution is logged with its reason. Search for `REFUSED`, `ended`, `UNATTRIBUTED` and `ATTRIBUTION DEFECT`. If a claim did not do what you expected, read the log line before assuming a bug. Facet tags are `[ch.N]`.
- **The menu page.** The Harbinger page in the SKSE Menu Framework shows, per facet, the sources, the winner, their priorities and hit counts, alerts and the seat census. See [HARBINGER-MENU.md](HARBINGER-MENU.md). Your DLL appears there under its file name once it files a claim.
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
| `Excursion.cpp`, `Deposit.cpp` | ch.19 travel, ch.20 pins, ch.1 hold and ch.12 idle |
| `CastGuard.cpp` | ch.25: a 6 second window, a `Repoint` heartbeat, and a fresh request when the claim has gone stale |
| `CombatEntry.cpp`, `ReentryDeny.cpp` | ch.21 and ch.22 with pending-versus-ended tracking |
| `PursuitLeash.cpp`, `CombatApproach.cpp`, `OffenseApproach.cpp` | ch.23 and ch.24 |
| `Los.cpp` | `GetLineOfSight` and `SenseActor` |

MFO files every claim with a basis of 200.

## Equip preference (ch.26, ABI v22), until FACETS.md has its section

New in the release after 0.13.0, not yet tested in game. The full contract is the "ABI v22: EQUIP PREFERENCE" block at the end of `APMF_API.h`; MAP.md "core/EquipPreference" has the internals.

- Check `abiVersion >= 22`. An older Harbinger refuses intent 26.
- `RequestEx(actor, kIntent_EquipPreference, basis, &param)` with `param.ival = 0`. Then `SetEquipPreference(handle, entries, n)` with one `APMF_EquipPrefEntry{form, slot, rank}` per preferred item: `kEquipSlot_Right` / `kEquipSlot_Left` for a one-hander, a shield or a torch, `kEquipSlot_Default` for a two-hander or a bow. Rank 0 is the most preferred. Declare when the preference changes, not every tick.
- In combat the game scores every weapon, shield, torch and spell about once a second and equips the best of each group while a hand is free. Harbinger lifts a declared item's score one step above the best other item of its group. The game still decides: a bow is skipped at melee range, a Restore, Ward or buff spell takes its hand before any weapon, and an item scored 0 (a bow with no arrows) is never lifted. A hand your own cast claim holds keeps only the spell.
- The lift follows the Equip priorities in `APMF_Sources.ini`, like every other deny. Equip refusals (the ch.17 set, the hand block, the ch.15 refusal) follow them too since this release.
- Read `GetEquipPreferenceState(actor, &info)` to tell a lift that did not win (`kEquipPrefState_Lifted`) from no lift at all (`HandHeld`, `Outranked`, `ZeroScore`, `Declared` = not scored yet).
- Escalate only after you saw a miss for about 2 seconds in combat: `Repoint` with `param.ival = kEquipPref_Force`. Harbinger then refuses the game's other equips into the preferred hands, ranked. It equips nothing itself: place the item yourself or through ch.17. A refusal the priorities forbid is logged as a FORCE ANOMALY and counted on the Harbinger page. Clear the bit once the item holds.

FACETS.md still needs: a "ch.26 Equip preference" section in the facet template, a row in the facet index and in Appendix A (Equip: ranked lift, ranked force), the D2 change in the ch.17 and hand-floor sections ("not ranked by priority" no longer holds), `EnableScoreSteer` default 1, and Appendix B rows for ABI v22.

## Sections that moved

Older comments in `APMF_API.h` and the source tree cite sections of this page by name. Those sections now live in [FACETS.md](FACETS.md) and [GUIDE.md](GUIDE.md).

| Old name | Now |
|---|---|
| "Spells-only floor" | [A hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly) |
| "Cast guards" | [ch.25 Cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) |
| "enter combat + pin" | the example under [ch.21 Combat entry](FACETS.md#ch21-combat-entry-kintent_combatentry) |
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
