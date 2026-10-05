#include "PCH.h"
#include "core/Log.h"
#include "core/CastProxy.h"

// ============================================================================
// See core/CastProxy.h for the contract and for WHAT WAS REMOVED here (the whole
// forced-cast phase chain -- retired in favour of the engine seats). This TU is
// now ONLY the delivery-flip proxy pool: mint, transient-teach, un-teach, free,
// pre-save sweep, revert reset.
//
// The pool is fixed-size and keyed by (OWNER, SOURCE SPELL), REFERENCE-COUNTED
// across the live cast claims that name it (fix/apmf-proxy-per-claim-refcount,
// 2026-10-05). A proxy is a delivery-flipped copy of ONE spell taught to ONE
// actor, so that pair is exactly its identity:
//   * Two live claims on the same actor naming the SAME kSelf spell (left hand +
//     right hand healing two allies, a dual cast, or a re-aim RequestCast landing
//     before the old claim's release) SHARE one form, and each holds one ref.
//     The form stays taught until the LAST of them releases.
//   * Two live claims naming DIFFERENT kSelf spells get DIFFERENT forms. A slot
//     that holds refs is NEVER re-Configured to another spell (the old per-owner
//     slot was, so one hand silently cast the other hand's spell).
// Keying by spell rather than by hand is deliberate: a same-spell re-aim onto the
// other hand, or a dual cast replacing a single-hand one, keeps the SAME form, so
// it needs no new CombatInventory item (core/CastProxy.h CLIENT DEPENDENCY).
// Every claim that got a form from Acquire owns exactly one ref and gives it back
// through Unref, deferred one main-thread hop past the Publish that removed the
// claim (Docs/INVARIANTS.md #20). Acquire runs synchronously inside Drain and
// Unref only in the Pump after it, so a claim acquired in the same Drain as an
// older claim's release always increments BEFORE the older claim decrements:
// the count can only reach zero once no published claim names the form.
// ============================================================================

namespace apmf::castproxy {

    namespace {

        struct Slot {
            RE::SpellItem* form   = nullptr;   // the minted 0xFF dynamic form (survives a free, not a load)
            RE::FormID     source = 0;         // which real spell it currently mirrors
            RE::FormID     owner  = 0;         // 0 == the slot is free
            std::uint32_t  refs   = 0;         // live claims naming this form; owner != 0 <=> refs > 0
        };
        // 8 = two per actor (one per hand: a two-hand heal with two different kSelf
        // spells) for the four actors the old one-slot-per-owner pool served. A slot's form is minted lazily, once, and
        // reused for the rest of the session, so an unused slot costs nothing.
        Slot g_slot[8];

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

        // This owner already has a live proxy for THIS spell: share it (one more ref)
        // and re-teach if a PreSaveSweep un-taught it. Never re-Configured -- its
        // source is already this spell.
        for (auto& s : g_slot) {
            if (s.owner != a_owner || s.source != sid || !s.form) continue;
            ++s.refs;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            spdlog::info("[castproxy] 0x{} SHARES delivery-flip proxy 0x{} for kSelf spell 0x{} "
                         "(refs {} -> {}).",
                         apmf::log::Hex(a_owner), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid), s.refs - 1, s.refs);
            return s.form->GetFormID();
        }

        for (auto& s : g_slot) {
            if (s.owner != 0) continue;
            if (!s.form) {
                auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                s.form  = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                if (!s.form) return 0;
            }
            Configure(s.form, a_src);   // only ever on a FREE slot (refs == 0)
            s.source = sid;
            s.owner  = a_owner;
            s.refs   = 1;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            spdlog::info("[castproxy] 0x{} minted delivery-flip proxy 0x{} for kSelf spell 0x{} "
                         "(kTargetActor copy, shared effects, transiently taught; refs 0 -> 1).",
                         apmf::log::Hex(a_owner), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid));
            return s.form->GetFormID();
        }

        spdlog::warn("[castproxy] pool overflow (owner 0x{}, spell 0x{}, all {} slots held by live cast "
                     "claims) -- the claim gets NO proxy, so the seats will not force the original "
                     "kSelf form at another actor (it would land on the caster).",
                     apmf::log::Hex(a_owner), apmf::log::Hex(sid), static_cast<int>(std::size(g_slot)));
        return 0;
    }

    void Unref(RE::FormID a_owner, RE::FormID a_proxy) {
        for (auto& s : g_slot) {
            if (s.owner != a_owner || !s.form || s.form->GetFormID() != a_proxy) continue;
            if (s.refs == 0) break;   // cannot happen (owner != 0 <=> refs > 0); reported below
            --s.refs;
            if (s.refs != 0) {
                spdlog::info("[castproxy] 0x{} released one claim on proxy 0x{} (spell 0x{}; refs {} -> {}) "
                             "-- still taught, another live claim names it.",
                             apmf::log::Hex(a_owner), apmf::log::Hex(a_proxy), apmf::log::Hex(s.source),
                             s.refs + 1, s.refs);
                return;
            }
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner)) {
                actor->DeselectSpell(s.form);   // never left equipped
                actor->RemoveSpell(s.form);     // never left known/castable
            }
            spdlog::info("[castproxy] 0x{} FREED proxy 0x{} (spell 0x{}; refs 1 -> 0) -- un-taught, slot "
                         "released.",
                         apmf::log::Hex(a_owner), apmf::log::Hex(a_proxy), apmf::log::Hex(s.source));
            s.owner  = 0;
            s.source = 0;
            return;
        }
        // A claim held a ref that no live slot accounts for. Only a pool reset with a
        // release still queued could do that, and every reset path Discard()s the
        // main-thread queue first -- so this is a ledger bug, said out loud and never
        // papered over (nothing is un-taught on a guess).
        spdlog::warn("[castproxy] 0x{} release of proxy 0x{} matched NO live slot -- the proxy refcount "
                     "ledger is out of step with the claims. Nothing un-taught.",
                     apmf::log::Hex(a_owner), apmf::log::Hex(a_proxy));
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
