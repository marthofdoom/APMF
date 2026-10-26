# AI Package Management Framework (APMF)

APMF ("Harbinger") is an SKSE framework for Skyrim on Windows. Its cast seats
open on **Anniversary Edition 1.6.1170**, **Special Edition 1.5.97** and **1.7.104** (Steam), behind
exact-version gates that refuse any other build by name, and VR is refused
outright. (This line said AE only until v0.9.5, which placed the verified 1.5.97
values.) It's a scalpel, not a takeover. A mod claims exactly the facet of an
NPC it needs, gets it without a fight, and everything else about that NPC
(movement, other packages, the rest of its AI) keeps running untouched. When the mod releases
the facet, the actor's own AI takes it straight back.

It's for SKSE plugin authors whose mod directs NPCs and wants to stop
fighting other mods over who owns the actor.

**Start here:**

- **[Docs/INTEGRATION.md](Docs/INTEGRATION.md)**: the getting-started quickstart for mod authors. Read this first.
- **[Docs/GUIDE.md](Docs/GUIDE.md)**: the guide. What a facet is, how claims are ranked, how a claim lives and dies, threading, binding.
- **[Docs/FACETS.md](Docs/FACETS.md)**: the facet reference. Every facet, its arguments, what it enforces, its lifetime, and examples.
- **[Docs/RECIPES.md](Docs/RECIPES.md)**: worked scenarios that combine facets, with complete code.
- **[Docs/GLOSSARY.md](Docs/GLOSSARY.md)**: every term, defined once.
- **[Docs/CHANNEL-MAP.md](Docs/CHANNEL-MAP.md)**: the full facet map, with an honest proof tier per facet.
- **[native/APMF_API.h](native/APMF_API.h)**: the header you build against. The whole public surface, one file.
- **[Docs/INI-REFERENCE.md](Docs/INI-REFERENCE.md)**: every key in `APMF.ini` and the format of `APMF_Sources.ini`.
- **[Docs/HARBINGER-MENU.md](Docs/HARBINGER-MENU.md)**: a short guide to the Harbinger page in the SKSE Menu Framework.
- **[CHANGELOG.md](CHANGELOG.md)**: what changed in each release.

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

## Facets

A facet is one part of an NPC's AI that a mod can claim. Each has its own intent in the API, its own rules, and a section in **[Docs/FACETS.md](Docs/FACETS.md)** that covers what it is for, the exact arguments, what Harbinger enforces, how long a claim lives, who wins a conflict, and short C++ examples.

| Facet | What it is for | Reference |
|---|---|---|
| Movement block (ch.1) | Stand the actor still | [ch.1](Docs/FACETS.md#ch1-movement-block-kintent_movementblock) |
| Gait (ch.1a) | Halve the actor's speed | [ch.1a](Docs/FACETS.md#ch1a-gait-kintent_gait) |
| Stance (ch.3) | Make the actor sneak | [ch.3](Docs/FACETS.md#ch3-stance-kintent_stance) |
| Weapon drawn (ch.4) | Draw the weapon, sheathe on release | [ch.4](Docs/FACETS.md#ch4-weapon-drawn-kintent_weapondrawn) |
| Headtrack (ch.5) | Make the actor look straight up (a test facet) | [ch.5](Docs/FACETS.md#ch5-headtrack-kintent_headtrack) |
| Combat target (ch.6) | Record who owns the combat target | [ch.6](Docs/FACETS.md#ch6-combat-target-kintent_combattarget) |
| Combat action (ch.7) | Deny attack or cast behaviour | [ch.7](Docs/FACETS.md#ch7-combat-action-kintent_combataction) |
| Select spell (ch.8) | Limit the spells the AI may cast | [ch.8](Docs/FACETS.md#ch8-select-spell-kintent_selectspell) |
| Cast (ch.8b) | Make the NPC's own AI cast a spell at a target, animated | [ch.8b](Docs/FACETS.md#ch8b-cast-kintent_cast) |
| Position cast | Cast a Target Location spell at a point (off by default) | [position cast](Docs/FACETS.md#position-cast-kcastflag_atposition) |
| Offer package (ch.9) | Hand the actor one AI package | [ch.9](Docs/FACETS.md#ch9-offer-package-kintent_offerpackage) |
| Dialogue (ch.10) | Pause the actor's own dialogue once | [ch.10](Docs/FACETS.md#ch10-dialogue-kintent_dialogue) |
| Disposition (ch.11) | Make the actor aggressive and loyal | [ch.11](Docs/FACETS.md#ch11-disposition-kintent_disposition) |
| Idle (ch.12) | Play an animation, optionally at a target | [ch.12](Docs/FACETS.md#ch12-idle-kintent_idle) |
| Shout and power (ch.14) | Record who owns the shout facet | [ch.14](Docs/FACETS.md#ch14-shout-and-power-kintent_shoutpower) |
| Equipment (ch.15) | Gate spell and staff re-arm | [ch.15](Docs/FACETS.md#ch15-equipment-kintent_equipment) |
| Detection (ch.16) | Silent movement, longer detect range | [ch.16](Docs/FACETS.md#ch16-detection-kintent_detection) |
| Equip authority (ch.17) | Declare what the NPC wears and refuse the rest | [ch.17](Docs/FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| Travel (ch.19) | Walk the NPC to an object, cell or point | [ch.19](Docs/FACETS.md#ch19-travel-kintent_travel) |
| Target pin (ch.20) | Hold the combat target on yours | [ch.20](Docs/FACETS.md#ch20-target-pin-kintent_targetpin) |
| Combat entry (ch.21) | Start a fight | [ch.21](Docs/FACETS.md#ch21-combat-entry-kintent_combatentry) |
| Combat re-entry deny (ch.22) | Keep the NPC out of combat for a while | [ch.22](Docs/FACETS.md#ch22-combat-re-entry-deny-kintent_combatreentrydeny) |
| Pursuit leash (ch.23) | Stop the NPC chasing too far from an anchor | [ch.23](Docs/FACETS.md#ch23-pursuit-leash-kintent_pursuitleash) |
| Combat approach (ch.24) | Close on an actor while fighting | [ch.24](Docs/FACETS.md#ch24-combat-approach-kintent_combatapproach) |
| Cast guard (ch.25) | Hold attack spells through friends, bound channels | [ch.25](Docs/FACETS.md#ch25-cast-guard-kintent_castguard) |
| Space queries | Ask for a clear spot or the hostiles in a sphere | [queries](Docs/FACETS.md#space-queries-findemptyspace-and-findhostilesinspace) |
| Line of sight | Harbinger's own ray between two actors | [queries](Docs/FACETS.md#line-of-sight-getlineofsight) |
| Awareness | Does this actor sense that one | [queries](Docs/FACETS.md#awareness-senseactor) |
| Observe-only seats | Who changes an actor value, the seat census, package sources | [observe only](Docs/FACETS.md#observe-only-facets-no-claim-api) |

Some facets are field-proven and some are built but not yet seen working in game. FACETS.md says which for each one.

## For mod authors

You build against one header, [`native/APMF_API.h`](native/APMF_API.h). Start with [Docs/INTEGRATION.md](Docs/INTEGRATION.md) for how to find the interface, check the version, register as a source and run the claim, heartbeat and release pattern. [Docs/GUIDE.md](Docs/GUIDE.md) is the model behind it. Then use [Docs/FACETS.md](Docs/FACETS.md) as the reference for each facet and [Docs/RECIPES.md](Docs/RECIPES.md) for scenarios that combine them. [MFO](https://github.com/marthofdoom/MFO) is the worked example, and its `native/apmf/` folder is the one to read.

## Quickstart

```cpp
#include "APMF_API.h"

// The newest struct is APMF_API_v19 (ABI 21). APMF_API.h is canonical, never hardcode
// the number in your own docs.
const APMF_API::APMF_API_v19* g_apmf = nullptr;

// Once, from kPostLoad/kDataLoaded:
if (HMODULE h = GetModuleHandleA("APMF.dll")) {
    auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
        GetProcAddress(h, APMF_API::kGetInterfaceExport));
    if (fn) {
        // fn() returns nullptr if the installed APMF is OLDER than the version you ask for,
        // and the newest interface for any version at or below its own. Ask for the LOWEST
        // ABI you can live with, then gate every newer slot on base->abiVersion.
        if (auto* base = fn(/*lowest ABI you need*/ 5)) {
            if (base->abiVersion >= 5)
                g_apmf = reinterpret_cast<const APMF_API::APMF_API_v19*>(base);
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

The full walkthrough, with `RequestEx`, `Repoint`, the pending-claim rule, threading and the
claim, heartbeat and release pattern, is in [Docs/INTEGRATION.md](Docs/INTEGRATION.md). Every facet is in
[Docs/FACETS.md](Docs/FACETS.md).

## How it works (short version)

One central hook on the actor update covers every NPC from a single install.
It's a virtual function hook, so it stays stable across game versions instead
of breaking on each update. Control is executed at the right layer for each
facet, so a client can direct an actor without overriding the package that
owns it.

Full design is in [design.md](design.md).

## Requirements

- Skyrim on **Windows**: Anniversary Edition **1.6.1170**, Special Edition **1.5.97**, or
  **1.7.104** (Steam). Every hook and every engine value Harbinger uses was checked on
  each of these three builds, and each opens on all three. Any other build is refused
  by name, including a 1.6.x that isn't 1170, so nothing runs unverified. VR is refused
  at install throughout.
- On **1.7.104** there is no Address Library, and none is needed. The id table for
  1.7.104 is built into Harbinger's DLL by the CommonLibSSE-NG fork it is built on,
  so there is nothing extra to download or install.
- Optional: SKSE Menu Framework 3.x for the Harbinger menu page. Without it Harbinger runs from `APMF_Sources.ini` and logs one line.
- Build: CMake 3.21+, a C++23 compiler, [vcpkg](https://vcpkg.io) with the
  `commonlibsse-ng` port (see `native/vcpkg.json` / `native/vcpkg-configuration.json`).
- A client mod links only against `native/APMF_API.h`, a plain C-ABI header
  with no CommonLib dependency of its own.

## Install

The archive's root is your `Data` folder, so a mod manager installs it with nothing to
place by hand. Three files come out of it, and all three have to be there. A fourth file,
`SKSE/Plugins/APMF_Sources.ini`, is optional and is written by the menu page when you
change a priority (format in `Docs/INI-REFERENCE.md`):

- `SKSE/Plugins/APMF.dll`, the plugin itself.
- `SKSE/Plugins/APMF.ini`, the settings file. Every key is documented in it and set to
  the configuration that was tested. Deleting the file, or any line in it, is safe.
  Every key is also listed in [Docs/INI-REFERENCE.md](Docs/INI-REFERENCE.md), including
  `bEquipEnforceArmor` and the `[Harbinger]` section's `bSceneProbe`.
- `APMF.esl`, at the root, beside `SKSE`.

`APMF.esl` is new in v0.9.5 and it has to be **enabled** in your load order like any
other plugin. It's ESL flagged, so it takes no regular load-order slot (it uses one of
the 4096 light slots), it has one master (Skyrim.esm), it overrides nothing, and it's
about 2.5 KB. It carries the travel packages the travel facet (ch.19) hands the engine,
and it is generated from `APMF_GenerateESL.py` rather than hand-authored.

If it's missing or disabled, APMF refuses every `kIntent_Travel` claim and names the
reason once in the log, so a client's own degrade path runs. Nothing else breaks. Every
other facet behaves exactly the same without it.

## What is in v0.15.0

- **Travel across cells (ABI v30).** The `kTravel_CrossCells` flag on a travel claim to a reference makes the follower walk through doors to that NPC, the game's own way, and follow them if they move. It arrives in the same interior cell or the same outdoor worldspace and inside the radius, then is held until the mod releases it. Without the flag nothing changes.
- **Client facet priority (ABI v29).** `GetClientFacetPriority` returns the calling mod's own priority on a facet. 0 means off.
- **The combat substrate holds the movement half** of the follower's combat tree instead of failing it, and its time limit runs from the tree's start. `bSubstrateMovementHold` and `fSubstrateTtlSec` in `[CombatSubstrate]`. A passive `CLAIM SCORE` line shows what the game scored the claimed spell.
- Needs ABI v30 for the new calls. Older clients keep working.

### v0.14.0

- **Place (ABI v28).** `kIntent_Place` moves an NPC beside a reference or to a point in an interior cell, once, through the game's own move call. `GetPlaceState` says what happened.
- **Equip enforced for every declared category by default,** and an equip claim can name one copy of an item (ABI v25).
- **Weapon rule (ch.15)** at the combat equip seat, with a stand down when the claimed weapon is not one the AI could pick. **Torch seat (ch.17)** switches off the game's own torch logic while a claim owns the torch. `bEquipmentWeaponAdmission`, `bTorchDeny` in `APMF.ini`.
- **Travel (ch.19).** A leg to a cell arrives by parent cell and holds the actor until the mod releases or re-points it. A re-point takes a fresh package slot. A passive DONE-WHILE-WALKING line names a walk that ended early.
- **The combat substrate (ch.27)** installs on 1.7.104, lets the game's AI equip the claimed spell, denies the enter-combat override package, battle music and barks, and reports its state with `GetSubstrateState` (ABI v26) and `IsSubstrateAvailable` (ABI v27). `[CombatSubstrate]` in `APMF.ini`.
- Needs ABI v28 for the new calls. Older clients keep working.

### v0.13.1

- **Held engagements.** A fight a mod starts or pins is held until the target is dead, disabled or unloaded, or the mod lets go. The follower chases the target the game's way, walks to a downed one, walks back from the edge of its leash, and gives up with a logged anomaly if it makes no progress. `[EngagementHold]` and `[PursuitLeash]` in `APMF.ini`.
- **The combat substrate (ABI v23, on by default).** A mod claims `kIntent_CombatSubstrate` beside its cast claim and the follower casts its spell animated, out of a real fight, against an invisible stand-in. `[CombatSubstrate]` in `APMF.ini`.
- **Idle state (ABI v24).** `GetIdleState` says what became of an idle claim's play.
- **Equip preference (ABI v22).** A mod names the weapons, shields, torches or staves it prefers in combat and Harbinger lifts them in the game's own scoring.
- **Equip refusals follow the priorities,** and heals look past friends.
- Needs ABI v24 for the new calls. Older clients keep working.

### v0.13.0

- **Harbinger names who changes the AI, in more places.** Combat entry, combat actions, combat approach, spell choice and cast claims now say whose answer they replaced, and a claim stands only while it outranks that source in `APMF_Sources.ini`. With the default priorities nothing changes.
- **Unknown sources checkbox.** Each facet has "Unknown sources can be overridden" on the Harbinger page. Off by default. `[UnknownSources]` in `APMF_Sources.ini`.
- **Spectator seat.** A retreating follower no longer stops to watch the fight while a mod's package claim outranks the engine. `iSpectatorSeat` in `[PackageGate]` of `APMF.ini`.
- **Actor value watch.** Harbinger names who changes a follower's speed, disposition, sneak noise and detection range values. Watch only. `iActorValueSeat` in `[ActorValueObserve]`.
- **Loaded plugin list.** The Harbinger page lists every loaded SKSE plugin, including ones that have not written anything yet.
- No ABI change. Still ABI v21.

### v0.12.0

- **Cast guard (ch.25, ABI v21).** A mod can ask Harbinger to hold an NPC's attack spell while a friend stands in the line of fire or the blast, and to keep the NPC's own concentration spells off one hand or both. Harbinger measures the line every frame. `Docs/FACETS.md`, "ch.25 Cast guard".
- **The controller (Phase 1a).** Harbinger keeps a registry of every source that writes an AI facet: client mods, other mods' hook DLLs, plugins, the engine, Papyrus and console calls. Each has a priority from 0 to 100 per facet, set in `APMF_Sources.ini`. Defaults are 20 for other mods, 50 for clients and 75 for direct calls. A change Harbinger cannot trace is allowed and alerted, never denied. `Docs/FACETS.md`, "Basis and priority".
- **Target restore.** When a named source is outranked on the combat target, Harbinger puts the target back through the engine's own call.
- **Seat census.** Every engine seat Harbinger hooks is re checked at each load stage and every 60 seconds, so a mod that hooks over it later is named in the log and on the menu.
- **Equip caller recovery.** The mod behind an engine equip is found by a stack scan. An equip Harbinger cannot attribute is never denied.
- **The Harbinger menu page.** A page in the SKSE Menu Framework shows the sources per facet, the winner, alerts and the census, and edits priorities live. `Docs/HARBINGER-MENU.md`.
- **Armor enforced for clients.** `bEquipEnforceArmor=1` by default: an outfit or the game can no longer replace the armor a client chose, and a stripped follower gets it back within seconds.
- **Own line of sight from the eyes,** a heal approach goal kept inside the pursuit leash, and a disarmed actor's weapon search left alone by the approach bound.

## Status

Phase 1 built, now at v0.15.0, ABI v30 (see [Docs/STATUS.md](Docs/STATUS.md)). The central
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
