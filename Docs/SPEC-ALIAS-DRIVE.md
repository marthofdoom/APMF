# SPEC — Alias-Drive (ch.9's 16-slot claim pool)

RELEASE-BLOCKER (marth, 2026-09-04). Field-proven gap: APMF's ch.9 package-offer
channel (`core/PackageGate.cpp`'s 0x49 hook, `channels/OfferPackage.cpp`) only
ever drives a follower whose ACTIVE package is already alias-sourced — a
framework follower like Cicero. A vanilla follower on a bare
`PlayerFollowerPackage` (Jesper) has ZERO alias membership, and the engine
never asks `CheckForCurrentAliasPackage` in a way that lets our hook's answer
matter for him — the offered package is claimed in `ControlMap` but his own
package keeps winning. Field-proven on deck 2026-09-04. This spec closes that
gap: **every NPC must be drivable by APMF**, not only ones another framework
already put on the engine's alias ladder.

This document is the authoritative spec for the mechanism; `MAP.md`'s
`core/AliasPool.{h,cpp}` entry and `channels/OfferPackage.cpp`'s header comment
point back here. It is a direct port of MFO's field-proven loot-travel alias
mechanism (`marth-follower-overhaul` repo, `native/Packages.cpp` +
`MFO_GenerateESP.py`).

## 1. The mechanism (implemented exactly as designed — not redesigned)

1. APMF ships its own tiny ESL, `APMF.esl` (`APMF_GenerateESP.py`): one claim
   quest, `APMF_ClaimQuest`, carrying a 16-slot ACTOR-ALIAS POOL at a high
   static priority.
2. On a ch.9 `OfferPackage` **Engage** (`channels/OfferPackage.cpp`),
   `core/AliasPool.cpp`'s `ClaimSlot` `ForceRefTo`-fills the claimed actor into
   a free pool slot. This is ONLY to put the actor onto the engine's alias
   ladder (give him SOME `ExtraAliasInstanceArray` membership) — it does not
   choose or influence which package he ends up running.
3. The engine, selecting a package for the now-aliased actor, calls
   `Actor::CheckForCurrentAliasPackage` (vfunc **0x49**) — `core/PackageGate.cpp`'s
   EXISTING hook (unmodified by this change) already returns the client's
   offered package from the `ControlMap` claim, for ANY actor that vfunc is
   asked about. The alias-fill's only job is to get the actor asked about at
   all; the existing hook supplies the package exactly as before.
4. On **Release**/`OnOwnerChanged`, `core/AliasPool.cpp`'s `ReleaseActor` evicts
   the actor from his pool slot: force-fills the session's eviction XMarker
   into that slot (a null clear is a no-op — never used). `OnOwnerChanged`
   itself does NOT touch the alias pool: the actor doesn't change on a
   winner-change, only which claim's `param.form` the 0x49 hook returns —
   Engage/Release are the only 0→1 / 1→0 transitions that need an alias-ladder
   change (`Channel.h`'s per-NPC lifecycle contract).

`core/PackageGate.cpp`'s redirect logic is **unchanged** by this work — the
hard constraint from the brief. The alias-fill is purely upstream plumbing
that makes the hook get asked in the first place.

## 2. FormID band + priority (`APMF_GenerateESP.py`)

One master (Skyrim.esm) → own-file prefix `0x01000000`, mirroring MFO's own
`DESIGN.md 8.2` banding doctrine.

| FormID (local) | Record | Name |
|---|---|---|
| `0x800` | QUST | `APMF_ClaimQuest` — the 16-slot alias pool |
| `0x801` | PACK | `APMF_PlaceholderPackage` — shared by all 16 slots |

`0x802`–`0xFFF` reserved for future APMF forms — ESL-legal range is
`0x800`–`0xFFF`; only the first two are used today.

**Priority: `CLAIM_PRIORITY = 90`** (env-overridable via `APMF_CLAIM_PRIORITY`
for a field test — `# FIELD-TUNABLE` in the generator). Skyrim's own
scene-quest band runs 80–96; 90 sits high in that precedent range without
claiming the very top, and clears every vanilla follower quest (~30) and
custom-follower-framework quest observed in the field (Tuxborn audit,
INVARIANTS #3a, all well under 90). Chosen ONCE, authored into the record
(QUST DNAM byte 2) — MFO's own field evidence (`ENGINE_NOTES` 0.35b/0.36,
ported into `Docs/INVARIANTS.md` reasoning here) proved a **runtime** priority
change does NOT re-arbitrate an actor already claimed by another quest: the
engine locks in the owning quest at ALIAS-FILL time. So the claim model is
entirely alias OCCUPANCY — fill = claim, evict = release — never a runtime
priority flip.

## 3. The placeholder package — why every slot needs one

MFO's own `ENGINE_NOTES.md` #0.24/#0.25 (mirrored into `Docs/INVARIANTS.md`
`#69`/`#70` in that repo) measured: **an alias-claimed actor with no VALID
package STANDS STILL** — the engine picks the highest-priority quest whose
alias claims the actor, asks it for a package, and does not skip a claiming
quest that has nothing valid. An EMPTY alias is exactly as bad as a
conditioned-false one: "claimed with nothing" roots the actor.

So every one of `APMF_ClaimQuest`'s 16 aliases carries the SAME
`APMF_PlaceholderPackage` (a vanilla Travel package, runtime-handle Location
— PLDT type 0 "Near Reference" toward the player, radius 200, **UNGATED**, not
`kIgnoreCombat`). It is close to a dead letter in normal operation: the 0x49
hook overrides whatever it would have offered the instant a live `ControlMap`
claim exists, which is true for essentially the whole time an actor occupies
a slot. It only matters in two narrow windows: between the alias fill landing
and the claim being read by the next 0x49 poll, and if a claim ever lapses
while the actor is still slotted (e.g. the owning client crashes without
releasing). In both cases the actor calmly heads toward the player instead of
rooting or running a random gated package — never `kIgnoreCombat`, so ordinary
combat still preempts it exactly like it preempts any other follower package.

## 4. Eviction discipline (INVARIANTS #3-equivalent — get this right or saves corrupt)

The alias fill is **engine-serialized** — it persists natively in the `.ess`,
exactly like MFO's own loot/retreat/command quest aliases. A save written
mid-claim loads with the actor still filled into his pool slot, claimed at
static priority 90. Two sweeps make this self-healing:

- **`kPreLoadGame`** (`apmf::aliaspool::ReleaseAll("kPreLoadGame")`, alongside
  `Arbiter::Get().ReleaseAll`, which already evicts THIS session's own live
  claims through ch.9's generic `Release` path). Belt-and-braces: a cheap
  O(16) defensive backstop, not the primary release for this session's own
  claims.
- **`kPostLoadGame` / `kNewGame`** (`EnsureEvictMarker()` THEN
  `ReleaseAll("post-load reconcile")`) — the load-bearing one. The in-process
  occupancy table is EMPTY on a fresh load (nothing survives a DLL reload), so
  this sweep walks the **alias occupancy itself**
  (`TESQuest::CreateRefHandleByAliasID` on all 16 slots — an engine read, not
  our bookkeeping) and evicts any ACTOR occupant found there, exactly mirroring
  MFO's own `Packages.cpp::ReleaseAll` per-slot loot/retreat sweep. This is
  what makes a save from a crashed or older session self-heal: the sweep
  doesn't need to know WHO was claimed, only that a slot currently holds an
  actor.

**Eviction marker.** Never the player (forcing the player into a
package-carrying alias is the furniture-ejection bug class — MFO's own
`#48`/`INVARIANTS #3`). `core/AliasPool.cpp::EnsureEvictMarker()` is a
verbatim port of MFO's `Packages::EnsureEvictMarker`: a vanilla XMarker
(base `0x3B`), minted once per session via `PlayerCharacter::PlaceObjectAtMe`
(force-persisted so its handle survives cell unloads), on the main thread,
called from `kPostLoadGame`/`kNewGame` BEFORE the reconcile sweep so evictions
displace with the marker rather than falling back to the player. A "clear" is
never a null-fill — always a real `ForceRefTo` (or its VM fallback) onto the
marker; a null clear on this codepath is a no-op (MFO's measured lesson).

**Co-save (`'ADRV'` v1).** Wired through `SKSE::SerializationInterface`
exactly like `core/AvLedger`'s existing `OnSave`/`OnLoad`/`OnRevert`
registration in `plugin.cpp`. Record layout: `count` (uint32), then per live
slot `{slot (uint32), actor FormID (uint32)}` — 8 bytes/entry. **This record
is a diagnostic mirror, not the reset mechanism** — restated because it is the
one place a reviewer might expect load-bearing restore logic and not find it:
the alias fills persist NATIVELY in the `.ess` (unlike `AvLedger`'s AV
overrides, which do NOT persist on their own and genuinely need restoring),
so the co-save's `Load`/`ApplyPending` only LOG which slots were live at save
time — the unconditional alias-occupancy sweep above is what actually resets
state, independent of what this record says. `OnRevert` clears the in-process
table (no restore — actors are being replaced, same semantics as
`ControlMap::Clear`/`av::Revert`).

A reader for every shipped version is kept forever (mirrors `AvLedger`'s
`INVARIANTS #15` discipline) — bump `kRecordVersion` and add a branch on any
future layout change, never redefine v1's byte shape.

## 5. VR

The whole alias path is VR-refused. `ForceRefTo`'s reloc id (25052, ported
from MFO's own SE/AE cross-check against SKSE64 2.2.6's source for
`CURRENT_RELEASE_RUNTIME` 1.6.1170) and `CheckForCurrentAliasPackage`'s vtable
index are SE/AE only. `ClaimSlot` returns `false` immediately on
`REL::Module::IsVR()`; `ResolveForms()` still resolves + logs the ESL's forms
on VR (harmless, useful diagnostic) but warns that the pool will never claim
a slot. `core/PackageGate.cpp`'s own VR-refusal (unchanged) already means ch.9
does nothing at all on VR, so this is consistent with the channel it feeds.

## 6. Threading

Every entry point in `core/AliasPool.cpp` runs on APMF's single MAIN/writer
thread (INVARIANTS #12): `ClaimSlot`/`ReleaseActor` are called only from
`channels/OfferPackage.cpp`'s `Engage`/`Release`, which `Channel.h`'s contract
confines to the game thread (the `ControlMap` Drain/writer seat);
`ReleaseAll`/`EnsureEvictMarker`/the co-save callbacks are called only from
`plugin.cpp`'s SKSE messaging callbacks — all confirmed the same MAIN thread
as the writer seat. So the slot-occupancy table (`RE::FormID
g_slotActor[16]`) needs no lock or atomics, mirroring `core/AvLedger`'s own
unlocked, main-thread-only ledger.

## 7. Deviations from the brief (and why)

- **Quest flags: start-game-enabled, NOT run-once** (`0x0011`), not the
  brief's literal "start-game-enabled, run-once" wording. Every one of MFO's
  own alias-carrier quests with this exact bare-pool shape
  (`MFO_CommandQuest`/`MFO_LootQuest`/`MFO_RetreatQuest`) ships NOT run-once —
  `MFO_CommandQuest`'s own generator comment is explicit: run-once is for
  one-shot startup logic, not a quest whose aliases must stay claimable for
  the whole session. Ported the field-proven shape rather than the literal
  wording. **Consequence:** being start-game-enabled and not run-once, the
  claim quest MUST be listed in `Data/SEQ/APMF.seq` or it never starts on an
  EXISTING save — `APMF_GenerateESP.py`'s `main()` writes that file
  automatically alongside `APMF.esl`.
- **Unconditional fill on every Engage**, even for an actor ALREADY
  alias-sourced by some other framework (Cicero). The brief describes an
  unconditional fill and explicitly says not to redesign the mechanism; a
  "skip if already aliased" special case was considered and rejected — it
  would need a new `ExtraAliasInstanceArray` read with its own edge cases, for
  no behavioral gain: an already-aliased actor filled into our pool too just
  means he temporarily occupies one of 16 slots redundantly (cosmetic
  capacity cost only, not a correctness issue — an actor can sit in multiple
  quests' aliases simultaneously, and the 0x49 hook's answer is unaffected
  either way), and Release cleanly hands him back to whatever the engine's
  next-highest claiming quest is (which still includes his own framework's
  alias, since filling ours never removed him from his own).

## 8. Open / unverified

- **Field-unverified:** whether a pool slot genuinely needs a real,
  resolvable placeholder package for `CheckForCurrentAliasPackage` to even be
  asked about a freshly-aliased actor, versus merely needing SOME
  `ExtraAliasInstanceArray` membership regardless of package validity. This
  spec follows MFO's own measured lesson (§3 above) conservatively — ship a
  real ungated package rather than gamble on an empty alias being sufficient
  now that the actor is aliased.
- **Field-unverified:** the exact set of actors that need only 1 of 16 slots
  at once in practice vs. any modlist that could genuinely want more than 16
  concurrent alias-drive claims. 16 is generous headroom (matches MFO's own
  4-slot loot pool at 4x scale for a wider genuinely-needed action set); a
  16th-slot exhaustion degrades gracefully (`ClaimSlot` returns `false`, no
  crash, ch.9 behaves exactly as it does today for that one actor).
- **SE VM-dispatch fallback** (`DispatchAlias`) is ported verbatim from MFO's
  command-quest fill/release path but has NOT been field-exercised for
  alias-drive specifically (MFO's own loot-quest analog is AE-only, no SE
  fallback at all) — carried over per the brief's explicit instruction to
  include it, but flagged here as the one code path with no direct MFO
  field-proof for THIS specific quest shape.
