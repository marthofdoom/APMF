# APMF Roadmap — from prototype to proof of concept

The prototypes proved the model (central 0xAD hook reaches every NPC; package stays coherent; true
source-blocks — AV, selectedSpells, SetDontMove — hold even on package-locked followers; a re-assert
loop = a failed block). v0.1.0 landed the modular architecture. This is the plan to make it real.

**Vision (marth 2026-09-02):** APMF is the gatekeeper for AI control. It must scale to potentially
HUNDREDS of NPCs at once — MFO uses it for a handful of followers, but others will want it for a whole
village. MFO is the first client and the full proof of concept: all of MFO's gambits plus the looting
improvements APMF unlocks, driven through APMF.

## Phase 1 — MULTI-NPC ARBITER + CLIENT API (NEXT)
The v0.1.0 arbiter holds a single crosshair-captured target. Replace it with a scalable per-NPC
control system:
- A control MAP keyed by NPC (formID/handle), each entry holding that NPC's engaged channels + params.
- Must scale to HUNDREDS of controlled NPCs. The 0xAD hook fires per-NPC per-frame for ALL NPCs, so an
  uncontrolled NPC must pay near-zero cost (one hash lookup); only controlled NPCs do channel work.
- Re-targetable and multi-target (any number of NPCs controlled simultaneously, each independent).
- Thread-safe: the hook runs on the game thread; API calls may come from a client's worker thread
  (MFO's BSJobs worker) — guard the control map (lock or marshal).
- **Client API (Layer 2, `core/ClientAPI`) made real, coupled to the map:** `Request(actor, intent,
  basis) -> handle` registers an NPC+channel into the control map; `Complete(handle)` releases. `basis`
  arbitrates when two clients want the same channel on the same NPC. This is how MFO (and others) drive.
- Reference channel done right: the FULL movement block (block the move INTENT, not just translation —
  no run-in-place, no teleport-snap; SetDontMove alone is one layer too shallow).

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
