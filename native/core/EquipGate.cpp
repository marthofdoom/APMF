#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/ControlMap.h"
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
//
// SECOND FACET (2026-09-03): also honors a ch.15 kIntent_Equipment claim --
// the weapon-order side of MFO CombatStyle.cpp's SAME gate (its g_owned[..]
// .equipOrder path, CombatStyle.cpp:286-317). MFO holds a follower's hands to
// a chosen weapon by RequestEx'ing kIntent_Equipment with param.form = that
// weapon's FormID; while the claim stands, every spell/staff item this hook
// sees (subjectForm) is, by construction, never equal to a weapon FormID, so
// Allowed(kIntent_Equipment, subjectForm) denies ALL of them -- exactly
// CombatStyle.cpp's "every vtable this gate patches is hand-competing magic
// by construction" rule, without per-item-type dispatch.
//
// The one exemption MFO's gate carries (its WantedSpell check, line 305) is
// the off-hand loan: a gambit spell the follower is ALSO allowed to cast
// stays equippable even while the weapon order holds the stance. Ported
// here as: if `subjectForm` IS the actor's own actively-claimed ch.8
// kIntent_SelectSpell form, it is exempt from the ch.15 deny outright (and,
// as before, the ch.8 exclusivity check never denies its own claimed spell).
// This is what lets MFO retire its own g_owned/equipOrder/WantedSpell gate
// entirely in favor of two independent APMF claims (ch.8 + ch.15) whose
// deny decisions this ONE hook now combines.
//
// PER-HAND (2026-09-0x, INVARIANTS #18): a `CombatInventoryItem` instance
// carries its OWN `itemSlot.equipSlot` (a real member, static_assert'd offset
// below) -- the AI sets this to the vanilla Left/Right Hand BGSEquipSlot when
// it builds the item for that hand. Comparing it against
// `BGSDefaultObjectManager`'s own Left/Right Hand default objects resolves
// which hand THIS CheckShouldEquip call is about, so the ch.8b cast-execution
// narrowing (`Allowance::AllowedCastForHand`) can be scoped to the claimed
// hand only, leaving the OTHER hand's re-arm decision fully AI-governed.
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
        static_assert(offsetof(RE::CombatInventoryItem, itemSlot) == 0x20,
                      "CombatInventoryItem::itemSlot moved -- re-verify against the "
                      "pinned header before shipping (per-hand deny reads "
                      "itemSlot.equipSlot, INVARIANTS #18)");

        using CheckShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);

        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        std::atomic<bool> g_installed{ false };

        // Per-hand deny (INVARIANTS #18): the vanilla Left/Right Hand BGSEquipSlot
        // forms, resolved ONCE at install through CommonLib's own version-robust
        // singleton (RE::BGSDefaultObjectManager -- the SAME table the engine
        // itself uses to look these up; no hardcoded FormID). A concrete
        // CombatInventoryItem instance's OWN `itemSlot.equipSlot` (set by the AI
        // when IT builds that item for a specific hand) is compared against these
        // to resolve which hand THIS CheckShouldEquip call is deliberating for.
        std::atomic<RE::BGSEquipSlot*> g_leftHandSlot{ nullptr };
        std::atomic<RE::BGSEquipSlot*> g_rightHandSlot{ nullptr };

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

            // Per-hand deny (INVARIANTS #18): resolve which hand THIS item instance
            // is for from its own itemSlot.equipSlot -- a real struct member (see
            // the static_assert above), not an invented offset. Neither vanilla
            // hand slot (kEitherHandEquip, a null slot, or the singleton table not
            // yet resolved) degrades to kUnknown -- AllowedCastForHand then falls
            // back to the actor-wide floor, never a guess.
            auto* slot = a_this->itemSlot.equipSlot;
            const auto lh = g_leftHandSlot.load(std::memory_order_acquire);
            const auto rh = g_rightHandSlot.load(std::memory_order_acquire);
            const auto callerHand =
                (slot && slot == lh) ? allowance::Hand::kLeft  :
                (slot && slot == rh) ? allowance::Hand::kRight :
                                        allowance::Hand::kUnknown;

            // Off-hand loan exemption (mirrors MFO CombatStyle.cpp's WantedSpell
            // rule): if this item IS the actor's own actively-claimed ch.8 spell,
            // it is equippable no matter what a ch.15 weapon-order claim says --
            // resolved directly off the SAME winning claim the ch.8 exclusivity
            // check below reads, so both branches necessarily agree with it.
            APMF_API::APMF_Param castClaim{};
            const bool isClaimedSpell =
                subjectForm != 0 &&
                apmf::ControlMap::Get().TryGetOwningClaim(fid, APMF_API::kIntent_SelectSpell, castClaim) &&
                castClaim.form == subjectForm;
            if (isClaimedSpell) return engineSays;

            // Same exemption, one level deeper -- APMF's OWN DRIVEN form (H1, the
            // 2026-09-05 drive-chain review). The exemption directly above only
            // recognises the ch.8 claim's literal `param.form`; when APMF's cast
            // executor drives a delivery-flip PROXY instead, the proxy is a form
            // the ch.8 claim never names, so the ch.8 narrow below would deny the
            // very item APMF put in that hand. `CastClaimNamesForHand` is the
            // strict positive test (a kIntent_Cast claim standing on THIS hand
            // naming THIS spell-or-proxy), so this widens nothing else: it admits
            // exactly the spell/proxy pair the ch.8b claim already admits, and
            // still lets the ENGINE have the final word. Applied here for the same
            // reason as in CastGate -- a claim keyed on a FormID must admit the
            // substitute form at EVERY gate that keys on that FormID.
            if (allowance::CastClaimNamesForHand(fid, subjectForm, callerHand))
                return engineSays;

            // ch.8 -- cast-select exclusivity (unchanged: deny any OTHER spell
            // while a SelectSpell claim names one).
            if (!allowance::Allowed(fid, APMF_API::kIntent_SelectSpell, subjectForm))
                return false;

            // ch.15 -- weapon-order claim: deny any spell/staff re-arm while a
            // kIntent_Equipment claim holds this actor's hands (its param.form,
            // a weapon FormID, never matches a spell/staff subjectForm, so a
            // held claim denies every item this hook sees -- the exemption
            // above already let the claimed cast spell through).
            if (!allowance::Allowed(fid, APMF_API::kIntent_Equipment, subjectForm))
                return false;

            // ch.8b -- cast-execution claim: while a kIntent_Cast claim stands the AI
            // may not re-arm THE CLAIMED HAND with any spell/staff OTHER than the
            // claimed cast spell/proxy (the freeze-free equivalent of "the package
            // holds the spell in hand"). AllowedCastForHand permits exactly claim
            // spell + proxy on the claim's own hand, denies every other subjectForm
            // on that hand, and -- per-hand deny, INVARIANTS #18 -- leaves the OTHER
            // hand's re-arm decision untouched (callerHand resolved above); an actor
            // with no cast claim, or a call this hook cannot resolve to a hand, is
            // unaffected / degrades to the actor-wide floor.
            if (!allowance::AllowedCastForHand(fid, subjectForm, callerHand))
                return false;

            return engineSays;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t2a] VR runtime -- CombatInventoryItem vtable indices are SE/AE-only "
                         "verified; CheckShouldEquip allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        // Per-hand deny (INVARIANTS #18): resolve the vanilla Left/Right Hand
        // BGSEquipSlot forms ONCE, through CommonLib's own version-robust
        // BGSDefaultObjectManager singleton (the same table the engine consults) --
        // never a hardcoded FormID. A null result (a runtime that somehow has no
        // default-object table populated yet) just means every item resolves to
        // Hand::kUnknown below and this gate degrades to its pre-existing
        // actor-wide floor -- never a crash, never a guess.
        if (auto* dobj = RE::BGSDefaultObjectManager::GetSingleton()) {
            g_leftHandSlot.store(dobj->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip),
                                 std::memory_order_release);
            g_rightHandSlot.store(dobj->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip),
                                  std::memory_order_release);
        }

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
                     "vtable(s) -- ch.8 casting-select and ch.15 equipment (weapon-order) "
                     "claims now enforced here too; ch.8b is per-hand-scoped via "
                     "itemSlot.equipSlot (left-hand slot {}, right-hand slot {}).",
                     n, static_cast<void*>(g_leftHandSlot.load(std::memory_order_relaxed)),
                     static_cast<void*>(g_rightHandSlot.load(std::memory_order_relaxed)));
    }

}
