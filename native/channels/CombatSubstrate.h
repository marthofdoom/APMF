#pragma once
#include "APMF_API.h"

#include <cstdint>

namespace RE {
    class Actor;
    class CombatController;
    class CombatMagicCaster;
    class TESObjectREFR;
    class CombatGroup;
}

// ============================================================================
// THE COMBAT SUBSTRATE (kIntent_CombatSubstrate, ABI v23; slice S2a of MFO's
// Docs/COMBAT-SUBSTRATE-DESIGN.md, INVARIANTS #0 (j)). Harbinger forces combat on an
// out-of-combat follower as a pure SUBSTRATE so the engine's own animated cast road
// (the CombatMagicCaster seats ch.8b answers) exists, opens the portal for the cast
// leaves only, and DENIES every other facet the controller switches on. See
// channels/CombatSubstrate.cpp for the mechanism and the RE evidence.
//
// Everything here is GAME THREAD unless it says ANY THREAD. The ANY THREAD reads are
// lock-free (a fixed slot table of atomics): they are what the combat-thread seats
// (core/ActionGate.cpp, the ch.22 slot-0x99 thunk, the engagement hold's detection
// seat, the caster census's 0x0B observer) consult.
// ============================================================================

namespace apmf::combatsubstrate {

    // kDataLoaded, AFTER reentrydeny::Install, actiongate::Install, castertypecensus::Install
    // and combatentry::hold::Install (it rides their seats). Reads [CombatSubstrate]; resolves
    // the anchor records in APMF.esl; verifies every address it calls; registers the hit and
    // combat-state event sinks. Refusal (logged once, reason kept) refuses every
    // kIntent_CombatSubstrate claim and the dry run. Idempotent.
    void Install();
    bool        Installed();             // ANY THREAD
    const char* NotInstalledReason();    // ANY THREAD, never null

    // ControlMap::EnqueueRequest's synchronous gate for kIntent_CombatSubstrate (ANY THREAD):
    // the reason the request is refused, or nullptr to accept it.
    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param);

    // Arbiter::OncePerFrame, after Drain and the main-thread pump. Opens, serves, closes and
    // drops substrates; runs the dry-run scheduler and the P3 / P6 probe lines. One relaxed
    // load while nothing is open, requested or scheduled.
    void Poll();

    // kPreLoadGame / revert: forget every substrate (the world is being replaced). No engine
    // call: the controllers of the outgoing world go with it.
    void ResetAll(const char* a_why);
    // kPostLoadGame / kNewGame, POSTED to the main-thread pump: the anchor goes home, and a
    // controller the incoming SAVE carries against the anchor (a save made while a substrate
    // was open) is cleaned (see the .cpp, "LOAD").
    void AfterLoad(const char* a_why);

    // ---- ANY THREAD (lock-free) ----
    // At least one substrate holds a slot (live or dropping). One relaxed load.
    bool AnyLive();
    // The anchor reference's FormID (0 before Install resolved it).
    RE::FormID AnchorId();
    // A substrate is suppressing combat behaviour on `a_actor` right now: a live holder whose
    // real-combat stand-down has not fired. ch.24 treats such an actor as NOT in combat.
    bool Suppressing(RE::FormID a_actor);
    // ABI v26 GetSubstrateState (APMF_API::SubstrateState): kSubstrateState_Held for a live holder whose
    // real-combat trigger has not fired (its combat is SUBSTRATE-ONLY), kSubstrateState_Real once it fired
    // (the combat counts; the substrate is dropping), kSubstrateState_Anchor for the anchor actor itself,
    // kSubstrateState_None otherwise (also before Install / when not installed). Lock-free.
    std::uint32_t StateOf(RE::FormID a_actor);

    // core/ActionGate.cpp: the leaf ALLOW-LIST. `a_leafIdx` indexes apmf::cbt::kLeaves (0..69),
    // then apmf::cbt::kSearchLeaves (70..73), then apmf::cbt::kParamLeaves (74..94). `a_movement` = ActionGate's movement-leaf
    // classification (the P6 "movement-leaves-denied" count). kNotHolder = no substrate on
    // the actor (the caller goes on as before); kAllow = the leaf runs; kDeny = force-fail it
    // with the ForceFail act()/pop() PAIR, then call LeafDenied.
    enum class Leaf : std::uint8_t { kNotHolder, kAllow, kDeny };
    Leaf ForLeaf(RE::FormID a_actor, std::uint32_t a_leafIdx, bool a_movement);
    void LeafDenied(RE::FormID a_actor, std::uint32_t a_leafIdx, bool a_movement);
    // The node index count the allow-list knows (kLeaves + kSearchLeaves + kParamLeaves): EVERY
    // CombatBehaviorTreeNodeObject* vtable the MIT fork ships (review F1 of a512763).
    inline constexpr std::uint32_t kLeafCount = 95;

    // channels/CombatReentryDeny.cpp's slot-0x99 thunk, at StartCombat's TARGET IsDead(false)
    // return site: true = answer "dead" (StartCombat refuses). `a_target` is the IsDead `this`
    // (StartCombat's target), `a_caller` is StartCombat's own `this` (rdi at that site, handed
    // in by the 17-byte stub). Refuses when the target is the anchor and the caller is not a
    // live holder that is still under its substrate.
    bool RefuseAnchorTarget(RE::Actor* a_target, RE::Actor* a_caller);

    // The anchor out of `a_g`'s targets through the engine's own CombatGroup::RemoveTarget (the verified row
    // Substrate.CombatGroup.RemoveTarget; DropSub / AfterLoad use the same call). channels/AnchorGuard.cpp's
    // leak sweep. GAME THREAD, a group read from a live controller this frame.
    void RemoveAnchorFromGroup(RE::CombatGroup* a_g);

    // channels/CombatEntry.cpp's detection seat (CombatGroup::UpdateTargetDetection's two call
    // sites), BEFORE the engine's update: is `a_target` the anchor? If so the caller runs the
    // engine's update and then AnchorKnowledge. Otherwise NoteGroupTarget is the real-combat
    // trigger (a): a target other than the anchor in a holder's group.
    bool IsAnchorRef(const RE::TESObjectREFR* a_target);
    void NoteGroupTarget(void* a_group, RE::TESObjectREFR* a_target);
    bool MusicDebtPending();
    // ^ ANY THREAD, one load: a holder still owes a put-back of Harbinger's music 0 (review F1 of b1c78b2), so the
    // detection seat must keep calling NoteGroupTarget even with no substrate open. NoteGroupTarget acts on music
    // only inside the engine's own locked update of the group (the seat checks the lock's writer thread).
    // After the engine's update of the ANCHOR in `a_group`: when a live holder that is still
    // under its substrate is a member of the group, the anchor's group entry is kept KNOWN
    // (the held engagement's own un-lose write, INVARIANTS #0 (i) condition 1, for the anchor
    // only), so the group never searches for it. Counted per holder.
    void AnchorKnowledge(void* a_group, RE::TESObjectREFR* a_anchor);

    // core/CasterTypeCensus.cpp's 0x0B NotifyStartCast observer: the engine released a spell
    // from a hand (the "fired through the animated path" signal). Records it for a holder; the
    // game thread closes a fire-and-forget substrate on it.
    void NoteCastFired(RE::CombatController* a_cc, RE::CombatMagicCaster* a_caster);

}
