# APMF Invariants

Numbered rules that keep APMF correct, version-robust, and crash-free. MAP.md and
the code cite these as `#N`. Break one and you get a data race, a CTD on a game
update, a mislabeled gate, or an actor left in a mutated state. Read the relevant
ones before touching a subsystem.

## Design principles

**#0 — APMF MODERATES; it MUST NOT MANUFACTURE OR SUSTAIN AN AI DECISION (the
hardest rule, the one a CTD taught us).** An APMF channel may do exactly three
things: (a) ARBITRATE — record which client owns a facet; (b) DENY — suppress the
losing source at its source (AV writes, movement full-block, detection AVs, package
yield, pausing an actor's own in-progress dialogue); and (c) PROMOTE A BOUNDED,
ONE-SHOT, CLIENT-REQUESTED ACTION for a facet that has no meaningful "deny" form and
involves no AI decision to arbitrate around — a single anim-graph event, a sticky
draw/sheathe, a stance toggle — fired ONCE at Engage/Release, on the game thread,
with no per-tick `Tick` work and no re-assert loop. A channel MUST NEVER call a
function that SELECTS WHAT an AI will decide to do when the client already has its
own proven mechanism for making that selection — NOT `Actor::StartCombat`, NOT
`MagicCaster::CastSpellImmediate`, NOT a movement drive-feed, NOT a direct
`selectedSpells`/`EquipShout`-style write that picks a target/spell/power for the AI
to later act on. That class of DECISION-SELECTION is BEHAVIOR, and behavior belongs
to the CLIENT (it executes with its own proven mechanisms; APMF only denies
competitors, or arbitrates the claim, so the client's behavior reaches the actor —
design.md §1a). **The bright line:** a lawful promote is a single deterministic call
whose outcome does not stand in for an AI decision (idle-play, weapon-draw,
stance-toggle); a forbidden generate calls a function that picks WHAT the AI decides
(a target, a spell, a shout) or drives it continuously. **Cautionary case:** ch.6
combat-target once called `StartCombat` (to command a target) — wrong LAYER, and
with a bad reloc signature it was a hard AV (EXCEPTION_ACCESS_VIOLATION inside
StartCombat). The deny-only rule makes that whole crash class structurally
impossible: APMF makes no such call at all. ch.6 and ch.8 are now arbitration-only;
the client commands the target / selects the spell. **ch.14 shout-power's direct
`EquipShout` call was the same anti-pattern in miniature** — no crash (the function
is bound and safe), but it duplicated the "APMF selects, not the client" mistake
this rule exists to end; it is now converted to arbitration-only, matching ch.6/ch.8.

**#1 — APMF is the gatekeeper: BLOCK the foreign input, do not force the output.**
Once APMF owns a channel on an actor, nothing else reaches that facet except through
APMF. A channel's job is to block the competing input at its source — deny the
losing source — so nothing competes. It does NOT let a source produce a write and
then override it every frame, and it does NOT itself write the behavior. A per-tick
re-assert loop is a FAILED block, a symptom of not-blocking, NOT an acceptable
pattern (design.md §1a; marth 2026-09-02). `Channel::Tick` is empty by default for
exactly this reason — a real block does no per-tick work.

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

**#3 — Never substitute the package — SCOPED to the movement-hijack channels.** A
channel that commandeers a facet BY DRIVING IT OVER A RUNNING PACKAGE (the movement
hijack: locomotion/facing) must leave the actor's current package current and
evaluating (design.md §5). Making another source the current package fires
`OnPackageEnd`/`OnPackageChange` and tears the preempted source down; truthful state
cannot save it. Those channels keep the package coherent; the arbiter logs PACKAGE
STABLE ~1/s so a regression is visible.

**THE ONE INTENTIONAL EXCEPTION — the 0x49 package-OFFER channel (design.md §5a).**
The `CheckForCurrentAliasPackage` (vfunc 0x49) offer channel deliberately REDIRECTS the
alias-tier package OFFER to a client's own package, so the engine then runs the CLIENT's
REAL package NATIVELY. This is a package-tier PROMOTE, not a movement hijack, and it is
allowed precisely because it costs exactly ONE `OnPackageChange` each way — the SAME
class of interruption as vanilla combat taking an NPC and handing it back, which every
follower already tolerates. It is bounded: engage only in a gambit-valid-and-live window,
relinquish cleanly so the framework package resumes, and touch NO alias/run-once state.
It is STRUCTURALLY BENEATH script-driven (PapyrusUtil) overrides — it cannot reach them
(#3a) — so it can never break a script-driven follower. (Movement-hijack channels still
obey the no-substitute rule above; the offer channel is the one facet where a promote is
the correct, bounded mechanism.)

**#3a — APMF is BENEATH script-driven package overrides; it uses ZERO Papyrus.** APMF
arbitrates the engine's NATIVE / alias tier (packages the engine itself selects, and the
alias-tier package offer at 0x49). A PapyrusUtil `AddPackageOverride` sits ABOVE that
tier: the script layer wins, and APMF neither sees nor touches it — so "never break a
custom follower" is AUTOMATIC for any script-driven follower. APMF calls no Papyrus
itself. **Never-break guardrails (all three hold for every control window):** (1) control
only in BOUNDED, gambit-valid-AND-live windows — never a standing hold; (2) RELINQUISH
cleanly so the framework's package resumes; (3) the offer path touches NO alias / run-once
state. **Supporting evidence (Tuxborn audit, 2026-09-02):** across all 1626 enabled mods,
Simple Follower Framework + every custom follower are alias-tier with ZERO PapyrusUtil
package overrides — so the alias-tier (0x49) mechanism covers every follower in that list,
and the beneath-script layering means even a hypothetical script-driven follower is safe by
construction.

## Threading

**#4 — The control map's WRITE side is main-thread-only state; the READ side is an
RCU snapshot readable from any thread.** `ControlMap`'s working map, `m_index`, and
every channel's engine mutation + per-NPC state map are mutated ONLY on the WRITER
thread — in `Drain()` (applying enqueued ops), `ReleaseAll()`, `Clear()`, and every
channel's `Engage`/`OnOwnerChanged`/`Release`; the input sink
(`BSInputDeviceManager`), the PlayerCharacter `0xAD` seat, and the SKSE revert/
preload callbacks all dispatch there and are serial (confirmed the same MAIN thread
by the `[threadcheck]` evidence — see #12). Do NOT mutate the map from any other
thread. The ONLY shared-with-workers state is the request `m_queue` (guarded by
`m_qmx`) and the atomic handle counter. The per-NPC hot path (`OnActorUpdate`) is
NOT confined to the writer thread (field-proven — #12) and READS a published,
immutable RCU snapshot with no lock, one sanctioned per-entry exception aside — see
#12/#13.

**#5 — Release restores what engage changed.** Any channel that writes engine state
(AVs, the spell slot, weapon state, AI-driven flag) must capture the prior value at
engage and restore it in `Release`. `Arbiter::ReleaseAll` runs on disengage,
target-unload, and `kPreLoadGame` — never skip or reorder it, or the actor keeps the
mutated state across a save load.

**#5a — an ARBITRATION / DENY channel RELINQUISHES on release; it never "undoes" a
live engine decision.** An arbitration-only channel (ch.6 combat-target, ch.8
casting) wrote nothing to the engine (#0), so its `Release` has nothing to restore —
it just drops the claim record. And a DENY channel over a self-correcting engine
decision the AI keeps re-making (a combat target, once a real deny gate exists) must
NOT reverse that decision on release (no `StopCombat`): clients release such a claim
CONSTANTLY (a gambit yields, an expiry sweep, a target switch), so undoing on each
release would yank the actor out of an ongoing fight and flicker on a switch. Let the
engine keep the decision if it is still valid and end it for its own reasons otherwise
("commanding WHICH foe is ours; commanding THAT there is a foe is not"). This is #5's
counterpart: a channel that SET a stored prior value restores it (#5); an arbitration/
deny channel relinquishes.

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
- `Actor::StartCombat` — not bound. **APMF does NOT call it (#0 — that is behavior; it
  is the CLIENT's job).** Recorded as a fact only: its REAL signature is
  `bool(RE::Actor*, RE::Actor*, void*)` — THREE args (a 2-arg wrapper faults inside the
  engine, reading the 3rd param from a garbage register -> a hard AV; that CTD is why
  ch.6 is now arbitration-only). A client that initiates/commands a combat target does
  so itself (e.g. MFO's own `currentCombatTarget` compare-and-write + its StartCombat).
- `Actor::SetCurrentSpell` — not bound (only a no-op `SetCurrentSpellImpl`); a CLIENT
  that owns cast selection writes `selectedSpells[slot]`/`caster->currentSpell` itself.
  APMF's ch.8 does NOT (arbitration-only, #0). Engine fact (deck-confirmed, useful to
  the client): writing `selectedSpells[slot]` + `caster->currentSpell` directly (guarded)
  makes the AI KEEP that selection and cast it as its own decision — the client's path
  to a real animated cast; APMF just arbitrates the facet.
- Use `actor->AsActorState()` for attack/weapon/block state, not raw members.
- `MovementControllerNPC` exposes NO named AI-driven setter in this rev — only
  unnamed `Unk_0C/0D` void(void) vfuncs (calling them blind is the documented
  CTD roulette). Movement FULL block (ch.1) therefore uses the Address-Library-bound
  `SetDontMove` (`RELOCATION_ID(36490, 37489)`) to lock translation PLUS
  `KeepOffsetFromActor` (`RELOCATION_ID(36870, 37894)`) / `ClearKeepOffsetFromActor`
  (`RELOCATION_ID(36871, 37895)`) to null the move INTENT at the source — none is
  bound in CommonLib (CommonLibSSE-NG does not vendor `KeepOffsetFromActor`). The
  KeepOffset IDs are VERIFIED: cross-checked against shipping SKSE source with the
  identical signature that also reproduces the `SetDontMove` anchor verbatim, with
  zero conflicting values found — safe to ship.
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

**#12 — RCU SNAPSHOT; single-writer, multi-reader.** [threadcheck] field evidence
(2026-09-02) proved the `0xAD` hook does NOT run on one serial thread: a `Character`
seat fired on a DIFFERENT worker thread than the `PlayerCharacter`/Drain seat. The
old "single-writer, unlocked-read" model that assumed one serial game thread for
BOTH sides was therefore FALSE and has been retired. What is still true, and is now
the load-bearing assumption instead:

- The WRITER side is single-threaded. `Drain()` (from the `PlayerCharacter` `0xAD`
  seat), `ReleaseAll()` (from `kPreLoadGame`), and `Clear()` (from `OnRevert`) all run
  on the same MAIN thread — this is exactly what `[threadcheck]` verifies: it records
  the `PlayerCharacter`/Drain seat's thread id once and would fire if a `Character`
  seat's thread ever matched it AND something else also mutated the map off that same
  thread; in practice only the per-NPC `Character` seats are parallelized across
  worker threads, never the writer path. A channel's OWN per-NPC state map is
  likewise writer-thread-only (`Engage`/`OnOwnerChanged`/`Release`).
- The READER side (`OnActorUpdate`, i.e. every `Character` `0xAD` seat) is NOT
  single-threaded, and is NOT assumed to be. It must never touch writer-thread state
  directly.
- Client API calls (`Request`/`Release`/`Repoint`) may arrive from a client's worker
  thread (MFO's BSJobs worker) — unchanged: they only ENQUEUE a small POD op under a
  brief lock on `ControlMap::m_queue`; they NEVER touch the map.

The fix is an RCU snapshot, not a hot-path lock: the writer keeps a private working
copy (`m_current`) built from the last published generation; `Drain()`/`ReleaseAll()`/
`Clear()` apply ops/the unload sweep/a wipe against that private copy and, ONLY if
something actually changed, atomically PUBLISH it —
`m_published.store(newSnapshot, memory_order_release)` — as a NEW immutable
`std::shared_ptr<const MapType>` generation (copy-on-CHANGE: a no-op frame allocates
nothing). Readers atomically LOAD a LOCAL `shared_ptr` copy of `m_published`
(`memory_order_acquire`, pairing with the writer's release) and read that frozen
generation lock-free: no torn reads, no UAF even if the writer publishes a newer
generation mid-call — the reader's local `shared_ptr` keeps its own generation alive
via refcount. `m_index` needs no snapshot wrapper (writer-thread-only, no reader ever
consults it) and stays a plain map. Handles are allocated with an atomic counter so
`Request()` returns synchronously before its op is drained; ops are FIFO so a
`Release` enqueued right after its `Request` is applied after it.

Per-entry mutation: every `NpcCtl` field is READ-ONLY to readers once published, with
ONE sanctioned exception — `obsTick` (the per-tick observability counter) is a
`mutable std::atomic<uint64_t>` (relaxed) so `OnActorUpdate` can `fetch_add` it
through the `const NpcCtl&` a snapshot lookup hands back, without breaking the
"snapshot is otherwise immutable" guarantee. No other field may ever be
reader-mutated — a future channel's `Tick()` must not write anything else in
`NpcCtl`/`ChannelCtl`/`Claim`; if it needs live per-claim data, give it the same
atomic treatment, don't reach into the map raw.

Lock-freedom of `std::atomic<std::shared_ptr<const MapType>>` is NOT assumed — it is
disclosed once at hook install (`Hook::Install`, via
`ControlMap::SnapshotIsLockFree()`). Not-lock-free (spinlock-backed, plausible on
MSVC) is ACCEPTABLE: the control map holds only a handful of controlled NPCs, so the
critical section is a pointer copy, not a hot loop — but it must show up in the log,
never be assumed silently.

Break any of this — mutate the published snapshot in place from a reader, mutate
`m_current`/`m_index` off the writer thread, or reader-mutate any `NpcCtl` field
besides `obsTick` — and you get a data race across hundreds of NPCs.

**#13 — An uncontrolled NPC pays near-zero.** `OnActorUpdate` runs for EVERY NPC
every frame, on whatever thread the engine schedules it on (#12). It must do at
most: a relaxed `m_anyControlled` check (no atomic `shared_ptr` traffic at all while
nothing is controlled), then one acquire-load of the published snapshot + ONE hash
lookup that misses — no allocation, no iteration over all NPCs, nothing else. Only a
CONTROLLED NPC runs its channels' `Tick` (and most channels no-op there — a clean
block does no per-tick work, #1). Never scan all NPCs, never allocate on the hot
path. The liveness sweep and queue drain run once per frame over the SMALL control
map (the writer's private working copy), never over all actors, and publish a new
snapshot only when something changed.

## API contract

**#14 — `APMF_API.h` is a frozen, APPEND-ONLY C-ABI contract.** `APMF_API.h` is the
ONLY file a client shares with APMF; a client (MFO) and APMF compile as SEPARATELY
built DLLs and interact ONLY through it at runtime. So the surface is C-ABI: a POD
struct of function pointers (`APMF_API_v1`) with POD args only (`RE::FormID`, the
plain `Intent` enum, floats) — NO C++ class, NO STL, NO vtable crosses the boundary.
Once shipped, NEVER change or reorder an existing field, `Intent` value, or
function-pointer slot; only APPEND (new `Intent` values at the end, new fields at the
END of a struct, bump `kABIVersion`). A client built against v1 must keep working
against every later APMF (same discipline MFO applies to `MEO_API.h`). The interface
is handed over by the exported query function `APMF_GetInterface` (chosen over the
SKSE-messaging handshake: synchronous, no message-ordering/routing subtlety). APMF
holds ZERO client-specific code — delete every client and APMF loses zero lines; the
header + the query function are the ONLY seam. NO EXCEPTION MAY CROSS THE BOUNDARY:
each exported body (`APMF_Request`/`APMF_RequestEx`/`APMF_Release`/`APMF_GetInterface`)
is wrapped in a `try { … } catch (...)` returning `kInvalidHandle`/void/`nullptr` — a
throw (bad_alloc from the queue, an spdlog throw) unwinding across the client's
separately compiled DLL is UB. A swallowed throw degrades to "no control taken",
never a crash.

**#14a — ABI revisions use PREFIX EXTENSION; the param payload is POD, append-only,
and NEVER retained.** A new ABI revision (v2: `RequestEx` + `APMF_Param`; v3:
`Repoint`, which re-points an existing claim's param in place, same handle) adds a
struct `APMF_API_vN` whose LEADING members are byte-identical, in order, to
`APMF_API_v(N-1)`, then appends the new function-pointer slots. The SAME static
object is handed to every client: `APMF_GetInterface` returns the base type
(`APMF_API_v1*`); a newer client checks `p->abiVersion >= N` and reinterpret_casts up
to `APMF_API_vN`. Never edit a shipped `APMF_API_vN` struct — add the next one.
`APMF_Param` is a PLAIN POD struct (`form`/`fval`/`ival`), never a class/STL/pointer-
to-owned-memory; it too is append-only (new fields at the END, so a v1-era caller's
zero-init reads them as 0). An ALL-ZERO param (`{}`), and the `Request`/no-param path,
means "channel default" — cast-select falls back to Firebolt, combat-target to the
player — so v1 behavior is preserved exactly. The `const APMF_Param*` a client passes
to `RequestEx` is READ AND COPIED synchronously inside the call (into the queued POD
op); APMF NEVER retains the client pointer, so a client stack temporary is safe.

## Persistence

**#15 — Persisted AV overrides are CO-SAVED; never stranded.** The AV channels
(Attribute/Speed/Detection) mutate dynamic ActorValues that persist in the `.ess`. A
RAM-only "prior value" would be STRANDED if the player saves while engaged and
reloads (after a load the live control map is empty, so a plain Release restores
nothing — MFO's "fix-forward never cleans old saves" class). So EVERY persisted-AV
write goes through the co-saved ledger `core/AvLedger` (`av::Override`/`av::Restore`),
NOT raw `SetActorValue`. The ledger records `(FormID, AV) -> captured prior value`
and is co-saved via SKSE serialization (unique ID `'APMF'`, record `'AVOV'`): `OnSave`
writes it, `OnLoad` reads it into a pending set (`ResolveFormID` for load-order
remap), `kPostLoadGame` restores each AV regardless of live engaged-state and clears,
`OnRevert` wipes ledger + control map (no restore — actors are being replaced). Any
NEW channel that writes a persisted actor value MUST route it through the ledger.

RECORD VERSIONING — a reader per version, KEPT FOREVER. The `'AVOV'` record carries
`kRecordVersion`; `Load(intf, version)` branches on it. NEVER change a record's byte
layout under an existing version number — bump `kRecordVersion` and add a reader
branch, or an old save's entries misalign (e.g. reading a 12-byte v1 entry as a
16-byte v2 entry consumes the next entry's FormID as `applied` → every override
stranded). Shipped layouts:
- **v1** (12 B/entry): `{FormID, ActorValue, prev}` — restored UNCONDITIONALLY (no
  `applied`; the clobber guard cannot apply — v1's original semantics).
- **v2** (16 B/entry): `{FormID, ActorValue, prev, applied}` — clobber-guarded.
`OnSave` always writes the current version; a `version > kRecordVersion` record is
skipped (a downgrade cannot read a future layout).

CLOBBER GUARD: the ledger stores both the captured `prev` AND the value we `applied`;
Restore/ApplyPending write `prev` back ONLY when the AV still equals `applied`. If a
quest or another mod changed the AV while our override was live, the newer value
wins and we just drop our stale record — APMF never silently overwrites another
author's write. ONE-CHANNEL-PER-AV assumption: the ledger keys by `(FormID, AV)`, so
at most one APMF channel may own a given AV on a given actor (true today: the three
AV channels own disjoint AVs). Sharing an AV would need per-AV refcounting.

SAVE-SAFETY BOUNDARY: only the AV channels are co-saved and therefore save-safe.
Transient facets (weapon draw, sneak, dialogue, idle, headtrack, combat-target) are
self-correcting and need no co-save. **Equipment (ch.15) is the exception that
matters: it mutates PERSISTED inventory (unequip) but its re-equip pointer is
RAM-only (NOT co-saved).** So a client must NOT hold an equipment claim across a
save — it self-heals (item stays in inventory, the AI re-equips), but APMF does not
restore it. Do not add a persisted-state channel without either co-saving it or
documenting the same boundary.

## Logging

**#16 — Log hex via `apmf::log::Hex`, never the `{:X}` spec.** On the deck (v0.2.2)
APMF's build rendered EVERY `{:08X}`/`{:02X}`/`{:X}` as raw garbage bytes, corrupting
the log to a binary file (grep refused it) — while decimal `{}` and string `{}`
rendered clean, and the IDENTICAL toolchain/baseline formats `{:X}` correctly for MFO
(its `MFO.log` shows clean `FE08F801`). So the trigger is APMF-build-specific and was
not statically isolable (it is not a format-string typo, a non-ASCII byte, an arg-type
slip, a config/PCH/preset/baseline difference, or a custom formatter — all ruled out).
The robust fix, immune to whatever the trigger is: format hex BY HAND into an ASCII
`std::string` (`core/Log.h` `Hex(value, width)`) and log it through the string path
(`spdlog::info("… 0x{} …", apmf::log::Hex(id))`), which both projects render correctly.
NEVER reintroduce a `{:X}`/`{:x}` presentation spec in a log call; use `Hex`. If the
underlying cause is ever found and fixed, this rule can relax — until then it keeps the
log (the primary field-validation channel) plain text.

## Framework coexistence

**#17 — APMF must coexist with arbitrary modlists: it is a good citizen, never assumes
exclusive access, and degrades rather than crashes.** APMF lives inside OTHER people's
modlists, alongside mods it has never seen and cannot enumerate — it must never be the
thing that derails one. Concretely:
- **Vtable hooks (`write_vfunc`) ONLY.** They chain cleanly — another mod hooking the
  same slot wraps the previous entry and calls the original, so independently-authored
  hooks compose without coordination. **NO raw call-site patches**
  (`write_call`/`write_branch`/a hand-rolled 5-byte overwrite/trampoline at a shared
  address): they stomp bytes at one address, and two uncoordinated patchers at the same
  site is a collision, not a composition. **Cautionary case:** the T4
  `TESActionData::Process` probe's devirtualised fallback patched valhallaCombat's
  known call site with `SKSE::GetTrampoline().write_call<5>` — SCAR.dll patches the
  SAME AI-attack-start site, and the two collided into an execute-AV CTD in ordinary
  combat (2026-09-03, not even during a probe keypress; see `Docs/PROBE-ALLOWANCE.md`
  "T4 — DEFERRED" for the full crash record). T4 was removed rather than patched
  around — this is the standing reason why. See also #6 (call-site offsets are also
  version-fragile; this is a second, independent reason they're banned).
- **Engine-answer-first.** Every thunk calls the stored original before deciding
  anything; it only ever flips the engine's own YES to NO, only for an actor APMF
  itself holds a claim on. Never invent a YES, never manufacture behavior, never
  re-assert (#0).
- **Verify at install.** RTTI-derivation check per vtable symbol before installing
  (`core/Allowance.h`'s `DerivesFrom`, the `CombatMagicCasterArmor` lesson) — a symbol
  that doesn't derive the expected class is skipped, not installed blind. At the hot
  path, a foreign/unrecognized object (a vtable APMF never installed on reaching the
  thunk) returns the benign default without touching its members.
- **Read, don't own.** Consult engine/APMF state via the lock-free RCU `ControlMap`
  snapshot (#12); never assume exclusive ownership of an actor, a vtable, or a call
  site just because APMF is loaded.
- **ADAPT, don't degrade; never crash.** When a preferred attach point is contested by
  another mod, absent on a runtime, or devirtualised, ROUTE TO AN ALTERNATIVE SEAT that
  covers the same facet — do not simply disable. The template is designed with
  REDUNDANT per-facet attach points to make this possible: choose the viable seat at
  install time (RTTI-verify, confirm the vtable/site is usable and not already
  stomped), and fall back to the next when the preferred one isn't viable. Examples of
  the redundancy: combat ACTIONS are covered by BOTH T1 (combat-tree leaves, chain-safe
  vtable) AND T4 (action-data) — so T4's call-site seat colliding with SCAR does NOT
  lose combat-action coverage, it falls through to T1; CASTING is covered by T1 Cast
  leaves AND T2 `CheckCast`. Only when a facet has NO usable seat is it disabled (a
  documented coverage gap), and even then APMF logs and continues — never a bad call,
  never a crash.

The current codebase satisfies this: every hook in `native/core/` is `write_vfunc` on a
version-pinned `VTABLE_*` symbol; no call-site patch remains anywhere in the tree after
T4's removal (grep-verified: no `write_call`/`write_branch`/`AllocTrampoline` in
`native/`).
