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

APMF becomes the single point that decides what actually drives an actor. A client mod does not
claim a package or fight over priority. It tells APMF what it needs the actor to do, on whatever
basis it chooses, and releases when it is done. APMF arbitrates every request and hands out
control.

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

Early. The mechanics are verified and the hook is found. The first prototype (the movement path)
is next. This is a framework for other mods to build on, not a standalone gameplay mod.

## License

MIT. See [LICENSE](LICENSE).
