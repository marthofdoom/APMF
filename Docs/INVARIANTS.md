# APMF Invariants

Numbered rules that keep APMF correct, version-robust, and crash-free. MAP.md and
the code cite these as `#N`. Break one and you get a data race, a CTD on a game
update, a mislabeled gate, or an actor left in a mutated state. Read the relevant
ones before touching a subsystem.

## Design principles

**#1 — APMF is the gatekeeper: BLOCK the foreign input, do not force the output.**
Once APMF owns a channel on an actor, nothing else reaches that facet except through
APMF. A channel's job is to block the competing input at its source — deny the
losing source, or set the input the AI itself reads — so nothing competes. It does
NOT let the AI produce a write and then override it every frame. A per-tick
re-assert loop is a FAILED block, a symptom of not-blocking, NOT an acceptable
pattern (design.md §1a rule 3; marth 2026-09-02). `Channel::Tick` is empty by
default for exactly this reason — a real block does no per-tick work.

**#2 — A channel that still needs re-assert is a KNOWN-INCOMPLETE block; label it
so.** Where we have not yet blocked the AI's own write to a facet, a re-assert
stopgap is permitted BUT the channel must (a) override `Tick`, (b) say
"known-incomplete block, not a clean gate" in its module header, and (c) accept
that it can LOSE to an aggressive or package-locked source. The FIX is to block the
AI's write (gate the relevant part of the AI decision layer at the 0xAD hook for
the owned channel — skip/neutralize the write), NOT to keep overriding after the
fact. Deck-tested: true source-blocks (AV, casting selection) hold even on a
package-locked Cicero; the un-blocked channels (headtrack, crouch) get out-fought
by the package — because they are not blocking yet. Today only `Headtrack` (ch.5)
is known-incomplete. Never mislabel a re-assert as a clean gate.

**#3 — Never substitute the package.** APMF commandeers a facet while the actor's
current package stays current and keeps evaluating (design.md §5). Substituting the
package fires `OnPackageEnd`/`OnPackageChange` and tears the preempted source down.
Every channel must keep the package coherent; the arbiter logs PACKAGE STABLE ~1/s
so a regression is visible. Only one source holds the coherent package slot at a
time.

## Threading

**#4 — The control map is game-thread-only state.** `ControlMap`'s `m_map` and
`m_index`, and every channel's engine mutation + per-NPC state map, are mutated
ONLY on the game/main thread — in `Drain()` (applying enqueued ops), `ReleaseAll()`,
and the per-NPC `Tick`; the input sink (`BSInputDeviceManager`) and both `0xAD`
seats all dispatch there and are serial. Do NOT mutate the map from any other
thread. The ONLY shared-with-workers state is the request `m_queue` (guarded by
`m_qmx`) and the atomic handle counter. The per-NPC hot path (`OnActorUpdate`) READS
the map with no lock — see #12/#13.

**#5 — Release restores what engage changed.** Any channel that writes engine state
(AVs, the spell slot, weapon state, AI-driven flag) must capture the prior value at
engage and restore it in `Release`. `Arbiter::ReleaseAll` runs on disengage,
target-unload, and `kPreLoadGame` — never skip or reorder it, or the actor keeps the
mutated state across a save load.

## Version robustness

**#6 — Version-robust hooks only; VR-refused.** Hook VIRTUAL vtable indices
(`Actor::Update` `0xAD`), never non-virtual call-site offsets (that is the whole
reason `0xAD` is safe — design.md §3, §8). Install once, idempotent. VR is refused
at install: the `0xAD` index is unverified for VR (`REL::Module::IsVR()` guard),
and Address-Library IDs like `StartCombat` have no sourced VR id.

**#7 — Guard + log every struct-member write.** Struct offsets
(`AIProcess.currentPackage`, `selectedSpells[]`, `movementController`,
`combatController`, `caster->currentSpell`) are more version-sensitive than vtable
indices. Reach them through CommonLib accessors and null-check the accessor before
writing. Rely on CommonLib's build-time `static_assert`s; never hand-write an
offset.

**#8 — Pinned CommonLib API surface (colorglass rev).** The probe confirmed this
rev does NOT bind some functions the design references:
- `Actor::StartCombat` — not bound; use `REL::RelocationID(37608, 38561)` (po3's
  published ID) if needed. Combat-target is probe-gated anyway.
- `Actor::SetCurrentSpell` — not bound (only a no-op `SetCurrentSpellImpl`); the
  casting-selection channel writes `selectedSpells[slot]` and `caster->currentSpell`
  directly, guarded. Deck-confirmed the AI KEEPS that selection (clean gate).
- Use `actor->AsActorState()` for attack/weapon/block state, not raw members.
- `MovementControllerNPC` exposes NO named AI-driven setter in this rev — only
  unnamed `Unk_0C/0D` void(void) vfuncs (calling them blind is the documented
  CTD roulette). Movement FULL block (ch.1) therefore uses the Address-Library-bound
  `SetDontMove` (`RELOCATION_ID(36490, 37489)`) to lock translation PLUS
  `KeepOffsetFromActor` (`RELOCATION_ID(36870, 37894)`) / `ClearKeepOffsetFromActor`
  (`RELOCATION_ID(36871, 37895)`) to null the move INTENT at the source — none is
  bound in CommonLib (verified against the fork's `Actor.h`); the IDs are
  calibration-checked against the known `SetDontMove` pair.
- `Actor::StartCombat` — not bound; combat-target STEER (ch.6) uses
  `RELOCATION_ID(37608, 38561)`, signature `void(Actor*, Actor* target)`. VR-refused
  (SE/AE IDs). `StopCombat()` IS a bound vfunc (0xE5).
- Bound and callable directly (verified against the fork): `ActorEquipManager::
  GetSingleton/EquipObject/UnequipObject/EquipShout`, `Actor::GetEquippedObject(bool)`,
  `DrawWeaponMagicHands(bool)`, `PauseCurrentDialogue()`, `IsSneaking()`,
  `NotifyAnimationGraph(const BSFixedString&)`, `AIProcess::PlayIdle` /
  `SetHeadtrackTarget(Actor*, NiPoint3&)`.
- `Actor::StopCurrentDialogue` does not exist; the real vfunc is
  `PauseCurrentDialogue()` (0x4F). `SetDialogueWithPlayer` is
  `(bool, bool, TESTopicInfo*)`.
- `movementController` is a `BSTSmartPointer` — reach the raw pointer with
  `.get()`; `combatController` is a raw pointer.
Match this surface; do not assume an unbound function exists. Every name here was
verified against the pinned rev's headers via CI (never from memory).

## Build / registration

**#9 — Channels self-register; keep every source in the DLL target directly.**
`APMF_REGISTER_CHANNEL` relies on a file-scope static initializer running at load.
That happens ONLY because CMake GLOBs every `.cpp` directly into the plugin DLL
target — NOT via an intermediate static archive, which would strip unreferenced
initializers and make channels silently vanish. Never wrap `channels/` in a static
library. Registration order is load-order-undefined; never assume a channel index or
ordering.

**#10 — Adding a channel touches one file (+ maybe one appended enum value).** One
new `channels/*.cpp` that subclasses `Channel`, declares its `ServesIntent()`, and
ends with `APMF_REGISTER_CHANNEL`. No edit to CMake, the registry, the input layer,
or the arbiter. If the facet needs a NEW client intent, APPEND one value to the
`Intent` enum in `APMF_API.h` (that is the ONLY permitted edit outside the channel
file, and it is append-only — see #14). If a change would require editing the core
to add a facet, the abstraction is wrong — fix the abstraction, not the core.

## Coherence / eviction

**#11 — Momentary channels hold no state; `Release` is a no-op.** A one-shot with no
lasting authority (`Dialogue` pauses once; `Idle` fires one animation) does its work
in `Engage` and leaves `Release` empty — there is nothing to restore. It still holds
a claim in the control map until released (the tester's second press, release-all,
or the client's `Release`); that claim is inert. Do not make a momentary channel
override `Tick` or capture per-NPC state.

## Multi-NPC control map (Phase 1)

**#12 — SINGLE-WRITER threading; the map is mutated only on the game thread.** The
`0xAD` hook runs on the game thread; client API calls (`Request`/`Release`) may
arrive from a client's worker thread (MFO's BSJobs worker). So API calls only
ENQUEUE a small POD op under a brief lock on `ControlMap::m_queue`; they NEVER touch
the map. The map (`m_map`) and handle index (`m_index`) are mutated ONLY on the game
thread — `Drain()` (once per frame, from the PlayerCharacter `0xAD` seat) applies
the queued ops, and `ReleaseAll()` clears everything. Because every writer is the
one serial game thread, the per-NPC per-frame hot path (`OnActorUpdate`) reads the
map with NO lock. Handles are allocated with an atomic counter so `Request()`
returns synchronously before its op is drained; ops are FIFO so a `Release` enqueued
right after its `Request` is applied after it. A channel's OWN per-NPC state map is
likewise game-thread-only. Break this and you get a data race across hundreds of
NPCs.

**#13 — An uncontrolled NPC pays near-zero.** `OnActorUpdate` runs for EVERY NPC
every frame. It must do at most: an `m_map.empty()` check, then ONE hash lookup that
misses — no allocation, no iteration over all NPCs, nothing else. Only a CONTROLLED
NPC runs its channels' `Tick` (and most channels no-op there — a clean block does no
per-tick work, #1). Never scan all NPCs, never allocate on the hot path. The
liveness sweep and queue drain run once per frame over the SMALL control map, never
over all actors.

## API contract

**#14 — `APMF_API.h` is a frozen, APPEND-ONLY C-ABI contract.** `APMF_API.h` is the
ONLY file a client shares with APMF; a client (MFO) and APMF compile as SEPARATELY
built DLLs and interact ONLY through it at runtime. So the surface is C-ABI: a POD
struct of function pointers (`APMF_API_v1`) with POD args only (`RE::FormID`, the
plain `Intent` enum, floats) — NO C++ class, NO STL, NO vtable crosses the boundary.
Once shipped, NEVER change or reorder an existing field, `Intent` value, or
function-pointer slot; only APPEND (new `Intent` values at the end, new fields at the
END of the struct, bump `kABIVersion`). A client built against v1 must keep working
against every later APMF (same discipline MFO applies to `MEO_API.h`). The interface
is handed over by the exported query function `APMF_GetInterface` (chosen over the
SKSE-messaging handshake: synchronous, no message-ordering/routing subtlety). APMF
holds ZERO client-specific code — delete every client and APMF loses zero lines; the
header + the query function are the ONLY seam.
