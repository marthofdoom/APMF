#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/CombatBehaviorRE.h"
#include "core/ControlMap.h"
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

        // vtable runtime address -> original act() (always the passthrough target).
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        // vtable runtime address -> classified category bitmask (0 = never denied,
        // and therefore never even looked up against a claim -- see header).
        std::unordered_map<std::uintptr_t, std::uint32_t>  g_category;

        // ForceFail's ORIGINAL act() -- the proven deny mechanism (see header).
        std::atomic<std::uintptr_t> g_forceFailAct{ 0 };

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

            // Build the deny mask from TWO independent claim sources, OR'd:
            //   * a real kIntent_CombatAction claim contributes its own ival bitmask;
            //   * a kIntent_Cast claim (ch.8b) is treated as an IMPLICIT combat-action
            //     claim with ival = kCombatActionCat_Cast -- it denies ONLY the four
            //     cast leaves (which carry the Cast bit), leaving attack/ranged/block/
            //     dodge/movement leaves firing so the follower keeps fighting while the
            //     client's cast plays. Either source may be absent.
            std::uint32_t denyMask = 0;
            APMF_API::APMF_Param caClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_CombatAction, caClaim))
                denyMask |= static_cast<std::uint32_t>(caClaim.ival);
            APMF_API::APMF_Param castClaim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(actorFid, APMF_API::kIntent_Cast, castClaim))
                denyMask |= APMF_API::kCombatActionCat_Cast;

            if (denyMask == 0)
                return orig(a_this, a_control);   // neither claim on this actor -- nothing to own
            if ((denyMask & leafCat) == 0)
                return orig(a_this, a_control);   // claims don't name this leaf's category -- allow

            const auto denyAct = g_forceFailAct.load(std::memory_order_relaxed);
            if (!denyAct) return orig(a_this, a_control);   // deny mechanism unresolved -- degrade, never crash

            // Invoke ForceFail's own ORIGINAL act() -- "this" is the denied leaf's
            // own object (safe: ForceFail's body needs only `control`, never `this`;
            // see T1Probe.cpp's file header for the full field-crash rationale for
            // why this is called rather than a hand-reconstructed SetFailed).
            reinterpret_cast<apmf::cbt::Act_t>(denyAct)(a_this, a_control);
            return a_control;   // do NOT call orig -- this IS the deny
        }

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
        const int n = allowance::InstallOnVtables(vtables, 0x02, &ActThunk, expectedTD.get(), "ch.7", g_orig);

        int classified = 0;
        for (const char* wanted : kOffenseLeafNames) {
            bool found = false;
            for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
                if (std::string_view(apmf::cbt::kLeaves[i].name) != wanted) continue;
                found = true;
                REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[i].vtbl };
                if (g_orig.contains(vt.address())) {
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
                if (g_orig.contains(vt.address())) {
                    g_category[vt.address()] |= APMF_API::kCombatActionCat_Cast;
                    ++castClassified;
                }
                break;
            }
        }
        spdlog::info("[ch.8b] {} cast leaf(s) also classified 'cast' -- a kIntent_Cast claim denies "
                     "exactly these (CastImmediateSpell/CastConcentrationSpell/PrepareDualCast/CastShout), "
                     "leaving attack/ranged/movement leaves firing.", castClassified);

        const int ffIdx = apmf::cbt::ForceFailIndex();
        if (ffIdx >= 0) {
            REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[static_cast<std::size_t>(ffIdx)].vtbl };
            if (auto oit = g_orig.find(vt.address()); oit != g_orig.end())
                g_forceFailAct.store(oit->second, std::memory_order_relaxed);
        }
        if (!g_forceFailAct.load(std::memory_order_relaxed))
            spdlog::warn("[ch.7] ForceFail::act() original not resolved -- combat-action DENY unavailable "
                         "this session (leaves still fire natively; claims are arbitration-only until this "
                         "resolves on a future load).");

        spdlog::info("[ch.7] combat-action allowance hooked on {} of 70 leaf vtables (slot 0x02, act/Enter); "
                     "{} leaf(s) classified 'offense'. A kIntent_CombatAction claim with kCombatActionCat_Offense "
                     "set in APMF_Param::ival denies exactly those leaves for its winning actor; every other leaf "
                     "is never looked up and never denied.", n, classified);
    }

}
