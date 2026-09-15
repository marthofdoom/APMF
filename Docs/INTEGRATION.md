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
decision-making engine function. It arbitrates who owns a facet and denies the
losers.

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
  unequipped it. APMF coalesces an identical re-issue inside 1 s and holds
  back an item whose queued equip from the previous pass has not applied yet,
  so a per-tick re-send cannot become a loop, but it is still wasted work.
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

### The probe criteria (what a field log must show before enforcement is switched on)

Read `[apmf][equip-obs]` lines. Each carries `actor= item= op=equip path=
site= ret= q= f= s= a= tls= verdict= name=`. `tls` is attribution only (an
equip APMF issued itself runs with `tls>0`); it never changes a verdict. The
`name='...'` field is the item's display name, for reading, not for parsing.

1. Every engine re-equip of an off-set item on a claimed actor logs
   `verdict=would-deny` with a NAMED path (`OutfitApply`, `AddWornOutfit`,
   `AiCommand`, `RemoveItemReequip`, `CombatNode`, `PlayerMenu`, `Script`,
   `Console`, `WorkerReentry`, `QueuedApply`, or `External(<dll>)`).
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
   followed by its `[apmf][equip-obs] ... tls=1 verdict=allow` line, and then
   its `path=QueuedApply ... verdict=allow` apply line.
5. At least one engine id attributes: the log must contain at least one
   `[apmf][equip-obs]` line whose `path` is a named engine path (not
   `External(...)`). If every line reads `External(<dll>)`, another plugin
   has inline-detoured the engine's `EquipObject` entry and every caller now
   returns through it; the seat logs `entry <id> detoured by <dll>` at install
   when it can see that. Attribution is degraded and the script exemption is
   unavailable until that detour is accounted for.

Only after all five hold on a real session is `bEquipObserveOnly` flipped to 0.

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
| `kIntent_EquipAuthority` (ch.17) | **Declare what the NPC wears; APMF equips it and refuses every other engine equip.** ABI v7, declare with `SetEquipSet` | `ival` (an `EquipAuthFlags` bitmask); the set itself via `SetEquipSet` | Built, not yet battle-tested. Ships OBSERVE-ONLY (`[EquipAuthority] bEquipObserveOnly=1`) until the probe criteria above pass. The only call-site seat in APMF, under `Docs/INVARIANTS.md` #17a. |

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
  game thread.
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
