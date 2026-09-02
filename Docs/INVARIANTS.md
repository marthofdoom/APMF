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

**#4 — The gated target is main-thread-only state.** `Arbiter`'s `target` /
`handle` / `pkgAtCapture`, and every channel's engine mutation, run on the main
thread: input events (`BSInputDeviceManager`) and the `0xAD` `Actor::Update` both
dispatch there. The `engaged` flags are atomic as cheap defensive insurance, never
as a lock. Do not touch this state from any other thread. `OnActorUpdate` runs for
every actor every frame — keep the `AnyEngaged()` + `actor != target` early-outs
first, or you tax the whole game.

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
  CTD roulette). Movement DENY (ch.1) therefore uses the Address-Library-bound
  `SetDontMove` (`RELOCATION_ID(36490, 37489)`), not a `SetAIDriven` method.
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

**#10 — Adding a channel touches exactly one file.** One new `channels/*.cpp` that
subclasses `Channel` and ends with `APMF_REGISTER_CHANNEL`. No edit to CMake, the
registry, the input layer, or the arbiter. If a change would require editing the
core to add a facet, the abstraction is wrong — fix the abstraction, not the core.

## Coherence / eviction

**#11 — Momentary channels release the target.** A one-shot that holds no lasting
authority (e.g. `Dialogue`) must not leave the arbiter holding the target: call
`Arbiter::ClearTargetIfIdle()` after firing so an idle target is dropped.
