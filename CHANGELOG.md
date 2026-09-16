## Unreleased -- A mod can say which hand, and potions are never refused

- **A mod can now say which hand an item goes in.** The first version equipped every item without a hand, so the game always put a one hand weapon in the right hand and pushed out whatever was there. A follower could not dual wield through Harbinger and a declared dagger evicted the declared sword. The new call carries a hand per item: right, left, or let the game pick. An item declared for the left hand that the follower holds in the right is moved. The same weapon can be declared once per hand.
- **Potions, food, scrolls, ingredients and books are never refused.** The equip check only governs armor, weapons, ammo and torches now. A follower drinking a potion, or a mod telling one to, passes through untouched. Before this the check would have refused every potion on a follower with a declared set once enforcement was switched on.
- **The player can still dress a follower by hand.** An equip from the trade or gift menu passes by default, though the mod's next declaration puts back whatever it displaced unless the mod folds the player's choice into that declaration. A mod that wants the strict form can ask for those to be refused too.
- **A claim is refused when the equip hook is not installed.** With the feature switched off in the ini, on VR, on an unsupported game version, or after a hook conflict, a mod asking for equip authority now gets a refusal instead of a claim that does nothing. A mod that stops its own equips on a successful claim would otherwise have equipped nothing all session. The log says why once.
- **A mod can ask whether the equip check is enforcing or only watching.** One new call answers it, so a mod can tell observe mode from the real thing.
- **The log now names the Harbinger release a newer mod needs.** When a mod asks for an API newer than the installed Harbinger, the refusal line states the installed version and the first release that would satisfy the mod, instead of only the two API numbers.
- New API revision (v8) with two calls, `SetEquipSetEx` and `IsEquipAuthorityEnforced`, and one flag. The v7 call still works and now runs through the same path with the hand left to the game. Older clients are unaffected.
- Still ships in observe mode. `bEquipObserveOnly=1` is unchanged.
- No change to any existing facet.

## Unreleased -- A mod can decide what a follower wears

- **A mod can now declare a follower's worn set and have it hold.** The mod sends the list of items. Harbinger equips them and refuses every other equip the game tries on that follower: outfit refresh, the AI's own weapon and armor picks, the re-equip after an item is removed. It holds until the mod declares a new set or releases. The player is never touched. Followers without a declaration are never touched.
- **This is the first call-site hook in Harbinger.** Every engine equip goes through one internal function that has no virtual entry to hook, so the two places the game calls it are patched instead. Both are checked byte for byte before either is written. If any other mod has touched them the whole feature refuses to install and says so in the log. A conflict is a missing feature, never a crash.
- **Ships in observe mode.** `bEquipObserveOnly=1` in `APMF.ini` logs what would have been refused and refuses nothing. It stays that way until a real session proves the log is right. Flip it to 0 to enforce.
- **Scripts still get through.** A Papyrus or console equip passes unless the mod asks for those to be refused too. Unequips are never refused in this version.
- New API revision (v7) with one call, `SetEquipSet`. Older clients are unaffected.
- The startup `[runtime]` line now also reports `equip-sink open` or `gated`. That field is the version predicate only. The `[apmf][equip-sink] INSTALLED` line is the truth about whether the seat is live.
- No change to any existing facet.

## Unreleased -- APMF's cast seats run on Skyrim 1.5.97

- **The cast classify seat and the weapon score seat now run on Skyrim 1.5.97.** Both used to refuse anything but 1.6.1170. Every value they read was checked against the 1.5.97 binary and its address library before being placed. Nothing was guessed.
- **The weapon score seat resolves its four vtables through the address library on both runtimes.** It used to carry hard 1.6.1170 addresses. The install check that compares the live function pointer against the expected one is now per runtime, because 1.5.97 compiles two of those functions as short thunks.
- **The version gates are exact.** Each seat opens on 1.6.1170 and 1.5.97 and refuses any other build by name. A 1.6 build other than 1170 is refused where it used to be admitted unverified.
- **One line at startup states the placement.** `[runtime] <version>: cast-classify open, group-C open` tells a log reader what this build placed before any seat installs.
- Skyrim 1.7.104 is still unsupported. There is no address library for it, so nothing was placed. CommonLib terminates at SKSE::Init with the address-library dialog on 1.7.104; APMF's gates never run.
- Not field-tested on either runtime. CI verified only.

## v0.9.4 -- Ships its settings file

- **Harbinger now ships `Data/SKSE/Plugins/APMF.ini`.** The plugin has always read that file, but no release ever contained it, so every switch fell back to a built in default and there was no way to see what the switches were. The file is now included, with every key documented and set to the configuration that was actually tested.
- **The spell score steer is on by default.** It nudges the game's own scoring so a follower picks the spell a mod asked for instead of its own preference. It was held off until field data proved the underlying hook runs. That data arrived on 2026-09-09.
- No code changed. Deleting the file, or any line in it, is still safe.

## v0.9.3 -- Package offers actually engage, and a cast claim can be renewed

- **A client's package offer now reaches the game.** The nudge that tells the engine to re-ask for a package was being posted before the claim was published, so the engine asked while the claim was still invisible and kept the package it already had. Nothing a client offered ever ran. It is now posted after the claim goes live. This is what was stopping follower loot travel from working.
- **A cast claim's deadline is a floor that can be renewed, not a hard expiry.** A client that keeps asking for the same spell on the same hand can now hold it through a long cast. Before this, a claim died on its own clock in the middle of a cast the engine was still performing.
- **A client can claim a hand purely to deny it.** This lets a client keep the game's AI off the other hand without pretending to cast anything there. It ships switched off and no client sets it yet.
- **The spell score steer now runs where spells are scored.** It was installed on the weapon path, where it could not affect a spell. The weapon steer is gone.
- Fixed a redirect answer that could be remembered past the point it stopped being true, so a repeat offer of the same package logged nothing and looked like a failure.
- Fixed a claim built from a package refusing its own client's heartbeat.
- Fixed the winner of a hand being picked before checking whether that claim was still alive, which could drop a live lower-priority deny for a frame.

## Diagnostic keys are opt-in

- **No key press can change what Harbinger does any more.** Earlier builds armed a keyboard test surface in every game: numpad keys took control of whatever NPC the crosshair was on, and two of them switched a live actor's ability to attack or cast on and off. That was a diagnostic surface that should never have shipped armed. It is now off unless you ask for it, and the release note for v0.9.0 ("nothing to configure") is finally true of the keyboard as well.
- Testers who want it back set `EnableTestSurface=1` under `[Input]` in `Data/SKSE/Plugins/APMF.ini`. The probe that flips an actor's attack and cast bits needs its own `[Probe.NativeBit] Enable=1` on top of that.
- The package-observation probe reads its switch from the ini (`[Probe.NonAlias] EnableObserveLog`) instead of needing a key press, so a diagnosis run no longer depends on the test surface at all.
- No change to any client-facing behaviour: the arbitration, the channels, the gates and the engine seats are untouched.

## v0.9.2 -- The equip deny no longer disarms the follower

- **Fixes the v0.9.1 issue where a follower could stop fighting.** While a cast claim stood, the equip deny took every spell and staff the actor owned, on both hands, for the whole life of the claim. A follower being healed repeatedly lost their own attack spells and appeared to freeze. The deny is now limited to the claim's own hand, so the other hand goes back to the NPC's own AI. The claimed spell still wins its slot, which is what makes the cast work.
- **Offense casts now work through the same path as heals.** The engine seats were installed only on the Restore caster, so a client claiming a hostile spell got nothing at all. They now cover the Offensive caster too, still gated per call on the claim naming that exact actor and spell.
- **A hostile spell is no longer re-classified.** The classification fix exists only because the game has no way to describe a heal aimed at someone else. Applying it to a hostile spell would have moved it to a row the game never uses and could have broken casting that already worked.
- Fixed a latent crash risk: one seat read a value that only exists on the Restore caster and would have read garbage off the Offensive one.
- **Not field-tested.** CI verified only. See the note on the release page.

## v0.9.1 -- The NPC's own AI performs a client's cast

- **A client mod can now ask Harbinger to make an NPC cast a chosen spell at a chosen target, and the NPC's OWN combat AI performs it.** Harbinger makes no equip call, no animation call and no cast call of any kind. It answers the questions the engine's own cast logic asks, so the charge, the aim, the animation style and the channel are all the game's own.
- **This makes a heal-other cast possible for the first time.** The vanilla combat AI cannot classify a healing spell aimed at someone else, so it can never build one as a candidate and never considers casting it. That is why followers in every mod have only ever healed themselves through the AI. Harbinger supplies the one classification decision the engine is missing, and the rest of the engine takes it from there.
- The forced cast drive is gone. Harbinger no longer equips, drives or fires anything, and it no longer calls CastSpellImmediate. A cast that the engine refuses now visibly does not happen instead of being faked.
- A client can state its own stop threshold for a channelled cast, so a heal can run to full instead of stopping where the engine would.
- Requires a client that uses the kIntent_Cast request. MFO v2.0.1 or newer.
- This is a beta build.

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
