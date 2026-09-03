#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/EquipGate.h"

// ============================================================================
// T2a -- CheckShouldEquip, the combat AI's per-item "should I put this in my
// hands?" permission bit. Docs/ALLOWANCE-TEMPLATE.md §3/§7.
//
// Hooks CombatInventoryItem::CheckShouldEquip (vtable slot 0x0F) on the 30
// concrete spell/staff CombatInventoryItemMagicT<item,caster> instantiations --
// the SAME set MFO's own CombatStyle.cpp equip gate patches, for the SAME
// reason: CombatInventoryItemMagic/...Staff are themselves ABSTRACT (pure
// CreateCaster; no live object ever dispatches through their own vtable), so
// only the concrete per-caster-category instantiations carry a real vtable to
// hook -- the same 14-way caster-category split CastGate/CasterConsent rides,
// times {Magic, Staff}, plus Armor (mage-armor spells are exactly what a
// caster re-arms; this concrete instantiation genuinely derives
// CombatInventoryItemMagic in the pinned headers, unlike the BARE
// VTABLE_CombatMagicCasterArmor symbol the §0.28 lesson excludes elsewhere).
//
// DELIBERATELY NOT the design doc's aspirational 87/"Melee-Ranged-Shield-Torch"
// count (Docs/ALLOWANCE-TEMPLATE.md §3/§4 table). Verified 2026-09-02 against
// the pinned upstream (CharmedBaryon/CommonLibSSE-NG): the CombatInventoryItem
// TYPE enum lists kMelee/kRanged/kShield/kTorch, but NO CONCRETE C++ CLASS for
// any of them exists in the headers -- there is no `CombatInventoryItemMelee.h`
// etc., so no `RE::VTABLE_CombatInventoryItemMelee` symbol exists to hook or
// RTTI-verify. This is the EXACT same finding MFO's own CombatStyle.cpp already
// records ("weapon items (Melee/Ranged) are absent too: their classes are NOT
// in the pinned headers"). Potion/Scroll/Shout DO have concrete header classes,
// but are deliberately excluded on GAMEPLAY grounds MFO's CombatStyle.cpp
// already proved out: a claimed SPELL's FormID never equals a potion's, so
// hooking potion/scroll/shout here would only ever be a false-positive block on
// combat drinking/shouting (the v1.0.32 lesson) -- never a true exclusivity
// check for a spell-select claim. Widen only on new header evidence + a
// deliberate new intent for weapon/shout selection (T1/future work).
//
// This replicates MFO's proven gate (CombatStyle.cpp's own CheckShouldEquip
// hook), but APMF-side and MUTEX-FREE: Allowed() is the lock-free RCU
// ControlMap read, never a mutex (contrast MFO CombatStyle.cpp:276, which
// takes one) -- exactly the threading discipline §5 requires of every T2
// thunk (all run on combat threads).
// ============================================================================

namespace apmf::equipgate {

    namespace {

        // Same layout guard MFO's CombatStyle.cpp/CasterConsent.cpp already
        // carry -- the combat thread may only read a CombatController member
        // BELOW 0x68 (the AE +8 layout bug, ENGINE_NOTES §0.29).
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the "
                      "SE/AE layout split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point "
                      "(0x68) -- its compiled offset is WRONG on AE runtimes");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10,
                      "CombatInventoryItem::item moved -- re-verify against the "
                      "pinned header before shipping");

        using CheckShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);

        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        std::atomic<bool> g_installed{ false };

        bool EquipGateThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            // Foreign object -> "don't equip" is the benign default here,
            // exactly as MFO's CombatStyle.cpp gate treats it (the base impl
            // itself returns false for a fleeing combatant).
            if (oit == g_orig.end()) return false;
            const auto original = reinterpret_cast<CheckShouldEquip_t>(oit->second);

            const bool engineSays = original(a_this, a_cc);
            if (!engineSays) return false;   // the AI already declined -- nothing to own
            if (!a_cc) return engineSays;

            auto attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>
            auto* actor = attPtr.get();
            if (!actor) return engineSays;
            const auto fid = actor->GetFormID();

            auto* item = a_this->item;
            const auto subjectForm = item ? item->GetFormID() : 0;

            if (allowance::Allowed(fid, APMF_API::kIntent_SelectSpell, subjectForm))
                return engineSays;
            return false;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t2a] VR runtime -- CombatInventoryItem vtable indices are SE/AE-only "
                         "verified; CheckShouldEquip allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        // Expected RTTI base: CombatInventoryItem (CheckShouldEquip is declared
        // there; every concrete spell/staff instantiation derives it).
        REL::Relocation<void*> expectedTD{ RE::RTTI_CombatInventoryItem };

        const REL::VariantID kVtables[] = {
            // spells in hand
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterArmor_[0],
            // staves in hand (a staff strips the forced weapon exactly like a
            // spell does, and EquipWeapon counts a staff as NEITHER category)
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterArmor_[0],
        };
        constexpr std::size_t kCheckShouldEquip = 0x0F;

        const int n = allowance::InstallOnVtables(kVtables, kCheckShouldEquip, &EquipGateThunk,
                                                   expectedTD.get(), "t2a", g_orig);
        spdlog::info("[t2a] CheckShouldEquip allowance hooked on {} spell/staff inventory-item "
                     "vtable(s) -- ch.8 casting-select claims now enforced here too.", n);
    }

}
