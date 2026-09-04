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

APMF is a MODERATOR. It arbitrates who controls each facet of an actor and
denies the losing sources, so the winner's own behavior reaches the actor. It
never generates behavior itself. It calls no combat, cast, or movement
command. A client mod doesn't claim a package or fight over priority: it
tells APMF which facet it's taking, on whatever basis it chooses, and
executes the behavior with its own proven mechanisms. APMF just makes it win
and releases when the client is done.

Control is granular, per facet. A client can claim a follower's cast
decision, so its own AI casts the client's chosen spell, fully animated,
while the follower keeps moving under its own control, because only the cast
facet was claimed.

Unaware mods and vanilla packages keep working. APMF never lies to them. It
reports the actor's true state, so a package that's waiting to reach a spot
simply waits, exactly as it would if the walk were slow. When APMF gives
control back, that package finishes on its own.

Any aspect of AI control can be directed, not only movement: stance and
sneak, weapon draw, combat targeting, headtracking, idle animations,
aggression, dialogue availability, and more. See
[Docs/CHANNEL-MAP.md](Docs/CHANNEL-MAP.md) for the full list and how proven
each one is.

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
