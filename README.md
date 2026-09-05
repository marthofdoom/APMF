# AI Package Management Framework (APMF)

APMF ("Harbinger") is an SKSE framework for Skyrim SE and AE. It's a scalpel,
not a takeover. A mod claims exactly the facet of an NPC it needs, gets it
without a fight, and everything else about that NPC (movement, other
packages, the rest of its AI) keeps running untouched. When the mod releases
the facet, the actor's own AI takes it straight back.

It's for SKSE plugin authors whose mod directs NPCs and wants to stop
fighting other mods over who owns the actor.

**Start here:**

- **[Docs/INTEGRATION.md](Docs/INTEGRATION.md)**: the integration guide. Read this first.
- **[Docs/CHANNEL-MAP.md](Docs/CHANNEL-MAP.md)**: the full facet map, with an honest proof tier per facet.
- **[native/APMF_API.h](native/APMF_API.h)**: the header you build against. The whole public surface, one file.

## The problem it solves

Skyrim gives one actor to one package at a time. Whoever claims the actor at
the highest priority wins, and everyone else is locked out. A custom follower
framework that holds a follower blocks any other mod from moving or directing
them. Mods that want to share control end up fighting over the same actor,
and one of them loses.

## What APMF does

APMF is a MODERATOR, not a package swap. A client DECLARES what it wants and
where. APMF ENFORCES it: it arbitrates who owns each facet of an actor,
denies the competing sources down to zero influence, and for a growing set of
facets now EXECUTES that facet itself. For a claimed cast, for example, APMF
equips the spell into the right hand and drives the actual animation, so the
client never has to touch the engine's cast machinery at all.

This is an evolution from APMF's earlier model, where it only arbitrated and
the client always executed. That old line still holds for most facets today
(combat targeting, shout selection, and casting's base mode all still work
that way), but it is no longer the whole story. What never changes: APMF
never substitutes a whole package. It doesn't take over an actor's AI or tear
down the package that actor is already running, it keeps that package's slot
intact. And APMF never fabricates input a client didn't declare. It composes
only the facets a client actually asked for, nothing more.

Control is granular, per facet. A client can claim a follower's cast
decision, so a chosen spell fires, fully animated, while the follower keeps
moving under its own control, because only the cast facet was claimed.

Unaware mods and vanilla packages keep working. APMF never lies to them. It
reports the actor's true state, so a package that's waiting to reach a spot
simply waits, exactly as it would if the walk were slow. When APMF gives
control back, that package finishes on its own.

Any aspect of AI control can be directed, not only movement: stance and
sneak, weapon draw, combat targeting, headtracking, idle animations,
aggression, dialogue availability, and more. See
[Docs/CHANNEL-MAP.md](Docs/CHANNEL-MAP.md) for the full list and how proven
each one is.

## How you use it: ASK or POINT

There are two ways to hand APMF a request. Both end up at the same enforcer.

- **ASK** (the recommended surface for almost everything). You declare intent
  directly with a simple command: `RequestEx(actor, Intent, basis, param)`
  returns a `Handle`. `Repoint(handle, param)` changes what or where the
  claim acts, in place, no release needed. `Release(handle)` ends it. This is
  the surface to reach for first.
- **POINT** (the interop surface). You already have a package, either your
  own, a vanilla one, or another mod's, and you want APMF to take just one
  facet out of it rather than run the whole thing. You reference the package
  and name the facet, and APMF dissects it and enforces only that piece. This
  is how APMF stays compatible with mods that were never written against it.
  `kIntent_OfferPackage` is a POINT-style claim too: point at a package and
  take all of it, letting the engine run it natively.

Either way, the request travels through one small POD envelope:

```cpp
struct APMF_Param {
    RE::FormID   form;    // WHAT: a spell, item, actor, or package FormID
    float        fval;    // a scale (e.g. speed, disposition bias)
    std::int32_t ival;    // WHERE / a variant: a hand, a slot, a category mask
    RE::FormID   target;  // an actor this request acts ON
    float        posX, posY, posZ;  // a world location (reserved)
};
```

Every intent uses the fields it needs and ignores the rest. A cast claim, for
example, uses `form` for the spell, `ival` for which hand, and `target` for
who to cast it at:

```cpp
APMF_API::APMF_Param param{};
param.form   = fireballSpell;
param.ival   = 0;              // 0 auto, 1 right, 2 left, 3 dual
param.target = targetActorId;  // 0 falls back to self or a combat-target claim

auto handle = g_apmf->RequestEx(actorFormID, APMF_API::kIntent_SelectSpell,
                                 /*basis=*/50.0f, &param);
// APMF equips the spell and drives the animated cast. Repeat with the same
// param to cast again, Repoint with a new one to switch, Release when done.
```

## Quickstart

```cpp
#include "APMF_API.h"

const APMF_API::APMF_API_v4* g_apmf = nullptr;

// Once, from kPostLoad/kDataLoaded:
if (HMODULE h = GetModuleHandleA("APMF.dll")) {
    auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
        GetProcAddress(h, APMF_API::kGetInterfaceExport));
    if (fn) {
        if (auto* base = fn(APMF_API::kABIVersion)) {
            if (base->abiVersion >= 4)
                g_apmf = reinterpret_cast<const APMF_API::APMF_API_v4*>(base);
        }
    }
}

// Claim a facet, drive it yourself, release when done:
if (g_apmf) {
    auto claim = g_apmf->Request(actorFormID, APMF_API::kIntent_MovementBlock, /*basis=*/50.0f);
    // ... your own logic while you hold it ...
    g_apmf->Release(claim);
}
```

The full walkthrough, with `RequestEx`, `Repoint`, threading rules, and the
arbitration model, is in [Docs/INTEGRATION.md](Docs/INTEGRATION.md).

## How it works (short version)

One central hook on the actor update covers every NPC from a single install.
It's a virtual function hook, so it stays stable across game versions instead
of breaking on each update. Control is executed at the right layer for each
facet, so a client can direct an actor without overriding the package that
owns it.

Full design is in [design.md](design.md).

## Requirements

- Skyrim SE or AE (Windows). VR is refused at install, not supported.
- Build: CMake 3.21+, a C++23 compiler, [vcpkg](https://vcpkg.io) with the
  `commonlibsse-ng` port (see `native/vcpkg.json` / `native/vcpkg-configuration.json`).
- A client mod links only against `native/APMF_API.h`, a plain C-ABI header
  with no CommonLib dependency of its own.

## Status

Phase 1 built (see [Docs/STATUS.md](Docs/STATUS.md)). The central
`Actor::Update` (0xAD) hook drives a multi-NPC control map (keyed by FormID,
scales to hundreds, an uncontrolled NPC pays one hash-miss), a real
inter-plugin C-ABI client API (`native/APMF_API.h`), and the full documented
channel catalog (movement full-block, disposition, casting selection,
headtrack, combat-target steer, equipment, gait, detection, and more) as a
first-release baseline. Persisted actor-value overrides are co-saved, so
nothing strands across a save/load.

This is a framework for other mods to build on, not a standalone gameplay
mod. [MFO (marth's Follower Overhaul)](https://github.com/marthofdoom/MFO) is
the worked example, the first real client integrating against it.

## License

MIT. See [LICENSE](LICENSE).
