# APMF Roadmap — from prototype to proof of concept

The prototypes proved the model (central 0xAD hook reaches every NPC; package stays coherent; true
source-blocks — AV, selectedSpells, SetDontMove — hold even on package-locked followers; a re-assert
loop = a failed block). v0.1.0 landed the modular architecture. This is the plan to make it real.

**Vision (marth 2026-09-02):** APMF is the gatekeeper for AI control. It must scale to potentially
HUNDREDS of NPCs at once — MFO uses it for a handful of followers, but others will want it for a whole
village. MFO is the first client and the full proof of concept: all of MFO's gambits plus the looting
improvements APMF unlocks, driven through APMF.

## Phase 1 — MULTI-NPC ARBITER + CLIENT API (BUILT, on `main`)
Replaced v0.1.0's single crosshair-captured target with a scalable per-NPC control system. DONE:
- A control MAP (`core/ControlMap`) keyed by NPC FormID, each entry holding that NPC's engaged channels
  + per-channel client claims + captured package. Scales to hundreds: an uncontrolled NPC pays an
  `empty()` check + ONE hash lookup that misses (INVARIANTS #13). Re-targetable + multi-target.
- Thread-safe SINGLE-WRITER model (INVARIANTS #12): the hook runs on the game thread; API calls (from a
  client's BSJobs worker) only ENQUEUE a POD op under a brief lock; the map is mutated ONLY on the game
  thread (`Drain()` once/frame from the PlayerCharacter `0xAD` seat + `ReleaseAll()`), read lock-free.
- **Client API (Layer 2) made real as an INTER-PLUGIN C-ABI** (`APMF_API.h` + `core/ClientAPI.cpp`): a
  separate client DLL (MFO — becoming a mandatory prerequisite) obtains a POD struct of function pointers
  via exported `APMF_GetInterface`, then calls `Request(actorFormID, intent, basis) -> handle` /
  `Release(handle)`. No C++/STL/vtable crosses the boundary. `basis` arbitrates same-channel same-NPC
  (higher wins, tie → earliest); refcounted to the last claim. APPEND-ONLY forever (INVARIANTS #14).
  APMF holds ZERO client-specific code — the header + query fn are the ONLY seam.
- Reference channel done right: the FULL movement block — `KeepOffsetFromActor(self)` nulls the move
  INTENT at the source + `SetDontMove` locks translation → a clean stand-still (no run-in-place, no snap).
- **First release ships the FULL documented channel catalog** (13 channels — every DOCUMENTED facet in
  `Docs/CHANNEL-MAP.md`) as a baseline benchmark; each a small self-registering module through the intent
  enum. The GAP channels (combat PIN, combat-actions, casting-trigger, headtrack all-types, package
  procedures, facial-expression setter) are the post-first-release probe work (see STATUS).

## Phase 2 — PASSIVE LOGGER (built into ALL debug builds)
A standing passive observer in every debug build that watches AI inputs/outputs and HEURISTICALLY LEARNS
NEW SIGNALS — surfaces channels/behaviors we have not mapped yet (event/delta logged, tagged, correlated).
Self-discovering instrumentation so the channel map keeps growing from real play.

## Phase 3 — MFO INTEGRATION (first client)
MFO calls APMF via the client API. Wire the ready channels first (movement block, casting-selection,
combat-target, disposition AVs, headtrack once fully blocked) plus the looting improvements APMF unlocks
(package-locked followers can finally be driven — the Cicero lockpick class).

## Phase 4 — FULL PROOF OF CONCEPT
All of MFO's available gambits + the looting improvements, performed through APMF on followers. This is
the POC that validates the whole framework.

## Ongoing — DEEPER DIVE INTO THE GAPS
Turn each remaining GAP (Docs/CHANNEL-MAP.md) into a true source-block: full movement block (Phase 1),
headtrack all-types block, combat-target PIN (block the threat re-selector), casting-trigger suppression,
combat-actions behavior-tree lever. Probe/RE as needed; the Phase-2 passive logger feeds this.
