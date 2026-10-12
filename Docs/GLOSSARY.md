# Harbinger glossary

Every term the mod-author pages use, defined once, with the page and section where it is introduced. The pages are [GUIDE.md](GUIDE.md) (the foundations), [FACETS.md](FACETS.md) (the reference) and [RECIPES.md](RECIPES.md) (worked scenarios).

| Term | Meaning | Introduced |
|---|---|---|
| ABI | The version number of the C-ABI contract in `native/APMF_API.h`. Append-only. Today 21 | [Binding and Version Gating](GUIDE.md#binding-and-version-gating) |
| actor-value ledger | Harbinger's co-saved record of the actor values a claim changed and their previous values, restored on release only if unchanged, and after a load whether or not a claim survives | [ch.1a Gait](FACETS.md#ch1a-gait-kintent_gait) |
| allow list | A bounded extra set of spells (up to 32) a select-spell claim admits beside its primary spell | [ch.8 Select spell](FACETS.md#ch8-select-spell-kintent_selectspell) |
| anchor | The actor a pursuit leash measures from, or the optional second listener of an awareness query | [ch.23 Pursuit leash](FACETS.md#ch23-pursuit-leash-kintent_pursuitleash), [Awareness](FACETS.md#awareness-senseactor) |
| arbitrate | A channel mode: Harbinger records who owns the facet and calls nothing. The client drives it | [A moderator, not a driver](GUIDE.md#a-moderator-not-a-driver) |
| basis | The `float` a client passes with a claim. Ranks claims against claims on one actor and one intent. Highest owns, tie to the earliest | [Claims against claims: the basis](GUIDE.md#claims-against-claims-the-basis) |
| blast area | A spell's largest effect area, stored in feet, converted at 64 over 3 units per foot for the cast guard | [ch.25 Cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) |
| BLOCKED | A travel leg end: the engine held the actor in its Movement Blocked package for 3 seconds of running game time | [ch.19 Travel](FACETS.md#ch19-travel-kintent_travel) |
| bound (combat approach) | The actor's standard combat area, centred on X with radius R while a combat approach claim is in force | [ch.24 Combat approach](FACETS.md#ch24-combat-approach-kintent_combatapproach) |
| bounded claim | A claim with a window that releases itself: the cast claim, the cast guard and the re-entry deny | [Repoint and the heartbeat](GUIDE.md#repoint-and-the-heartbeat) |
| built | A field tier: compiles, mechanism sound and CI verified, no field run of its own | [The field tiers](GUIDE.md#the-field-tiers) |
| call-site seat | The equip authority's hook: two patched call sites of the engine's equip worker, byte-verified, refused on any collision | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| census (seat) | Harbinger's re-check of every hooked seat at each load stage and every 60 seconds, naming any mod that hooked on top | [Observe-only facets](FACETS.md#observe-only-facets-no-claim-api) |
| channel | Harbinger's implementation of a facet, numbered (`ch.N`), the log tag | [Intent, channel and sources facet](GUIDE.md#intent-channel-and-sources-facet) |
| channel bound | A cast guard rule: the AI's own concentration spell on a hand is refused unless a live cast claim on that hand names it | [ch.25 Cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) |
| claim | A request that says "I need this facet on this actor": actor, intent, basis, optional parameter. Named by a handle | [Anatomy of a claim](GUIDE.md#anatomy-of-a-claim) |
| client | A mod that asks Harbinger for control through the API. A source kind, default priority 50 | [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority) |
| combat group | The engine's set of combat targets for an actor. A target becomes pinnable once it is in the group | [ch.20 Target pin](FACETS.md#ch20-target-pin-kintent_targetpin) |
| compose | What the cast channel adds to arbitrate and deny: answering the engine's own cast-decision seats so the NPC's own AI casts the claimed spell | [A moderator, not a driver](GUIDE.md#a-moderator-not-a-driver) |
| degrade path | The client's own mechanism for a facet, run when Harbinger refuses a claim | [Where it runs and what you bring](GUIDE.md#where-it-runs-and-what-you-bring) |
| declaration | An equip authority's set and scope, sent when the loadout changes, never per tick | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| deny | A channel mode: Harbinger suppresses a competing input at its source. Nothing is re-asserted | [A moderator, not a driver](GUIDE.md#a-moderator-not-a-driver) |
| deny-only claim | A cast claim with `kCastFlag_DenyHandOnly`: claims a hand to deny it and drives nothing. See floor | [A hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly) |
| drain | The once-per-frame pass on the game thread that applies queued operations in order, runs the expiry pass and the unload sweep, and publishes one snapshot | [The drain](GUIDE.md#the-drain) |
| driving claim | A cast claim that names a spell to cast, as opposed to a deny-only floor | [A hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly) |
| eviction | A live single-hand cast claim removed by a higher-basis dual-cast claim, or the reverse | [How a claim ends](GUIDE.md#how-a-claim-ends) |
| expiry | The end of a bounded claim's window. Final: a late `Repoint` does not reopen it | [The drain](GUIDE.md#the-drain) |
| facet | One input the engine's AI reads when it decides one thing about an actor | [What a facet is](GUIDE.md#what-a-facet-is) |
| field-proven | A field tier: a live deck run exercised the facet, or a shipping client uses it | [The field tiers](GUIDE.md#the-field-tiers) |
| floor | A deny-only cast claim on a hand. Admits nothing, loses a tie to a driving claim | [A hand floor](FACETS.md#a-hand-floor-kcastflag_denyhandonly) |
| gait | The travel facet's per-leg speed (Walk, Jog, Run, FastWalk), or the ch.1a facet that halves `SpeedMult` | [ch.19 Travel](FACETS.md#ch19-travel-kintent_travel), [ch.1a Gait](FACETS.md#ch1a-gait-kintent_gait) |
| gate-only | A claim whose channel writes nothing. The deny lives in an engine gate Harbinger already owns | [ch.15 Equipment](FACETS.md#ch15-equipment-kintent_equipment) |
| handle | The `std::uint32_t` that names a claim. 0 (`kInvalidHandle`) means refused | [Anatomy of a claim](GUIDE.md#anatomy-of-a-claim) |
| heartbeat | A `Repoint` sent before a bounded claim's window lapses, to renew it | [Repoint and the heartbeat](GUIDE.md#repoint-and-the-heartbeat) |
| inert | A live claim the drain found unusable (a wrong form type). It does nothing until released | [The two refusal timings](GUIDE.md#the-two-refusal-timings) |
| intent | The facet's name in the API (`kIntent_Cast`). One intent is one facet | [Intent, channel and sources facet](GUIDE.md#intent-channel-and-sources-facet) |
| leg | One walk of the travel facet, from request or re-point to its end. A leg ends and the claim stands | [ch.19 Travel](FACETS.md#ch19-travel-kintent_travel) |
| line of fire | The cast guard's test: a friend within 80 units of the caster-to-aim line and not occluded from the caster, or within the blast area plus 64 units of the aim | [ch.25 Cast guard](FACETS.md#ch25-cast-guard-kintent_castguard) |
| live | A claim from the drain that applies it until the drain that ends it. `IsClaimLive` reads true, owner or not | [Pending and live](GUIDE.md#pending-and-live) |
| main thread | The thread that runs the player's `Actor::Update`, where Harbinger drains and where the synchronous queries must run. Not SKSE's `AddTask` | [The main thread and the queries](GUIDE.md#the-main-thread-and-the-queries) |
| moderator | What Harbinger is: it arbitrates and denies, and generates no behaviour | [A moderator, not a driver](GUIDE.md#a-moderator-not-a-driver) |
| nudge | The `EvaluatePackage` call a package offer or travel leg makes at engage, owner change and release, posted one hop after the claim publishes | [ch.9 Offer package](FACETS.md#ch9-offer-package-kintent_offerpackage) |
| observe-only | A seat or mode that names and counts a write and never denies it | [Observe-only facets](FACETS.md#observe-only-facets-no-claim-api) |
| one-shot | An engine call made once, with no claim behind it. The position cast | [Position cast](FACETS.md#position-cast-kcastflag_atposition) |
| outrank | A claim's client priority strictly above a source's on a sources facet. The condition for a deny to stand | [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority) |
| owner | The claim with the highest basis on one actor and one intent. It drives the facet | [Claims against claims: the basis](GUIDE.md#claims-against-claims-the-basis) |
| own ray | Harbinger's physics line-of-sight test from a viewer's eye to three points on a target, no view cone, on the character-controller layer | [Line of sight](FACETS.md#line-of-sight-getlineofsight) |
| pending | A claim between the call and the drain that applies it. `IsClaimLive` reads false | [Pending and live](GUIDE.md#pending-and-live) |
| pin pause | The target pin standing down for one update while the engine has no target or the pinned target is not in the group, or (with the flag) is not visible | [ch.20 Target pin](FACETS.md#ch20-target-pin-kintent_targetpin) |
| priority | A number from 0 to 100 a source has on each sources facet, from `APMF_Sources.ini`. Ranks a source against a source | [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority) |
| probe | A log-only switch that changes no behaviour (`bCasterTypeCensus`, `bRestoreCensus`, `bGateProbe2`, `bSceneProbe`) | [Observe-only facets](FACETS.md#observe-only-facets-no-claim-api) |
| probe path | The ch.15 claim with `form == 0`: it unequips the right-hand weapon and re-equips it on release. Not save-safe | [ch.15 Equipment](FACETS.md#ch15-equipment-kintent_equipment) |
| promote | A channel mode: one bounded engine call at engage or release, no tick, no re-assert | [A moderator, not a driver](GUIDE.md#a-moderator-not-a-driver) |
| proxy | The delivery-flip copy of a Self-delivery spell Harbinger mints so a cast claim can aim it at another actor. Read with `GetCastProxy` | [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast) |
| publish | The last step of a drain: one new snapshot every reader sees at once | [The drain](GUIDE.md#the-drain) |
| query | A read-only question with no claim, no handle and no write: the space queries, line of sight, awareness | [Read-only queries](FACETS.md#read-only-queries-no-claim) |
| release | `Release(handle)`: ends your part of a claim. A no-op on an ended handle | [How a claim ends](GUIDE.md#how-a-claim-ends) |
| Repoint | `Repoint(handle, param)`: changes a claim's parameter in place and renews a bounded window | [Repoint and the heartbeat](GUIDE.md#repoint-and-the-heartbeat) |
| reserved field | An `APMF_Param` field an intent accepts, stores and ignores today | [The catalogue](GUIDE.md#the-catalogue) |
| scope | The equip categories an equip authority owns or denies (`APMF_EquipScope`) | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| seat | An engine function Harbinger hooks or patches to answer a decision (a vfunc slot such as 0x0A, or a call site) | [What Harbinger Is](GUIDE.md#what-harbinger-is) |
| size rule | Every query struct starts with `size`, set by the caller to `sizeof`. Harbinger refuses a short input and never writes past the declared size | [Space queries](FACETS.md#space-queries-findemptyspace-and-findhostilesinspace) |
| snapshot | The published control map readers see. A queued request is not in it until drained | [The drain](GUIDE.md#the-drain) |
| source | Anything Harbinger can name as the origin of a write to a facet: a client DLL, a hook DLL, a plugin, `DirectCalls`, `Engine`, `Unattributed` | [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority) |
| sources facet | A group of channels that share one priority table: `Target`, `Package`, `Equip`, `CombatAction`, `Cast`, `Combat`, `Detection`, `ActorValue`, `Core` | [Intent, channel and sources facet](GUIDE.md#intent-channel-and-sources-facet) |
| Spectator hold-off | The one enforcement on the Package facet: the engine's runtime Spectator package is not created while a package claim outranks the engine | [ch.9 Offer package](FACETS.md#ch9-offer-package-kintent_offerpackage) |
| standing claim | A claim with no window. It ends only by release, by Harbinger ending it, an unload, a load or a new game | [How a claim ends](GUIDE.md#how-a-claim-ends) |
| stop percent | Bits 8 to 15 of `CastFlags` (`MakeStopPct`): the percent of the target's permanent actor value at which a heal or restore channel stops | [ch.8b Cast](FACETS.md#ch8b-cast-kintent_cast) |
| stored claim | A live claim outranked by a better one. It takes over if the owner goes | [Pending and live](GUIDE.md#pending-and-live) |
| unattributed | A write Harbinger cannot trace to a named source. Allowed and alerted, never denied by default | [Claims against the world: the priority](GUIDE.md#claims-against-the-world-the-priority) |
| verdict (line of sight) | `kLos_Unknown`, `Visible`, `Occluded`, `Unavailable` or `Unsupported`, the own ray's answer for a pair | [Line of sight](FACETS.md#line-of-sight-getlineofsight) |
| verdict (equip) | The equip seat's decision for one engine equip: allow, deny or would-deny, path-classified | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| window | The bounded lifetime of a cast claim (`ttlMs`), a cast guard or a re-entry deny (`fval` seconds). Wall-clock | [Repoint and the heartbeat](GUIDE.md#repoint-and-the-heartbeat) |
| would-deny | An equip verdict in observe-only mode: logged as a deny, nothing refused | [ch.17 Equip authority](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
