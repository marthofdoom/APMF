# Integrating with APMF

A quickstart for SKSE plugin authors who want their mod to share NPC control
with other mods, instead of fighting them for it.

## What APMF is

APMF (AI Package Management Framework, "Harbinger") is a per-facet control
layer for NPC control. You declare what you want and where, on one facet of
an actor (its combat target, its cast selection, whether it's sneaking, and
so on), at a priority you choose. APMF arbitrates who owns that facet, denies
whoever else was driving it, and for some facets now executes the behavior
itself. Everything else about that actor, movement, other packages, the rest
of its AI, keeps running exactly as it was. When you release the claim, APMF
stops suppressing and the actor's own AI (or the next-highest claim) takes it
back.

This is the DECLARE -> ENFORCE model. You declare intent, APMF enforces it:
it arbitrates who owns the facet, denies the competing sources completely (no
half-denied facet, no lingering competitor), and where execution belongs to
APMF, it carries the execution out. A claimed cast is the clearest example:
you name the spell, the hand, and the target, and APMF equips the spell and
drives the animated cast itself. You don't write `selectedSpells`, you don't
touch a `MagicCaster`, you just hold the claim.

This is newer than APMF's original design, where it only ever arbitrated and
denied, and never called a decision-making engine function itself. That older
model still applies to most facets today (combat targeting, shout selection,
casting's base arbitration-only mode), where you still bring your own
mechanism (your own `currentCombatTarget` write, your own `selectedSpells`
write) and APMF just makes sure it reaches the actor. Two things never
change, on either model: APMF never substitutes a whole package for the one
an actor is already running, and it never fabricates a facet you didn't
declare. It composes exactly what you asked for.

## Two surfaces: ASK or POINT

APMF gives you two ways to make a request. Both land on the same enforcer
underneath.

- **ASK** is the surface to reach for first, for almost everything. You
  declare intent directly: `RequestEx(actor, Intent, basis, param)` returns a
  `Handle`. `Repoint(handle, param)` changes what or where the claim acts, in
  place. `Release(handle)` ends it.
- **POINT** is the interop surface. You already have a package, your own, a
  vanilla one, or another mod's, and you want APMF to enforce just one facet
  of it rather than run the whole thing. You reference the package and name
  the facet, and APMF dissects it down to that one facet. Reach for this when
  you're integrating with something that predates APMF, or when a vanilla
  package already does what you want and you only need one piece of it.
  `kIntent_OfferPackage` is a POINT-style claim carried to its limit: point at
  a package and take every facet of it, and the engine runs it natively.

There used to be a third path: hand APMF a whole package and have it swap the
package in wholesale, byte-marshaled across the boundary. That model is
retired. It isn't documented here because it doesn't reflect what the code
does anymore, POINT replaced it.

Every request, on either surface, travels through the same small envelope:

```cpp
struct APMF_Param {
    RE::FormID   form;    // WHAT: a spell, item, actor, or package FormID
    float        fval;    // a scale (e.g. speed, disposition bias)
    std::int32_t ival;    // WHERE / a variant: a hand, a slot, a category mask
    RE::FormID   target;  // an actor this request acts ON
    float        posX, posY, posZ;  // a world location (reserved, not read yet)
};
```

Each intent reads only the fields it needs. `form` is always WHAT (the
spell, the item, the actor, the package). `ival` is usually WHERE or a
variant (which hand, which slot, a category bitmask). `fval` is a scale.
`target` is an actor the request acts on, distinct from WHAT: for a cast,
`form` is the spell you're casting and `target` is who it lands on. `pos` is
reserved for a future ground-target pass and has no effect today.

## Quickstart

### 1. Get the interface

Fetch the interface pointer once, after SKSE has loaded (`kPostLoad` or
`kDataLoaded` is a good spot), and keep it. APMF exports one C function,
`APMF_GetInterface`, returning a static POD struct you never free.

```cpp
#include "APMF_API.h"

namespace MyMod {

    const APMF_API::APMF_API_v4* g_apmf = nullptr;   // pick the newest struct you use

    void TryBindAPMF() {
        HMODULE h = GetModuleHandleA("APMF.dll");
        if (!h) return;   // APMF not installed. Degrade gracefully.

        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) return;

        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) return;   // ABI mismatch. APMF refused.

        if (base->abiVersion >= 4)
            g_apmf = reinterpret_cast<const APMF_API::APMF_API_v4*>(base);
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

### 4. A worked example: a driven cast

`kIntent_SelectSpell` (ch.8) is the one facet today where APMF does more than
arbitrate. Claim it with a hand and a target, and APMF equips the spell into
that hand and drives the actual animated cast. You don't write
`selectedSpells`, you don't touch a `MagicCaster`, and the effect always
lands (APMF drives the real animated sequence, or falls back to an immediate
cast if the drive can't animate, so delivery never depends on the animation
succeeding).

Offense first, casting at whoever the follower is fighting:

```cpp
APMF_Param param{};
param.form = fireballSpell;
param.ival = 0;   // 0 auto (a free hand, prefers right), 1 right, 2 left, 3 dual
// param.target left at 0: falls back to a winning kIntent_CombatTarget claim
// on this actor, then to self.

Handle castClaim = g_apmf->RequestEx(actor, kIntent_SelectSpell, /*basis=*/50.0f, &param);
```

Healing someone else is a different case, and it matters that you name the
target explicitly. A combat-target claim is the actor's FOE, not who to heal.
If you leave `target` at 0 for a heal spell, APMF's fallback resolves to
combat-target-or-self, which is wrong for "heal my ally." Name the ally:

```cpp
APMF_Param param{};
param.form   = healingSpell;
param.ival   = 0;             // auto hand
param.target = allyActorId;   // heal THIS actor, not whoever is being fought

Handle healClaim = g_apmf->RequestEx(actor, kIntent_SelectSpell, /*basis=*/50.0f, &param);
```

To cast again, `Repoint` the same handle with the same param, a repeat pulse.
To switch spell, hand, or target, `Repoint` with new values. `Release` when
the actor should stop casting and its hand should go back to normal use.

`pos` (a world-location target for a ground-effect spell) is in the struct
today but not read by anything yet. Pass zeros. A future release will wire it
without an ABI break.

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
| `kIntent_SelectSpell` (ch.8) | Claim the casting facet | `form` (the spell), `ival` (+ACT hand: 0 auto/1 right/2 left/3 dual), `target` (the cast target) | **Base mode field-proven, for the owned/exact cast.** A follower AI-fired an animated spell allowed only through APMF's cast-check gate, live in combat, no whack-a-mole, no crash. This covers the single claimed spell as the actor's only castable choice. **+ACT mode (new, this branch) is built and CI-green, field test pending:** APMF itself equips the spell and drives the animated cast, so the client no longer writes `selectedSpells` for this facet. The graduated multi-spell allow list (`SetSpellAllowList`, v4) is a separate capability and is not yet proven. Denying a competing framework's own spell selection is also still a future gap. |
| `kIntent_OfferPackage` (ch.9) | Claim the package-offer facet | `form` (the TESPackage FormID) | **Field-proven** for engage/release. A live deck run confirmed the redirect holds and releases cleanly. Save/load persistence of an engaged claim across that boundary is unexercised. |
| `kIntent_Dialogue` (ch.10) | Pause the actor's own in-progress dialogue | none | Built, not yet battle-tested |
| `kIntent_Disposition` (ch.11) | Aggression / confidence / assistance / morality bias | `fval` (reserved, not yet read) | **Field-proven.** An actor-value source-block, deck-tested to hold even on a package-locked actor. |
| `kIntent_Idle` (ch.12) | One-shot idle/animation | none | Built, not yet battle-tested |
| `kIntent_ShoutPower` (ch.14) | Claim the shout/power selection facet | `form` (the shout/power FormID) | Built, not yet battle-tested. Arbitration only today, the same shape as ch.6. |
| `kIntent_Equipment` (ch.15) | Unequip/equip a worn item, and (with a param) gate re-equip of a spell/staff while the claim stands | `form` (optional) | Built, not yet battle-tested. The most recently landed facet in the catalog. |
| `kIntent_Detection` (ch.16) | Silent movement + reduced detection range | `fval` (reserved, not yet read) | **Field-proven.** An actor-value source-block, deck-tested to hold even on a package-locked actor. |

Where a field is marked "reserved, not yet read", the channel currently
applies a fixed built-in behavior and ignores whatever you pass in that field.
The ABI accepts the value today so a later APMF release can start reading it
without an ABI break. Don't design a release around a reserved field doing
anything yet.

Read `Docs/CHANNEL-MAP.md` for the full research behind every row (the exact
hook, the exact vfunc, the version-robustness notes) and `Docs/ROADMAP.md` /
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
  into a NO for the one thing it's denying.
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
