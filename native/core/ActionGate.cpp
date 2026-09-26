#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/CombatBehaviorRE.h"
#include "core/ControlMap.h"
#include "core/ActionGate.h"

#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>

// Win32 INI read for [Probe.mvcbt] -- same hand-declared extern every other
// INI-gated file in this project uses (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// T1 -- COMBAT-ACTION allowance (ch.7, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven T1Probe (Docs/PROBE-ALLOWANCE.md
// "Probe 1"): SAME thunk shape (one thunk installed at slot 0x02 on all 70
// `VTABLE_CombatBehaviorTreeNodeObject_*` leaves, RTTI-verified via
// core/Allowance.h's InstallOnVtables), SAME +0x158 actor-resolution (both
// hypotheses tried -- hypothesis B, the +0x20 hop, is the one that resolves
// on 1.6.1170, but the fallback to A is kept exactly as the probe proved
// safe/necessary, INVARIANTS #17), SAME deny mechanism (invoke
// `CombatBehaviorForceFail`'s own ORIGINAL act() rather than a
// hand-reconstructed `SetFailed` call -- T1Probe.cpp's file header records
// the field crash that made the hand-rolled call the wrong approach).
//
// THE NODE PROTOCOL -- act() and pop() are a PAIR (feat/ai-cast-suppress,
// 2026-09-04, the recurring deck CTD's root cause; full RE in
// core/CombatBehaviorRE.h "The node protocol"). Every node's act() (slot 0x02)
// PUSHES its per-thread state onto the CombatBehaviorThread's DATA STACK
// (ForceFail: 4 bytes; a leaf: sizeof(T) rounded, e.g. CastImmediateSpell
// 0xC; the ContextMagic CreateContextNode1: 0x30 = the built context + the
// saved context window), and the tree runner calls the SAME node's pop()
// (slot 0x03) in the SAME step right after an act() that ascended -- pop()
// POPS exactly what its own act() pushed (and, for the context node, releases
// two NiPointers inside the context and restores the window). Substituting
// ForceFail's act() for a node's act() while the node's OWN pop() still runs
// therefore unbalances the data stack by (sizeof(node state) - 4) bytes AND,
// for the ContextMagic node, makes its pop() release two NiPointers read from
// the ENCLOSING frame's live data (a premature CombatInventoryItem free) and
// restore the context window from garbage. That is what corrupted the
// thread's state until the runner walked a garbage `cur_node` during an
// interrupt unwind (`call [rax+0x28]`, rax=0 -- the three 2026-09-04 deck
// crashes). The fix is structural, not a workaround: the deny now installs a
// SECOND thunk at slot 0x03 (pop) on the SAME vtables and, for a denied act(),
// records {node, control} in a THREAD-LOCAL "pending ForceFail pop"; the very
// next pop() for that {node, control} on that thread runs ForceFail's own
// ORIGINAL pop() (`top -= 4`) instead of the node's own -- so a denied node
// executes EXACTLY ForceFail's act()+pop() pair, byte-for-byte the engine's own
// failure protocol. Thread-local is exact here: the runner calls pop()
// synchronously on the same OS thread with no intervening node call (the step
// function checks phase, flips it, and calls pop() -- nothing else can pop in
// between); a mismatch is impossible by construction and is still counted +
// logged once as a protocol anomaly rather than trusted silently. If ForceFail's
// pop() cannot be resolved at install, the WHOLE deny is refused (claims become
// arbitration-only, logged) -- an unpaired deny is the corruption, never an
// acceptable degrade (INVARIANTS #17/#18).
//
// WHAT'S NEW vs the probe: the claim source. The probe read a throwaway
// hotkey-driven FormID set (core/ProbeClaimSet, now REMOVED -- superseded by
// this real channel, never left fighting the probe on the same vtables);
// this reads a REAL ControlMap claim on APMF_API::kIntent_CombatAction via
// the lock-free RCU TryGetOwningClaim (Docs/ALLOWANCE-TEMPLATE.md §3), keyed
// by a CATEGORY bitmask (APMF_API::CombatActionCategory, carried in
// APMF_Param::ival): a claim denies a LEAF only when the leaf's own
// classified category bit is SET in the claim's mask -- "claim ch.7 with
// kCombatActionCat_Offense" denies every offensive leaf and leaves
// movement/defense/utility leaves completely untouched (never a blanket
// lock, never an invented deny for an unclassified leaf).
//
// CLASSIFICATION (install-time, exact-name lookup against apmf::cbt::kLeaves
// -- Docs/ALLOWANCE-TEMPLATE.md's graduation brief's own list): offense =
// Attack, AttackLow, Bash, PowerAttack, RangedAttack, SpecialAttack,
// GroundAttack, FlyingAttack, CastImmediateSpell, CastConcentrationSpell,
// CastShout, PrepareDualCast. NOTE: "CombatBehaviorPowerAttack" has NO
// separate leaf on this build's 70-leaf catalog (CombatBehaviorRE.h) --
// CombatBehaviorAttack itself appears to cover both normal and power
// attacks; there is no standalone node to classify, and this is logged once
// at install (never silently dropped). "CombatBehaviorAttackFromCover"
// exists as a real leaf but is deliberately NOT classified here (not named
// in the brief) -- a later pass can add it on purpose if marth wants
// cover-attacks folded into "offense" too. Every OTHER leaf (movement,
// block, dodge, flee, cover, search, selectors, ...) is UNCLASSIFIED
// (category 0) and is NEVER looked up against the claim map -- the thunk
// skips actor resolution entirely for those, which is both the correct
// "everything else stays allowed" semantics AND the cheap path for the ~58
// of 70 leaves that are never deniable through this channel.
//
// RETIRED HERE (feat/ai-cast-seats-impl, 2026-09-05) -- read this before adding
// anything back. THREE things this gate used to do are gone, and two of them are
// gone because they now DIRECTLY CONTRADICT how the cast facet works:
//
//  1. The `CombatBehaviorContextMagic` CreateContextNode act()/pop() deny
//     (`apmf::cbt::kCastContextNodes`) is REMOVED, hooks and classification both.
//     Its whole purpose was to stop the AI ever BUILDING its magic context, so
//     APMF's own forced drive could equip and animate a cast without the AI
//     racing it for the hand. That drive is retired; the cast facet is now
//     delivered BY the AI's own magic branch (core/CastSeats.cpp), so denying the
//     context node would suppress the very cast a `kIntent_Cast` claim asks for.
//     It is also the seat whose act()-only deny caused a months-live CTD (the
//     data-stack imbalance recorded under "THE NODE PROTOCOL" above and in
//     INVARIANTS #18) -- removing it is a real, permanent risk reduction, not
//     just a cleanup. COVERAGE TRADED, stated plainly: an explicit
//     `kIntent_CombatAction` claim naming Cast/Offense no longer suppresses the
//     CONTEXT-BUILD path, only the four cast LEAVES (below), so the AI may still
//     build a magic context and equip a spell it then cannot fire. Recorded as an
//     open gap in Docs/DENY-COMPLETENESS-AUDIT.md.
//
//  2. `kIntent_Cast` (ch.8b) no longer contributes an implicit
//     `kCombatActionCat_Cast` deny. It used to mean "the client is firing its own
//     cast, keep the AI's out of the way"; it now means "the AI IS the one
//     casting, on APMF's answer" -- so contributing a Cast deny would make a cast
//     claim silence its own cast. A `kIntent_Cast` claim is invisible to this
//     gate today.
//
//  3. A ch.8 `kIntent_SelectSpell` claim with the old `+ACT` drive opt-in bit no
//     longer contributes anything either -- that bit is retired ABI-wide
//     (APMF_API.h) along with the drive it selected.
//
// WHAT REMAINS, AND WHY IT IS STILL LOAD-BEARING. The four cast LEAVES keep their
// act()/pop() deny and their Cast|Offense classification, because they serve a
// DIFFERENT, still-live intent: `kIntent_CombatAction`. A client that genuinely
// wants "this actor must not cast at all" claims ch.7 with `kCombatActionCat_Cast`
// (or Offense) and gets exactly that. That is NOT subsumed by the cast seats --
// the seats make a CLAIMED cast happen; they do not suppress casting in general.
//
// SCOPE ("when the cast/offense deny arms"). Exactly ONE source now: a winning
// ch.7 `kIntent_CombatAction` claim whose own `param.ival` mask names the leaf's
// classified category. Arm = the claim's Drain publish, disarm = its release
// publish; no separate flag to race, and never an implicit deny inferred from
// another intent.
//
// PER-HAND (feat/deny-perhand): this gate is PER-ACTOR, not per-hand -- neither
// the tree node (CombatBehaviorTreeNode's fixed 10-vfunc layout, no per-instance
// data beyond it) nor CombatBehaviorTreeControl documents a hand/casting-source
// field at this seat, so there is no RTTI/struct-verified way to scope THIS
// deny to one hand. Documented gap (INVARIANTS #18) -- Docs/
// DENY-COMPLETENESS-AUDIT.md row 8b. The per-hand requirement IS met at the two
// seats that DO carry a native hand signal: CastGate.cpp
// (MagicCaster::GetCastingSource()) and EquipGate.cpp
// (CombatInventoryItem::itemSlot.equipSlot), both scoping their kIntent_Cast
// narrowing via Allowance::AllowedCastForHand.
// ============================================================================

namespace apmf::actiongate {

    namespace {

        // Exact leaf names to classify "offense" -- verbatim from the
        // graduation brief. See the file header for the two names that don't
        // land 1:1 on this build's real leaf catalog.
        constexpr std::array<const char*, 12> kOffenseLeafNames{ {
            "CombatBehaviorAttack",
            "CombatBehaviorAttackLow",
            "CombatBehaviorBash",
            "CombatBehaviorPowerAttack",   // no separate leaf on this catalog -- see header comment
            "CombatBehaviorRangedAttack",
            "CombatBehaviorSpecialAttack",
            "CombatBehaviorGroundAttack",
            "CombatBehaviorFlyingAttack",
            "CombatBehaviorCastImmediateSpell",
            "CombatBehaviorCastConcentrationSpell",
            "CombatBehaviorCastShout",
            "CombatBehaviorPrepareDualCast",
        } };

        // The FOUR cast leaves get kCombatActionCat_Cast IN ADDITION to Offense, so a
        // kIntent_CombatAction claim can name "no casting" (Cast) separately from "no
        // offense at all" (Offense, which still denies them too -- they carry both
        // bits). RangedAttack et al. are Offense-only, so a Cast-only claim leaves them
        // firing. All four names land 1:1 on this build's 70-leaf catalog.
        // NOTE (feat/ai-cast-seats-impl): kIntent_Cast no longer maps onto this
        // category -- see the file header's RETIRED block.
        constexpr std::array<const char*, 4> kCastLeafNames{ {
            "CombatBehaviorCastImmediateSpell",
            "CombatBehaviorCastConcentrationSpell",
            "CombatBehaviorPrepareDualCast",
            "CombatBehaviorCastShout",
        } };

        // ====================================================================
        // PURSUIT LEASH -- ch.23, kIntent_PursuitLeash (ABI v16, ClickUp 86e3ex5ve batch L,
        // 2026-09-25). Its OWN intent, arbitrated separately from ch.7: these leaves carry NO
        // ch.7 category (g_category never names them), and the seats below consult the
        // ch.23 winner only. They share ch.7's act()/pop() thunks (one install per slot per
        // vtable -- a second hook on the same slot would chain two APMF thunks) and the
        // ForceFail pair.
        //
        // THE TEN PURSUIT LEAVES -- goal = the COMBAT TARGET. Measured on both unpacked
        // executables by walking each leaf's act()/update() call graph to the CombatPath type
        // it builds (agentlog apmf-pursuit.md "RE step A"):
        //   Advance, Reposition, Chase      Generic<FindTargetLocation> -> Ref   (melee close-in / out of position; flank chase)
        //   PursueTarget, Stalk             Standard|Flight -> Ref               (low-combat pursue; flanking stalk)
        //   Flank                           Generic<Flank> -> Ref
        //   FlankDistant                    Generic<FlankDistant> -> Location
        //   Surround                        Standard|Flight -> Ref (+ Generic<Retreat>)
        //   MaintainOptimalRange            StraightPath -> Location             (the ranged close-to-range)
        //   FindAttackLocation              Generic<FindAttackLocation>          (the ranged reposition)
        // THE FOUR SEARCH LEAVES -- goal = the combat GROUP's SEARCH CENTRE (CombatGroup
        // +0x100, the position SearchCenter::Enter itself reads; core/CombatBehaviorRE.h
        // kSearchLeaves): Search, SearchCenter, SearchLocation, SearchWander -- the 'Search'
        // tree that Low Combat's Search branch and the combat search use. Not in the 70-leaf
        // catalog, so ch.23 installs act/pop on them as well as update.
        // NOT here, on purpose: CheckUnreachableTarget (a reachability test), TrackTarget (no
        // path), the local StraightPath moves (Circle*, Strafe, Backoff, Fallback*,
        // DodgeThreat, FindLateralAttackLocation), Orbit*, flee/cover/hide/exit-water,
        // FindAllyAttackLocation (ally-relative), SearchInvestigateDoor (its goal is a DOOR,
        // not the search centre) and ReturnToCombatArea (its goal is the combat area, not a
        // target -- DENY-COMPLETENESS-AUDIT gap 14 says why neither fits the rule).
        //
        // THE RULE, for an actor whose winning ch.23 claim has a resolved leash:
        //   dist(actor, anchor) > radius  AND  dist(goal, anchor) > dist(actor, anchor)
        // -- "the move would take it FARTHER": moving straight toward the goal never leaves
        // the anchor farther than max(dActor, dGoal), so a goal nearer the anchor than the
        // actor is always allowed (it still fights whatever comes in).
        // Two seats, both the engine's own protocol (core/CombatBehaviorRE.h):
        //   act()    -- the ForceFail act()+pop() PAIR (the leaf never starts);
        //   update() -- slot 0x04: a leaf already running (it began inside the radius and has
        //               since carried the actor out) is ended through its own failure exit,
        //               SetFailed(thread, 1) + Ascend(thread); the runner then calls the
        //               node's OWN pop(). Without this half a chase begun inside the radius
        //               runs to the target, however far (an Advance runs its whole path in
        //               one leaf) -- so the leash is refused unless BOTH halves arm on all 14.
        enum class Goal : std::uint8_t { kTarget, kSearchCentre };
        struct LeashLeaf {
            const char*    name;
            REL::VariantID vtbl;
            Goal           goal;
        };
        constexpr std::array<const char*, 10> kPursuitLeafNames{ {
            "CombatBehaviorAdvance",
            "CombatBehaviorChase",
            "CombatBehaviorFindAttackLocation",
            "CombatBehaviorFlank",
            "CombatBehaviorFlankDistant",
            "CombatBehaviorMaintainOptimalRange",
            "CombatBehaviorPursueTarget",
            "CombatBehaviorReposition",
            "CombatBehaviorStalk",
            "CombatBehaviorSurround",
        } };
        constexpr std::size_t kPursuitN = kPursuitLeafNames.size() + apmf::cbt::kSearchLeaves.size();   // 14

        // Filled at Install(): index -> {name, vtable, goal}. Index < 10 = pursuit, >= 10 = search.
        std::array<LeashLeaf, kPursuitN> g_leashLeaves{};

        // vtable -> original update() (slot 0x04), leash leaves only.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_origUpdate;
        // vtable -> index into g_leashLeaves (a leash leaf; also the per-leaf counters).
        std::unordered_map<std::uintptr_t, std::size_t>    g_pursuitIdx;
        // The engine's leaf-failure exit (verified function rows, or the category is refused).
        std::atomic<std::uintptr_t> g_setFailed{ 0 };
        std::atomic<std::uintptr_t> g_ascend{ 0 };
        std::atomic<bool>           g_pursuitArmed{ false };
        std::atomic<const char*>    g_pursuitNotArmedWhy{ "not installed yet (before kDataLoaded)" };

        // The per-actor leash, written ONLY on the game thread (ch.23 Engage / OnOwnerChanged /
        // Release, and the load reset), read by the seats under a shared_lock. The anchor is
        // resolved to a HANDLE on the game thread (the seats never look up a form -- the
        // ch.20 precedent). The published claim is still the gate: no winning ch.23 claim ->
        // no deny, whatever this map holds.
        struct Leash {
            RE::FormID      anchorFid = 0;
            RE::ActorHandle anchor{};
            float           radius    = 0.0f;
        };
        std::shared_mutex                      g_leashMx;
        std::unordered_map<RE::FormID, Leash>  g_leash;
        std::atomic<std::size_t>               g_leashCount{ 0 };

        // RULE C counters (printed by PursuitHeartbeat even at zero). "seen" counts every
        // leash-leaf act() on ANY actor -- the anchor that proves the leaves run at all
        // (principle 5); the rest are for leashed actors only.
        std::array<std::atomic<std::uint64_t>, kPursuitN> g_pSeen{};      // act(), any actor
        std::array<std::atomic<std::uint64_t>, kPursuitN> g_pDenyAct{};   // act() denied (never started)
        std::array<std::atomic<std::uint64_t>, kPursuitN> g_pEndUpd{};    // running leaf ended at update()
        std::atomic<std::uint64_t> g_pPassInside{ 0 };      // actor within the radius
        std::atomic<std::uint64_t> g_pPassCloser{ 0 };      // goal no farther from the anchor than the actor
        std::atomic<std::uint64_t> g_pPassNoAnchor{ 0 };    // anchor unresolved / unloaded / dead / self
        std::atomic<std::uint64_t> g_pPassOtherSpace{ 0 };  // anchor in another cell / worldspace
        std::atomic<std::uint64_t> g_pPassNoTarget{ 0 };    // no goal to measure (no target / no search centre)
        std::atomic<std::uint64_t> g_pPassStale{ 0 };       // no leash entry, or it names another anchor
        std::atomic<std::uint64_t> g_pUpdAnomaly{ 0 };      // update() for a node that is not cur_node / phase != 1
        std::atomic<bool>          g_pUpdAnomalyLogged{ false };
        constexpr std::uint64_t    kPursuitHeartbeatMs = 30000;
        std::atomic<std::uint64_t> g_pLastHeartbeatMs{ 0 };

        // Deny-line rate limit, per actor (the combat thread can re-enter a denied leaf every
        // tree loop). Touched only on a deny, never on the pass-through path.
        std::mutex                                     g_pLogMx;
        std::unordered_map<RE::FormID, std::uint64_t>  g_pLastLogMs;
        constexpr std::uint64_t                        kPursuitLogGapMs = 2000;
        // ====================================================================

        // ====================================================================
        // PFP PHASE 0 -- movement-leaf OBSERVE-ONLY reporting (marth 2026-09-06,
        // scratchpad/progressive-facet-probe-design.md §3.1.2). ZERO new hooks:
        // this rides the SAME act() thunk already installed above on all 70
        // leaves. Movement leaves are deliberately NEVER classified into
        // g_category (see the file header -- "everything else stays allowed"),
        // so the classification here is a SEPARATE, deny-inert lookup table
        // consulted BEFORE the `leafCat == 0` early-return, purely to observe
        // and log -- it never denies anything and never changes what `orig`
        // returns.
        //
        // THE OPEN QUESTION THIS SETTLES (design doc §3.1.2, "the single most
        // consequential unknown"): does ch.1's FULL block
        // (channels/MovementDeny.cpp: KeepOffsetFromActor(self,0) +
        // SetDontMove(true)) actually stop the combat AI's OWN movement
        // branch, or does the behaviour tree keep entering movement leaves
        // underneath the block regardless? Scope: only actors that currently
        // hold a WINNING kIntent_MovementBlock (ch.1) claim -- read the same
        // way the existing deny path reads kIntent_CombatAction, one RCU
        // snapshot lookup, no new API.
        //
        // THE LEAF LIST IS A HYPOTHESIS, NOT AN ASSERTION (RULE C/"no silent
        // negatives" -- design doc §3.1.3 item 3). Only 8 of CombatBehaviorRE.h's
        // 70 leaves are corroborated by an independent source (CombatPathingRevolution's
        // OWN `CombatBehaviorNodesMovement.h`, which groups exactly these 8 under a
        // "CloseMovement" context). The remaining ~22 are HYPOTHESIS-BY-NAMING only
        // (locomotion-shaped names with no independent confirmation) -- they are
        // still instrumented, because the entire point of an observe-only probe is
        // to let the field tell us which of them actually fire, not to assume it.
        // Leaves already classified Offense/Cast, equip leaves, selectors, and
        // pure-utility/control nodes (ForceFail, Pause, DrinkPotion, FindWeapon,
        // the Find*AttackLocation trio, WaitBehindCover, CheckUnreachableTarget,
        // SearchInvestigateDoor, DiveBomb, PerchAttack) are deliberately EXCLUDED
        // as ambiguous or out of scope -- a later pass can fold any of them in once
        // this run's field data says whether they behave like locomotion.
        constexpr std::array<const char*, 8> kMovementLeafConfirmed{ {
            "CombatBehaviorAdvance",
            "CombatBehaviorBackoff",
            "CombatBehaviorCircle",
            "CombatBehaviorCircleDistant",
            "CombatBehaviorFallback",
            "CombatBehaviorFallbackToRanged",
            "CombatBehaviorReposition",
            "CombatBehaviorSurround",
        } };
        constexpr std::array<const char*, 22> kMovementLeafHypothesis{ {
            "CombatBehaviorChase",
            "CombatBehaviorDodgeThreat",
            "CombatBehaviorExitWater",
            "CombatBehaviorFindCover",
            "CombatBehaviorFlank",
            "CombatBehaviorFlankDistant",
            "CombatBehaviorFlee",
            "CombatBehaviorFleeThroughDoor",
            "CombatBehaviorFleeToAlly",
            "CombatBehaviorFleeToCover",
            "CombatBehaviorHide",
            "CombatBehaviorHover",
            "CombatBehaviorLand",
            "CombatBehaviorMaintainOptimalRange",
            "CombatBehaviorOrbit",
            "CombatBehaviorOrbitDistant",
            "CombatBehaviorPursueTarget",
            "CombatBehaviorReturnToCombatArea",
            "CombatBehaviorStalk",
            "CombatBehaviorStrafe",
            "CombatBehaviorTakeoff",
            "CombatBehaviorTrackTarget",
        } };

        std::atomic<bool> g_mvcbtEnabled{ false };   // [Probe.mvcbt] Enable, default 0

        // vtable -> movement leaf name. Populated at Install() from the two lists
        // above; a lookup HIT means "this act() call is a movement-shaped leaf,"
        // independent of g_category (which stays 0 for all of these -- never denied).
        std::unordered_map<std::uintptr_t, const char*> g_movementLeaf;

        // RULE C heartbeats -- printed even at zero by PfpHeartbeat() below.
        // "anchor": every movement-leaf act() call seen while the probe is armed,
        // ANY actor -- proves the thunk itself is alive regardless of whether
        // anyone currently holds a ch.1 claim (the fact a subject-scoped count
        // alone could never establish -- see RULE C's own worked example).
        std::atomic<std::uint64_t> g_mvcbtAnchorHits{ 0 };
        // "ch1": the subset of the above where the deliberating actor currently
        // holds a WINNING kIntent_MovementBlock claim -- the number that actually
        // answers this facet's question.
        std::atomic<std::uint64_t> g_mvcbtCh1Hits{ 0 };

        // RULE D -- high-water dedup by TRANSITION, never a timer: one line per
        // (actor, new leaf), never a repeat of the same leaf. Guarded the same way
        // AiCastSeats.cpp's throttle tables are (a small mutex; combat-thread calls
        // here are rare -- gated behind an INI flag AND a live ch.1 claim).
        std::mutex                                       g_mvcbtMx;
        std::unordered_map<RE::FormID, const char*>      g_mvcbtLastLeaf;

        // RULE E/session volume cap for the transition lines (heartbeats are far
        // fewer and are never capped -- RULE C, they must never go silent).
        constexpr std::uint64_t    kMvcbtLineCap = 1500;
        std::atomic<std::uint64_t> g_mvcbtLineCount{ 0 };

        // Heartbeat cadence. No CombatController-lifetime ("episode") signal is
        // wired at this seat, so this is a fixed main-thread interval instead of a
        // true per-episode print -- a deliberate, documented approximation, not an
        // invented episode boundary.
        constexpr std::uint64_t    kMvcbtHeartbeatMs = 30000;
        std::atomic<std::uint64_t> g_mvcbtLastHeartbeatMs{ 0 };
        // ====================================================================

        std::atomic<bool> g_installed{ false };

        // vtable runtime address -> original act() (slot 0x02; always the passthrough target).
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        // vtable runtime address -> original pop() (slot 0x03; the act()'s paired half).
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_origPop;
        // vtable runtime address -> classified category bitmask (0 = never denied,
        // and therefore never even looked up against a claim -- see header).
        std::unordered_map<std::uintptr_t, std::uint32_t>  g_category;

        // ForceFail's ORIGINAL act() + pop() -- the proven deny mechanism, as a PAIR
        // (see header). Both must resolve or the deny is refused at install.
        std::atomic<std::uintptr_t> g_forceFailAct{ 0 };
        std::atomic<std::uintptr_t> g_forceFailPop{ 0 };

        using Pop_t = void (*)(void* a_this, void* a_control);   // slot 3, type-erased

        // The paired-pop bookkeeping: set by ActThunk on a deny, consumed by the
        // very next PopThunk for the same {node, control} on this OS thread.
        struct PendingPop {
            void* node    = nullptr;
            void* control = nullptr;
        };
        thread_local PendingPop t_pending{};

        // Protocol-anomaly counter: a pop() arrived while a DIFFERENT pending
        // pop was recorded (impossible by the runner's construction -- counted
        // and logged once, never trusted silently).
        std::atomic<std::uint32_t> g_popAnomalies{ 0 };
        std::atomic<bool>          g_popAnomalyLogged{ false };

        // Resolve the deliberating actor from a node's `control` argument -- BOTH
        // +0x158 hypotheses, exactly the guard T1Probe field-proved (see the
        // deny path below for the full history). Shared by the deny path AND the
        // PFP movement-probe path so the fragile offset logic exists in exactly
        // one place. Declared BEFORE ActThunk (which calls it) -- this file has
        // no header-declared helpers, so ordering inside the anonymous namespace
        // is the only thing that makes it visible.
        RE::FormID ResolveDeliberatingActor(void* a_control) {
            if (!a_control) return 0;
            auto* tc      = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);
            void* p0x158 = tc->master_controller;
            if (!p0x158) return 0;
            auto* ctrlA = reinterpret_cast<apmf::cbt::ControllerMini*>(p0x158);
            if (auto a = ctrlA->attackerHandle.get()) return a->GetFormID();
            void* cbcPlus20 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p0x158) + 0x20);
            if (cbcPlus20) {
                auto* ctrlB = reinterpret_cast<apmf::cbt::ControllerMini*>(cbcPlus20);
                if (auto b = ctrlB->attackerHandle.get()) return b->GetFormID();
            }
            return 0;
        }

        // ch.23 PURSUIT LEASH: the same two +0x158 hypotheses as ResolveDeliberatingActor, in the
        // same order, but returning the controller that named the actor too -- the combat
        // target is read from THAT controller (targetHandle, +0x2C), never from a guess.
        apmf::cbt::ControllerMini* ResolveController(void* a_control, RE::NiPointer<RE::Actor>& a_actor) {
            a_actor.reset();
            if (!a_control) return nullptr;
            auto* tc     = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);
            void* p0x158 = tc->master_controller;
            if (!p0x158) return nullptr;
            auto* ctrlA = reinterpret_cast<apmf::cbt::ControllerMini*>(p0x158);
            if (auto a = ctrlA->attackerHandle.get()) {
                a_actor = a;
                return ctrlA;
            }
            void* cbcPlus20 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p0x158) + 0x20);
            if (cbcPlus20) {
                auto* ctrlB = reinterpret_cast<apmf::cbt::ControllerMini*>(cbcPlus20);
                if (auto b = ctrlB->attackerHandle.get()) {
                    a_actor = b;
                    return ctrlB;
                }
            }
            return nullptr;
        }

        // THE leash decision, shared by the act() and update() seats (see the PURSUIT LEASH
        // section). Runs on a combat behaviour thread: the leash entry is copied out under a
        // shared_lock (released before any engine call), the actors come from handle-table
        // reads, the distances from the references' own data.location and, for a search leaf,
        // from the combat group's search centre read exactly as SearchCenter::Enter reads it
        // (unlocked, CombatBehaviorRE.h). Never a form lookup. Every pass-through reason is
        // counted (RULE C), so a leash that never bites says why.
        bool PursuitDenies(void* a_control, RE::FormID a_actorFid, const APMF_API::APMF_Param& a_claim, Goal a_goal,
                           float& a_dActor, float& a_dGoal, float& a_radius) {
            Leash l{};
            {
                std::shared_lock lk(g_leashMx);
                const auto       it = g_leash.find(a_actorFid);
                if (it != g_leash.end()) l = it->second;
            }
            if (l.anchorFid == 0 || l.anchorFid != a_claim.target) {   // no entry, or mid-publish of a new anchor
                g_pPassStale.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            RE::NiPointer<RE::Actor> self;
            auto*                    ctrl = ResolveController(a_control, self);
            if (!ctrl || !self || self->GetFormID() != a_actorFid) {
                g_pPassStale.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const auto anchor = l.anchor.get();
            if (!anchor || anchor.get() == self.get() || anchor->IsDead()) {
                g_pPassNoAnchor.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // Distances across a load door mean nothing: the anchor must share the actor's
            // worldspace (exterior) or its cell (interior). Otherwise the leash is silent.
            auto*      wsSelf = self->GetWorldspace();
            auto*      wsAnch = anchor->GetWorldspace();
            const bool same   = wsSelf ? (wsSelf == wsAnch)
                                       : (!wsAnch && self->GetParentCell() &&
                                          self->GetParentCell() == anchor->GetParentCell());
            if (!same) {
                g_pPassOtherSpace.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const RE::NiPoint3 pA = anchor->GetPosition();
            const float        dS = self->GetPosition().GetDistance(pA);
            if (!(dS > l.radius)) {
                g_pPassInside.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            RE::NiPoint3 goal{};
            if (a_goal == Goal::kTarget) {
                const auto target = ctrl->targetHandle.get();
                if (!target) {
                    g_pPassNoTarget.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                goal = target->GetPosition();
            } else {
                const auto group = reinterpret_cast<std::uintptr_t>(ctrl->combatGroup);
                if (!group || !*reinterpret_cast<void* const*>(group + apmf::cbt::kGroupSearchLocSpace)) {
                    g_pPassNoTarget.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                goal = *reinterpret_cast<const RE::NiPoint3*>(group + apmf::cbt::kGroupSearchLoc);
            }
            const float dG = goal.GetDistance(pA);
            if (!(dG > dS)) {
                g_pPassCloser.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            a_dActor = dS;
            a_dGoal  = dG;
            a_radius = l.radius;
            return true;
        }

        void LogPursuitDeny(RE::FormID a_id, bool a_atUpdate, std::size_t a_idx, float a_dS, float a_dG, float a_r) {
            const auto now = apmf::clock::MonotonicMs();
            {
                std::scoped_lock lk(g_pLogMx);
                auto&            last = g_pLastLogMs[a_id];
                if (last != 0 && now - last < kPursuitLogGapMs) return;   // counted either way; the line is rate-limited
                last = now;
            }
            const bool search = a_idx < kPursuitN && g_leashLeaves[a_idx].goal == Goal::kSearchCentre;
            spdlog::info("[ch.23] 0x{} LEASH {} {}: the actor is {:.0f} from its anchor (radius {:.0f}) and its "
                         "{} {:.0f} -- the move would take it farther. (Counts per leaf in the [ch.23] pursuit "
                         "heartbeat; this line is limited to one per {} ms per actor.)",
                         apmf::log::Hex(a_id), a_atUpdate ? "ENDED at update()" : "DENIED at act()",
                         a_idx < kPursuitN ? g_leashLeaves[a_idx].name : "?", a_dS, a_r,
                         search ? "search centre" : "target", a_dG, kPursuitLogGapMs);
        }

        // ch.23 act() half: a leash leaf's act(). NOT a ch.7 path -- these leaves carry no ch.7
        // category -- so ch.7's own logic never sees them. Deny = the same ForceFail PAIR.
        void* LeashAct(void* a_this, void* a_control, apmf::cbt::Act_t a_orig, std::size_t a_idx);

        // PFP Phase 0 -- OBSERVE ONLY (see the PFP section above). Called from
        // ActThunk BEFORE the leafCat==0 early-return, since every movement leaf
        // IS category 0 (never classified for deny) and would otherwise never
        // reach any code below that gate. Never denies, never touches `orig`'s
        // return; a pure side-channel read + log. Also declared BEFORE ActThunk
        // for the same ordering reason as ResolveDeliberatingActor above.
        void PfpObserveMovementLeaf(std::uintptr_t vt, void* a_control) {
            const auto mit = g_movementLeaf.find(vt);
            if (mit == g_movementLeaf.end()) return;   // not a movement-shaped leaf -- nothing to report

            g_mvcbtAnchorHits.fetch_add(1, std::memory_order_relaxed);   // RULE C: the anchor is alive

            if (apmf::ControlMap::Get().ControlledCount() == 0)
                return;   // near-zero cost: nobody is claimed on anything right now

            const RE::FormID actorFid = ResolveDeliberatingActor(a_control);
            if (actorFid == 0) return;   // unresolvable -- degrade to silence, never a guess (#17)

            APMF_API::APMF_Param blockParam{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_MovementBlock, blockParam))
                return;   // this actor has no winning ch.1 claim -- out of scope for this question

            g_mvcbtCh1Hits.fetch_add(1, std::memory_order_relaxed);

            const char* leafName = mit->second;
            bool        emit     = false;
            {
                std::scoped_lock lock(g_mvcbtMx);
                auto&            last = g_mvcbtLastLeaf[actorFid];
                if (last != leafName) {   // RULE D: transition dedup, never a repeat, never a timer
                    last = leafName;
                    emit = true;
                }
            }
            if (!emit) return;

            const auto lineIdx = g_mvcbtLineCount.fetch_add(1, std::memory_order_relaxed);
            if (lineIdx < kMvcbtLineCap) {
                spdlog::info("[pfp] mvcbt A 0x{} leaf={} stage=MC2 block=1", apmf::log::Hex(actorFid), leafName);
            } else if (lineIdx == kMvcbtLineCap) {
                spdlog::warn("[pfp] mvcbt line cap ({}) reached -- further transition lines suppressed "
                             "this session (heartbeats keep printing, RULE C). Not a mask: the cap and "
                             "this trip are themselves logged (#7).", kMvcbtLineCap);
            }
        }

        void* ActThunk(void* a_this, void* a_control) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) return a_control;   // foreign vtable -- benign, touch nothing
            auto orig = reinterpret_cast<apmf::cbt::Act_t>(oit->second);

            const auto cit    = g_category.find(vt);
            const auto leafCat = cit != g_category.end() ? cit->second : 0u;

            // PFP Phase 0 (marth 2026-09-06) -- OBSERVE ONLY, must run BEFORE the
            // leafCat==0 early-return below: every movement leaf IS category 0
            // (never classified for deny -- see the file header), so this is the
            // only point in the thunk that ever sees a movement leaf's act() call.
            // Never denies, never touches the return value. See the PFP section
            // above ActThunk for the full design.
            if (g_mvcbtEnabled.load(std::memory_order_relaxed)) PfpObserveMovementLeaf(vt, a_control);

            // ch.23 PURSUIT LEASH (ABI v16): its own seat logic on its own intent. g_pursuitIdx
            // is filled only when the leash armed, and none of its leaves has a ch.7 category.
            if (const auto pit = g_pursuitIdx.find(vt); pit != g_pursuitIdx.end())
                return LeashAct(a_this, a_control, orig, pit->second);

            if (leafCat == 0) return orig(a_this, a_control);   // never a denyable leaf -- skip everything below

            if (apmf::ControlMap::Get().ControlledCount() == 0)
                return orig(a_this, a_control);   // near-zero cost: nothing claimed anywhere

            // Resolve the deliberating actor -- BOTH +0x158 hypotheses, exactly the
            // guard T1Probe field-proved (2026-09-03, 1.6.1170: hypothesis B, the
            // +0x20 hop, is the one that actually resolves on this runtime, but the
            // fallback to hypothesis A is kept -- never narrow to one alone, per the
            // probe's own fixed bug history, Docs/PROBE-ALLOWANCE.md). Shared with
            // the PFP probe path via ResolveDeliberatingActor() above.
            const RE::FormID actorFid = ResolveDeliberatingActor(a_control);
            if (actorFid == 0) return orig(a_this, a_control);   // unresolvable -- degrade to passthrough (#17)

            // ONE deny source (see the file header's RETIRED/SCOPE sections): a real
            // ch.7 kIntent_CombatAction claim's own ival bitmask. kIntent_Cast and the
            // retired ch.8 +ACT bit no longer contribute an implicit Cast deny -- a cast
            // claim is now DELIVERED BY the AI's own magic branch (core/CastSeats.cpp),
            // so denying that branch would silence the very cast being claimed. One
            // lock-free RCU snapshot read.
            std::uint32_t denyMask = 0;
            APMF_API::APMF_Param caClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_CombatAction, caClaim))
                denyMask |= static_cast<std::uint32_t>(caClaim.ival);

            if (denyMask == 0)
                return orig(a_this, a_control);   // no claim on this actor -- nothing to own
            if ((denyMask & leafCat) == 0)
                return orig(a_this, a_control);   // claims don't name this leaf's category -- allow

            const auto denyAct = g_forceFailAct.load(std::memory_order_relaxed);
            if (!denyAct) return orig(a_this, a_control);   // deny mechanism unresolved -- degrade, never crash

            // Arm the paired pop FIRST (the runner will call this node's pop() for
            // this control right after we return -- PopThunk must already know), then
            // invoke ForceFail's own ORIGINAL act() -- "this" is the denied node's own
            // object (safe: ForceFail's act() body needs only `control`, never `this`:
            // push 4 bytes, SetFailed, Ascend -- see CombatBehaviorRE.h). Never a
            // hand-reconstructed SetFailed (T1Probe.cpp's file header, the field crash).
            t_pending = PendingPop{ a_this, a_control };
            reinterpret_cast<apmf::cbt::Act_t>(denyAct)(a_this, a_control);
            return a_control;   // do NOT call orig -- this IS the deny
        }

        void PopThunk(void* a_this, void* a_control) {
            // The paired half of a deny: this pop() belongs to the act() we just
            // ForceFail'd on this thread -> run ForceFail's ORIGINAL pop() (top -= 4)
            // instead of the node's own, so the data stack sees exactly ForceFail's
            // push/pop pair and the node's own state-teardown never runs over state
            // its act() never built.
            if (t_pending.node == a_this && t_pending.control == a_control) {
                t_pending = PendingPop{};
                if (const auto ffPop = g_forceFailPop.load(std::memory_order_relaxed)) {
                    reinterpret_cast<Pop_t>(ffPop)(a_this, a_control);
                    return;
                }
                // Unreachable by construction (a deny never fires unless BOTH halves
                // resolved at install) -- fall through to the node's own pop() rather
                // than skip a pop entirely.
            } else if (t_pending.node) {
                // A different node popped while a deny's pop was pending: impossible per
                // the runner's step protocol. Drop the stale record (a stale substitution
                // on a later legitimate pop would itself unbalance the stack) and count.
                t_pending = PendingPop{};
                g_popAnomalies.fetch_add(1, std::memory_order_relaxed);
                if (!g_popAnomalyLogged.exchange(true))
                    spdlog::warn("[ch.7] paired-pop protocol ANOMALY: a pop() arrived for a node other than the "
                                 "one just denied on this thread -- stale pending dropped. Counted; if this "
                                 "recurs the runner's act->pop pairing assumption needs re-verification "
                                 "(core/CombatBehaviorRE.h 'The node protocol').");
            }

            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_origPop.find(vt);
            if (oit == g_origPop.end()) return;   // foreign vtable -- no original to recover; touch nothing
            reinterpret_cast<Pop_t>(oit->second)(a_this, a_control);
        }

        void* LeashAct(void* a_this, void* a_control, apmf::cbt::Act_t a_orig, std::size_t a_idx) {
            // RULE C anchor (principle 5): every leash-leaf act(), any actor. One relaxed increment.
            g_pSeen[a_idx].fetch_add(1, std::memory_order_relaxed);
            if (g_leashCount.load(std::memory_order_relaxed) == 0 || apmf::ControlMap::Get().ControlledCount() == 0)
                return a_orig(a_this, a_control);
            const RE::FormID actorFid = ResolveDeliberatingActor(a_control);
            if (actorFid == 0) return a_orig(a_this, a_control);   // unresolvable -- passthrough (#17)
            APMF_API::APMF_Param claim{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_PursuitLeash, claim))
                return a_orig(a_this, a_control);                  // not leashed -- the leaf runs natively
            float dS = 0.0f, dG = 0.0f, r = 0.0f;
            if (!PursuitDenies(a_control, actorFid, claim, g_leashLeaves[a_idx].goal, dS, dG, r))
                return a_orig(a_this, a_control);
            const auto denyAct = g_forceFailAct.load(std::memory_order_relaxed);
            if (!denyAct) return a_orig(a_this, a_control);        // unreachable: the leash never arms without it
            g_pDenyAct[a_idx].fetch_add(1, std::memory_order_relaxed);
            LogPursuitDeny(actorFid, false, a_idx, dS, dG, r);
            // The ForceFail PAIR, exactly as ch.7's ActThunk does it (see there).
            t_pending = PendingPop{ a_this, a_control };
            reinterpret_cast<apmf::cbt::Act_t>(denyAct)(a_this, a_control);
            return a_control;
        }

        // ch.23, the update() half (slot 0x04, the 14 leash leaves only). A leash leaf
        // that is ALREADY RUNNING when the leash starts to hold -- it began inside the
        // radius and has since carried the actor out -- is ended through the engine's own
        // leaf-failure exit: SetFailed(thread, 1) + Ascend(thread), exactly what the leaf's
        // own update does when its path fails (core/CombatBehaviorRE.h). The runner then
        // calls that node's OWN pop(), which removes what its own act() pushed: no data-stack
        // imbalance is possible, because nothing here pushes or pops. Before ending it, the
        // runner state is checked to be exactly "this node is cur_node, phase 1 (update)";
        // anything else passes through to the original and is counted (never trusted).
        void UpdateThunk(void* a_this, void* a_control) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_origUpdate.find(vt);
            if (oit == g_origUpdate.end()) return;   // foreign vtable -- no original to recover; touch nothing
            const auto orig = reinterpret_cast<apmf::cbt::Update_t>(oit->second);

            // Cheap exits first: this runs every frame for every actor on one of these leaves.
            if (!g_pursuitArmed.load(std::memory_order_relaxed) || g_leashCount.load(std::memory_order_relaxed) == 0 ||
                apmf::ControlMap::Get().ControlledCount() == 0)
                return orig(a_this, a_control);
            const auto pit = g_pursuitIdx.find(vt);
            if (pit == g_pursuitIdx.end()) return orig(a_this, a_control);
            const std::size_t idx = pit->second;

            RE::NiPointer<RE::Actor> self;
            if (!ResolveController(a_control, self) || !self) return orig(a_this, a_control);
            const RE::FormID     fid = self->GetFormID();
            APMF_API::APMF_Param claim{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(fid, APMF_API::kIntent_PursuitLeash, claim))
                return orig(a_this, a_control);   // this actor is not leashed -- the leaf runs natively

            float dS = 0.0f, dG = 0.0f, r = 0.0f;
            if (!PursuitDenies(a_control, fid, claim, g_leashLeaves[idx].goal, dS, dG, r))
                return orig(a_this, a_control);

            const auto thread = reinterpret_cast<std::uintptr_t>(a_control);
            if (*reinterpret_cast<void* const*>(thread + apmf::cbt::kThreadCurNode) != a_this ||
                *reinterpret_cast<const std::uint32_t*>(thread + apmf::cbt::kThreadPhase) != 1) {
                g_pUpdAnomaly.fetch_add(1, std::memory_order_relaxed);
                if (!g_pUpdAnomalyLogged.exchange(true))
                    spdlog::warn("[ch.23] leash update() ANOMALY: the node is not the thread's cur_node in phase 1 "
                                 "-- the leaf was NOT ended, it ran natively. Counted in the heartbeat; if this "
                                 "recurs the runner protocol in core/CombatBehaviorRE.h needs re-measuring.");
                return orig(a_this, a_control);
            }

            reinterpret_cast<apmf::cbt::SetFailed_t>(g_setFailed.load(std::memory_order_relaxed))(a_control, true);
            reinterpret_cast<apmf::cbt::Ascend_t>(g_ascend.load(std::memory_order_relaxed))(a_control);
            g_pEndUpd[idx].fetch_add(1, std::memory_order_relaxed);
            LogPursuitDeny(fid, true, idx, dS, dG, r);
        }

        // A vtable is deniable only when BOTH halves of the pair were installed on
        // it (act at 0x02 AND pop at 0x03) -- a half-hooked node must never be
        // classified, or a deny would run ForceFail's act() against the node's own pop().
        bool Paired(std::uintptr_t vt) { return g_orig.contains(vt) && g_origPop.contains(vt); }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ch.7] VR runtime -- the 70 leaf vtable indices are SE/AE-only verified; "
                         "combat-action allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        std::array<REL::VariantID, 70> vtables{};
        for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) vtables[i] = apmf::cbt::kLeaves[i].vtbl;

        REL::Relocation<void*> expectedTD{ apmf::cbt::RTTI_CombatBehaviorTreeNode };
        const int n    = allowance::InstallOnVtables(vtables, 0x02, &ActThunk, expectedTD.get(), "ch.7", g_orig);
        const int nPop = allowance::InstallOnVtables(vtables, 0x03, &PopThunk, expectedTD.get(), "ch.7-pop", g_origPop);

        int classified = 0;
        for (const char* wanted : kOffenseLeafNames) {
            bool found = false;
            for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
                if (std::string_view(apmf::cbt::kLeaves[i].name) != wanted) continue;
                found = true;
                REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[i].vtbl };
                if (Paired(vt.address())) {
                    g_category[vt.address()] |= APMF_API::kCombatActionCat_Offense;
                    ++classified;
                }
                break;
            }
            if (!found)
                spdlog::info("[ch.7] '{}' has no leaf on this build's 70-leaf catalog -- not classified "
                             "(see ActionGate.cpp's file header).", wanted);
        }

        // ch.8b: OR the Cast bit onto the four cast leaves (already Offense-classified
        // above). A kIntent_Cast claim then denies exactly these, no attack/ranged.
        int castClassified = 0;
        for (const char* wanted : kCastLeafNames) {
            for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
                if (std::string_view(apmf::cbt::kLeaves[i].name) != wanted) continue;
                REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[i].vtbl };
                if (Paired(vt.address())) {
                    g_category[vt.address()] |= APMF_API::kCombatActionCat_Cast;
                    ++castClassified;
                }
                break;
            }
        }
        spdlog::info("[ch.7] {} cast leaf(s) also classified 'cast' -- a kIntent_CombatAction claim naming "
                     "kCombatActionCat_Cast denies exactly these (CastImmediateSpell/CastConcentrationSpell/"
                     "PrepareDualCast/CastShout), leaving attack/ranged/movement leaves firing. A kIntent_Cast "
                     "(ch.8b) claim NO LONGER denies them -- it is now delivered BY the AI's own cast branch "
                     "through the engine seats (core/CastSeats.cpp).", castClassified);

        // NOT INSTALLED (feat/ai-cast-seats-impl): the magic CONTEXT-CREATION node
        // (apmf::cbt::kCastContextNodes). See this file's RETIRED header block --
        // denying the AI's magic context build would now suppress the very cast a
        // kIntent_Cast claim asks for, and that node's deny is the seat whose
        // act()-only form caused a months-live data-stack CTD. The RE record for the
        // node stays in core/CombatBehaviorRE.h; it is simply never hooked.

        // Resolve ForceFail's ORIGINAL act() AND pop() -- the deny mechanism is the
        // PAIR. Either half missing => refuse the deny entirely (an unpaired ForceFail
        // is the data-stack corruption this pass fixed; never ship half of it).
        const int ffIdx = apmf::cbt::ForceFailIndex();
        if (ffIdx >= 0) {
            REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[static_cast<std::size_t>(ffIdx)].vtbl };
            if (auto oit = g_orig.find(vt.address()); oit != g_orig.end())
                g_forceFailAct.store(oit->second, std::memory_order_relaxed);
            if (auto pit = g_origPop.find(vt.address()); pit != g_origPop.end())
                g_forceFailPop.store(pit->second, std::memory_order_relaxed);
        }
        if (!g_forceFailAct.load(std::memory_order_relaxed) || !g_forceFailPop.load(std::memory_order_relaxed)) {
            g_forceFailAct.store(0, std::memory_order_relaxed);
            g_forceFailPop.store(0, std::memory_order_relaxed);
            spdlog::warn("[ch.7] ForceFail::act()/pop() PAIR not fully resolved -- combat-action DENY unavailable "
                         "this session (leaves still fire natively; claims are arbitration-only until this "
                         "resolves on a future load). A half-resolved pair is refused on purpose.");
        }

        spdlog::info("[ch.7] combat-action allowance hooked on {} of 70 leaf vtables as an act/pop PAIR "
                     "(act {} / pop {}); {} leaf(s) classified 'offense'. A kIntent_CombatAction claim with "
                     "kCombatActionCat_Offense set in APMF_Param::ival denies exactly those leaves for its winning "
                     "actor; every other leaf is never looked up and never denied.", n, n, nPop, classified);

        // ================================================================
        // ch.23 PURSUIT LEASH (ABI v16). Arms ONLY when every half is there: the INI switch,
        // the ForceFail pair (the act() half), the verified SetFailed + Ascend rows (the
        // update() half's engine calls), and act + pop + update installed on ALL 14 leash
        // vtables (the ten pursuit leaves already carry ch.7's act/pop; the four search leaves
        // get theirs here, same thunks, same maps). Anything less is refused whole and logged:
        // a leash that denies a leaf's start but lets a running chase continue is exactly the
        // partial deny #18 forbids.
        // ================================================================
        {
            const char* why = nullptr;
            if (GetPrivateProfileIntA("PursuitLeash", "bPursuitLeash", 1, "Data/SKSE/Plugins/APMF.ini") == 0) {
                why = "[PursuitLeash] bPursuitLeash=0 in APMF.ini";
            } else if (!g_forceFailAct.load(std::memory_order_relaxed) ||
                       !g_forceFailPop.load(std::memory_order_relaxed)) {
                why = "the ForceFail act()/pop() pair did not resolve (the act() half of the leash)";
            } else {
                REL::Relocation<apmf::cbt::SetFailed_t> setFailed{ RELOCATION_ID(46240, 47496) };
                REL::Relocation<apmf::cbt::Ascend_t>    ascend{ RELOCATION_ID(46229, 47484) };
                const bool sfOk = allowance::SeatVerified(setFailed.address(), "ActionGate.Pursuit.SetFailed");
                const bool asOk = allowance::SeatVerified(ascend.address(), "ActionGate.Pursuit.Ascend");
                if (!sfOk || !asOk) {
                    why = "the address self-check refused SetFailed / Ascend (the update() half of the leash)";
                } else {
                    bool allNamed = true;
                    for (std::size_t p = 0; p < kPursuitLeafNames.size(); ++p) {
                        bool found = false;
                        for (const auto& leaf : apmf::cbt::kLeaves) {
                            if (std::string_view(leaf.name) != kPursuitLeafNames[p]) continue;
                            g_leashLeaves[p] = LeashLeaf{ leaf.name, leaf.vtbl, Goal::kTarget };
                            found            = true;
                            break;
                        }
                        if (!found) {
                            allNamed = false;
                            spdlog::warn("[ch.23] pursuit leaf '{}' has no entry in the 70-leaf catalog.",
                                         kPursuitLeafNames[p]);
                        }
                    }
                    for (std::size_t k = 0; k < apmf::cbt::kSearchLeaves.size(); ++k) {
                        const auto& leaf = apmf::cbt::kSearchLeaves[k];
                        g_leashLeaves[kPursuitLeafNames.size() + k] = LeashLeaf{ leaf.name, leaf.vtbl, Goal::kSearchCentre };
                    }
                    if (!allNamed) {
                        why = "a pursuit leaf is missing from the leaf catalog";
                    } else {
                        std::array<REL::VariantID, kPursuitN>                  pvt{};
                        std::array<REL::VariantID, apmf::cbt::kSearchLeaves.size()> svt{};
                        for (std::size_t p = 0; p < kPursuitN; ++p) pvt[p] = g_leashLeaves[p].vtbl;
                        for (std::size_t k = 0; k < svt.size(); ++k) svt[k] = apmf::cbt::kSearchLeaves[k].vtbl;
                        const int nSAct = allowance::InstallOnVtables(svt, 0x02, &ActThunk, expectedTD.get(),
                                                                      "ch.23-search", g_orig);
                        const int nSPop = allowance::InstallOnVtables(svt, 0x03, &PopThunk, expectedTD.get(),
                                                                      "ch.23-search-pop", g_origPop);
                        const int nUpd  = allowance::InstallOnVtables(pvt, 0x04, &UpdateThunk, expectedTD.get(),
                                                                      "ch.23-update", g_origUpdate);
                        std::size_t armed = 0;
                        for (std::size_t p = 0; p < kPursuitN; ++p) {
                            REL::Relocation<std::uintptr_t> vt{ pvt[p] };
                            if (Paired(vt.address()) && g_origUpdate.contains(vt.address()) &&
                                !g_category.contains(vt.address()))   // never a ch.7-classified leaf
                                ++armed;
                        }
                        if (armed != kPursuitN) {
                            why = "act + pop + update did not all install on every leash leaf";
                            spdlog::warn("[ch.23] leash: {} of {} leaves have act+pop+update (search act {} / pop {}, "
                                         "update {}) -- refusing the WHOLE leash.", armed, kPursuitN, nSAct, nSPop, nUpd);
                        } else {
                            for (std::size_t p = 0; p < kPursuitN; ++p) {
                                REL::Relocation<std::uintptr_t> vt{ pvt[p] };
                                g_pursuitIdx[vt.address()] = p;
                            }
                            g_setFailed.store(setFailed.address(), std::memory_order_relaxed);
                            g_ascend.store(ascend.address(), std::memory_order_relaxed);
                        }
                    }
                }
            }
            if (why) {
                g_pursuitNotArmedWhy.store(why, std::memory_order_release);
                spdlog::warn("[ch.23] PURSUIT LEASH NOT armed: {}. Every kIntent_PursuitLeash request is refused at "
                             "the call; ch.7 is unaffected.", why);
            } else {
                g_pursuitNotArmedWhy.store(nullptr, std::memory_order_release);
                g_pursuitArmed.store(true, std::memory_order_release);
                spdlog::info("[ch.23] PURSUIT LEASH armed on {} leaves (act+pop deny pair, update() ended through "
                             "SetFailed+Ascend). Goal = the combat target: Advance, Chase, FindAttackLocation, Flank, "
                             "FlankDistant, MaintainOptimalRange, PursueTarget, Reposition, Stalk, Surround. Goal = the "
                             "group's search centre: Search, SearchCenter, SearchLocation, SearchWander. A "
                             "kIntent_PursuitLeash claim (anchor = param.target, radius = param.fval) denies them only "
                             "while the move would take the actor farther from the anchor.",
                             kPursuitN);
            }
        }

        // ================================================================
        // PFP PHASE 0 -- movement-leaf classification (OBSERVE ONLY; see the
        // PFP section above kMovementLeafConfirmed/kMovementLeafHypothesis for
        // the design). Deliberately does NOT check Paired(vt) -- unlike the
        // offense/cast classification above, this table is never used to deny
        // anything, so it does not need the act()/pop() pair to be intact, only
        // for act() (already installed on all 70 leaves unconditionally) to run.
        // ================================================================
        int mvConfirmed = 0, mvHypothesis = 0;
        auto classifyMovement = [](const char* wanted, int& counter) {
            for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
                if (std::string_view(apmf::cbt::kLeaves[i].name) != wanted) continue;
                REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[i].vtbl };
                g_movementLeaf[vt.address()] = apmf::cbt::kLeaves[i].name;
                ++counter;
                return;
            }
            spdlog::info("[pfp] mvcbt '{}' has no leaf on this build's 70-leaf catalog -- not classified "
                         "(see ActionGate.cpp's PFP section).", wanted);
        };
        for (const char* wanted : kMovementLeafConfirmed)  classifyMovement(wanted, mvConfirmed);
        for (const char* wanted : kMovementLeafHypothesis) classifyMovement(wanted, mvHypothesis);

        g_mvcbtEnabled.store(GetPrivateProfileIntA("Probe.mvcbt", "Enable", 0,
                                                    "Data/SKSE/Plugins/APMF.ini") != 0,
                             std::memory_order_relaxed);
        spdlog::info("[pfp] mvcbt {} -- {} movement leaf(s) classified for OBSERVE-ONLY reporting "
                     "({} CPR-corroborated + {} hypothesis-by-naming, out of {} total leaves). Never denies; "
                     "reports leaf-fire transitions for actors holding a winning ch.1 (kIntent_MovementBlock) "
                     "claim, so a deck cycle can answer whether the full block holds against combat pathing. "
                     "[Probe.mvcbt] Enable=0 in Data/SKSE/Plugins/APMF.ini is the default (OFF).",
                     g_mvcbtEnabled.load(std::memory_order_relaxed) ? "ARMED" : "installed, disabled",
                     mvConfirmed + mvHypothesis, mvConfirmed, mvHypothesis, apmf::cbt::kLeaves.size());
    }

    void PfpHeartbeat() {
        // RULE C -- prints ONCE per interval, INCLUDING ZERO counts, so silence
        // is never mistaken for "the anchor never fires." No CombatController
        // "episode" boundary is wired at this seat, so this uses a fixed ~30s
        // main-thread cadence as a documented approximation instead. Cheap when
        // disabled: one relaxed atomic-bool load, nothing else.
        if (!g_mvcbtEnabled.load(std::memory_order_relaxed)) return;

        const auto now  = apmf::clock::MonotonicMs();
        const auto last = g_mvcbtLastHeartbeatMs.load(std::memory_order_relaxed);
        if (now - last < kMvcbtHeartbeatMs) return;
        g_mvcbtLastHeartbeatMs.store(now, std::memory_order_relaxed);   // main-thread-only writer, no race

        const auto lineCount = g_mvcbtLineCount.load(std::memory_order_relaxed);
        const auto dropped   = lineCount > kMvcbtLineCap ? lineCount - kMvcbtLineCap : 0;
        spdlog::info("[pfp] mvcbt H stage=MC2-anchor hits={} drops={}",
                     g_mvcbtAnchorHits.load(std::memory_order_relaxed), dropped);
        spdlog::info("[pfp] mvcbt H stage=MC2-ch1 hits={}", g_mvcbtCh1Hits.load(std::memory_order_relaxed));
    }

    // ---- ch.23 PURSUIT LEASH (ABI v16): the leash table, written on the game thread only ----

    bool PursuitArmed() { return g_pursuitArmed.load(std::memory_order_acquire); }

    const char* PursuitNotArmedReason() {
        const char* r = g_pursuitNotArmedWhy.load(std::memory_order_acquire);
        return r ? r : "armed";
    }

    void SetLeash(RE::FormID a_id, const APMF_API::APMF_Param& a_param) {
        const char*     why = nullptr;
        RE::ActorHandle h{};
        if (!g_pursuitArmed.load(std::memory_order_acquire)) {
            why = PursuitNotArmedReason();
        } else if (a_param.target == 0) {
            why = "param.target is 0 (the pursuit leash needs the ANCHOR actor's FormID)";
        } else if (a_param.target == a_id) {
            why = "the anchor is the actor itself";
        } else if (!std::isfinite(a_param.fval) || !(a_param.fval > 0.0f)) {
            why = "param.fval (the radius) is not a positive number";
        } else if (auto* form = RE::TESForm::LookupByID(a_param.target); !form) {
            why = "no form has the anchor's FormID";
        } else if (auto* anchor = form->As<RE::Actor>(); !anchor) {
            why = "the anchor form is not an Actor";
        } else {
            h = anchor->GetHandle();
            if (!h) why = "the anchor actor has no reference handle (not loaded?)";
        }
        if (why) {
            ClearLeash(a_id);
            spdlog::warn("[ch.23] 0x{} pursuit leash NOT set: {}. The claim stands and denies nothing. Repoint "
                         "with a valid anchor + radius to arm it.",
                         apmf::log::Hex(a_id), why);
            return;
        }
        {
            std::unique_lock lk(g_leashMx);
            g_leash[a_id] = Leash{ a_param.target, h, a_param.fval };
            g_leashCount.store(g_leash.size(), std::memory_order_relaxed);
        }
        spdlog::info("[ch.23] 0x{} pursuit LEASH set: anchor 0x{}, radius {:.0f}. While the actor is farther than "
                     "the radius from the anchor, a leash leaf whose goal is farther still is denied at act() "
                     "and ended at update(); every other leaf and every other move runs natively.",
                     apmf::log::Hex(a_id), apmf::log::Hex(a_param.target), a_param.fval);
    }

    void ClearLeash(RE::FormID a_id) {
        bool erased = false;
        {
            std::unique_lock lk(g_leashMx);
            erased = g_leash.erase(a_id) != 0;
            g_leashCount.store(g_leash.size(), std::memory_order_relaxed);
        }
        {
            std::scoped_lock lk(g_pLogMx);
            g_pLastLogMs.erase(a_id);
        }
        if (erased) spdlog::info("[ch.23] 0x{} pursuit leash cleared.", apmf::log::Hex(a_id));
    }

    void ResetLeash(const char* a_why) {
        std::size_t n = 0;
        {
            std::unique_lock lk(g_leashMx);
            n = g_leash.size();
            g_leash.clear();
            g_leashCount.store(0, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lk(g_pLogMx);
            g_pLastLogMs.clear();
        }
        if (n != 0) spdlog::info("[ch.23] {} -- dropped {} pursuit leash entr{}.", a_why, n, n == 1 ? "y" : "ies");
    }

    void PursuitHeartbeat() {
        // RULE C: while any leash is set, print every counter every ~30 s, zeros included, so
        // a leash that never bites is visible as such (and says which pass reason held it).
        if (!g_pursuitArmed.load(std::memory_order_relaxed) || g_leashCount.load(std::memory_order_relaxed) == 0)
            return;
        const auto now  = apmf::clock::MonotonicMs();
        const auto last = g_pLastHeartbeatMs.load(std::memory_order_relaxed);
        if (now - last < kPursuitHeartbeatMs) return;
        g_pLastHeartbeatMs.store(now, std::memory_order_relaxed);   // game-thread-only writer

        std::string perLeaf;
        for (std::size_t p = 0; p < kPursuitN; ++p) {
            std::string_view name = g_leashLeaves[p].name ? g_leashLeaves[p].name : "?";
            if (name.starts_with("CombatBehavior")) name.remove_prefix(14);
            perLeaf += fmt::format(" {}={}/{}/{}", name, g_pSeen[p].load(std::memory_order_relaxed),
                                   g_pDenyAct[p].load(std::memory_order_relaxed),
                                   g_pEndUpd[p].load(std::memory_order_relaxed));
        }
        spdlog::info("[ch.23] pursuit H leashes={} leaf=seen/denied-at-act/ended-at-update:{}",
                     g_leashCount.load(std::memory_order_relaxed), perLeaf);
        spdlog::info("[ch.23] pursuit H pass inside={} closer={} no-anchor={} other-space={} no-goal={} stale={} "
                     "update-anomaly={}",
                     g_pPassInside.load(std::memory_order_relaxed), g_pPassCloser.load(std::memory_order_relaxed),
                     g_pPassNoAnchor.load(std::memory_order_relaxed), g_pPassOtherSpace.load(std::memory_order_relaxed),
                     g_pPassNoTarget.load(std::memory_order_relaxed), g_pPassStale.load(std::memory_order_relaxed),
                     g_pUpdAnomaly.load(std::memory_order_relaxed));
    }

}
