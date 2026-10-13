# APMF.ini and APMF_Sources.ini reference

Every setting Harbinger reads, with its default. This file is generated from the comments in `Data/SKSE/Plugins/APMF.ini`, which is the source of truth: that file ships in the zip and every key in it is also documented there. Deleting the file or any line is safe. Harbinger falls back to the same compiled default and logs normally. `1` is on and `0` is off unless a key says otherwise.

Harbinger needs no configuration to work. The defaults are the tested configuration.

For what each facet does, its arguments and its rules, see [FACETS.md](FACETS.md). Each section below links to the facet it switches.

## APMF.ini

Path: `Data/SKSE/Plugins/APMF.ini`. Read once at startup.

### [CastSeats]

Facet reference: [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast).

the seats that let a client's chosen spell actually be cast by the NPC's own AI. These are the feature, not diagnostics. Leave them on.

- **`EnableSeat0Classify`** (default `1`). Lets a heal aimed at someone else be understood by the game's own spell classifier. Without it a client's heal-other cannot be selected by the AI.
- **`EnableAimSeat`** (default `1`). Lets the claimed spell be aimed at the claimed target.

### [EquipGate]

Facet reference: [ch.8 Select spell](FACETS.md#ch8-select-spell-kintent_selectspell), [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast), [ch.15 Equipment](FACETS.md#ch15-equipment-kintent_equipment).

who is allowed to put something in a claimed hand.

- **`EnableEquipDenyComplete`** (default `1`). While a client holds a hand, the AI may not re-equip a different spell or staff into it. This is what stops a cast being swapped out mid-cast.
- **`EnableEquipGateLog`** (default `0`). Verbose per-decision equip logging. Diagnosis only, noisy.
- **`EnableDualWieldPreference`** (default `0`). Prefer a second one-handed weapon over a shield for a perked follower. Off: it needs per-hand score data that does not exist yet.

### [AiCastSeats]

Facet reference: [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast).

score observation, and the one score lever.

- **`EnableScoreSteer`** (default `1`). Lifts the AI's own combat score for an item a client prefers, so the AI picks it by its own rules: a spell a client's cast claim drives, or a weapon, shield, torch or staff a client declared with the equip preference (ABI v22). The lift is one step above the best other item in the same group, never from a score of 0, and only while the client outranks the source beneath the score in `APMF_Sources.ini`. `0` turns every lift off and refuses equip preference claims. Compiled default is 1 since ABI v22 (it was 0 in code and 1 in the shipped file).
- **`EnableWeaponScoreProbe`** (default `1`). Observe-only probes. All three only add log lines, none change behaviour. `EnableWeaponScoreProbe` also installs the weapon score seat the equip preference lifts weapons, shields and torches through: `0` leaves only spells and staves.
- **`EnableItemScoreProbe`** (default `0`). 
- **`EnableCasterSeatProbe`** (default `0`). 

### [EquipAuthority]

Facet reference: [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority), [probe criteria](FACETS.md#appendix-e-equip-authority-probe-criteria).

the engine-equip facet, taken whole (ABI v7/v8, ch.17). A client declares what a follower wears. APMF equips that set and refuses every other engine equip of armor, weapons, ammo or a torch on that actor (outfit re-apply, AI weapon or armor choice, combat re-arm) until the client re-declares or releases. Potions, food, scrolls, ingredients and books are never governed. The player dressing the follower through the trade or gift menu is allowed unless the client asks for the strict form. The player and any actor without a claim are never touched. Docs/INVARIANTS.md #17a.

- **`bEquipAuthority`** (default `1`). Installs the seat. 0 = the seat is not installed and kIntent_EquipAuthority claims are REFUSED (the client keeps its own equips; logged once).
- **`bEquipObserveOnly`** (default `1`). 1 = log every verdict as would-deny and refuse nothing. THE FIRST FIELD BUILD SHIPS WITH THIS ON: it stays on until the probe criteria in Docs/FACETS.md Appendix E pass (every engine re-equip of an off-set item logs would-deny with a named path, zero would-deny with tls>0, zero Unknown paths). Set to 0 to enforce.
- **`bEquipEnforceArmor`** (default `1`). 1 = enforce ARMOR verdicts even while bEquipObserveOnly=1 (weapons, shields, torches and ammo stay observe-only). marth 2026-10-06: MFO is the armor equip authority until the weighting rework. Set to 0 to make armor observe-only again.
- **`bObserveUnequip`** (default `1`). 1 = log every unequip of armor, a weapon, ammo or a torch on a follower under a ch.17 or ch.26 claim, as `[apmf][equip-obs] op=unequip` (who asked, and the equip that displaced it). Watch only: every unequip still happens. 0 = not watched. Needs the equip sink (`bEquipAuthority=1`). Refused on 1.7.104 for now: its ids are not in the MIT id table.
- **`bEquipDenyScript`** (default `0`). 1 = also refuse Papyrus (EquipItem/EquipItemEx) and console equips of items outside the declared set. Off: scripts are a quest author's deliberate act. Scope (APMF-B65): this also applies on an actor with NO ch.17 claim whose hand a live cast claim holds (the hand-claim block reads it there too): with it set, a Papyrus / console equip into that held hand is refused as well. Since equip refusals follow the priorities (2026-10-12), a script or console equip ranks as `DirectCalls` (75 by default) on the Equip facet. A client at the default 50 does not outrank it, so with the default priorities this setting refuses nothing. It only takes effect for a client raised above DirectCalls in `APMF_Sources.ini` `[Equip]`.

### [HandBlock]

Facet reference: [ch.8b hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly), [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority).

a hand taken for a cast is closed to every other action. While a cast claim holds a hand (a heal, an attack spell, or a client's "keep this hand empty" floor), the follower cannot put a weapon, a shield or a torch into that hand. The combat AI never picks one for it, and any other equip into it is refused. The other hand is untouched. When the claim ends, the follower arms itself again as usual. This is enforced whatever bEquipObserveOnly says.

- **`bHandClaimBlocksEquip`** (default `1`). 0 = off: a weapon may be equipped into a hand a cast claim holds.

### [PackageGate]

Facet reference: [ch.9 Offer package](FACETS.md#ch9-offer-package-kintent_offerpackage).

package redirection (what makes a follower walk to a body).

- **`EnableRedirectLog`** (default `1`). Logs each redirect answer, including a heartbeat when none fire, so "never fired" is visibly different from "nothing was logged".
- **`iSpectatorSeat`** (default `2`). The engine's Spectator package (a follower out of combat stops to watch a fight) is held off while a mod's package claim on that follower outranks the engine. 2 = on, 1 = log only (the package is still given), 0 = off.

### [ActorValueObserve]

Facet reference: [actor-value watch](FACETS.md#actor-value-watch-detection-and-actorvalue-facets).

who changes the actor values a mod asked Harbinger to set. While a mod holds a follower's sneak noise, detection range, speed or disposition values through Harbinger, a change to one of them by the game, a script or another mod is named in the log and on the Harbinger page. Watch only: nothing is ever refused or changed.

- **`iActorValueSeat`** (default `1`). 1 = watch, 0 = off.

### [Travel]

Facet reference: [ch.19 Travel](FACETS.md#ch19-travel-kintent_travel).

ch.19, ABI v10. "Walk this follower over there." A mod names an actor and a destination -- any object in the world, or a whole cell -- and Harbinger walks the actor there under its own travel package (Data/APMF.esl, which ships beside this file), stopping about 75 units short of an object or once the actor is inside a cell, and lets go the moment that actor is in combat. That is the whole feature. Harbinger does not pick a fight, does not claim the target and does not pretend anyone has seen anything. A mod that wants those asks for them separately.

- **`bTravel`** (default `1`). Installs the channel. 0 = kIntent_Travel claims are REFUSED (the mod keeps doing whatever it does on its own; logged once with the reason).
- **`bGateProbe2`** (default `0`). Passive gate probe 2 (observe-only, changes nothing). When a travel leg ends BLOCKED it logs [travel-gate2] lines: the navmesh under the stall and under the destination, and the doors / activators / obstacles next to the straight line between them, with their animation state and collision. 1 = on (only for a gate field test), 0 = off.

### [PositionCast]

Facet reference: [position cast](FACETS.md#position-cast-kcastflag_atposition).

ABI v11. "Apply this spell right there." A mod names an actor, a Target Location spell and a point on the ground. Harbinger drops an invisible marker at the point, casts the spell from it in the actor's name, and deletes the marker a frame later. Summons are refused (the game only lets the caster itself summon), and so are runes and other spells that launch a projectile (the game only places those from the player's crosshair).

- **`bPositionCast`** (default `0`). OFF by default: the follower does not animate when a spell is cast this way, and every action needs a proper animation, so this is a building block, not a finished feature. 0 = every position-cast request is REFUSED (logged with the reason). 1 = on. Walking a follower to a point (Travel) works either way.

### [TargetPin]

Facet reference: [ch.20 Target pin](FACETS.md#ch20-target-pin-kintent_targetpin).

ABI v13. "Fight THIS one." A mod names an actor and a target. While the actor is already fighting, Harbinger answers the game's own "who do I fight" question with that target, as long as the target is one the actor's side is already fighting. It never starts a fight. When the game ends the fight, or the target dies or unloads, the game picks its own targets again.

- **`bTargetPin`** (default `1`). Installs the seats. 0 = kIntent_TargetPin claims are REFUSED (the mod keeps doing its own targeting; logged once with the reason).

### [CombatEntry]

Facet reference: [ch.21 Combat entry](FACETS.md#ch21-combat-entry-kintent_combatentry).

ABI v14. "Start a fight with THIS one." A mod names an actor and a target. Harbinger asks the game, once, to put the actor into combat against that target, the same way the game starts any fight. The game may say no. Harbinger never keeps the fight going and never stops it: what happens next is the game's AI (pair it with [TargetPin] to make the actor fight that target and not another). Whatever the world does about the fight (crime, guards, allies joining) is the mod's doing.

- **`bCombatEntry`** (default `1`). 0 = kIntent_CombatEntry claims are REFUSED (the mod starts fights its own way; logged once with the reason).

### [EngagementHold]

Facet reference: [ch.20 Target pin](FACETS.md#ch20-target-pin-kintent_targetpin) and [ch.21 Combat entry](FACETS.md#ch21-combat-entry-kintent_combatentry). Rule: `Docs/INVARIANTS.md` #0 (i).

"Once a fight is called, it is a move-to: it cannot be lost" (marth, 2026-10-12). When a mod starts a fight ([CombatEntry]) or pins a target ([TargetPin]), the game no longer drops that target because its own eyes lost track of it. Harbinger keeps the game's knowledge of where the target is, so the follower chases it the game's own way. The hold ends when the target dies, is disabled or unloads, when the mod lets go, when a higher-priority claim takes over, or when the follower makes no progress toward the target for 20 seconds (logged as an anomaly, then released). A [PursuitLeash] never holds back the approach to a held target.

- **`bEngagementHold`** (default `1`). 0 = the old behaviour: a pin ends when the game loses the target, the leash bounds the approach to it too, and the keys below do nothing.
- **`bTargetKnowledge`** (default `1`). 1 = installs the two knowledge seats (after the game's own detection update, a held target the game marked lost is un-lost the game's own way and its last-seen place is its real one). 0 = no seat: the hold still never ends on "lost", but the game may search for the target instead of chasing it. Exactly 1.6.1170 and 1.5.97 (refused on 1.7.104, logged).
- **`bBleedoutDrive`** (default `1`). 1 = a held target that is bleeding out (or essential-down) is walked to: the follower's combat area is centred on it (radius 160) until it is up again. A mod's own [CombatApproach] claim on the follower wins. 0 = the game decides.
- **`bKnowledgeTrace`** (default `0`). DIAGNOSTIC, passive. 1 = one `[hold] trace` line per follower per second showing what the game did to a held target's knowledge and what Harbinger wrote.

### [Idle]

Facet reference: [ch.12 Idle](FACETS.md#ch12-idle-kintent_idle).

ABI v17. "Play THIS animation at THAT." A mod names an actor, an idle (an IDLE record) and optionally a reference to play it at. Harbinger asks the game, once, to play it. The old form-free idle (no idle named) is not affected by this switch.

- **`bIdleV2`** (default `1`). 0 = kIntent_Idle claims that name an idle are REFUSED (logged with the reason); claims without one still play the default-pose reset.
- **`bIdleConfirm`** (default `1`). 1 = an idle counts as played only when the actor's behaviour graph raises one of that idle's own entry events (read from the graph at the call); none within 3 s of world time ends the claim ("ANIMATION NOT CONFIRMED"). 0 = no graph read: every play is logged UNCONFIRMABLE and never ended for not playing (the kill switch for the graph read).

### [CombatReentryDeny]

Facet reference: [ch.22 Combat re-entry deny](FACETS.md#ch22-combat-re-entry-deny-kintent_combatreentrydeny).

ABI v15. "Stay out of the fight for a while." A mod names an actor and a number of seconds. While that time runs, the game cannot put the actor into combat: when it tries (it sees an enemy, it gets hit, an ally is fighting), the game's own combat start says no. Harbinger never stops a fight that is going on; the mod does that itself, once. The mod's own [CombatEntry] request still works.

- **`bCombatReentryDeny`** (default `1`). 0 = kIntent_CombatReentryDeny claims are REFUSED (the mod keeps the actor out of combat its own way; logged once with the reason).

### [PursuitLeash]

Facet reference: [ch.23 Pursuit leash](FACETS.md#ch23-pursuit-leash-kintent_pursuitleash).

ABI v16. "Do not chase a fight away from me." A mod names an actor, an anchor actor (often the player) and a radius. While the actor is farther than the radius from the anchor, the game's own combat moves toward its target or its search area (close in, chase, flank, reposition, search) are refused when that goal is farther from the anchor still. The actor keeps fighting what comes to it: attacks, spells, blocks and every other move are untouched.

- **`bPursuitLeash`** (default `1`). 0 = kIntent_PursuitLeash claims are REFUSED (the mod leashes the actor its own way; logged at startup with the reason).
- **`bLeashReturn`** (default `1`). 1 = a leashed actor whose search the leash refuses is walked back toward its anchor: while it searches, its combat area is centred on the anchor with the leash radius, so the game's own "return to combat area" brings it back and its search stays inside the leash. Not for a held engagement (its approach is exempt). 0 = it stands where the leash stopped it.

### [CombatApproach]

Facet reference: [ch.24 Combat approach](FACETS.md#ch24-combat-approach-kintent_combatapproach).

ABI v19. "Get within this distance of that actor, and keep fighting." A mod names an actor, a target X and a radius R. While the actor is in combat, its own combat area (the bound the game keeps every fighter inside) is centred on X with radius R: the game walks it back toward X when it is outside and keeps its tactical moves inside. Attacks, both hands' spells, blocks, the combat target and equipment decide as they would without it: every place the game reads the area for them sees its own area. A package that holds the actor in place still wins. Ends on arrival (unless the mod asks to hold), when X is lost, when combat ends, or when the mod releases it.

- **`bCombatApproach`** (default `1`). 0 = kIntent_CombatApproach claims are REFUSED (logged at startup with the reason).

### [Sightline]

Facet reference: [line of sight](FACETS.md#line-of-sight-getlineofsight), [own line of sight on a cast](FACETS.md#own-line-of-sight-kcastflag_ownlineofsight).

Harbinger's own line of sight (ABI v18). Rays from an actor's eyes to another actor's feet, chest and head, in every direction. Mods ask for it with a flag on a cast claim or a target pin, or ask directly. Another NPC's body blocks the rays, except on a healing or other friendly cast: there the player's and the teammates' bodies do not block.

- **`bOwnLineOfSight`** (default `1`). 0 = off: the line-of-sight flags are REFUSED at the request and mods keep their own test; the awareness query answers Unsupported too.

### [Awareness]

Facet reference: [awareness](FACETS.md#awareness-senseactor).

the "does this actor sense that one?" query (ABI v18).

- **`bHearNoise`** (default `1`). 0 = hearing ignores the game's noise records (spells cast, arrows fired or landing, hits). Hearing a fight nearby, sight, being close and being fought still work.

### [Harbinger]

Facet reference: [sources and priorities](FACETS.md#appendix-a-sources-and-priorities-per-facet), [package and scene source capture](FACETS.md#package-and-scene-source-capture).

who drives each AI facet. Mods that ask Harbinger for a facet, and other mods that change the same facet on their own, are SOURCES. Each one has a priority from 0 to 100 per facet (0 = off). A mod's claim wins over another mod's change only when its priority is higher. Defaults: other mods 20, mods that ask Harbinger 50, Papyrus and console calls 75. A change Harbinger cannot trace to a mod is always allowed and shown as an alert. The priorities live in their own file, Data/SKSE/Plugins/APMF_Sources.ini, keyed by mod name ([Target] SmartNPCTargetSelector.dll=20). The Harbinger page in the SKSE Menu Framework edits it live. Without that framework, edit the file by hand.

- **`bSceneProbe`** (default `1`). A passive probe, ON while it is field tested. It writes log lines only. Logs which game code asks for an actor's scene while a mod has given that actor a package (the scene hold-off is not built yet). 0 = off.

### [Probe]

Facet reference: [caster-type and restore census](FACETS.md#caster-type-and-restore-census).

a passive probe that is ON while it is being field tested. It only writes log lines and changes nothing in the game.

- **`bCasterTypeCensus`** (default `1`). For an actor a mod has told to cast a spell (a cast claim), logs which kind of combat caster the game's AI builds for that spell (Restore, Offensive, Armor, Summon and the rest), whether it then fires it from the hand, whether the hand charged it, and the case where it builds nothing at all. Also counts, for every actor, how often each kind fires. Lines are rate limited. 0 = off. Goes back to 0 before the next release.
- **`bRangedSelect`** (default `0`). For an actor whose equipment a mod governs (an equip claim), logs how the game's combat AI picks a bow or crossbow, what its equip steps try to equip, whether the target can be reached, and which combat moves run while the mod owns the hands. Lines are rate limited. 1 = on (only for a field test), 0 = off.
- **`bRestoreCensus`** (default `1`). For an actor a mod has told to cast a healing or restoring spell (a cast claim), logs whether the game's combat AI ever reaches its own restore caster for that spell, how long that took, whether the hand took the spell, and the case where it never happens. Lines are rate limited. 0 = off. Goes back to 0 before the next release.

### [Input]

DIAGNOSTICS -- all off. Only turn one on if you are chasing a specific bug and were asked to. The test surface takes live keyboard input.

- **`EnableTestSurface`** (default `0`). 

### [Probe.NativeBit]

- **`Enable`** (default `0`). 

### [Probe.NonAlias]

- **`EnableObserveLog`** (default `0`). 

### [Probe.mvcbt]

- **`Enable`** (default `0`). 

## APMF_Sources.ini

Path: `Data/SKSE/Plugins/APMF_Sources.ini`. Optional. Harbinger works with no file. The Harbinger page in the SKSE Menu Framework writes it when you move a slider, and you can also edit it by hand. It is read once at startup.

A **source** is anything that changes an AI facet: a mod that asks Harbinger for the facet (a client), another mod's hook DLL, a plugin that owns a package or scene, the engine, Papyrus and console calls, or "Unattributed" for a change Harbinger cannot trace. Each source has a **priority from 0 to 100 for each facet**. 0 turns the source off for that facet. A source is only denied when another source with a higher priority wants the same facet. Harbinger never denies a change it cannot trace to a named source. It allows it, logs `[sources] UNATTRIBUTED` and shows an alert on the menu.

**Defaults when the file or a key is missing**

| Source kind | Default priority |
|---|---|
| Another mod (unregistered) | 20 |
| A mod that asks Harbinger (client) | 50 |
| Papyrus and console calls (`DirectCalls`) | 75 |

**Format.** One section per facet, one key per source name, the value is the priority. Names are the mod's DLL or plugin name and are not case sensitive.

```ini
[Defaults]
iUnregisteredTarget=20
iClientTarget=50

[Target]
SmartNPCTargetSelector.dll=20
MFO.dll=60

[Equip]
SomeOutfitMod.dll=0
```

**Facet sections:** `Target`, `Detection`, `Package`, `Equip`, `CombatAction`, `Cast`, `Combat`, `Core`, `ActorValue`. `ActorValue` (speed and disposition values) and the sneak noise and detection range values on `Detection` are watched only: a change by another source is named and counted, never refused, at any priority. In this release `Target` is restored when a named source is outranked, and `Cast` refusals by a guard claim stand only while the client outranks the engine. Every other `Cast` refusal or substitution (a spell selection or cast claim refusing a spell at the cast check or at the AI's equip check, and the cast claim answering the AI's own cast decisions) stands only while the claiming mod outranks the source of the answer it replaces: the engine, or a mod that hooked the same engine function before Harbinger (a mod that also asks Harbinger for claims counts as the engine there). If SKSE itself (skse64) is ever seen hooked inside one of these cast seats, it ranks as a hook source (20 by default), not as Papyrus and console calls (`DirectCalls`, 75). `Equip` writers are traced by a stack scan and denied only when positively identified. `Package` and scene sources are observe only. On `Combat`, a combat re-entry deny refuses a `StartCombat` only while the claim outranks the source that called it (the engine or the calling mod's DLL; a vanilla Papyrus `Actor.StartCombat` or a console command runs inside the game and is named `Engine` in this release), and a combat approach bound is written only while the claim outranks the engine's own combat area. On `CombatAction`, a combat action deny or a pursuit leash stops a leaf the engine's combat tree chose only while the claim outranks `Engine`. With the defaults (client 50, engine 20) these behave exactly as before. Raise `Engine=` in `[Combat]`, `[CombatAction]` or `[Cast]` above the client to let the engine win. `Detection` and `Core` are counted and shown on the menu but nothing is denied on them yet.

**`[UnknownSources]`.** One key per facet, `allow` (default) or `override`: `Target=override`. The Harbinger page shows it as a checkbox, "Unknown sources can be overridden", under each facet header, and writes it when you tick it. With `allow`, a change Harbinger cannot trace is allowed, as described above. With `override`, such a change ranks at the priority of the "Unattributed" row (0 unless you move its slider), but never below the module that owns the unproven part of the chain (a raised Engine priority still wins), and it stands if that module is the claiming mod itself. Otherwise a mod that asks Harbinger for the facet outranks it and Harbinger enforces exactly as it does for a named source that is outranked. The alert and the `[sources] UNATTRIBUTED` log line stay either way, and an override is logged. `Target`, `Combat` and `Cast` act on this today (on `Combat`, a `StartCombat` whose caller cannot be named during a re-entry deny; on `Cast`, a cast or equip answer that came through a hook Harbinger cannot name, which is allowed by default where it used to be refused). On the other facets the setting is stored and shown but has no effect yet.

**`[Defaults]` keys:** `iUnregistered<Facet>` and `iClient<Facet>` (for example `iClientCast=50`) change the default for a whole facet. A name listed in a facet section beats the default.

The generated reference for `APMF.ini` above lists every other key. See also `Docs/HARBINGER-MENU.md`. The priority rules, which facet each intent belongs to and what is enforced on each are in [FACETS.md Appendix A](FACETS.md#appendix-a-sources-and-priorities-per-facet).
