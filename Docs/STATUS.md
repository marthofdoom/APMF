# APMF STATUS — living handoff (start here)

Updated 2026-09-02. The current state of the build: what's shipped, what's
probe-gated, what's next. Keep this current in the SAME change as any
build/finding/workflow change.

## Where we are

**Framing (marth 2026-09-02): APMF is THE GATEKEEPER.** Once it owns a channel on
an actor, nothing else reaches that facet except through APMF. Each channel's job
is to BLOCK the foreign input at its source so nothing competes — a re-assert loop
is a FAILED block, not an acceptable pattern. The arbiter/registry is centered on
that: being the gate.

**First real modular APMF is built and on `main`.** It replaces the
prototype/probe monolith with the extensible two-layer architecture:
- **Core spine** (`native/core/`): the central `Actor::Update(0xAD)` arbiter hook,
  a self-registering channel `Registry`, the `Arbiter` (single gated target,
  per-tick channel drive, PACKAGE STABLE observability), the input test surface,
  and the stubbed Layer-2 client-API seam.
- **Channels** (`native/channels/`): one small self-registering module per facet.
  Add a facet = drop one file (see MAP.md recipe).

Full nav: `MAP.md`. Design: `design.md` + `Docs/ARCHITECTURE.md`. Rules:
`Docs/INVARIANTS.md`. Per-channel catalog: `Docs/CHANNEL-MAP.md`.

## Built — the READY clean-gate channels (test-surface hotkeys)

Aim the crosshair at a follower/NPC, then press the key. Logs to
`Data/SKSE/Plugins/APMF.log` (tagged `[ch.N]` / `[obs]` / `[target]`).

| Key | Ch | Facet | Kind | In-game test |
|-----|----|-------|------|-------------|
| Numpad1 | 1 | movement DENY | source-block (`SetDontMove`) | actor stops walking its package route; `[obs]` still PACKAGE STABLE |
| Numpad2 | 11 | aggression+confidence | **true source-block** | actor turns aggressive/foolhardy; holds even on package-locked followers |
| Numpad3 | 5 | headtrack look-up | **known-incomplete block** | head cranes up; re-assert stopgap; on a package-locked follower may hold only the eyes (documented) |
| Numpad4 | 8 | cast selection (Firebolt) | **true source-block** | right-hand selection := Firebolt; AI keeps it (casts it when it triggers in combat) |
| Numpad5 | 4 | weapon draw | one-shot | weapon drawn; press again to sheathe |
| Numpad6 | 10 | dialogue | one-shot | current dialogue pauses |
| Numpad7 | 1a | half speed | source-gate (`kSpeedMult`) | actor moves at half pace |
| Numpad8 | 16 | silent movement | source-gate (`kMovementNoiseMult`) | actor's movement noise -> 0 |

Every channel keeps the package coherent (no substitution) and restores the state
it changed on release / disengage / pre-load-game. Only headtrack re-asserts.

## Deck field findings folded in (gap-probe, 2026-09-02)

- **True source-blocks are robust even on package-locked followers** (Cicero, pkg
  0x0009BE51): setting AI-attribute AVs and owning `selectedSpells` set the input
  the AI itself reads, so an aggressive package cannot out-fight them. Implemented
  as true source-blocks (ch.11, ch.8, ch.1a, ch.16, ch.1).
- **Headtrack is a KNOWN-INCOMPLETE block, not a clean gate.** The re-assert war is
  the symptom of an un-blocked AI write, not an inherent limit. The AI writes
  multiple headtrack types; a package-locked follower reclaims the head via a
  higher-priority type. The real fix is to BLOCK the AI's headtrack write for the
  owned channel at the 0xAD hook (skip/neutralize it), not re-assert after — left
  for later. Flagged in the module + INVARIANTS #2.
- **Casting selection confirmed KEPT** every tick by the AI (never overwritten);
  it only *shows* as a cast when the actor is in combat / has a trigger (expected).

## Probe-gated — do NOT build without a live probe

Marked GAP in `Docs/CHANNEL-MAP.md`:
- Combat-target PIN (ch.6), combat ACTIONS behavior tree (ch.7), casting TRIGGER
  suppression (ch.8), sustained package procedures (ch.9), facial-expression
  setter (ch.13).
- Headtrack's real block (block the AI write at the hook) — see above.

**Movement PROMOTE is NOT treated as a gap/blocker.** Once the movement source is
BLOCKED, driving the walk is uncontested (MFO walks followers fine when it is
actually in control; the probe's Move-inject "did nothing" only because the package
still owned the body). The question is not "how do we drive" but "are we truly in
control (blocking)." The `IMovementDirectControl` unnamed-feed detail is a driver
choice to settle when the promote feed is wired, not a mystery.

## Client API (Layer 2) — stubbed seam only

`native/core/ClientAPI.{h,cpp}`: `Request(actor, intent, basis)` / `Complete(handle)`
declared; bodies log "not implemented". Intentionally unbuilt — design.md §7 open
questions (raw package vs high-level intent; numeric basis vs policy callback; how
a client ranks against an unaware intercepted source; yield cadence) must be
resolved first. When built, route through the SAME arbiter + channel engage the
hotkeys use (one control path).

## Build / CI

- Compile is **CI-only** (GitHub Actions, `native.yml`, on `native/**`):
  `gh run list -R marthofdoom/APMF`. Windows + vcpkg + colorglass CommonLibSSE-NG,
  same toolchain/baseline as MFO.
- CMake **GLOBs** `native/**/*.cpp` so a new channel needs no build-file edit.
- Pinned CommonLib API-surface gotchas are in INVARIANTS #8 (verified via CI, not
  memory).

## Next

1. Field-test the eight channels on the deck (marth); confirm PACKAGE STABLE and
   per-channel behavior; confirm the AV/casting gates hold on package-locked
   followers and note where headtrack loses.
2. Decide the client-API shape (design.md §7) against what these channels prove
   the arbiter can express, then build Layer 2 through the same path.
3. Probe the gap channels (movement PROMOTE first) on a live runtime.
4. Multi-target arbitration (today the test surface holds one gated target).
