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
  `0xAD` hook makes APMF the arbiter of every NPC's tick. It drives the
  `ControlMap` (a multi-NPC map keyed by FormID) and a registry of channel modules.
- **Layer 2 — the client API** (`APMF_API.h` + `core/ClientAPI.cpp`): REAL. An
  inter-plugin C-ABI (POD struct of function pointers, obtained via exported
  `APMF_GetInterface`). A separate client DLL calls `Request(actor,intent,basis)` /
  `Release(handle)`; both forward to the ControlMap enqueue path (#14).

## Module map

### `native/plugin.cpp` — entry (thin)
`SKSEPluginLoad` → log setup + messaging listener. `kDataLoaded` installs the
hook, registers the input sink, logs the hotkey help. `kPreLoadGame` →
`Arbiter::ReleaseAll`.
- **What breaks if you change this:** if `kPreLoadGame` stops calling
  `ReleaseAll`, engaged channels leak engine state across a save load (a follower
  stuck sneaking / silent / speed-halved). Keep the release on pre-load.

### `native/APMF_API.h` — the inter-plugin C-ABI contract (shared with clients)
The ONLY file a client shares with APMF. POD struct of function pointers
(`APMF_API_v1`: `Request`/`Release`), the `Intent` enum, `Handle`, and the exported
query-fn name. No C++ class / STL / vtable crosses the boundary.
- **What breaks:** APPEND-ONLY forever (#14). Never reorder/change an existing
  `Intent` value, struct field, or fn-pointer slot — a client built against v1 must
  keep working. A client (MFO) and APMF are separately built DLLs; this header +
  `APMF_GetInterface` are the ONLY seam. APMF carries zero client-specific code.

### `native/core/ClientAPI.{h,cpp}` — the C-ABI implementation
The exported `APMF_GetInterface(abiVersion)` hands over a static POD `APMF_API_v1`;
its `Request`/`Release` fn-pointers forward to `ControlMap::EnqueueRequest/Release`.
- **What breaks:** the exported fn must stay `extern "C"` + undecorated
  (`APMF_GetInterface`) or clients' `GetProcAddress` fails. The enqueue path is the
  ONE control path (the hotkeys use it too) — never add a parallel one.

### `native/core/ControlMap.{h,cpp}` — the multi-NPC engine (Phase 1 heart)
`unordered_map<FormID, NpcCtl>` (each NpcCtl = engaged channels + per-channel client
claims + captured package). `EnqueueRequest/Release` (any thread; brief queue lock,
atomic handle), `Drain` (game thread, once/frame: apply ops + sweep unloaded),
`OnActorUpdate` (per-NPC hot path: empty-check → one hash lookup → tick engaged),
`ReleaseAll`. Arbitration by basis (higher wins, tie → earliest); claims refcount.
- **What breaks:** SINGLE-WRITER (#12) — the map is mutated ONLY on the game thread
  (Drain + ReleaseAll); API calls only enqueue. Mutate it off-thread and you race
  across hundreds of NPCs. The hot path (#13) must stay `empty()` + ONE lookup for
  an uncontrolled NPC — no allocation, no all-NPC scan, or you tax the whole game.
  Handles must stay atomic-allocated so `Request` returns before Drain; ops FIFO.

### `native/core/AvLedger.{h,cpp}` — co-saved AV override ledger
`(FormID, ActorValue) -> captured prior value`, co-saved via SKSE serialization
(unique ID `'APMF'`, record `'AVOV'`). `av::Override`/`av::Restore` (the AV channels
call these, not raw SetActorValue), `Save`/`Load`/`ApplyPending`/`Revert`.
- **What breaks:** this is the ANTI-STRANDING backbone (#15). A persisted AV written
  raw (bypassing the ledger) is stranded on save-while-engaged + reload. `OnSave`
  writes it, `OnLoad`→pending, `kPostLoadGame`→`ApplyPending` restores + clears,
  `OnRevert` wipes. Any new channel writing a PERSISTED actor value must use the
  ledger. Game/main-thread only.

### `native/core/Hook.{h,cpp}` — the central seat
`Character` + `PlayerCharacter` vtable patched once at index **`0x0AD`**
(`Actor::Update(float)`); the thunk calls the original FIRST, then
`Arbiter::OnActorUpdate(this)`. The PlayerCharacter seat also calls
`Arbiter::OncePerFrame()` → `ControlMap::Drain()` (once/frame, game thread — the
single-writer drain seat). VR-refused. Installed once.
- **What breaks:** the index `0x0AD` is the whole version-robustness thesis
  (design.md §3) — do NOT swap it for a call-site offset. The original must run
  first (we act on top of the real AI tick, never instead of it). If you hook a
  non-virtual (`EvaluatePackage`) instead, you reintroduce version fragility.
  Every NPC routes through this thunk each frame — keep it cheap (identity
  compare + early-out; see Arbiter).

### `native/core/Channel.h` — the channel interface
`Channel` = `Name`/`ChannelNo`/`ServesIntent`/`Hotkeys`/`Engage`/`Tick`/`Release`.
Per-NPC lifecycle (no global `engaged` flag — the ControlMap refcounts claims and
calls Engage/Tick/Release keyed by actor; a channel keeps its own per-NPC state
map). `Hotkey{code,label}`.
- **What breaks:** `Tick` default is EMPTY on purpose — a real block (APMF is the
  gatekeeper: block the foreign input at its source) does no per-tick work
  (design.md §1a rule 3, INVARIANTS #1). If you make channels do per-tick work by
  default you invite the re-assert loop, which is a FAILED block. Only a
  known-incomplete block overrides `Tick` as a flagged stopgap (today: `Headtrack`).

### `native/core/Registry.{h,cpp}` — the channel list + self-registration
`Registry::Get()` (Meyers singleton), `Register`, `All`, `ChannelForIntent`,
`ChannelForHotkey`. `APMF_REGISTER_CHANNEL(Type)` macro + `AutoRegister<T>` construct
a program-lifetime instance and register it at load. The list is immutable after
load, so `ChannelForIntent` is safe from the client's worker thread (#12).
- **What breaks:** channels self-register via a file-scope static initializer.
  This works ONLY because every `.cpp` links directly into the DLL target (CMake
  GLOB, no intermediate static archive) — see INVARIANTS #9. If you ever wrap
  `channels/` in a static library, unreferenced initializers get stripped and
  channels silently vanish. Registration order is load-order-undefined; never
  assume a channel index.

### `native/core/Arbiter.{h,cpp}` — coordinator + test surface
`Arbiter::Get()`. Thin façade the hook/input/plugin call: `OnActorUpdate` /
`OncePerFrame` / `ReleaseAll` delegate to `ControlMap`. Owns the crosshair TEST
SURFACE: `DispatchHotkey` resolves the aimed NPC and toggles a test claim for that
key's channel (its own `m_testHandles` map), and a dedicated key (Numpad0) releases
all. Multi-NPC: aim + key ADDS an NPC; aim another + key adds it too.
- **What breaks:** the test surface drives the SAME `ControlMap` enqueue path the
  C-ABI uses — one control path, never a parallel one. `m_testHandles` is
  input-thread (main) only. Real driving is the client API, not this.

### `native/core/Input.{h,cpp}` — test surface
`InputSink` (keyboard button-down) → `Arbiter::DispatchHotkey`. `LogHelp`
enumerates the registry's hotkeys.
- **What breaks:** test surface only — do NOT let gameplay logic depend on it
  (the real driver is the client API). Adding a channel needs NO edit here;
  hotkeys come from the channel's own `Hotkeys()`.

### `native/channels/*.cpp` — one module per facet (FULL documented catalog)
Each: a `Channel` subclass + `APMF_REGISTER_CHANNEL`, per-NPC `Engage`/`Release`.
The first release ships the full documented catalog (13 channels) as a baseline
benchmark. Each `ServesIntent()` maps to an `APMF_API::Intent`. Test keys in
parentheses.

| File | Ch | Facet | Gate / mechanism | Kind |
|------|----|-------|------|-----------|
| `MovementDeny.cpp` | 1 | movement FULL block (Num1) | `KeepOffsetFromActor(self)` + `SetDontMove(true)` (both Address-Library bound) | source-block |
| `Speed.cpp` | 1a | gait/speed (Num7) | `kSpeedMult` AV (arbitrary factor, default x0.5) | source-block |
| `Stance.cpp` | 3 | sneak/crouch (Num9) | `NotifyAnimationGraph("SneakStart/Stop")` | one-shot promote |
| `WeaponDraw.cpp` | 4 | draw/sheathe (Num5) | `DrawWeaponMagicHands(bool)` | one-shot (sticky) |
| `Headtrack.cpp` | 5 | look-at (Num3) | `AIProcess::SetHeadtrackTarget` (own point slot) | **known-incomplete block (Tick re-assert; loses to a package-locked follower)** |
| `CombatTarget.cpp` | 6 | combat-target STEER (Num-) | `StartCombat(player)` reloc; `StopCombat()` | promote (steers, not pins) |
| `CastingSelect.cpp` | 8 | cast selection (Num4) | own `selectedSpells[kRightHand]` + `caster->currentSpell` | source-block |
| `Dialogue.cpp` | 10 | dialogue (Num6) | `PauseCurrentDialogue()` | one-shot |
| `Attribute.cpp` | 11 | disposition (Num2) | 4 AVs: aggression/confidence/assistance/morality | source-block |
| `Idle.cpp` | 12 | idle/anim (Num+) | `NotifyAnimationGraph("IdleForceDefaultState")` | one-shot |
| `ShoutPower.cpp` | 14 | shout select (Num*) | `ActorEquipManager::EquipShout` | one-shot (sticky) |
| `Equipment.cpp` | 15 | equip/unequip (Num.) | `GetEquippedObject` + `UnequipObject`/`EquipObject` (melee-vs-ranged lever) | source-block |
| `Detection.cpp` | 16 | stealth (Num8) | `kMovementNoiseMult` + `kDetectLifeRange` AVs | source-block |

- **What breaks (all channels):** each must (1) keep the package coherent — none
  substitutes the package (§5); (2) capture-and-restore engine state in `Release`,
  keyed by the per-NPC state map (guard `actor` null — it may have unloaded); (3)
  guard every struct-member write. `Engage`/`Tick`/`Release` are game-thread only
  (#12). Only `Headtrack` overrides `Tick` (flagged known-incomplete, #2). Movement
  FULL block, `StartCombat`, and KeepOffset use Address-Library IDs (#8), VR-refused.

### NOT built (probe-gated GAPs — do not add without a live probe)
Movement PROMOTE feed (ch.1, `IMovementDirectControl` unnamed), combat-target PIN
(ch.6, block the threat re-selector), combat ACTIONS behavior tree (ch.7), casting
TRIGGER suppression (ch.8), headtrack all-types full block (ch.5), sustained package
procedures (ch.9), facial-expression setter (ch.13). See `Docs/CHANNEL-MAP.md` "Need
live probing" and STATUS "post-first-release gap work".

## How to add a channel (the whole recipe)
1. Copy `channels/Speed.cpp` to `channels/<Facet>.cpp`.
2. Rename the class; set `Name`/`ChannelNo`/`ServesIntent`/`Hotkeys`; capture+apply
   in `Engage(actor)`, restore+erase in `Release(actor)` (and `Tick` only if a
   flagged known-incomplete block).
3. End with `APMF_REGISTER_CHANNEL(<Class>);`.
4. Nothing else — CMake GLOBs it, the registry picks it up, the help log lists it.
   (A NEW client intent = APPEND one value to `APMF_API.h`'s `Intent` enum, #14.)
