#include "PCH.h"
#include "core/Log.h"
#include "core/CastProxy.h"

// ============================================================================
// See core/CastProxy.h for the contract and for WHAT WAS REMOVED here (the whole
// forced-cast phase chain -- retired in favour of the engine seats). This TU is
// now ONLY the delivery-flip proxy pool: mint, transient-teach, un-teach, free,
// pre-save sweep, revert reset.
//
// The pool is fixed-size and per-OWNER (never per-hand): a dual-cast uses ONE
// spell in both hands, so one proxy form covers both. An owner already holding a
// slot re-targets it in place rather than consuming a second.
// ============================================================================

namespace apmf::castproxy {

    namespace {

        struct Slot {
            RE::SpellItem* form   = nullptr;   // the minted 0xFF dynamic form (survives Free, not a load)
            RE::FormID     source = 0;         // which real spell it currently mirrors
            RE::FormID     owner  = 0;         // 0 == the slot is free
        };
        Slot g_slot[4];

        // Copy the source's data, flip ONLY the delivery, and SHARE the source's
        // Effect* objects by pointer (that is the whole point -- identical effects,
        // different delivery). The borrowed pointers are what ResetAll must clear
        // before a load-time purge (INVARIANTS #19).
        void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src) {
            a_p->data          = a_src->data;                                // castingType/cost/etc.
            a_p->data.delivery = RE::MagicSystem::Delivery::kTargetActor;    // the ONLY change
            a_p->effects.clear();
            for (auto* e : a_src->effects) a_p->effects.push_back(e);        // shared source Effect*
        }

    }

    RE::FormID Acquire(RE::FormID a_owner, RE::SpellItem* a_src) {
        if (!a_src || !a_owner) return 0;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner);
        if (!actor) return 0;   // no actor to teach the proxy to -- it could never be selected

        const auto sid = a_src->GetFormID();

        // Already holding a slot: re-target it in place (re-share the new effects)
        // and re-teach if a PreSaveSweep/park un-taught it.
        for (auto& s : g_slot) {
            if (s.owner != a_owner || !s.form) continue;
            if (s.source != sid) Configure(s.form, a_src);
            s.source = sid;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            return s.form->GetFormID();
        }

        for (auto& s : g_slot) {
            if (s.owner != 0) continue;
            if (!s.form) {
                auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                s.form  = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                if (!s.form) return 0;
            }
            Configure(s.form, a_src);
            s.source = sid;
            s.owner  = a_owner;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            spdlog::info("[castproxy] 0x{} minted delivery-flip proxy 0x{} for kSelf spell 0x{} "
                         "(kTargetActor copy, shared effects, transiently taught).",
                         apmf::log::Hex(a_owner), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid));
            return s.form->GetFormID();
        }

        spdlog::warn("[castproxy] pool overflow (owner 0x{}, all {} slots held by other live cast "
                     "claims) -- the claim gets NO proxy, so the seats will not force the original "
                     "kSelf form at another actor (it would land on the caster).",
                     apmf::log::Hex(a_owner), static_cast<int>(std::size(g_slot)));
        return 0;
    }

    void Free(RE::FormID a_owner) {
        for (auto& s : g_slot) {
            if (s.owner != a_owner) continue;
            if (s.form) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner)) {
                    actor->DeselectSpell(s.form);   // never left equipped
                    actor->RemoveSpell(s.form);     // never left known/castable
                }
            }
            s.owner  = 0;
            s.source = 0;
        }
    }

    RE::FormID FormForOwner(RE::FormID a_owner) {
        for (auto& s : g_slot)
            if (s.owner == a_owner && s.form) return s.form->GetFormID();
        return 0;
    }

    void ResetAll() {
        // Revert / new game / kPreLoadGame. Clear the BORROWED source Effect* FIRST
        // (Configure copied them by pointer) so the load-time dynamic-form purge frees
        // an EMPTY array and can never double-free a live source spell's effects --
        // MFO's `native/Actuation_Direct.cpp` ConcProxy::Reset, verbatim in shape --
        // then null the slot so the next claim re-mints. No engine calls: the actors
        // are being replaced.
        for (auto& s : g_slot) {
            if (s.form) s.form->effects.clear();
            s = {};
        }
    }

    void PreSaveSweep() {
        // Belt-and-braces (INVARIANTS #19): a save can be taken at ANY moment, so
        // un-teach + deselect every live proxy before a record is written. Slots stay
        // owned; Acquire re-teaches on the claim's next Drain pass.
        for (auto& s : g_slot) {
            if (!s.owner || !s.form) continue;
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.owner)) {
                actor->DeselectSpell(s.form);
                actor->RemoveSpell(s.form);
            }
        }
    }

}
