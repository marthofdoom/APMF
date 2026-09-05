## Unreleased -- Composition rework (cast facet)

Branches `feat/composition-cast` then `feat/ai-cast-seats-impl`, field-unproven. Adds the cast-EXECUTION facet so a client can hand APMF a cast to moderate around while the actor keeps moving, instead of freezing the body with a package.

- New ABI v5 (`APMF_API.h`, append-only): `kIntent_Cast`, `RequestCast(actor, basis, APMF_CastRequest{spell, proxy, target, flags, ttl})`, `kCastFlag_FromPackage`, `kCombatActionCat_Cast`.
- A `kIntent_Cast` claim arbitrates and denies through the three gates cast-select already rides (CheckCast, CheckShouldEquip, the T1 cast leaves). APMF makes no cast write. The client fires its own animated cast. Movement stays the actor's.
- The claim is always TTL-bounded (default 4 s, max 15 s) and auto-releases. Never a standing hold, never a re-assert.
- `kCastFlag_FromPackage` extracts only the spell and target out of a handed package and never runs, offers, or evaluates it. If the package read is not clean on the pinned engine, the client passes the spell directly.
- Passive observe-only cast-path probe: watches a real NPC cast (the MagicCaster state machine plus the animation-graph event strings) and logs the sequence, so a client can replicate the proven path.
- Shelved the `feat/alias-drive` approach (package substitution). Tagged `archive/alias-drive-shelved-2026-09-04`. The shipping build carries no ESL, quest, or alias pool.
- Fixed the recurring combat-thread crash (`call [rax+0x28]`, rax=0). Root cause was the behavior-tree deny itself: a denied node ran ForceFail's act() but its own pop(), which unbalanced the thread's data stack. Every T1 deny is now ForceFail's act()+pop() pair (vtable slots 0x02+0x03). The deny refuses to arm if either half fails to resolve.
- A claimed cast is now performed by the NPC's own combat AI. Harbinger answers the five engine decision points the AI's cast choice is built out of, so the NPC picks up the spell, charges it, aims it and fires it at the target the client named, with the game's own animation, magicka cost, line of sight and interrupt handling. Harbinger itself casts nothing.
- Removed the forced cast drive that preceded it, including its guaranteed-delivery fallback. Nothing in Harbinger force-casts a spell any more.
- Removed the deny that stopped the AI building its magic context. It was the source of a long-running crash, and it now works against the cast it was meant to protect. A client that wants an actor not to cast at all still has that, through a combat-action claim.
- A self-only spell (Fast Healing and the like) always lands on its caster, whatever the AI is told to aim at, so Harbinger mints a delivery-flipped copy for the length of the claim and the AI casts that instead. The copy is taught and untaught around the cast and can never reach a save.
- A cast claim can now name the health percentage to stop at. Left unset, a channelled heal runs until the target is full.

## v0.9.0 -- First release

Harbinger (APMF) is a control layer for the AI that drives your NPCs. When more than one mod wants to steer the same actor, Harbinger decides which mod owns which part of it at any moment, so they run together instead of overwriting each other. It arbitrates per facet, not per actor, and withholds only the exact competing input while everything else keeps running.

This is the first public release. It ships as the framework marth's Follower Overhaul 2.0 is built on, and it is open for other mod authors to build against (see INTEGRATION.md and CHANNEL-MAP.md in the repo). It is deep engine work and it is new: some facets are proven in the field, others are built and ready but a mod author would be the first to run them, and the docs are honest about which is which.

- Requires SKSE64 and Address Library for SKSE Plugins. Anniversary Edition (1.6.x) only. No MCM, nothing to configure.
