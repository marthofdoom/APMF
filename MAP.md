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
SUPPORTED|NOT SUPPORTED ... cast-classify <open|gated>, group-C <open|gated>, equip-sink
<open|gated>` line (2026-09-15; SUPPORTED/NOT SUPPORTED added by G1 2026-10-04): the
exact-version predicate (`1.6.1170 || 1.5.97 || 1.7.104` since F2b, `allowance::RuntimeSupported()`) every
runtime-dependent seat gates on, logged once at load so a deck log states its placement
state up front. `kDataLoaded` installs the
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
  pinned 3.7.0 `IsSE()` is the `default:` arm, and in the mit-3.7 fork every 1.7.x is
  filed AE, so a bucket test would admit an unverified build. **F2b (2026-10-05):**
  exactly 1.7.104 is the third supported build; each of the three copies (and every
  literal gate below) gained a 1.7.104 arm, proven per seat (`Docs/VERIFIED-ADDRESSES.md`
  "1.7.104 proof"); `allowance::IsRuntime1_7_104()` / `RuntimeAEColumn()` in
  `core/Allowance.h` pick the per-runtime values.
  **G1 exact-version sweep (2026-10-04):** `core/Allowance.h` now carries
  `allowance::RuntimeSupported()` (`REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) ||
  IsExactly(SKSE::RUNTIME_SSE_1_5_97)`, the mit-3.7 fork's). Every install that used to
  test `IsVR()` alone (Hook, CastGate, EquipGate, ActionGate, PackageGate, Travel,
  NonAliasProbe, AiCastSeats A/B, CastSeats, the input test surface here, ch.1's
  Engage/Release) now also refuses, by name, any build that is not exactly one of the
  two. The `[runtime]` line uses that helper. The literal-compare copies above (and in
  PositionCast, SpaceQuery, EquipSink, CasterTypeCensus, Travel's gait/gate probe) and
  the four `IsExactly` pairs in the ch.20/21/22/12 Installs evaluate the SAME predicate;
  if another binary is ever admitted, change all of them (grep `RuntimeSupported`,
  `IsExactly`, `IsRuntime1_7_104`, `Version{ 1, 6, 1170`). Never add an `IsAE()/IsSE()`
  test for a path. F2b (2026-10-05) admitted exactly 1.7.104 everywhere (all of them
  changed; the ch.20/21/22/12 pairs now call `RuntimeSupported()`).
  **APMF-B45 (F2b):** `ControlMap::EnqueueRequest` / `EnqueueCast` refuse a new claim
  (kInvalidHandle, logged once) when `RuntimeSupported()` is false or
  `hook::RefusedReason()` (`core/Hook.h`) says the 0xAD seat was refused.
  Open: REVIEW-BACKLOG APMF-B47 (a claim queued before kDataLoaded outlives a refused 0xAD install).
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
`kTravel_SpeedSet` (1<<2) + `kTravel_SpeedMask` (bits 3-4 = the engine's PreferredSpeed)**;
**ABI v18 adds `APMF_API_v18 : APMF_API_v12` with TWO slots, `GetLineOfSight(viewer, target,
APMF_LosInfo*)` (any thread, lock-free) and `SenseActor(const APMF_AwarenessQuery*,
APMF_AwarenessResult*)` (main thread only), the PODs `APMF_LosInfo` (24 B), `APMF_AwarenessQuery`
(32 B), `APMF_AwarenessResult` (48 B), the enums `LosVerdict`, `LosWhy`, `TargetPinFlags`,
`AwareSense`, `AwareFlags`, `AwareDetail`, the `kLos*` / `kAware*` timing and range constants,
`kCastFlag_OwnLineOfSight` (CastFlags bit 6) and `kTargetPin_OwnLineOfSight` (kIntent_TargetPin
param.ival bit 0)** (core/Sightline.h, core/Awareness.h); **ABI v19 adds `kIntent_CombatApproach = 24`,
`CombatApproachFlags` (`kApproach_Hold`), `CombatApproachState` (0-13), the POD `APMF_CombatApproachInfo`
(48 B, size rule frozen at `kCombatApproachInfoV19Size`) and `APMF_API_v19 : APMF_API_v18` with ONE slot,
`GetCombatApproachState(actor, APMF_CombatApproachInfo*)` (any thread)** (channels/CombatApproach.h);
**ABI v20 adds `kCastFlag_FloorSpellsOnly` (CastFlags bit 7) only; ABI v21 adds `kIntent_CastGuard = 25` (ch.25)
and the `CastGuardFlags` (`kCastGuard_LineOfFire` 1<<0, `kCastGuard_ChannelRight` 1<<1, `kCastGuard_ChannelLeft`
1<<2, `kCastGuard_All` 0x7) and NOTHING else: no struct, no slot (core/CastGuard.h). To stay under 2500 lines the
v21 change also folded the v13..v16 history paragraphs into one (comments only)**; **ABI v22 (2026-10-12, weighting
phase 1) adds `kIntent_EquipPreference = 26` (ch.26), `EquipPrefFlags` (`kEquipPref_Force` 1<<0), the step constants
`kEquipPrefStepRel` (0.05) / `kEquipPrefStepAbs` (0.01), `kMaxEquipPreference` (8), the 8-byte POD
`APMF_EquipPrefEntry{form@0, slot@4, rank@5, reserved[2]@6}`, `EquipPrefState` (0-8), the 64-byte POD
`APMF_EquipPrefInfo` (size rule at `kEquipPrefInfoV22Size`) and `APMF_API_v22 : APMF_API_v19` with TWO slots,
`SetEquipPreference(handle, entries, count)` and `GetEquipPreferenceState(actor, APMF_EquipPrefInfo*)`** (core/EquipPreference.h);
**ABI v24 (2026-10-13, ch.12 idle state) adds `IdleState` (0-10), `IdleUnconfirmable` (0-6), `IdleEnd` (0-5),
`kIdleInfo_Carried`, the informational `kIdleConfirmWindowMs` (3000, static_assert-tied to `Idle.cpp`'s `kConfirmWaitMs`) /
`kIdleStateGraceMs` (10000) / `kIdleMsNone`, the 60-byte POD `APMF_IdleStateInfo` (size rule at `kIdleStateInfoV24Size`)
and `APMF_API_v24 : APMF_API_v22` with ONE slot, `GetIdleState(handle, APMF_IdleStateInfo*)` (any thread)**
(channels/Idle.h). **ABI v25 (2026-10-14, ch.17 entry instance) adds NO struct and NO slot:
`kEquipEntry_HasUid` (reserved[0] bit 0) + the uid in `APMF_EquipEntry::reserved[1..2]` and the inline
helpers `SetEquipEntryUid` / `EquipEntryUid`** (Docs/APMF_API-REFERENCE.md#abi-v25-equip-instance).
**ABI v26 (2026-10-19, ch.27 substrate state, ClickUp 86e3pu939) adds `SubstrateState` (None 0 / Held 1 / Real 2 /
Anchor 3) and `APMF_API_v26 : APMF_API_v24` with ONE slot, `GetSubstrateState(actor)` (any thread, lock-free)**
(channels/CombatSubstrate.h `StateOf`; Docs/APMF_API-REFERENCE.md#abi-v26-substrate-state).
**ABI v27 (2026-10-19, ch.27 substrate availability, round R1) adds `APMF_API_v27 : APMF_API_v26` with ONE slot,
`IsSubstrateAvailable()` (any thread, one load of `combatsubstrate::Installed()`)** (Docs/APMF_API-REFERENCE.md#abi-v27-substrate-available);
v27 had been left free for the per-item equip deny (86e3t8w23), which moves to v28. The header is 2542 lines after
v27 (still over the 2500 backstop; reported again, not split inside the task).
**ABI v28 (2026-10-19, `kIntent_Place`, MFO town phase 2) adds intent 28 (rides `RequestEx`; a one-shot, never a
claim), `PlaceFlags` (OnlyIfUnloaded / RefuseIfPlayerPresent / SetYaw), `PlaceState` (Unknown 0 / Queued 1 / Placed 2 /
Refused 3 / Failed 4), `PlaceWhy` (0-10), `kPlaceInfo_*`, `kPlaceStateGraceMs` (30000), the 64-byte POD
`APMF_PlaceStateInfo` (size rule at `kPlaceStateInfoV28Size`) and `APMF_API_v28 : APMF_API_v27` with ONE slot,
`GetPlaceState(handle, APMF_PlaceStateInfo*)` (any thread)** (core/Place.h; Docs/APMF_API-REFERENCE.md#abi-v28-place).
The per-item equip deny (86e3t8w23) moves to v29. The header is 2661 lines after v28 (over the 2500 backstop,
reported again, not split inside the task). The header
is at 2520 lines after v26 (OVER the 2500 backstop; reported, not split inside the task) and `core/ControlMap.cpp`
at 2499: the next change to either plans a split first.
To stay under 2500 lines the v24 change first moved three long-form blocks (v18 own ray,
SenseActor senses, v22 equip preference) to `Docs/APMF_API-REFERENCE.md`, comments only, the
`Intent` enum, `Handle`, and the exported query-fn name. No C++ class / STL / vtable
crosses the boundary. **Companion docs (2026-10-12, marth: historical notes go to a note file):** the header
keeps a concise contract comment on every declaration and a `// History: Docs/APMF_API-HISTORY.md#<anchor>` or
`// See Docs/APMF_API-REFERENCE.md#<anchor>` pointer where text moved; `Docs/APMF_API-HISTORY.md` holds the
historical notes and `Docs/APMF_API-REFERENCE.md` the long-form explanations, verbatim and anchored per symbol.
A header comment change is still a byte-shared change (MFO mirrors the file); keep both docs in step with any
moved text. **The current `kABIVersion` is stated ONLY in the header**
(INVARIANTS #14b — this line said 3 while the header was at 6).
- **What breaks:** APPEND-ONLY forever (#14/#14a). Never reorder/change a shipped
  `Intent` value, struct, or fn-pointer slot — a v1 client must keep working. New ABI
  = a new `APMF_API_vN` whose leading members mirror v(N-1) exactly (prefix
  extension), + appended slots; `APMF_Param` grows only at the END. A client (MFO)
  and APMF are separately built DLLs; this header + `APMF_GetInterface` are the ONLY
  seam. APMF carries zero client-specific code.

### `native/core/ClientAPI.{h,cpp}` — the C-ABI implementation
The exported `APMF_GetInterface(abiVersion)` hands over the static POD newest-struct
object (`APMF_API_v19` since ABI v19 — read the header, not this line) as a base
`APMF_API_v1*`; a client casts up to the newest struct it uses. It returns **nullptr**
when the client asks for a version NEWER than this APMF implements (`:125-128`), which
is why a needless `kABIVersion` bump is expensive (INVARIANTS #14b). Its
`Request`/`RequestEx`/`Release`/`Repoint`/`SetSpellAllowList`/`SetEquipSet`/`SetEquipSetEx`/
`SetEquipScope` fn-pointers forward to `ControlMap::EnqueueRequest/Release/Repoint/SetSpellAllowList/
SetEquipSet/SetEquipSetEx/SetEquipScope` (Request == RequestEx with a null param; `SetEquipSet` is
`SetEquipSetEx` with every slot Default); `IsEquipAuthorityEnforced` → `equipsink::Enforcing()`.
`MinReleaseForAbi` names the first release per ABI (v7..v10 = "0.9.5", `core/ClientAPI.cpp:226-229`; REVIEW-BACKLOG APMF-B10 CLOSED 2026-10-06).
ABI v11 adds `FindEmptySpace` / `FindHostilesInSpace` → `core/SpaceQuery.cpp` (each a
`try/catch(...)` returning `kQuery_Failed`), and `MinReleaseForAbi(11)` = `0.9.8`.
ABI v12 adds `GetTravelLegState` → `travel::GetLegState` (`channels/Travel.cpp`, a
`try/catch(...)` returning `kLeg_None`), and `MinReleaseForAbi(12)` = `0.9.8` (v11 and v12
ship together; both static_assert pairs pin the prefix ends).
ABI v18 adds `GetLineOfSight` → `sightline::GetLineOfSight` (a `try/catch(...)` returning
`kLos_Unknown`) and `SenseActor` → `awareness::SenseActor` (`kQuery_Failed`); the object is an
`APMF_API_v18` now (a third static_assert pair pins the v12 prefix end), and
`MinReleaseForAbi(18)` names "the first release after 0.9.11" until the cut.
ABI v19 adds `GetCombatApproachState` → `combatapproach::GetState` (a `try/catch(...)` returning
`kApproachState_None`); the object is an `APMF_API_v19` (a fourth static_assert pair pins the v18 prefix
end), and `MinReleaseForAbi(19)` shares v18's placeholder until the cut.
ABI v21 (ch.25 `kIntent_CastGuard`) adds no slot: `MinReleaseForAbi(21)` = "the first release after 0.11.4"
until the cut.
ABI v22 (ch.26 equip preference) adds `SetEquipPreference` -> `equippref::Set` and `GetEquipPreferenceState` ->
`equippref::GetState` (each `try/catch(...)`); the object is an `APMF_API_v22` (a static_assert pair pins the v19
prefix end); `APMF_Release` also calls `equippref::Forget(handle)`; `MinReleaseForAbi(22)` = "the release after
0.13.0" until the cut.
ABI v24 (ch.12 idle state) adds `GetIdleState` -> `idle::GetState` (`try/catch(...)` returning `kIdleState_Unknown`); the
object is an `APMF_API_v24` (a static_assert pair pins the v22 prefix end); `RequestEx(kIntent_Idle)` calls
`idle::NoteFiled` after the enqueue (insert-only: Pending), `Repoint` calls `idle::NoteRepoint` BEFORE
`EnqueueRepoint` (so the game thread's play of the new param always lands after it), `Release` calls
`idle::NoteReleased`; `MinReleaseForAbi(23/24)` = "0.13.1" until the cut.
ABI v26 (ch.27 substrate state) adds `GetSubstrateState` -> `combatsubstrate::StateOf` (`try/catch(...)` returning
`kSubstrateState_None`); the object is an `APMF_API_v26` (a static_assert pair pins the v24 prefix end);
`MinReleaseForAbi(25/26)` stays the default ("a release newer than this one") until the cut names it.
ABI v27 (ch.27 substrate availability) adds `IsSubstrateAvailable` -> `combatsubstrate::Installed()` (`try/catch(...)`
returning false); the object is an `APMF_API_v27` (a static_assert pair pins the v26 prefix end); `MinReleaseForAbi(27)`
is the default too.
ABI v28 (`kIntent_Place`): `Request` / `RequestEx` DIVERT intent 28 to `RequestPlace` BEFORE the control map (which has
no channel for it): a handle from `ControlMap::NextHandle()`, `sources::NoteClientRequest` first, then
`place::Enqueue(h, actor, param, sources::ClientForHandle(h))` (the client captured at the call); a synchronous refusal
calls `sources::ForgetHandle(h)` and returns `kInvalidHandle`. `GetPlaceState` -> `place::GetState` (`try/catch(...)`
returning `kPlaceState_Unknown`); the object is an `APMF_API_v28` (a static_assert pair pins the v27 prefix end);
`MinReleaseForAbi(28)` is the default.
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
**ABI v25 INSTANCE:** `PendingOp::equipInst[3*kMaxEquipSet]` (each entry's raw reserved bytes, copied at
enqueue) → `ApplySetEquipSet(..., inst, ...)` validates (only `kEquipEntry_HasUid` alone with uid != 0
names an instance; anything else non-zero = one warn per handle, read as no uid) into `Claim::equipUids[]`
(+ `equipBadInst`, appended at the END), compared for `changed`; `TryGetEquipSet` copies them to
`EquipSetView::uids[]` (appended at the END) in the same snapshot read.
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
**ch.25 (ABI v21):** `EnqueueRequest` refuses a `kIntent_CastGuard` claim by `castguard::RefuseReason`;
`ApplyRequest` grants it a TTL (`castguard::TtlMs(param.fval)`, the cast bounds) so the generic Drain TTL pass and
Repoint renewal apply. Its readers `TryGetCastGuard` / `CastGuardActors` are declared here but DEFINED in
`core/CastGuard.cpp` (this file is near the 2500-line cap; a split is its own brief).
**ABI v28:** `NextHandle()` (header-only, any thread) hands `core/Place` a handle from the claim counter for a one-shot
that never becomes a claim; `ControlMap.cpp` is not touched (2499 lines, at the cap).
**ch.26 (ABI v22):** `EnqueueRequest` refuses a `kIntent_EquipPreference` claim by `equippref::RefuseReason`. The
declaration lives OUTSIDE the map (`core/EquipPreference.cpp`'s own snapshot, keyed by handle); the seats read the
winning claim with `TryGetOwningClaimBasis`. **D2:** `TryGetEquipSet` also copies the winning ch.17 claim's
`handle` into `EquipSetView` (same snapshot read) so the sink ranks its deny with that claim's client.
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
Refused off exactly 1.6.1170 / 1.5.97 / 1.7.104, VR included (G1 2026-10-04, F2b 2026-10-05,
`allowance::RuntimeSupported()`); a refusal is recorded for `hook::RefusedReason()` (APMF-B45). Installed once. `OnMainThread()` (ABI v11) compares the calling thread
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

### `native/core/Sources.{h,cpp}` — HARBINGER 1a: source registry, priorities, degrade rule, seat census
**Served counter (2026-10-12, fix/apmf-menu-client-hits):** `served[facet]` atomic + `NoteServed(id,f)` / `NoteServedClaim(handle,f)` (resolves the client via `ClientForHandle`'s shared leaf lock, released before the atomics) + `Served()`. It is NOT a Hit: Hits stays "outside writes seen". Both stamp `lastSeenMs` / `lastLoadSeq`, and so does `NoteClientRequest` (a client with live claims is never `Idle`, never dropped from the menu's `Winner()`). Credit sites: `facetattr::Decide` on `enforce` (every ranked deny: cast, combat, action, approach, entry, re-entry), `EquipSink` applied deny, `TargetPin` seat deny/held + `UpdateCombat` restore, `EquipAuthority::Enforce` equipped>0, `PackageGate` 0x49 `won`, `Travel::StartLeg`, `EquipPreference` lift/steer, `CombatEntry::Enter` entered. What breaks: moving a credit into a hot path that holds another lock (the handle map's lock is a leaf).

**Menu counters + Equip split (2026-10-18, feat/apmf-menu-split):** `Facet::kEquip` became `kEquipWeapon` (same index 3, INI section `EquipWeapon`) plus an APPENDED `kEquipArmor` (index 9, `kFacetCount` 10; `g_unkPending` initialiser and its `static_assert` track it). `FacetForEquipCats(Categorize mask)` (inline, Sources.h) picks the facet: any Right / Left / Ammo / Light bit (a shield or torch carries Left) = Weapon, Armor-only = Armor. `FacetMaskForIntent` replaces `FacetForIntent` (the three equip intents claim both). `ReadIni` reads a legacy `[Equip]` section (keys, `iUnregisteredEquip` / `iClientEquip`, `[UnknownSources] Equip`) as a PER-KEY fallback and never rewrites it; the slider writes the new sections. New counters `observed[]` (`NoteObserved`: `facetattr::Observe` users, the package poll's outranked-not-enforced) and `own[]` (`NoteOwn`: a claim's own client on its own claim, from `facetattr::Decide` and the EquipSink own paths). `EquipSink` computes `Categorize` BEFORE the caller recovery (the facet keys every counter) and DEFERS the hit of a source that is the client of a claim on the actor (`hitDeferred` / `isOwnClient`, flushed only against another client's claim). `RankEquipDeny` takes the facet. Menu columns Served / Hits / Blocked (= overruled) / Passed (= stood) / Observed; `Winner()` requires `LastSeenMs` within 60 s. What breaks: a new equip-facet counter site that hard-codes a facet instead of using the item's `FacetForEquipCats`; reordering the Facet enum (the INI section names and `involved` bitmask are by name / bit, but `g_unkPending` is positional).

**1b observe slice (2026-10-12):** facet `kActorValue` ("ActorValue": ch.1a gait / ch.11 disposition, fed by
`core/ActorValueObserve`); `Kind::kLoaded` + `RegisterLoadedPlugins()` (kDataLoaded: `K32EnumProcessModules`, every
module under `...\SKSE\Plugins\` but APMF pre-registered by base name, keeping 160 of the now 512 registry slots free;
`kind` is atomic and a kLoaded entry takes the kind it is first SEEN as, in `Register` / `AddOneLocked`); the menu
lists still-kLoaded sources under "Loaded SKSE plugins not seen writing yet".
Plan `_research/harbinger-controller-plan-2026-10-11.md` + marth's Q1-Q15 (2026-10-11). An outside
write on a facet is an INPUT from a SOURCE `{name, kind}` (client / hook / interposer / plugin /
script / engine / direct calls / unknown). `Init()` (at SKSEPluginLoad, before ch.20's layer 0) reads
`Data/SKSE/Plugins/APMF_Sources.ini` (`[Defaults] iUnregistered<Facet>=20`, `iClient<Facet>=50`;
`[<Facet>] <Module.dll|Plugin.esp>=0..100`, 0 = off; facets Target / Detection / Package / EquipWeapon /
EquipArmor / CombatAction / Cast / Combat / Core / ActorValue; legacy `[Equip]` read as a per-key fallback) and pins the built-ins at ids 0..2 (Engine, DirectCalls = 75,
Unattributed). The registry is a fixed append-only array (256): name/kind written before the
count is published (release), so readers on any thread (combat jobs, SMF's render thread) scan
it lock-free; counters/priorities are atomics; one mutex on the add. CLIENT IDENTITY (Q7):
`core/ClientAPI.cpp` Request / RequestEx / RequestCast pass `_ReturnAddress()` and the intent to
`NoteClientRequest` (handle -> module via `GetModuleHandleEx(FROM_ADDRESS)`); Release forgets;
`Pump()` purges dead handles every 60 s. `ClaimPriority(handle, facet)` = the client's priority,
the client default for an internal handle. THE DEGRADE RULE (Q4): `Unattributed(facet, actor,
detail)` = allowed (unless that facet's `[UnknownSources] <Facet>=override`, `OverrideUnknown(f)`: then an unattributed writer ranks at Priority(Unattributed) floored by the unproven segment owner (stands if that is the claiming client), and only ch.20's restore in `channels/TargetPin.cpp` acts on it; stored/shown only on other facets; menu checkbox -> `EnqueueSetOverrideUnknown` -> Pump saves) + rate-limited `[sources] UNATTRIBUTED <facet>` + an alert row (menu); NEVER a
deny. THE SEAT CENSUS: every seat registers at install (`RegisterVtableSeat` / `NoteVfunc` /
`RegisterCallSeat`; `allowance::InstallOnVtables` registers its rows itself, facet from its tag via
`FacetForTag`); the module of the DISPLACED value is a hook that runs inside Harbinger's seat.
`Census()` re-reads every seat at kPostLoad / kPostPostLoad / kInputLoaded, kDataLoaded (baseline), kPostLoadGame / kNewGame (`OnPostLoad`,
also the Q14 load sequence) and every 60 s (`Pump`): an entry now holding another module's pointer
= a LATE installer -> a source on that facet, logged once; a seat registered with a `rehook`
callback (today only ch.20's 0xE4) gets a new Harbinger layer directly above the new hook (Q13); others log "lost outermost" (a call-site
seat stays innermost, so its deny holds). An entry now holding ANOTHER APMF seat (ch.24's selector
over ch.20's) is still ours. `Pump()` also applies queued menu slider changes (atomic store +
`WritePrivateProfileStringA`, main thread). **THE CONTROLLER'S DESIGN RULE (marth 2026-10-11: "harbinger should ALWAYS be able to tell"):
recognition is COMPLETE on every facet. Every write a facet sees is attributed to its real writer;
`Unattributed` is the fail-safe for a DEFECT (a recognition gap Harbinger reports loudly), never an
expected state. 1a meets it on Target (a layer between every pair of foreign 0xE4 hooks, each
segment proven) and Equip (stack scan); Package attributes by scene / package plugin; 1b's
acceptance bar is no expected unattributed path on any facet.** Open review items: REVIEW-BACKLOG
APMF-B69 (INI parsing, slider debounce, census-only idle, plugin.cpp comment, thread-local frame on
unwind). A module can be a hook source AND a client (MFO.dll): `kind` keeps how it was first seen,
`IsClient` records that it files claims, and its priorities then follow the client default
(`NoteClientRequest`). Module image ranges are cached (`ModuleOf`), `IsSelf` keeps APMF's own equips
out of the sources. Phase 2 ABI items NOT in 1a (no ABI change here):
`RegisterClient(name)` (Q7 override), a hook-into-Harbinger registration for late installers (Q13),
and a fresh per-decision line-of-sight call (MFO-B229).
- **What breaks:** never deny in the name of `kUnattributedSource` or a `kNoSource` writer — that
  is the whole degrade rule; never make a reader path take `g_addMx` (Find/Priority/Hits are the
  lock-free reads the combat thread and the render thread use); never reorder the built-ins (ids
  0..2 are constants); the census reads POINTERS only — never call through what it finds. Census
  rows are registered on the main thread only (kDataLoaded installs).

### `native/core/HarbingerMenu.{h,cpp}` — HARBINGER 1a: the SKSE Menu Framework page
`Register()` at kDataLoaded: `GetModuleHandleA("SKSEMenuFramework.dll")`, then resolves
`AddSectionItem` and 20 cimgui `ig*` exports by name (signatures copied from SMF's own consumer
header `resources/SKSEMenuFramework.h`, QTR-Modding/SKSE-Menu-Framework-3 @ dc91824; nothing
vendored). Any export missing -> the page is refused whole, loudly; SMF absent -> one loud line,
Harbinger runs from the INI (Q10). Page `Harbinger/Sources` (layout 2026-10-11): an alerts summary line
(expandable), a "hide idle" checkbox (UI state only), one collapsible header per facet (collapsed unless it has
an alert) holding an "Unknown sources can be overridden" checkbox (saved to `[UnknownSources]`), then a fixed-column table (source, kind badge, served, hits, blocked, passed, observed, priority slider; Equip is two headers, weapon and armor; idle sources greyed and last;
details in the hover tooltip), and a collapsed census list at the bottom. Exports: `AddSectionItem` + 20 `ig*`
(adds igSpacing, igCheckbox, igCollapsingHeader_TreeNodeFlags, igBeginTooltip, igEndTooltip, igIsItemHovered;
the tab-bar exports are no longer used).
- **What breaks:** the render callback runs on SMF's render thread — it may only read lock-free
  registry state / the census snapshot and ENQUEUE slider values; never call an `ig*` pointer that
  was not resolved (the refusal resets them all).

### `native/core/PackageSources.{h,cpp}` — HARBINGER 1a: package capture + the 0x4A scene PROBE
`Poll()` (main, 500 ms): for each actor with a winning `kIntent_OfferPackage` claim (ch.19's
internal offer included), a running package != the claimed one is captured: source = the plugin
of the actor's current scene (`GetCurrentScene()->GetFile(0)`, parent quest logged), else the
package's own plugin, else (runtime-created, FF) the Engine; a scene with no file is Unattributed.
Logged once per transition with both priorities (claim = its client, or the winning travel
claim's client for ch.19's internal offer). NOTHING IS ENFORCED: Q3's "hold the scene off" needs a
seat, and the candidate — Character slot 0x4A `GetCurrentScene` (AE 0x662150 / SE 0x5D19E0 /
1.7.104 0x674B70, reads the kHasSceneExtra bit, `BGSScene*(const Actor*)`; `EvaluatePackage` AE
37401+0x1F0 re-evaluates only when it answers null) — is UNPROVEN in the field (principle 5). So
`Install()` puts an OBSERVE-ONLY chaining hook there ([Harbinger] bSceneProbe, default 1): for a
claimed actor it logs each distinct caller RVA that asks while a scene stands (`[scene-probe]`,
capped) and a 60 s heartbeat. Its own poll's calls are filtered by this DLL's image range.
- **What breaks:** the probe must stay a pass-through (return the original answer); the deny is a
  separate, probed, reviewed round. `ResetAll()` at kPreLoadGame + revert.

### `native/core/CastProxy.{h,cpp}` — the delivery-flip PROXY POOL (was `CastExecutor`)
**The forced cast DRIVE that used to live here is DELETED** (feat/ai-cast-seats-impl,
2026-09-05): `PhaseSelect`/`PhaseRest`/`PhaseDrawn`/`PhaseFire`/`PhaseHold`/
`PhaseParkWait`, `ParkHand`/`TeardownHand`/`EmitStopTail`/`StartHandDrive`/
`ResolveHands`/`ResolveTarget`, the wall-clock `Budget` plumbing, the per-hand
`g_drives` state, and the `FireFallback` (`CastSpellImmediate`) guaranteed-delivery
path. Superseded by `core/CastSeats.cpp`'s five engine seats, which make the NPC's own
AI perform the cast. Do not resurrect any of it: the fallback in particular is what
INVARIANTS #0 forbids by name.
What survived is the field-proven ally road for a **kSelf-delivery** spell. (CORRECTED
2026-10-06, APMF-B62: the old reason, "the Self branch never reads `desiredTarget`, so a kSelf
spell always lands on the caster", is false on all three builds -- FindTargets' Self branch
takes `desiredTarget` (+0x20) when it is an Actor, and seat 0x0A feeds it; the un-proxied
road is unproven, not impossible.) So this module mints
a delivery-flipped `kTargetActor` COPY (`Configure`: source `data` + shared source
`Effect*` by pointer, delivery flipped) that the AI selects and casts instead.
`Acquire` (mint or SHARE + TRANSIENT `AddSpell`; +1 ref) / `Unref` (-1 ref; un-teach +
deselect + release the slot only at 0) / `ResetAll` (revert + kPreLoadGame) /
`PreSaveSweep` (SKSE save) / `ReteachLive` (posted by `OnSave` right after the sweep:
re-teaches every slot still holding refs on the next Pump, because a Repoint-heartbeated
claim never calls `Acquire` again). Fixed 8-slot pool keyed by (OWNER, SOURCE SPELL,
HAND: right / left / dual) and REF-COUNTED across the live claims naming each form
(fix/apmf-proxy-per-claim-refcount, 2026-10-05): a same-hand re-aim or same-hand rival
shares ONE form; the same spell on the OTHER hand gets its OWN form, because the seats
resolve a claim BY DRIVEN FORM (`TryGetCastSeatClaimForForm`) and one shared form aimed
both hands at the first claim's target; different kSelf spells get different forms, and
a slot holding refs is never re-Configured. Live slots never share a FormID (a mint
whose id is 0 or collides is refused loudly). Each claim that got a form records
`Claim::castProxyRef` and owns exactly one ref. EVERY driving kSelf-at-ally claim takes
a proxy (no prospective-winner gate); one that cannot get one keeps an INVALID target
handle, so no seat drives its original form (it would heal the caster).
**SELF-FLIP (feat/apmf-self-delivery-proxy, 2026-10-05):** the pool also mints the REVERSE copy --
`Flip::kToSelf`, delivery kSelf -- for an Aimed / Touch / TargetActor beneficial spell claimed at the claimant itself
(the SELF-FLIP block in `ControlMap::ApplyRequest`, right after the delivery-flip block). The flip direction is
part of the key and must match the source's delivery (`Acquire` refuses otherwise). Gate (review F2): no hostile
or detrimental effect, no Calm / Frenzy / Demoralize / Paralysis / Stagger archetype. A kSelf mint arms a passive LANDING watch checked
on the OWNER's OWN update (`castproxy::OnOwnerUpdate`, from `Arbiter::OnActorUpdate` after the Character 0xAD
original; per-watch leaf mutex, try_lock only; pre-mint effects of a reused slot are a baseline, ignored) that logs
`LANDED on the caster` or `NOT seen on the caster`. Slot ranking (review F3): a free slot already mirroring this
source + flip, else never minted, else freed longest ago (APMF-B58). Pool pressure: APMF-B62. Disassembly (all three builds, STATUS "SELF-FLIP PROXY"): the copy keys the native self row
(ctor `[+0x4c]` = GetDelivery()==kSelf), gets no aim controller (49081: delivery 2/3 only), and FindTargets' Self
branch lands it on `desiredTarget` when that is an Actor (0x0A answers the claimant) else the caster.
WRITER/MAIN THREAD ONLY.
Called from: `ControlMap::ApplyRequest` (`Acquire`, on the writer thread where form
lookups are legal, BEFORE the claim publishes), and every claim-removal path in `core/ControlMap.cpp`
(`ApplyRelease`, dual/single eviction, the Drain unload sweep, `ReleaseAll`) through
`PostProxyUnref` — deferred one `mainthread::Post` hop so it lands AFTER the removal
publishes (INVARIANTS #20). Ordering: Acquire is synchronous inside Drain and Unref runs
in the Pump after it, so a same-Drain re-acquire always increments before the old
claim decrements. `plugin.cpp` (`ResetAll` on revert + kPreLoadGame, `PreSaveSweep` on
save). `channels/CastCompose.cpp`'s `Release` no longer frees anything (its owner-wide
`Free` was the bug).
- **What breaks:** both halves of INVARIANTS #19. (a) A proxy KNOWN to the actor when
  a save is taken persists a reference to a 0xFF dynamic form that will not exist on
  the next load — `PreSaveSweep` must stay wired into `OnSave` BEFORE any record is
  written, and `Unref` must stay the single choke point every claim-removal path
  reaches, exactly once per `castProxyRef` claim (a missed Unref leaks a taught form;
  an extra one un-teaches a form a live claim still names — the 2026-10-05 SEV-2).
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
Runtime gate: exactly 1.6.1170 / 1.5.97 / 1.7.104 (F2b), VR refused. Evidence:
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

### `native/core/Place.{h,cpp}` — ABI v28 PLACE (`kIntent_Place`, a one-shot, never a claim)
Puts an actor (loaded or NOT) beside a reference (its position + `param.pos`, in its cell and worldspace: Papyrus
`MoveTo(akTarget, x, y, z)`) or at `param.pos` in an INTERIOR cell, with ONE `TESObjectREFR::MoveTo_Impl` (private in
the fork, called through `REL::Relocation<MoveToImpl_t>{ RELOCATION_ID(56227, 56626) }`, verified row
`Place.TESObjectREFR.MoveTo`) on the game thread. Research (all three builds): Papyrus `MoveTo` only queues a VM task;
its Run (AE 56101 / SE 55572) calls `MoveTo_Impl`; a `Character` there gets `StopInteractingQuick(true)` + the actor
relocation (AE 37313) with (point, rot.z, cell, worldspace); the handle argument is read on the player branch only, so
an empty `ObjectRefHandle` is passed. `Install` (kDataLoaded): VR refused, `allowance::RuntimeSupported()` (exactly
1.6.1170 / 1.5.97 / 1.7.104), `SeatVerified`; no hook, no INI. `Enqueue` (any thread, from `ClientAPI::RequestPlace`):
POD checks only (no form lookup), a Queued record, ONE `mainthread::Post(Deliver)`. `Deliver`/`Run` (game thread,
`mainthread::Pump` in `Arbiter::OncePerFrame`): world-generation check, actor / destination resolution (exterior cell
refused), the two flags, ARBITRATION (the winning ch.19 / ch.1 claim via `ControlMap::TryGetOwningClaimBasis` must be
the same `sources` client, else Refused TravelOwned / HoldOwned), the call, then a POSITION READ-BACK (within 1u =
Placed, else Failed NotMoved, ERROR line). One log line per request: `queued`, then `PLACED` / `REFUSED` / `FAILED`.
Records: `g_recs` under a leaf mutex, cap 128 (oldest finished evicted; full of queued = refused at the call), grace
30 s wall after finishing. `ResetAll` (revert, kPreLoadGame, `plugin.cpp`) clears them and bumps `g_gen` under the
same lock, so a posted move from the old world drops itself (Discard() drops it too).
- **What breaks:** (a) making it a CLAIM: it would be swept as unloaded and engaged on a live actor only, which is the
  case it exists to avoid. (b) a form lookup in `Enqueue` (caller's thread). (c) moving the call off the game thread.
  (d) reporting Placed without the read-back (principle 7). (e) passing a handle and assuming it is read: on the
  Character branch it is not, which is the only reason a bare CELL destination works; a non-actor self would take the
  generic branch (the player is refused, `LookupByID<RE::Actor>` guarantees an actor). (f) dropping the client capture
  at the call: a client `Release` on the place handle forgets its attribution, and the arbitration would then refuse
  the client's own placement.

### `native/core/SpaceQuery.{h,cpp}` — ABI v11 SPACE QUERIES (read-only)
`FindEmptySpace` (walk ray at feet+64u, down ray to feet-maxDrop, 8 knee-height
clearance rays, 4 ring ground rays, live-actor scan) and `FindHostilesInSpace` (high
actors + player, same worldspace/cell, 3D radius, `candidate->IsHostileToActor(side)`,
nearest first, cap `kMaxHostileResults` = 64). Rays: `bhkPickData` +
`bhkWorld::PickObject` under `worldLock` (read), filter
`(originActorSystemGroup << 16) | kCharController` (MFO Sightline's field-proven recipe);
ground = layers static/terrain/ground (the engine's own Target Location placement rule)
plus the stair helper. A ray whose pick comes back with `bhkPickData::unkC0` set was NOT cast
(the engine's early-out): `Cast` flags it `skipped` and the query returns `kQuery_Failed`, never
"clear" (APMF-B52, same rule as `core/Sightline.cpp`). Synchronous; `hook::OnMainThread()` or `kQuery_NotMainThread`.
Runtime gate as PositionCast (`Install` at kDataLoaded).
- **What breaks:** calling either off the main thread (havok world and the hostility
  test are main-thread state; the refusal is the guard, do not remove it). Writing
  past the caller's `size` (append-only structs: always honour `out->size`). Using a
  hand-kept faction list instead of `IsHostileToActor`. The "stair helper counts as
  ground" choice is a judgement call, not engine evidence (see the file comment).
  Note (ABI v18 finding, not changed here): `Cast` does not check `bhkPickData::unkC0`
  (+0xC0), which `PickObject`'s engine early-out sets when it returns WITHOUT casting --
  such a ray reads as "no hit". core/Sightline.cpp treats it as UNAVAILABLE. Open: APMF-B52.

### `native/core/Sightline.{h,cpp}` — ABI v18 OWN LINE OF SIGHT (2026-10-05, ClickUp 86e3h6qj9)
Harbinger's own-ray verdict for an ordered (viewer, target) pair, replacing the engine's
cached `Actor::HasLineOfSight` (AE 53829 0x9BA120: for an NPC viewer a cached byte of the
AIProcess LOS cache, no ray). The ray (MFO Sightline's own-ray recipe): from
`GetEyeVector`'s origin (Character 0xC2; NO facing / cone), to feet+16 / 55% / 90% of
`TESObjectREFR::GetHeight() const` (0x73/0x74; NOT `Actor::GetHeight`, which writes the
cached height), each ray ending short by the TARGET's own size (review SEV-3, 2026-10-05:
the fixed 48u read giants / mammoths / dragons as OCCLUDED by their own capsule): bound
half-diagonal x base scale + 16, clamped [48, 1024], from the same GetBoundMin/Max read
(min = c - e, max = c + e of middleHigh +0x180, disassembly, all three builds); layer
`kCharController` with the viewer's own group
(`Actor::GetCollisionFilterInfo`, row); any clear = VISIBLE, all blocked = OCCLUDED, other
havok world = OCCLUDED/`kLosWhy_OtherWorld`, cannot cast = UNAVAILABLE (+why), and a pick
that came back with `+0xC0` set (the engine's early-out, nothing cast) = UNAVAILABLE
`kLosWhy_PickSkipped`. Storage: 256 seqlock slots (`Read` lock-free from ANY thread; the
main thread is the only writer), a 64-cell lock-free request ring for unknown pairs,
`askedMs` outside the seqlock payload. `Pump()` (Arbiter::OncePerFrame, after
`mainthread::Pump`) drains the ring, drops pairs unasked for `kLosInterestMs` (2 s),
re-measures <= `kLosMaxPairsPerFrame` (8) pairs older than `kLosRefreshMs` (250 ms), oldest
first. Every 250 ms Pump also asks for the pair of every standing `kCastFlag_OwnLineOfSight`
cast claim and `kTargetPin_OwnLineOfSight` pin claim (`ControlMap::OwnLosCastPairs` /
`OwnLosPinPairs`), so the pair lives as long as the claim, not only while the seat polls (seat
0x06's 2-5 s gaps, past the 2 s interest window, made every poll read UNKNOWN in session 1006c;
fix/apmf-los-false-occlusion). THE HIT FILTER (same fix): each pick carries `LosCollector` in
`bhkPickData` +0xA8 (PickObject's closest-hit collector slot, disassembled on all three builds);
it skips the viewer's own group, every layer NOT in L_SPELL's COLL collides-with set
(`kSpellBlocks`: STAIRHELPER, TRANSPARENT, COLLISIONBOX, CAMERA, ACOUSTIC_SPACE, ACTORZONE,
TRAP_TRIGGER, CLOUDTRAP, PROJECTILE, SPELL, SPELLEXPLOSION, LIVING_AND_DEAD_ACTORS pass) and any
phantom (TRIGGER / GASTRAP volumes) -- never a CHARCONTROLLER hit (marth: actors block). **FRIENDS PASS
(fix/apmf-heal-los-friends, marth 2026-10-12 "Heals see past friends"):** the same rays also yield a second
reading, stored beside the strict one in the slot's seqlock payload (`passWord`, `passFriend`): a
CHARCONTROLLER hit in a FRIEND's group (`FriendsFor`: for a player / player-teammate viewer, the player and every
high-process teammate with a controller -- the teammate half of CastGuard's `Friendly()`; built once per Pump
frame, main thread; never the viewer's or target's group) is stepped past. The collector keeps both nearest hits
(the pass hit is at or beyond the strict one, so earlyOut moves to it); with no friend set the code path and the
strict result are exactly the old ones. `Read(..., a_friendsPass=true)` returns it (+ `Reading::friendBody`, the
friend let through on the clear ray); ONLY CastSeats 0x06/0x07 ask, for a beneficial item. Everything else
(GetLineOfSight, pins, Awareness, ch.25) reads strict. A
nearest hit in the TARGET's own collision group reads as "reached" (that ray is clear). Each
OCCLUDED measurement logs `[los] ... OCCLUDED by -- feet: L_<layer>(n) body|phantom group g [=
the VIEWER's/TARGET's own] ref 0x.. <TYPE> '<name>' [actor: ...] at Xu of Yu; torso ...; head ...`
(per pair 2 s, 5/s overall), names via the LOG-ONLY row `Sightline.TESHavokUtilities.FindCollidableRef`. `MeasureNow` (main, for SenseActor) reuses a < 250 ms verdict and casts for at most
`kLosMaxSyncPairsPerFrame` (8) pairs per frame (Pump advances `g_frame`); past that it answers
from a fresh stored verdict or Unknown (`kAwareDetail_SightDeferred`), so the hard bound is
3 x (8 + 8) rays per frame. `[los]` VISIBLE/OCCLUDED lines are at most one per pair per 2 s
(suppressed ones counted in the heartbeat). Consumers:
`GetLineOfSight` (C-ABI), CastSeats 0x06/0x07 (`kCastFlag_OwnLineOfSight`), TargetPin's
selector seat (`kTargetPin_OwnLineOfSight`), Awareness. `[los]` transition lines, a
rate-limited UNAVAILABLE warn per pair (10 s), a 30 s heartbeat. INI `[Sightline]
bOwnLineOfSight` (default 1). Rows: bhkWorld (0x33), Character (0xC2/0x73/0x74),
`Sightline.Actor.GetCollisionFilterInfo`, `Sightline.TESObjectCELL.GetbhkWorld`,
`Sightline.bhkWorld.WorldScale` (ripref via `AIProcess::KnockExplosion`).
- **LOCK SCOPE (the 2026-09-30 MFO freeze hypothesis, settled by disassembly):** NO lock is
  taken around a ray. `PickObject` read-locks `worldLock` itself on all three builds (AE
  0xE86560 +0x115 -> 0xCC90C0), and `BSReadWriteLock` is reader-preferring (LockForRead
  waits only while a writer HOLDS bit 31; LockForWrite is a bare CAS 0 -> 0x80000001, no
  pending bit), so the old outer read guard was a recursive read that cannot deadlock against
  a waiting writer -- and is redundant. **Never add an outer world lock here, and never hold
  any lock across a ray.**
- **What breaks:** a ray off the main thread (only `Pump`/`MeasureNow` cast, both main);
  any consumer but the 0x06/0x07 BENEFICIAL path reading `a_friendsPass` (offensive casts, ch.25, pins and
  awareness must stay strict -- marth 2026-10-06 / 2026-10-12); building `FriendsFor` off the main thread or
  calling anything from inside the collector (it only compares groups against the prebuilt array). Open review
  item REVIEW-BACKLOG APMF-B74 (diagnostic volume / viewer counters inflated in friends mode);
  a mutex on the `Read` path (the cast and pin seats call it on combat threads and must stay
  lock-free); the collector's layout (`rayHit` at +0x10 is what PickObject resets and copies; slot 0 =
  addRayHit, slot 1 = dtor -- never reorder its virtuals or members before `rayHit`); calling
  FindCollidableRef anywhere but the main thread right after the pick (it is a log name, never
  a verdict input); writing a slot from any thread but main (single-writer seqlock); treating
  UNKNOWN / UNAVAILABLE / a skipped pick as visible (principle 7 -- a cast into a wall);
  raw `now - askedMs` (another thread's later stamp wraps it: use `Since`); widening the
  per-frame budgets without re-deriving the cost (3 rays x pairs per frame, pump + sync); a
  fixed end margin again (big creatures read OCCLUDED by their own capsule); changing the
  ray's samples / layer / margin without saying so in APMF_API.h (MFO mirrors the contract).
  Open review items (6d84bab): `Docs/REVIEW-BACKLOG.md` APMF-B50, APMF-B53 (a)-(c), (e), (f)
  (APMF-B52, SpaceQuery's skipped pick, is drained).

### `native/core/Awareness.{h,cpp}` — ABI v18 AWARENESS, `SenseActor` (read-only)
Omnidirectional multi-sense "does viewer sense target": SIGHT (Sightline own ray, within
sightRange), HEARING (a NEW engine noise -- `HighProcessData::actorsGeneratedDetectionEvent`
+0x3D8, the record only `AIProcess::SetActorsDetectionEvent` writes (SE 38311 / AE 39286 /
1.7.104 0x6F2560), level > 0, within kAwareNoiseWindowMs 3 s, within hearRadius of the
viewer or the anchor; or the target `IsInCombat` (Character 0xE3) within hearRadius),
PROXIMITY (3D), ENGAGED (in combat and `currentCombatTarget` is the viewer or anchor;
`LookupReferenceByHandle` row). Noise NOVELTY, not age: the event's stamp is the engine AI
clock (AE id 404125), which is absent from the 1.7.104 MIT table, so it is never read; a
noise is new when the stamp changed since Harbinger last looked, aged on our own clock; the
first look is a baseline. Main thread only (`hook::OnMainThread`), main-thread maps (no
lock). `[aware] noise` / `[aware] senses` (transition) / heartbeat. INI `[Awareness]
bHearNoise` (default 1). Static asserts pin AIProcess::high +0x10, +0x3D8 and the 0x18-byte
event layout (verified in SetActorsDetectionEvent on all three builds).
- **What breaks:** reading the AI clock global (fatal on 1.7.104: id absent); deciding
  hostility here (the client's call); a call off the main thread doing work; reading the
  event other than as ONE 0x18-byte copy-out. Known exposure: the event object is freed only
  at the high-process teardown (AE 41406), not proven main-thread-only -- the same exposure
  as any main-thread read of another actor's high process. `[aware] senses` lines are at most
  one per pair per 2 s (counted in the heartbeat); the sight sense obeys MeasureNow's per-frame
  cap (`kAwareDetail_SightDeferred`). Open review items (6d84bab): `Docs/REVIEW-BACKLOG.md`
  APMF-B50 (the event free on an untraced path), APMF-B51 (`level > 0`), APMF-B53 (c), (d).

### `native/core/CastClassify.{h,cpp}` — ch.8b SEAT 0: CLASSIFY (2026-09-05)
**HARBINGER 1b CAST SLICE (2026-10-12):** every claim-driven refusal / substitution here is ranked on the Cast facet against the seat's displaced function (`core/FacetAttribution` "CAST SLICE"); never give a claim answer before that test, and never count it on the silent follow-on seats.
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
  other 1.6.x/1.5.x) is refused with a loud line naming the version. F2b (2026-10-05):
  exactly 1.7.104 opens too (ctor `0x832A90`, visitor `0x832D20`, KeepBestRow and the
  classify table identical to 1.6.1170). The REAL guard against a wrong-id resolve is
  the RTTI mangled-name string compare — in 3.7.0 a missing id is never a null resolve
  (`IDDatabase::id2offset` `report_and_fail`s past the end and otherwise `lower_bound`s
  with no equality check off VR, so it silently returns the NEXT id's offset); the
  `vt.address() == 0` test that precedes the RTTI walk is unreachable belt-and-braces
  (Fable tier-3 on c70767c, SEV-4). INI kill-switch `[CastSeats]
  EnableSeat0Classify` (default 1).
  Install ordering vs `core/EquipGate.cpp`/`core/CastSeats.cpp` does not matter
  (disjoint vtable). Runs on the combat thread; the classify path itself does one lock-free
  RCU read (`TryGetCastSeatClaim`) and calls the chained original. `ServeUnclassedHeal` also
  takes the leaf mutex `g_healLogMx` once per served/NOT-served log line (dedup), never across
  an engine call.
  **`ServeUnclassedHeal` EXIT LOGS (apmf-restore-serve-exitlogs):** every early exit of the
  unclassed-heal serve (`NotServed()` + `enum NoServe`, above `ServeUnclassedHeal`) logs
  `[restore-serve] <spell> (<id>) NOT served: <reason> (<values>)` once per (spell, reason)
  under the leaf mutex `g_healLogMx` (set `g_noServeLogged`). Exits 8/9 (NoClaim,
  ClaimNoTarget) dedup per (spell, actor, reason) in `g_noServeActorLogged`, so an unclaimed
  NPC never hides the follower's line. Logging only; the exits and their order are unchanged.
  Exits 1, 3, 4, 5 and 6 fire for every classified spell, so they are claim-gated: each does
  one `attackerHandle.get()` plus one RCU claim read (`TryGetCastSeatClaimForForm`) and logs
  only when a claim drives the spell. Exception: for a loosely heal-shaped effect (beneficial,
  Health, magnitude > 0, any archetype; exits 1, 5, 6) with no live claim, one line per spell
  is logged anyway with `(no live claim on this actor at classify time; ...)` under reason
  tag `|0x80`. So "never classified" is distinguishable from "classified without a claim".
  Exits 3/4 (no effect / base effect) stay gated. Keep any new exit routed through `NotServed`.
- **PLACEMENT SPELLS + ROWLESS ROW SUBSTITUTION (feat/apmf-buff-summon-seats, 2026-10-05).**
  `IsPlacementSpell` (exported): a spell with a SummonCreature or Reanimate effect is never
  self-forced (spell-level; Reanimate's only row is {Reanimate, OTHER}, so forcing self dropped
  it from every row; Summon's rows 14/15 share one creator), and `ControlMap::ApplyRequest`
  resolves a target-0 claim on it to the claimant (APMF-B39's block, call-site edit).
  `ServeRowless` (after the chained visitor, after `ServeUnclassedHeal`): for a CLAIMED driven
  form with a target handle whose effects key NO row with a creator (and none is heal-shaped),
  each effect goes to KEEP-BEST with the engine's own Script row (row 22 beneficial / row 9
  hostile). Script's creator builds a bare 0x20-byte `CombatMagicCasterScript`; its slots read
  nothing archetype-specific (0x0C sets a blackboard timer from the effect's own aiDelayTimer);
  the row scores by the effect's own aiScore and KEEP-BEST's best starts at -1.0. Restore is
  not used for rowless (its 0x07 reads the effect's AV as a restore percent; its 0x0B/0x0C set
  the 15 s MagicRestoreRestrictionTimer). The "keys a row" test replicates the visitor key
  (`kAvKeyedArchetypes`, primary then secondary AV) against `kExpectedRows`, and Install
  compares all 23 LIVE table rows with `kExpectedRows` (+ both Script rows share one creator)
  before enabling it; any mismatch refuses only this part, loudly. **The lookup MAP holds
  every ValueModifier row again under archetypes 4 / 5 / 32 / 34** (map builder AE 0x81DCA0 /
  SE 0x7816C0 / 1.7.104 0x833190) -- so a PeakValueModifier Health self heal (Close Greater
  Wounds) DOES key the Restore row natively, contrary to `ServeUnclassedHeal`'s banner;
  `ServeUnclassedHeal`'s `AlreadyClassed` test only knows arch 0 and so calls KEEP-BEST a second
  time with the SAME row for those (same row kept; adds to the resolver's +0x48 score sum).
  Left unchanged here (not this brief's scope); open for the heal road.
  **What breaks:** feeding a non-claimed spell to `ServeRowless`; deciding "rowless" from
  anything but the replicated key (a native row must always win); adding a row without updating
  `kExpectedRows` (Install then refuses, by design). Open review items: APMF-B58 (b)(c), APMF-B59 (d)(e)(f).

### `native/core/CastSeats.{h,cpp}` — ch.8b: THE ENGINE CAST SEATS (the keystone)
**HARBINGER 1b CAST SLICE (2026-10-12):** every claim-driven refusal / substitution here is ranked on the Cast facet against the seat's displaced function (`core/FacetAttribution` "CAST SLICE"); never give a claim answer before that test, and never count it on the silent follow-on seats.
While a `kIntent_Cast` claim {actor A, spell S, target T} stands, APMF answers the
vfunc seats the combat AI's OWN cast decision is built out of, so the NPC's own AI
selects, equips, charges, aims, fires and channels S at T — engine animation, engine
magicka, engine LOS/interrupts. **APMF makes no `EquipSpell`, `CastSpell`,
`CastSpellImmediate`, `NotifyAnimationGraph` or caster-state write anywhere.**
FOUR seats here, on **TEN caster vtables** since `feat/apmf-buff-summon-seats` (2026-10-05):
Restore, Offensive, Ward, Summon, Cloak, Light, Invisibility, BoundItem, Armor, Script
(`SeatTypes()` in `CastSeats.cpp`), RTTI-verified, each a VerifiedAddresses vtable row on all three
builds with its hooked slot functions listed in `Docs/VERIFIED-ADDRESSES.md`. Never Stagger, Disarm,
TargetEffect, Paralyze or Reanimate. History: Restore only, then + Offensive in v0.9.2
(`feat/offense-seat-scope`, a claimed HOSTILE spell classifies into Offensive); the 2026-10-05 widening
serves claimed self buffs (Oakflesh -> Armor, Candlelight -> Light, wards, cloaks, bound weapons,
Invisibility), buffs at another actor (their delivery-flip proxy keys the same row through seat 0),
summons (Summon) and rowless spells (seat 0's Script substitution). On those types the shared base
0x0A aimed a non-kSelf form at the combat target (the FOE) and 0x06 was the type's own native gate.
**NATIVE PLACEMENT:** a claim whose target is the claimant on a NON-kSelf spell (ControlMap resolves
that only for a summon / reanimate spell, `castclassify::IsPlacementSpell`) keeps the engine's own
placement: 0x0A chains, 0x0D leaves the engine default; 0x06 / 0x07 still answer from the claim.
**COMPOSED WHETHER (round 2, review SEV-2/SEV-3 of 3903988):** on the eight new types 0x06's claim YES
also needs the engine's own redundancy check -- `AlreadyApplied` replicates `0x81E6C0` (target via
GetMagicTarget; Light/Armor/Invisibility/Script/Cloak/Ward) and `0x81E400` (the CASTER; Summon/BoundItem):
"an active effect whose spell IS this magicItem", skipped for concentration (functor MagicCastOnTarget,
`ActiveEffect+0x40`, identical on all three builds; neither id is in the fork's 1.7.104 table, so it is
re-implemented through the fork's members, not called). Light serves only kSelf / kTargetActor delivery
(an aimed / location Light spell leaves no active effect to bound). Ward keeps its own magicka floor
(GMST `fCombatMagicWardMagickaCastLimit`, read by name at install; Ward is NOT seated without it): 0x06
refuses and 0x07 stops below it. `NativePlacement` also requires `IsPlacementSpell` (an explicit self
target on Heal Other must never chain 0x0A to the foe). Open: APMF-B58 / APMF-B59.
**What breaks:** dropping the bound re-casts persistent effects every hand cycle (Light = the 2026-08-19
CTD shape, summons re-summon); seating Light for a non-actor-borne delivery has no bound.
**PROBES (principle 5):** `[ch.8b seats] FIRST claim answer on the <Type> caster, seat 0xNN ...` once
per (type, seat), and `castseats::SeatCountsLine()` (per-type claim-answer counts 0x06/0x07/0x0A/0x0D,
plus native-placement 0x0A calls) in the `[ctcensus] HEARTBEAT`.
- `0x06 CheckStartCast` -> TRUE from the claim (magicka stays enforced by
  `MagicCaster::CheckCast` inside `CastSpell`; hand-idle/`bMLh_Ready` by the leaf's
  own 0x89f3c0; the equip by seat 0x0F). Bypasses only the vanilla health threshold,
  the effect-already-active test and the 15s restrict-timer window — the exact policy
  the claim replaces. **ONLY for a claim with a resolved target handle.** A SELF claim
  (RequestCast with target 0) ON A kSelf-DELIVERY SPELL is resolved to the CLAIMANT's own
  FormID and handle in `ControlMap::ApplyRequest` (the "A SELF CLAIM NAMES THE CLAIMANT"
  block, APMF-B39 CLOSED 2026-09-29), so every seat and EquipGate's 0x0F YES +
  deny-complete serve it like an ally claim. A NON-kSelf spell claimed at target 0 keeps
  target 0 and no handle (as before B39) and logs a rate-limited `not kSelf` warning
  (`SelfClaimLogDue`). Before that it had no handle: the seats chained native and the vanilla
  restore gate decided (AE 45371 / SE 43979: 15 s `MagicRestoreRestrictionTimer`,
  health < lerp(0, 0.5, ctrl)), and 0x0F's deny-complete was off for its hand. Still
  handle-less, on purpose: a deny-only claim and a degenerate `RequestEx(kIntent_Cast)`
  claim (documented as carrying no target). **What breaks:** dropping that block, or
  moving it before the FromPackage extraction or out from under `!denyHandOnly`, reopens
  the self gap or seats a deny-only floor; dropping its kSelf-delivery test hands a
  kAimed / kTargetActor spell the caster's OWN handle, so 0x0A/0x0D aim it at the caster
  (a stuck hand, or a kTargetActor spell applied to the claimant; review F1/F2 of
  `5250fb2`); the proxy mint's `castTarget != op.actor`
  test is what keeps a self claim from minting a delivery-flip proxy. An EXPLICIT self target (the claimant's own
  FormID) on a Touch / Aimed / TargetActor BUFF instead mints a SELF-FLIP proxy (kSelf copy; the block right after
  the delivery flip, feat/apmf-self-delivery-proxy) so no seat aims it at its own caster; hostile and non-placement
  kTargetLocation spells are declined (logged) and keep the old behaviour. Open review items:
  REVIEW-BACKLOG APMF-B41 (the degenerate kIntent_Cast doc sentence). STATUS "Phase 0c". 0x06 and 0x0A also feed the Restore census (`CensusNote`, Restore
  vtable only, read-only; `core/RestoreCensus`).
  **ch.25 (ABI v21):** 0x06 first asks `castguard::HoldStartCast` (claim-driven or native): an OFFENSIVE item of an
  actor whose winning guard carries `kCastGuard_LineOfFire` gets NO unless a <= 250 ms measurement says the line is
  clear (`core/CastGuard`).
  **ABI v18 `kCastFlag_OwnLineOfSight` (`OwnLosApplies`: the bit, not deny-only, target not 0
  and not the claimant):** 0x06 answers YES only on a fresh (<= 1 s) own-ray VISIBLE from
  `sightline::Read(actor, claim.target)` (lock-free, FormIDs, marks the pair asked-for) and NO
  otherwise (OCCLUDED / UNKNOWN / UNAVAILABLE / UNSUPPORTED; the census sees `false`); 0x07
  STOPS a channel on two consecutive OCCLUDED readings (occRun >= 2, newest <= 1 s), after
  the TTL / target checks; UNKNOWN / UNAVAILABLE never stop. **BENEFICIAL casts (fix/apmf-heal-los-friends):**
  when `OwnLosFriendsPass(a_this)` (no effect of `a_this->magicItem` is hostile, any delivery -- NOT ch.25's
  `Offensive()`, which exempts Self delivery) both seats read the FRIENDS-PASS verdict (player / teammate capsules
  do not block; core/Sightline); `passFriend` is stored only when the strict reading was not visible (the friend
  changed the verdict). `NoteFriendPassed` logs once per (actor, driven, target, friend) -- not expiresMs, which
  client Repoint heartbeats move -- the friend looked past, the name
  resolved in a `mainthread::Post` (never a form lookup on the combat thread). Offensive casts read strict. **What breaks:** reading the
  verdict any other way than `Read` (a lock or a form lookup on the combat thread); letting
  a non-VISIBLE verdict answer YES. The bit only means anything on RequestCast (a target): the
  degenerate `RequestEx(kIntent_Cast)` form has no target, so `ControlMap::EnqueueRequest`
  REFUSES the bit there by name (review SEV-3, 6d84bab) -- never let it through as a no-op.
  Every seated caster type carries these seats (ten since feat/apmf-buff-summon-seats; APMF-B53 (e)'s
  "Restore and Offensive only" wording predates that).
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
  (0x81e020) is the BASE, shared by 14 of the 15 caster vtables (Reanimate overrides it).
  Adding Stagger / Disarm / TargetEffect / Paralyze to the install list would let a claim aim
  their HOSTILE effects; adding Reanimate would replace its corpse targeting (its own 0x06
  corpse search feeds its own 0x0A / 0x0D). Removing `NativePlacement` aims a summon at its
  own caster. (2026-10-05: the list is ten types, see above.) (Corrected 2026-09-07: this note used to say "beyond Restore",
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
  + helper `0x782100`). Do not add a version gate to either construct; do re-cite the
  table if the shape is ever re-derived. G1 (2026-10-04): the whole `castseats::Install`
  is exact-version gated (`allowance::RuntimeSupported()`) because its slot indices are
  per-runtime facts, so these constructs are never reached off 1.6.1170 / 1.5.97 / 1.7.104
  (F2b 2026-10-05: aim ctor `0x809EE0` and GetMagicTarget `0x833510` identical there).
  Installed AFTER `core/AiCastSeats.cpp` on purpose so that passive probe keeps
  logging the ENGINE's raw answer beneath these.

### `native/core/AiCastSeats.{h,cpp}` — OBSERVE-ONLY: the AI cast/equip-decision seats
Three independently-flagged, chain-unconditionally probes (`Install()`), all
`[AiCastSeats]`-INI-gated, all refused off exactly 1.6.1170 / 1.5.97 / 1.7.104 (VR included; G1
2026-10-04, `allowance::RuntimeSupported()` at the top of `Install()`): **GROUP A** — `CalculateScore` (0x0C) on
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
immune to the `GetMagicTarget` sret-ABI bug class this file's banner documents).
**ABI v22 (2026-10-12, weighting phase 1): both 0x0C thunks hand the engine's answer to
`equippref::OnScore` (core/EquipPreference.cpp)**, the ONE score lift: a ch.26 preferred item
(GROUP C weapons / shields / torches, GROUP A staves) or the form a ch.8b claim drives on its hand
(GROUP A) returns one step above the best competitor in its engine category. `kScoreSteerBias`
(+1000) and `SteerScoreForCastClaim` are DELETED; `EnableScoreSteer` defaults to 1 (marth D5) and
`Install()` ends with `equippref::Arm(...)`. GROUP C is no longer observe-only for a ch.26 item;
the WEAPON-SCORE line names a lift after `engineScore=`. The history below is kept as written.
A second, independent flag (`EnableScoreSteer`, default 0 until ABI v22) biases a claimed form's own
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
  build would pass a family test). F2b (2026-10-05): exactly 1.7.104 opens too, with its
  own expected values (`calcScoreRva17`: `0x82D2C0/0x82D790/0x82DCD0/0x82E360`, each the
  1.6.1170 body). **The REAL guard against a wrong-id resolve on a build that DOES
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
  **CORRECTED 2026-10-06:** 0x0F IS hookable on the weapon leaves, and `core/HandBlock.cpp` now
  denies weapon admission there into a hand a live cast claim holds (gap 10 closed; see that
  file's entry). It installs BEFORE this file, so the Ranged probe and Shield TASK2 wrap it.
  `kScoreSteerBias` is **1000.0f** (`AiCastSeats.cpp:501`), right-sized 2026-09-06 to
  measured magnitudes (Falmer War Axe 194, bow 81, magic 0.08-29) — it replaced an
  unmeasured 100000.0f placeholder, which this line quoted until 2026-09-07. And the
  bias no longer applies HERE at all: after F5 it belongs to the SPELL/STAFF seat
  (`SteerScoreForCastClaim`, `AiCastSeats.cpp:523-551`), driven by a ch.8b cast claim
  on the item's own hand. GROUP C keeps its observe-only probe and no steer.
  **[Probe] bRangedSelect (2026-09-29, phase 1 of the Ranged-selection deny, default 0 since v0.9.10, set 1
  for a field cycle, STATUS PRE-RELEASE CHECKLIST):** `RangedEquipProbeThunk` observes `CheckShouldEquip` (0x0F) on the
  RANGED vtable only, installed inside the GROUP C loop after that vtable's 0x0C identity check (so it needs
  `EnableWeaponScoreProbe=1`). Calls the original first and returns it unmodified; for a ch.17 claimant it
  logs the engine answer, the call site (per-runtime return-address labels `kRetAE`/`kRetSE`: pre-loop 44868 /
  43637, evaluate 44899 / 43666 passes set118/set148) and the sink verdict from
  `actiongate::RangedProbeClassify`. The slot's value is compared (log only) with the disassembled base
  `!IsFleeing` (AE 0x817FC0 / SE 0x77DC90, shared by Melee/Ranged/Shield); a foreign hook is chained, a null
  slot refused. Counters feed `actiongate::RangedProbeHeartbeat` via `RangedGateProbeCounts()`.
  - **What breaks:** this is the seat a phase-2 deny would answer NO at; it is observe-only now. Never
    install it on the Melee or Shield vtable from this block (same base function, different facet). The
    spec row `CombatInventoryItemRanged` lists slot 0x0F; the call-site labels are cosmetic, exact-version.
    Open review items: REVIEW-BACKLOG APMF-B36 (F2 = phase-2 prerequisite: refuse the deny unless the slot
    holds the base).

### `native/core/CasterTypeCensus.{h,cpp}` — PASSIVE caster-type census, `[ctcensus]` (2026-09-29)
**COMBAT SUBSTRATE (2026-10-13):** the 0x0B `NotifyStartCastThunk` also calls `combatsubstrate::NoteCastFired`
(one relaxed load when no substrate is open): the substrate's FIRE close rides this probe's seat, so
`[Probe] bCasterTypeCensus=0` leaves a substrate closing on its claim's end / TTL only (logged at OPEN,
`NotifyInstalled()`). Resetting the probe default to 0 before a release must keep this dependency in mind.
Answers design Q-L by observation: for every live DRIVING `kIntent_Cast` claim, which of the 15
`CombatMagicCaster` types the engine built for the driven form, whether it fired it, and the ZERO case.
Two observe-only seats on all 15 caster vtables (`CasterVtables()`, incl. Armor): **0x06 CheckStartCast**
(chain first, log the returned answer: "BUILT") and **0x0B NotifyStartCast** (void, observe then chain:
"FIRED", the engine's release from the hand; AE callers 0x89F2B0 / 0x89FEB0 / 0x8A2369, concentration
0x89EFB8; SE 0x808A30). Game-thread `Poll()` (Arbiter, after `nonaliasprobe`) opens one window per
(actor, hand, driven, target), reads the claimed hand's MagicCaster state and the kInstant caster, labels
the window with its PREDICTED classify row (`kRows`, the engine's 23-row table decoded from AE 0x20163B0 /
SE 0x1DF2B00, identical; `kAvArchetypes` from the flag table AE 0x1FD3028 / SE 0x1DB0028), prints
OPEN / BUILT / FIRED / OTHER / HAND / INSTANT / ZERO (3, 10, 30 s) / CLOSE (verdict) / SERIES (back-to-back
windows, e.g. an AUTO fan) and a 60 s HEARTBEAT with engine-wide per-type fire counts (every actor).
`core/CastObserve.cpp`'s anim sink forwards BeginCast/SpellFire tags (`NoteAnimEvent`).
`[Probe] bCasterTypeCensus` (code default 1, PRE-RELEASE CHECKLIST in STATUS). Installed after
`castseats::Install` (outermost on 0x06). Exact-version gate (1.6.1170 / 1.5.97 / 1.7.104), VR refused.
- **What breaks:** the thunks must never alter the chain's answer or skip `orig`. `g_mx` is a leaf lock:
  never log or call the engine under it (lines are collected, logged after). `kRows` is a LABEL only;
  never branch on it. The Armor caster vtable IS a real `CombatMagicCaster` on both runtimes (RTTI
  hierarchy walk; ENGINE_NOTES §0.28 does not hold for these binaries) and has its own VerifiedAddresses
  row since this change; the `DerivesFrom` walk at install stays the guard. A type that failed to install
  reads as zero: the install line names it. Never call `Actor::GetMagicCaster` here (it allocates on a
  null slot): read `magicCasters[]` directly. Open review items: `Docs/REVIEW-BACKLOG.md` APMF-B40.
  2026-10-05 (feat/apmf-buff-summon-seats): `LookupRow` knows the map's ValueModifier aliases
  (archetypes 4/5/32/34), `PredictRows` skips the self flip for placement spells and appends
  ` sub[Script]` when seat 0's rowless substitution applies, and the HEARTBEAT carries
  `claim seats answered 0x06/0x07/0x0A/0x0D [...]` from `castseats::SeatCountsLine()`. Labels
  only; never branch on them.

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
**HARBINGER 1b CAST SLICE (2026-10-12):** every claim-driven refusal / substitution here is ranked on the Cast facet against the seat's displaced function (`core/FacetAttribution` "CAST SLICE"); never give a claim answer before that test, and never count it on the silent follow-on seats.
Hooks `MagicCaster::CheckCast` (vtable slot **0x0A**) on `VTABLE_ActorMagicCaster[0]`
ONLY (the other two ActorMagicCaster vtable entries are base-subobject vtables of
the same class, not separate casters — patching them clobbers unrelated engine
vtables). Resolver = `MagicCaster::GetCasterAsActor` (an ordinary virtual call
through the object's own unhooked slot 0x0C). Denies any spell whose FormID
isn't the winning `kIntent_SelectSpell` claim's `param.form`; sets
`CannotCastReason::kMultipleCast` on deny. Refused off exactly 1.6.1170 / 1.5.97 / 1.7.104 (VR
included; G1 2026-10-04), install-once
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
  **INSTANT CASTER (INVARIANTS #18 amendment, 2026-09-29, `instantCaster` in
  `CheckCastThunk`):** for `kInstant` ONLY the ch.8b read is skipped
  (`castAllows = true`); ch.8 select and the H1 positive override still apply. Do NOT
  route it back through `AllowedCastForHand(kUnknown)` — that actor-wide floor refused
  a client's own direct casts, the AI's potions, and cut a running instant-caster
  concentration channel (the engine re-runs 0x0A every channel tick and calls
  `InterruptCast(true)` on NO: AE 34407 / SE 33629). **Do NOT exempt the voice caster
  (`kOther`)** (review F1, DECIDED): a busy voice caster makes a hand's CheckCast return
  `kCastWhileShouting` (AE `0x5B19B6`), so an unclaimed shout would block the claimed hand
  from starting; it stays on the actor-wide floor. The deny line prints
  `hand=?(instant)` with ch.8b `n/a`, or `hand=?(other)` with the floor's answer;
  `CheckCast ADMITTED ... on the instant caster` marks a cast the pre-amendment read would
  have refused. EquipGate keeps the `kUnknown` actor-wide floor for its neither-hand slot.
  **ch.25 (ABI v21):** on a HAND caster the thunk asks `castguard::DenyCheckCast` right after resolving the hand and
  BEFORE the H1 override; a YES refuses with `kMultipleCast` (castobs `NoteCheckCast`, byApmf). `castgate::Installed()`
  (true once the thunk is written) gates every guard claim. See `core/CastGuard`.

### `native/core/CastGuard.{h,cpp}` + `native/channels/CastGuard.cpp` — ch.25 CAST GUARD (ABI v21, 2026-10-12)
`kIntent_CastGuard`: two per-actor guards on an NPC's OWN casts, moved under Harbinger from MFO's
CasterConsent hooks (MFO branch `fix/mfo-gates-client-only`): `kCastGuard_LineOfFire` (the friendly-fire hold)
and `kCastGuard_ChannelRight` / `kCastGuard_ChannelLeft` (MFO's Exact concentration bound, per hand). NO hook of
its own and no engine address beyond the verified `SpaceQuery.IsHostileToActor` row: it rides
`core/CastGate.cpp` 0x0A (`castguard::DenyCheckCast`, HAND casters only, called right after the hand is
resolved and BEFORE the H1 positive override, so claim-driven offensive casts are held too) and
`core/CastSeats.cpp` 0x06 (`castguard::HoldStartCast`, first thing after the original is recovered, claim or not).
The channel (`channels/CastGuard.cpp`, ch.25) only logs Engage / OnOwnerChanged / Release. The claim is bounded:
`ControlMap::ApplyRequest` grants `castguard::TtlMs(param.fval)` (seconds; 0 = `kCastDefaultTtlMs`, clamp
`kCastMaxTtlMs`) and the generic Repoint renewal keeps it; `EnqueueRequest` refuses by `castguard::RefuseReason`
(CheckCast gate not installed, no / unknown / zero bits, bad fval, the player, LineOfFire while unarmed).
The two ControlMap readers `TryGetCastGuard` / `CastGuardActors` are DEFINED in `core/CastGuard.cpp` (ControlMap.cpp
is near its size cap): best LIVE claim by basis, tie earliest, bits masked to `kCastGuard_All`.
THE MEASUREMENT: `castguard::Pump` (Arbiter::OncePerFrame, after `sightline::Pump`, main thread) refreshes the seats'
`g_anyGuard` pre-gate from the snapshot, then measures up to 8 guarded LineOfFire casters per frame (round robin):
the aim (the live driving cast claim's target when that claim's spell is offensive, else `currentCombatTarget`),
then every high actor + the player in the caster's worldspace / cell: within `kLinePad` (80u) of the
caster-to-aim segment (projecting between) and not read OCCLUDED from the caster by `sightline::Read` (asked, so the
pair stays measured; unknown counts), or nearest to the aim (within 4096u, for the seat's `area x kUnitsPerFoot + kBlastPad`
test: EFIT area is in FEET, 64/3 u per foot, review SEV-2 of d2d591d). A Repoint is refused by the same `RefuseReason`.
Friendly = `!other->IsHostileToActor(caster)`, plus the player and teammates for a teammate caster. Published per
caster in a 128-slot SEQLOCK table (main writes; readers lock-free); slots of casters no longer guarded are emptied.
Seats trust a measurement for `kFreshMs` (250 ms): 0x06 answers NO unless fresh and clear; 0x0A refuses only on a
fresh BLOCKED reading. The channel bound refuses a concentration `SpellItem` on a bounded hand at 0x0A unless
`allowance::CastClaimNamesForHand` admits it. Every would-be refusal goes through `Decide`: Engine on the Cast facet
gets a hit and an overruled (refusal stands: the guard's client priority > Engine's) or a stood (allowed, logged).
Logs: `[ch.25] ... REFUSED / would refuse, ALLOWED ...` (per actor, spell, reason, 1.5 s), a 30 s `[ch.25] HEARTBEAT`.
Evidence that 0x0A covers the RELEASE: `ActorMagicCaster` slot 3 `RequestCastImpl` and slot 6 `StartCastImpl` both
`call [vt+0x50]` on 1.6.1170 (`0x5B0840` / `0x5B0A90`), 1.5.97 (`0x541A70` / `0x541CB0`) and 1.7.104 (`0x5BF370` /
`0x5BF5C0`); on NO, slot 6 runs the cannot-cast handler (AE `0x5B1C20`), which interrupts.
- **What breaks:** a refusal at 0x0A on an UNKNOWN measurement (it would interrupt a release or a channel tick on
  no evidence); 0x06 answering clear by fallback (principle 7); moving the 0x0A consult after the H1 override (claim-
  driven offensive casts would escape the line of fire) or onto the instant / voice caster (the client's direct
  stream and shouts are out of scope by contract); a lock or a form lookup on the seat path (the seats read FormIDs,
  the seqlock and one RCU snapshot; `Decide`'s `ClaimPriority` shared lock is taken only on a would-be refusal); a
  ray or an actor walk off the main thread (Measure is main only; `sightline::Read` is the lock-free ask); dropping
  the `Decide` priority test (the controller's degrade rule: enforce only against an outranked source); a measurement
  slot outliving its caster's guard (Pump empties it); adding a decay or let-one-through in Harbinger (client
  policy, INVARIANTS #18 ch.25 (e)); reading EFIT area as units (it is feet). Open review items: REVIEW-BACKLOG
  APMF-B70 (hostility over-holds aggroed non-faction-hostile combatants; self area spells unguarded; other-hand aim
  at 0x0A; log-throttle map never cleared).

### `native/core/EquipPreference.{h,cpp}` + `native/channels/EquipPreference.cpp` — ch.26 EQUIP PREFERENCE (ABI v22, 2026-10-12)
The weighting rework, phase 1 (MFO `Docs/WEIGHTING-INVENTORY.md` "PHASE 1 PLAN", marth D1-D5). `kIntent_EquipPreference`:
a client DECLARES preferred items per hand (`SetEquipPreference`, `APMF_EquipPrefEntry{form, slot, rank}`); in
combat the engine's own `CalculateScore` (0x0C) answer for a declared item is LIFTED, never replaced. NO new seat and
no new address: `OnScore` is called by both `core/AiCastSeats.cpp` 0x0C thunks (GROUP C weapon classes, GROUP A the
30 spell/staff items), which chain to the engine first. The same function is the ch.8b spell steer (W2): the form a
live cast claim drives on the item's hand is lifted the same way (rank 0, ranked on the Cast facet).
**THE LIFT (D1):** `max(engine, best * (1 + kEquipPrefStepRel) + kEquipPrefStepAbs)` (5 % + 0.01). `best` = the
highest `itemScore` (+0x18) the engine holds for another item of `inventoryItems[GetCategory()]`, read IN PLACE
(the caller is that list's rescore loop, AE 0x80F380 / SE 0x777A50 / 1.7.104 0x824260; the look-alike loop at AE
0x811A5E is a DEAD twin with zero callers, MFO ENGINE_NOTES §0.43); items this actor's preferences or cast claims lift at an equal or better rank are left out (no
chase). Layout read: `CombatController+0x10` inventory, `CombatInventory+0` parentController (checked == the
call's controller), `+8 + cat*0x18` the lists (size at +0x10), item `+0x18` score, vfunc 0x0B `GetCategory`
= `int(this)`, a leaf on every concrete item class of all three builds; all static_asserted where CommonLib
declares them and disassembled 2026-10-12 (file header). 5 % comes from the LoreRim 10-12 log: 3599 same-item
rescores two passes apart rose at most 3.94 % (scores >= 1), and a rival not yet rescored this pass may still
rise by that much. **NEVER LIFTED:** engine score <= 0 (a bow with no arrows); a declared item for a hand a live
cast claim holds (`handblock::HeldFor`, D3); any item while the source beneath the seat
(`facetattr::InsideSource(orig)`) is not outranked (`facetattr::Outranks`, Equip facet; Cast facet for the
steer: D5); a list that fails validation (`Unreadable`). **FORCE (D4):** `kEquipPref_Force` in the claim's
`param.ival` (Repoint) makes `core/EquipSink.cpp` refuse a governed engine equip into a hand the claim prefers
something for, unless it is one of that hand's items (`ForcedFor` / `ForceRefusal`), ranked by `RankEquipDeny`;
a refusal the sliders do not allow is a FORCE ANOMALY (warn line, `NoteForceAnomaly`, menu + heartbeat +
`APMF_EquipPrefInfo::forceAnomalies`). Harbinger equips nothing for a preference. Refusal (`ControlMap::
EnqueueRequest` -> `RefuseReason`): actor 0, the player, before `Arm`, `EnableScoreSteer=0`, no score seat.
Declarations (review of 7cda290, F4: the header's contract): `ClientAPI` Request/RequestEx of intent 26 calls
`NoteClaim(h)`; `Set` is a silent no-op for any other handle and only QUEUES (`g_pending`); `Pump` (main thread)
`ApplyPending` type-checks every entry (`LookupByID` + `Categorize` hand bits: a weapon, staff, shield or torch;
a spell, body armor, ammo or unknown form is IGNORED and named once in that declaration's line) and publishes a
copy-on-write `unordered_map<Handle, Decl>` through `atomic<shared_ptr>` (writers under `g_writeMx`; the seats
take no lock on it). A declaration is dropped when its claim ends by ANY road: `ClientAPI` Release -> `Forget`;
anything else (Harbinger releasing it, the Drain sweep of an unloaded actor) -> `Prune`, once a second on the
main thread, asks `ControlMap::IsClaimLive` per tracked handle and drops one that was live and is not, or was
never live 10 s after its request; `plugin.cpp` OnRevert calls `ResetAll`. F7: `BestRival` notes whether the
list's FIRST item (the previous pass's winner, the list is re-sorted descending only after the loop) is a
competitor; a scoring of an item that was LIFTED last time with a competitor on top counts a lost pass
(heartbeat, menu; no ABI field). F1: the sink's force step gives the claim's own client a separate `own client`
verdict (info line, `NoteForceOwnClient`), never a FORCE ANOMALY. Per-actor state (`GetState`) + the `[ch.26]` line dedup sit under one LEAF mutex taken after
every engine read. The channel (ch.26) only logs Engage / OnOwnerChanged / Release (a force escalation at warn).
Logs: `[ch.26] <actor> LIFTED / leading / OUTRANKED ... item= hand= cat= rank= engineScore= best-competitor=
-> result (claim h=)` (on change, 15 s floor), `[ch.26] HEARTBEAT` (30 s, warn on a new anomaly), the
WEAPON-SCORE / SCORE-STEER lines name a lift; the Equip facet header on the Harbinger page shows the counters.
- **What breaks:** an additive constant (D1: the wrong shape on LoreRim, 0.22 to 1320); lifting a 0 score;
  counting a lifted item as a rival (two preferences chase each other upward every pass); reading the
  category list off the rescore thread or without the parentController check; a lock held across
  `GetCategory` / the list walk; dropping the `Outranks` test (D2/D5: the lift is a deny of the answer beneath
  the seat and ranks like one); the force step refusing APMF's own enforce equip or the player's menu equip;
  a force refusal applied when the source outranks the claim (that is the anomaly, never a deny); keying the
  rival on the item FORM only (per-hand clones of a one-hander are distinct items); widening `kMaxEquipPreference`
  without an ABI bump. Scroll / potion / shout items are visible as RIVALS (read from the engine list) but
  are never lifted (their 0x0C is not seated). Publishing a declaration off the main thread (the type check
  does a form lookup). **Open review items: REVIEW-BACKLOG APMF-B75 (RankOf excludes items that will not
  actually be lifted), APMF-B76 (CastClaimHandle ranks with the wrong client when two clients hold different
  hands), APMF-B77 (BestRival does not verify `a_this` is in its own list), APMF-B78 (a slot-less left-hand
  preference passes ForceRefusal; the ~2 s force delay is not enforced) -- read them before editing.**

### `native/core/RestoreCensus.{h,cpp}` — phase 0b Restore-caster census (PASSIVE probe)
Read-only. `[Probe] bRestoreCensus` (code default 1 for the field; STATUS pre-release
checklist). Two feeds: `NoteRestoreSeat` from `core/CastSeats.cpp` `CensusNote` at seats
0x06/0x0A, **Restore caster vtable only** (combat thread, one leaf lock, never an engine
call under it); `Poll` from `Arbiter::OncePerFrame` after `castobserve::Poll` (game
thread, ~200 ms, engine reads first with no lock). A window = (actor, claimed hand, driven
form) of a live, driving, restore-shaped `kIntent_Cast` claim (costliest effect a
non-hostile Value/PeakValue/DualValue modifier on Health/Magicka/Stamina). Lines:
`[census] ... OPEN`, `ENGAGE`, `STILL ZERO` (2/5/10/20/40 s), `CLOSE ... VERDICT`,
`HEARTBEAT` (60 s, always). Budget 30 lines / 10 s (heartbeat exempt).
- **What breaks:** `CensusNote` must never change a seat's answer or its arguments. At
  0x06 it runs after the answer is decided (the claim path is always YES; the native path
  records `orig`'s value). At 0x0A it runs BEFORE `orig` and only reads `this`/`cc`
  (review F3, APMF-B37). It filters on `g_restoreVtable` so the Offensive caster is never
  counted. "Caster seen" means first seen at a seat (construction is NOT hooked). Hand
  state is read from `GetActorRuntimeData().magicCasters[src]`, NEVER
  `Actor::GetMagicCaster`, which allocates a caster when the slot is null (review F2; the
  same fix is in `core/CastObserve.cpp`). A self claim on a kSelf spell now carries the claimant's own
  `targetHandle` (APMF-B39, closed 2026-09-29), so its window should ENGAGE with `claim
  YES` like an ally's; a self window with `claim YES=0` after that fix is a regression,
  not the phase-0c finding. Census `self` = target 0 OR target == the actor, so both the
  old and the new shape read as self. Review items: REVIEW-BACKLOG APMF-B37..B39 (all closed), APMF-B41/B42 (self-claim review).

### `native/core/EquipGate.cpp` — T2a: CheckShouldEquip allowance (per-item equip gate)
**HARBINGER 1b CAST SLICE (2026-10-12):** every claim-driven refusal / substitution here is ranked on the Cast facet against the seat's displaced function (`core/FacetAttribution` "CAST SLICE"); never give a claim answer before that test, and never count it on the silent follow-on seats.
Hooks `CombatInventoryItem::CheckShouldEquip` (vtable slot **0x0F**) on the 30
concrete spell/staff `CombatInventoryItemMagicT<item,caster>` instantiations —
the identical set MFO's own `CombatStyle.cpp` equip gate patches (mirrored here
APMF-side, mutex-free). Resolver = `CombatController::attackerHandle` @0x28
(`static_assert`s pin it `<0x68`, AE-safe). Denies any spell/staff item whose
FormID isn't the winning `kIntent_SelectSpell` claim's `param.form`.
Refused off exactly 1.6.1170 / 1.5.97 / 1.7.104 (VR included; G1 2026-10-04), install-once.
- **What breaks:** deliberately NOT the design doc's aspirational 87-vtable /
  Melee-Ranged-Shield-Torch count — verified 2026-09-02 against the pinned
  upstream that those 4 categories have NO CONCRETE C++ CLASS in the pinned
  CommonLibSSE-NG headers (CORRECTED 2026-09-15: the VTABLE symbols DO exist —
  `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}`, now used by
  `core/AiCastSeats.cpp` GROUP C; what is missing is the C++ class, so 0x0F's
  per-class semantics on those leaves remain unverified and this gate still does
  not hook them; 2026-10-06: `core/HandBlock.cpp` hooks 0x0F on those four leaves for the
  hand-claim block, with the slot functions read per build), and
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
  per-instantiation function on both (and on 1.7.104, F2b); the hand slots 19/20 are
  read through the fork's DOBJ `GetObject` since F2b (init flags +0xB90 / +0xB80 /
  +0xBC0 per build); the `CallSiteName` 0x0F call-site label table (`0x80fcd0`
  pre-loop, `0x813af2/0x813d38/0x814270/0x8144b2` selector; 1.7.104 `0x824bb0`,
  `0x8289d2/0x828c18/0x829150/0x829392`) is consulted on those two builds only — on
  any other binary the live RVA still prints, labelled "unlabelled". Open:
  REVIEW-BACKLOG APMF-B46 (those literals are call sites, not return addresses, so
  they never match; log labels only).
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
**COMBAT SUBSTRATE (2026-10-13, S2a, #0 (j)):** `ActThunk` consults `combatsubstrate::ForLeaf` FIRST (after the
observe-only probes, before ch.23 and ch.7) when `combatsubstrate::AnyLive()`: a leaf of a holder under its substrate
that is not one of the cast leaves (or a structural node) is force-failed with the SAME ForceFail act()/pop()
pair, then `LeafDenied` counts it. `g_leafIdx` (vt -> 0..69 kLeaves / 70..73 kSearchLeaves / 74..94 kParamLeaves, only
paired nodes) is built at the end of `Install()`, which also installs act + pop on the 21 PARAMETERISED nodes
(`core/CombatBehaviorRE.h` `kParamLeaves`, review F1 of a512763) when `[CombatSubstrate] bCombatSubstrate=1`;
`SubstrateLeavesPaired()` must read 95 (every concrete NodeObject class on both runtimes; of the 21, 12 denied, 2 cast-gated, 7 pass incl. the two Idle waits, round 2) or the substrate refuses
(so ch.23's search-leaf install, `[PursuitLeash] bPursuitLeash`, is a substrate dependency). **What breaks:** never move the substrate
check after ch.7 / ch.23 (the allow-list must be whole); never deny without arming `t_pending` first.
**HARBINGER 1b (2026-10-12):** every ch.7 deny (`ActThunk`) and ch.23 leash deny / end (`LeashAct`, `UpdateThunk`) is
decided by `ClaimOutranksTree` (`core/FacetAttribution` `Decide`, source Engine, facet CombatAction) after every
mechanism check: it enforces only when the claim (now read with `TryGetOwningClaimBasis` for its handle) outranks
Engine; otherwise the leaf runs (orig) and a `[ch.7]` / `[ch.23]` "leaf RUNS" line is logged (5 s per actor).
`Install()` (kDataLoaded, refused off exactly 1.6.1170 / 1.5.97 / 1.7.104 with VR included (G1
2026-10-04), install-once) hooks the 70
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
**ch.23 PURSUIT LEASH (ABI v16, `kIntent_PursuitLeash`) lives here too, on its own intent.**
14 leash leaves (`g_leashLeaves`): the ten pursuit leaves (`kPursuitLeafNames`, goal = the
combat target via the resolving controller's `targetHandle`) and the four search leaves
(`apmf::cbt::kSearchLeaves`, goal = the combat group's search centre, `CombatGroup+0x100`,
valid when `+0x110` is non-null -- the engine's own unlocked read). They carry NO ch.7
category; `ActThunk` routes them (`g_pursuitIdx`) to `LeashAct` BEFORE any ch.7 logic, which
reads the ch.23 winner only. `PursuitDenies()` = the per-actor leash (`g_leash`, anchor
HANDLE + radius, written on the game thread by `SetLeash`/`ClearLeash` from
`channels/PursuitLeash.cpp`, dropped by `ResetLeash` at load/revert) and denies only while
actor->anchor > radius and goal->anchor > actor->anchor (same cell/worldspace, live anchor).
Two seats: act() = the ForceFail pair (act/pop installed on the four search vtables by
Install, same thunks, same maps); slot **0x04** `UpdateThunk` on the 14 leaves ends an
already-running leaf with the engine's `SetFailed(thread,1)` + `Ascend(thread)` (verified rows
`ActionGate.Pursuit.SetFailed/Ascend`) after checking `cur_node == this && phase == 1`. Arms only
with INI `[PursuitLeash] bPursuitLeash`, the ForceFail pair, both rows and act+pop+update on all
14 (`PursuitArmed()`; ControlMap refuses ch.23 otherwise). `PursuitHeartbeat()` (Arbiter, ~30 s,
zeros included) prints per-leaf seen / denied-at-act / ended-at-update and the pass reasons.
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
  **ch.23 leash -- what breaks:** the update() half must END a running leaf only through the
  engine's `SetFailed` + `Ascend` pair, never a raw field write, never SetFailed alone, never a
  push/pop of its own (the node's OWN pop() must run after it -- INVARIANTS #18 "the update()
  half"); keep the `cur_node`/`phase` check in front of it. Never arm with only the act() half.
  Never give a leash leaf a ch.7 category, and never let ch.7's mask decide a leash leaf (ch.23
  is its own facet). The seats never look up a form: the anchor is a handle resolved on the
  game thread. The target distance comes from the SAME controller that resolved the actor
  (`ResolveController`). Open items: DENY-COMPLETENESS-AUDIT gap 14; REVIEW-BACKLOG APMF-B32.
  **HELD ENGAGEMENT + LEASH RETURN (2026-10-12, INVARIANTS #0 (i) condition 4):** `PursuitDenies` PASSES a pursuit leaf
  whose goal (`targetHandle`) is the actor's held target and a search leaf whose group searches for it (`CombatGroup+0xFC`,
  `cbt::kGroupTargetToSearchFor`) when `HeldOutranksLeash` (`ActionGate.cpp:526`, review F-3) finds the engagement claim's
  `ClaimPriority` on CombatAction >= the leash claim's (tie = engagement), counted `held-engagement=` / `leash outranked`.
  `LeashAct` / `UpdateThunk` stamp per leashed actor when a search leaf ran / was denied (`StampSearch`, `:311`; read by
  `SearchSeenAgeMs` / `SearchDeniedAgeMs` `:1658` for the hold's search-elsewhere drive); `LeashReturnPoll` (`:1673`,
  Arbiter, 250 ms, `[PursuitLeash] bLeashReturn`) gives a leashed, UN-held, in-combat actor whose search was leash-denied
  in the last 1.5 s a ch.24 internal bound (X = anchor, R = radius) until it has not searched for 3 s, holds an
  engagement, leaves combat or loses the leash; a return not carried for 3 s is RELEASED with a warning and not retried
  for 10 s (review F-6). **What breaks:** never exempt a goal that is not the held target or against a leash that
  outranks the engagement; never give a held actor a leash return (the hold drives it to its held target instead).
  **[Probe] bRangedSelect (2026-09-29, PASSIVE, default 0 since v0.9.10):** rides the existing
  act()/pop() thunks, no seat of its own (`RangedProbeInstall`, the RANGED-SELECT PROBE section above
  `ActThunk`). In `ActThunk`, BEFORE any deny: for a ch.17 claimant (`ControlMap::TryGetEquipSet`) it counts
  the leaf (only while the claim owns Right or Left) and, for `EquipObject` / `EquipRangedWeapon`, reads the
  context item (`thread+0x128` window, `+0x130` offset, CombatInventoryItem* at context+8 -- the bytes AE
  48124/48126 and SE 46955/46957 read) and logs it with `RangedProbeClassify`'s sink verdict (the sink's steps
  2-6 over `equipsink::Categorize`). In `PopThunk`, on the normal path only, for `CheckUnreachableTarget`: the
  thread state (`+0x148`) before the node's own pop() = its outcome. `RangedProbeHeartbeat` (Arbiter, game
  thread) prints per-actor leaf summaries ~15 s and a RULE C line ~30 s.
  - **What breaks:** the observer must stay read-only and must never sit on the denied-pair path of
    `PopThunk` (the pending ForceFail pop returns before it). `g_rsLeafIdx` and the three leaf vtables are
    written once in `RangedProbeInstall` and published by the `g_rsEnabled` release store; keep the thunks'
    acquire loads. The context offsets are measured, not CommonLib's; re-measure before reusing them.
    The window is read ONLY when the leaf's own attack-state test would read it (`RsEquipLeafWouldReadWindow`,
    review F1: AE 48124/48126 `cmp [actor+0xC8],0x10000000`, SE 46955/46957 `test [actor+0xC0],0xF0000000`)
    and only below the window header's top. Open review items: REVIEW-BACKLOG APMF-B36.

### `native/core/CombatBehaviorRE.h` — the local RE:: extension for the combat tree (measured)
The `CombatBehaviorTreeNode` layout (10 vfuncs: act 0x02 / pop 0x03 / update 0x04 /
on_interrupted 0x05), `TreeControl` (+0x158 master), `ControllerMini` (attackerHandle
0x28, targetHandle 0x2C, `static_assert` < 0x68), the 70 leaf VariantID triples, the four
ch.23 search leaves (`kSearchLeaves`) and the group search-centre offsets, `kCastContextNodes`,
the update-seat block (runner step, SetFailed/Ascend both runtimes, the TLS/control identity
proof, the corrected 2026-09-03 crash history), and "The node protocol" — the disassembly-measured CombatBehaviorThread layout + the
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
§0.38), refused off exactly 1.6.1170 / 1.5.97 / 1.7.104 with VR included (G1 2026-10-04). Thunk `TESPackage*(Actor*)`: if the actor holds the winning
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
**RANKED 0x49 (2026-10-18, feat/apmf-queued-0x49, ClickUp 86e3t1gfh / 86e3t1gfp; marth: "name AND ENFORCE",
"theyll stop winning if Auri.esp is actively doing anything").** `RankAndDecide` (anon ns, before `PkgHook`) runs
for a claimed actor whose named package resolves. `orig` (the engine's own answer: slot 0x49 AE 37398 0x66CB60 /
SE 0x5DB180 / 1.7.104 0x67F620, instruction-identical: null when dead or life-state 0x800000, else the FIRST quest
alias package whose conditions + schedule pass, AE 12057 -> 12721 0x182390 -> 29619) null or == the claimed package
= IDLE: served, nothing counted against a source. Otherwise ACTIVE: owner = `orig->GetFile(0)` plugin (`Register`
kPlugin), no file = Engine, failed register = Unattributed (degrade rule: never displaced unless `[UnknownSources]
Package=override`); owner == the claim's client (offer client, or ch.19 travel claim's client: `RankClaimPrio`, same
rule as PackageSources/SpectatorGate) = served + `NoteOwn`; else `NoteHit`, then claim prio STRICTLY > owner prio =
served + `NoteOverruled`, else the original stands + `NoteStood` (tie keeps the follower mod's package).
**Round 2/3 (marth "Plain following isn't active"; review 34f2779 fixes):** before ranking, `ClassifyPackage(orig)`
(cached per plugin package pointer, leaf `g_kindMx`) reads what the package DOES: PKDT `packData.packType` (+0x24;
NOT the derived +0xD8, which 29582 sets to 0x2E for every tree package); types 18/19 -> `TESCustomPackageData`
(+0x30) tree (+0x20, else the `templateParent` (+0x30) chain, 4 hops); nodes and procedures named by their own RTTI
through `RttiName`, which VALIDATES each hop (vtable and locator in .rdata, locator signature 1 + self-RVA,
TypeDescriptor in .data, name ".?A" and NUL-terminated in bounds) because the DLL has no /EHa and catch(...) cannot
catch an AV. Procedure node `procedure` +0x10, Sequence/Stacked/Simultaneous/Random children BSTArray +0x10/size
+0x20 (verified AE/SE/1.7.104, addresses in the code comment). Roles (`kProcKinds`): STANDING follow, wait, sandbox,
sit, sleep, eat, idle marker, wander, keep an eye on, done, guard, patrol (marth: routine posture); AUXILIARY
travel, lock/unlock doors, find, acquire; everything else ACTIVE. Any active -> active; else any standing ->
`kStanding` (served, nothing counted, logged `owner=<plugin> kind=<label> -> idle`); else auxiliary-only (a pure
Travel) -> active. Unknown node/procedure/unreadable -> active. Source counters (Hit, Overruled, Stood, Own) move
ONCE PER TRANSITION per actor; Served stays per consult (keeps the client seen for `Winner()`). `SourceForFile`
caches plugin -> SourceId per `TESFile*` (leaf `g_fileSrcMx`). `RankedAtSeat(actor, pkg)` lets
`core/PackageSources.cpp`'s poll leave a 0x49-ranked capture (not a scene) to this seat (no Hit/Stood/Observed
there, an info line pointing at `[ch.9-rank]`). What breaks: classifying by +0xD8; making an auxiliary procedure
active again (every vanilla follower template has Travel: 107 of Skyrim.esm's templates); dereferencing an RTTI
hop without the bounds checks; treating an unknown node as standing; caching a runtime-created package (address
reuse); counting per consult again (the poll and the menu count per transition). Spot left: a vanilla package
reused in a follower mod's alias is still owned by Skyrim.esm (the owning alias is not visible after the
original returns; attributing it means a second ExtraAliasInstanceArray walk under its lock).
`[ch.9-rank]` is logged per (claimForm, orig, owner, decision, kind) transition per actor (leaf `g_rankMx`, cap
4000, always on), cleared by `ForgetRedirect` / `ForgetAllRedirects`; the heartbeat gains `rankOutranks` /
`rankYields` (per consult). A registry throw serves the claim unranked and logs an error. Also: ranking against
`GetCurrentPackage()` instead of `orig` is tenure (the incumbent would always win); never count Served on a yield;
never take `g_rankMx` / `g_kindMx` / `g_fileSrcMx` across `Register` or a lookup.
- **What breaks:** the redirect only takes effect **when the engine ASKS**, and the field
  evidence is that it does not ask on a useful cadence of its own — every win so far
  reconciles to an explicit `EvaluatePackage` nudge (MFO `DIAG-2026-09-06-loot-travel.md`;
  `Docs/SPEC-PACKAGE-HOLD.md` §2.2). So the nudge's ORDERING relative to the claim's
  `Publish()` is load-bearing, not incidental — inline it again and the channel silently
  goes back to answering with the pre-claim package. Never touch alias/run-once state
  (INVARIANTS #3a); never return null.

### `native/core/SpectatorGate.{h,cpp}` — the Package facet's SPECTATOR seat (2026-10-12, ClickUp 86e3m5feu)
`Install()` <- `plugin.cpp` kDataLoaded (after `packagesources::Install`): `write_vfunc` **0xDC**
`InitiateSpectator(Actor* target)` on `VTABLE_Character[0]` (AE 0x7270E0 / SE 0x6932D0 / 1.7.104 0x739C10;
1.7.104 instruction-identical to AE), VR-refused, exact-runtime gated, `SeatVerified` on the Character row,
census-registered on the Package facet. The engine's only live creator of a Spectator package (procedure 24):
detection reaction (AE 0x70B6F0) -> spectate predicate (AE 0x70AF90: self out of combat, the other in combat)
-> spectator dispatcher (AE 0x83D5F0 / SE 0x7A62E0 / 1.7.104 0x8524D0, one vcall to 0xDC) -> SpectatorPackage
built and PUT with 0xDF. 0x49 is never asked, which is why it beat MFO's retreat offer (field 1006d). Thunk: an
actor with a winning ch.9 claim whose package resolves and whose client (ch.19's internal offer: the travel
claim's client, as `PackageSources`) outranks `Priority(Engine, Package)` is NOT forwarded (enforce) or
forwarded with a "WOULD DENY" line (observe); Engine hit / overruled / stood counted on the Package facet.
`[PackageGate] iSpectatorSeat` 0 off / 1 observe / 2 enforce (default). Logs: `[spectator]` one line per
(actor, claimed package, decision) transition with the engine call-site RVA (expected AE 0x83D637), `[spectator] H`
totals every 30 s (zeros included), `ResetAll()` at revert and kPreLoadGame.
- **What breaks:** never move the deny to 0xDF (the package is allocated by then: leak / double free); never
  deny without the claim+priority test (a Spectator package is legitimate engine behaviour for every other
  actor); the thunk may run off the main thread, so it reads only the RCU snapshot, the lock-free registry and
  atomics (the transition map takes its own mutex). A Spectator package already running when the claim
  engages is not evicted (`Docs/DENY-COMPLETENESS-AUDIT.md` gap 15). NOT field-proven yet.

### `native/core/FacetAttribution.{h,cpp}` — HARBINGER 1b: attribution at the pre-registry seats (2026-10-12)
**CAST SLICE (2026-10-12):** `InsideSource(prevFn)` (the source a seat replaces = the function it displaced on THAT
vtable: engine / another APMF layer -> Engine; a foreign module -> that module as a hook (skse64 by name, never
DirectCalls); a module that is also a CLIENT -> Engine (its hook only wraps the engine's answer); no module ->
kNoSource, the degrade rule; cached per address), `CastClaimHandle(actor)` (highest-basis live kIntent_Cast claim via
`ControlMap::CastFacetOutranks(-inf)`; approximate with two different clients' cast claims on one actor),
`SelectClaimHandle(actor)` (ch.8), `Outranks(...)` (Decide's rule with no counter / alert / log, for the silent
follow-on seats). Users: `core/CastGate.cpp` (the final 0x0A refusal; both narrowings refusing -> the higher-priority
claim decides), `core/EquipGate.cpp` (`claimWins` lambda: Restore seat YES, N4, deny-complete, engine-first ch.8 and
ch.8b refusals; one counted decision per claim kind per call; NOT the completeness fix; the ch.15 refusal is ranked
on the EQUIP facet since D2, 2026-10-12, `Decide` with the ch.15 claim's handle), `core/HandBlock.cpp` (D2: the 0x0F
hand block, Equip facet), `core/EquipSink.cpp` (D2: `RankEquipDeny`, Decide's rule without the second hit),
`core/EquipPreference.cpp` (ch.26: `Outranks` for the score lift), `core/CastSeats.cpp`
(`ClaimNamesThisCast(..., prevFn, count)`: EVERY seat decides against the 0x06 slot's displaced function on that
vtable (`StartSlotPrev`, review SEV-3), 0x06 counts, 0x0A / 0x07 / 0x0D silent, so a cast is wholly claim-driven or
wholly the engine's), `core/CastClassify.cpp` (`ClaimOutranksClassify`: the force site counts, the two serves are silent).
Every one keeps the claim's answer on a throw: the handle reads, `InsideSource`, `ClaimPriority` and the registry all
run inside the try; a throw in the "STANDS" line itself is caught separately and keeps the stand. Every "STANDS" line
uses `WhyStands` (an unknown source with the override off says unknown sources are ALLOWED by `[UnknownSources]`). APMF-B71 folds: `Sources` `ModuleOf` negative cache (4 KiB page, 60 s,
256 cap) and `ActionGate` `ClaimOutranksTree` try/catch. ch.25 CastGuard untouched.
Shared by the Combat and CombatAction facets' existing seats (no new seat, no new address, no ABI change).
`Decide(facet, claimHandle, src, actor, unknownDetail)` = NoteHit + the priority test (`ClaimPriority(handle)` >
`Priority(src)`, STRICT: a tie stands; a source that IS the claim's client ranks at the claim's priority) +
NoteOverruled / NoteStood; `src == kNoSource` runs the degrade rule (`Sources::Unattributed`; allowed unless
`OverrideUnknown(facet)`, then ranked at the Unattributed row). `Observe(...)` = hit + stood (seats with no
enforcement). `CallerBelow(siteRet)` = STACK SCAN (`RtlCaptureStackBackTrace`, 16 frames): the frame after the one
equal to a known engine return site is that engine function's caller, resolved by `Sources::ForAddress(kPlugin)`
(exe -> Engine, skse64 -> DirectCalls, APMF -> `self`). `LineDue(facet, actor, key)` = the 5 s per-key rate limit
for the attribution lines (bounded map). Users: `channels/CombatReentryDeny.cpp` (ch.22 decision + ch.21
`ObserveEntryClaim`), `channels/CombatEntry.cpp` (engine refusal), `core/ActionGate.cpp` (`ClaimOutranksTree`, ch.7 /
ch.23), `channels/CombatApproach.cpp` (S1 decision, `AttributeYield`, the drift detector).
- **What breaks:** any thread (combat jobs, BSJobs detection jobs, the game thread): never add an engine call or a
  form lookup here. `Decide` must be called only AFTER every mechanism check at the seat passed (so "overruled"
  counts a real enforcement). `CallerBelow` is only meaningful for a seat reached from ONE verified call site
  (`g_siteRet`); a seat reached through a foreign wrapper that CALLS (not tail-jumps) never sees that return
  address at all. Papyrus natives inside SkyrimSE.exe resolve to Engine, not DirectCalls (KNOWN LIMIT; needs
  per-runtime caller rows).
  Open review findings: Docs/REVIEW-BACKLOG.md APMF-B72 (cast slice: actor-wide CastClaimHandle; client hook ranked as
  Engine; hit-count noise) and APMF-B71 (no negative ModuleOf cache under the loader lock; `found=false`
  counted as unattributed; Papyrus/console StartCombat named Engine; ActionGate ClaimOutranksTree lacks try/catch).

### `native/core/ActorValueObserve.{h,cpp}` — HARBINGER 1b: the OBSERVE-ONLY actor-value seat (2026-10-12)
`Install()` <- `plugin.cpp` kDataLoaded: `write_vfunc` 0x04 SetBaseActorValue / 0x05 ModActorValue / 0x06
RestoreActorValue on Character's ActorValueOwner sub-object vtable `VTABLE_Character[5]` (1.6.1170 id 207896
0x18A5FE8, COL 0xB8; 1.5.97 id 261402 0x165E4D0, COL 0xB0; 1.7.104 0x1923CE8, COL 0xB8), spec rows
`Character.ActorValueOwner` (AE, carries 1.7.104) / `Character.ActorValueOwner.SE`, `SeatVerified` on the running
build's row, exact-runtime gated, VR-refused, `[ActorValueObserve] iActorValueSeat` (1 observe default / 0 off).
Slot shapes read off all three images (this = the sub-object; edx av + xmm2 value for 0x04 / 0x05; edx modifier,
r8d av, xmm3 value for 0x06); 0x07 SetActorValue is the base `jmp [rax+0x20]` tail jump into 0x04, so it is seen there.
Each thunk runs the ORIGINAL FIRST and always, then `Note`: for an AV a claim channel holds (ch.16
MovementNoiseMult / DetectLifeRange -> facet Detection; ch.1a SpeedMult, ch.11 Aggression / Confidence / Assistance /
Morality -> facet ActorValue) on an actor holding that intent's WINNING claim, the caller (`_ReturnAddress()`) goes
through `Sources::ForAddress` (exe -> Engine, skse64 -> DirectCalls, a DLL -> that DLL; APMF's own AvLedger writes
skipped) and `FacetAttribution::Observe` (hit + stood). `[avobs]` first-call OBSERVED line, 5 s per (actor, av) line,
30 s heartbeat (`Arbiter::OncePerFrame`).
- **What breaks:** OBSERVE ONLY at any priority: never return early before the original, never change an argument.
  The thunk runs on any thread for EVERY Character AV write (health regen included): keep the AV switch first and
  read nothing but the ControlMap snapshot, the registry and the FormID. The `this` rebase (0xB8 AE / 1.7.104, 0xB0 SE)
  is the COL offset of the row, not a guess. A Papyrus SetAV / ModAV resolves to Engine (APMF-B71 limit).
  COVERAGE LIMIT: engine-internal actor-value paths that do not go through these virtual slots (non-virtual helpers
  called directly inside the exe) bypass the seat and are not seen. A hook layered over the seat is handled by
  `LayerOver` + `CallerBelow` (unrecoverable -> "through hook X", not attributed); the 0x04-0x06 originals are read
  and stored in `func` BEFORE each thunk is written (safe_write), so no thunk can run with a null original.
  Open review findings: Docs/REVIEW-BACKLOG.md APMF-B73.

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
**RANK ONCE AS REQUESTER (2026-10-18, feat/apmf-queued-0x49, marth ruling (a)):** a queued equip (q=1) passes the
sink twice: leg 1 from its requester, then (when worker +0x272 -> 37684 queued it rather than applying at once)
leg 2 from QueuedApply (AE 38906/39814, SE 37950/38789; 39814/38789 read the 0x48 node: +8 obj, +0x10 extra, +0x18
count, +0x20 slot, EquipObject(queue=0)). `recordIfQueued` (after the worker call, both worker call sites in the
thunk; after because the worker nulls an empty `data->extra` at +0x25C before queuing) stores {actor, item, extra,
eqSrc, ch.17 handle, time, caller kind, selfPath, attributed} in `g_queued` (leaf `g_queuedMx`, cap 128, append
order). At a QueuedApply leg the recovery branch `ConsumeQueued`s the NEWEST live record for (actor, item),
preferring the same extra (newest because an immediately applied leg 1 leaves a record no leg 2 consumes) and
ranks as that requester (eqSrc, selfPath, attributed, caller.kind all restored: the script / console / player-menu
exemptions carry). Leg 2 adds no hit, stood, own, force-own/anomaly or Unattributed count (an applied deny still
counts). TTL `kQueuedTtlMs` 5000 = a FLOOR, ~3x the field max (1994 lines 2026-10-18: p99 305 ms, max 1.7 s); a
miss ranks as Engine as before and logs once per (actor, item) (`req=none`). Log field `req=<source|self|
unattributed|none>` before `verdict=`. What breaks: recording before the worker (extra mismatch), recording a
refused leg 1 (nothing queued), counting leg 2's hit again (the double count this fixes).
**ABI v25 INSTANCE (2026-10-14):** for a form the set declares ONLY by uid (`InstanceOnly`), the thunk reads
the instance the equip names from `EquipData::extra` (`ExtraUid`: `GetByType<ExtraUniqueID>`, the list's read
lock, which EquipObject took on this thread just before) and calls the 4-argument
`DeclaredSetVerdict(set, item, competes, EquipInstance)`: named + same uid = in-set; another / no uid / NULL
extra = off-set. The 0x0F seat (`core/HandBlock.cpp DeclaredSetAnswer`) passes `held` instead (the hand
already holds the form, `GetEquippedObject`), since a CombatInventoryItem has no extra list and its equip
passes NULL. The 3-argument verdict is the base-form one, kept for `blacked`-only callers (the denied-
category unequip, `WornDeniedRefused`). Log field `inst=none|nouid|uid:0x....` before `verdict=`.
What breaks: reading the extra for every equip (keep the `InstanceOnly` gate), treating a NULL extra as
in-set for a uid-only form (the engine would pick any copy), and adding a runtime to the install gate
without re-verifying EquipData+0, the list lock offset and ExtraUniqueID+0x14 on it.
**D2 (marth 2026-10-12): every deny here is RANKED.** The caller's source (`eqSrc`: a named engine path or the
player's menu -> Engine, Papyrus / console -> DirectCalls, a named external module -> that plugin, APMF itself ->
`selfPath`, unranked) is ranked by `RankEquipDeny` (Decide's rule without a second hit) against the claim behind
each deny: the hand block against `facetattr::CastClaimHandle`, the ch.17 set against `EquipSetView::handle`
(new, same snapshot read), the ch.26 FORCE step (ABI v22) against the forced preference. A source that outranks
the claim keeps its equip (`allow (the source outranks the claim: its equip stands)`, a rate-limited STANDS
line, `NoteStood`); an applied deny counts `NoteOverruled`. Unattributed stays allowed unless `[UnknownSources]
Equip=override`. Behaviour at the default priorities: Engine/other mods (20) still lose to a client (50);
a client's OWN equip now stands against its own claim; a script equip under DenyScript ranks as DirectCalls
(75) and stands unless the client is raised above it. The force step sits after ch.17, ignores the observe-only
switch (it is the client's explicit escalation), exempts the player's menu and APMF's own equips, and a
refusal the sliders forbid is a FORCE ANOMALY (core/EquipPreference.h); the claim's own client's equip is a
separate `own client` verdict (review F1). Open: REVIEW-BACKLOG APMF-B76 (the hand block ranks with
`CastClaimHandle`, the highest-basis cast claim on ANY hand), APMF-B78 (force and slot-less left-hand items).
**HARBINGER 1a (2026-10-11): WHO ASKED is a STACK SCAN now.** UAPNG.dll and
EquipmentDurabilitySystem-NG.dll re-patch both E8s after our kDataLoaded verify (LoreRim diag
2026-10-10), so the thunk is reached from THEIR stub and the old `_ReturnAddress()-5` site test +
fixed-depth caller read produced 81 refusals at `path=Unknown(0)`. The thunk now runs
`RtlCaptureStackBackTrace` (16 frames): the frame equal to one of our two sites + 5 is
EquipObject's (an interposer's own E8 sits at the same address), the next frame is its caller
(`Classify` as before); foreign frames in between are INTERPOSER sources (Equip facet). The fixed
depth survives only as a once-logged cross-check on the direct path. A caller that cannot be
recovered (no site frame, or Unknown(0), or an unnamed external module) is UNATTRIBUTED: verdict
`allow (unattributed: caller not recovered)`, `path=Unattributed`, `sources::Unattributed` — never
denied, hand block included (the degrade rule). Both E8s are census call-site rows (re-read at
kPostLoadGame / kNewGame / 60 s; an interposer on top is named there). No new deny anywhere.
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
exemptions sit ABOVE the masks; **step 1a (2026-10-06, the hand-claim block):** the item's
`Categorize` hand bits hit a hand a live `kIntent_Cast` claim holds (`handblock::HeldFor`, the
claim's own spell/proxy excepted) → `deny (hand R|L|R+L held by a cast claim)`, worker not
called, EVEN IF IN-SET and whatever observe-only says (an actor with NO ch.17 claim reaches the
verdict only through this step's `AnyHandHeld` pre-gate, with an emptied view, owned=0, and leaves
untouched and unlogged when its equip hits no held hand; INVARIANTS #17a(5) as amended); `black` → refuse EVEN IF IN-SET; `owned` → no declaration
(`count==0`) or item in the declared set (FormID compare — the v8 hand is NOT consulted here) →
allow, else refuse; neither owned nor denied → allow (`owned=0`, the engine's category).
A refusal is `would-deny` + call under observe-only
(INI `bEquipObserveOnly`, default 0 since 2026-10-13 -- every category enforced, the armor-only
`bEquipEnforceArmor` retired -- or the claim's `kEquipAuth_ObserveOnly`). The verdict rules 2-6 are
ONE function, `equipsink::DeclaredSetVerdict` (EquipSink.h), shared with the 0x0F selection step
(core/HandBlock.cpp `DeclaredSetAnswer`) and the enforce pass's denied-category unequip
(channels/EquipAuthority.cpp `UnequipDenied`); change it there, never in one caller. The ch.17
ranking passes `bindsOwner` to `RankEquipDeny`: the claim's own client does NOT tie with its claim
(ch.17 only; the hand block and the force step keep the tie). A DENIED-category item is refused from EVERY route (player menu, script /
console, unattributed ranked as Unattributed; review F1 2026-10-13), still ranked; a denied-category equip the sink
lets STAND records its source (`RecordStood`), and the enforce pass strips only via `WornDeniedRefused` (that
source, else Engine). HandBlock `NoteHandAnswer` warns once per (actor, hand, claim) when a hand saw only ch.17
NOs for 5 s at selection (review F2); else `deny` = RETURN WITHOUT CALLING the worker (nothing queued, no
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
  APMF-B5..B10** (CLOSED 2026-10-06: B5, B6, B8, B9 superseded by ClickUp 86e3jwmk7 (B7 REOPENED 2026-10-06), the weighting rework retires the ch.17 declaration road; B10 done; B11/B12 likewise) (B5 Engage log names only v7; B6 the hand-EQUP resolve error over-claims;
  B7 the once-logged claim-refusal reason can be stale; B8 Enforce's seat-missing branch is
  unreachable under the v8 refusal; B9 queued-slot carriage unobserved — INTEGRATION criterion
  7; B10 `MinReleaseForAbi`'s v7/v8 placeholder must be named at the cut) (ch.15's probe re-equip is
  `External(APMF.dll)` on a ch.17 actor; a detour target inside a trampoline page prints
  `detoured by ?`; the truncation log fires at Apply) — read them before editing this seat.

### `native/core/HandBlock.{h,cpp}` — the HAND-CLAIM BLOCK (2026-10-06, `fix/apmf-hand-claim-blocks-equip`)
marth: "harbinger taking a hand for an action means other actions on that hand are blocked for the
duration". A hand held by a LIVE `kIntent_Cast` claim (driving, or a `kCastFlag_DenyHandOnly` floor;
`ControlMap::TryGetCastClaimForHand`, lapsed claims excluded) refuses every other weapon / shield /
torch equip into it. Spells/staves were already refused per hand at `core/EquipGate.cpp`; this closes
the weapon half (DENY-COMPLETENESS-AUDIT gap 10). Field origin: `_research/field-1006-heal-diagnosis.md`
cause 2 (a dagger equipped into the deny-only right hand 32 ms before the left heal was interrupted).
- **The 0x0F seat (this file).** `CheckShouldEquip` (slot 0x0F, `bool(CombatInventoryItem*,
  CombatController*)`, scalar return) on the four weapon-class item vtables Melee / Ranged / Shield /
  Torch AND the unarmed `OneHandedBlock` item (review F1: built over the unarmed weapon with slot
  kLeftHandEquip; its own 0x0F AE 0x819300 / SE 0x77EF60 / 1.7.104 0x82E1E0) (VerifiedAddresses rows
  `CombatInventoryItem*`, `SeatVerified` per vtable). CombatInventory's
  evaluate (AE 44899 / SE 43666) and pre-loop (AE 44868 / SE 43637) skip a candidate on NO, so the
  item never enters the equipment set and the EquipObject leaf (AE 48124) never gets it. Engine answer
  first; only YES turns to NO, for an item whose `equipsink::Categorize(item, itemSlot.equipSlot)` hand
  bits (2H / bow -> both; a one-hander by its own slot [at this seat EitherHand / null never
  arrives: AddItem splits a multi-parent slot into a RightHand item plus a LeftHand clone, APMF-B66 F10;
  the sink's Categorize still treats it as both]; shield / torch -> left) hit a held hand. Slot functions per build: Melee/Ranged/Shield `!IsFleeing` base AE 0x817FC0 /
  SE 0x77DC90 / 1.7.104 0x82CEA0; Torch OVERRIDES the slot, AE 0x819760 / SE 0x77F350 / 1.7.104
  0x82E640 (1.7.104 raw rows `AiCastSeats.kCheckShouldEquipBaseAE` / `HandBlock.Torch.CheckShouldEquip`,
  `HandBlock.OneHandedBlock.CheckShouldEquip`; the two HandBlock rows are proven through the committed
  `tools/verified_addresses/idmap-1.7.104-apmf.csv`, backlog APMF-B63, CLOSED 2026-10-06: superseded by ClickUp 86e3jwgvy). Line: `[handblock 0x0F] <actor>
  CheckShouldEquip <class> item=... competes=... -> NO (hand R held by a cast claim: R=... L=...)
  site=pre-loop|evaluate`, 1.5 s per (actor, item) and a global 100/s cap (dropped lines counted on
  the next one). `EngineAnswerFor` / `IsThunk` let the Ranged probe (chained OUTSIDE this seat) log
  `engine=` (the engine's own answer) and `handblock=` separately and name this thunk at install.
- **The sink step.** `core/EquipSink.cpp` verdict step 1a calls `HeldFor` (`AnyHandHeld` pre-gate for an
  actor with no ch.17 claim): `verdict=deny (hand R held by a cast claim)`, worker not called, enforced
  whatever `bEquipObserveOnly` says. INVARIANTS #17a(5) as amended.
- `[HandBlock] bHandClaimBlocksEquip` (default 1) switches both halves.
- **ch.17 at SELECTION (2026-10-13).** The same 0x0F thunk also answers NO for a candidate the actor's
  ch.17 declaration refuses (`DeclaredSetAnswer` -> `equipsink::DeclaredSetVerdict`, the candidate's own
  `itemSlot`), ranked by `facetattr::Decide` against `InsideSource(orig)`; observe-only logs and keeps YES.
  So the seat installs when `bHandClaimBlocksEquip=1` OR `[EquipAuthority] bEquipAuthority=1` (read
  directly: `handblock::Install` runs before `equipsink::Install`). Combat-only by nature; out of combat
  the equip sink alone refuses.
- **D2 (2026-10-12): ranked on the Equip facet.** The 0x0F NO stands only while the holding cast claim
  (`facetattr::CastClaimHandle`) outranks `InsideSource(orig)` (`facetattr::Decide`, Equip facet); else the
  engine's YES stands (`[handblock 0x0F] ... STANDS`, 5 s per actor). The sink step ranks the same way.
- **Spells-only floors (ABI v20, 2026-10-06, `fix/apmf-floor-spells-only`).** A floor carrying
  `kCastFlag_DenyHandOnly | kCastFlag_FloorSpellsOnly` holds its hand in `HeldFor` ONLY when the
  competing item is `SpellLike` (a spell, a scroll, or a staff via `TESObjectWEAP::IsStaff`), so a
  one-handed weapon / shield / torch in the floored hand passes both halves for it; a two-hander, a bow
  or a no-hand item (unarmed, EitherHand / null slot) still competes for BOTH hands (`Categorize`) and
  is refused while a driving claim holds the other hand (the field's bow 32 and Unarmed 22 refusals
  continue; correct by marth's rule). Field origin: Cicero's floored right
  hand refused his bow, swords and fists 119 times in 40 s. The spell side of the floor is NOT here
  and is unchanged (CastGate 0x0A, EquipGate 0x0F on the magic/staff items). A driving claim and a
  floor without the bit keep the full block. Lines say WHAT holds the hand: the 0x0F line's
  `R=` / `L=` are `HoldDesc` ("claim (spell 0x...)", "deny-only floor", "spells-only floor"), the sink
  verdict is `deny (hand R held by a cast claim | a deny-only floor | a spells-only floor: a staff)`.
- **What breaks:** (1) **Install order**: `plugin.cpp` calls `handblock::Install()` right after
  `equipgate::Install()` and BEFORE `aicastseats::Install()`, because the install REFUSES a class whose
  slot 0x0F does not hold this build's disassembled engine function (APMF-B36 F2: a deny must know what
  it wraps). Moving it after AiCastSeats' Ranged probe / Shield TASK2 would refuse Ranged/Shield
  whenever those probes are on. (2) The hand map is `equipsink::Categorize` -- never a second map (MAP
  EquipSink 8b). (3) The claim's own spell/proxy is never refused (`HeldFor`'s `a_item`); a client that
  ever drives a staff through a cast claim relies on that. (4) Thread: combat thread at 0x0F, any thread
  at the sink; RCU reads and member reads only, no form lookup. (5) Torch's literal differs from the
  base; a new build needs both columns. (6) The script / console / player-menu exemptions stay above
  the sink step (a quest script or the player can still arm a held hand). (7) `SpellLike` is the ONE
  definition of "spell-like" for the spells-only floor at both halves; a staff MUST stay spell-like
  (it starts a spell in the hand, which is what the floor exists to stop). `AnyHandHeld` asks with
  `a_spellLike = true` because it is only the sink's pre-gate. (8) The sink's verdict texts are
  stored as POINTERS by `castobserve::NoteEquip`: literals only. Open backlog: APMF-B67 (spells-only
  floor review: CHANGELOG heading, `MinReleaseForAbi` text, INVARIANTS #14b stale bullet, mixed-hold
  sink label).

### `native/core/CastObserve.{h,cpp}` — passive cast observation, `[castobs]`
The 0xAD-era observe-and-replicate probe (CASTER state transitions every 100 ms, the per-actor anim sink
that also feeds `castertypecensus::NoteAnimEvent`), plus, since 2026-10-06, two always-on, rate-limited
diagnosis lines for WATCHED actors (a live `kIntent_Cast` claim, refreshed every 100 ms, at most 16):
- `[castobs] INTERRUPT-ATTRIB`: on an `InterruptCast` anim tag, printed on the game thread at the next
  frame: each hand's claim (spell / proxy / deny-only), each hand's and the instant caster's MagicCaster
  state + spell, whether a CombatController exists, the behaviour-tree leaves entered in the last 1 s,
  the CombatMagicCasters built / fired in the last 1 s, CheckCast NOs in the last 500 ms (engine or
  Harbinger, with the reason) and the governed equips in the last 100 ms (path + sink verdict).
- `[castobs] CHANNEL-END`: for a claim whose driven form is a concentration cast, the claimed hand's
  caster is read every frame; when it returns to 0 after running (state >= 4 holding the driven form):
  the state transitions with ms offsets, seat 0x07 STOP reasons, CheckCast NOs on that hand, cast anim
  tags, the behaviour-tree leaves entered in the last 1 s (the same ring INTERRUPT-ATTRIB prints,
  added 2026-10-06 `fix/apmf-floor-spells-only`), whether the claim still stood, magicka %,
  Harbinger's stored own-LoS reading, and "engine LoS re-check: not seated" (that engine check has no
  seat; it is not guessed). The CALLER of the InterruptCast / NotifyStopCast that ended the channel is
  NOT printed: this line is a per-frame poll of caster state and InterruptCast reaches Harbinger only as
  an anim-graph output event, so no frame of that call is ever on the stack here; naming it needs a new
  seat on `MagicCaster::InterruptCast`, which has not been built.
Recorders (`NoteEquip` / `NoteLeafAct` / `NoteCaster` / `NoteCheckCast` / `NoteStopCast`, any thread,
leaf lock, no engine call) are fed by one line each in `core/EquipSink.cpp`, `core/ActionGate.cpp`
`ActThunk`, `core/CasterTypeCensus.cpp` `Observe`, `core/CastGate.cpp` (engine NO and Harbinger deny) and
`core/CastSeats.cpp` seat 0x07.
- **What breaks:** the recorders must stay observe-only and lock-leaf (never log or call the engine
  under `g_recMx`); string arguments are stored as pointers, so they must stay literals. The sink never
  reads the actor off its thread: the attribution is formatted on the game thread. The leaf-name map
  resolves ch.7's VariantIDs only on the three supported builds (an id miss is fatal in the fork).

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
  and 1.7.104, slot 0xDF the same 12-callee function id-for-id on SE. G1 (2026-10-04):
  the 0xDF hook is now exact-version gated anyway (`allowance::RuntimeSupported()`);
  F2b (2026-10-05) reopened it on 1.7.104 (slot 0xDF `0x691590`, identical). The INI
  switch and the 0x49 assist are not gated.

### `native/channels/*.cpp` — one module per facet (FULL documented catalog)
Each: a `Channel` subclass + `APMF_REGISTER_CHANNEL`, per-NPC `Engage`/`Release`.
The first release shipped the documented catalog of 13 channels as a baseline
benchmark; the table below is the CURRENT set — 26 files, 26 rows (plus `AnchorGuard.{h,cpp}`, 2026-10-19, NOT a channel and registers nothing: the combat substrate's anchor-leak guard, documented in the ch.22 and ch.27 rows; the combat substrate `CombatSubstrate` added 2026-10-13 (ABI v23, slice S2a); ch.26 `EquipPreference` added 2026-10-12 (ABI v22); ch.25 `CastGuard` added 2026-10-12 (ABI v21); ch.24 `CombatApproach` added 2026-10-05 (ABI v19); ch.23 `PursuitLeash` added 2026-09-25 (ABI v16); ch.22 `CombatReentryDeny`, ch.21 `CombatEntry` and ch.20 `TargetPin` added 2026-09-25; ch.7
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
| `CastCompose.cpp` | 8b | cast EXECUTION claim (`RequestCast`, ABI v5) | Log-only. (The delivery-flip proxy is ref-counted per claim and given back by `core/ControlMap.cpp`'s claim-removal paths, one `mainthread::Post` hop after the removal publishes — INVARIANTS #20.) The claim's real effect is the FIVE ENGINE SEATS (`core/CastSeats.cpp` 0x06/0x07/0x0A/0x0D + `core/EquipGate.cpp` 0x0F) — the NPC's own AI performs the cast | claim + seat answers; the ENGINE executes |
| `Dialogue.cpp` | 10 | dialogue (Num6) | `PauseCurrentDialogue()` | one-shot |
| `Attribute.cpp` | 11 | disposition (Num2) | 4 AVs: aggression/confidence/assistance/morality | source-block |
| `Idle.cpp` (+ `Idle.h`) | 12 | idle/anim (Num+) | v1: `NotifyAnimationGraph("IdleForceDefaultState")`. v2 (ABI v17, `param.form` != 0): `AIProcess::PlayIdle(actor, idle, target)` posted past Publish + re-validated (`Play`), ends the claim on a refusal / owner death (`EndClaim`, `apmf::idle::Poll` from `Arbiter::OncePerFrame`), per-actor anim sink records `IdleStop` = not held; `Release` sends `IdleForceDefaultState` only when held. Gate `apmf::idle::Install` (kDataLoaded, SeatVerified `Idle.AIProcess.SetupSpecialIdle`); sync refusal in `ControlMap::EnqueueRequest`; `ResetAll` at kPreLoadGame / revert (`plugin.cpp`). **What breaks:** never call PlayIdle for an actor with no process or no high process data (the engine returns false on a null high; keep the named check); never re-play on a timer or when the idle ends (#0 (c): one call per declaration); never send IdleForceDefaultState at Release for an idle whose graph raised IdleStop (it would pop an actor out of whatever it is doing now); never drop the sit/sleep != kNormal guard in `ResetIfHeld` (shared by Release and a form-0 owner change; review F1: a false "held" must not eject an actor from furniture); keep `[Idle] bIdleV2` feeding `g_notInstalledReason` (review F2); never make an engine call from the anim sink (it runs on the graph-update thread; plain values under `g_obsMx` only); never EnqueueRelease from inside Drain; keep v1 (`param.form` 0) byte-for-byte: no gate, no Release action. **Entry confirmation (fix/apmf-idle-confirm):** an accepted play counts as PLAYED only when the actor's graph raises one of the idle's own ENTRY events (destination enterNotify + nested start-state enter; source exitNotify only when no enter event exists), resolved per play by `ResolveEntry` / `EntryWalk`, a read-only walk of the behaviour TEMPLATES (vtables BShkbAnimationGraph / hkbBehaviorGraph / hkbStateMachine self-checked, every other Havok object identified by its RTTI name before a member is read; Havok 2010 x64 in-place offsets documented in the file header). None in 3000 ms of WORLD time (+30 frames; `g_worldMs`, Poll-to-Poll gaps clamped to 100 ms) = ANIMATION NOT CONFIRMED and `EndClaim`; NO TRANSITION (only after a COMPLETE walk) = `EndClaim`; otherwise UNCONFIRMABLE (claim kept, legacy held rule). `[Idle] bIdleConfirm` (default 1) is the kill switch. **What breaks:** never count ANY graph event as confirmation again (the 2026-09-28 lockpick); never answer NO TRANSITION from a walk with a blind spot (unknown class, unlinked reference, out-of-range event id, unresolved target, wrong-class object, unwalked graph -- `EntryInfo::Blind`); an out-of-range event id makes the play UNCONFIRMABLE (id space broken), never judged; never read a Havok member before the object's class is identified; never time the window on wall clock (a menu pause would end live claims); an IdleStop before any entry event is not the idle's end; an entry event other transitions raise too is logged WEAK evidence (IdleOffsetStop: ~105 others). Open findings: `Docs/REVIEW-BACKLOG.md` APMF-B34 (round 1) and APMF-B35 (round 2; R2-4 SEV-3 OPEN: WEAK is a label only, a false CONFIRMED on a shared entry event keeps the claim live). **Idle state (ABI v24, feat/apmf-idle-state):** a per-HANDLE record table `g_states` behind the LEAF mutex `g_stateMx` (nothing acquired under it, no engine call under it). Pending from the client's thread (`NoteFiled` after `RequestEx`, insert-only; `NoteRepoint` before `EnqueueRepoint`; `NoteReleased`); game-thread transitions: `Play` learns the claim's handle with `TryGetOwningClaimBasis`, marks the previous handle `Superseded`, `StateBegin`, then NotPlayed / Refused / NoTransition / Unconfirmable (code from `EntryInfo::unreadableWhy` / `Entry::unconfWhy`) / Accepted; `Poll` sets Confirmed / ConfirmedWeak (judged on the RAW fresh `Obs::enterTag`) and NotConfirmed; `Arm` carries only for the SAME handle and idle (`Obs::handle`, raw `carriedEvent`, `entered` stays false so a fresh entry still counts): the play reads `Carried` (10), a fresh entry upgrades it only inside the window (before the verdict line; after it an entry is not this play's evidence, R2-1), an IdleStop sets `kIdleInfo_CarryEnded` (`Entry::carriedOpen` keeps Poll watching), and a carry never ends the claim, as before (review idle-state F1/F2); `EndClaim(cause)` / `Release` / form-0 `OnOwnerChanged` set `endCause` (the first FINAL cause wins, Superseded is not final). Records stay while `IsClaimLive`, a not-live record with no final cause (None / Superseded) is stamped `kIdleEnd_Gone` after `kStateGoneFloorMs` (1000) and its grace restarts (F3), then `kStateGraceMs` (10000) of WORLD time from the last change (`g_worldMsPub`, the atomic mirror of `g_worldMs`); `PruneStates` runs from Poll every 250 ms before the `g_count` pre-gate with `IsClaimLive` OUTSIDE the lock; cap 1024 (oldest ended first). `ResetAll` clears it. **What breaks:** never take another lock or call the engine under `g_stateMx`; never move `NoteRepoint` after the enqueue (a fast drain + play would be overwritten back to Pending); never let `NoteFiled` overwrite (the game thread may have played first); never time the grace on wall clock (a paused client would lose the terminal state); never carry an entry across handles or report a carry as Confirmed (F1); never judge evidence from a log string (F2); open findings APMF-B102 / APMF-B103; a StateUpdate must carry the play's `gen` (a late Poll verdict for an older play must not land on a newer one). | one-shot promote (#0c) |
| `ShoutPower.cpp` | 14 | shout/power select (Num*) | **ARBITRATION-ONLY** — records the voice-slot owner and the chosen shout in `param.form`; makes NO engine write. It previously called `ActorEquipManager::EquipShout` directly (the #0 anti-pattern ch.6/ch.8 were fixed for); the CLIENT issues its own `EquipShout`. `Release` has nothing to undo (corrected 2026-09-07) | arbitration-only (#0); client executes |
| `CombatAction.cpp` | 7 | combat-action category deny (NumpadEnter) | Arbitration + claim lifecycle only; the deny is `core/ActionGate.cpp`'s T1 paired act()/pop() on the 70 combat behavior-tree leaves, for the categories named in `param.ival` (`kCombatActionCat_Offense` today). The test key carries no category, so a test claim denies nothing | claim + T1 enforcement |
| `OfferPackage.cpp` | 9 | package-procedure activity (NumpadSlash) | Arbitration + claim lifecycle + the `EvaluatePackage(true,false)` nudge, `mainthread::Post`ed so it lands PAST the claim's publish; the redirect itself is `core/PackageGate.cpp`'s T3 0x49 hook returning the claim's `param.form`. The test key carries no package, so a test claim offers nothing | claim + T3 enforcement; the ENGINE runs the package natively |
| `Equipment.cpp` | 15 | equip/unequip (Num.) | `GetEquippedObject` + `UnequipObject`/`EquipObject` (melee-vs-ranged lever) | source-block |
| `Detection.cpp` | 16 | stealth (Num8) | `kMovementNoiseMult` + `kDetectLifeRange` AVs | source-block |
| `EquipAuthority.cpp` | 17 | ENGINE-EQUIP facet, WHOLE by default, SCOPED by category from ABI v9 (`kIntent_EquipAuthority`, ABI v7/v8/v9; no test key) | Arbitration + claim lifecycle (standing, no TTL) + the ONE #17a-licensed equip: on every applied `SetEquipSet`/`SetEquipSetEx` for the owning claim (and on a win/repoint, and on a CHANGED `SetEquipScope`) it POSTS one `mainthread` hop that lands after `Publish()` and equips each declared item the actor is not wearing via `ActorEquipManager::EquipObject` (queued, not forced; v8: with the declared hand's `BGSEquipSlot` 0x13F42/0x13F43 by `LookupByID`, "worn" = in THAT hand, same form allowed once per hand) inside `equipsink::ApmfEquipScope`; skips a declared non-governed form type (ARMO/WEAP/AMMO/LIGH only); v9: skips (counts `skipped-unowned`/`skipped-denied`, names once per actor+set signature) any entry whose `equipsink::Categorize(obj, declaredHandEQUP)` bits are not ALL owned or ANY denied — never equips or evicts into an unowned category; since 2026-10-13 the pass UNEQUIPS (`UnequipDenied`, `ActorEquipManager::UnequipObject`, queued, held 3 s per (form, hand)) every worn item whose category the claim DENIES (`DeclaredSetVerdict().blacked`; `would-unequip` under observe-only), also with no declared set; nothing else is unequipped; no re-assert (every declaration walks the inventory; an item APMF already queued is held 3 s from its issue before it is queued again, per (form, hand, uid)); ABI v25: an entry with a uid equips THAT copy (the inventory extra list whose `ExtraUniqueID` matches, passed as `EquipObject`'s extra), "worn" = that list carries `ExtraWorn` (Default/Right) / `ExtraWornLeft` (Default/Left), a copy not carried is `instance-missing`, one worn in the other hand `instance-other-hand` (EquipObject ignores a worn list), never another copy; uid entries do not count toward the per-hand copy count. The deny is `core/EquipSink.cpp`'s call-site seat. `Release` relinquishes (nothing to undo). Refuses `kEquipAuth_DenyUnequip` (reserved). Review findings APMF-B5, B6, B8, B9, B11, B12 in `Docs/REVIEW-BACKLOG.md` are CLOSED (APMF-B7 REOPENED, stays LIVE) 2026-10-06 (superseded by ClickUp 86e3jwmk7, the weighting rework retires the declaration road) | claim + #17a seat; APMF equips the DECLARED set, the ENGINE keeps its hands off |
| `Travel.cpp` | 19 | WALK this actor to a destination — an object REFERENCE (arrival = distance <= radius) or a CELL (arrival = parent-cell identity; the radius is not consulted) — (`kIntent_Travel`, ABI v10; no test key — the crosshair surface can name only ONE ref and ch.19 needs an actor AND a destination, so a hotkey would mean APMF inventing intent) | **ADDS NO SEAT; CLAIMS NO OTHER INTENT.** One `mainthread::Post` hop past `Drain`'s `Publish`, it files ONE internal ch.9 `kIntent_OfferPackage` claim naming an APMF-OWNED Travel package from `Data/APMF.esl`, whose `Place to Travel` Location input `core/PackageData.cpp` points at the destination. TWO kinds, chosen by the FormID's own record type: an object REFERENCE (`kNearReference`, a 4-byte handle -- `SetTravelTarget`) or a CELL (`kInCell`, an 8-byte form POINTER -- `SetTravelCell`). The two write DIFFERENT members of the same 8-byte union, which is why they are separate functions with separate types; both member choices were read off the engine's own locType switch on both unpacked images. Every other locType is REFUSED -- see `Docs/DENY-COMPLETENESS-AUDIT.md` row 19 (h). A world POSITION is REFUSED unless the claim sets `kTravel_ToPosition` (ABI v11), in which case APMF places its own XMarker and runs a REFERENCE leg to it (see What breaks (7)). That offer is travel's IMPLEMENTATION, not a composed client intent; it is filed at the CLIENT's basis (read back through `ControlMap::TryGetOwningClaimBasis`) and RE-FILED — request-new-then-release-old, in one `Drain` — when the winner's basis moves, because a claim's basis is immutable. The leg is ENDED (offer released, package slot freed) by the per-frame monitor `travel::Poll()` on `Arbiter::OncePerFrame` on ARRIVAL (distance for a ref, PARENT-CELL IDENTITY for a cell), on `Actor::IsInCombat() && !combatsubstrate::Suppressing(id)` (a HELD substrate is not a fight, 2026-10-19; one `[travel] ... substrate-held, not combat` line per leg; `Travel.cpp` `Poll`, the only raw `IsInCombat` site in the file; open: APMF-B129, a re-pointed leg does not re-log), or on the destination being gone — plus a 120 s STUCK safety net that logs a failure and retries nothing. **No LOS test, no detection test, no `StartCombat`, no 0xE4 PIN, so INVARIANTS #0 is untouched.** 8 concurrent legs; a 9th is refused and logged. Ships OBSERVE-ONLY | one internal ch.9 offer; the ENGINE runs the package natively |
| `TargetPin.cpp` (+ `TargetPin.h`) | 20 | PIN this actor's combat target (`kIntent_TargetPin`, ABI v13; no test key -- the crosshair surface names one ref and a pin needs an actor AND a target) | **SOURCE DENY:** write_vfunc slot 6 (`SelectTarget`) on `VTABLE_CombatTargetSelectorStandard` and `...Fixed`, chaining; the answer is replaced with the winning claim's target only when the engine answered non-zero and the target is in `combatGroup->targets` (group read lock) and not `kTargetLost`. `targetpin::Poll()` on `Arbiter::OncePerFrame` ENDS the claim (EnqueueRelease, reason logged) when the target is dead / disabled / unloaded / unresolvable or the owner dies; **HELD ENGAGEMENT (2026-10-12, marth: "once an engagement is called in harbinger its move-to, it cannot be lost", INVARIANTS #0 (i)): with the knowledge seats installed, a group entry flagged kTargetLost no longer ends the pin -- the seat declines that update (`TargetPin.cpp:322`); DEGRADED (seats not installed, or `[EngagementHold] bEngagementHold=0`) the old end-on-lost applies (review F-4); in every config a target that WAS a group target and is not any more ends the pin (`kReasonAbsent` `:172`, trusted by Poll like "lost"); Apply / Release / ResetAll register the pin with `combatentry::hold::SetPin/ClearPin`; `targetpin::EndHeld` (`:1123`) is the hold watchdog's end path; open items APMF-B83, APMF-B84**. Character slot 0xE4 = the HARBINGER 1a LAYERS: layer 0 installed at APMF's own SKSEPluginLoad (`targetpin::InstallLayer0`, plugin.cpp, right after `sources::Init`) so later 0xE4 hooks land above it, and a new layer directly above EVERY foreign hook the census finds (`RehookOnTop`, up to 7, census at kPostLoad / kPostPostLoad / kInputLoaded / kDataLoaded / load / 60 s). Each segment is PROVEN single-module (segment 0: layer 0 displaced the engine's own function; segment i: layer i-1 was called from inside the module layer i wraps) or it is a loud ATTRIBUTION DEFECT (allowed + alerted, never enforced). An OVERWRITTEN after the seat answered is attributed to the proven writer of each segment; when the pin claim's priority ([Target]) is strictly higher than every writer's the pin is RESTORED at the outermost layer through the engine's own `CombatController::SetTarget` (AE 33235 / SE 32488 / 1.7.104 raw 0x5614A0, verified rows) plus a plain store of `previousTargetHandle`'s pre-update value (`captured <src> overruled`), else `captured <src> stands`; `Install()` (kDataLoaded, selectors) refuses the pin when layer 0 did not install; an unverified SetTarget = capture without restore. INVARIANTS #0 (f) condition 4 AMENDED 2026-10-11. Open: REVIEW-BACKLOG APMF-B69. Per-actor handle map (`shared_mutex`) filled on the game thread; `ResetAll` at revert + kPreLoadGame. All three seats verified before any is written. ABI v18 `kTargetPin_OwnLineOfSight` (param.ival bit 0): after the lost / absent checks, the pin PAUSES (declines, `notInSight` counted, `[ch.20] ... PAUSED` rate-limited) unless `sightline::Read(actor, target)` is a fresh VISIBLE; refused at the request when the line-of-sight service is not armed, and a Repoint that adds it then is refused too (`ControlMap::ApplyRepoint`, whole Repoint dropped). Open review item APMF-B51 (a) (pin cold start). **What breaks:** the raw selector offset +0x10 is guarded only by the per-call vtable-identity test -- never read it without that test; the only after-the-update write is the Harbinger 1a RESTORE (marth Q1), and it runs ONLY when the seat ANSWERED with the pin this update and a NAMED writer is outranked -- never on a SOURCE SEAT MISS and never against an unattributed writer (that would mask a miss, principle 7); every restore is logged; open review items `Docs/REVIEW-BACKLOG.md` APMF-B26 (cross-thread StopCombat), APMF-B28, APMF-B29 (APMF-B27, MFO hook order, CLOSED 2026-10-06: MFO retired its own write on the pin route). `INVARIANTS #0 (f)` | source-block (DENY) |
| `CombatEntry.cpp` (+ `CombatEntry.h`) | 21 | ENTER COMBAT against a named target (`kIntent_CombatEntry`, ABI v14; no test key -- it needs an actor AND a target) | **ONE ENGINE CALL, NO SEAT:** `Engage` / `OnOwnerChanged` resolve the target on the game thread and `mainthread::Post` ONE task that re-validates the published claim and calls `actor->StartCombat(target, nullptr)` (fork binding, `RELOCATION_ID(37608, 38561)`, 3-arg form verified on both images). Gates in the task: actor loaded, alive, `currentProcess != null` (StartCombat dereferences it unchecked), target loaded / enabled / alive. Release calls NO StopCombat. NEVER live and inert: `EndClaim` (from Pump / Poll, never inside Drain) releases the claim on an engine refusal, an entry that cannot be attempted, "combat ended" (controller POINTER null after a successful entry), owner dead, target dead / disabled / unloaded / unresolvable. Per-actor map touched on the main seat only (no lock); `ResetAll` at revert + kPreLoadGame. Passive rate-limited entry log (entered / group member on a new fight / whether a ch.20 claim names the target). **What breaks:** never call StartCombat with two arguments (garbage R8 = the ch.6 CTD) or for an actor with no process; never re-enter on a timer or when the engine ends combat (that is SUSTAINING a decision, #0); never add a StopCombat on release (an undo, #0 (g) condition 4); never dereference the controller or group here (use the pointer; `IsInCombat` reads [cc+0x43]) -- APMF-B26 (`Docs/REVIEW-BACKLOG.md`); never EnqueueRelease from inside Drain; keep `engineGaveUp` across Apply (a rival claim naming the same target must not re-enter after "combat ended" / "engine refused", #0 (g) condition 3). Open review items: APMF-B26, APMF-B30 (StopCombat+StartCombat inside one Poll window), APMF-B31 (brief 3D loss ends the claim). `INVARIANTS #0 (g)`. **THE HELD ENGAGEMENT (2026-10-12, `CombatEntry.cpp:518`, `namespace apmf::combatentry::hold`, INVARIANTS #0 (i)) lives in this file too:** the hold map (actor -> ch.21 entry target + range, ch.20 pin target, the holder's own handle; `shared_mutex`, written by ch.20/ch.21 Apply/Release/ResetAll on the game thread); the two KNOWLEDGE seats `DetectThunk<0/1>` (`:717`, own `SKSE::Trampoline` write_call<5> post-call at `CombatGroup::UpdateTargetDetection`'s two call sites, rows `EngagementHold.*`, exact 1.6.1170 / 1.5.97; the verified function is stored as the original BEFORE the patch) that, after ANY member's event in a group where a member holds the target (review F-1: the entry is the group's), un-lose it the engine's own way (+0x90 = max(+0x90,+0x94)) and write its real last-known location (the holder-in-this-group test walks EVERY hold naming the target, after the engine's call, no cap -- taunt audit 2026-10-12: holders of one target can sit in two groups); `hold::Poll` (`:1241`, Arbiter, 250 ms) = per-hold `[hold]` telemetry every 5 s, the WINDOWED no-progress watchdog (suspended while the target `IsFlying()`: samples restart each poll; `Track::targetFlying`, 2026-10-18) and the arrived-not-fighting watchdog (`kNoProgressMs` `:848`; only attacks / casts on the HELD target count), marth's RANGE RULE (ch.21 `param.fval` = the client's engage range; out of range and not hostile, no debounce -> `DropNotEngaged` (`:1047`): both claims released + `StopCombatRanked` (`:991`, Character slot 0xE5, ranked, not while another group target attacks the party) + every internal effect cleared in the same pass), the drives (bleed-out, search-elsewhere: a ch.24 internal bound; released loudly when not carried); NO re-engage cooldown (marth 2026-10-12), each end logged with "End #N"; `hold::HeldVia` (`:1222`) is what ch.23 asks. **What breaks (hold):** never write the CombatTarget except AFTER the engine's own update and only the three facts; never key the fix by the event's member (the entry is the group's -- key by "a holder is a member of this group"); never cap or sample the holds naming the target before the group test (a target held from two groups would be lost in one); never hold g_holdMx across the engine's call; never take the group lock in the seat (the listener path runs unlocked in the "Non render-safe AI" job, the group pass under the engine's own WRITE lock -- a lock of ours could invert); never compute the seat ids on 1.7.104 (absent from the MIT table: a fatal id miss); never store the original after the patch; never refuse a request synchronously in ControlMap for a hold reason (MFO reads a synchronous refusal as "seat not installed" and drops its route for the session); never add a re-engage wait (marth: "No waiting ever"); a not-engaged drop must leave nothing re-armed (hold slots, internal bound, ch.23 stamps all cleared in the pass); never end a hold on "lost" while the seats are installed; never StopCombat when another live group target is attacking the party (review R-1: `StopCombatRanked` reads the group under its READ lock first). Open review items: REVIEW-BACKLOG APMF-B82 (internal-bound slot has no owner token), APMF-B83 (moot: no cooldown), APMF-B84 (mid-fight group merge ends the pin). | one-shot engine call (#0 (g)) + held knowledge (#0 (i)) |
| `CombatReentryDeny.cpp` (+ `CombatReentryDeny.h`) | 22 | DENY every engine `StartCombat` for an actor during a bounded window (`kIntent_CombatReentryDeny`, ABI v15; no test key) | **SOURCE DENY at `Actor::StartCombat`'s own self-check:** `IsDeadHook` = write_vfunc on Character slot 0x99 (`IsDead`), chaining; `AnswerSelfCheck` runs only when `_ReturnAddress()` == `g_siteRet` (the verified call-site row `ReentryDeny.StartCombat.SelfIsDeadCall` + 11: AE `0x6B69CA` / SE `0x625248`) and answers "dead" (StartCombat refuses before its spinlock / equip / anything) for the WINNING claim inside its window, EVERY call (new entry and in-combat target-add, closing-round option (a)) except ch.21's own entry (`ClientEntryScope`, thread-local, held by `CombatEntry.cpp::Enter` around its StartCombat). The seat reads no actor state. **HARBINGER 1b:** before refusing, `facetattr::CallerBelow(g_siteRet)` names StartCombat's CALLER (stack scan: Engine / DirectCalls / the calling DLL) and `facetattr::Decide(Combat, winning handle, caller)` refuses only when the claim outranks it; otherwise it passes (`sourcePass`, a `[ch.22] ... PASSES` line); an unnamed caller is the degrade rule (allowed unless `[UnknownSources] Combat=override`). `ObserveEntryClaim` names any non-ch.21 StartCombat on an actor holding a winning ch.21 claim and no ch.22 claim (attribution only). WINDOW: `param.fval` s (0 = 10, clamped 120) from the claim's OWN request / last Repoint -- `NoteRequest` / `NoteRepoint` (called from `ControlMap::EnqueueRequest` / `EnqueueRepoint`, any thread, `g_reqs` under its own mutex); `Apply` (inside Drain) sets a provisional deadline and posts `Settle`, which after Publish reads the winning handle and sets the real deadline (ending a takeover with no time left). Map under `shared_mutex` (unique on the game thread, shared in the seat, which may run on ANY thread). `Poll` (Arbiter seat, 250 ms) ends the claim on "window elapsed" / "owner dead" via `EndClaim` (EnqueueRelease, never inside Drain) and runs the DENY-MISS detector (controller appeared under a live window with no ch.21 pass and no 1b `sourcePass`). `g_siteSeen` + the one-time `seat OBSERVED` log (principle 5); every Engage / Repoint line names the slot-0x99 owner. `ResetAll` at revert + kPreLoadGame (also clears `g_reqs`). **THE SUBSTRATE'S SECOND SITE (2026-10-13, S2a, #0 (j)):** the slot holds a 17-byte stub (`BuildRdiStub`: `mov r8, rdi; jmp [rip+0]; &thunk`, own `SKSE::Trampoline`) so the 3-argument thunk also gets StartCombat's own `this` (rdi); at the TARGET-check return site (`g_targetRet`, verified row `Substrate.StartCombat.TargetIsDeadCall` + 14: AE `0x6B6A0A` / SE `0x625288`) `AnswerTargetCheck` asks `combatsubstrate::RefuseAnchorTarget` (refuse a StartCombat against the ANCHOR by a non-holder). Row or stub refused = the seat is exactly ch.22's (the thunk in the slot) and `TargetSiteInstalled()` is false (the substrate refuses). **THE ANCHOR JOIN GUARD (2026-10-19, SEV-1, `channels/AnchorGuard.{h,cpp}`):** when `anchorguard::InstallSeat(startCombat)` verifies row `AnchorGuard.StartCombat.TargetGroupRegs` (AE +0x23 / SE +0x24 `mov rbp,r8; mov rsi,rdx; mov rdi,rcx`; exact 1.6.1170 / 1.5.97; refused BY NAME on 1.7.104; `[CombatSubstrate] bSubstrateJoinGuard`), `BuildRdiStub(true)` writes the WIDENED 53-byte stub (`mov rax, g_siteRet; cmp [rsp], rax; jne generic; mov r8, rsi; mov r9, rbp; jmp thunk; generic: mov r8, rdi; jmp thunk`) and `g_joinStub` is set before the slot write; the thunk takes 4 args (r8 / r9); at the SELF site, after `AnswerSelfCheck` did not refuse, `AnswerJoinCheck` -> `anchorguard::AnswerJoin` (refuse a NULL-target join into a published substrate group by a non-holder; passive `[substrate] ENTRY StartCombat` trace). Otherwise the 17-byte stub stands, byte for byte. **What breaks:** never make the widened stub's branches anything but JUMPS (the return-address compares, as above); never read r8 / r9 as rsi / rbp unless `g_joinStub` and the return address is the SELF site (elsewhere r8 = rdi and r9 is garbage); never let `AnswerJoin` dereference the group (address compare only: no refcount, any thread) or take an engine lock; never set `g_joinStub` without the row verified (the registers are only proven on the two exact runtimes); never answer at any return address but StartCombat's SELF check and (for the substrate's anchor ONLY) its TARGET check (any other target is another actor's facet; any other site would lie to the whole engine); never read the third argument at any other return address (it is whatever rdi held); never change the stub to a `call` (the return-address compares would see the stub); never read the controller in the seat (the cross-thread StopCombat race the closing round removed); never let the seat call an engine function or take an engine lock (it can run inside a BSJobs detection job); never drop the `ClientEntryScope` in ch.21 (a client's declared entry would be refused by its own deny); never drop `NoteRequest` / `NoteRepoint` from ControlMap (every window would run from apply time again, F3); never add a StopCombat here (the client's call, #0 (h) condition 1); never EnqueueRelease from inside Drain; keep the call-site row's 11-byte check (it is what pins the exact instruction). `INVARIANTS #0 (h)` | source deny (#0 (h)) |
| `PursuitLeash.cpp` | 23 | LEASH an actor's in-combat pursuit + search to an anchor actor (`kIntent_PursuitLeash`, ABI v16; no test key) | Arbitration + claim lifecycle only: `Engage` / `OnOwnerChanged` hand `param.target` (anchor) + `param.fval` (radius) to `actiongate::SetLeash`; `Release` calls `ClearLeash`. The enforcement is `core/ActionGate.cpp` (the ch.23 block: act/pop ForceFail pair + the slot-0x04 update seat on 14 leaves; 2026-10-12: the HELD target's approach is exempt, and the LEASH RETURN walks an un-held, search-denied actor back). Synchronous refusals in `ControlMap::EnqueueRequest`. **What breaks:** see ActionGate's ch.23 note | claim + T1 enforcement |
| `CombatApproach.cpp` (+ `CombatApproach.h`) | 24 | CLOSE to within R of X WHILE IN COMBAT, the in-combat movement goal (`kIntent_CombatApproach`, ABI v19; no test key) | **SUBSTITUTE one unweighted engine input, the actor's standard COMBAT AREA,** through its own vtable (write_vfunc, chaining): S1 vfunc 0x0B Update (restore the engine geometry, run the original, write X/space/R, period 0.25 s), S2 0x05 IsInside(WorldLocation) (engine geometry inside a deny bracket), S6 0x06 observe-only (the engine's own inside/outside verdict for the actor); deny brackets S3 CombatTargetSelectorStandard 0x06, S4 ten invisibility/bound-item CheckShouldEquip 0x0F and S5 CombatBehaviorAttack / Bash act 0x02 (CheckUnreachableTarget deliberately NOT bracketed: part of the movement facet, review R2-1); S3 also puts back (and re-activates) the area of a finished claim on the actor's own thread. `Poll` (Arbiter, game thread) resolves X, decides the state, ends + releases a finished claim; `RestoreBeforeSave` (kSaveGame) | the engine's Movement half walks the actor back into the area (Return To Combat Area) and clamps its moves to it; nothing else is touched |
| `EquipPreference.cpp` | 26 | PREFER items for an actor's hands in combat by LIFTING the engine's own score (`kIntent_EquipPreference`, ABI v22; no test key) | Arbitration + claim lifecycle only (log lines; a force escalation at warn). The LIFT lives in `core/EquipPreference.cpp` `OnScore`, called by both `core/AiCastSeats.cpp` 0x0C thunks; the force deny in `core/EquipSink.cpp` | nothing: the engine scores its items as it does |
| `CombatSubstrate.cpp` (+ `CombatSubstrate.h`) | 27 | COMBAT AS A SUBSTRATE for an out-of-combat animated cast (`kIntent_CombatSubstrate`, ABI v23, slice S2a, `[CombatSubstrate] bCombatSubstrate` default 0; test surface `bSubstrateDryRun`) | **One `StartCombat(anchor)` + Harbinger's own deny set (#0 (j)).** The ANCHOR = `APMF.esl` NPC_ 0x808 / ACHR 0x80A (Nazeem clone, ghost + essential on the base) in holding CELL 0x809, home XMarker 0x80B; reference-counted, AI off (boolBits kProcessMe), `SetAlpha(0)` per move, 72 u BEHIND the shortest-range holder (`BehindOf`, `PlaceAnchor`); collision NOT switched off (CommonLib's `SetCollision` writes form flag 1<<4, marked unverified "?" there -- never write it). Lock-free holder table `g_slots` (16, atomics, id re-read after state) read by: ch.7's act thunk (`ForLeaf` / `LeafDenied`: the ALLOW-list, 3 cast leaves only while a driving cast claim stands, structural nodes pass), ch.22's slot-0x99 target site (`RefuseAnchorTarget`), the hold's detection seat (`AnchorKnowledge`, trigger (a) `NoteGroupTarget`), the census 0x0B seat (`NoteCastFired`), the hit / combat event sinks. Game thread (`Poll`, Arbiter after Pump): `Begin` (validate, rank on CombatAction / Combat / Equip / Detection, slot, anchor, two internal deny-only floors at basis -1e9 with `sources::InheritClient`) -> `Enter` (floors published, one `StartCombat`, `EvaluatePackage(false,false)` FIRST) -> `Serve` (`WriteBitAfterRepick`: `AIProcess+0x139` ONCE after kEvpBuffered clears + one Poll, review F2; floors renewed by Repoint every 10 s, a lapsed floor CLOSES it, F5; `SetAlpha(0)` on the anchor every Poll, F3; close on fire / claim end / release / TTL / death; P3 / P6 lines with the reset attribution) / `DropSub` (REAL bit: bit restored, `CombatGroup::RemoveTarget`; NO StopCombat while the real foe is in the group, an EMPTY group is ended once through `combatentry::hold::StopCombatRanked`, marth 2026-10-13 F6) / `CloseSub` (bit restored, StopCombat once, floors released, anchor home on 1 -> 0). `ResetAll` (revert / kPreLoadGame), Install also requires `combatentry::hold::StopCombatAvailable()` (the empty-fight end; round 2), and the empty-fight StopCombat is followed by `EvaluatePackage(false,false)`. `AfterLoad` (posted at kPostLoadGame / kNewGame: anchor home, the save's anchor targets removed; APPROVED by marth 2026-10-13, review F8). `g_served` is ATOMIC (read by the combat-thread refusal's log, F4). **MUSIC (2026-10-19, ClickUp 86e3pu939; seat form after the tier-3 review of b1c78b2):** the deny and the put-back run ONLY inside the engine's own write-locked update of the group, in the held engagement's detection seat (`CombatEntry.cpp` `DetectThunk` -> `AnchorKnowledge` -> `SeatMusicAnchor`, `NoteGroupTarget` -> `SeatMusicRealTarget`), gated by `WriteLockedHere` (the lock's +0 writer thread == this thread, +4 writer bit). `SeatMusicAnchor`: pure group (every member a live non-real holder, every target the anchor) reading 0xFF -> 0, each member's slot gets `kMusWrote` and a DEBT (`g_debts`, per holder handle, leaf `g_debtMx`); a holder joining a denied group co-owns it (F2). `SeatMusicRealTarget`: a group with a real target, reading 0, holding a debtor -> 0xFF (the merge carries the 0 into an ally's fight, F1; matched by membership, never a stored pointer, F4). Debts: open = no expiry, `DropSub` gives 30 s, `CloseSub` forgets, `AfterLoad` owes one per actor whose saved group kept a real target (F3: +0x15C is saved), `Poll` expires them, `ResetAll` clears. `DetectThunk` stays armed while `MusicDebtPending()`. The game thread never dereferences a group for music (F6); `Serve` logs the first decided state from `Slot::music`. `MusicProbe` (Poll) logs every BSMusicManager change while a substrate is open + 15 s tail. Install: six rows (`Substrate.CombatGroup.MusicWriteLock` / `DetectionPassInUpdate` / `MusicUndecided`, `Substrate.CombatManager.MusicPlayTest`, the two lock-layout rows) on exactly 1.6.1170 / 1.5.97 plus `hold::KnowledgeInstalled()`, or the SUBSTRATE refuses; `[CombatSubstrate] bSubstrateMusicDeny=0` = off with a logged hole per OPEN. **ABI v26 `StateOf`** (any thread, the slot table only): Held = live & !real, Real = live & real, Anchor = the anchor's FormID. `LeaveSlotAndAnchor` names the holder the placement pass will pick when the anchor stood at the leaving one (the old "stays for 0x00000000" log bug). Open review items: REVIEW-BACKLOG APMF-B91..B100 (S2a rounds 1-2), APMF-B122 (music purity gap), APMF-B123 (file split), APMF-B124..B128 (music debt edges, seat logging), APMF-B132..B136 (R1 probes). **2026-10-19 (field: every ch.19 walk ended `kLeg_CombatCancelled` as the dry run opened):** `Suppressing` is now ALSO read by ch.19's `travel::Poll` (a HELD substrate no longer cancels a travel leg; the REAL bit makes it false, so real combat still does). `DryRunSchedule` picks only truly idle teammates (`NotIdleReason`: no ch.19 / ch.9 / ch.1 / ch.12 owning claim from any client, `GetSitSleepState() != kNormal`, in dialogue with the player (`MenuTopicManager::speaker` +0x68 == the actor's handle, the engine's own IsInDialogueWithPlayer test; verified rows `Substrate.MenuTopicManager.Reader` / `.Singleton`, refused by name on 1.7.104 where every teammate then counts as "dialogue unknown, not idle"; NPC-to-NPC talk is NOT tested), `GetOccupiedFurniture()`); skips are logged once per round. `BystanderCensus` (Poll, `[CombatSubstrate] bSubstrateBystanderCensus`, default 1): PASSIVE change-only `[substrate] CENSUS` lines for actors that are not the player, the anchor or a live holder (non-holder TEAMMATES included since 2026-10-19), within 2048 u of a holder or (any distance) whose controller targets the anchor (`target-is-anchor` field; such an actor keeps the window open), window = open + 10 s, 60 lines per window; plain member reads only (no hook, no write, no address id). **THE ANCHOR GUARD (2026-10-19, SEV-1: a non-holder teammate fought the anchor; `channels/AnchorGuard.{h,cpp}`, NEW):** `Poll` calls `anchorguard::Sweep(now)` in BOTH branches (after `MusicProbe`): game thread, every frame while `AnyLive()` and 10 s after; `ForEachHighActor`, each controller's target and its group's targets / members read under `BSReadLockGuard(g->lock)` (handles copied, resolved after the lock), no group pointer kept past the frame; it PUBLISHES the substrate groups (no non-anchor target; a Held holder member or the anchor listed) into 32 atomics the join guard's seat compares by address, and treats every non-holder whose controller targets the anchor or whose group lists it: ERROR `[substrate] ANCHOR LEAK`, `RemoveAnchorFromGroup` (this file's `g_removeTarget`) ONLY when no Held holder shares the group, `combatentry::hold::StopCombatRanked(actor, kInvalidHandle, anchor)` when nothing but the anchor is left (in a holder-shared group: StopCombat only); no StopCombat while a real target is in the group, but a HOLDERLESS group loses the anchor entry whatever else it fights (review F1 of ace1d8b; a holder-shared real group waits for the holder's drop); a StopCombat that ended the fight is followed by `EvaluatePackage(false,false)` (`StopAndRepick`, the substrate's verified row, F3); a leak extends the tail; the stub pool is 128 bytes (both stubs fit; `Trampoline::allocate` report_and_fails, F2); open review items APMF-B137..B140; the player is logged only; one attempt per actor per 500 ms, lines every 5 s. `CloseSub` reads `anchorguard::GroupMembers(actor)` BEFORE the holder's StopCombat and calls `anchorguard::AfterClose` after it; `ResetAll` calls `anchorguard::ResetAll`. **What breaks:** open review items APMF-B129 / B130 / B131 (census and ch.19 SEV-5s, see Docs/REVIEW-BACKLOG.md); the census is another reader at the APMF-B26 seat; never let ch.19 (or any consumer of "in combat") go back to a raw `IsInCombat()` while a HELD substrate can stand; never turn the census into a write or give it a new address id (it must stay passive); never read through a `CombatGroup*` in the census (addresses are only compared); never let a combat-thread seat read a controller or the game-thread map (the slot table only); never set the REAL bit anywhere but `MarkReal` (it must be the FIRST write: every deny reads it first); never StopCombat on the drop path (#0 (j) condition 5); never call StartCombat before both floors are PUBLISHED (StartCombat's re-arm equip runs inside the call); never deny a structural node (the cast leaves sit under the selectors, AE `0x8A0720`); never re-assert the ignoring bit in S2a (that is S2b, after P6); never file the floors from inside Drain (Engage only records; `Begin` runs from Poll); never add a raw call-site probe for the ignoring-bit readers (#17: the OBSERVED line says "not instrumented"); never read or write +0x15C anywhere but the seat under `WriteLockedHere` (a CombatGroup has no refcount: only the engine's own locked update of it proves it alive), on a group with a member that is not a live holder or a target that is not the anchor, or over a value that is not 0xFF (overwriting a decided 1 would mask a real fight's music); never put back anything but a 0 in a group with a real target and a debtor member; never key a debt on a group pointer (the merge moves the 0; a freed address is reused); never drop `MusicDebtPending()` from `DetectThunk`'s gate (a dropped holder's debt would never be paid); never build the music rows' RELOCATION_IDs on 1.7.104 (an id miss is fatal there); `StateOf` must stay lock-free (MFO calls it per actor per tick on its worker). **R1 probes (2026-10-19, `SubstrateProbe.cpp`, next row):** this file makes ONE-LINE call-outs only (`BeforeDryRound` / `Requested` in `DryRunSchedule`, `BeforeEntry` before the entry `StartCombat`, `Opened` after the OPEN, `RepickNotRun` in `WriteBitAfterRepick`'s blind-write branch, `OtherReset` at Serve's OTHER reset attribution, `Closing` at the top of `CloseSub` / `DropSub`, `Poll` right after `DryRunSchedule` (before the early return), `Reset` in `ResetAll`, `Install(g_dialogueOk)` in `Install`); the probes never see `Sub` or the slot table. Never move probe logic into this file (it is over the planning limit, APMF-B123), never make a call-out depend on the probe's answer, and keep `Poll`'s call before the early return (the post-close timers run with nothing open). | Harbinger-owned substrate; the engine's own cast road + the actor's own package |
| `SubstrateProbe.cpp` (+ `SubstrateProbe.h`) | 27 | PASSIVE field probes for the substrate (round R1, `[CombatSubstrate] bSubstrateProbes`, default 1 while in test) | **Reads only: no write, no hook, no address id (#17).** Game thread, from `combatsubstrate::Poll`'s seat (APMF-B26 quiescence). Own state: `g_win` (per-holder window, at most 32: stage before / open / after, 250 ms position samples -> speed + distance-to-player Stats per stage, the package id / start time of this and the previous Poll, the aggressor bit before the entry / close / +1 s / +5 s, the weapon state after the close) and `g_pre` (the last second before a dry-run round: every loaded teammate sampled every 250 ms, at most 16 actors x 8 samples; a pick's samples become its "before" stage). Lines: `MOVE-BASELINE` at close + 3 s, `PROBE after-close` when the +5 s read and the sheathe watch (until sheathed or 10 s) finish, `PROBE re-pick NOT run` once per window, `PROBE other ignoring-bit reset` at most 8 per window. Fields and how each was verified: the file header (processLevel +0x137 and the ActorPackage +0x24 / TESPackage +0x20 flag words from the AE / SE disassembly, weaponState at ActorState +0x0C bits 5..7 from both binaries; static_asserts at the bottom of the anonymous namespace). **What breaks:** never add a write, a hook or an address id here (it must stay passive); never dereference a `CombatGroup` (the controller pointer is only null-tested); never read MenuTopicManager unless `Install` was told its rows verified; never call it off the game thread (no locks); a window must always end (the unopened 5 s and no-close 60 s expiries) or the 32-window cap fills. | none (observation only) |
| `CastGuard.cpp` | 25 | GUARD an actor's own casts: hold OFFENSIVE casts while a friendly is in the line of fire, bound the AI's own concentration channel per hand (`kIntent_CastGuard`, ABI v21; no test key) | Arbitration + claim lifecycle only (log lines). The DENY lives in `core/CastGate.cpp` 0x0A (hand casters) and `core/CastSeats.cpp` 0x06 through `core/CastGuard` (see that section) | nothing: the engine's own cast simply does not start, or its release / channel tick is refused |

- **Harbinger-internal bounds (open item APMF-B82: one slot per actor, no owner token) (2026-10-12, `CombatApproach.cpp:290`, `ApplyInternal` `:410`, `SetInternalBound` `:1337`,
  `ClearInternalBound` `:1354`):** the held engagement's bleed-out drive and ch.23's leash return ask S1 to carry "within R
  of X" through the same write + `g_touched` record (so the same deny brackets, put-back, F3 re-activation and save
  restore). S1 consults them ONLY when the actor has no live ch.24 claim (the client's declared area wins); 1b ranks the
  owner claim handle against the area's source. **What breaks:** never hold `g_entryMx` and `g_intMx` together in S1 (it
  unlocks the entry map before `ApplyInternal`); `ClearInternalBound` marks F3 only when no client claim is live.
- **What breaks (ch.24 `CombatApproach.cpp`, ABI v19):** (1) S1 MUST put the engine geometry back BEFORE calling the
  original Update (the standard area's centre is STICKY: the engine recomputes it only under conditions, so it must
  always run on its own last values) and only when the area still carries exactly what S1 wrote (bit-exact + the same
  controller); never write a field without the per-call vtable check (`== g_vtStd`). (2) The deny brackets are what
  keep the bound out of the TARGET and CAST facets: S2 answers from the engine geometry only while `t_view > 0`, and the
  seat set installs ALL or NOTHING (a bound without its brackets is a leak). (3) Never put an APMF object into the
  controller's areas array or its current-area pointer: the area is saved by index and by its own fields, which is why
  `RestoreBeforeSave` runs at kSaveGame. (4) The seats never look up a form: X is a position + cell space resolved by
  `Poll`. (5) The stall end (3 s, a floor over the 0.25 s period) is the principle-7 report of an engine that stopped
  carrying the bound; do not turn it into a retry. Addresses and the 1.7.104 compare: `Docs/VERIFIED-ADDRESSES.md` "ch.24";
  RE log: scratchpad `agentlogs/apmf-combat-moveto.md`. Shared slots: ch.20 also hooks selector 0x06, EquipGate also
  hooks four of the 0x0F slots, ActionGate the Attack / Bash / FindWeapon act() 0x02 (it hooks all 70 leaves); all chain, any order. (6) Every NON-movement
  reader of the area must stay inside a deny bracket (S3 target score, S4 inv/bound casts, S5 attack/bash pick and (APMF-B57) the FindWeapon act() search;
  CheckUnreachableTarget is movement, review R2-1); the full reader list by tree half is `Docs/DENY-COMPLETENESS-AUDIT.md` row 24 --
  a new reader found later is a deny hole until bracketed. (7) Stall / verdict / purge run on the WORLD-TICK clock (a
  menu pause must not end a claim). (8) An area of a FINISHED claim is put back (and re-activated, if the engine's failed
  path deactivated it) only by S3 on the actor's own combat thread, or by RestoreBeforeSave. (9) HARBINGER 1b: S1 writes the bound only
  when `facetattr::Decide(Combat, e.owner, g_s1Src)` enforces (`g_s1Src` = the census's displaced module on the Update
  seat, else Engine); otherwise `kSeatStood` (Poll reports YIELDED, the heartbeat counts "not written by priority").
  A yield to a higher-priority area is named by `AttributeYield` (Poll, game thread) after the RUNNING package's plugin.
  Step 1's mismatch on the same controller is the DRIFT DETECTOR (`Unattributed`, not undone). Open review items:
  `Docs/REVIEW-BACKLOG.md` APMF-B55 (leash graze: DECIDED 2026-10-11, Poll clamps the goal point to `kLeashClampMargin` inside the leash when X is outside it but the R-disc touches it), APMF-B56 (save thread), APMF-B57 (FindWeapon: DECIDED 2026-10-11, exempt via an S5 bracket on its act(); the path to the weapon is still confined),
  APMF-B60 (slow-time stall floor), APMF-B61 (put-back waits on the selector; its exclusive-lock scan).
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
  KeepOffset (the DENY gate) use Address-Library IDs (#8), refused at Engage (loud) and a
no-op at Release off exactly 1.6.1170 / 1.5.97 / 1.7.104, VR included (G1 2026-10-04).
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
  on an exact build. Passive: it must never write anything. GATE PROBE 2 (`[Travel] bGateProbe2`,
  Travel.cpp:1287 header, `P2Locate`:1376, `P2Graph`:1491, `P2Collision`:1518, `GateProbe2`:1553, armed
  :2135, called :2425 right after `GateProbe`; the destination is captured BEFORE `EndLeg` because EndLeg
  deletes a point leg's marker): navmesh at stall and destination (safe by GAME-THREAD serialisation, not the
  cell `spinLock`, which guards only the mesh array; obstacle cuts are copy-on-write into a NEW NavMesh; each
  mesh is held by a `BSTSmartPointer` while read; ONE 262144 triangle+vertex budget per probe), then DOOR/ACTI/Obstacle refs near the stall->destination segment by bound box (collected under the
  cell lock, measured after), with graph state and Havok body. Raw offsets: spec.json layout_facts (the five
  probe-2 rows); every object is vtable-checked against a self-check row before a member read. Travel.cpp is
  2494 lines: the next addition there is a STOP (2500 cap). A Travel.cpp split is REQUIRED before any
  further Travel work (REVIEW-BACKLOG APMF-B33). Deferred review items:
  REVIEW-BACKLOG APMF-B24 and APMF-B25 (read B25 R2-2 before touching `OnOwnerChanged`'s refusal
  teardown). `GetLegState` tests the caller's size against the FROZEN `kTravelLegInfoV12Size`
  (72), never `sizeof` — appended fields are written only inside the caller's size.
  Its open review findings are `Docs/REVIEW-BACKLOG.md` APMF-B13..B18 — read them before
  editing. Its deny holes are stated in
  `Docs/DENY-COMPLETENESS-AUDIT.md` row 19 — read them before editing.

### NOT built (probe-gated GAPs — do not add without a live probe)
**Harbinger 1b observe slice (2026-10-12), facets with NO virtual seat -- "no seat; poll-only later":**
(a) MOVEMENT (ch.1): `Actor::SetDontMove` (AE 37489 / SE 36490) and `KeepOffsetFromActor` (37894 / 36870) are
non-virtual engine functions (the fork's Actor.h declares no virtual setter for either; `MovementControllerNPC`
exposes only unnamed vfuncs, INVARIANTS #8). Attributing their callers needs a call-site patch (INVARIANTS #17,
the 1a DetectionProbe precedent), so there is no seat. (b) the DETECTION VALUE producer (SmartNPC H2 shape, the E8 at
AE 0x7B0D8B / SE 0x719EB6) is a direct call, no vfunc: no seat (its removal is the 1a review's). The detection
ACTOR VALUES ch.16 writes ARE seen, by `core/ActorValueObserve`. (c) The other poll-only rows of the plan (stance,
draw, headtrack, dialogue, idle, shout) were out of this slice's scope.
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
