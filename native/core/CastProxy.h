#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the DELIVERY-FLIP PROXY POOL (was core/CastExecutor.{h,cpp}).
//
// WHAT THIS IS NOW. The forced cast DRIVE that used to live here (PhaseSelect ->
// PhaseRest -> PhaseDrawn -> PhaseFire -> PhaseHold -> ParkHand, the wall-clock
// Budget plumbing, the `CastSpellImmediate` guaranteed-delivery fallback and the
// ch.8 `+ACT` opt-in) is RETIRED (feat/ai-cast-seats-impl, marth 2026-09-05).
// It is superseded by the five ENGINE SEATS in core/CastSeats.cpp +
// core/EquipGate.cpp, which make the NPC's OWN combat AI select, equip, charge,
// aim, fire and channel the claimed spell at the claimed target -- a real
// animated native cast, with no APMF equip/anim/cast write at all. See
// Docs/CHANNEL-MAP.md ch.8b and Docs/INVARIANTS.md #20.
//
// WHAT SURVIVED, AND WHY IT MUST. One engine fact the seats cannot answer around
// (disassembly-CERTAIN, `FindTargets` 0x5bc160 @0x5bc98a): a **kSelf-delivery**
// spell ALWAYS lands on the caster's own reference -- the Self branch reads the
// caster's ref, not `desiredTarget`, so redirecting `GetMagicTarget` (seat 0x0A)
// cannot make Fast Healing land on an ally. The only honest fix is a
// delivery-flipped COPY of the spell (identical data + shared source `Effect*`,
// `delivery = kTargetActor`) that the AI selects and casts instead. That copy is
// what this pool mints.
//
// Two lifecycle rules ride on it, both INVARIANTS #19 (a runtime-minted form must
// never be reachable from a save, and must drop its borrowed pointers before a
// load):
//   * TRANSIENT TEACH. The AI's own equip selector can only choose a spell the
//     actor KNOWS, and `CombatInventory::Rebuild` only builds a
//     `CombatInventoryItem` for a spell in the actor's spell list -- so the proxy
//     is `AddSpell`'d when the cast claim is applied and `RemoveSpell`'d when it
//     is released. Never persisted as a real learned spell.
//   * BORROWED POINTERS. `Configure` shares the SOURCE spell's `Effect*` objects
//     BY POINTER. `ResetAll` clears them BEFORE nulling a slot, or the load-time
//     dynamic-form purge would free a live spell's effect array through a dead
//     proxy (MFO's `native/Actuation_Direct.cpp` lesson).
//
// CLIENT DEPENDENCY (documented, not enforceable from here). Teaching the proxy
// only makes it selectable once the actor's `CombatInventory` REBUILDS -- the one
// confirmed dirty-trigger is a change of `Actor::GetCombatStyle()` (RE notebook
// D6). MFO already swaps `MFO_CastStyle` on a commanded cast, which dirties it.
// APMF deliberately does NOT force a rebuild: there is no RTTI/struct-verified,
// version-robust lever for it on the pinned CommonLib (#7), and inventing one
// would be manufacturing the AI's decision (#0).
//
// THREADING. Every entry point here is WRITER/MAIN-THREAD ONLY -- the
// `ControlMap::Drain` seat (where `Acquire` runs, from `ApplyRequest`), the
// `apmf::mainthread` pump (where `Free` runs, one hop AFTER the release has been
// published -- see channels/CastCompose.cpp), and the SKSE save/revert/preload
// callbacks. It makes engine calls (`AddSpell`/`RemoveSpell`/`DeselectSpell`) and
// must never be reached from a combat-thread seat.
// ============================================================================

namespace RE { class SpellItem; }

namespace apmf::castproxy {

    // Mint (or re-target) this owner's delivery-flip proxy for `a_src` and TEACH
    // it to the owner so the AI's own inventory can build an item for it.
    // Returns the proxy's FormID, or 0 when no slot is free / the actor is not
    // loadable / the form factory refuses -- in which case the CALLER must leave
    // the claim's proxy at 0 rather than let the AI cast the original kSelf form
    // at an ally (it would silently heal the caster). WRITER/MAIN THREAD ONLY.
    RE::FormID Acquire(RE::FormID a_owner, RE::SpellItem* a_src);

    // Un-teach + deselect + release this owner's slot. Idempotent; a no-op for an
    // owner with no slot. MUST run AFTER the cleared claim has been published
    // (channels/CastCompose.cpp defers it through apmf::mainthread::Post for
    // exactly that reason -- see Docs/INVARIANTS.md #20's release-ordering rule).
    // WRITER/MAIN THREAD ONLY.
    void Free(RE::FormID a_owner);

    // This owner's live proxy FormID, or 0. WRITER/MAIN THREAD ONLY.
    RE::FormID FormForOwner(RE::FormID a_owner);

    // Drop ALL proxy state (revert / new game / kPreLoadGame). Clears every form's
    // BORROWED source `Effect*` FIRST, then nulls the slot, so the load-time form
    // purge can never free a live spell's effect array through a dead proxy, and
    // the fixed-size pool can never stay permanently occupied by an owner that no
    // longer exists (`ControlMap::Clear()` deliberately does not call
    // `channel->Release`, so nothing else would reset it). Makes no engine calls.
    // MAIN THREAD ONLY (the SKSE revert / kPreLoadGame seat).
    void ResetAll();

    // Un-teach + deselect every live proxy, keeping the slots (SKSE save callback).
    // A runtime 0xFF dynamic form must never be capturable into the `.ess`; a cast
    // whose proxy is pulled out from under it simply stops being selectable and the
    // AI reverts to its own choice on the next rescore -- never a corrupt save.
    // MAIN THREAD ONLY (SKSE's save callback seat).
    void PreSaveSweep();

}
