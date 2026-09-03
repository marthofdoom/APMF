# APMF STATUS — living handoff (start here)

Updated 2026-09-02. The current state of the build: what's shipped, what's
probe-gated, what's next. Keep this current in the SAME change as any
build/finding/workflow change.

## Where we are

**Framing (marth 2026-09-02): APMF is a MODERATOR — it ARBITRATES + DENIES, it NEVER
generates behavior (design.md §1a, INVARIANTS #0).** Its only lever on the engine is
DENY (suppress the losing source at its source). It calls NO behavior-generating engine
function (`StartCombat`, `CastSpellImmediate`, movement drive, anim trigger); the CLIENT
executes behavior with its own proven mechanisms and APMF just makes it win. Once it owns
a facet, nothing else reaches it except through APMF; a re-assert loop is a FAILED block.
**ch.6 (combat-target) and ch.8 (casting) are now ARBITRATION-ONLY** — the client commands
the target / selects the spell; APMF only records the claim. (A CTD from a ch.6
`StartCombat` executor is the cautionary case that fixed this drift — see INVARIANTS #0.)

**Phase 1 is built and on `main`: the MULTI-NPC arbiter + the real C-ABI client
API + the full documented channel catalog.** This replaces v0.1.0's single
crosshair-captured target.

- **Multi-NPC control map** (`core/ControlMap`): a hash map keyed by NPC FormID →
  that NPC's control state (engaged channels + per-channel client claims + captured
  package). Any number of NPCs controlled independently and simultaneously.
  - **Performance (#13):** the `0xAD` hook calls `OnActorUpdate` for EVERY NPC every
    frame; an uncontrolled NPC pays an `empty()` check + ONE hash lookup that misses,
    nothing else. Only a controlled NPC runs its channels (most no-op).
  - **Threading — single-writer (#12):** client `Request`/`Release` (any thread)
    only ENQUEUE a POD op under a brief lock; the map is mutated ONLY on the game
    thread — `Drain()` (once/frame, from the PlayerCharacter `0xAD` seat) applies the
    queue, `ReleaseAll()` clears. The per-NPC hot path reads lock-free.
- **Client API (Layer 2) is REAL** (`APMF_API.h` + `core/ClientAPI.cpp`): an
  inter-plugin C-ABI. A separate client DLL (MFO — soon a mandatory prerequisite)
  gets a POD struct of function pointers via the exported `APMF_GetInterface`, and
  calls `Request(actorFormID, intent, basis) -> handle` / `Release(handle)` — or
  `RequestEx(…, const APMF_Param*)` (ABI v2) to name WHICH thing (cast-select's spell,
  combat-target's target). No C++ class / STL / vtable crosses the boundary. `basis`
  arbitrates same-channel
  same-NPC (higher wins; tie → earliest); the channel stays engaged until the LAST
  claim releases. APMF holds ZERO client-specific code (#14); the header + the query
  fn are the ONLY seam. `APMF_API.h` is APPEND-ONLY forever.
- **Full movement block** (ch.1, the reference channel done right): `SetDontMove`
  alone (v0.1.0) blocked translation but not the move INTENT — run-in-place +
  teleport-snap. Now `KeepOffsetFromActor(self, offset 0)` nulls the move GOAL at the
  source (planner sees "already there", produces no locomotion) PLUS `SetDontMove`
  locks translation. Result: a clean stand-still — no walking, no run-in-place, no
  snap. Both Address-Library bound (verified IDs, #8), package left current.

Full nav: `MAP.md`. Design: `design.md` + `Docs/ARCHITECTURE.md`. Rules:
`Docs/INVARIANTS.md`. Per-channel catalog: `Docs/CHANNEL-MAP.md`.

## Built — the FULL documented catalog (first-release baseline, 13 channels)

The first release ships the full commonly-documented catalog as a baseline
benchmark (MFO will exceed it immediately). Each is a small self-registering module
exposed through an `APMF_API::Intent`. Test surface: aim the crosshair at an NPC +
the key ADDS it to the controlled set; aim another + a key adds it too; **Numpad0
releases ALL**. Logs to `Data/SKSE/Plugins/APMF.log` (`[ctl]`/`[obs]`/`[test]`/`[api]`).

| Key | Ch | Facet | Kind | Mechanism |
|-----|----|-------|------|-----------|
| Num1 | 1 | movement FULL block | source-block | `KeepOffsetFromActor(self)` + `SetDontMove` |
| Num2 | 11 | disposition (4 AVs) | source-block | aggression/confidence/assistance/morality |
| Num3 | 5 | headtrack look-up | **known-incomplete block** | own point slot; Tick re-assert (flagged) |
| Num4 | 8 | casting CLAIM | arbitration-only (#0) | records owner; CLIENT selects the spell + fires (no APMF write) |
| Num5 | 4 | weapon draw | one-shot | `DrawWeaponMagicHands` |
| Num6 | 10 | dialogue pause | one-shot | `PauseCurrentDialogue` |
| Num7 | 1a | gait scale (x0.5) | source-block | `kSpeedMult` AV (arbitrary factor) |
| Num8 | 16 | stealth (silent+keen) | source-block | `kMovementNoiseMult` + `kDetectLifeRange` |
| Num9 | 3 | sneak/crouch | one-shot promote | `NotifyAnimationGraph(SneakStart/Stop)` |
| Num- | 6 | combat-target CLAIM | arbitration-only (#0) | records owner; CLIENT commands the target (no APMF combat call) |
| Num+ | 12 | idle/animation | one-shot | `NotifyAnimationGraph(IdleForceDefaultState)` |
| Num* | 14 | shout select CLAIM | arbitration-only (#0) | records owner; CLIENT selects via its own `EquipShout` (no APMF equip call) |
| Num. | 15 | unequip weapon | source-block | `GetEquippedObject`+`Unequip/EquipObject` |

Every channel keeps the package coherent (no substitution) and restores state on
release / disengage / pre-load-game. Only Headtrack re-asserts (flagged #2). ch.2
facing is not a separate channel — it rides the movement gate.

## Fix pass (trailing review, folded in)

- **C-ABI exception guard (#14):** `APMF_Request`/`APMF_Release`/`APMF_GetInterface`
  each wrapped in `try/catch(...)` — no throw crosses into the client DLL.
- **FormID threaded through Engage/Tick/Release:** channels key per-NPC state by the
  `id` (not `actor->GetFormID()`), so a null/deleted actor still cleans + restores —
  no state-map leak, contract met.
- **KeepOffset reloc IDs VERIFIED** (36870/37894, 36871/37895) against shipping SKSE
  source with the identical signature + verbatim SetDontMove anchor (#8).
- **AV clobber guard:** the ledger stores `{prev, applied}` and restores `prev` only
  when the AV still equals `applied` (else the newer external value wins).
- **Co-save record versioning (v0.2.2):** adding `applied` changed the record layout
  12→16 B, so `kRecordVersion` is bumped to 2 and `Load` branches per version — a v1
  record reads its 12-byte entries and restores UNCONDITIONALLY (no `applied`); a v2
  record uses the clobber guard. A reader per version is kept forever (#15).
- **Log hex formatting (v0.2.2):** on the deck every `{:08X}` rendered as raw garbage
  bytes (corrupting the log to binary) while decimal/strings were clean; the same
  toolchain formats `{:X}` fine for MFO, so the trigger is APMF-build-specific and not
  statically isolable (typo/encoding/arg-type/config/formatter all ruled out). Robust
  fix (#16): all hex now formats via `apmf::log::Hex()` (manual ASCII hex, logged
  through the clean string path); no `{:X}` spec remains in any log call.
- **AvLedger hardening:** `Load(intf, version)` threads the record version; `Save`
  checks `WriteRecordData` and logs on failure.
- **Equipment save-safety:** decided + documented (#15) — only AV channels are
  co-saved; Equipment must not be held across a save (self-heals via AI re-equip).

## Save/load safety (Phase 1)

- **Persisted AV overrides are CO-SAVED (#15).** The AV channels (disposition, gait,
  detection) route every write through `core/AvLedger` (co-saved via SKSE
  serialization), so a save-while-engaged + reload restores the AV regardless of
  live engaged-state and never strands it. `kPostLoadGame` sweeps + clears;
  `OnRevert` wipes ledger + control map.
- **No stale-pointer deref:** every Release resolves the actor FRESH by FormID
  (`LookupByID`) / handle — never a cached raw pointer — so kPreLoadGame with a
  torn-down actor is safe.
- **VR:** input test surface is NOT armed on VR (hooks refuse; no drain seat).
- **Headtrack Release** now actively CLEARS the point slot
  (`ClearActionHeadtrackTarget`), not just stops re-asserting.

## Post-first-release GAP work (do NOT attempt without a live probe)

Marked GAP in `Docs/CHANNEL-MAP.md`; deliberately left for after the first release:
- Combat ACTIONS behavior tree (ch.7).
- Casting TRIGGER suppression (ch.8, no documented suppressor).
- Headtrack all-types FULL block (ch.5) — block the AI's headtrack write at the
  `0xAD` hook so the re-assert stopgap can be removed.
- Sustained package-procedure activities (ch.9).
- Facial-expression setter (ch.13, not exposed in this CommonLib build).
- Per-request FORM/target params in the API (a v2 addition): ch.8 left hand, ch.12
  a specific `TESIdleForm`, ch.14 an arbitrary shout, ch.15 an arbitrary item, ch.6
  an arbitrary target. Today those channels use a fixed demo form/target (the
  CastingSelect-Firebolt precedent); the mechanism is bound and ready.

**Movement PROMOTE is NOT a blocker.** Once the movement source is BLOCKED, driving
the walk is uncontested; wiring the promote feed (`IMovementDirectControl`) is a
driver choice for MFO integration, not a mystery.

**▶ 0x49 PACKAGE-OFFER PROBE (throwaway, on `main`; NOT wired to any client, NOT a
travel/nav build).** Demystifies the ONE intentional package-tier promote (design.md
§5a / INVARIANTS #3): hook `Actor::CheckForCurrentAliasPackage` (vfunc **0x49**) on
`VTABLE_Character` ONLY and return a client's package for a claimed actor → the engine
runs it natively. `native/core/AliasPkgProbe.{h,cpp}`; installed at kDataLoaded after
the 0xAD hook; game-thread eval pump on `Arbiter::OncePerFrame`; test hotkey DIK `0x57`
(F11) toggles a single-actor offer claim on the aimed NPC + `EvaluatePackage(true,false)`
(RELOCATION_ID 36407/37401, resetAI=false). **Phased:** Phase 0 (armed now, no claim,
no package needed) answers the make-or-break — *does 0x49 fire?* (census logs hit count
+ thread + returned pkg; **0 hits ⇒ devirtualised/inlined ⇒ mechanism DEAD, stop**);
Phases 1-3 (engage/release/save-load) now ARMED — `kProbePackageForm` is set to
`DefaultSandboxCurrentLocation256` (Skyrim.esm `0x000956B8`, a vanilla radius-256
current-location sandbox, verified by parsing Skyrim.esm's own PACK group directly,
not guessed), engage/release logs the `ExtraAliasInstanceArray` size before/after
(must be UNCHANGED), and `kPreLoadGame` now drops the claim with no engine call
(Phase 3, no latch). For marth to field-test on Cicero (owner quest 0x0009BE51) or
any generic NPC, 3 deck cycles.

**▶ T1/NATIVE-BIT PROBES (throwaway, on this branch; see `Docs/PROBE-ALLOWANCE.md` for
the full hotkey map, method, and pass/fail criteria; all probe/test hotkeys are numpad
— F-keys are occupied by the game/modlist).** T1 = combat behavior-tree leaf
`Enter`/act (slot 0x02, all 70 `VTABLE_CombatBehaviorTreeNodeObject_*` leaves,
`core/T1Probe.{h,cpp}` + the local `core/CombatBehaviorRE.h` RE:: extension since
CommonLib doesn't ship these classes) — Phase 0 OBSERVE (NumpadEnter claim, shared
with the 0x49 probe) + Phase 1 DENY the Attack leaf via a runtime-derived `SetFailed`
(disassembled from `ForceFail`'s own body, NumpadSlash toggle). Native-bit = a plain
`kAttackingDisabled`/`kCastingDisabled` toggle on the aimed NPC, no hook
(`core/NativeBitProbe.{h,cpp}`, Numpad1/Numpad2). Both field-test-first,
hotkey-driven, NOT wired to any client.

**T4 (`TESActionData::Process` body-command seat) was built, field-CRASHED, and
REMOVED (2026-09-03).** Its devirtualised fallback (`SKSE::GetTrampoline().
write_call<5>` at valhalla's known call site) collided with SCAR.dll's own hook on the
same AI attack-start path — execute-AV in ordinary combat, not even during a probe
keypress (the patch was live from `Install()`). Full crash record in
`Docs/PROBE-ALLOWANCE.md` "T4 — DEFERRED". Not a coverage dead end: T1 already covers
combat body-commands (chain-safe `write_vfunc`), so this falls through to T1 rather
than opening a gap; only the non-combat body-command slice (sneak/draw/activate/idle
OOC) still awaits a chain-safe seat. **New standing rule from this crash:**
`Docs/INVARIANTS.md` #17 — vtable hooks (`write_vfunc`) ONLY, no raw call-site patches
ever, ADAPT to a redundant alternative seat rather than degrade when a preferred one
is contested/absent/devirtualised. Also recorded in `design.md` §10.

## Allowance channels ch.7 / ch.9 graduated (2026-09-03)

The two field-proven allowance probes (`Docs/PROBE-ALLOWANCE.md` — T1 combat
behavior-tree leaf deny, and the 0x49 package-offer redirect) are now REAL,
API-driven channels; the throwaway hotkey-claim surface (`T1Probe`,
`AliasPkgProbe`, `ProbeClaimSet`) is REMOVED — never left installed alongside
the real channels on the same vtables (`Docs/INVARIANTS.md` #17).

- **ch.7 combat-action** (`native/channels/CombatAction.cpp` arbitration +
  `native/core/ActionGate.cpp` enforcement): `kIntent_CombatAction`,
  `APMF_Param.ival` = an `APMF_API::CombatActionCategory` bitmask of leaf
  categories to DENY (starts with `kCombatActionCat_Offense`, append-only for
  future categories). Reuses the proven T1 mechanism verbatim: `write_vfunc`
  slot 0x02 on all 70 `CombatBehaviorTreeNodeObject_*` leaves, the `+0x158`
  A‖B actor-resolution, and `CombatBehaviorForceFail`'s own original `act()`
  as the deny call. A leaf is denied only if its classified category bit is
  set in the winning claim's mask; unclassified leaves (movement/defense/
  utility) are never looked up at all.
- **ch.9 offer-package** (`native/channels/OfferPackage.cpp` arbitration +
  `native/core/PackageGate.cpp` enforcement): `kIntent_OfferPackage`,
  `APMF_Param.form` = the `TESPackage` FormID to offer. Reuses the proven
  0x49 mechanism verbatim: `write_vfunc` on `VTABLE_Character[0]` slot 0x49,
  never-null fallback to the engine's own answer, one
  `EvaluatePackage(true,false)` nudge on Engage/OnOwnerChanged/Release (now
  driven by the Channel lifecycle instead of a hotkey-queued op).
- Both intents (`14`/`15`) and the `CombatActionCategory` enum are
  append-only additions to `APMF_API.h` — no existing field/enum value
  changed. Neither channel needs bespoke `kPreLoadGame` handling: the generic
  `ControlMap::ReleaseAll` already calls every channel's `Release()`.
- **Not yet field-tested** (built + CI-green only, same as every prior
  graduation before its own deck pass) — marth reviews the diff before Pass B.

## Client API (Layer 2) — REAL

`APMF_API.h` (the shared header) + `core/ClientAPI.cpp` (the impl). A client:
`GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface")` → `fn(kABIVersion)`
→ a `const APMF_API_v1*` (null on ABI mismatch); check `p->abiVersion` and cast up to
`APMF_API_v2*` (>=2) or `APMF_API_v3*` (>=3) → `Request/RequestEx/Release/Repoint`.
`RequestEx` carries the POD `APMF_Param` (`form`/`fval`/`ival`) — cast-select reads
`param.form` as the spell (no param → Firebolt), combat-target as the target (no param
→ player). `Repoint(handle,param)` re-points a live claim in place (same handle) — the
retarget primitive (combat-target switches the held foe without release/re-request).
Forwards to `ControlMap` enqueue (the SAME path the hotkeys use — one control path).
Frozen, append-only (#14/#14a): each ABI = a prefix-extension struct, `kABIVersion = 3`.

## Build / CI

- Compile is **CI-only** (GitHub Actions, `native.yml`, on `native/**`):
  `gh run list -R marthofdoom/APMF`. Windows + vcpkg + colorglass CommonLibSSE-NG,
  same toolchain/baseline as MFO.
- CMake **GLOBs** `native/**/*.cpp` so a new channel needs no build-file edit.
- Pinned CommonLib API-surface gotchas + the verified Address-Library IDs (movement
  block, StartCombat) are in INVARIANTS #8 (verified against the fork's headers, not
  memory).

## Next

1. Field-test the 13 channels on the deck (marth): confirm the multi-NPC test
   surface (freeze 3 different followers independently, release-all), PACKAGE STABLE
   per NPC, the clean stand-still (no run-in-place / snap), and the AV/casting gates
   on a package-locked follower.
2. MFO integration (Phase 3): MFO becomes the first client, calling `APMF_API.h`.
3. Probe the GAP channels (movement PROMOTE first) on a live runtime.
4. Phase 2 passive logger.
