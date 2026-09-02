# APMF STATUS — living handoff (start here)

Updated 2026-09-02. The current state of the build: what's shipped, what's
probe-gated, what's next. Keep this current in the SAME change as any
build/finding/workflow change.

## Where we are

**Framing (marth): APMF is THE GATEKEEPER.** Once it owns a channel on an actor,
nothing else reaches that facet except through APMF. Each channel BLOCKS the foreign
input at its source so nothing competes — a re-assert loop is a FAILED block. The
arbiter/registry is centered on being the gate.

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
  calls `Request(actorFormID, intent, basis) -> handle` / `Release(handle)`. No C++
  class / STL / vtable crosses the boundary. `basis` arbitrates same-channel
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
| Num4 | 8 | cast selection (Firebolt) | source-block | own `selectedSpells[R]` + caster |
| Num5 | 4 | weapon draw | one-shot | `DrawWeaponMagicHands` |
| Num6 | 10 | dialogue pause | one-shot | `PauseCurrentDialogue` |
| Num7 | 1a | gait scale (x0.5) | source-block | `kSpeedMult` AV (arbitrary factor) |
| Num8 | 16 | stealth (silent+keen) | source-block | `kMovementNoiseMult` + `kDetectLifeRange` |
| Num9 | 3 | sneak/crouch | one-shot promote | `NotifyAnimationGraph(SneakStart/Stop)` |
| Num- | 6 | combat-target STEER | promote (steer, not pin) | `StartCombat(player)` / `StopCombat` |
| Num+ | 12 | idle/animation | one-shot | `NotifyAnimationGraph(IdleForceDefaultState)` |
| Num* | 14 | shout select (Unrelenting Force) | one-shot (sticky) | `ActorEquipManager::EquipShout` |
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
- Combat-target PIN (ch.6, block the threat re-selector — we only STEER today).
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

## Client API (Layer 2) — REAL

`APMF_API.h` (the shared header) + `core/ClientAPI.cpp` (the impl). A client:
`GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface")` → `fn(kABIVersion)`
→ a `const APMF_API_v1*` (null on ABI mismatch) → `Request/Release`. Forwards to
`ControlMap` enqueue (the SAME path the hotkeys use — one control path). Frozen,
append-only (#14).

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
