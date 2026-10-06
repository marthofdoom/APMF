#include "PCH.h"
#include "core/Log.h"
#include "core/CastProxy.h"
#include "core/Hook.h"   // hook::OnMainThread(): which thread the save sweep ran on (log only)
#include "core/Clock.h"        // MonotonicMs: the self-flip landing watch's grace window + slot ranking

// ============================================================================
// See core/CastProxy.h for the contract and for WHAT WAS REMOVED here (the whole
// forced-cast phase chain -- retired in favour of the engine seats). This TU is
// now ONLY the delivery-flip proxy pool: mint, transient-teach, un-teach, free,
// pre-save sweep, revert reset.
//
// The pool is fixed-size and keyed by (OWNER, SOURCE SPELL, HAND), REFERENCE-
// COUNTED across the live cast claims that name it (fix/apmf-proxy-per-claim-
// refcount, 2026-10-05). A proxy is a delivery-flipped copy of ONE spell taught to
// ONE actor for ONE hand key (right, left, or dual):
//   * Two live claims naming the same kSelf spell on the SAME hand key (a re-aim
//     RequestCast landing before the old claim's release, or a same-hand rival)
//     SHARE one form, and each holds one ref. The form stays taught until the LAST
//     of them releases.
//   * The same spell on the OTHER hand gets a DIFFERENT form (review F3). The
//     seats resolve a claim by its driven form (TryGetCastSeatClaimForForm), so a
//     left-hand heal at ally A and a right-hand heal at ally B with one shared form
//     would both resolve to the first claim's target and B would never be healed.
//     The cost is that a cross-hand re-aim teaches a new form, which needs a
//     CombatInventory rebuild -- core/EquipGate.cpp already dirties it while a
//     cast-seat claim stands.
//   * Two live claims naming DIFFERENT kSelf spells get DIFFERENT forms. A slot
//     that holds refs is NEVER re-Configured to another spell (the old per-owner
//     slot was, so one hand silently cast the other hand's spell).
// Every claim that got a form from Acquire owns exactly one ref and gives it back
// through Unref, deferred one main-thread hop past the Publish that removed the
// claim (Docs/INVARIANTS.md #20). Acquire runs synchronously inside Drain and
// Unref only in the Pump after it, so a claim acquired in the same Drain as an
// older claim's release always increments BEFORE the older claim decrements:
// the count can only reach zero once no published claim names the form.
//
// TWO DIRECTIONS (feat/apmf-self-delivery-proxy, 2026-10-05). The original flip turns
// a kSelf spell aimed at an ally into a kTargetActor copy; the SELF-FLIP turns an
// Aimed / Touch / TargetActor spell claimed at the caster itself into a kSelf copy
// (see core/CastProxy.h for the engine facts). The direction is fixed by the source's
// own delivery, so one (owner, spell, hand) can only ever want one of them; it is
// still part of the key and Acquire refuses a direction the source does not call for,
// so a caller bug can never hand a claim a form flipped the wrong way.
// ============================================================================

namespace apmf::castproxy {

    namespace {

        struct Slot {
            RE::SpellItem* form   = nullptr;   // the minted 0xFF dynamic form (survives a free, not a load)
            RE::FormID     source = 0;         // which real spell it currently mirrors
            RE::FormID     owner  = 0;         // 0 == the slot is free
            std::uint32_t  refs   = 0;         // live claims naming this form; owner != 0 <=> refs > 0
            Hand           hand   = Hand::kRight;   // the hand key it serves (meaningful while owned)
            Flip           flip   = Flip::kToTargetActor;   // which way it flips (meaningful while owned)
            std::uint64_t  freedMs = 0;        // when its last ref went (0 = never freed); slot ranking (F3)
        };

        const char* FlipName(Flip f) {
            return f == Flip::kToSelf ? "self-flip proxy (kSelf copy)" : "delivery-flip proxy (kTargetActor copy)";
        }

        const char* HandName(Hand h) {
            switch (h) {
            case Hand::kLeft: return "left";
            case Hand::kDual: return "dual";
            default:          return "right";
            }
        }
        // 8 = two per actor (one per hand: a two-hand heal with two different kSelf
        // spells) for the four actors the old one-slot-per-owner pool served. A slot's form is minted lazily, once, and
        // reused for the rest of the session, so an unused slot costs nothing.
        Slot g_slot[8];

        // ---- SELF-FLIP LANDING WATCH (principle 5, passive) ----------------------
        // One per slot, armed by a kToSelf mint (writer/main thread). The CHECK runs on the
        // OWNER's OWN update: core/Hook.cpp's Character 0xAD thunk calls
        // Arbiter::OnActorUpdate -> ControlMap::OnActorUpdate -> castproxy::OnOwnerUpdate right
        // AFTER that actor's own Actor::Update returned, on whichever worker runs that actor --
        // the thread that runs the actor's own active-effect update, sequentially, instead of
        // the player seat reading the list while the NPC's job mutates it (review F4, threading
        // carve-out (a)). Every other actor pays one relaxed load (g_armedWatches == 0) or eight
        // relaxed FormID compares. Each watch is guarded by its own LEAF mutex: the writer takes
        // it to arm / mark released / reset; the update seat only try_locks it (an update never
        // waits; a contended frame is skipped) and holds it across a pure read of the owner's
        // own list, never across an engine call that could take another lock.
        // FIRST sighting records the effects of the slot's form ALREADY on the owner (a reused
        // slot's stale effect, review F3) as a baseline; only an effect NOT in the baseline
        // counts as this mint's landing. (Baseline by pointer: a freed effect whose address is
        // re-used by the new one would read as old -- a false "NOT seen", never a false
        // "LANDED".) Logs once and disarms; or, kWatchGraceMs after the slot's last ref went
        // without a landing, logs "NOT seen". An owner that never updates again (unloaded)
        // keeps its watch until the slot is re-minted or ResetAll -- both say so / clear it.
        struct Watch {
            std::mutex                 mx;   // leaf
            std::atomic<RE::FormID>    owner{ 0 };   // 0 = not armed (relaxed pre-gate read)
            const RE::MagicItem*       form       = nullptr;
            RE::FormID                 source     = 0;
            std::uint64_t              mintedMs   = 0;
            std::uint64_t              releasedMs = 0;   // 0 while the slot still holds refs
            bool                       baselined  = false;
            // Every effect of the form already on the owner at first sighting, UNCAPPED (review
            // R2-2: Rally / Courage carry 6 effects, a reused slot can hold several casts' worth).
            std::vector<const RE::ActiveEffect*> base;
        };
        Watch g_watch[std::size(g_slot)];
        std::atomic<std::uint32_t> g_armedWatches{ 0 };
        constexpr std::uint64_t kWatchGraceMs = 5000;

        // Caller holds w.mx.
        void Disarm(Watch& w) {
            if (w.owner.load(std::memory_order_relaxed) != 0) g_armedWatches.fetch_sub(1, std::memory_order_relaxed);
            w.owner.store(0, std::memory_order_relaxed);
            w.form = nullptr; w.source = 0; w.mintedMs = 0; w.releasedMs = 0;
            w.baselined = false; w.base.clear();
        }

        // Caller holds w.mx. Writer/main thread.
        void Arm(Watch& w, RE::FormID a_owner, const RE::MagicItem* a_form, RE::FormID a_source) {
            Disarm(w);
            w.form     = a_form;
            w.source   = a_source;
            w.mintedMs = apmf::clock::MonotonicMs();
            g_armedWatches.fetch_add(1, std::memory_order_relaxed);
            w.owner.store(a_owner, std::memory_order_release);
        }

        // Copy the source's data, flip ONLY the delivery, and SHARE the source's
        // Effect* objects by pointer (that is the whole point -- identical effects,
        // different delivery). The borrowed pointers are what ResetAll must clear
        // before a load-time purge (INVARIANTS #19).
        void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src, Flip a_flip) {
            a_p->data          = a_src->data;                                // castingType/cost/etc.
            a_p->data.delivery = a_flip == Flip::kToSelf ? RE::MagicSystem::Delivery::kSelf
                                                         : RE::MagicSystem::Delivery::kTargetActor;   // the ONLY change
            a_p->effects.clear();
            for (auto* e : a_src->effects) a_p->effects.push_back(e);        // shared source Effect*
        }

    }

    RE::FormID Acquire(RE::FormID a_owner, RE::SpellItem* a_src, Hand a_hand, Flip a_flip) {
        if (!a_src || !a_owner) return 0;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner);
        if (!actor) return 0;   // no actor to teach the proxy to -- it could never be selected

        const auto sid = a_src->GetFormID();

        // The direction must be the one the source's own delivery calls for. A caller
        // asking otherwise is a bug: refused loudly, no ref taken, nothing taught.
        const bool srcSelf = a_src->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
        if (srcSelf != (a_flip == Flip::kToTargetActor)) {
            spdlog::error("[castproxy] 0x{} asked for a {} of spell 0x{} whose delivery is {} -- REFUSED (wrong "
                          "direction for this source), no proxy.",
                          apmf::log::Hex(a_owner), FlipName(a_flip), apmf::log::Hex(sid),
                          static_cast<int>(a_src->GetDelivery()));
            return 0;
        }

        // This owner already has a live proxy for THIS spell on THIS hand key: share
        // it (one more ref) and re-teach if it is somehow unknown. Never
        // re-Configured -- its source is already this spell.
        for (auto& s : g_slot) {
            if (s.owner != a_owner || s.source != sid || s.hand != a_hand || s.flip != a_flip || !s.form) continue;
            ++s.refs;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            spdlog::info("[castproxy] 0x{} SHARES {} 0x{} for spell 0x{} on the {} hand (refs {} -> {}).",
                         apmf::log::Hex(a_owner), FlipName(a_flip), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid), HandName(a_hand), s.refs - 1, s.refs);
            return s.form->GetFormID();
        }

        // Pick the FREE slot to (re-)mint (review F3). A freed proxy form can still be the
        // `spell` of a live active effect (a buff on an ally, or since the self-flip on the
        // caster) until that effect runs out; re-pointing it at another spell makes the seats'
        // AlreadyApplied read that stale effect as the NEW spell's. So, without walking any
        // actor's effect list from here: (1) a free slot that already mirrors THIS source with
        // THIS flip (its stale effects ARE this spell's), else (2) a slot never minted, else
        // (3) the slot freed longest ago. A rank-3 slot freed less than kStaleRiskMs ago is
        // said out loud (APMF-B58).
        constexpr std::uint64_t kStaleRiskMs = 120000;
        Slot* pick = nullptr;
        int   rank = 4;
        for (auto& c : g_slot) {
            if (c.owner != 0) continue;
            int r = 3;
            if (c.form && c.source == sid && c.flip == a_flip) r = 1;
            else if (!c.form) r = 2;
            if (r < rank || (r == 3 && rank == 3 && c.freedMs < pick->freedMs)) { pick = &c; rank = r; }
        }
        if (pick) {
            auto& s = *pick;
            if (rank == 3 && s.freedMs != 0) {
                const auto ago = apmf::clock::MonotonicMs() - s.freedMs;
                if (ago < kStaleRiskMs)
                    spdlog::info("[castproxy] re-pointing pool slot form 0x{} (was spell 0x{}, freed {} ms ago) to spell "
                                 "0x{}: an effect of the old spell may still be up under this form (APMF-B58) -- every "
                                 "slot is in use or recently freed.",
                                 apmf::log::Hex(s.form->GetFormID()), apmf::log::Hex(s.source), ago, apmf::log::Hex(sid));
            }
            if (!s.form) {
                auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                s.form  = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                if (!s.form) return 0;
                // Distinct FormIDs are what let the seats (and a client attributing a
                // fire to its hand) tell two proxies apart. The engine hands every
                // created form its own 0xFF id; if that ever fails, refuse loudly
                // rather than hand two claims one indistinguishable driven form.
                const auto nid = s.form->GetFormID();
                bool clash = (nid == 0);
                for (const auto& o : g_slot)
                    if (&o != &s && o.form && o.form->GetFormID() == nid) clash = true;
                if (clash) {
                    spdlog::error("[castproxy] minted proxy form has FormID 0x{} (zero or already held by "
                                  "another pool slot) -- REFUSED, no proxy for owner 0x{} spell 0x{}.",
                                  apmf::log::Hex(nid), apmf::log::Hex(a_owner), apmf::log::Hex(sid));
                    s.form = nullptr;   // never configured, never taught: nothing borrowed to clear
                    return 0;
                }
            }
            Configure(s.form, a_src, a_flip);   // only ever on a FREE slot (refs == 0)
            s.source = sid;
            s.owner  = a_owner;
            s.refs   = 1;
            s.hand   = a_hand;
            s.flip   = a_flip;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            const bool taught = actor->HasSpell(s.form);
            spdlog::info("[castproxy] 0x{} minted {} 0x{} for spell 0x{} (delivery {} -> {}) on the {} hand "
                         "(shared effects, transiently taught: {}; refs 0 -> 1).",
                         apmf::log::Hex(a_owner), FlipName(a_flip), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid), static_cast<int>(a_src->GetDelivery()),
                         static_cast<int>(s.form->GetDelivery()), HandName(a_hand), taught ? "yes" : "NO");
            {
                auto&             w = g_watch[static_cast<std::size_t>(&s - g_slot)];
                std::scoped_lock  lk(w.mx);
                if (const auto prev = w.owner.load(std::memory_order_relaxed); prev != 0) {
                    // Re-minted while the previous self-flip's watch was still open: that watch
                    // ends here, said out loud.
                    spdlog::info("[castproxy] 0x{} self-flip proxy (spell 0x{}) NOT seen on the caster before its pool "
                                 "slot was re-minted (+{} ms after its mint).",
                                 apmf::log::Hex(prev), apmf::log::Hex(w.source),
                                 apmf::clock::MonotonicMs() - w.mintedMs);
                    Disarm(w);
                }
                if (a_flip == Flip::kToSelf) Arm(w, a_owner, s.form, sid);
            }
            return s.form->GetFormID();
        }

        spdlog::warn("[castproxy] pool overflow (owner 0x{}, spell 0x{}, all {} slots held by live cast "
                     "claims) -- the claim gets NO proxy, so the seats will not force the "
                     "original form (a kSelf form at another actor is an unproven road, APMF-B62; an aimed form at "
                     "the caster is a ray at its own shooter).",
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
            spdlog::info("[castproxy] 0x{} FREED {} 0x{} (spell 0x{}; refs 1 -> 0) -- un-taught, slot "
                         "released.",
                         apmf::log::Hex(a_owner), FlipName(s.flip), apmf::log::Hex(a_proxy), apmf::log::Hex(s.source));
            {
                auto&            w = g_watch[static_cast<std::size_t>(&s - g_slot)];
                std::scoped_lock lk(w.mx);
                if (w.owner.load(std::memory_order_relaxed) != 0 && w.releasedMs == 0)
                    w.releasedMs = apmf::clock::MonotonicMs();   // the landing watch's grace window starts now
            }
            s.freedMs = apmf::clock::MonotonicMs();
            // source + flip are KEPT on a free slot (review R2-1): the form still mirrors that
            // spell, which is what the mint's rank-1 reuse ("same source + flip") matches. Every
            // other reader of `source` gates on `owner != 0` first (share loop, Unref, ReteachLive).
            s.owner  = 0;
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
        for (auto& w : g_watch) {
            std::scoped_lock lk(w.mx);
            Disarm(w);
        }
    }

    void OnOwnerUpdate(RE::Actor* a_actor) {
        if (g_armedWatches.load(std::memory_order_relaxed) == 0 || !a_actor) return;
        const RE::FormID fid = a_actor->GetFormID();
        for (auto& w : g_watch) {
            if (w.owner.load(std::memory_order_relaxed) != fid) continue;
            std::unique_lock lk(w.mx, std::try_to_lock);
            if (!lk.owns_lock() || w.owner.load(std::memory_order_acquire) != fid || !w.form) continue;
            const auto now = apmf::clock::MonotonicMs();
            std::uint32_t hits = 0, stale = 0;
            auto*         mt   = a_actor->AsMagicTarget();
            auto*         list = mt ? mt->GetActiveEffectList() : nullptr;
            if (list) {
                for (auto* ae : *list) {
                    if (!ae || ae->spell != w.form) continue;
                    if (!w.baselined) {
                        w.base.push_back(ae);
                        continue;
                    }
                    const bool old = std::find(w.base.begin(), w.base.end(), ae) != w.base.end();
                    old ? ++stale : ++hits;
                }
            }
            if (!w.baselined) {
                w.baselined = true;
                if (!w.base.empty())
                    spdlog::info("[castproxy] 0x{} self-flip proxy 0x{} (spell 0x{}): {} effect(s) of this pool form were "
                                 "already on the caster before the cast (a reused slot's stale effect, APMF-B58) -- "
                                 "ignored by the landing watch.",
                                 apmf::log::Hex(fid), apmf::log::Hex(w.form->GetFormID()), apmf::log::Hex(w.source),
                                 w.base.size());
                continue;
            }
            if (hits != 0) {
                spdlog::info("[castproxy] 0x{} self-flip proxy 0x{} (spell 0x{}) LANDED on the caster: {} new active "
                             "effect(s) of the proxy on 0x{} ({} pre-existing ignored), +{} ms after the mint (seen on "
                             "the owner's own update).",
                             apmf::log::Hex(fid), apmf::log::Hex(w.form->GetFormID()), apmf::log::Hex(w.source), hits,
                             apmf::log::Hex(fid), stale, now - w.mintedMs);
                Disarm(w);
                continue;
            }
            if (w.releasedMs != 0 && now - w.releasedMs >= kWatchGraceMs) {
                spdlog::info("[castproxy] 0x{} self-flip proxy (spell 0x{}) NOT seen on the caster: no new active effect "
                             "of it on 0x{} from the mint (+{} ms) to {} ms after its last claim released (never fired, "
                             "or an instant / zero-duration effect that left nothing to see).",
                             apmf::log::Hex(fid), apmf::log::Hex(w.source), apmf::log::Hex(fid), now - w.mintedMs,
                             kWatchGraceMs);
                Disarm(w);
            }
        }
    }

    void PreSaveSweep() {
        // Belt-and-braces (INVARIANTS #19): a save can be taken at ANY moment, so
        // un-teach + deselect every live proxy before a record is written. Slots stay
        // owned with their refs; plugin.cpp's OnSave Posts ReteachLive right after.
        int swept = 0;
        for (auto& s : g_slot) {
            if (!s.owner || !s.form) continue;
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.owner)) {
                actor->DeselectSpell(s.form);
                actor->RemoveSpell(s.form);
                ++swept;
            }
        }
        if (swept != 0) {
            spdlog::info("[castproxy] save: un-taught {} live proxy form(s) before the save is written "
                         "(on the Drain thread: {}); ReteachLive restores them on the next pump.",
                         swept, apmf::hook::OnMainThread() ? "yes" : "NO");
        }
    }

    void ReteachLive() {
        for (auto& s : g_slot) {
            if (!s.owner || s.refs == 0 || !s.form) continue;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.owner);
            if (!actor || actor->HasSpell(s.form)) continue;
            actor->AddSpell(s.form);   // TRANSIENT teach, exactly as Acquire does
            spdlog::info("[castproxy] 0x{} re-taught {} 0x{} (spell 0x{}, {} hand, refs {}) after the save "
                         "sweep -- its claim is still live.",
                         apmf::log::Hex(s.owner), FlipName(s.flip), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(s.source), HandName(s.hand), s.refs);
        }
    }

}
