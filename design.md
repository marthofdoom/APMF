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

## 1a. Core operating principles (marth 2026-09-02) — READ FIRST

APMF is a CHANNEL ROUTER, not a behavior factory. Three rules govern everything below; where any
later section (the prototypes especially) conflicts with these, these win.

1. **Arbitrate channels, do not manufacture behavior.** APMF decides, per facet, which SOURCE's
   input reaches the actor. It does NOT build movement, facing, pathing, or animation. Whoever owns
   the promoted channel (a client mod, vanilla AI, or an unaware framework) produces the behavior;
   APMF only routes. "Walk to the merchant naturally" is a CLIENT's channel — APMF promotes it and
   denies the framework's follow channel; the naturalness is the channel's problem, never APMF's.

2. **Promote and deny.** For each facet APMF PROMOTES one channel (grants it authority) and DENIES
   the competing channels (suppresses their input).

3. **Selectively ALLOW at the source — do NOT FORCE over the top.** The director gates the INPUT a
   channel feeds the engine; it does not let the AI produce an output and then override it every
   frame. Deny the losing channel at its source (or set the input the AI itself consumes) so the AI
   never produces the competing command and there is nothing to fight. Where a channel has no clean
   source-gate, override is a DOCUMENTED FALLBACK, flagged as such — never the default. **A re-assert
   loop is a code smell**, a sign we are forcing an output instead of gating an input.

**What the prototypes actually proved vs. these rules:** prototype 0/1 confirmed the foundation —
the central 0xAD hook reaches every NPC, the package stays coherent (`PACKAGE STABLE`, no CTD) while
control is exerted, and truthful non-arrival holds. But their EXECUTORS violate the rules and are
stand-ins only: `KeepOffsetFromActor` MANUFACTURES movement (rule 1) and the SneakStart re-assert
FORCES the output (rule 3). The real design replaces both with source-gated promote/deny. The next
work is the CHANNEL MAP: enumerate the channels (movement, facing, stance, headtrack, combat-target,
dialogue, …) and, per channel, its allow-gate and deny-gate — starting from a clean source-gated DENY
(suppress a source's movement) with no re-assert.

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
