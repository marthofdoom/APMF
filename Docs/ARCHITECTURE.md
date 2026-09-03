# APMF Architecture

The engineering companion to `design.md` (the why) and `Docs/CHANNEL-MAP.md` (the
per-channel catalog). This describes the CODE: the two layers, the arbiter/registry,
the channel-module pattern, and the recipe for adding a channel. `#N` = a numbered
rule in `Docs/INVARIANTS.md`.

## 1. Two layers

**Layer 1 — the core spine (`native/core/`).** A single central hook —
`Actor::Update` at vtable index `0xAD` — makes APMF the arbiter of every NPC's AI
tick. Patched once on the `Character` (and `PlayerCharacter`) vtable, so every NPC
routes through one thunk with no per-actor hooks (design.md §3). The thunk runs the
original tick first, then hands the actor to the `ControlMap` (via the `Arbiter`
façade). The PlayerCharacter seat additionally DRAINS the client-API queue once per
frame (§2a).

**Layer 2 — the client API (`APMF_API.h` + `core/ClientAPI.cpp`).** An INTER-PLUGIN
C-ABI. A separate client DLL (MFO) declares intents (`Request(actorFormID, intent,
basis)` → handle … `Release(handle)`, or `RequestEx(…, const APMF_Param*)` to name
WHICH thing — the spell for cast-select, the target for combat-target) instead of
claiming a package or an alias. The contract is POD structs of function pointers
(`APMF_API_v1`; `APMF_API_v2` appends `RequestEx` via prefix extension, `kABIVersion=2`)
— no C++ class, no STL, no vtable crosses the DLL boundary (#14/#14a). A client obtains
it via the exported query function `APMF_GetInterface`. `Request`/`RequestEx`/`Release`
forward to the SAME
`ControlMap` enqueue path the test-surface hotkeys drive, so there is one control
path, not two. APMF holds zero client-specific code (#14).

```
              Actor::Update(0xAD)  ── every NPC, every frame
                       │             (Character seat: ANY thread, field-proven
                       │              not single-threaded -- #12; Player seat:
                       │              the one WRITER/main thread)
                 Hook::thunk  (original first, then:)
              ┌────────┴─────────────────────────────┐
   NPC seat:  │                        Player seat:   │
   Arbiter::OnActorUpdate(actor)       Arbiter::OncePerFrame()
   → ControlMap::OnActorUpdate         → ControlMap::Drain()
     • m_anyControlled==0? return        • copy m_current -> a private working map
       (relaxed, no snapshot touch)      • apply queued Request/Release ops to it
     • acquire-load m_published          • sweep unloaded controlled NPCs
       (a LOCAL shared_ptr snapshot)     • Publish() a NEW snapshot -- ONLY if
     • ONE hash lookup on that              something changed (copy-on-CHANGE;
       frozen generation                    a no-op frame allocates nothing)
     • miss → return (uncontrolled)
     • hit  → Tick engaged channels      (the working map + m_index are mutated
              + obsTick fetch_add         ONLY on the writer thread -- Drain /
              (the ONE reader-mutable     ReleaseAll / Clear; see #12)
              field) + PACKAGE STABLE ~1/s

   Hotkeys ─► Arbiter::DispatchHotkey ─┐   (test surface: aim + key adds an NPC)
   Client DLL ─► APMF_GetInterface ────┤─► ControlMap::EnqueueRequest/Release
     (Request/Release, ANY thread)     ┘   (brief queue lock; applied at Drain)
```

## 2. The control map, the arbiter, and the registry

**`ControlMap`** (`core/ControlMap.{h,cpp}`) is the scalable heart (Phase 1). An RCU
snapshot: a hash map keyed by NPC `FormID` → that NPC's control state (its engaged
channels, and per channel the list of client claims + the package captured at first
control), published as an immutable `std::shared_ptr<const MapType>` generation.
Any number of NPCs are controlled independently and simultaneously.
- `EnqueueRequest/EnqueueRelease` — thread-safe (any thread): allocate a handle
  (atomic), push a POD op under the queue lock, return. Never touch the map.
- `Drain()` — writer thread, once per frame: copy the last-published snapshot into a
  private working map, apply the queued ops against it (engage a channel on the 0→1
  claim, release on 1→0, arbitrate extra claims by basis), sweep controlled NPCs that
  have unloaded, then publish a NEW snapshot — ONLY if something actually changed.
- `OnActorUpdate(actor)` — the per-NPC hot path (#12/#13), ANY thread: a relaxed
  `m_anyControlled` check, then an acquire-load of a LOCAL snapshot copy + ONE hash
  lookup on that frozen generation; a miss (uncontrolled NPC) returns immediately; a
  hit ticks that NPC's engaged channels, bumps the one reader-mutable field
  (`obsTick`, a `mutable std::atomic`), and logs PACKAGE STABLE ~1/s.
- `ReleaseAll(why)` — restore + clear every controlled NPC (disengage-all /
  kPreLoadGame): every channel's `Release()` (where AV-ledger restore fires) runs
  BEFORE the empty snapshot is published, so a reader can never observe a
  torn/partially-cleared map.

Arbitration: when two clients claim the SAME channel on the SAME NPC, the higher
`basis` owns it; on a tie the earlier claim owns it. The channel stays engaged until
the LAST claim is released (claims refcount the engagement). Only ONE source holds
the coherent package slot; "multiple mods at once" is arbitration over that one
body, never package substitution (§5, #3).

**`Arbiter`** (`core/Arbiter.{h,cpp}`) is the thin coordinator the hook / input /
plugin talk to: it delegates `OnActorUpdate`/`OncePerFrame`/`ReleaseAll` to the
`ControlMap` and owns the crosshair TEST SURFACE — `DispatchHotkey` resolves the
aimed NPC and toggles a test claim for that key's channel through the same enqueue
path (a dedicated key releases all).

**`Registry`** (`core/Registry.{h,cpp}`) is a flat list of every `Channel` instance.
Channels self-register at load — `APMF_REGISTER_CHANNEL(Type)` expands to a
file-scope `AutoRegister<Type>` whose constructor builds a program-lifetime instance
and calls `Registry::Register`. It resolves an intent → its channel
(`ChannelForIntent`) and a hotkey → its channel (`ChannelForHotkey`); the list is
built once at load and immutable after, so those reads are safe from the client's
worker thread (#12). No central switch to edit when a facet is added (#9).

## 2a. The per-frame drain seat (single-writer, RCU-published)

The PlayerCharacter ticks exactly once per frame on the game thread — the right seat
to `Drain()` the client-API queue. So the control map's WRITE side is mutated on ONE
serial thread (Drain + ReleaseAll + Clear). Field evidence (`[threadcheck]`,
2026-09-02) proved the READ side is NOT confined to that same thread — a `Character`
`0xAD` seat fired on a different worker thread than the Drain seat. So the writer
builds its next working copy, then PUBLISHES it as a new immutable snapshot
(`ControlMap::Publish`, `std::atomic<std::shared_ptr<const MapType>>`,
`memory_order_release`); every per-NPC `OnActorUpdate`, on whatever thread it runs,
acquire-loads a local copy of that snapshot and reads it lock-free with no torn
reads (#12). A worker-thread `Request` is applied at most one frame later
(invisible).

## 3. The channel-module pattern

Every directable facet is one `Channel` subclass in its own small file under
`native/channels/`. The interface (`core/Channel.h`):

- `Name()` / `ChannelNo()` — identity + the CHANNEL-MAP number.
- `ServesIntent()` — the client `Intent` this channel serves (its unique key; the
  `ControlMap` resolves a Request's intent to the matching channel).
- `Hotkeys()` — the test-surface keys (empty if none).
- `Engage(id, actor, param)` — first claim landed on this NPC: capture the prior
  engine state and apply the source-block using the winning claim's `param` (a
  parameterized channel reads `param.form`/`fval`/`ival`; all-zero == its default).
  Called once per 0→1 claim transition, game thread.
- `OnOwnerChanged(id, actor, param)` — the winning claim changed (a higher-basis
  claim arrived, or the owner released and another now wins): re-point a
  parameterized channel at the new winner's `param` WITHOUT re-capturing restore
  state. Default no-op (parameterless channels do not care who owns them).
- `Tick(actor)` — per-tick drive. **Default empty** — a clean source-gate needs no
  per-tick work (#1). Only a known-incomplete block overrides it.
- `Release(actor)` — last claim gone (or the NPC unloaded, `actor` may be null):
  restore any engine state Engage changed and drop the per-NPC entry (#5).

A channel is a program-lifetime singleton operating on ANY number of NPCs; it keeps
per-NPC restore data in its OWN `std::unordered_map<FormID, State>` (game-thread
only, #12). The `ControlMap` refcounts client claims and calls Engage/Tick/Release
keyed by actor — the channel never sees the toggle/hotkey logic.

APMF MODERATES; it NEVER manufactures or sustains an AI decision (#0, design.md §1a).
Once a facet is owned on an actor, APMF is the arbiter of who controls it. A channel
does exactly three things: ARBITRATE (record the owner), DENY (suppress the losing
source at its source), and — for a facet with no meaningful deny form and no AI
decision to arbitrate around — PROMOTE a bounded, one-shot, client-requested action
(#0c). It calls NO decision-generating engine function (`StartCombat`,
`CastSpellImmediate`, movement drive, a `selectedSpells`/`EquipShout`-style selection
write); where the client already has its own proven mechanism for a selection, the
CLIENT executes it. Kinds:
1. **DENY / true source-block (no re-assert)** — set the input the AI itself reads, or
   deny the losing source once, so nothing competes. Robust even against a package-locked
   follower. Examples: AI-attribute AVs (ch.11), movement FULL block (ch.1
   `KeepOffsetFromActor` self + `SetDontMove` — the move intent is nulled at the source),
   gait (ch.1a), detection (ch.16), equipment (ch.15), dialogue pause (ch.10 — suppresses
   the actor's own in-progress dialogue, does not manufacture it). These do NOT override
   `Tick`.
2. **Arbitration-only (no engine write)** — record that a client owns the facet so APMF
   is the single arbiter; the CLIENT executes. Combat-target (ch.6 — client writes
   `currentCombatTarget`), casting (ch.8 — client writes `selectedSpells` + grants its AI
   consent), and shout/power select (ch.14 — client writes its own `EquipShout`) are
   here. APMF makes NO combat/cast/equip call for them (#0).
3. **Bounded one-shot promote (#0c, sanctioned)** — a single deterministic engine call
   at Engage/Release for a facet with no deny form and no AI decision to arbitrate
   around: weapon draw (ch.4, `DrawWeaponMagicHands`), stance toggle (ch.3,
   `NotifyAnimationGraph SneakStart/Stop`), idle/animation (ch.12,
   `NotifyAnimationGraph IdleForceDefaultState`). No per-tick work, no re-assert.
4. **Known-incomplete block (flagged)** — we have NOT yet blocked the AI's own write to
   the facet, so a re-assert stopgap holds it imperfectly. A FAILED block, flagged (#1,
   #2), never called clean. Fix: block the AI's write at the 0xAD hook. Headtrack (ch.5)
   is the only one today, and it says so.

## 4. Version robustness

- Hook a VIRTUAL (vtable index `0xAD`), never a call-site offset (#6).
- Engine calls via CommonLib accessors / named methods, or Address-Library
  `RELOCATION_ID(SE,AE)` for the DENY-gate functions the CommonLib rev does not bind
  (`KeepOffsetFromActor`/`SetDontMove` — movement full-block). Never a hardcoded
  call-site offset. (APMF does NOT call `StartCombat`/`CastSpellImmediate` at all —
  those are behavior, the client's job, #0; the CTD that taught us this is in #8.)
- Guard every struct-member write with a null-check on the accessor (#7).
- VR is refused at install (#6) — the `0xAD` index is unverified for VR.

## 5. How to add a channel (recipe)

1. Copy `channels/Speed.cpp` → `channels/<Facet>.cpp`.
2. Rename the class; set `Name`/`ChannelNo`/`ServesIntent`/`Hotkeys`.
3. `Engage(id, actor, param)`: capture the prior state into your per-NPC map, apply
   the gate (a parameterized channel reads `param`; all-zero == its default).
4. `Release(id, actor)`: restore from the per-NPC map (guard `actor` null), erase it.
   Override `OnOwnerChanged(id, actor, param)` only if parameterized.
5. `Tick`: leave it out UNLESS the facet is a known-incomplete block — then say so
   in the module header and INVARIANTS.
6. End the file with `APMF_REGISTER_CHANNEL(<Class>);`.
7. If the facet needs a NEW client intent, APPEND one value to the `Intent` enum in
   `APMF_API.h` (append-only, #14). Reuse an existing intent otherwise.

Nothing else is touched: CMake GLOBs the new file, the registry picks it up at
load, and the startup help log lists its hotkeys. This one-file-per-facet property
is a hard requirement — keep it (the appended enum value is the sole exception).
