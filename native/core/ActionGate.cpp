#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/CombatBehaviorRE.h"
#include "core/ControlMap.h"
#include "core/CastExecutor.h"   // kActFlag_Drive -- the ch.8 +ACT opt-in bit (the "under cast control" scope signal)
#include "core/ActionGate.h"

#include <array>

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
// DENY-COMPLETENESS (INVARIANTS #18, 2026-09-04): in ADDITION to the 70 leaves,
// this gate also installs on the AI's magic cast/equip CONTEXT-CREATION nodes
// (apmf::cbt::kCastContextNodes -- the CombatBehaviorContextMagic
// CreateContextNode Base + Node1). That node's act() BUILDS the magic context
// (CombatBehaviorContextMagic over the EquipContext's CombatInventoryItem)
// UPSTREAM of the cast-firing leaves and then descends into the magic
// subtree. Denying it (with the paired pop above) means the AI never builds a
// magic context, never equips a spell on its own, and never reaches a cast
// leaf while the facet is held -- the general, spell-agnostic "the combat AI
// does not cast" gate. Classified Cast|Offense.
//
// SCOPE ("under cast control" -- when the cast deny arms). The Cast category is
// denied for an actor when ANY of these claims is winning on it, all read from
// the same lock-free RCU snapshot (arm = the claim's Drain publish, disarm =
// its release/TTL publish; no separate flag to race):
//   * a ch.8b `kIntent_Cast` claim (the client's own executed cast window);
//   * a ch.7 `kIntent_CombatAction` claim whose mask names Cast (or Offense);
//   * NEW (feat/ai-cast-suppress): a ch.8 `kIntent_SelectSpell` claim with the
//     +ACT opt-in (`ival & castexec::kActFlag_Drive`) -- APMF itself drives the
//     cast (core/CastExecutor.cpp), so for the WHOLE claim window (not just the
//     ~150 ms internal ch.8b pulse) the AI's own magic branch must be silent:
//     no context build, no self-equip fighting the drive's EquipSpell, no
//     autonomous fire of the claimed spell alongside the driven one.
//   A BARE ch.8 claim (gate-only mode, MFO's offense gambit) deliberately does
//   NOT arm this: there the client WANTS its AI to build the magic context and
//   cast the claimed spell itself (Docs/DENY-COMPLETENESS-AUDIT.md row 4).
// The forced drive is untouched by this gate: CastExecutor equips/animates/
// fires through ActorEquipManager::EquipSpell, NotifyAnimationGraph and the
// MagicCaster directly -- none of which route through the behavior tree.
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

        // The FOUR cast leaves get kCombatActionCat_Cast IN ADDITION to Offense
        // (design.md §3.5): a kIntent_Cast claim denies exactly these, while a
        // kIntent_CombatAction(Offense) claim still denies them too (they carry both
        // bits). RangedAttack et al. are Offense-only, so a cast claim leaves them
        // firing. All four names land 1:1 on this build's 70-leaf catalog.
        constexpr std::array<const char*, 4> kCastLeafNames{ {
            "CombatBehaviorCastImmediateSpell",
            "CombatBehaviorCastConcentrationSpell",
            "CombatBehaviorPrepareDualCast",
            "CombatBehaviorCastShout",
        } };

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

        void* ActThunk(void* a_this, void* a_control) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) return a_control;   // foreign vtable -- benign, touch nothing
            auto orig = reinterpret_cast<apmf::cbt::Act_t>(oit->second);

            const auto cit    = g_category.find(vt);
            const auto leafCat = cit != g_category.end() ? cit->second : 0u;
            if (leafCat == 0) return orig(a_this, a_control);   // never a denyable leaf -- skip everything below

            if (apmf::ControlMap::Get().ControlledCount() == 0)
                return orig(a_this, a_control);   // near-zero cost: nothing claimed anywhere

            // Resolve the deliberating actor -- BOTH +0x158 hypotheses, exactly the
            // guard T1Probe field-proved (2026-09-03, 1.6.1170: hypothesis B, the
            // +0x20 hop, is the one that actually resolves on this runtime, but the
            // fallback to hypothesis A is kept -- never narrow to one alone, per the
            // probe's own fixed bug history, Docs/PROBE-ALLOWANCE.md).
            RE::FormID actorFid = 0;
            if (a_control) {
                auto* tc      = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);
                void* p0x158 = tc->master_controller;
                if (p0x158) {
                    auto* ctrlA = reinterpret_cast<apmf::cbt::ControllerMini*>(p0x158);
                    if (auto a = ctrlA->attackerHandle.get()) actorFid = a->GetFormID();
                    if (actorFid == 0) {
                        void* cbcPlus20 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p0x158) + 0x20);
                        if (cbcPlus20) {
                            auto* ctrlB = reinterpret_cast<apmf::cbt::ControllerMini*>(cbcPlus20);
                            if (auto b = ctrlB->attackerHandle.get()) actorFid = b->GetFormID();
                        }
                    }
                }
            }
            if (actorFid == 0) return orig(a_this, a_control);   // unresolvable -- degrade to passthrough (#17)

            // Build the deny mask from THREE independent claim sources, OR'd (see the
            // file header's SCOPE section):
            //   * a real kIntent_CombatAction claim contributes its own ival bitmask;
            //   * a kIntent_Cast claim (ch.8b) is treated as an IMPLICIT combat-action
            //     claim with ival = kCombatActionCat_Cast -- it denies ONLY the cast
            //     leaves + the magic context node (which carry the Cast bit), leaving
            //     attack/ranged/block/dodge/movement leaves firing so the follower keeps
            //     fighting while the client's cast plays;
            //   * a kIntent_SelectSpell claim with the +ACT opt-in (APMF drives the
            //     cast itself) contributes kCombatActionCat_Cast for the WHOLE claim
            //     window -- the AI's autonomous magic branch is silent while APMF owns
            //     the cast. A bare (gate-only) SelectSpell claim contributes nothing.
            // Any source may be absent. All three are lock-free RCU snapshot reads.
            std::uint32_t denyMask = 0;
            APMF_API::APMF_Param caClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_CombatAction, caClaim))
                denyMask |= static_cast<std::uint32_t>(caClaim.ival);
            APMF_API::APMF_Param castClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_Cast, castClaim))
                denyMask |= APMF_API::kCombatActionCat_Cast;
            APMF_API::APMF_Param selClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_SelectSpell, selClaim) &&
                (selClaim.ival & apmf::castexec::kActFlag_Drive) != 0)
                denyMask |= APMF_API::kCombatActionCat_Cast;

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
        spdlog::info("[ch.8b] {} cast leaf(s) also classified 'cast' -- a kIntent_Cast claim (or a ch.8 +ACT "
                     "claim) denies exactly these (CastImmediateSpell/CastConcentrationSpell/PrepareDualCast/"
                     "CastShout), leaving attack/ranged/movement leaves firing.", castClassified);

        // ── deny-completeness (INVARIANTS #18): the AI's magic cast/equip
        // CONTEXT-CREATION node. The cast LEAVES above deny the cast FIRING, but
        // NOT the upstream node that BUILDS the magic context and descends into
        // the magic subtree. Install the SAME paired act/pop thunks on those
        // nodes and classify them Cast|Offense, so a kIntent_Cast, a ch.8 +ACT,
        // or an Offense claim denies the AI EVER building its magic context (and
        // therefore ever equipping/charging/firing a spell of its own) while the
        // facet is held. RTTI-verified per node at install (#17); a node that
        // does not derive CombatBehaviorTreeNode is skipped, never hooked blind.
        // Granular: only the ContextMagic node is touched, so melee/ranged/
        // movement context nodes keep firing.
        std::array<REL::VariantID, apmf::cbt::kCastContextNodes.size()> ctxVts{};
        for (std::size_t i = 0; i < apmf::cbt::kCastContextNodes.size(); ++i)
            ctxVts[i] = apmf::cbt::kCastContextNodes[i].vtbl;
        const int nCtx    = allowance::InstallOnVtables(ctxVts, 0x02, &ActThunk, expectedTD.get(),
                                                        "ch.8b-ctx", g_orig);
        const int nCtxPop = allowance::InstallOnVtables(ctxVts, 0x03, &PopThunk, expectedTD.get(),
                                                        "ch.8b-ctx-pop", g_origPop);
        int ctxClassified = 0;
        for (const auto& node : apmf::cbt::kCastContextNodes) {
            REL::Relocation<std::uintptr_t> vt{ node.vtbl };
            if (Paired(vt.address())) {
                g_category[vt.address()] |=
                    (APMF_API::kCombatActionCat_Cast | APMF_API::kCombatActionCat_Offense);
                ++ctxClassified;
            }
        }
        spdlog::info("[ch.8b] {} of {} magic CONTEXT-CREATION node(s) hooked as an act/pop PAIR (slots 0x02+0x03) "
                     "+ classified Cast|Offense -- a kIntent_Cast / ch.8 +ACT / Offense claim denies the AI "
                     "BUILDING its magic context at all (no self-equip, no charge, no fire while APMF owns the "
                     "cast). act hooked: {}, pop hooked: {}.",
                     ctxClassified, apmf::cbt::kCastContextNodes.size(), nCtx, nCtxPop);

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
    }

}
