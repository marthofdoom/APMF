#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/CombatBehaviorRE.h"
#include "core/ControlMap.h"
#include "core/ActionGate.h"

#include <array>
#include <mutex>

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

}
