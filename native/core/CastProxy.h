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
// WHAT SURVIVED. A delivery-flipped COPY of a kSelf spell (identical data + shared
// source `Effect*`, `delivery = kTargetActor`) that the AI selects and casts at an
// ally instead -- the FIELD-PROVEN road for ally heals (2026-09-05/06).
// CORRECTED 2026-10-06 (review F6, APMF-B62): the reason this used to give -- "a kSelf
// spell ALWAYS lands on the caster; the Self branch never reads desiredTarget" -- is
// false on all three builds. FindTargets' Self branch (AE 0x5BC98A / SE 0x54D508 /
// 1.7.104 0x5CB4D1) applies a kSelf spell to `MagicCaster::desiredTarget` (+0x20) when
// that resolves to an Actor, else to the caster, and seat 0x0A FEEDS desiredTarget
// (AE 0x89EE30 -> 0x5BB720 -> SetDesiredTarget 0x5BE720). Whether an un-proxied kSelf
// claim at an ally would land on the ally is UNPROVEN (never observed); the proxy stays
// because it is the proven road, not because the other is proven impossible.
//
// THE REVERSE FLIP (feat/apmf-self-delivery-proxy, marth 2026-10-05: "a self cast is
// valid if its needed, just needs to be animated"). An AIMED / TOUCH / TARGET-ACTOR
// beneficial spell claimed at the claimant itself (an explicit self target) has the
// opposite problem: aimed, it is a projectile at its own shooter; touch, a reach test
// that cannot pick the caster; target-actor, an aim controller pointed at itself
// (APMF-B39 F1/F2). So the pool also mints a SELF-FLIP copy (`delivery = kSelf`,
// same shared effects) and the seats drive that. A kSelf form takes the engine's own
// self road, the one a native Oakflesh self claim already takes: the classifier keys
// its self=1 row natively (CombatMagicItemData ctor [+0x4c] = GetDelivery()==kSelf,
// AE 0x81D5BC / SE 0x780F5C / 1.7.104 0x832AAC), no aim controller is built (49081
// builds one for Aimed/TargetActor only: AE 0x89EBF3 / SE 0x808392 / 1.7.104
// 0x8B4093), and FindTargets' Self branch (AE 0x5BC98A / SE 0x54D508 / 1.7.104
// 0x5CB4D1) applies it to the caster (desiredTarget if it resolves to an Actor --
// seat 0x0A answers the claimant -- else the caster itself; no projectile). The
// direction of a flip is fixed by the SOURCE's delivery (kSelf -> kTargetActor,
// anything else -> kSelf), and the direction is part of the pool key.
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

namespace RE { class SpellItem; class Actor; }

namespace apmf::castproxy {

    // Which hand(s) the claim a proxy serves occupies. Part of the proxy's KEY
    // (review F3, 2026-10-05): the engine seats resolve a claim BY ITS DRIVEN FORM
    // (ControlMap::TryGetCastSeatClaimForForm), so two live claims on one actor --
    // e.g. the same heal on the left hand at ally A and on the right hand at ally
    // B -- must drive two DIFFERENT forms, or every seat resolves both hands to the
    // first claim's target. A dual claim occupies both hands and is its own key.
    enum class Hand : std::uint8_t { kRight = 0, kLeft = 1, kDual = 2 };

    // Which way a proxy flips its source's delivery. kToTargetActor: a kSelf spell
    // claimed at ANOTHER actor (the original delivery flip). kToSelf: an Aimed / Touch /
    // TargetActor spell claimed at the claimant ITSELF (the self-flip proxy,
    // feat/apmf-self-delivery-proxy). Part of the pool key; Acquire refuses a direction
    // the source's own delivery does not call for.
    enum class Flip : std::uint8_t { kToTargetActor = 0, kToSelf = 1 };

    // Hand this owner a delivery-flip proxy for `a_src` on `a_hand`, flipped `a_flip`,
    // and TEACH it so the AI's own inventory can build an item for it. Keyed by (owner,
    // source spell, hand, flip) and REFERENCE-COUNTED: if the owner already has a live proxy for
    // this exact spell on this exact hand key, the SAME form is returned and its ref
    // count goes up by one; otherwise a free slot is configured for it (ref count 1).
    // Two live slots never share a form, and a minted form whose FormID is 0 or
    // collides with another slot's is refused loudly, so the two hands' proxies are
    // always distinct FormIDs. A slot holding refs is never re-pointed at a
    // different spell. Every nonzero return is ONE ref the caller's claim owns and
    // must give back through Unref exactly once.
    // Returns 0 when no slot is free / the actor is not loadable / the form factory
    // refuses, or `a_flip` does not match the source's delivery (kToTargetActor needs a
    // kSelf source, kToSelf a non-kSelf one) (no ref taken) -- in which case the CALLER
    // must not let the seats serve the claim: the original kSelf form cast "at an ally"
    // would silently heal the caster, and the original aimed form cast "at the caster"
    // is a ray at its own shooter. WRITER/MAIN THREAD ONLY.
    // A kToSelf mint also arms a passive LANDING watch (principle 5), checked by
    // OnOwnerUpdate below on the owner's own update: it logs `[castproxy] ... self-flip
    // proxy ... LANDED on the caster` once, or, 5 s after the slot's last ref went without
    // one, `... NOT seen on the caster`. Read-only.
    // Slot choice for a mint (review F3): a free slot already mirroring this source with
    // this flip, else a never-minted slot, else the one freed longest ago -- a freed form
    // can still be the `spell` of a live effect until it runs out (APMF-B58).
    RE::FormID Acquire(RE::FormID a_owner, RE::SpellItem* a_src, Hand a_hand, Flip a_flip);

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
    // `channel->Release`, so nothing else would reset it). Also disarms every self-flip
    // landing watch (under each watch's leaf mutex). Makes
    // no engine calls. MAIN THREAD ONLY (the SKSE revert / kPreLoadGame seat).
    void ResetAll();

    // The self-flip LANDING WATCH's check (principle 5, passive). Called for EVERY NPC from
    // core/Hook.cpp's Character 0xAD thunk (via Arbiter::OnActorUpdate), right AFTER that
    // actor's own Actor::Update, on whichever worker thread runs it -- so the owner's
    // active-effect list is read on the thread that runs the owner's own effect update, not
    // from the player seat (review F4). One relaxed load when no watch is armed; otherwise
    // relaxed FormID compares, and for the owner a try_lock of that watch's leaf mutex (never
    // waits) and a read of its own list. Logs; changes nothing. ANY THREAD.
    void OnOwnerUpdate(RE::Actor* a_actor);

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
