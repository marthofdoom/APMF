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
The ONLY file a client shares with APMF. POD structs of function pointers
(`APMF_API_v1`: `Request`/`Release`; `APMF_API_v2`: + `RequestEx` carrying the POD
`APMF_Param`; `APMF_API_v3`: + `Repoint` = re-point an existing claim's param in place,
same handle), the `Intent` enum, `Handle`, and the exported query-fn name. No C++
class / STL / vtable crosses the boundary. `kABIVersion = 3`.
- **What breaks:** APPEND-ONLY forever (#14/#14a). Never reorder/change a shipped
  `Intent` value, struct, or fn-pointer slot — a v1 client must keep working. New ABI
  = a new `APMF_API_vN` whose leading members mirror v(N-1) exactly (prefix
  extension), + appended slots; `APMF_Param` grows only at the END. A client (MFO)
  and APMF are separately built DLLs; this header + `APMF_GetInterface` are the ONLY
  seam. APMF carries zero client-specific code.

### `native/core/ClientAPI.{h,cpp}` — the C-ABI implementation
The exported `APMF_GetInterface(abiVersion)` hands over a static POD `APMF_API_v3`
(as a base `APMF_API_v1*`; a client casts up to the newest struct it uses); its
`Request`/`RequestEx`/`Release`/`Repoint` fn-pointers forward to
`ControlMap::EnqueueRequest/Release/Repoint` (Request == RequestEx with a null param).
`RequestEx`/`Repoint` copy the client's `APMF_Param` synchronously (never retained).
`Repoint(handle,param)` updates a live claim's param + re-points its channel if it
owns it — no release/re-request (the "own the gambit" retarget primitive).
- **What breaks:** the exported fn must stay `extern "C"` + undecorated
  (`APMF_GetInterface`) or clients' `GetProcAddress` fails. Every exported body is a
  `try/catch(...)` — NO exception may unwind across the client DLL (#14). The enqueue
  path is the ONE control path (the hotkeys use it too) — never add a parallel one.

### `native/core/ControlMap.{h,cpp}` — the multi-NPC engine (Phase 1 heart), RCU snapshot
`unordered_map<FormID, NpcCtl>` published as an immutable `shared_ptr<const MapType>`
generation (each NpcCtl = engaged channels + per-channel client claims + captured
package; each `Claim` carries its `APMF_Param`). `EnqueueRequest` (takes
`const APMF_Param*`, copied) / `Release` / `EnqueueRepoint` (any thread; brief queue
lock, atomic handle), `Drain` (WRITER thread, once/frame: copy `m_current` into a
private working map, apply `kRequest`/`kRelease`/`kRepoint` ops + sweep unloaded,
then `Publish()` a NEW snapshot ONLY if something changed), `OnActorUpdate` (ANY
thread — the `Character` `0xAD` seat is field-proven multi-thread, `[threadcheck]`:
relaxed `m_anyControlled` check → acquire-load a LOCAL snapshot copy → one hash
lookup on that frozen generation → tick engaged → `obsTick.fetch_add`, the one
reader-mutable field, a `mutable std::atomic`), `ReleaseAll`/`Clear` (writer thread;
restore/wipe then `Publish` an empty snapshot). Arbitration by basis (higher wins,
tie → earliest); claims refcount. On a real owner change (add, release, or
`ApplyRepoint`) a parameterized channel gets `OnOwnerChanged(winner.param)`; `Engage`
gets the winning claim's param. `ApplyRepoint` updates a claim's stored param and, if
it owns the channel, re-points it in place (same handle — no release/re-engage).
- **What breaks:** the RCU contract (#12) — the working map/`m_index` are mutated
  ONLY on the writer thread (Drain/ReleaseAll/Clear, all the same MAIN thread; API
  calls only enqueue) and published via `Publish()`
  (`m_published.store(..., release)`); readers `acquire`-load a LOCAL `shared_ptr`
  copy and must treat every `NpcCtl` field as read-only EXCEPT `obsTick`.
  Reader-mutate anything else, or mutate the working map/index off the writer
  thread, and you race across hundreds of NPCs. The hot path (#13) must stay the
  relaxed pre-gate + ONE lookup for an uncontrolled NPC — no allocation, no all-NPC
  scan, or you tax the whole game. Handles must stay atomic-allocated so `Request`
  returns before Drain; ops FIFO. `SnapshotIsLockFree()` DISCLOSES (never assumes)
  whether the snapshot pointer is actually lock-free on the build toolchain — logged
  once at `Hook::Install`; not-lock-free is acceptable for this small map but must
  stay visible, never silent.

### `native/core/AvLedger.{h,cpp}` — co-saved AV override ledger
`(FormID, ActorValue) -> {prev, applied}`, co-saved via SKSE serialization (unique
ID `'APMF'`, record `'AVOV'`). `av::Override(id,actor,av,val)`/`av::Restore(id,actor,
av)` (the AV channels call these, not raw SetActorValue), `Save`/`Load(intf,version)`/
`ApplyPending`/`Revert`.
- **What breaks:** the ANTI-STRANDING backbone (#15). A persisted AV written raw
  (bypassing the ledger) is stranded on save-while-engaged + reload. `OnSave` writes
  it, `OnLoad(version)`→pending, `kPostLoadGame`→`ApplyPending` restores + clears,
  `OnRevert` wipes. CLOBBER GUARD: restore only when the AV still equals `applied`
  (else a quest/mod's newer value wins). Keys by `(FormID, AV)` — one channel per AV.
  Only AV channels are co-saved/save-safe; Equipment mutates persisted inventory but
  is NOT co-saved (must not be held across a save — #15). Game/main-thread only.

### `native/core/Hook.{h,cpp}` — the central seat
`Character` + `PlayerCharacter` vtable patched once at index **`0x0AD`**
(`Actor::Update(float)`); the thunk calls the original FIRST, then
`Arbiter::OnActorUpdate(this)`. The PlayerCharacter seat also calls
`Arbiter::OncePerFrame()` → `ControlMap::Drain()` (once/frame, on the single WRITER
thread — Drain publishes an RCU snapshot; `ControlMap`'s readers are NOT confined to
this thread, see #12). `[threadcheck]` (retired to an informational one-time log,
2026-09-02): confirms the `Character` seat runs on a different thread than the
`PlayerCharacter`/Drain seat — expected post-RCU, no longer a warning. `Install()`
also logs, once, whether the RCU snapshot pointer is actually lock-free on this
toolchain (`ControlMap::SnapshotIsLockFree()`) — disclosed, never assumed.
VR-refused. Installed once.
- **What breaks:** the index `0x0AD` is the whole version-robustness thesis
  (design.md §3) — do NOT swap it for a call-site offset. The original must run
  first (we act on top of the real AI tick, never instead of it). If you hook a
  non-virtual (`EvaluatePackage`) instead, you reintroduce version fragility.
  Every NPC routes through this thunk each frame — keep it cheap (identity
  compare + early-out; see Arbiter). Do NOT re-tighten `[threadcheck]` back into a
  race warning without re-verifying `ControlMap` is still RCU-safe first.

### `native/core/Channel.h` — the channel interface
`Channel` = `Name`/`ChannelNo`/`ServesIntent`/`Hotkeys`/`Engage`/`Tick`/`Release`.
Per-NPC lifecycle (no global `engaged` flag — the ControlMap refcounts claims and
calls `Engage/Tick/Release(RE::FormID id, RE::Actor* actor)` keyed by the NPC). A
channel keeps its own per-NPC state map. `Hotkey{code,label}`.
- **What breaks:** key per-NPC state by `id`, NOT `actor->GetFormID()` — `actor` MAY
  BE NULL (a deleted form), and the channel must still erase/clean its entry then; do
  engine writes only when `actor` is non-null. Guarding the whole body on
  `if (actor)` leaks the state map on a deleted actor.
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

### `native/core/MainThread.{h,cpp}` — confirmed-main-thread task pump (feat/cast-act)
`Post(fn)` (any thread, mutex-guarded push) / `Pump()` (drains a local swap, FIFO,
runs every queued task once). Called from `Arbiter::OncePerFrame` right after
`ControlMap::Drain()` -- reuses the SAME confirmed-main `PlayerCharacter` 0xAD seat
`core/Hook.cpp` already proved single-threaded, rather than trusting SKSE's
`TaskInterface::AddTask` (MFO's own hard lesson: AddTask does not reliably land on
a main-thread-safe seat for equip/3D work).
- **What breaks:** `Pump()` must ONLY be called from that one confirmed seat, never
  from `OnActorUpdate` (field-proven multi-thread, INVARIANTS #4/#12). A task
  Post()'d during `Pump()` runs on the NEXT `Pump()`, never re-entrantly.

### `native/core/CastExecutor.{h,cpp}` — ch.8 SelectSpell's +ACT mode (feat/cast-act)
Wired 1:1 from `channels/CastingSelect.cpp`'s `Engage`/`OnOwnerChanged`/`Release`.
Turns a `kIntent_SelectSpell` claim into APMF OWNING the cast: `ResolveHands` (0
auto/1 right/2 left/3 dual, `param.ival`) + `ResolveTarget` (`param.target` if the
client named one explicitly -- the heal-the-player fix, 2026-09-05: a ch.6
`kIntent_CombatTarget` claim is the actor's FOE, never who to heal -- else a
winning ch.6 claim, else self; `param.posX/Y/Z` is RESERVED, not yet read) ->
`StartHandDrive` per resolved hand (delivery-flip `proxy::Acquire` if a
self-delivery spell resolves to a non-self target, an internal `kIntent_Cast`
protection claim via `ControlMap::EnqueueCast`,
`ActorEquipManager::EquipSpell`) -> `PhaseSelect`/`PhaseFire` (the observed
BeginCast->Charging->Charged->SpellFire sequence, `core/MainThread.h`-posted across
frames) -> `TeardownHand` (interrupt/anim/deselect/release-claim/free-proxy) or
`FireFallback` (`CastSpellImmediate` on the kInstant caster) on any degrade path
(VR, never-selects, never-charges). Per-actor/per-hand state (`g_drives`) is
writer-thread-only (Drain seat + MainThread::Pump, same thread) -- no lock.
- **What breaks:** the whole per-hand PROTECTION relies on feat/deny-perhand's
  CastGate/EquipGate/ActionGate deny already being live -- this module assumes
  EXCLUSIVE hand ownership once its internal claim is applied, not a race to
  defend against. On VR (no per-hand deny installed there) it MUST skip the
  animated drive entirely and go straight to `FireFallback` (see `ApplyDesired`'s
  VR branch) -- do not let a future edit route VR through the equip/animate path.
  `TeardownHand`'s proxy-free is conditional on the SIBLING hand not still using
  the SAME proxy form (`kHandDual` shares one proxy across both hands, keyed by
  owner not hand) -- freeing unconditionally there corrupts the surviving hand's
  equipped form.

### `native/core/Input.{h,cpp}` — test surface
`InputSink` (keyboard button-down) → `Arbiter::DispatchHotkey` (+ `probe::OnHotkey`).
`LogHelp` enumerates the registry's hotkeys.
- **What breaks:** test surface only — do NOT let gameplay logic depend on it
  (the real driver is the client API). Adding a channel needs NO edit here;
  hotkeys come from the channel's own `Hotkeys()`.

### `native/core/Allowance.{h,cpp}` — the reusable ALLOWANCE TEMPLATE (Docs/ALLOWANCE-TEMPLATE.md §3)
`DerivesFrom` (install-time RTTI derivation walk: reads the CompleteObjectLocator*
at `vtableAddr-8`, walks `ClassHierarchyDescriptor::baseClassArray`, confirms the
expected base's TypeDescriptor appears — the ENGINE_NOTES §0.28
CombatMagicCasterArmor lesson turned into a build-time-safe runtime check),
`InstallOnVtables<ThunkFn>` (header-only template: write_vfunc across a vtable
list after RTTI-verifying each, storing per-vtable originals keyed by runtime
address, logging + SKIPPING a non-deriving symbol rather than installing
blind), `Allowed` (the one shared "flip YES->NO" decision: a lock-free RCU
`ControlMap::TryGetOwningClaim` read — never a mutex), `AllowedCast`/
`AllowedCastForHand` (ch.8b `kIntent_Cast` exclusivity via
`ControlMap::TryGetCastClaim`; the `ForHand` overload additionally takes an
`allowance::Hand{kUnknown,kLeft,kRight}` the CALLER resolved from its own
engine-native signal, and ALLOWS without narrowing when it differs from the
claim's `CastFlags::kCastFlag_LeftHand` bit — feat/deny-perhand, INVARIANTS
#18). Consumed today by `core/CastGate.cpp` (T2c) and `core/EquipGate.cpp`
(T2a); T1/T3/T4 reuse the same pieces when built.
- **What breaks:** `Allowed`/`InstallOnVtables`'s thunk callers run on COMBAT
  THREADS (§5) — never take a lock, never touch the follower/actor list, never
  call anything beyond the stored `orig` + one ControlMap read. `DerivesFrom`
  must run at INSTALL time only (main thread, kDataLoaded) — it is not
  thread-safe to call from a hot thunk (it doesn't need to be; install-once).

### `native/core/CastGate.cpp` — T2c: CheckCast allowance (the hard cast gate)
Hooks `MagicCaster::CheckCast` (vtable slot **0x0A**) on `VTABLE_ActorMagicCaster[0]`
ONLY (the other two ActorMagicCaster vtable entries are base-subobject vtables of
the same class, not separate casters — patching them clobbers unrelated engine
vtables). Resolver = `MagicCaster::GetCasterAsActor` (an ordinary virtual call
through the object's own unhooked slot 0x0C). Denies any spell whose FormID
isn't the winning `kIntent_SelectSpell` claim's `param.form`; sets
`CannotCastReason::kMultipleCast` on deny. VR-refused, install-once
(`plugin.cpp` kDataLoaded, after `hook::Install()`).
- **What breaks:** this is the PRIMARY cast allowance — CheckStartCast (T2's
  advisory twin, not built) leaks (a denied spell still fires per MFO field
  evidence); CheckCast is the one that actually stops the charge. Only
  `VTABLE_ActorMagicCaster[0]` may be patched at slot 0x0A — `[1]`/`[2]` are a
  DIFFERENT interface (anim-graph holder / event sink) at the same class.
  PER-HAND (feat/deny-perhand): `a_this` is already a per-hand `MagicCaster`
  (one per `RE::MagicSystem::CastingSource`); the ch.8b `kIntent_Cast` narrowing
  resolves the hand via `a_this->GetCastingSource()` (slot 0x15, ordinary
  unhooked virtual call) and feeds `Allowance::AllowedCastForHand`, so a
  single-hand cast claim leaves the OTHER hand's charge decision untouched.

### `native/core/EquipGate.cpp` — T2a: CheckShouldEquip allowance (per-item equip gate)
Hooks `CombatInventoryItem::CheckShouldEquip` (vtable slot **0x0F**) on the 30
concrete spell/staff `CombatInventoryItemMagicT<item,caster>` instantiations —
the identical set MFO's own `CombatStyle.cpp` equip gate patches (mirrored here
APMF-side, mutex-free). Resolver = `CombatController::attackerHandle` @0x28
(`static_assert`s pin it `<0x68`, AE-safe). Denies any spell/staff item whose
FormID isn't the winning `kIntent_SelectSpell` claim's `param.form`.
VR-refused, install-once.
- **What breaks:** deliberately NOT the design doc's aspirational 87-vtable /
  Melee-Ranged-Shield-Torch count — verified 2026-09-02 against the pinned
  upstream that those 4 categories have NO CONCRETE C++ CLASS in the pinned
  CommonLibSSE-NG headers (no vtable symbol exists to hook), and
  potion/scroll/shout are deliberately excluded on the SAME v1.0.32 gameplay
  lesson MFO's own gate already proved (denying them would only ever be a
  false-positive block on combat drinking/shouting). Widen only on new header
  evidence, never by guessing a symbol name.
  PER-HAND (feat/deny-perhand): `a_this` (`CombatInventoryItem`) carries its
  OWN `itemSlot.equipSlot` (`static_assert`'d @0x20); compared against
  `BGSDefaultObjectManager`'s Left/Right Hand default objects to resolve the
  hand, fed into `Allowance::AllowedCastForHand` for the ch.8b narrowing —
  same per-hand guarantee as CastGate above. `kIntent_SelectSpell`/
  `kIntent_Equipment` (`Allowed`, not `AllowedCastForHand`) stay actor-wide —
  unchanged, per-hand was scoped to `kIntent_Cast` only.

### `native/core/AliasPkgProbe.{h,cpp}` — the 0x49 package-offer PROBE (throwaway)
Demystifies the design.md §5a package-tier promote. `Install()` ← `plugin.cpp` kDataLoaded
(after `hook::Install`): `write_vfunc` **0x49** `CheckForCurrentAliasPackage` on
`VTABLE_Character` ONLY (never PlayerCharacter — §0.38). Thunk `TESPackage*(Actor*)`: census
(hit count/thread/last pkg — Phase 0) + return the client's package if the actor is the
claimed offer, else `original(self)`. `OnHotkey` (DIK 0x57) toggles a single-actor claim
(two atomics — lock-free); `OncePerFrame` ← `Arbiter::OncePerFrame` runs the pending
`EvaluatePackage(true,false)` on the game thread + the periodic census. VR-refused.
- **What breaks:** NOT wired to any client, NOT a travel/nav build — a probe for a field
  test, gated behind arm + `kProbePackageForm` (0 ⇒ Phase 0 only). Never touch alias/
  run-once state (INVARIANTS #3a). Delete wholesale once the mechanism is proven or dead.

### `native/core/NonAliasProbe.{h,cpp}` — OBSERVE-ONLY 0xDF hook + 0x49 assist + RTTI dumper
Docs/PROBE-NONALIAS-PACKAGE.md's runtime probe: does `Actor::CheckForCurrentAliasPackage`
(0x49, `core/PackageGate.cpp`'s existing hook) fire for a NON-alias package actor (Cicero,
`0009BE51`) at all, and does `Actor::PutCreatedPackage` (0xDF, RTTI-verified via
`core/Allowance.h::InstallOnVtables` on `VTABLE_Character[0]`) ever carry that same package?
Pure logging, chained to the original unconditionally, no decision/denial (INVARIANTS #17).
`0x45` NumLock toggles the 0x49/0xDF observe log (OFF by default; PackageGate.cpp's thunk
checks `IsEnabled()`/`RateLimitOK()` from here to add its own log line on the same switch);
`0x46` ScrollLock one-shots a vtable/RTTI dump (module-relative RVAs + best-effort RTTI type
name, mirroring `Allowance.cpp`'s `DerivesFrom` walk) of the crosshair-aimed actor — the
reusable "find sites ourselves" tool so a future probe skips hand-reversing the vtable layout.
- **What breaks:** THROWAWAY/instrumentation-only, same class as `native/core/
  NativeBitProbe.{h,cpp}` — not wired to any client. Never touch alias/run-once state (#3a).
  0xDF is a 1.6.1170-pinned raw index (Docs/PROBE-NONALIAS-PACKAGE.md §2) — no named
  CommonLib binding exists to prefer over it today.

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
| `CombatTarget.cpp` | 6 | combat-target CLAIM (Num-) | **ARBITRATION-ONLY** — records the owner; makes NO engine combat call (no `StartCombat`, no `currentCombatTarget` write). The CLIENT commands the target. Release relinquishes | arbitration-only (#0); client executes |
| `CastingSelect.cpp` | 8 | casting CLAIM (Num4) | Claim itself still makes NO `selectedSpells`/`CastSpellImmediate` write (client selects + grants AI consent) — but the claim is now a REAL allowance: `core/CastGate.cpp` (T2c CheckCast) + `core/EquipGate.cpp` (T2a CheckShouldEquip) deny any spell/item that isn't the claimed `param.form` | claim + T2 enforcement; client still executes |
| `Dialogue.cpp` | 10 | dialogue (Num6) | `PauseCurrentDialogue()` | one-shot |
| `Attribute.cpp` | 11 | disposition (Num2) | 4 AVs: aggression/confidence/assistance/morality | source-block |
| `Idle.cpp` | 12 | idle/anim (Num+) | `NotifyAnimationGraph("IdleForceDefaultState")` | one-shot |
| `ShoutPower.cpp` | 14 | shout select (Num*) | `ActorEquipManager::EquipShout` | one-shot (sticky) |
| `Equipment.cpp` | 15 | equip/unequip (Num.) | `GetEquippedObject` + `UnequipObject`/`EquipObject` (melee-vs-ranged lever) | source-block |
| `Detection.cpp` | 16 | stealth (Num8) | `kMovementNoiseMult` + `kDetectLifeRange` AVs | source-block |

- **What breaks (all channels):** each must (1) keep the package coherent — none
  substitutes the package (§5); (2) capture-and-restore engine state in `Release`,
  keyed by the per-NPC state map (guard `actor` null — it may have unloaded); (3)
  guard every struct-member write. **A channel MUST NOT generate behavior (#0):** no
  `StartCombat`, `CastSpellImmediate`, movement drive-feed, or anim trigger — only
  arbitrate + DENY. `Engage`/`Tick`/`Release` are game-thread only (#12). Only
  `Headtrack` overrides `Tick` (flagged known-incomplete, #2; ch.6/ch.8 are now
  arbitration-only, no `Tick`). Movement FULL block + KeepOffset (the DENY gate) use
  Address-Library IDs (#8), VR-refused.

### NOT built (probe-gated GAPs — do not add without a live probe)
Movement PROMOTE feed (ch.1, `IMovementDirectControl` unnamed), combat ACTIONS
behavior tree (ch.7 / T1, PROBE-gated — see Docs/ALLOWANCE-TEMPLATE.md §6/§7),
the DENY of a COMPETING framework's combat-target selection at the hook (ch.6 —
still arbitration-only; the CLIENT commands the target, no T-hook yet), body
commands (T4, PROBE-gated), headtrack all-types full block (ch.5), sustained
package procedures (ch.9), facial-expression setter (ch.13). ch.8 (casting
selection) GAINED its deny 2026-09-02 (Phase 2): `core/CastGate.cpp` (T2c
CheckCast) + `core/EquipGate.cpp` (T2a CheckShouldEquip) — see those entries
above; ch.8 is no longer arbitration-only. **Load-bearing open mechanism:
cleanly DENY/starve an outranking framework's PACKAGE (Cicero/travel) — T3
(`AliasPkgProbe.cpp`'s 0x49 hook) is built as a probe but not yet folded into
the Allowance template / wired to a client.**
See `Docs/CHANNEL-MAP.md` "Need live probing" and STATUS "post-first-release gap work".

## How to add a channel (the whole recipe)
1. Copy `channels/Speed.cpp` to `channels/<Facet>.cpp`.
2. Rename the class; set `Name`/`ChannelNo`/`ServesIntent`/`Hotkeys`; capture+apply
   in `Engage(id, actor, param)`, restore+erase in `Release(id, actor)` (override
   `OnOwnerChanged(id, actor, param)` only if the channel is PARAMETERIZED; `Tick`
   only if a
   flagged known-incomplete block).
3. End with `APMF_REGISTER_CHANNEL(<Class>);`.
4. Nothing else — CMake GLOBs it, the registry picks it up, the help log lists it.
   (A NEW client intent = APPEND one value to `APMF_API.h`'s `Intent` enum, #14.)
