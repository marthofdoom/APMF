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
`SKSEPluginLoad` → log setup + messaging listener + ONE `[runtime] <version>:
cast-classify <open|gated>, group-C <open|gated>` line (2026-09-15): the exact-version
predicate (`1.6.1170 || 1.5.97`) the two per-binary-literal seats gate on, logged once
at load so a deck log states its placement state up front. `kDataLoaded` installs the
hook, registers the input sink, logs the hotkey help. `kPreLoadGame` →
`Arbiter::ReleaseAll`.
- **What breaks if you change this:** if `kPreLoadGame` stops calling
  `ReleaseAll`, engaged channels leak engine state across a save load (a follower
  stuck sneaking / silent / speed-halved). Keep the release on pre-load. The
  `[runtime]` line MUST use the same two-version predicate as `core/CastClassify.cpp`
  `Install()` and `core/AiCastSeats.cpp` GROUP C — it is a report of those gates, not a
  third gate; if one of them ever admits a third binary, change all three. The predicate
  is NOT a shared helper: `IsRuntime1_6_1170()/IsRuntime1_5_97()` are file-local to
  `AiCastSeats.cpp`'s anonymous namespace, while `CastClassify.cpp` and `plugin.cpp`
  each inline their own `REL::Version{1,6,1170,0}` / `{1,5,97,0}` compares — three
  copies to keep in step by hand. Never `REL::Module::IsAE()/IsSE()` here: in the
  pinned 3.7.0 `IsSE()` is the `default:` arm, so 1.7.104 reads as SE — though it never
  reaches this line: CommonLib terminates at `SKSE::Init` with the address-library
  dialog on 1.7.104; APMF's gates never run there.
  **ABI v11 (2026-09-23):** `kDataLoaded` also runs `poscast::Install()` and
  `spacequery::Install()`, and both revert and `kPreLoadGame` call
  `poscast::ResetAll()` BEFORE `mainthread::Discard()`. Those two Installs carry two
  more inline copies of the same exact-version predicate (five copies now).
  Co-save (ABI v11): `OnSave` also writes `poscast::SaveMarkers` (record `'XMRK'`), `OnLoad`
  dispatches `'XMRK'` to `LoadMarkers`, `OnRevert` calls `RevertMarkers` (must stay before the
  load callback, which SKSE guarantees), and `kPostLoadGame` POSTS `SweepCarriedMarkers` to the
  main-thread pump (review F4), so it runs on the first player-Update after the load.

### `native/APMF_API.h` — the inter-plugin C-ABI contract (shared with clients)
The ONLY file a client shares with APMF. POD structs of function pointers
(`APMF_API_v1`: `Request`/`Release`; `APMF_API_v2`: + `RequestEx` carrying the POD
`APMF_Param`; `APMF_API_v3`: + `Repoint` = re-point an existing claim's param in place,
same handle; `APMF_API_v4`: + `SetSpellAllowList`; `APMF_API_v5`: + `RequestCast` with
the rich `APMF_CastRequest`; `APMF_API_v6`: + `GetCastProxy`/`IsCastActive`;
`APMF_API_v7`: + `SetEquipSet` — the ch.17 equip-authority DECLARATION, plus
`kIntent_EquipAuthority = 17`, the `EquipAuthFlags` bits and `kMaxEquipSet`;
`APMF_API_v8`: + `SetEquipSetEx` — the same declaration with a HAND per item via
the 8-byte POD `APMF_EquipEntry{form@0, slot@4 (EquipSlot: Default 0/Right 1/Left 2),
reserved[3]@5}`, static_assert-pinned, + `IsEquipAuthorityEnforced` (installed AND not
INI-observe-only), plus `kEquipAuth_DenyPlayerMenu = 1<<3`; `APMF_API_v9`: +
`SetEquipScope(Handle, const APMF_EquipScope*)` — SCOPE the claim to OWNED and DENIED
`EquipCategory` masks (Armor 1<<0 / Shield 1<<1 / Right 1<<2 / Left 1<<3 / Ammo 1<<4 /
Light 1<<5, `kEquipCat_All` 0x3F) via the 16-byte POD `APMF_EquipScope{owned@0, denied@4,
reserved[2]@8}`, static_assert-pinned; nullptr resets to `{All, 0}` == v8; unknown bits
masked + logged once per handle); **ABI v10 adds NO struct and NO fn-pointer slot at all** --
`kIntent_Travel = 19` (ch.19) plus the `TravelFlags` bits ride the EXISTING
`RequestEx`/`Repoint`/`Release` slots, so a v9 client is byte-unaffected and the newest
struct stays `APMF_API_v9`; intent 18 is deliberately SKIPPED, reserved for the unauthored
ch.18 attack-selection spec); **ABI v12 adds `APMF_API_v12 : APMF_API_v11` with ONE slot,
`GetTravelLegState(FormID, APMF_TravelLegInfo*)`, the 72-byte POD `APMF_TravelLegInfo` (size
field first, static_assert-pinned; +60 ownerHandle, +64 blocker, +68 blockerKind), the
`TravelLegState` and `TravelBlocker` enums, and the travel gait bits
`kTravel_SpeedSet` (1<<2) + `kTravel_SpeedMask` (bits 3-4 = the engine's PreferredSpeed)**, the
`Intent` enum, `Handle`, and the exported query-fn name. No C++ class / STL / vtable
crosses the boundary. **The current `kABIVersion` is stated ONLY in the header**
(INVARIANTS #14b — this line said 3 while the header was at 6).
- **What breaks:** APPEND-ONLY forever (#14/#14a). Never reorder/change a shipped
  `Intent` value, struct, or fn-pointer slot — a v1 client must keep working. New ABI
  = a new `APMF_API_vN` whose leading members mirror v(N-1) exactly (prefix
  extension), + appended slots; `APMF_Param` grows only at the END. A client (MFO)
  and APMF are separately built DLLs; this header + `APMF_GetInterface` are the ONLY
  seam. APMF carries zero client-specific code.

### `native/core/ClientAPI.{h,cpp}` — the C-ABI implementation
The exported `APMF_GetInterface(abiVersion)` hands over the static POD newest-struct
object (`APMF_API_v12` since ABI v12 — read the header, not this line) as a base
`APMF_API_v1*`; a client casts up to the newest struct it uses. It returns **nullptr**
when the client asks for a version NEWER than this APMF implements (`:125-128`), which
is why a needless `kABIVersion` bump is expensive (INVARIANTS #14b). Its
`Request`/`RequestEx`/`Release`/`Repoint`/`SetSpellAllowList`/`SetEquipSet`/`SetEquipSetEx`/
`SetEquipScope` fn-pointers forward to `ControlMap::EnqueueRequest/Release/Repoint/SetSpellAllowList/
SetEquipSet/SetEquipSetEx/SetEquipScope` (Request == RequestEx with a null param; `SetEquipSet` is
`SetEquipSetEx` with every slot Default); `IsEquipAuthorityEnforced` → `equipsink::Enforcing()`.
`MinReleaseForAbi` names the first release per ABI (v7/v8/v9 = the placeholder until the cut, REVIEW-BACKLOG APMF-B10).
ABI v11 adds `FindEmptySpace` / `FindHostilesInSpace` → `core/SpaceQuery.cpp` (each a
`try/catch(...)` returning `kQuery_Failed`), and `MinReleaseForAbi(11)` = `0.9.8`.
ABI v12 adds `GetTravelLegState` → `travel::GetLegState` (`channels/Travel.cpp`, a
`try/catch(...)` returning `kLeg_None`), and `MinReleaseForAbi(12)` = `0.9.8` (v11 and v12
ship together; both static_assert pairs pin the prefix ends).
The "client too new" null (`abiVersion > kABIVersion`) logs the running APMF version
(`SKSE::PluginDeclaration::GetSingleton()`) and `MinReleaseForAbi(abi)` — a table that must
be extended on every `kABIVersion` bump.
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
`const APMF_Param*`, copied; REFUSES `kIntent_EquipAuthority` with `kInvalidHandle` while
`equipsink::Installed()` is false — logged once with `NotInstalledReason()` — so a client
never holds a ch.17 claim the seat cannot enforce; ABI v11: a `kIntent_Cast` param with
`kCastFlag_AtPosition` is diverted to `poscast::Enqueue` and NEVER becomes a claim, and
`EnqueueCast` refuses that bit by name) / `Release` / `EnqueueRepoint` (any thread; brief queue
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
it owns the channel, re-points it in place (same handle — no release/re-engage). On a
`kIntent_Cast` claim Repoint is a HEARTBEAT: it renews the TTL and updates the non-form
param fields, but a form CHANGE is refused + logged (proxy/target/flags were resolved
against the original spell and Repoint re-runs none of that) — with the one exception
that a `kCastFlag_FromPackage` claim's heartbeat necessarily carries the PACKAGE the
client named, which the claim remembers as `Claim::castSrcForm` and accepts silently
(the stored form stays the spell APMF extracted).
**ch.17 (ABI v7/v8):** each `Claim` also carries the equip-authority DECLARATION
(`equipForms[kMaxEquipSet]`/`equipCount`, POD, appended at the end; 0 == none; ABI v8
appends the parallel `equipSlots[kMaxEquipSet]` (an `APMF_API::EquipSlot` per form, all
Default for a v7 call) and `equipBadSlots` (the once-per-handle out-of-range-slot log)).
`EnqueueSetEquipSetEx` (any thread, entries copied + clamped at enqueue; `EnqueueSetEquipSet`
packs v7 forms into it with slot Default) → `kSetEquipSet` op →
`ApplySetEquipSet` (writer: validates each slot to 0..2, else Default + one warn per handle;
stores on the claim whether or not it owns the channel;
if it IS the owner it fires `channel->OnOwnerChanged` on EVERY applied declaration,
changed or not — a client re-issuing the same set is asking for it back, and that is
a declaration, not a loop; returns "stored set changed" for the Publish gate).
**ABI v9 SCOPE:** each `Claim` also carries `equipOwned`/`equipDenied` (defaults `kEquipCat_All`/0
== v8) + `equipBadScopeBits` (the once-per-handle unknown-bit log), appended at the end;
`EnqueueSetEquipScope` (any thread, the 16-byte scope copied, nullptr == the defaults) →
`kSetEquipScope` op → `ApplySetEquipScope` (writer: masks both words to `kEquipCat_All`, stores on
the claim owner or not, logs `[ctl] … SET-EQUIP-SCOPE (h=, owned=0x, denied=0x, changed|unchanged,
owner|not owner)`; fires `OnOwnerChanged` ONLY on the owner AND ONLY on a real change — a scope is
a policy, an unchanged re-send has nothing new to enforce, unlike a re-declared set).
`TryGetEquipSet(actor, EquipSetView&)` is the seat's read: any thread, relaxed
pre-gate, one acquire-load, one lookup, the WINNING claim's set copied out BY VALUE
with its `param.ival` flags, (v8) its `slots[]` and (v9) its `owned`/`denied` — ALL FROM THE
SAME SNAPSHOT READ, so the seat never pairs a set from one generation with a scope from
another; false for an unclaimed/unloaded actor.
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
  stay visible, never silent. **v9:** `owned`/`denied` MUST keep riding `TryGetEquipSet`'s
  single copy-out — a second read for the scope would let the seat see a set and a scope
  from different generations; and `ApplySetEquipScope` must keep firing `OnOwnerChanged`
  on a CHANGE only, or a scope-ticking client turns the enforce hop into a per-tick walk.

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
VR-refused. Installed once. `OnMainThread()` (ABI v11) compares the calling thread
with the id the PlayerCharacter seat records (`g_drainThreadId`); the space queries
refuse to run when it is false.
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
runs every queued task once) / `Discard()` (drops the queue WITHOUT running it and
returns the count -- called from `plugin.cpp`'s kPreLoadGame and revert handlers so a
task posted by a teardown `Release` cannot survive the world swap and fire against the
NEW world; nothing Pumps between pre-load and the first post-load player Update). Called from `Arbiter::OncePerFrame` right after
`ControlMap::Drain()` -- reuses the SAME confirmed-main `PlayerCharacter` 0xAD seat
`core/Hook.cpp` already proved single-threaded, rather than trusting SKSE's
`TaskInterface::AddTask` (MFO's own hard lesson: AddTask does not reliably land on
a main-thread-safe seat for equip/3D work).
- **What breaks:** `Pump()` must ONLY be called from that one confirmed seat, never
  from `OnActorUpdate` (field-proven multi-thread, INVARIANTS #4/#12). A task
  Post()'d during `Pump()` runs on the NEXT `Pump()`, never re-entrantly.

### `native/core/CastProxy.{h,cpp}` — the delivery-flip PROXY POOL (was `CastExecutor`)
**The forced cast DRIVE that used to live here is DELETED** (feat/ai-cast-seats-impl,
2026-09-05): `PhaseSelect`/`PhaseRest`/`PhaseDrawn`/`PhaseFire`/`PhaseHold`/
`PhaseParkWait`, `ParkHand`/`TeardownHand`/`EmitStopTail`/`StartHandDrive`/
`ResolveHands`/`ResolveTarget`, the wall-clock `Budget` plumbing, the per-hand
`g_drives` state, and the `FireFallback` (`CastSpellImmediate`) guaranteed-delivery
path. Superseded by `core/CastSeats.cpp`'s five engine seats, which make the NPC's own
AI perform the cast. Do not resurrect any of it: the fallback in particular is what
INVARIANTS #0 forbids by name.
What survived is ONE engine fact the seats cannot answer around
(disassembly-CERTAIN, `FindTargets` 0x5bc160 @0x5bc98a): a **kSelf-delivery** spell
always lands on the CASTER's own reference — the Self branch never reads
`desiredTarget`, so seat 0x0A cannot aim Fast Healing at an ally. So this module mints
a delivery-flipped `kTargetActor` COPY (`Configure`: source `data` + shared source
`Effect*` by pointer, delivery flipped) that the AI selects and casts instead.
`Acquire` (mint/re-target + TRANSIENT `AddSpell`) / `Free` (un-teach + deselect +
release the slot) / `FormForOwner` / `ResetAll` (revert + kPreLoadGame) /
`PreSaveSweep` (SKSE save). Fixed 4-slot pool keyed by OWNER, not hand (a dual cast
shares one form). WRITER/MAIN THREAD ONLY.
Called from: `ControlMap::ApplyRequest` (`Acquire`, on the writer thread where form
lookups are legal, BEFORE the claim publishes), `channels/CastCompose.cpp`'s
`Release` (`Free`, deferred one `mainthread::Post` hop so it lands AFTER the cleared
claim publishes — INVARIANTS #20), `plugin.cpp` (`ResetAll` on revert + kPreLoadGame,
`PreSaveSweep` on save).
- **What breaks:** both halves of INVARIANTS #19. (a) A proxy KNOWN to the actor when
  a save is taken persists a reference to a 0xFF dynamic form that will not exist on
  the next load — `PreSaveSweep` must stay wired into `OnSave` BEFORE any record is
  written, and `Free` must stay the single choke point every release path reaches.
  (b) `Configure` shares the SOURCE spell's `Effect*` BY POINTER, so `ResetAll` must
  clear `effects` FIRST and only then null the slot, or the load-time purge frees a
  LIVE spell's effect array through the dead proxy (MFO's `Actuation_Direct.cpp`
  lesson). `ResetAll` is ALSO what stops the pool staying permanently occupied by a
  dead owner across a revert (`ControlMap::Clear()` deliberately does not call
  `channel->Release`) — without it every later ally heal declines as overflow.
  A pool overflow must leave `castProxy == 0` and the ORIGINAL kSelf form must then
  NOT be seat-forced: casting it at an ally would silently heal the caster instead.
  DOCUMENTED CLIENT DEPENDENCY: teaching the proxy only makes it selectable once the
  actor's `CombatInventory` REBUILDS; the one confirmed dirty-trigger is a change of
  `Actor::GetCombatStyle()` (MFO's `MFO_CastStyle` swap does this). **UPDATE
  (feat/seat0-classify, 2026-09-05): APMF now ALSO forces a rebuild itself** —
  `core/EquipGate.cpp`'s `EquipGateThunk` sets `CombatController::inventory->dirty =
  true` (both real, CommonLib-declared members — no raw offset) every call it makes
  for an actor holding a live cast-seat claim. This is additive to the MFO trigger
  above, not a replacement, and only fires while the hook is already being called for
  the actor (i.e. it has at least one OTHER magic/staff item already) — see that
  file's own comment for the documented edge case.

### `native/core/PositionCast.{h,cpp}` — ABI v11 POSITION CAST (one-shot remote cast) + the shared XMarker helpers
Also exports `MarkersSupported()` / `PlaceMarker()` / `DeleteMarker()`, used by the cast
below AND by ch.19's position legs (`channels/Travel.cpp`). `MarkersSupported` is the
runtime + XMarker-base gate and does NOT depend on `[PositionCast] bPositionCast` (that
switch turns off the cast only). Changing either helper changes both callers.
A `kIntent_Cast` RequestEx with `kCastFlag_AtPosition` (`ControlMap::EnqueueRequest`
diverts it here; it is never a claim). `Enqueue` (any thread) checks the POD (flag
alone, spell and actor non-zero, finite point) and posts `Deliver` to the main-thread
pump. `Deliver` re-validates on the main thread (loaded live actor, attached cell,
SpellItem with Target Location + fire-and-forget + no Summon Creature effect + NO projectile on any effect (the engine never launches a TL projectile for a marker caster: only the player pick feeds it) + not
disease/ability/addiction), places an XMarker (`0x3B`) with
`TESDataHandler::CreateReferenceAtLocation` in the cell that CONTAINS the point (outdoors `TES::GetCell(point)` = the loaded grid cell, required attached and in the actor's worldspace; indoors the actor's cell), tracks it,
then `marker->GetMagicCaster(kInstant)` → `InterruptCast(false)` →
`CastSpellImmediate(spell, false, nullptr, 1.0, false, 0.0, actor)` (Papyrus
`RemoteCast`'s sequence) and posts `Retire` one hop later (`Disable()` + `SetDelete(true)`
after re-checking handle, FormID and base). `ResetAll` (revert / kPreLoadGame) forgets
the table without touching refs. Cap `kMaxLiveMarkers` = 16. INI `[PositionCast] bPositionCast`.
Runtime gate: exactly 1.6.1170 / 1.5.97, VR refused. Evidence:
`Docs/ADDRESS-TABLE-2026-09-15.md` ADDENDUM 2026-09-23; doctrine `Docs/INVARIANTS.md` #0 (e).
- **What breaks:** (a) making it a CLAIM: the cast seats read claims as actor targets
  (seat 0x0A writes an `Actor*`), so a position cast must never enter the control map.
  (b) casting from the ACTOR instead of the marker: the engine's Target Location branch
  ignores the target and lands at the caster's hand (both runtimes). (c) dropping the
  summon refusal: the engine applies a summon only to the casting actor, so a marker
  "summon" silently does nothing. (d) deleting by FormID instead of the tracked handle:
  0xFF FormIDs recycle; the handle's age bits are the reuse guard. (e) moving
  `ResetAll` after `mainthread::Discard` in `plugin.cpp` changes nothing today, but
  never let a `Retire` task outlive the world swap. (f) SAVE FOOTPRINT: every placed,
  not-yet-deleted marker is in the co-saved ledger (record `'XMRK'` v1, `SaveMarkers` /
  `LoadMarkers` / `RevertMarkers` / `SweepCarriedMarkers`, wired in `plugin.cpp`), so the load of
  a save that captured markers DELETES them at kPostLoadGame, under three proofs (0xFF FormID
  resolves, XMarker base, recorded position within 1u; else forgotten, never touched). Markers
  not in memory at the sweep are carried and retried at later loads (cap 64, oldest dropped
  loudly). Breaking the ledger (a PlaceMarker that does not record, a DeleteMarker that drops a
  STALE-handle entry instead of carrying it, the sweep moved into the load callback before the
  refs exist, the revert clear moved after the load callback) re-opens unbounded .ess growth.
  (g) REVIEW ROUND on `ed729ec`: the cast is OFF by default (`bPositionCast` code default 0;
  INVARIANTS #0 (e) is ADOPTED with condition (8) "not an endpoint": no animation, so no client
  ships a user-facing action on it alone); `MarkersSupported` must stay independent of that switch or
  ch.19 position legs die with it. `Enqueue` does NO form lookup (review R2-1: 3.7.0's
  `LookupByID` took no lock. Fork F1b makes it take the real read lock, and the spell checks still live in
  `Deliver`, main thread), and
  `ControlMap::CastFacetOutranks` refuses (at the call AND again in `Deliver`, R2-3c; a non-finite
  basis is refused first, R2-3b) a cast while a live cast claim owns the actor's
  facet (condition 7). The load sweep is POSTED from kPostLoadGame. `g_carry` is capped
  wherever it grows (`CapCarried`). Open deferred findings: `Docs/REVIEW-BACKLOG.md`
  APMF-B19 (sweep proofs vs a different XMarker) and APMF-B20 (player-blamed location).

### `native/core/SpaceQuery.{h,cpp}` — ABI v11 SPACE QUERIES (read-only)
`FindEmptySpace` (walk ray at feet+64u, down ray to feet-maxDrop, 8 knee-height
clearance rays, 4 ring ground rays, live-actor scan) and `FindHostilesInSpace` (high
actors + player, same worldspace/cell, 3D radius, `candidate->IsHostileToActor(side)`,
nearest first, cap `kMaxHostileResults` = 64). Rays: `bhkPickData` +
`bhkWorld::PickObject` under `worldLock` (read), filter
`(originActorSystemGroup << 16) | kCharController` (MFO Sightline's field-proven recipe);
ground = layers static/terrain/ground (the engine's own Target Location placement rule)
plus the stair helper. Synchronous; `hook::OnMainThread()` or `kQuery_NotMainThread`.
Runtime gate as PositionCast (`Install` at kDataLoaded).
- **What breaks:** calling either off the main thread (havok world and the hostility
  test are main-thread state; the refusal is the guard, do not remove it). Writing
  past the caller's `size` (append-only structs: always honour `out->size`). Using a
  hand-kept faction list instead of `IsHostileToActor`. The "stair helper counts as
  ground" choice is a judgement call, not engine evidence (see the file comment).

### `native/core/CastClassify.{h,cpp}` — ch.8b SEAT 0: CLASSIFY (2026-09-05)
**THE ROOT-CAUSE FIX** the other five seats sat downstream of and could never reach:
a heal/buff-OTHER spell (kTargetActor/kAimed RestoreHealth — Healing Hands, Heal
Other, every delivery-flip proxy) was **NEVER GIVEN a `CombatInventoryItem` at all**
by the engine's own classifier (the 23-row table at `0x20163b0` has no row for
`(Health, self=0, hostile=0)` — RE notebook J3/J9). Hooks `VTABLE_CombatMagicItemData`
slot 1 (the per-effect classification visitor, `0x81d830`, ID 45321): if the effect's
owning spell (`+0x10`) is the live `kIntent_Cast` claim's DRIVEN form for the
deliberating actor (`+0x18` `CombatController*` → `attackerHandle`), force the
self-delivery byte at `+0x4c` to 1 **before chaining** — the classifier then keys the
SAME row a self-heal uses (Restore, creator `0x824510`). Deliberately NOT restored
after the call (a multi-effect spell re-enters this thunk once per effect on the SAME
resolver; delivery is a spell-level property, so every effect of that spell should see
the same forced answer within one classify pass — see the .cpp's comment).
- **What breaks:** `CombatMagicItemData` is NOT a CommonLib-declared type in this
  pinned rev — no header, no `static_assert`. Verification is by RTTI **mangled-name
  string match** (`.?AVCombatMagicItemData@@`) at install — the RE pass did not
  establish an `RTTI_CombatMagicItemData` Address-Library ID, though the pinned 3.7.0
  `Offsets_RTTI.h` does carry one (`{687623, 395938}`, CONFIRMED table 2026-09-15); the
  string match stays (a name mismatch refuses install, never a blind write).
  **RUNTIMES (2026-09-15, `feat/apmf-1.5.97-pass`): placed on 1.6.1170 AND 1.5.97**, by
  the CONFIRMED address table's "CastClassify.h SEAT 0" slot-1 row — the ctor (SE
  `0x780F5C`) stores `[+0x10]=MagicItem`, `[+0x18]=CombatController`, `[+0x4c]=self
  flag` byte-for-byte as on AE, the slot-1 thunk is SE `0x7811F0` (id 43931), the
  vtable resolves via SE id 265000 (`0x1686BF8`). The gate is EXACT-VERSION
  (`1.6.1170 || 1.5.97`), not `IsAE()/IsSE()`; every other binary that loads (VR, any
  other 1.6.x/1.5.x) is refused with a loud line naming the version. 1.7.104 never
  reaches the gate: CommonLib terminates at `SKSE::Init` with the address-library dialog
  on 1.7.104; APMF's gates never run there. The REAL guard against a wrong-id resolve is
  the RTTI mangled-name string compare — in 3.7.0 a missing id is never a null resolve
  (`IDDatabase::id2offset` `report_and_fail`s past the end and otherwise `lower_bound`s
  with no equality check off VR, so it silently returns the NEXT id's offset); the
  `vt.address() == 0` test that precedes the RTTI walk is unreachable belt-and-braces
  (Fable tier-3 on c70767c, SEV-4). INI kill-switch `[CastSeats]
  EnableSeat0Classify` (default 1).
  Install ordering vs `core/EquipGate.cpp`/`core/CastSeats.cpp` does not matter
  (disjoint vtable). Runs on the combat thread; one lock-free RCU read
  (`TryGetCastSeatClaim`), no mutex, no engine call besides the chained original.

### `native/core/CastSeats.{h,cpp}` — ch.8b: THE ENGINE CAST SEATS (the keystone)
While a `kIntent_Cast` claim {actor A, spell S, target T} stands, APMF answers the
vfunc seats the combat AI's OWN cast decision is built out of, so the NPC's own AI
selects, equips, charges, aims, fires and channels S at T — engine animation, engine
magicka, engine LOS/interrupts. **APMF makes no `EquipSpell`, `CastSpell`,
`CastSpellImmediate`, `NotifyAnimationGraph` or caster-state write anywhere.**
FOUR seats here, on `VTABLE_CombatMagicCasterRestore[0]` **and
`VTABLE_CombatMagicCasterOffensive[0]`** ONLY — 2 of the 14 caster vtables,
RTTI-verified (`CastSeats.cpp:483-500`; Offensive added in v0.9.2 by
`feat/offense-seat-scope` because a claimed HOSTILE spell classifies into the
Offensive caster, never Restore, so a claim on it was inert):
- `0x06 CheckStartCast` -> TRUE from the claim (magicka stays enforced by
  `MagicCaster::CheckCast` inside `CastSpell`; hand-idle/`bMLh_Ready` by the leaf's
  own 0x89f3c0; the equip by seat 0x0F). Bypasses only the vanilla health threshold,
  the effect-already-active test and the 15s restrict-timer window — the exact policy
  the claim replaces.
- `0x0A GetMagicTarget` -> `out->handle = claim target's native handle; out->ptr =
  nullptr`. THREE args with a hidden 16-byte sret out-slot (CommonLib declares two and
  is WRONG — the bug that CTD'd the passive probe). Handle form is the lifetime-safe
  one: all 12 consumers branch on `handle` first and resolve it refcounted.
- `0x07 CheckStopCast` -> STOP on claim TTL / target unresolvable / target dead /
  target reached the claim's stop percent (`CastFlags` bits 8-15; 0 = full
  restoration). Left native it stops at threshold+0.25 of the ALLY — a ~0.25s pulse
  for any claim whose trigger sits above the vanilla ceiling.
- `0x0D SetupAimController` -> writes `CombatAimController+0x30` (the aim-target
  override every aim reader honours first). ALWAYS writes: the claim's target on a
  match, the engine's own ctor default 0 otherwise, so this seat can never leave a
  stale override behind. Needed for kAimed heals (the projectile flies at the aim, not
  `desiredTarget`) and for a concentration heal's tolerance/LOS re-checks.
The FIFTH seat (`0x0F CheckShouldEquip`) lives in `core/EquipGate.cpp` — see there.
- **What breaks:** **SCOPE IS THE SAFETY ARGUMENT.** `GetMagicTarget`'s implementation
  (0x81e020) is the BASE, shared by 13 of the 14 caster vtables. Widening the install
  list beyond {Restore, Offensive} aims Stagger/Disarm/Reanimate/Summon/Ward HOSTILE
  effects at the ally. (Corrected 2026-09-07: this note used to say "beyond Restore",
  which by then pointed the wrong way — Offensive has been in the list since v0.9.2 and
  is what makes an offense claim work at all. The rule is "no vtable a live claim does
  not need", not "Restore".) The second gate (`ClaimNamesThisCast`: deliberating actor holds the claim AND
  `this->magicItem` IS the claim's DRIVEN form) must stay too — and DRIVEN means
  proxy-when-one-exists, never "spell OR proxy": forcing the original kSelf form would
  heal the caster. Every thunk runs on the COMBAT thread: one lock-free RCU read, no
  mutex, no follower list, and only `CombatController` members below the AE +0x68
  divergence (`attackerHandle` 0x28). The target is a PRE-RESOLVED `ActorHandle`
  because a `TESForm::LookupByID` here would take the engine's forms-map lock.
  `+0x30` is the one RAW OFFSET in the tree (no `CombatProjectileAimController` class
  exists in the pinned CommonLib) — its three guards (INI kill-switch, install-time
  RTTI check, per-call exact vtable-identity check) are not optional; #20 says so.
  It carries NO runtime gate, by evidence, not by omission (CONFIRMED table
  2026-09-15, "MFO layout facts" CombatController row (c)): the `CombatAimController`
  ctor zeroes `[+0x30]` and the projectile aim vfunc7 reads it first on 1.6.1170,
  1.5.97 and 1.7.104. Same for `Out16` (the `GetMagicTarget` hidden sret out-slot,
  "GetMagicTarget sret" row: identical `{u32 @0, ptr @8}` on all three; SE `0x781CB0`
  + helper `0x782100`). Do not add a version gate to either; do re-cite the table if
  the shape is ever re-derived.
  Installed AFTER `core/AiCastSeats.cpp` on purpose so that passive probe keeps
  logging the ENGINE's raw answer beneath these.

### `native/core/AiCastSeats.{h,cpp}` — OBSERVE-ONLY: the AI cast/equip-decision seats
Three independently-flagged, chain-unconditionally probes (`Install()`), all
`[AiCastSeats]`-INI-gated, all VR-refused: **GROUP A** — `CalculateScore` (0x0C) on
the 30 magic/staff `CombatInventoryItem` vtables (`EnableItemScoreProbe`, default
OFF). **GROUP B** — `CheckStartCast`/`CheckStopCast`/`GetMagicTarget` (0x06/0x07/0x0A)
on the 14 `CombatMagicCaster` vtables (`EnableCasterSeatProbe`, default OFF). **GROUP
C** (marth 2026-09-06, Opus PASS S) — `CalculateScore` (0x0C) on the four
WEAPON-class leaves (Melee/Ranged/Shield/Torch), which have **no CommonLib concrete
C++ class** (Docs/DENY-COMPLETENESS-AUDIT.md row 15's gap) but DO have vtable symbols:
since 2026-09-15 (`feat/apmf-1.5.97-pass`) resolved through the pinned 3.7.0
`VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}[0]` VariantIDs (SE
264523/264525/264527/264531, AE 210297/210299/210301/210305 — the CONFIRMED table's
"Group C" rows decoded all eight to exactly the disasm-confirmed vtables), **placed on
1.6.1170 AND 1.5.97** behind an EXACT-VERSION gate, and gated per class by an
install-time check that the LIVE function pointer already at vtable slot 0x0C equals
that runtime's confirmed `CalculateScore` entry (AE `0x8183e0/0x8188b0/0x818df0/
0x819480`; SE `0x77e0a0/0x77e550/0x77eac0/0x77f0e0` — the SE Shield/Torch entries are
0xF-byte arg-swap thunks, so the expected value is per runtime, never shared; stands in
for an RTTI-name match). Ships **ENABLED**
by default (`EnableWeaponScoreProbe`, default 1 — `CalculateScore` is scalar-return,
immune to the `GetMagicTarget` sret-ABI bug class this file's banner documents). A
second, independent flag (`EnableScoreSteer`, default 0) biases a claimed form's own
returned score upward by a fixed constant. **Where that bias lives MOVED on 2026-09-06
(F5 / DIAG RC5):** it used to sit in `WeaponScoreThunk`, whose `driven == itemForm`
test compared a WEAPON's FormID against a cast claim's driven spell form and so could
never match — dead code under any INI setting. It now rides `CalculateScoreThunk`, the
seat that scores SPELL/STAFF items (`SteerScoreForCastClaim`), hand-resolved from the
item's own `itemSlot.equipSlot`, and applies only when a live ch.8b claim occupying
that hand DRIVES that exact form (proxy if one was minted, else the spell). A deny-only
claim drives no form and can never bias anything. The dead weapon branch is deleted.
Still OFF by default.
- **What breaks:** GROUP C's `kWeaponClasses` table (VariantID + per-runtime
  CalculateScore RVA + category) is disassembly-CERTAIN for 1.6.1170 and 1.5.97 ONLY
  (CONFIRMED table 2026-09-15, "Group C" rows) — the gate is `IsRuntime1_6_1170() ||
  IsRuntime1_5_97()` (file-local helpers), deliberately NOT `IsAE()/IsSE()` (3.7.0's
  `IsSE()` is the `default:` arm, so any 1.5.x/1.7.x reads as SE and an unverified
  build would pass a family test). 1.7.104 itself never reaches the gate: CommonLib
  terminates at `SKSE::Init` with the address-library dialog on 1.7.104; APMF's gates
  never run there. **The REAL guard against a wrong-id resolve on a build that DOES
  load is the slot-0x0C expected-value compare** — in 3.7.0 a missing id is never a
  null resolve (`IDDatabase::id2offset` `report_and_fail`s past the end and otherwise
  `lower_bound`s with no equality check off VR, so it silently returns the NEXT id's
  offset); the `vt.address() == 0` test ahead of it is unreachable belt-and-braces
  (Fable tier-3 on c70767c, SEV-4), kept only so `RecoverLiveOriginal(0, 0x0C)` can
  never read near address 0. A slot-value mismatch refuses that one class loudly, never
  a blind write; a NEW build could still resolve a DIFFERENT class at a matching value
  if the check is ever loosened; keep it exact and per-runtime. Adding a third
  binary means a third expected-value column sourced from a CONFIRMED table row, the
  same predicate change in `core/CastClassify.cpp` and `plugin.cpp`'s `[runtime]` line.
  **TASK2 (Shield slot 0x0F, `EnableDualWieldPreference`) rides inside this loop and so
  opens on 1.5.97 too** — re-derived (same review) and re-confirmed from the unpacked
  1.5.97 image: SE Shield vtable `0x1681C28` has 21 slots (= AE), slot 0x0F =
  `0x77DC90`, byte-shape-identical to AE `0x817FC0`, so `ShieldEquip_t` holds on SE;
  the row is in `Docs/ADDRESS-TABLE-2026-09-15.md` "Group C" (added post-confirmation).
  The 14 caster and 30 item VariantID lists (GROUP A/B) and the `<0x68`
  `CombatController` guards are confirmed on SE by the same table (14-seat / 30-combo
  / CombatController rows) — no gate, RTTI-verified at install.
  Melee+Ranged share the engine's arbitration category 0, Shield+Torch share
  category 3 — a weapon score can only ever decide within its own category
  (melee-vs-ranged, shield-vs-torch), so **this STEER cannot push a weapon above a
  claimed spell** — do not build a weapon-vs-spell lever on this.
  **CORRECTED 2026-09-07:** this note used to read "it can NEVER outscore a
  spell/staff that already claimed the hand", stated as a property of the WORLD
  rather than of the steer. It is not one. Nothing in APMF gates weapon ADMISSION on
  a claimed hand (0x0F is not hookable on the weapon leaves —
  `Docs/DENY-COMPLETENESS-AUDIT.md` row 15 + open gap 10), so a weapon the engine's
  own scoring already ranks above the claimed spell can still take the hand; MFO's
  `DIAG-2026-09-06-deny-heal-failures.md` row P5 lists exactly that as a plausible
  cause of a claimed heal holding a hand only briefly. What the ordering table
  `[1,2,4,0,3,5,0,6]` buys is that OUR steer cannot make it worse.
  `kScoreSteerBias` is **1000.0f** (`AiCastSeats.cpp:501`), right-sized 2026-09-06 to
  measured magnitudes (Falmer War Axe 194, bow 81, magic 0.08-29) — it replaced an
  unmeasured 100000.0f placeholder, which this line quoted until 2026-09-07. And the
  bias no longer applies HERE at all: after F5 it belongs to the SPELL/STAFF seat
  (`SteerScoreForCastClaim`, `AiCastSeats.cpp:523-551`), driven by a ch.8b cast claim
  on the item's own hand. GROUP C keeps its observe-only probe and no steer.

### `native/core/Input.{h,cpp}` — test surface (OPT-IN, DEFAULT OFF)
`InputSink` (keyboard button-down) → `Arbiter::DispatchHotkey` (+ each probe's
`OnHotkey`). `LogHelp` enumerates the registry's hotkeys.
**`Register()` adds NO event sink unless `[Input] EnableTestSurface=1` in
`Data/SKSE/Plugins/APMF.ini`** (2026-09-07). That single gate is what makes the
shipped DLL obey CLAUDE.md's "probes are FULLY PASSIVE: no hotkeys, no toggles":
with no sink, no scancode can claim a channel, flip a native bit or toggle the
non-alias observe switch in a player's game. `LogHelp()` is a no-op when the surface
did not arm.
- **What breaks:** test surface only — do NOT let gameplay logic depend on it
  (the real driver is the client API). Adding a channel needs NO edit here;
  hotkeys come from the channel's own `Hotkeys()`. **Do not re-arm the sink
  unconditionally**: every channel `Hotkeys()` entry is a real ControlMap claim on
  the crosshair-aimed NPC, and the native-bit probe writes live actor flags — that is
  a user-visible behaviour change from one stray numpad press, which is exactly what
  the gate exists to prevent.

### `native/core/Allowance.{h,cpp}` — the reusable ALLOWANCE TEMPLATE (Docs/ALLOWANCE-TEMPLATE.md §3)
**FORM LOOKUP LOCK (mit-3.7 F1b, 2026-09-24): threading.** `RE::TESForm::LookupByID`/`LookupByEditorID` now hold
the game's read lock on the form map for the find (3.7.0 copied the lock and held nothing). Proof:
Docs/ADDRESS-TABLE-2026-09-15.md "ADDENDUM 2026-09-24 (mit-3.7 F1b)". A read waits only while another thread inserts
or erases a form. Writers are leaf, and a thread that already holds the lock re-enters, so no lookup can deadlock.
Audit 2026-09-24: 36 sites, 0 risks, none under an APMF mutex. They run on MAIN (Drain, Pump, OncePerFrame, the
main-only SpaceQuery calls, kDataLoaded, kPostLoadGame), the save callback, and the `PackageGate` 0x49 thunk
(engine AI threads). The 0x49 lookup happens before `g_redirectMx` is taken. **What breaks:** holding an APMF mutex
that the game could wait on across a lookup. None can today. The `PositionCast.cpp` `Enqueue` comment about the
unlocked 3.7.0 lookup is history now. Keep the rule it states: no lookup on the caller's thread.

**SEAT SELF-CHECK (mit-3.7 F1, 2026-09-24):** `SelfCheckResult()` / `LogSelfCheck()` / (open backlog: APMF-B21 APMF-B22, APMF-B23)
**`SeatVerified(address, seat)`** over the generated `native/VerifiedAddresses.h` (175 rows per
runtime, incl. the F1b `BSReadWriteLock::LockForRead`/`UnlockForRead` rows, logged only, and the
ABI v12 gate-probe rows `Travel.GetOpenState` / `Travel.ScriptEventSourceHolder.GetSingleton`; `Docs/VERIFIED-ADDRESSES.md`). `InstallOnVtables` refuses a list whose expected RTTI
TypeDescriptor is unverified and skips each unverified vtable; direct guards in Hook, PackageGate
(+ EvaluatePackage), CastClassify, EquipSink (worker + both sites), MovementDeny (3 natives), CastSeats
aim seat, AiCastSeats Group C, PositionCast::Install, SpaceQuery::Install. `plugin.cpp` logs
`[selfcheck] ... N/N verified` after the `[runtime]` line. **What breaks:** a new id-based hook or call
without a spec row (tools/verified_addresses/spec.json) + regenerated header refuses itself at startup.
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
`ControlMap::TryGetCastClaim` — whose winner is the best LIVE claim, a TTL-elapsed
claim being skipped as a candidate so it can neither answer nor mask a live
lower-basis one before the Drain sweep; the `ForHand` overload additionally takes an
`allowance::Hand{kUnknown,kLeft,kRight}` the CALLER resolved from its own
engine-native signal, and ALLOWS without narrowing when it differs from the
claim's `CastFlags::kCastFlag_LeftHand` bit — feat/deny-perhand, INVARIANTS
#18), and `CastClaimNamesForHand` (the strict POSITIVE form of the same read: a
ch.8b claim must actually STAND, on THIS hand, naming THIS exact spell-or-proxy).
Consumed today by `core/CastGate.cpp` (T2c) and `core/EquipGate.cpp`
(T2a); T1/T3/T4 reuse the same pieces when built.
`CastClaimNamesForHand` exists because of H1: a ch.8 `kIntent_SelectSpell` claim
names the ORIGINAL spell, so the moment a delivery-flip PROXY is minted,
`Allowed(fid, kIntent_SelectSpell, proxyFid)` denied the proxied cast — and because
ch.8 and ch.8b were AND-ed, ch.8b's correct spell||proxy allowance could never rescue
it. Both gates now let a matching cast claim ADMIT its own form ahead of the ch.8
narrow. That rule OUTLIVED the drive that exposed it: with the engine seats the
**AI ITSELF** charges the proxy through `CastGate`'s `CheckCast`, so admitting the
claim's proxy there is what lets the NPC's own cast get off the ground at all.
`core/CastSeats.cpp` and `core/EquipGate.cpp`'s seat 0x0F read the richer
`ControlMap::TryGetCastSeatClaim` instead (spell + proxy + target + resolved
`ActorHandle` + flags + TTL in one RCU read) — same discipline, more fields, and the
same live-candidate rule: `TryGetCastSeatClaim`/`…ForHand` skip a TTL-elapsed claim as
a CANDIDATE rather than testing only the winner, so a lapsed claim can neither seat nor
mask a live lower-basis one before the Drain sweep.
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
  CommonLibSSE-NG headers (CORRECTED 2026-09-15: the VTABLE symbols DO exist —
  `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}`, now used by
  `core/AiCastSeats.cpp` GROUP C; what is missing is the C++ class, so 0x0F's
  per-class semantics on those leaves remain unverified and this gate still does
  not hook them), and
  potion/scroll/shout are deliberately excluded on the SAME v1.0.32 gameplay
  lesson MFO's own gate already proved (denying them would only ever be a
  false-positive block on combat drinking/shouting). Widen only on new header
  evidence, never by guessing a symbol name.
  PER-HAND (feat/deny-perhand): `a_this` (`CombatInventoryItem`) carries its
  OWN `itemSlot.equipSlot` (`static_assert`'d @0x20); compared against
  `BGSDefaultObjectManager`'s Left/Right Hand default objects to resolve the
  hand, fed into `Allowance::AllowedCastForHand` for the ch.8b narrowing —
  same per-hand guarantee as CastGate above. RUNTIMES (CONFIRMED table
  2026-09-15): the 30 item VariantIDs (28 + the two `_CombatMagicCasterArmor_`
  rows) decode to RTTI-verified vtables on 1.6.1170 and 1.5.97 with slot 0x0F a
  per-instantiation function on both; the direct `objects[19]/[20]` read is
  layout-safe on SE (364 entries) and AE (366); the `CallSiteName` 0x0F call-site
  label table (`0x80fcd0` pre-loop, `0x813af2/0x813d38/0x814270/0x8144b2`
  selector) is 1.6.1170-ONLY and is consulted only there — on any other binary the
  live RVA still prints, labelled "unlabelled", never matched against AE literals.
  `kIntent_SelectSpell`/
  `kIntent_Equipment` (`Allowed`, not `AllowedCastForHand`) stay actor-wide —
  unchanged, per-hand was scoped to `kIntent_Cast` only.
  **ch.8b SEAT 0x0F (feat/ai-cast-seats-impl) — the FIFTH engine cast seat and the
  ONE NON-CHAINING ANSWER IN THE TREE.** This thunk now resolves actor/subject/hand
  BEFORE calling the original, so that while a `kIntent_Cast` claim stands it can
  return TRUE for the claim's DRIVEN form on a RESTORE item vtable without chaining.
  It must: the five Restore ITEM templates OVERRIDE `CheckShouldEquip` with the
  static `0x81f7c0`, which reads its target STRAIGHT off the `CombatController`
  (`kSelf ? attacker : combat TARGET`) and runs `ShouldRestore` on it — so for a
  healthy follower fighting a healthy foe the original always says NO, the heal never
  enters the equipment set, no magic context is built, and seats 0x06/0x0A/0x07/0x0D
  are NEVER CALLED. There is no interposable seat between it and those fields, so
  chaining is not a weaker answer, it is no answer. INVARIANTS #20 states the
  exception and its three conditions. **Everything else in this thunk still runs
  strictly engine-answer-first.** Scope, all required together: `g_restoreVtables`
  (recorded at install from the two NAMED Restore symbols, never inferred), the
  claim's DRIVEN form (proxy-when-one-exists — the ORIGINAL kSelf spell must never be
  forced, it would heal the caster), and the claim's own hand when resolvable. The
  same block also DENIES the original kSelf spell's item while its proxy is being
  driven (N4 exactness — `AllowedCastForHand` permits spell||proxy and would
  otherwise let the AI take the hand with the self-healing form).
  **0x0F COMPLETENESS FIX + SEAT 0 REBUILD TRIGGER (feat/seat0-classify, 2026-09-05,
  RE notebook J9 / INVARIANTS #18):** below the driven-form match, any Restore item
  whose spell delivery is NOT `kSelf` and is NOT the live claim's driven form is now
  denied OUTRIGHT (`item->GetDelivery() != kSelf → false`) — vanilla data never
  produces one (no table row exists for it, `core/CastClassify.cpp`'s own finding),
  so this changes nothing today; it closes the path a stale/foreign heal-OTHER item
  could reach the hands after Release, on the wrong hand, or from a future source
  (`0x81f7c0` aims a non-self Restore item at `ctrl.TARGET`, i.e. the FOE). Also: this
  thunk now sets `a_cc->inventory->dirty = true` (one lock-free RCU read of the cast
  claim, reused by both this write and the block above) whenever the deliberating
  actor holds a live cast-seat claim — the rebuild trigger `core/CastClassify.cpp`
  needs to reclassify the claim's spell; see that file's entry above for the scope
  note. Per-call trace log added (`[EquipGate] EnableEquipGateLog`, default OFF) —
  the 0x0F force/deny decisions above always log at the seat's own throttle
  regardless of that flag.

### `native/core/ActionGate.{h,cpp}` — T1: the combat behavior-tree allowance (ch.7 ONLY)
`Install()` (kDataLoaded, VR-refused, install-once) hooks the 70
`VTABLE_CombatBehaviorTreeNodeObject_*` leaves (`apmf::cbt::kLeaves`) at vtable slot
**0x02 (act)** and slot **0x03 (pop)**, RTTI-verified through
`allowance::InstallOnVtables`. `ActThunk` resolves the deliberating actor from
`control+0x158` (both hypotheses, probe-proven), reads ONE lock-free RCU claim
(`kIntent_CombatAction`'s `ival` category mask) and, when the node's install-time
category is named, DENIES it as `CombatBehaviorForceFail`'s own act()+pop() **PAIR**:
it records `{node, control}` in a `thread_local` pending-pop and invokes ForceFail's
original `act()`; `PopThunk` then routes that node's very next `pop()` (same thread,
same step — the runner's protocol, `core/CombatBehaviorRE.h` "The node protocol") to
ForceFail's original `pop()`. Categories: offense leaves (12 names) and the four cast
leaves (Cast|Offense).
**RETIRED 2026-09-05 (feat/ai-cast-seats-impl), do not re-add without re-reading why:**
the `CombatBehaviorContextMagic` CreateContextNode deny (`kCastContextNodes`) is GONE —
hooks and classification both — and so are the two IMPLICIT deny sources
(`kIntent_Cast` ⇒ Cast, and `kIntent_SelectSpell`+`kActFlag_Drive` ⇒ Cast). A cast
claim is now DELIVERED BY the AI's own magic branch (`core/CastSeats.cpp`); denying that
branch would silence the very cast being claimed. The context node was also the seat
whose act()-only deny caused the months-live data-stack CTD. `kIntent_Cast` is invisible
to this gate today; only an explicit `kIntent_CombatAction` claim arms anything.
- **What breaks:** the PAIR is the whole fix for the 2026-09-04 recurring deck CTD
  (INVARIANTS #18 "act()/pop() pair"): an act()-only ForceFail leaves the node's OWN
  `pop()` to pop `sizeof(node state)` for a 4-byte push — balanced by accident for the
  4-byte leaves, a data-stack corruption for CastImmediate/Concentration/RangedAttack
  (0xC), GroundAttack (0x18), FlyingAttack (0x30) and catastrophic for the ContextMagic
  node — which is why that one is now never hooked at all). Never classify a vtable that
  is not `Paired()` (both maps); never arm the deny when ForceFail's `act()` OR `pop()`
  failed to resolve (Install refuses both together). `t_pending` is `thread_local` on
  purpose — the runner calls `pop()` synchronously on the same OS thread with nothing in
  between; a global would race across combat threads. Thunks run on COMBAT threads (§5):
  no lock, no follower list, only the stored originals + ControlMap RCU reads.
  **Never make `kIntent_Cast` (or any future cast claim) contribute a Cast deny here** —
  it would suppress the AI's own cast branch, which is now the cast facet's delivery
  mechanism. COVERAGE TRADED by dropping the context node, stated plainly: an explicit
  `kIntent_CombatAction(Cast|Offense)` claim now suppresses only the four FIRING leaves,
  not the CONTEXT-BUILD path, so the AI may build a magic context and equip a spell it
  then cannot fire (Docs/DENY-COMPLETENESS-AUDIT.md, open gap).

### `native/core/CombatBehaviorRE.h` — the local RE:: extension for the combat tree (measured)
The `CombatBehaviorTreeNode` layout (10 vfuncs: act 0x02 / pop 0x03 / update 0x04 /
on_interrupted 0x05), `TreeControl` (+0x158 master), `ControllerMini` (attackerHandle
0x28, `static_assert` < 0x68), the 70 leaf VariantID triples, `kCastContextNodes`, and
"The node protocol" — the disassembly-measured CombatBehaviorThread layout + the
push/pop sizes per node kind (AE IDs cited as evidence only; no hook targets an ID —
every hook here is a vtable slot).
- **What breaks:** the VariantID triples are copied verbatim from CPR's pinned
  Offsets_VTABLE.h — never retype; the protocol block is what justifies the paired deny —
  re-measure before changing `ActionGate.cpp`'s pairing, don't reason from CPR's class
  declaration alone (that is exactly how the pop half was missed the first time).

### `native/core/PackageGate.{h,cpp}` — T3: the 0x49 package-offer gate (ch.9)
**(This entry replaces MAP's old `core/AliasPkgProbe.{h,cpp}` section, corrected
2026-09-07: that file NO LONGER EXISTS — the probe graduated into this gate on
2026-09-03 and MAP kept documenting the deleted throwaway for four days.)**
`Install()` ← `plugin.cpp` kDataLoaded: `write_vfunc` **0x49**
`CheckForCurrentAliasPackage` on `VTABLE_Character[0]` ONLY (never PlayerCharacter —
§0.38), VR-refused. Thunk `TESPackage*(Actor*)`: if the actor holds the winning
`kIntent_OfferPackage` claim (lock-free RCU `TryGetOwningClaim`), return the package
FormID that claim NAMES (`APMF_Param::form`); a FormID that does not resolve falls back
to `original(self)` — NEVER a fabricated null (§0.25 "claimed with nothing = rooted").
`EvaluatePackage(actor)` is the engine nudge, POSTED by `channels/OfferPackage.cpp`'s
Engage/OnOwnerChanged/Release through `apmf::mainthread::Post` so it runs one hop PAST
`ControlMap::Publish()` — calling it inline asks the engine the 0x49 question while the
claim is still only in the writer's private copy, which is exactly the 0-of-6 bug. A
`[ch.9-redirect]` log line per transition, gated by `[PackageGate] EnableRedirectLog`
(default 1), deduped per actor on the answer tuple; `ForgetRedirect` (release edge —
POSTED through `mainthread::Post`, queued ahead of the release nudge, because an inline
erase sits in the pre-`Publish()` window where a combat-thread 0x49 consult re-inserts
the identical tuple) and `ForgetAllRedirects` (revert/new game) drop that memory so a
re-dispatch still prints.
- **What breaks:** the redirect only takes effect **when the engine ASKS**, and the field
  evidence is that it does not ask on a useful cadence of its own — every win so far
  reconciles to an explicit `EvaluatePackage` nudge (MFO `DIAG-2026-09-06-loot-travel.md`;
  `Docs/SPEC-PACKAGE-HOLD.md` §2.2). So the nudge's ORDERING relative to the claim's
  `Publish()` is load-bearing, not incidental — inline it again and the channel silently
  goes back to answering with the pre-claim package. Never touch alias/run-once state
  (INVARIANTS #3a); never return null.

### `native/core/PackageData.{h,cpp}` — read a templated package's inputs, point its Location (ch.19 travel)
`SetTravelTarget(pkg, ref, radius)` and `SetTravelCell(pkg, cell, radius)`: resolve the vanilla
Travel template's `"Place to Travel"` input BY NAME, guard the recovered pointer by the
input's own `GetTypeName()`, then write the locType, the payload and `rad`.
- **TWO ENTRY POINTS BECAUSE THE TWO KINDS READ DIFFERENT UNION MEMBERS**, and that is read
  off the engine, not assumed: `PackageLocation::AllocateLocation` (vtable slot 1) switches on
  locType through a 13-entry jump table, and case 0 does `mov eax, DWORD [rsi+0x10]` (a 4-byte
  HANDLE) while case 1 does `mov rcx, QWORD [rsi+0x10]` (an 8-byte POINTER, null-checked, used
  as `this`). Writing one where the other is read hands the engine a truncated or garbage
  pointer. Decoded on both unpacked images, SE `0x441BE0`/table `0x442194`, AE
  `0x49C9E0`/table `0x49CF94`. **A PORT of MFO's field-run accessor** (`marth-follower-overhaul/native/Packages.cpp`
`FindInput`/`ReadLocation`/`SetAPMFLootTravelTarget`, plus the RTTI layout derivation at the top of
that file), carried over with its guards AND its reasoning rather than re-derived.
- **The recovered constant:** `pointer = IPackageData* + 0x10`, correct for BOTH the single- and
  multiple-inheritance package-data shapes because the stored pointer is already base-adjusted.
  The full derivation is reproduced in the .cpp — do not summarize it away; it is the whole
  memory-safety argument. Five `static_assert`s pin the CommonLib layouts it rests on, so a
  CommonLib bump that moved one fails the BUILD instead of the game.
- **`"Place to Travel"` is the TEMPLATE's BNAM parameter name and is FIELD-PROVEN, not guessed**
  (deck 2026-09-03: MFO shipped `"Location"` — the ANAM TYPE — for three versions, missed both
  name maps every time, and its runtime write never ran at all). Do not "tidy" it.
- **What breaks:** GAME THREAD ONLY (it walks engine-owned package data and mutates a form).
  A guard failure must keep DECLINING LOUDLY and writing NOTHING — a half-mutated package sends
  an actor to the wrong place, and reading `+0x10` off a `BGSPackageDataBool` (0x10 bytes TOTAL)
  is a read past the end of the object. No Address-Library id, no vtable index, nothing
  per-runtime: it is pure struct arithmetic over layouts the pin asserts identically on
  1.6.1170 and 1.5.97.

### `native/core/EquipSink.{h,cpp}` — THE #17a CALL-SITE SEAT: the engine-equip sink (ch.17)
**The one non-vtable seat in the tree, licensed by INVARIANTS #17a and NOTHING else.**
Every engine equip funnels through the `ActorEquipManager` worker (AE 38929 → RVA
0x6CBE30 / SE 37974 → 0x639E20), reached from exactly TWO internal E8 sites — `EquipObject`
38894+0x170 / 37938+0xE5 and the list sibling 38893+0xBC / 37937+0xBC (full .text E8 scan
plus E9/lea/pointer scans on both unpacked binaries, 2026-09-15: nothing else reaches it;
`Actor::AddWornItem` is devirtualised, so no vtable seat exists for this facet). `Install()`
← `plugin.cpp` kDataLoaded: VR-refused, install-once, INI `[EquipAuthority] bEquipAuthority`
(default 1); resolves the worker via `REL::ID`, then BYTE-VERIFIES all sites BEFORE writing
any (`E8 rel32` whose target == the library-resolved worker on the running binary); ANY
mismatch refuses the WHOLE seat with `[apmf][equip-sink] site-verify FAILED …` (a SCAR-class
collision is a refusal, never a CTD); on success logs `site <id>+<off> verified E8-><rva>`
per site, builds a sorted `REL::IDDatabase::Offset2ID` copy of the Address Library (the
caller classifier), `SKSE::AllocTrampoline(64)` and `write_call<5>` both sites onto ONE thunk.
**Thunk** (worker signature `(ActorEquipManager*, Actor*, TESBoundObject*, EquipData*)`;
`EquipData{extra@0,count@8,slot@0x10,0@0x18,queue@0x20,force@0x21,sounds@0x22,applyNow@0x23}`,
`static_assert`-pinned): player / null / UNCLAIMED actor (`ControlMap::TryGetEquipSet` false)
→ call the worker, unlogged (#17a-5, #13). Claimed actor → classify the caller by reading
`EquipObject`'s CALLER's return slot at the per-site, per-runtime depth measured from the
bytes (AE 38894 = 5 pushes + `sub rsp,0x50` → `[rsp+0x80]`; AE 38893 = 3 pushes → `[rsp+0x70]`;
SE 37938 = `push rdi` → `[rsp+0x60]`; SE 37937 = 3 pushes → `[rsp+0x70]`; the site is
identified from `_ReturnAddress()-5`), map the RVA to the containing function's library id
(greatest offset ≤ RVA via `upper_bound-1` over the public iterators — NEVER `Offset2ID::
operator()`, which `report_and_fail`s on a miss), name it from the per-runtime path table
(OutfitApply / AddWornOutfit / AiCommand / RemoveItemReequip / CombatNode / Script / Console /
PlayerMenu / WorkerReentry / QueuedApply — the last is the deferred apply of a queued equip,
38906,39814 / 37950,38789, which is how APMF's OWN queued equips come back one actor-update
later with tls=0 — plus, from the 2026-09-16 Tier-C sweep, AiCommand for the DISPATCHER
39637/38606 itself (+0xF47/+0xCCD → EquipObject, the first v8 deck log's `Unknown(39637)`) and
its family 39948,39655,39645,37510 / 38902,38624,38614,36510, DropObject 37520/36520 (Actor
vtable 0xCB), PickUpObject 37521/36521 (0xCC), BoundItem 38898/37942 (← BoundItemEffect slot 4),
ProcedureEat 28422/27700 (← BGSProcedureEat slot 0xC), InventoryReequip 41244/40241, StartCombat
38561/37608; 16104/15864 and the orphan 52410/51535 stay Unknown on purpose — AUDIT row 17 has the
evidence per id), `External(<module.dll>)` for a return address outside the exe (skse64*
counts as Script), `Unknown(<id>)` otherwise. **Governed types (ABI v8), checked right after `TryGetEquipSet`:** only
ARMO / WEAP / AMMO / LIGH are governed (`IsGovernedType`); any other `GetFormType()` on a
claimed actor calls the worker at once, logged ONCE per (actor, formType) at debug
(`FirstUngovernedSight`, a bounded set under `g_logMx`), never per event — `DrinkPotion`
and the AI's own potion/food/scroll use are never refused. **Categories (ABI v9):** `Categorize(obj, data->slot)` — THE ONE category map, a public
function in this TU shared with `channels/EquipAuthority.cpp` (which calls it with the DECLARED
hand's EQUP) — maps the item to `APMF_API::EquipCategory` bits from member reads only (ARMO
`GetSlotMask`: `kShield` → Shield+Left, any OTHER bit (or none) → Armor, BOTH kinds of bits →
Shield+Left+Armor — a modded shield-on-back piece must not slip under an Armor-only scope; WEAP `IsTwoHandedSword/IsTwoHandedAxe/IsBow/
IsCrossbow` → Right+Left; a one-hander (incl. staff) by the slot's FormID 0x13F42 → Right /
0x13F43 → Left / anything else incl. null → Right+Left CONSERVATIVE; AMMO → Ammo; LIGH →
Light+Left); `competes & set.denied` = `black`, `competes & set.owned` = `owned`.
**Verdict, in order (v9 steps 0-6):** Script/
Console without `kEquipAuth_DenyScript`/INI `bEquipDenyScript` → allow; `PlayerMenu`
(`PathKind::kPlayerMenu`, the three per-runtime ids) without the claim's
`kEquipAuth_DenyPlayerMenu` → `allow (player agency)` (v8: player agency, no INI twin) — both
exemptions sit ABOVE the masks; `black` → refuse EVEN IF IN-SET; `owned` → no declaration
(`count==0`) or item in the declared set (FormID compare — the v8 hand is NOT consulted here) →
allow, else refuse; neither owned nor denied → allow (`owned=0`, the engine's category).
A refusal is `would-deny` + call under observe-only
(INI `bEquipObserveOnly`, default 1, or the claim's `kEquipAuth_ObserveOnly`); else `deny` = RETURN WITHOUT CALLING the worker (nothing queued, no
re-entry; the caller's own spin-lock/epilogue run as if the worker returned — both callers
discard its return value, verified). The default scope `{All, 0}` makes the v9 table collapse
to v8's. `tls` (the `ApmfEquipScope` depth) is a LOG FIELD, never
a bypass: APMF's own equips pass by being in-set. Install also inspects the PUBLIC entry of
both site functions for a third-party inline detour (E9 / FF 25 / 48 B8 / E8) and, if found,
logs `entry <id> detoured by <dll>: attribution degraded` — the seat still installs, but every
caller then classifies `External(<that dll>)` and the Script exemption is unavailable.
Runtime gate is EXACT-VERSION (`1.6.1170 || 1.5.97`, the same predicate as CastClassify /
plugin.cpp's `[runtime]` line), never `IsAE()`; other builds log `runtime <v> gated`. The
`[runtime] … equip-sink open|gated` field is that VERSION PREDICATE ONLY — the
`[apmf][equip-sink] INSTALLED … entries=clean|DETOURED` line is the truth about whether the
seat is live (INI off, a site-verify refusal, or VR all leave `open` on the runtime line). The
entry-detour inspection is re-run at kPostLoadGame / kNewGame (`ReinspectEntries`) and logs
only on a change of verdict, because a plugin later in load order may detour after our
kDataLoaded.
One `[apmf][equip-obs] actor= item= op=equip path=
site=<id>+<off> ret=<rva> q= f= s= a= tls= cat=<+-joined> owned=<0|1> black=<0|1>
eslot=<none|R|L|E|hex> verdict=` line per decision (`E` = EitherHand 0x13F44, a LOG LABEL only), deduped 2 s per
(actor,item,path), capped 100/s with a per-minute dropped-count line, wrapped in
`try/catch` so logging can never unwind into the engine frame.
- **What breaks:** (1) **#17a's five conditions are the ONLY licence** — a third site, a
  public entry, a "warn and install anyway" on a byte mismatch, an equip the client did not
  declare, or a touch on an unclaimed actor each voids it; a NEW call-site seat anywhere
  needs its own explicit #17a argument, not an appeal to this one. (2) **The depths are
  per-site AND per-runtime**: a new runtime (1.7.x) needs its ids, offsets AND frame shapes
  re-measured from its bytes (rule 11); never carry AE's `0x80` to a binary you have not
  disassembled. (3) **The TLS bracket** is what lets `channels/EquipAuthority.cpp`'s own
  equips through AND what proves them in the log (`tls=1`); enforce without it and the
  probe criterion "zero would-deny with tls>0" cannot be read. (4) The seat reads ONLY the
  RCU snapshot (FormID compares; never `LookupByID`, never client state) — it runs on
  whatever thread equips; a form lookup there takes the engine's forms-map lock on the
  combat thread. (5) `Offset2ID` is built BEFORE the patch (the thunk must never see a
  half-built table) and is immutable after. (6) UNEQUIPS are not seated (the twin
  `UnequipObject` 38901/37945 → worker 38934/37979, single caller 38901+0x1B9 / 37945+0x138)
  — `kEquipAuth_DenyUnequip` is RESERVED and refused; seating it is its own #17a argument.
  (7) ONE path runs BELOW the seat and is not a decision: the worker's apply function 38001
  (worker +0x289) is also reached from 16073+0xf0 (InventoryChanges family ← 40746 ← 40655),
  the persisted-ExtraWorn RESTORE when an actor's 3D loads; it re-applies a state the seat
  governed when it was chosen, never chooses. 38919 is an orphan copy of the worker's tail
  (no references). Do not write "every engine equip" — write "every engine equip DECISION".
  (8) Never re-add a `tls>0` short-circuit to the verdict: 38913 re-equips the SAME object it
  was handed, so a second copy of a displaced off-set item could ride back in on it, and
  probe criterion 2 would be vacuous. (8b, v9) **The category map is ONE function** —
  `Categorize` — and both halves MUST keep calling it: a second map in the channel, or a
  seat-side special case, lets the seat refuse what the pass equips (or the reverse) for a
  shield or a torch. **The either-hand rule is conservative ON PURPOSE** (a one-hander with
  a null/EitherHand slot competes for BOTH hands): narrowing it to one hand lets an engine
  equip that the engine later lands in the OTHER hand slip past a claim that owns only that
  other hand. **`owned`/`denied` ride the same snapshot read** as the set (`TryGetEquipSet`),
  never a second read. The script/player-menu exemptions stay ABOVE both masks. (9) The `channels/EquipAuthority.cpp` pass holds an
  item it already queued for 3 s from its issue before queuing it again — that is what keeps a
  per-tick client from piling up the engine's equip queue; keep the hold if you touch `Enforce`. **CORRECTED (Fable round 2 on 123d50e):** the
  1 s coalesce is GONE — there is no signature/time guard at all; the inventory walk (the
  real dedupe) always runs, so a "give it back" re-declaration right after a script unequip
  is honoured, and the pass line marks an identical re-declaration. A held item
  keeps its ORIGINAL issue time across passes (the 3 s window is real). (11) **The governed-type set is
  mirrored in TWO places** — `core/EquipSink.cpp IsGovernedType` (the deny) and
  `channels/EquipAuthority.cpp IsGovernedType` (the equip pass skips a declared non-governed
  form; equipping a potion drinks it) — change both or a declared potion is drunk per pass
  while the seat lets every potion through. (12) **The hand (v8) lives ONLY in the equip
  pass**: `EquipSetView::slots[]` → `Enforce` resolves `BGSEquipSlot` 0x13F42/0x13F43 by
  `LookupByID` (NEVER `BGSDefaultObjectManager::GetObject` — CommonLib 3.7.0's
  `IsObjectInitialized` reads +0xB80 as a `bool*`, always-null on 1.6.1170 and a fault on
  1.5.97), tests "worn" per hand via `Actor::GetEquippedObject(left)`, keys the 3 s hold by
  (form, slot), and counts same-form instances per hand against the inventory count. Never
  let a null slot lookup degrade to a slot-less equip (it is skipped + logged). (14) **Every path-table row is
  a PROVEN chain, not a guess** — the 2026-09-16 rows were named from an E8 sweep of both unpacked
  images (address-library bounds, RTTI vtable slots, caller chains; tooling was scratch, the
  evidence is in AUDIT row 17). Adding a row from a design table alone is how `Unknown(39637)`
  cost a field cycle: the table named the dispatcher's executors and missed the dispatcher's own
  direct call. Name a new id only after reading its E8 in the image. (13)
  `NotInstalledReason()` must be a string LITERAL set at every refusal point in `Install()`
  and cleared to "" on success — `ControlMap::EnqueueRequest` reads it from any thread to
  refuse ch.17 claims; a new refusal point that forgets to set it leaves the stale
  "not yet installed" text in the log. `Enforcing()` (= `IsEquipAuthorityEnforced`) is
  installed AND NOT INI-observe-only; a claim's own ObserveOnly bit is deliberately not
  folded in. (10) **Open review
  findings live in `Docs/REVIEW-BACKLOG.md` APMF-B2..B4 and, for the v8 round on a369e9f,
  APMF-B5..B10** (B5 Engage log names only v7; B6 the hand-EQUP resolve error over-claims;
  B7 the once-logged claim-refusal reason can be stale; B8 Enforce's seat-missing branch is
  unreachable under the v8 refusal; B9 queued-slot carriage unobserved — INTEGRATION criterion
  7; B10 `MinReleaseForAbi`'s v7/v8 placeholder must be named at the cut) (ch.15's probe re-equip is
  `External(APMF.dll)` on a ch.17 actor; a detour target inside a trampoline page prints
  `detoured by ?`; the truncation log fires at Apply) — read them before editing this seat.

### `native/core/NonAliasProbe.{h,cpp}` — OBSERVE-ONLY 0xDF hook + 0x49 assist + RTTI dumper
Docs/PROBE-NONALIAS-PACKAGE.md's runtime probe: does `Actor::CheckForCurrentAliasPackage`
(0x49, `core/PackageGate.cpp`'s existing hook) fire for a NON-alias package actor (Cicero,
`0009BE51`) at all, and does `Actor::PutCreatedPackage` (0xDF, RTTI-verified via
`core/Allowance.h::InstallOnVtables` on `VTABLE_Character[0]`) ever carry that same package?
Pure logging, chained to the original unconditionally, no decision/denial (INVARIANTS #17).
**`[Probe.NonAlias] EnableObserveLog`** (INI, default 0/OFF, read once at `Install()`)
is the switch for the 0x49/0xDF observe log; `core/PackageGate.cpp`'s thunk checks
`IsEnabled()`/`RateLimitOK()` from here to add its own line on the same switch. The
`0x45` NumLock toggle and the `0x46` ScrollLock one-shot vtable/RTTI dump
(module-relative RVAs + best-effort RTTI type name, mirroring `Allowance.cpp`'s
`DerivesFrom` walk, of the crosshair-aimed actor — the reusable "find sites ourselves"
tool) still exist but are **unreachable unless the keyboard test surface is armed**
(`[Input] EnableTestSurface=1`, default OFF — see `core/Input.{h,cpp}`).
- **What breaks:** THROWAWAY/instrumentation-only, same class as `native/core/
  NativeBitProbe.{h,cpp}` (which is itself gated by `[Probe.NativeBit] Enable`, default
  0, because it MUTATES live actor flags) — not wired to any client. Never re-arm either
  probe by default: CLAUDE.md's rule is passive, config-gated, OFF. Never touch
  alias/run-once state (#3a).
  0xDF is a 1.6.1170-pinned raw index (Docs/PROBE-NONALIAS-PACKAGE.md §2) — no named
  CommonLib binding exists to prefer over it today. CONFIRMED on 1.5.97 (table
  2026-09-15, `VTABLE_Character` row): 298-slot `Character` vtable on 1.6.1170, 1.5.97
  and 1.7.104, slot 0xDF the same 12-callee function id-for-id on SE — no runtime gate.

### `native/channels/*.cpp` — one module per facet (FULL documented catalog)
Each: a `Channel` subclass + `APMF_REGISTER_CHANNEL`, per-NPC `Engage`/`Release`.
The first release shipped the documented catalog of 13 channels as a baseline
benchmark; the table below is the CURRENT set — 20 files, 20 rows (ch.21 `CombatEntry` and ch.20 `TargetPin` added 2026-09-25; ch.7
`CombatAction` and ch.9 `OfferPackage` were missing from it until 2026-09-07; ch.17
`EquipAuthority` added 2026-09-15; ch.19 `Travel` added 2026-09-22 — ch.18 is RESERVED
for an attack-selection design that is NOT YET ON MAIN, so the intent numbers skip it). Each `ServesIntent()` maps to an `APMF_API::Intent`. Test keys in
parentheses.

| File | Ch | Facet | Gate / mechanism | Kind |
|------|----|-------|------|-----------|
| `MovementDeny.cpp` | 1 | movement FULL block (Num1) | `KeepOffsetFromActor(self)` + `SetDontMove(true)` (both Address-Library bound) | source-block |
| `Speed.cpp` | 1a | gait/speed (Num7) | `kSpeedMult` AV (arbitrary factor, default x0.5) | source-block |
| `Stance.cpp` | 3 | sneak/crouch (Num9) | `NotifyAnimationGraph("SneakStart/Stop")` | one-shot promote |
| `WeaponDraw.cpp` | 4 | draw/sheathe (Num5) | `DrawWeaponMagicHands(bool)` | one-shot (sticky) |
| `Headtrack.cpp` | 5 | look-at (Num3) | `AIProcess::SetHeadtrackTarget` (own point slot) | **known-incomplete block (Tick re-assert; loses to a package-locked follower)** |
| `CombatTarget.cpp` | 6 | combat-target CLAIM (Num-) | **ARBITRATION-ONLY** — records the owner; makes NO engine combat call (no `StartCombat`, no `currentCombatTarget` write). The CLIENT commands the target. Release relinquishes | arbitration-only (#0); client executes |
| `CastingSelect.cpp` | 8 | casting CLAIM (Num4) | Log-only + a REAL allowance one layer down: `core/CastGate.cpp` (T2c CheckCast) + `core/EquipGate.cpp` (T2a CheckShouldEquip) deny any spell/item that isn't the claimed `param.form`. Makes NO engine write. The `+ACT` drive opt-in it briefly carried is RETIRED (`ival`/`target`/`pos` accepted-and-ignored, bits stay RESERVED in the byte-frozen ABI) — for a cast APMF should MAKE happen, use `kIntent_Cast` (ch.8b) | claim + T2 enforcement; client's own AI executes |
| `CastCompose.cpp` | 8b | cast EXECUTION claim (`RequestCast`, ABI v5) | Log-only, PLUS one lifecycle duty: `Release` frees the delivery-flip proxy one `mainthread::Post` hop AFTER the cleared claim publishes (INVARIANTS #20). The claim's real effect is the FIVE ENGINE SEATS (`core/CastSeats.cpp` 0x06/0x07/0x0A/0x0D + `core/EquipGate.cpp` 0x0F) — the NPC's own AI performs the cast | claim + seat answers; the ENGINE executes |
| `Dialogue.cpp` | 10 | dialogue (Num6) | `PauseCurrentDialogue()` | one-shot |
| `Attribute.cpp` | 11 | disposition (Num2) | 4 AVs: aggression/confidence/assistance/morality | source-block |
| `Idle.cpp` | 12 | idle/anim (Num+) | `NotifyAnimationGraph("IdleForceDefaultState")` | one-shot |
| `ShoutPower.cpp` | 14 | shout/power select (Num*) | **ARBITRATION-ONLY** — records the voice-slot owner and the chosen shout in `param.form`; makes NO engine write. It previously called `ActorEquipManager::EquipShout` directly (the #0 anti-pattern ch.6/ch.8 were fixed for); the CLIENT issues its own `EquipShout`. `Release` has nothing to undo (corrected 2026-09-07) | arbitration-only (#0); client executes |
| `CombatAction.cpp` | 7 | combat-action category deny (NumpadEnter) | Arbitration + claim lifecycle only; the deny is `core/ActionGate.cpp`'s T1 paired act()/pop() on the 70 combat behavior-tree leaves, for the categories named in `param.ival` (`kCombatActionCat_Offense` today). The test key carries no category, so a test claim denies nothing | claim + T1 enforcement |
| `OfferPackage.cpp` | 9 | package-procedure activity (NumpadSlash) | Arbitration + claim lifecycle + the `EvaluatePackage(true,false)` nudge, `mainthread::Post`ed so it lands PAST the claim's publish; the redirect itself is `core/PackageGate.cpp`'s T3 0x49 hook returning the claim's `param.form`. The test key carries no package, so a test claim offers nothing | claim + T3 enforcement; the ENGINE runs the package natively |
| `Equipment.cpp` | 15 | equip/unequip (Num.) | `GetEquippedObject` + `UnequipObject`/`EquipObject` (melee-vs-ranged lever) | source-block |
| `Detection.cpp` | 16 | stealth (Num8) | `kMovementNoiseMult` + `kDetectLifeRange` AVs | source-block |
| `EquipAuthority.cpp` | 17 | ENGINE-EQUIP facet, WHOLE by default, SCOPED by category from ABI v9 (`kIntent_EquipAuthority`, ABI v7/v8/v9; no test key) | Arbitration + claim lifecycle (standing, no TTL) + the ONE #17a-licensed equip: on every applied `SetEquipSet`/`SetEquipSetEx` for the owning claim (and on a win/repoint, and on a CHANGED `SetEquipScope`) it POSTS one `mainthread` hop that lands after `Publish()` and equips each declared item the actor is not wearing via `ActorEquipManager::EquipObject` (queued, not forced; v8: with the declared hand's `BGSEquipSlot` 0x13F42/0x13F43 by `LookupByID`, "worn" = in THAT hand, same form allowed once per hand) inside `equipsink::ApmfEquipScope`; skips a declared non-governed form type (ARMO/WEAP/AMMO/LIGH only); v9: skips (counts `skipped-unowned`/`skipped-denied`, names once per actor+set signature) any entry whose `equipsink::Categorize(obj, declaredHandEQUP)` bits are not ALL owned or ANY denied — never equips or evicts into an unowned category; never unequips; no re-assert (every declaration walks the inventory; an item APMF already queued is held 3 s from its issue before it is queued again, per (form, hand)). The deny is `core/EquipSink.cpp`'s call-site seat. `Release` relinquishes (nothing to undo). Refuses `kEquipAuth_DenyUnequip` (reserved). Open findings: `Docs/REVIEW-BACKLOG.md` APMF-B5, B6, B8, B9 | claim + #17a seat; APMF equips the DECLARED set, the ENGINE keeps its hands off |
| `Travel.cpp` | 19 | WALK this actor to a destination — an object REFERENCE (arrival = distance <= radius) or a CELL (arrival = parent-cell identity; the radius is not consulted) — (`kIntent_Travel`, ABI v10; no test key — the crosshair surface can name only ONE ref and ch.19 needs an actor AND a destination, so a hotkey would mean APMF inventing intent) | **ADDS NO SEAT; CLAIMS NO OTHER INTENT.** One `mainthread::Post` hop past `Drain`'s `Publish`, it files ONE internal ch.9 `kIntent_OfferPackage` claim naming an APMF-OWNED Travel package from `Data/APMF.esl`, whose `Place to Travel` Location input `core/PackageData.cpp` points at the destination. TWO kinds, chosen by the FormID's own record type: an object REFERENCE (`kNearReference`, a 4-byte handle -- `SetTravelTarget`) or a CELL (`kInCell`, an 8-byte form POINTER -- `SetTravelCell`). The two write DIFFERENT members of the same 8-byte union, which is why they are separate functions with separate types; both member choices were read off the engine's own locType switch on both unpacked images. Every other locType is REFUSED -- see `Docs/DENY-COMPLETENESS-AUDIT.md` row 19 (h). A world POSITION is REFUSED unless the claim sets `kTravel_ToPosition` (ABI v11), in which case APMF places its own XMarker and runs a REFERENCE leg to it (see What breaks (7)). That offer is travel's IMPLEMENTATION, not a composed client intent; it is filed at the CLIENT's basis (read back through `ControlMap::TryGetOwningClaimBasis`) and RE-FILED — request-new-then-release-old, in one `Drain` — when the winner's basis moves, because a claim's basis is immutable. The leg is ENDED (offer released, package slot freed) by the per-frame monitor `travel::Poll()` on `Arbiter::OncePerFrame` on ARRIVAL (distance for a ref, PARENT-CELL IDENTITY for a cell), on `Actor::IsInCombat()`, or on the destination being gone — plus a 120 s STUCK safety net that logs a failure and retries nothing. **No LOS test, no detection test, no `StartCombat`, no 0xE4 PIN, so INVARIANTS #0 is untouched.** 8 concurrent legs; a 9th is refused and logged. Ships OBSERVE-ONLY | one internal ch.9 offer; the ENGINE runs the package natively |
| `TargetPin.cpp` (+ `TargetPin.h`) | 20 | PIN this actor's combat target (`kIntent_TargetPin`, ABI v13; no test key -- the crosshair surface names one ref and a pin needs an actor AND a target) | **SOURCE DENY:** write_vfunc slot 6 (`SelectTarget`) on `VTABLE_CombatTargetSelectorStandard` and `...Fixed`, chaining; the answer is replaced with the winning claim's target only when the engine answered non-zero and the target is in `combatGroup->targets` (group read lock) and not `kTargetLost`. `targetpin::Poll()` on `Arbiter::OncePerFrame` ENDS the claim (EnqueueRelease, reason logged) when the target is lost / dead / disabled / unloaded / unresolvable or the owner dies -- the only place APMF releases a client's pin claim. Character slot 0xE4 hooked OBSERVE-ONLY (`SOURCE SEAT MISSED` / `OVERWRITTEN`). Per-actor handle map (`shared_mutex`) filled on the game thread; `ResetAll` at revert + kPreLoadGame. All three seats verified before any is written. **What breaks:** the raw selector offset +0x10 is guarded only by the per-call vtable-identity test -- never read it without that test; never re-add an after-the-update rewrite (it would mask a source miss, principle 7); open review items `Docs/REVIEW-BACKLOG.md` APMF-B26 (cross-thread StopCombat), APMF-B27 (MFO hook order), APMF-B28, APMF-B29. `INVARIANTS #0 (f)` | source-block (DENY) |
| `CombatEntry.cpp` (+ `CombatEntry.h`) | 21 | ENTER COMBAT against a named target (`kIntent_CombatEntry`, ABI v14; no test key -- it needs an actor AND a target) | **ONE ENGINE CALL, NO SEAT:** `Engage` / `OnOwnerChanged` resolve the target on the game thread and `mainthread::Post` ONE task that re-validates the published claim and calls `actor->StartCombat(target, nullptr)` (fork binding, `RELOCATION_ID(37608, 38561)`, 3-arg form verified on both images). Gates in the task: actor loaded, alive, `currentProcess != null` (StartCombat dereferences it unchecked), target loaded / enabled / alive. Release calls NO StopCombat. NEVER live and inert: `EndClaim` (from Pump / Poll, never inside Drain) releases the claim on an engine refusal, an entry that cannot be attempted, "combat ended" (controller POINTER null after a successful entry), owner dead, target dead / disabled / unloaded / unresolvable. Per-actor map touched on the main seat only (no lock); `ResetAll` at revert + kPreLoadGame. Passive rate-limited entry log (entered / group member on a new fight / whether a ch.20 claim names the target). **What breaks:** never call StartCombat with two arguments (garbage R8 = the ch.6 CTD) or for an actor with no process; never re-enter on a timer or when the engine ends combat (that is SUSTAINING a decision, #0); never add a StopCombat on release (an undo, #0 (g) condition 4); never dereference the controller or group here (use the pointer; `IsInCombat` reads [cc+0x43]) -- APMF-B26 (`Docs/REVIEW-BACKLOG.md`); never EnqueueRelease from inside Drain. `INVARIANTS #0 (g)` | one-shot engine call (#0 (g)) |

- **What breaks (all channels):** each must (1) keep the package coherent — none
  substitutes the package (§5); (2) capture-and-restore engine state in `Release`,
  keyed by the per-NPC state map (guard `actor` null — it may have unloaded); (3)
  guard every struct-member write. **A channel MUST NOT generate behavior (#0):** no
  `StartCombat`, `CastSpellImmediate`, movement drive-feed, or anim trigger — only
  arbitrate + DENY. `Engage`/`Tick`/`Release` are game-thread only (#12). Only
  `Headtrack` overrides `Tick` (flagged known-incomplete, #2; ch.6/ch.8 are now
  arbitration-only, no `Tick`; **ch.19 deliberately does NOT override `Tick` either** —
  its monitor runs on the confirmed-main `OncePerFrame` seat because `Tick` is
  multi-threaded and the monitor makes engine LOS/detection calls). Movement FULL block +
  KeepOffset (the DENY gate) use Address-Library IDs (#8), VR-refused.
- **What breaks (ch.19 specifically — it is the ONE channel that files an internal claim on
  another channel; `core/ControlMap.h`'s `TryGetOwningClaimBasis` exists only for it):**
  (1) THE INTERNAL ch.9 OFFER IS TRAVEL'S IMPLEMENTATION, NOT A COMPOSED CLIENT INTENT — do
  not grow it back into one. An earlier cut also filed a ch.6 combat-target claim on the
  client's behalf and it was DELETED (marth, 2026-09-22: "We should avoid combining intents
  anyway"); a client that wants ch.6 claims ch.6. (2) The offer must stay at the CLIENT's
  basis (an invented basis steals, or loses, a facet the client did not bid on) and must be
  RE-FILED — request the new one, THEN release the old, in one `Drain` — when the winner's
  basis moves: a claim's basis is immutable, `ApplyRepoint` updates the param only, so a
  re-point alone leaves the offer arbitrating at the FIRST owner's basis. (3) Every
  ControlMap write must stay POSTED (a lifecycle call runs BEFORE `Publish`). (4) The package
  SLOT must stay owned until the posted teardown runs, or a release-then-re-request inside
  ONE `Drain` hands the new leg the very record the outgoing ch.9 claim is still offering.
  (5) The arrival radius written into the package must stay equal to the one the monitor
  tests, or "the engine thinks it arrived" and "APMF thinks it arrived" can disagree forever.
  (6) The end conditions are ARRIVAL, `IsInCombat()` and destination-gone — do NOT re-add a
  line-of-sight or detection test; the LOS one was removed because it fires before the actor
  moves in the case this facet exists for (`Docs/DENY-COMPLETENESS-AUDIT.md` row 19 (d)).
  (7) **ABI v11 position legs (`kTravel_ToPosition`, 2026-09-23).** `Engage`/`OnOwnerChanged`
  record the POINT; `Compose` places APMF's XMarker through `poscast::PlaceMarker` and the
  leg is then an ordinary ref leg to it (`destAliveAtTarget = false`: a marker never dies).
  `StillOurs` identifies a position leg by its POINT (its destId is the marker, which the
  client never sees). The marker's lifetime IS the leg: `EndLeg` (arrival, combat, actor
  gone/dead/3D, stuck, a declined re-point) and `Release` delete it via `RetireMarkerLater`
  (one posted hop, so the ch.9 release applies first; `poscast::DeleteMarker` re-checks
  handle, FormID, base); `Compose` deletes a marker the claim no longer names (a Repoint to a
  form or another point) AFTER re-pointing the package, and any marker on a leg that did not
  start. `ResetAll` forgets markers and `RestoreMarkerSlots` points every record that last
  aimed at a marker back at its PlayerRef placeholder (`g_slotAtMarker`). Break any of those
  and a marker outlives its leg or a record carries a stale marker handle across a load.
  Not rule #0 (e): no cast, only a package destination.
  (8) **ABI v12 (2026-09-24): BLOCKED end, leg-state mirror, gait, gate probe.** `Poll` reads
  the running package's `packData.packType` (TESPackage+0x24) every 250 ms through
  `ReadRunningPackage`, UNDER that ActorPackage's `packageLock` (review F7); 36 = Movement
  Blocked (the engine's own type-name table, both images). The clock counts RUNNING time
  (review F1): each MB poll adds `min(since last poll, kMbMaxStepMs = 500)`, so a menu pause
  adds at most one step; ONE non-MB poll in a run is tolerated, two end it (F6).
  `ResetBlockedClock` at every `StartLeg`, live re-point and `EndLeg`. At `kBlockedEndMs`
  (3000, sized from the deck: ~1 s healthy blips vs 39-45 s gate freezes) the leg ends
  `kLeg_Blocked` AFTER the arrival/destination checks and BEFORE the stuck net, and
  `FindBlocker` names the nearest live actor in front (192u ground plane, |dz| <= 128,
  +-60 deg of facing OR goal bearing; player / teammate / actor) into the leg info (F3).
  `leg.ownerHandle` comes from `TryGetOwningClaimBasis`'s optional handle out (Compose).
  A REFUSED re-point (`OnOwnerChanged`) or a re-pointed ref that no longer resolves at
  Compose ENDS the old leg and records `kLeg_Failed` with the refused destination (F2);
  nothing on the leg is mutated before the refusal is decided. `seq` comes from one
  process-wide `g_seqCounter` that ResetAll does not reset (F5). Never
  deny or fight MB. Every state change goes through `SetLegState[For]` into `g_state` under
  `g_stateMx` — the ONE any-thread structure besides the probe's `g_watches`; `GetLegState`
  copies it out and touches nothing else. **What breaks:** an `EndLeg` without its
  `TravelLegState` (the client then reads a stale state and treats an ended leg as a theft), a
  new end path that skips `SetLegState`, or an engine call made while `g_stateMx`/`g_watchMx`
  is held. GAIT: `ApplyGait` writes `packData.maxSpeed` (+0x26) and `kPreferredSpeed` (0x2000)
  into the slot's record right after `PointPackage` and BEFORE the ch.9 offer is queued; a
  leg without `kTravel_SpeedSet` restores the record's AUTHORED values (`g_authoredSpeed`/
  `g_authoredPrefSpeed`, captured at Install), or a previous leg's gait leaks. The engine copies
  the speed at package START (1.6.1170 0x6CE2C0 / 1.5.97 0x63BD40), so a live re-point's gait
  change is logged, not applied to the running copy. Exact-build gated (`g_gaitVerified`).
  `ObserveGait` reads the running ActorPackage under its `packageLock`, log only. GATE PROBE:
  `GateProbe` at a BLOCKED end (DOOR/ACTI refs within 1024u, parent cell + `TES::GetCell`
  corners outdoors), then `GateEventSink` (TESOpenCloseEvent +0x8F0 / TESActivateEvent +0x58)
  logs events near a watched stall for 10 min; sinks run on engine threads and only read plain
  fields + `mainthread::Post` the `GetOpenState` re-reads. Armed only when the self-check
  verifies `Travel.GetOpenState`, `Travel.ScriptEventSourceHolder.GetSingleton` and TES::GetCell
  on an exact build. Passive: it must never write anything. Deferred review items:
  REVIEW-BACKLOG APMF-B24 and APMF-B25 (read B25 R2-2 before touching `OnOwnerChanged`'s refusal
  teardown). `GetLegState` tests the caller's size against the FROZEN `kTravelLegInfoV12Size`
  (72), never `sizeof` — appended fields are written only inside the caller's size.
  Its open review findings are `Docs/REVIEW-BACKLOG.md` APMF-B13..B18 — read them before
  editing. Its deny holes are stated in
  `Docs/DENY-COMPLETENESS-AUDIT.md` row 19 — read them before editing.

### NOT built (probe-gated GAPs — do not add without a live probe)
Movement PROMOTE feed (ch.1, `IMovementDirectControl` unnamed),
the DENY of a COMPETING framework's combat-target selection at the hook (ch.6 —
still arbitration-only; the CLIENT commands the target, no T-hook yet), body
commands (T4, PROBE-gated), headtrack all-types full block (ch.5), sustained
package procedures (ch.9), facial-expression setter (ch.13). ch.8 (casting
selection) GAINED its deny 2026-09-02 (Phase 2): `core/CastGate.cpp` (T2c
CheckCast) + `core/EquipGate.cpp` (T2a CheckShouldEquip) — see those entries
above; ch.8 is no longer arbitration-only. ch.7 combat ACTIONS graduated 2026-09-03
(`core/ActionGate.cpp`, paired act/pop 2026-09-04). **CORRECTED 2026-09-07:** this
paragraph used to end "T3 (`AliasPkgProbe.cpp`'s 0x49 hook) is built as a probe but not
yet folded into the Allowance template / wired to a client". T3 GRADUATED on 2026-09-03
into `core/PackageGate.cpp` + `channels/OfferPackage.cpp` (ch.9), the probe file is
deleted, and MFO ships on it. What remains open is narrower and different: the redirect
answers only when the engine ASKS, and the engine does not ask on a useful cadence of
its own — holding a package against an outranking framework therefore depends on
nudging at the right moment (MFO `DIAG-2026-09-06-loot-travel.md`), and the non-alias
/ procedure tier (`BGSProcedureTreeProcedure`) still has no hook at all
(HOOK-SITE-COVERAGE §5).
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
