# AI Package Management Framework (APMF)

APMF is an SKSE framework for Skyrim SE and AE. It sits between an actor and the game's AI
package system and acts as a traffic cop for control. Several mods can direct the same follower
or NPC at once, and none of them break each other.

## The problem it solves

Skyrim gives one actor to one package at a time. Whoever claims the actor at the highest
priority wins, and everyone else is locked out. A custom follower framework that holds a
follower blocks any other mod from moving or directing them. Mods that want to share control end
up fighting over the same actor, and one of them loses.

## What APMF does

APMF is a MODERATOR: it arbitrates WHO controls each facet of an actor and DENIES the losing
sources so the winner's own behavior reaches the actor. It NEVER generates behavior itself — it
calls no combat, cast, or movement command. A client mod does not claim a package or fight over
priority: it tells APMF which facet it is taking, on whatever basis it chooses, and EXECUTES the
behavior with its own proven mechanisms; APMF just makes it win and releases when the client is
done. Control is granular per-facet — e.g. a client can engage a follower's cast decision (so its
own AI casts the client's chosen spell, fully animated) while the follower keeps moving under its
own control, because only the cast facet was claimed.

Unaware mods and vanilla packages keep working. APMF never lies to them. It reports the actor's
true state, so a package that is waiting to reach a spot simply waits, exactly as it would if the
walk were slow. When APMF gives control back, that package finishes on its own.

Any aspect of AI control can be directed, not only movement. Stance and sneak, weapon draw,
combat targeting, headtracking, idle animations, aggression, dialogue availability, and more.

## How it works (short version)

One central hook on the actor update covers every NPC from a single install. It is a virtual
function hook, so it stays stable across game versions instead of breaking on each update.
Control is executed at the right layer for each facet, so a client can direct an actor without
overriding the package that owns it.

Full design is in [design.md](design.md).

## Status

Phase 1 built (see [Docs/STATUS.md](Docs/STATUS.md)). The central `Actor::Update` (0xAD) hook drives
a multi-NPC control map (keyed by FormID, scales to hundreds — an uncontrolled NPC pays one hash-miss),
a real inter-plugin C-ABI client API ([`native/APMF_API.h`](native/APMF_API.h): `Request`/`Release`
via `APMF_GetInterface`), and the full documented channel catalog (movement full-block, disposition,
casting selection, headtrack, combat-target steer, equipment, gait, detection, and more) as a
first-release baseline. Persisted AV overrides are co-saved so nothing strands across a save/load.
This is a framework for other mods to build on (MFO is the first client), not a standalone gameplay mod.

## License

MIT. See [LICENSE](LICENSE).
