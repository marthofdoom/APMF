# MAP.md — APMF architecture + change-impact map

Consult this first. Per-module responsibility, key symbols, and — the point — a
**"what breaks if you change this"** note per module (mirrors MFO's MAP.md). Nav
by module; grep to a symbol, read a narrow window. Authoritative companions:
`design.md` (the design, esp. §1a operating principles), `Docs/CHANNEL-MAP.md`
(per-channel source/deny/promote/verdict), `Docs/ARCHITECTURE.md`,
`Docs/INVARIANTS.md`.

**APMF is the GATEKEEPER** (marth 2026-09-02): once it owns a channel on an actor,
nothing else reaches that facet except through APMF. Each channel BLOCKS the
foreign input at its source so nothing competes; a re-assert loop is a FAILED
block, not an acceptable pattern (INVARIANTS #1).

## The two layers (design.md §2)

- **Layer 1 — the core spine** (`native/core/`): the central `Actor::Update`
  `0xAD` hook makes APMF the arbiter of every NPC's tick. The arbiter drives a
  registry of channel modules.
- **Layer 2 — the client API** (`native/core/ClientAPI.h`): STUB SEAM. Where
  client mods declare intents instead of claiming packages. Not built yet.

## Module map

### `native/plugin.cpp` — entry (thin)
`SKSEPluginLoad` → log setup + messaging listener. `kDataLoaded` installs the
hook, registers the input sink, logs the hotkey help. `kPreLoadGame` →
`Arbiter::ReleaseAll`.
- **What breaks if you change this:** if `kPreLoadGame` stops calling
  `ReleaseAll`, engaged channels leak engine state across a save load (a follower
  stuck sneaking / silent / speed-halved). Keep the release on pre-load.

### `native/core/Hook.{h,cpp}` — the central seat
`Character` + `PlayerCharacter` vtable patched once at index **`0x0AD`**
(`Actor::Update(float)`); the thunk calls the original FIRST, then
`Arbiter::OnActorUpdate(this)`. VR-refused. Installed once.
- **What breaks:** the index `0x0AD` is the whole version-robustness thesis
  (design.md §3) — do NOT swap it for a call-site offset. The original must run
  first (we act on top of the real AI tick, never instead of it). If you hook a
  non-virtual (`EvaluatePackage`) instead, you reintroduce version fragility.
  Every NPC routes through this thunk each frame — keep it cheap (identity
  compare + early-out; see Arbiter).

### `native/core/Channel.h` — the channel interface
`Channel` = `Name`/`ChannelNo`/`Hotkeys`/`OnHotkey`/`Tick`/`Release` + an atomic
`engaged` flag. `Hotkey{code,label}`.
- **What breaks:** `Tick` default is EMPTY on purpose — a real block (APMF is the
  gatekeeper: block the foreign input at its source) does no per-tick work
  (design.md §1a rule 3, INVARIANTS #1). If you make channels do per-tick work by
  default you invite the re-assert loop, which is a FAILED block. Only a
  known-incomplete block overrides `Tick` as a flagged stopgap (today: `Headtrack`).

### `native/core/Registry.{h,cpp}` — the channel list + self-registration
`Registry::Get()` (Meyers singleton), `Register`, `All`, `AnyEngaged`.
`APMF_REGISTER_CHANNEL(Type)` macro + `AutoRegister<T>` construct a
program-lifetime instance and register it at load.
- **What breaks:** channels self-register via a file-scope static initializer.
  This works ONLY because every `.cpp` links directly into the DLL target (CMake
  GLOB, no intermediate static archive) — see INVARIANTS #9. If you ever wrap
  `channels/` in a static library, unreferenced initializers get stripped and
  channels silently vanish. Registration order is load-order-undefined; never
  assume a channel index.

### `native/core/Arbiter.{h,cpp}` — the decision point
`Arbiter::Get()`. Owns the single gated target (`target`/`handle`/`pkgAtCapture`).
`EnsureTarget` (crosshair-pick when idle; hold current while engaged — no
mid-session hijack), `OnActorUpdate` (tick engaged channels on the gated target +
~1/s PACKAGE STABLE observability), `DispatchHotkey` (route a key to its channel),
`ReleaseAll`, `ClearTargetIfIdle`.
- **What breaks:** `OnActorUpdate` runs for EVERY actor every frame — the
  `AnyEngaged()` + `actor != target` early-outs must stay first or you tax the
  whole game. `pkgAtCapture` is the coherence check; if you stop capturing it the
  PACKAGE STABLE signal goes dark. `handle.get()` is the liveness source of truth
  (never deref `target` raw across frames without it). The target is
  main-thread-only state (input events + `0xAD` both run on the main thread).

### `native/core/Input.{h,cpp}` — test surface
`InputSink` (keyboard button-down) → `Arbiter::DispatchHotkey`. `LogHelp`
enumerates the registry's hotkeys.
- **What breaks:** test surface only — do NOT let gameplay logic depend on it
  (the real driver is the client API). Adding a channel needs NO edit here;
  hotkeys come from the channel's own `Hotkeys()`.

### `native/core/ClientAPI.{h,cpp}` — Layer-2 seam (STUB)
`client::Request(actor,intent,basis)` / `Complete(handle)` — declared, bodies log
"not implemented". `Intent` enum maps 1:1 to channel families.
- **What breaks:** intentionally unbuilt (design.md §7 open questions). When you
  build it, route through the SAME `Arbiter` + channel engage/release the hotkeys
  use — do not create a parallel control path. Keep `Intent` an OPEN enum.

### `native/channels/*.cpp` — one module per facet
Each: a `Channel` subclass + `APMF_REGISTER_CHANNEL`. All self-contained and
small. Implemented (READY / DOCUMENTED gates only):

| File | Ch | Facet | Gate | Re-assert? |
|------|----|-------|------|-----------|
| `MovementDeny.cpp` | 1 | movement DENY | `SetDontMove(true)` (Address-Library bound) | no |
| `Attribute.cpp` | 11 | aggression/confidence | `ActorValueOwner::SetActorValue` | no |
| `Headtrack.cpp` | 5 | look-at | `AIProcess::SetHeadtrackTarget` (own point slot) | **yes — OVERRIDE-with-hold, not a clean gate; can lose to a package-locked follower** |
| `CastingSelect.cpp` | 8 | cast selection | own `selectedSpells[kRightHand]` + `caster->currentSpell` | no |
| `WeaponDraw.cpp` | 4 | draw/sheathe | `DrawWeaponMagicHands(bool)` | no (sticky one-shot) |
| `Dialogue.cpp` | 10 | dialogue | `PauseCurrentDialogue()` | momentary one-shot |
| `Speed.cpp` | 1a | gait/speed | `kSpeedMult` AV | no |
| `Detection.cpp` | 16 | stealth | `kMovementNoiseMult` AV | no |

- **What breaks (all channels):** each must (1) keep the package coherent — none
  substitutes the package (§5); (2) capture-and-restore any engine state it
  writes in `Release` (Attribute/Speed/Detection restore prior AVs; CastingSelect
  restores the prior spell); (3) guard every struct-member write (null-check the
  accessor). If a channel's `Release` is skipped or reordered, the actor keeps the
  mutated state after disengage. Movement DENY uses `SetDontMove` (bound) because
  the pinned CommonLib rev exposes no named AI-driven setter on
  `MovementControllerNPC` (only unnamed `Unk_0C/0D`) — see INVARIANTS #8.

### NOT built (probe-gated — do not add without a live probe)
Movement PROMOTE feed (ch.1, `IMovementDirectControl` unnamed), combat-target PIN
(ch.6), combat ACTIONS (ch.7), casting TRIGGER suppression (ch.8), sustained
package procedures (ch.9), facial-expression setter (ch.13). See
`Docs/CHANNEL-MAP.md` "Need live probing".

## How to add a channel (the whole recipe)
1. Copy `channels/Speed.cpp` to `channels/<Facet>.cpp`.
2. Rename the class, set `Name`/`ChannelNo`/`Hotkeys`, write engage in
   `OnHotkey`, restore in `Release` (and `Tick` only if a documented fallback).
3. End with `APMF_REGISTER_CHANNEL(<Class>);`.
4. Nothing else — CMake GLOBs it, the registry picks it up, the help log lists it.
