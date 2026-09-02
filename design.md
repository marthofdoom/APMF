# AI Package Management Framework (APMF) — Design

Status: DESIGN / mechanics-verification (2026-09-01). Not yet scaffolded as code.
Folder name is provisional (`ai-package-management-framework`) — rename freely (e.g. `apmf`, `marth-apmf`).

## 1. Premise

APMF is a standalone SKSE framework (its own mod/repo) that sits between an actor and the
package system and acts as a **traffic cop for AI control**. It becomes the single point that
decides what actually drives a follower/NPC's body at any instant, so multiple sources (MFO,
other mods, and unaware vanilla/framework packages) can coexist on one actor without fighting
over the alias/package priority ladder.

It exists because Skyrim's package arbitration is winner-take-all by quest priority: a
custom-follower framework claiming an actor above priority 60 locks everyone else out (the
MFO "Cicero can't be walked to the lockpick" case). APMF replaces that fight with brokered,
time-shared control.

## 1a. THE BINDING CONTRACT (marth 2026-09-02) — READ FIRST, THESE WIN

APMF is a **MODERATOR**, not a behavior factory. This is the top-level contract; where ANY later
section (the prototypes and the channel spec especially) reads as APMF *driving* or *promoting* a
facet, THIS SECTION WINS and that section is describing a stand-in to be corrected. (This contract
was added after a CTD proved the drift: the channel spec's "PROMOTE" framing licensed a channel that
called `Actor::StartCombat` to command a target — APMF generating behavior — which crashed. The
lesson is written into the rules below.)

1. **APMF MODERATES; it NEVER generates behavior.** APMF does not build or trigger movement, facing,
   pathing, casting, combat, or animation. It calls NO behavior-generating engine function
   (`StartCombat`, `CastSpellImmediate`, a movement drive-feed, an anim trigger, …). Its channels may
   do exactly two things and nothing else: (a) **ARBITRATE** — record which source owns a facet
   (basis; the ControlMap), and (b) **DENY** — suppress the losing source so it stops reaching the
   actor. Deny is APMF's ONLY lever on the engine: neutralize / redirect-to-null / starve the input.
   **APMF cannot force an output and cannot boost one** — no override-every-frame, no re-assert loop
   (a re-assert loop is a failed deny, not an acceptable pattern).

2. **The CLIENT brings and EXECUTES the behavior.** Real behavior comes from proper, already-
   discovered mechanisms the CLIENT owns: real AI packages and proven engine commands (e.g. MFO
   commands a combat target via its own `currentCombatTarget` write, selects a spell via its own
   `selectedSpells`, grants its own AI consent, runs its own packages). The client executes; APMF
   only ensures DELIVERY by denying the native AI / competing frameworks so the client's behavior
   reaches the actor. "Walk to the merchant naturally" is the CLIENT's package; APMF only denies the
   framework's follow so that package wins.

3. **Control is GRANULAR, per-facet.** APMF engages or denies ONE facet without interrupting the
   others. Worked example — the AI-DECIDED CAST: the client sets `selectedSpells` + `currentCombatTarget`
   and grants `CasterConsent`, so the follower's own combat AI DECIDES to cast the chosen spell at the
   chosen target *as if it chose to* (full animation), while its MOVEMENT stays its own — the follower
   keeps kiting/repositioning. APMF claims only the cast + combat-target facets and leaves movement
   untouched. This is NOT a forced cast and NOT a body freeze. That granular non-interruption is
   APMF's entire value.

4. **Hook SITE is negotiable; FUNCTION is not.** Where APMF hooks (which vtable slot / call site) is
   chosen for function and for out-of-the-box compatibility with the load order, and may change. WHAT
   it must achieve — moderate a facet by deny/arbitrate without generating behavior — does not.

**The one genuinely hard job (the load-bearing open mechanism):** cleanly DENY / STARVE an
*outranking* framework's package so a client's own package drives the actor natively (the Cicero /
travel-nav case). This is the crux of "deny, don't force," and the mechanism is being demystified
separately — treat it as the open problem, not a solved one.

**Prototype status vs. this contract:** the 0xAD hook + coherent-package findings hold. The
executor stand-ins that violated the contract are being removed: ch.6 combat-target no longer calls
StartCombat/writes currentCombatTarget (arbitration-only now; the client commands the target), and
ch.8 casting no longer writes selectedSpells (arbitration-only; the client selects). Remaining
channels are being reframed to deny/arbitrate-only in the CHANNEL MAP.

## 2. Architecture — two layers

**Layer 1 — the hook (APMF core).** A single central engine hook makes APMF the arbiter of the
actor's locomotion. All "who moves this body, and where" decisions funnel through it.

**Layer 2 — the client API.** A client mod does not claim a package or an alias. It sends APMF
a request ("I need this actor to do action X") on whatever basis it chooses (its own
priority/policy), and later releases ("action complete"). APMF arbitrates all outstanding
client requests plus any intercepted unaware-source intent.

Two source types:
- **API-aware clients** (MFO, future mods): cooperative, use the request/complete API, set their
  own override basis. MFO becomes a client.
- **Unaware sources** (vanilla packages, Cicero/Lucien follower frameworks): intercepted at the
  hook, kept coherent by truthful-state passthrough (see §5). No per-framework knowledge needed.

## 3. The hook — `Actor::Update` @ vtable index 0xAD

- **Hook point:** `Actor::Update(float)`, VIRTUAL at index **`0xAD`** (CommonLib `RE/A/Actor.h:377`).
  `Character` and `PlayerCharacter` inherit the same slot.
- **Central by construction:** it is a vtable entry, so patching the `Character` vtable ONCE
  (via `write_vfunc`) routes EVERY NPC through the override. No per-actor hooks. This is exactly
  the "one central point, track all NPCs" requirement.
- **Version-robust:** the vtable address is Address-Library-covered — `VTABLE_Actor`/`VTABLE_Character`/
  `VTABLE_PlayerCharacter` exist in both the SE and AE blocks of `Offsets_VTABLE.h` with per-runtime
  `REL::ID`s; the index `0xAD` is a single source constant shared across runtimes (unlike a call-site
  offset). **True Directional Movement** hooks this exact slot (`write_vfunc(0xAD, Update)` on the
  Character vtable, no SE/AE branch), proving the chokepoint and the version thesis at scale.
- **Do NOT hook** `EvaluatePackage`/`GetCurrentPackage` (non-virtual → call-site → version-fragile),
  and there is no package-procedure `Update` virtual exposed to hook. `Actor::Update` is the only
  clean, central, version-robust seat.

Inside the override: read the actor's would-be intent from `currentProcess->currentPackage`
(`AIProcess.currentPackage` at offset 0x018; `AIProcess` has no vtable — use CommonLib accessors,
not raw offsets), arbitrate, then drive locomotion (§4) or call the original.

## 3a. Scope: GENERAL AI control, not just movement (marth 2026-09-01)

APMF is a WIDE-USE framework — other modders will direct non-movement AI. So a client must be able
to direct ANY aspect of AI control, not only locomotion: stance/sneak, weapon draw/sheathe, combat
target selection + combat behavior, headtracking/look-at, idle/animation playback, aggression/
confidence, package-procedure activities (sandbox/guard/use-furniture/patrol), dialogue availability,
teleport/position. The `Actor::Update` 0xAD hook is the right central seat for all of them (it is the
parent of the whole AI tick); each facet needs its own EXECUTOR (analogous to the movement virtuals
for locomotion) and its own coherence story (some are actor-state flags settable independent of the
package — sneak, headtrack — and are easy; some are procedure-level activities more entangled with the
package). A facet→executor→directability catalog is being built and will fill in §4b. The movement
prototype (below) proves the hardest facet first; the pattern generalizes from the same hook.

## 4. Movement is executed at the movement layer, not by swapping packages

APMF decides *who drives* in the `Update` hook, but *executes* the drive through the movement
virtuals / controller, never by substituting the current package:
- `Actor::Move(float, NiPoint3&)` VIRTUAL 0xC8, `Actor::ModifyMovementData(...)` VIRTUAL 0x11A
  (`RE/A/Actor.h:404,486`) — last-mile locomotion mutation.
- `MovementControllerNPC::SetControlsDriven`/`SetAIDriven` (VIRTUAL, `RE/M/MovementControllerNPC.h:31-34`)
  + the `IMovementDirectControl` feed — drive the body directly while a package still runs.
- `Actor::actorMover` (0x140), `movementController` (0x148), `currentProcess` (0x0F0)
  (`RE/A/Actor.h:677,692,693`).

This split is what makes §5 work.

## 4b. Facet catalog — any AI aspect is directable from 0xAD (verified)

Verdict: **"any aspect of AI control can be directed from the central `Actor::Update` (0xAD) hook" is
TRUE, with one bounded exception.** All header refs under CommonLib `RE/`. Three tiers:

**Tier A — trivially directable, package-INDEPENDENT** (a flag write / AV override / one-shot vfunc or
method layered OVER the running package, no coherence machinery):
- Stance/sneak: `actorState1.sneaking` flag (`A/ActorState.h:110`) driven via `NotifyAnimationGraph("SneakStart/Stop")` (IAnimationGraphManagerHolder vfunc 01).
- Weapon draw/sheathe: `Actor::DrawWeaponMagicHands(bool)` vfunc 0xA6 (`A/Actor.h:370`).
- Headtracking: `AIProcess::SetHeadtrackTarget` (`A/AIProcess.h:176`).
- One-shot idle/anim: `AIProcess::PlayIdle` (`A/AIProcess.h:171`), `NotifyAnimationGraph`.
- Aggression/confidence/assistance: dynamic ActorValue overrides (`A/ActorValues.h:8-13`) via the ActorValueOwner interface (does not touch base AIData/package).
- Dialogue/greeting: `SetDialogueWithPlayer` 0x041 / `StopCurrentDialogue` 0x04F / `InitiateDialogue` 0xD8 (`A/Actor.h:299,308,420`).
- Teleport/position: `TESObjectREFR::MoveTo` (`T/TESObjectREFR.h:441`), `Actor::SetPosition` vfunc 0xA9.

**Tier B — needs a dedicated executor, still package-INDEPENDENT:**
- Locomotion (facet 1) — the movement-virtual hijack of §4.
- Combat target/behavior — combat runs on `CombatController`/`CombatGroup`, NOT the sandbox/travel
  package, so `StartCombat` (`A/Actor.h:652`) / `StopCombat` vfunc 0xE5 / `SetCombatGroup` vfunc 0xD5
  steer it without owning the package. Prefer those over raw `CombatController::targetHandle` member
  writes (offset-fragile — heed MFO's "member < 0x68, AE +8" scar).

**Tier C — the ONE package-entangled facet: sustained package-procedure ACTIVITIES** (sandbox-here,
guard, patrol, use-furniture-over-time, eat, use-item-at). The engine exposes NO sustained executor for
these independent of a package; the only starter, `AIProcess::SetRunOncePackage` (`A/AIProcess.h:178`),
IS package substitution (fires the OnPackageEnd teardown §5 rejects).
- **Resolution (keeps the "don't substitute" principle intact for 9 of 10 facets):** APMF COMPOSES an
  activity from primitives — locomotion to the anchor/furniture + a one-shot `PlayIdle` or furniture
  `ActivateRef` (`T/TESObjectREFR.h:345`) + stance/headtrack flags — rather than applying a package.
  Real engine package application stays available only as an explicit, flagged, §5/§6-coherence-managed
  fallback for a client that truly needs full native procedure behavior.

Version note: prefer the vfunc surfaces (index-stable like 0xAD); the offset-sensitive touchpoints
(direct CombatController/CombatGroup/HighProcessData member access) use CommonLib accessors + static_asserts,
never hand-written offsets. Method-ID executors (MoveTo/StartCombat/SetHeadtrackTarget/PlayIdle/AV set)
are the standard native-call pattern — robust where Address Library covers the runtime.

## 5. The truthful-state feedback mechanic (the make-or-break, verified)

**Principle (do NOT emulate success — report the truth).** APMF never fakes that a preempted
source's package completed. It reports the actor's real state.

**Why it must be a movement-level hijack.** Skyrim runs exactly ONE current package per actor.
- Package-SUBSTITUTION (rejected): if APMF makes another source the current package, the
  preempted source A loses the slot, its procedure stops, and `OnPackageEnd`/`OnPackageChange`
  fires — A tears down or falsely advances. Truthful state cannot save it; the engine told it it
  stopped.
- Movement-HIJACK (adopted): APMF leaves A as the current running package and commandeers only
  locomotion (drive the body toward another goal). A stays current, ticks every frame, computes
  its path toward its own target, and observes **truthful non-arrival** because the body is
  physically elsewhere. A's travel/follow procedure sits in its normal "slow walk, not arrived"
  pending state. `OnPackageStart` fired once and does not re-fire; `OnPackageEnd` does not fire.
  When APMF yields, the body reaches A's target, A advances (travel → activate/dialogue), the
  scripted conversation fires normally.

Worked example: a framework's "walk to the player, then start a conversation" package. While APMF
drives the body elsewhere, the package truthfully sees "not at the player" and waits. APMF yields,
the follower walks over, the conversation triggers. No lie, no desync, no per-framework code.

**Scope of §5:** truthful non-arrival is the coherence story for the MOVEMENT-HIJACK channels
(drive the body over a still-running package). It is NOT the only way APMF can let a client's
behavior run — see §5a for the package-OFFER channel, where the engine runs the client's OWN
package natively and no hijack (and no lie) is involved at all.

## 5a. The package-OFFER channel — the ONE intentional package-tier promote (0x49)

The movement hijack (§4/§5) is how APMF lets a client's *locomotion goal* win without substituting
the package. There is a second, cleaner path for the case where a client has a REAL AI PACKAGE it
wants the actor to run natively (travel-to-merchant, sandbox-at-town): **redirect the package
OFFER at its source** so the engine itself adopts the client's package and runs it with full native
fidelity (real pathing, real procedures, real arrival) — APMF then does nothing per-frame.

- **Mechanism:** hook `Actor::CheckForCurrentAliasPackage` (VIRTUAL **0x49**) on `VTABLE_Character`
  ONLY (never `PlayerCharacter` — §0.38 scar). The engine calls it to ask "does an ALIAS give this
  actor a package right now?" The thunk: if APMF holds a package-offer claim for the actor
  (game-thread, lock-free ControlMap read), return the CLIENT's `TESPackage*`; else return
  `original(self)`. The engine adopts the returned package as current and runs it natively.
- **Why this is allowed where §3 forbids "substitution":** it is NOT a movement hijack and NOT a
  `SetRunOncePackage`-style forced swap. It redirects the alias-tier OFFER the engine was already
  going to evaluate, so the cost is exactly ONE `OnPackageChange` on engage and ONE on release —
  the SAME class of interruption as vanilla combat taking an NPC and handing it back, which every
  follower framework already tolerates every fight. It is the ONE intentional package-tier promote.
- **Bounded + never-break (the guardrails, all three always hold):** (1) claim only inside a
  BOUNDED, client-gambit-valid-AND-live window — never a standing hold; (2) RELINQUISH cleanly (the
  thunk returns `original(self)` again) so the framework's own package resumes; (3) the offer path
  touches NO alias / run-once / ExtraAliasInstanceArray state — it only chooses which package the
  offer returns.

## 5b. The layering — APMF is STRUCTURALLY BENEATH script-driven overrides (architecture, not a caveat)

Skyrim resolves an actor's package by TIER: a PapyrusUtil `AddPackageOverride` (script tier) wins
over the engine's own native/alias selection. APMF operates at the NATIVE / alias tier (the 0xAD
movement hijack and the 0x49 alias-offer). It therefore sits BENEATH the script tier and **cannot
reach a PapyrusUtil override** — the script layer wins, APMF neither sees nor touches it. So "never
break a custom follower" is AUTOMATIC for any script-driven follower: it is a structural property
of where APMF lives, not a special case APMF has to handle. **APMF uses ZERO Papyrus itself.**

**Supporting evidence — Tuxborn follower audit (2026-09-02):** across all 1626 enabled mods, Simple
Follower Framework and every custom follower are ALIAS-TIER with ZERO PapyrusUtil package overrides.
So the alias-tier (0x49) offer mechanism covers every follower in that load order, and the
beneath-script layering means even a hypothetical script-driven follower is safe by construction.
(This is what makes the 0x49 probe worth building before travel/nav.)

## 6. Time-sharing the body (the one real risk + its mitigation)

Truthful state does NOT protect against a source with an **arrival timeout** ("not there in N
seconds → force-reset / MoveTo / teleport / abandon"). The body genuinely never arrives while
APMF pursues another goal, so that watchdog fires. Such timeouts are common in scripted scenes
and travel packages.

**Mitigation — APMF time-shares the body; it never holds it hostage.** The body is a
time-shared resource: APMF yields it back to each coherent source's goal on a cadence shorter
than the shortest source timeout. APMF cannot generally read a third-party timeout, so the safe
default is a conservative yield interval, tuned per known framework where possible.

Corollary: only ONE source holds the coherent current-package slot at a time. "Accept multiple
packages at once" is realized as **fast time-sharing of the single body/slot**, not truly
parallel procedures. An API client's intent is pursued by APMF pathing to its declared target,
while the nominal package owner stays coherent via truthful non-arrival.

## 7. Client API (Layer 2) — shape, TBD after a movement prototype

Rough shape (marth): `request(actor, action, basis) -> handle` … do it … `complete(handle)`.
Open questions to settle once the hook/movement prototype pins down what the hook can express:
- Does a client hand APMF a raw **package**, a higher-level **intent** (walk-to-X / sneak / hold
  position / follow-at-offset), or **raw movement**? (Leaning: high-level intent — APMF owns the
  locomotion, clients declare goals.)
- How is a client's override **basis** expressed — a number, or a policy callback APMF invokes to
  resolve conflicts?
- How do a client request and an unaware intercepted source rank against each other, and how is
  the yield cadence chosen when multiple sources have timeouts?

## 8. Version robustness

- Prefer VIRTUAL (vtable) hooks over call-site patches — the whole reason `0xAD` is safe and
  MFO's ImGui call-site trampolines are not.
- Minimal surface: one arbiter hook (`Update` 0xAD) + the movement executor (`Move` 0xC8 /
  `ModifyMovementData` 0x11A or the MovementControllerNPC feed). Nothing else.
- Derivation heuristics if a runtime's Address Library lags: resolve the vtable by walking
  `*(void**)character` from any live `Character*` and indexing `0xAD` (no offset DB needed for
  the index); cache once at load. Startup self-check that `[vtable+0xAD]` behaves like `Update`
  (per-frame cadence) before arming redirection.
- Struct offsets (`AIProcess.currentPackage` 0x018, etc.) are more version-sensitive than the
  vtable index — use CommonLib accessors and rely on its build-time `static_assert`s.

## 9. Open unknowns before committing to code (from the RE spike)

1. **Direct-control feed details** — the concrete way to drive the body while a package runs:
   `MovementControllerNPC::SetControlsDriven` + which `IMovementDirectControl` methods feed
   velocity/heading (un-named `Unk_0N` in CommonLib — needs RE or a reference mod), OR overriding
   `Actor::Move`/`ModifyMovementData`. This is the biggest engineering unknown; prototype first.
2. **Does the running procedure fight the hijack?** When A's kTravel/kFollow procedure computes
   its own path while the body goes elsewhere — path-recompute spam, stutter, or an internal
   "can't reach" state firing faster than an explicit timeout. Bench test.
3. **Impatient-timeout catalog** — which common frameworks/scenes have arrival watchdogs and
   typical N values, to set the yield cadence.
4. **1.6.629+ parent-offset access** — use accessors for non-`TESObjectREFR` parents; no raw casts.
5. **VR / 1.7.x index confirmation** — smoke-test `0xAD` per runtime (no single source tabulates
   it across 1.5.97 / 1.6.1170 / 1.7.x). MFO refuses VR today; scope TBD.
6. **Confirm `0xAD` + offsets against the pinned CommonLib commit** used to build.

## 10. Relationship to MFO and the ecosystem

- MFO becomes a client: its loot-travel, town-nav, and assassin positioning/sneak send intents to
  APMF instead of fighting the priority ladder. This supersedes the narrow "granular conditional
  package priority" idea (which was blocked by §0.36: runtime priority writes don't re-arbitrate).
- Ecosystem gap confirmed: no existing mod hooks a package-selection virtual (there isn't one).
  Follower frameworks (NFF) and PapyrusUtil reimplement a coarse Papyrus package-override stack;
  TDM and position-fix mods hook `Actor::Update` + mutate AIProcess. APMF is the finer, brokered
  layer that gap is missing.

## Next steps

1. Prototype the movement hijack on 1.6.1170: `write_vfunc(0xAD)` on Character, keep a test
   follower's package current, drive its body to a marker via the MovementControllerNPC feed (or
   `Move` override), confirm the package stays coherent and resumes on yield. Resolve unknown #1/#2.
2. Then design the client API (§7) against what the prototype proves the hook can express.
3. Then scaffold the SKSE plugin properly (CMake/vcpkg/CommonLibSSE-NG, mirroring MFO's toolchain).
