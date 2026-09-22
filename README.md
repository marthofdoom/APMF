# AI Package Management Framework (APMF)

APMF ("Harbinger") is an SKSE framework for Skyrim on Windows. Its cast seats
open on **Anniversary Edition 1.6.1170** and **Special Edition 1.5.97**, behind
exact-version gates that refuse any other build by name, and VR is refused
outright. (This line said AE only until v0.9.5, which placed the verified 1.5.97
values.) It's a scalpel, not a takeover. A mod claims exactly the facet of an
NPC it needs, gets it without a fight, and everything else about that NPC
(movement, other packages, the rest of its AI) keeps running untouched. When the mod releases
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

// Pick the NEWEST struct you actually call -- APMF_API_v9 is the current one
// (APMF_API.h is canonical, never hardcode the number in your own docs).
const APMF_API::APMF_API_v9* g_apmf = nullptr;

// Once, from kPostLoad/kDataLoaded:
if (HMODULE h = GetModuleHandleA("APMF.dll")) {
    auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
        GetProcAddress(h, APMF_API::kGetInterfaceExport));
    if (fn) {
        // fn() returns nullptr if the installed APMF is OLDER than the header you
        // compiled against, so this single call is all-or-nothing: if you want a
        // graceful degrade to an older APMF, ask for the lowest version you can
        // live with and feature-test with base->abiVersion from there.
        if (auto* base = fn(APMF_API::kABIVersion)) {
            if (base->abiVersion >= 9)
                g_apmf = reinterpret_cast<const APMF_API::APMF_API_v9*>(base);
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

- Skyrim on **Windows**: Anniversary Edition **1.6.1170** or Special Edition
  **1.5.97**. The cast-classify seat (`core/CastClassify.cpp`) and the weapon-score
  seat (`core/AiCastSeats.cpp` Group C) are placed per runtime from values verified
  against each binary, and both open on both. Any other build is refused by name,
  including a 1.6.x that isn't 1170, so nothing runs unverified. VR is refused at
  install throughout, and 1.7.104 is unsupported because there's no address library
  for it. (Corrected at v0.9.5. This said AE only, the cast path refuses to install
  on SE 1.5.97.)
- Build: CMake 3.21+, a C++23 compiler, [vcpkg](https://vcpkg.io) with the
  `commonlibsse-ng` port (see `native/vcpkg.json` / `native/vcpkg-configuration.json`).
- A client mod links only against `native/APMF_API.h`, a plain C-ABI header
  with no CommonLib dependency of its own.

## Install

The archive's root is your `Data` folder, so a mod manager installs it with nothing to
place by hand. Three files come out of it, and all three have to be there:

- `SKSE/Plugins/APMF.dll`, the plugin itself.
- `SKSE/Plugins/APMF.ini`, the settings file. Every key is documented in it and set to
  the configuration that was tested. Deleting the file, or any line in it, is safe.
- `APMF.esl`, at the root, beside `SKSE`.

`APMF.esl` is new in v0.9.5 and it has to be **enabled** in your load order like any
other plugin. It's ESL flagged, so it takes no regular load-order slot (it uses one of
the 4096 light slots), it has one master (Skyrim.esm), it overrides nothing, and it's
about 2.5 KB. It carries the travel packages the travel facet (ch.19) hands the engine,
and it is generated from `APMF_GenerateESL.py` rather than hand-authored.

If it's missing or disabled, APMF refuses every `kIntent_Travel` claim and names the
reason once in the log, so a client's own degrade path runs. Nothing else breaks. Every
other facet behaves exactly the same without it.

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
