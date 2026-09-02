# APMF Architecture

The engineering companion to `design.md` (the why) and `Docs/CHANNEL-MAP.md` (the
per-channel catalog). This describes the CODE: the two layers, the arbiter/registry,
the channel-module pattern, and the recipe for adding a channel. `#N` = a numbered
rule in `Docs/INVARIANTS.md`.

## 1. Two layers

**Layer 1 — the core spine (`native/core/`).** A single central hook —
`Actor::Update` at vtable index `0xAD` — makes APMF the arbiter of every NPC's AI
tick. Patched once on the `Character` (and `PlayerCharacter`) vtable, so every NPC
routes through one thunk with no per-actor hooks (design.md §3). The thunk runs
the original tick first, then hands the actor to the `Arbiter`, which drives the
engaged `Channel`s.

**Layer 2 — the client API (`native/core/ClientAPI.h`).** The seam where client
mods declare intents (`request(actor, intent, basis)` … `complete(handle)`)
instead of claiming a package or an alias. STUB today (design.md §7 open
questions). When built it routes through the SAME arbiter + channel engage/release
the test-surface hotkeys drive, so there is one control path, not two.

```
              Actor::Update(0xAD)  ── every NPC, every frame
                       │
                 Hook::thunk  (original first, then:)
                       │
                 Arbiter::OnActorUpdate(actor)
                       │  (acts only on the single gated target)
        ┌──────────────┼───────────────────────────┐
   Channel::Tick   Channel::Tick   ...        [obs] PACKAGE STABLE
   (engaged ones only; most no-op — clean source-gates)

   Hotkeys ─► Arbiter::DispatchHotkey ─► Channel::OnHotkey  (engage/deny)
   ClientAPI::Request (STUB) ─► [future] ─► Channel engage on the Arbiter
```

## 2. The arbiter and the registry

**`Registry`** (`core/Registry.{h,cpp}`) is a flat list of every `Channel`
instance. Channels self-register at load — `APMF_REGISTER_CHANNEL(Type)` expands to
a file-scope `AutoRegister<Type>` whose constructor builds a program-lifetime
instance and calls `Registry::Register`. The registry is the only place the core
learns which facets exist; there is no central switch to edit (#9).

**`Arbiter`** (`core/Arbiter.{h,cpp}`) is the decision point. It owns exactly one
gated target at a time (test surface): a crosshair-picked actor, with its package
identity captured at engage. Responsibilities:
- `EnsureTarget()` — crosshair-pick when idle; while any channel is engaged, hold
  the current target (no mid-session hijack).
- `OnActorUpdate(actor)` — called for EVERY actor; early-outs unless the actor is
  the gated target and something is engaged (#4), then ticks the engaged channels
  and logs PACKAGE STABLE ~1/s.
- `DispatchHotkey(code)` — routes a key to the channel that declares it.
- `ReleaseAll(why)` — releases every engaged channel (disengage / target-unload /
  pre-load-game).

Only ONE source holds the coherent package slot at a time; "multiple mods at once"
is realized as arbitration over that one body, never package substitution (§5, #3).

## 3. The channel-module pattern

Every directable facet is one `Channel` subclass in its own small file under
`native/channels/`. The interface (`core/Channel.h`):

- `Name()` / `ChannelNo()` — identity + the CHANNEL-MAP number.
- `Hotkeys()` — the test-surface keys (empty if none).
- `OnHotkey(code, target)` — engage/deny. `target` may be null → refuse and log.
- `Tick(actor)` — per-tick drive. **Default empty** — a clean source-gate needs no
  per-tick work (#1). Only an override-with-hold channel overrides it.
- `Release(actor)` — restore any engine state the channel changed (#5).

Three kinds of channel:
1. **True source-gate (no re-assert)** — set the input the AI itself reads, or deny
   the losing source once. Robust even against a package-locked follower.
   Examples: AI-attribute AVs (ch.11), casting selection (ch.8, deck-confirmed
   KEPT every tick), movement DENY (ch.1 `SetDontMove`, Address-Library bound),
   gait (ch.1a), detection (ch.16). These do NOT override `Tick`.
2. **One-shot promote** — a sticky/momentary state set once. Weapon draw (ch.4),
   dialogue (ch.10).
3. **Override-with-hold (documented exception)** — no clean input to gate because
   the AI co-writes the very output slot; re-assert each tick and accept it can
   lose to an aggressive source. Headtrack (ch.5) is the only one, and it says so
   (#2). This is the design's flagged fallback (design.md §1a rule 3), never the
   default.

## 4. Version robustness

- Hook a VIRTUAL (vtable index `0xAD`), never a call-site offset (#6).
- Engine calls via CommonLib accessors / named methods, or Address-Library
  `RELOCATION_ID(SE,AE)` for functions the CommonLib rev does not bind
  (`StartCombat`, `KeepOffsetFromActor` — see the probe). Never a hardcoded
  call-site offset.
- Guard every struct-member write with a null-check on the accessor (#7); the
  pinned colorglass CommonLib rev does not bind `StartCombat`/`SetCurrentSpell`,
  so those facets write members directly, guarded (#8).
- VR is refused at install (#6) — the `0xAD` index is unverified for VR.

## 5. How to add a channel (recipe)

1. Copy `channels/Speed.cpp` → `channels/<Facet>.cpp`.
2. Rename the class; set `Name`/`ChannelNo`/`Hotkeys`.
3. `OnHotkey`: resolve/guard the target, do the source-gate, set `engaged`.
4. `Release`: restore whatever you changed.
5. `Tick`: leave it out UNLESS the facet is an override-with-hold — then say so in
   the module header and INVARIANTS.
6. End the file with `APMF_REGISTER_CHANNEL(<Class>);`.

Nothing else is touched: CMake GLOBs the new file, the registry picks it up at
load, and the startup help log lists its hotkeys. This one-file-per-facet property
is a hard requirement — keep it.
