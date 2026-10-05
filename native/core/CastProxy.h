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
// `apmf::mainthread` pump (where `Unref` runs, one hop AFTER the claim's removal
// has been published -- see core/ControlMap.cpp PostProxyUnref; Drain and Pump
// share the one confirmed-main seat, Arbiter::OncePerFrame), and the SKSE save/revert/preload
// callbacks. It makes engine calls (`AddSpell`/`RemoveSpell`/`DeselectSpell`) and
// must never be reached from a combat-thread seat.
// ============================================================================

namespace RE { class SpellItem; }

namespace apmf::castproxy {

    // Which hand(s) the claim a proxy serves occupies. Part of the proxy's KEY
    // (review F3, 2026-10-05): the engine seats resolve a claim BY ITS DRIVEN FORM
    // (ControlMap::TryGetCastSeatClaimForForm), so two live claims on one actor --
    // e.g. the same heal on the left hand at ally A and on the right hand at ally
    // B -- must drive two DIFFERENT forms, or every seat resolves both hands to the
    // first claim's target. A dual claim occupies both hands and is its own key.
    enum class Hand : std::uint8_t { kRight = 0, kLeft = 1, kDual = 2 };

    // Hand this owner a delivery-flip proxy for `a_src` on `a_hand` and TEACH it so
    // the AI's own inventory can build an item for it. Keyed by (owner, source
    // spell, hand) and REFERENCE-COUNTED: if the owner already has a live proxy for
    // this exact spell on this exact hand key, the SAME form is returned and its ref
    // count goes up by one; otherwise a free slot is configured for it (ref count 1).
    // Two live slots never share a form, and a minted form whose FormID is 0 or
    // collides with another slot's is refused loudly, so the two hands' proxies are
    // always distinct FormIDs. A slot holding refs is never re-pointed at a
    // different spell. Every nonzero return is ONE ref the caller's claim owns and
    // must give back through Unref exactly once.
    // Returns 0 when no slot is free / the actor is not loadable / the form factory
    // refuses (no ref taken) -- in which case the CALLER must not let the seats serve
    // the claim: the original kSelf form cast "at an ally" would silently heal the
    // caster. WRITER/MAIN THREAD ONLY.
    RE::FormID Acquire(RE::FormID a_owner, RE::SpellItem* a_src, Hand a_hand);

    // Give back ONE claim's ref on `a_proxy` (the FormID Acquire returned to it).
    // Un-teaches + deselects + releases the slot only when the LAST ref goes, so
    // releasing one hand's claim never pulls the form out from under another live
    // claim that names it. Logged once per change, with the ref count. The single
    // choke point every claim-removal path reaches (ControlMap's ApplyRelease,
    // dual/single eviction, unload sweep and ReleaseAll). MUST run AFTER the
    // removal has been published -- the callers defer it through
    // apmf::mainthread::Post (Docs/INVARIANTS.md #20). WRITER/MAIN THREAD ONLY.
    void Unref(RE::FormID a_owner, RE::FormID a_proxy);

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
    // Runs on SKSE's save callback seat (logged with whether that is the Drain
    // thread). Every swept slot keeps its owner and refs; ReteachLive puts them back.
    void PreSaveSweep();

    // Re-teach every slot that still holds refs (review F1, 2026-10-05). The save
    // sweep above un-teaches the proxies of claims that are still LIVE, and only
    // Acquire teaches -- a client that keeps its claim alive with Repoint heartbeats
    // never calls it again, so its proxy stayed unknown (unselectable) for the rest
    // of the claim. plugin.cpp's OnSave Posts this through apmf::mainthread, so it
    // runs on the next Pump (after Drain, on the confirmed main seat), i.e. after
    // the save call has returned; a load that follows Discard()s it. Logs each
    // re-teach. WRITER/MAIN THREAD ONLY.
    void ReteachLive();

}
