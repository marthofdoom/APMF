#include "PCH.h"
#include "core/Log.h"
#include "core/CastProxy.h"
#include "core/Hook.h"   // hook::OnMainThread(): which thread the save sweep ran on (log only)
#include "core/Clock.h"        // MonotonicMs: the self-flip landing watch's grace window + slot ranking
#include "core/Allowance.h"    // RuntimeSupported: the save / load direct member writes run on the three proven builds only
#include "core/MainThread.h"   // Post: the load watch re-checks on every pump

#include <unordered_set>

// ============================================================================
// See core/CastProxy.h for the contract and for WHAT WAS REMOVED here (the whole
// forced-cast phase chain -- retired in favour of the engine seats). This TU is
// now ONLY the delivery-flip proxy pool: mint, transient-teach, un-teach, free,
// the save sweep + its second check, the load-side repair, revert reset.
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
            // Every OTHER spell this form mirrored earlier this session (a re-point). An effect of an old
            // mirror can outlive the re-point (APMF-B58); the save sweep finds its real spell here.
            std::vector<RE::FormID> past;
            // Every actor that owned this slot earlier this session and released it (Unref). The save
            // sweep walks the process lists; an earlier owner that has left them (no AI process) is
            // looked up by id from here, like the current owner (review F2 of f2fa89b).
            std::vector<RE::FormID> pastOwners;
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
        // THE POOL'S SIZE (APMF-B62, MFO-B270). It used to be 8 = two per actor for four actors. Since the
        // self-flip, every Aimed / Touch / TargetActor buff or heal a claim aims at its own caster takes a
        // slot too, beside every kSelf heal or buff at an ally, so the demand is up to ONE PER HAND KEY
        // (right, left, dual) PER FLIP per claiming actor, plus a re-aim's one-frame overlap on a new spell,
        // and on overflow a legitimate claim stood as a dead deny of its hand. The bound is now kPoolCap
        // slots: 64 = three hand keys for 21 actors casting a flipped spell at the same moment, far above any
        // party (the field peak was 2, APMF-B62's verdict 2026-10-06). It is a CEILING, not an allocation:
        // a slot's form is minted LAZILY, the first time the pool needs one more than it has, and the minted
        // slots are always the PREFIX [0, g_minted) (a new mint takes index g_minted), so a session that never
        // needs more than two proxies mints two forms and every loop below walks two slots. A slot's form is
        // reused for the rest of the session (until ResetAll). Reaching the cap means a client holds far
        // more flipped cast claims than any party casts: the overflow is an ERROR line, counted, naming who
        // holds the slots (never a silent plain deny).
        constexpr std::size_t kPoolCap = 64;
        Slot g_slot[kPoolCap];
        // How many slots have a minted form (the prefix). Written by the writer/main thread only (Acquire,
        // ResetAll), read relaxed by OnOwnerUpdate's watch scan (a stale smaller value only skips a just-armed
        // watch for one update).
        std::atomic<std::uint32_t> g_minted{ 0 };
        std::atomic<std::uint64_t> g_overflows{ 0 };   // pool overflows since the DLL loaded (never reset)
        std::uint32_t MintedCount() { return g_minted.load(std::memory_order_relaxed); }

        // ---- SAVE / LOAD INTEGRITY state (the block before PreSaveSweep has the mechanism) ----
        // An active effect PreSaveSweep re-keyed from its proxy to the source spell for the length of
        // the save; SaveWrittenCheck puts the proxy back. Main thread, inside one save call.
        struct Rekey {
            RE::FormID        target;
            RE::ActiveEffect* ae;       // compared by address on the target's own list, never trusted alone
            RE::SpellItem*    proxy;
            RE::SpellItem*    source;
        };
        std::vector<Rekey> g_rekeyed;
        bool               g_sweptForSave = false;   // kSaveGame ran since the last co-save callback

        // Actors holding a LIVE runtime (0xFF) form in a spell slot after a load: re-checked on every
        // pump until the form leaves the slot or dies (then repaired). No expiry (principle 9): the
        // form's lifetime belongs to whoever made it. Main thread.
        constexpr std::size_t   kLoadWatchCap = 64;
        std::vector<RE::FormID> g_loadWatch;
        std::uint32_t           g_loadWatchGen    = 0;       // bumped by ResetAll: a queued tick of an older world stops
        bool                    g_loadWatchPosted = false;
        bool                    g_loadWatchFull   = false;   // overflow said once per world


        // ---- SELF-FLIP LANDING WATCH (principle 5, passive) ----------------------
        // One per slot, armed by a kToSelf mint (writer/main thread). The CHECK runs on the
        // OWNER's OWN update: core/Hook.cpp's Character 0xAD thunk calls
        // Arbiter::OnActorUpdate -> ControlMap::OnActorUpdate -> castproxy::OnOwnerUpdate right
        // AFTER that actor's own Actor::Update returned, on whichever worker runs that actor --
        // the thread that runs the actor's own active-effect update, sequentially, instead of
        // the player seat reading the list while the NPC's job mutates it (review F4, threading
        // carve-out (a)). Every other actor pays one relaxed load (g_armedWatches == 0) or one relaxed
        // FormID compare per MINTED slot (the prefix, APMF-B62). Each watch is guarded by its own LEAF mutex: the writer takes
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
        //
        // The source's IDENTITY travels too (fix/apmf-gate-probe-proxy-name, 2026-10-26: the field
        // proxy was nameless, so the active-effect UI showed generic text and MFO logged "CAST
        // (unnamed)"): its name and its menu display object, BOTH written on EVERY Configure (a
        // re-point of a free slot overwrites the previous spell's values, a source with none writes
        // none -- never a stale one). The menu display object is a plain member write, the exact
        // body of the engine accessor, verified on all three unpacked binaries (SpellItem COL
        // offsets: BGSMenuDisplayObject at +0xA0, GetMenuDisplayObject = `mov rax,[rcx+8]` at its
        // vtable slot 4), so no vfunc is called through CommonLib's declaration (principle 6).
        // The EQUIP SLOT is deliberately NOT copied (tier-3 review F1 of 6ea134d): the SpellItem
        // factory's constructor gives every proxy EitherHand (1.6.1170 0x14F903..0x14F91E:
        // SetEquipSlot on the +0x90 sub-object with default object 0x15; 1.5.97 objects[21]), and
        // that is what every field-proven proxy cast ran with. A source's BothHands / single-hand /
        // Voice slot would change which hands the AI uses, and a null-slot source would keep the
        // proxy out of the CombatInventory (a masked non-cast). fullName is a pooled
        // BSFixedString: the copy-assign takes its own reference (inline acquire) and drops the
        // old one (BSStringPool release, id 67847 / 69192, mapped on 1.7.104 in the fork's
        // idmap), so the proxy OWNS its name -- nothing borrowed, the form's own destructor drops
        // it. NOT copied (on purpose): KEYWORDS (an engine array allocation, and #19 would need
        // a reset for it -- REVIEW-BACKLOG APMF-B206), the TESDescription (not copyable; the MGEF
        // description is shared through the borrowed Effect* already).
        void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src, Flip a_flip) {
            a_p->data          = a_src->data;                                // castingType/cost/etc.
            a_p->data.delivery = a_flip == Flip::kToSelf ? RE::MagicSystem::Delivery::kSelf
                                                         : RE::MagicSystem::Delivery::kTargetActor;   // the ONLY change
            a_p->effects.clear();
            for (auto* e : a_src->effects) a_p->effects.push_back(e);        // shared source Effect*
            a_p->fullName       = a_src->fullName;         // TESFullName +0x30/+0x08: owned pool reference
            a_p->menuDispObject = a_src->menuDispObject;   // BGSMenuDisplayObject +0xA0/+0x08
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
        for (std::uint32_t i = 0; i < MintedCount(); ++i) {
            auto& s = g_slot[i];
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
        // (2) is the slot at index g_minted (the first never-minted one), so the minted slots stay a prefix.
        Slot* pick = nullptr;
        int   rank = 4;
        const std::uint32_t minted = MintedCount();
        for (std::uint32_t i = 0; i < minted; ++i) {
            auto& c = g_slot[i];
            if (c.owner != 0) continue;
            const int r = (c.source == sid && c.flip == a_flip) ? 1 : 3;
            if (r < rank || (r == 3 && rank == 3 && c.freedMs < pick->freedMs)) { pick = &c; rank = r; }
        }
        if (rank > 2 && minted < kPoolCap) { pick = &g_slot[minted]; rank = 2; }
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
                for (std::uint32_t i = 0; i < MintedCount(); ++i)
                    if (&g_slot[i] != &s && g_slot[i].form && g_slot[i].form->GetFormID() == nid) clash = true;
                if (clash) {
                    spdlog::error("[castproxy] minted proxy form has FormID 0x{} (zero or already held by "
                                  "another pool slot) -- REFUSED, no proxy for owner 0x{} spell 0x{}.",
                                  apmf::log::Hex(nid), apmf::log::Hex(a_owner), apmf::log::Hex(sid));
                    s.form = nullptr;   // never configured, never taught: nothing borrowed to clear
                    return 0;
                }
                // The new form extends the minted prefix (this slot is g_slot[g_minted], rank 2 above).
                g_minted.store(MintedCount() + 1, std::memory_order_relaxed);
                if (MintedCount() > kPoolCap / 2)
                    spdlog::warn("[castproxy] the proxy pool has minted {} of its {} forms this session -- far above "
                                 "any party's demand (APMF-B62): a client may be holding many flipped cast claims.",
                                 MintedCount(), kPoolCap);
            }
            if (s.source != 0 && s.source != sid && std::find(s.past.begin(), s.past.end(), s.source) == s.past.end())
                s.past.push_back(s.source);   // the save sweep's SourceOf looks an old mirror's effect up here
            Configure(s.form, a_src, a_flip);   // only ever on a FREE slot (refs == 0)
            s.source = sid;
            s.owner  = a_owner;
            s.refs   = 1;
            s.hand   = a_hand;
            s.flip   = a_flip;
            if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach
            const bool taught = actor->HasSpell(s.form);
            const char* pname = s.form->fullName.c_str();
            spdlog::info("[castproxy] 0x{} minted {} 0x{} for spell 0x{} (delivery {} -> {}) on the {} hand "
                         "(shared effects, transiently taught: {}; refs 0 -> 1; name \"{}\", equip slot 0x{} (the "
                         "factory's, not the source's), menu display object 0x{}).",
                         apmf::log::Hex(a_owner), FlipName(a_flip), apmf::log::Hex(s.form->GetFormID()),
                         apmf::log::Hex(sid), static_cast<int>(a_src->GetDelivery()),
                         static_cast<int>(s.form->GetDelivery()), HandName(a_hand), taught ? "yes" : "NO",
                         pname ? pname : "", apmf::log::Hex(s.form->equipSlot ? s.form->equipSlot->GetFormID() : 0u),
                         apmf::log::Hex(s.form->menuDispObject ? s.form->menuDispObject->GetFormID() : 0u));
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

        // OVERFLOW (APMF-B62): every one of the kPoolCap slots holds refs of a live claim. Loud: an ERROR line,
        // counted, naming how many slots each owner holds, so the client that floods the pool is in the log.
        const auto n = g_overflows.fetch_add(1, std::memory_order_relaxed) + 1;
        std::string holders;
        {
            std::vector<std::pair<RE::FormID, std::uint32_t>> per;
            for (std::uint32_t i = 0; i < MintedCount(); ++i) {
                const auto o = g_slot[i].owner;
                if (!o) continue;
                auto it = std::find_if(per.begin(), per.end(), [&](const auto& e) { return e.first == o; });
                if (it == per.end()) per.emplace_back(o, 1u); else ++it->second;
            }
            for (const auto& [o, c] : per) holders += fmt::format("{}0x{} x{}", holders.empty() ? "" : ", ", apmf::log::Hex(o), c);
        }
        spdlog::error("[castproxy] pool OVERFLOW #{} (owner 0x{}, spell 0x{}, {} hand, {}): all {} slots are held by "
                      "live cast claims [{}] -- the claim gets NO proxy, so the seats will not force the original form "
                      "(a kSelf form at another actor is an unproven road, APMF-B62, and an aimed form at the caster is a "
                      "ray at its own shooter) and the claim stands as a plain deny of its hand.",
                      n, apmf::log::Hex(a_owner), apmf::log::Hex(sid), HandName(a_hand), FlipName(a_flip),
                      static_cast<int>(kPoolCap), holders);
        return 0;
    }

    void Unref(RE::FormID a_owner, RE::FormID a_proxy) {
        for (std::uint32_t i = 0; i < MintedCount(); ++i) {
            auto& s = g_slot[i];
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
            if (std::find(s.pastOwners.begin(), s.pastOwners.end(), a_owner) == s.pastOwners.end())
                s.pastOwners.push_back(a_owner);   // the save sweep still checks this actor (SweepActors)
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
        // The menu display object is a BORROWED form pointer too (the source's), so it is nulled
        // here with the effects (the equip slot is the factory's own, never copied: left alone). The name is NOT touched: it is an
        // owned pool reference (Configure's copy-assign acquired it) that the form's own
        // destructor drops in the purge; dropping it here would be an engine call.
        for (auto& s : g_slot) {
            if (s.form) {
                s.form->effects.clear();
                s.form->menuDispObject = nullptr;
            }
            s = {};
        }
        for (auto& w : g_watch) {
            std::scoped_lock lk(w.mx);
            Disarm(w);
        }
        g_minted.store(0, std::memory_order_relaxed);   // every slot is unminted again (s = {} above)
        // Save / load integrity state: the outgoing world's effect addresses and watched actors mean
        // nothing in the next one. A queued load-watch tick sees the new generation and stops.
        g_rekeyed.clear();
        g_sweptForSave = false;
        g_loadWatch.clear();
        g_loadWatchPosted = false;
        g_loadWatchFull   = false;
        ++g_loadWatchGen;
    }

    void OnOwnerUpdate(RE::Actor* a_actor) {
        if (g_armedWatches.load(std::memory_order_relaxed) == 0 || !a_actor) return;
        const RE::FormID fid = a_actor->GetFormID();
        const std::uint32_t minted = MintedCount();   // a watch is armed only on a minted slot (the prefix)
        for (std::uint32_t i = 0; i < minted && i < kPoolCap; ++i) {
            auto& w = g_watch[i];
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

    // ============================================================================
    // SAVE / LOAD INTEGRITY (INVARIANTS #19; 2026-11-15, the Save373 crash).
    //
    // WHAT HAPPENED. LoreRim Save373 was written while Auri held a live Candlelight
    // delivery-flip proxy (0xFF00081F). The old sweep ran in the co-save callback, which
    // SKSE64 calls from its Papyrus global-data hook, AFTER the actor records are in the
    // buffer, so the save kept the proxy in her left hand and spell list. On the next
    // load that id resolved to whatever form held it then, that form was destroyed, and
    // Immersive Equipment Displays read her left hand 0.8 s after the load: a call
    // through freed memory.
    //
    // WHAT THE ENGINE CALLS DO (disassembly; identical in structure on all three builds):
    //   Actor::DeselectSpell  AE 38769 0x6C4300 / SE 37820 0x6322F0 / 1.7.104 0x6D6DC0
    //     for i in 0..3: if selectedSpells[i] == spell { selectedSpells[i] = 0;
    //       37372(actor, spell) -> 39193(process, spell, slot 0); GetMagicCaster(i)
    //       (vfunc 0x5C) -> 34427, whose 34435 tail writes currentSpell (+0x28) = 0 }
    //     then selectedPower (if it is a SPEL and == spell) = 0.
    //   39193  AE 0x6D86B0 / SE 38233 0x645C50 / 1.7.104 0x6EB260 (AIProcess, form, slot)
    //     erases every AIProcess::equippedForms (+0xA0, count +0xB0, 16-byte {form, slot})
    //     entry of the form. NO entry found: returns WITHOUT reading the hands. Otherwise
    //     (slot 0) nulls equippedObjects[left] (+0xF0) / [right] (+0xF8) when == form.
    //   Actor::RemoveSpell    AE 38717 0x6C14B0 / SE 37772 0x62F720 / 1.7.104 0x6D3F70
    //     looks the spell up in addedSpells; NOT there: returns before any deselect.
    //     There: the same deselect (SE calls 37820), then the array erase.
    // So a hand is cleared only when the matching selectedSpells slot names the spell AND
    // equippedForms has its entry. The sweep therefore re-reads every slot after the
    // engine calls and writes the leftovers itself. Those writes are the engine's own
    // bodies (a pointer store, an array erase), through the fork's members:
    //   Actor runtime data  addedSpells SE +0x188 / AE, 1.7.104 +0x190; magicCasters
    //     +0x1A0 / +0x1A8; selectedSpells +0x1C0 / +0x1C8; selectedPower +0x1E0 / +0x1E8
    //     (GetActorRuntimeData's 0xE0 / 0xE8 base; read off the three DeselectSpell /
    //     RemoveSpell / Character vfunc 0x5C bodies).
    //   AIProcess  equippedForms +0xA0, equippedObjects +0xF0 (the three 39193 bodies).
    //   MagicCaster  currentSpell +0x28 (SaveGame AE 0x5BE8F0 / SE 0x54F2D0 / 1.7.104
    //     0x5CD440 writes it into the save; LoadGame resolves the id and RTDynamicCasts).
    //   ActiveEffect  spell +0x40, effect +0x48: the effect-list save (AE 0x5AD5A0 /
    //     0x5AD7E0) writes spell's form id, then the index of `effect` in spell's own
    //     effect array (11339). A proxy shares its source's Effect* in the source's
    //     order, so the source spell saves the same index.
    // No new address id and no new engine call: the two calls are the ones Unref already
    // makes. Direct writes run only on the three builds the offsets were read on.
    //
    // LIVENESS WITHOUT TRUST (the load repair). TESForm's destructor (AE 0x1E2250) calls
    // 14627 (0x1E06A0), which erases the form's own id entry from the form map. So for
    // any pointer p: read the 4 bytes where its id would be (guarded: freed memory can be
    // unmapped), look that id up, and p is a live form exactly when the map answers p.
    // Nothing else of p is read before that.
    // ============================================================================
    namespace {

        static_assert(offsetof(RE::TESForm, formID) == 0x14, "TESForm::formID moved -- the guarded id read uses +0x14");
        static_assert(offsetof(RE::AIProcess, equippedForms) == 0xA0 && offsetof(RE::AIProcess, equippedObjects) == 0xF0,
                      "AIProcess equip members moved -- 39193 (AE 0x6D86B0 / SE 0x645C50 / 1.7.104 0x6EB260) reads +0xA0 / +0xF0");
        static_assert(sizeof(RE::AIProcess::EquippedObject) == 0x10, "AIProcess::EquippedObject is 16 bytes in 39193");
        static_assert(offsetof(RE::MagicCaster, currentSpell) == 0x28,
                      "MagicCaster::currentSpell moved -- SaveGame (AE 0x5BE8F0) writes +0x28");
        static_assert(offsetof(RE::MagicCaster, state) == 0x30,
                      "MagicCaster::state moved -- InterruptCast (AE 0x5BBFA0 / SE 0x54CB70 / 1.7.104 0x5CAAE0) writes +0x30");
        static_assert(offsetof(RE::ActiveEffect, spell) == 0x40 && offsetof(RE::ActiveEffect, effect) == 0x48,
                      "ActiveEffect::spell / effect moved -- the effect-list save (AE 0x5AD5A0) reads +0x40 / +0x48");

        // Where a spell-slot reference was found (one counter per kind of slot).
        struct Tally {
            std::uint32_t hand = 0, equipList = 0, selected = 0, power = 0, known = 0, caster = 0;
            // Not part of Total() (they qualify `caster`): casters ended by the engine's InterruptCast,
            // casters LEFT holding the form (mid-cast and the InterruptCast row refused), and casters
            // nulled by a direct write while mid-cast (the load repair: no engine call is safe there).
            std::uint32_t interrupted = 0, casterLeft = 0, casterMidCast = 0;
            std::uint32_t Total() const { return hand + equipList + selected + power + known + caster; }
            std::string   Text() const {
                std::string s;
                auto add = [&](std::uint32_t n, const char* what) {
                    if (n != 0) s += fmt::format("{}{} {}", s.empty() ? "" : ", ", n, what);
                };
                add(hand, "hand slot(s)");
                add(equipList, "equipped-list entr(y/ies)");
                add(selected, "selected-spell slot(s)");
                add(power, "selected power");
                add(known, "spell-list entr(y/ies)");
                add(caster, "caster current spell(s)");
                return s.empty() ? "nothing" : s;
            }
        };

        // Which kind of slot a visited pointer sits in (the type each may hold: TypeFits).
        enum class Ref : std::uint8_t { kSelected, kPower, kKnown, kCaster, kEquipList, kHand };

        // What VisitSpellRefs does with a pointer its test picks.
        //   kCount     nothing.
        //   kLivePool  the save sweep: the pointer is a LIVE pool form. Direct member write; a caster
        //              that is mid-cast is ended through the engine's InterruptCast first.
        //   kDirect    the load repair: the pointer is DEAD or a form of the wrong type. Direct member
        //              write only, never an engine call on it.
        enum class Clear : std::uint8_t { kCount, kLivePool, kDirect };

        // MagicCaster::InterruptCast (33630 / 34408; AE 0x5BBFA0, SE 0x54CB70, 1.7.104 0x5CAAE0, the same
        // code on all three): returns at once when currentSpell is null; else InterruptCastImpl (vfunc
        // 8), three sound stops, state = 9, vfunc 9, then the finish function (AE 34435 0x5BEB60 / SE
        // 33657 / 1.7.104 0x5CD6B0), which nulls currentSpell and resets state / timers. That finish
        // function CALLS THROUGH currentSpell (its vtable +0x2A8 / +0x2B8), so InterruptCast is only
        // ever made on a LIVE spell (the save sweep), never on the load repair's dead pointers.
        // The address is the already generated VerifiedAddresses row "PositionCast.InterruptCast".
        bool InterruptSeatOk() {
            static const bool s_ok = apmf::allowance::SeatVerified(
                REL::Relocation<std::uintptr_t>{ RELOCATION_ID(33630, 34408) }.address(), "PositionCast.InterruptCast");
            return s_ok;
        }

        // Visit every pointer of the actor that the save writes as "a spell / equipped form of this
        // actor": selectedSpells[4], selectedPower, addedSpells, each existing caster's currentSpell,
        // the process's equippedForms entries and its two hand slots. `a_test` gets the RAW pointer
        // (it may be dead on the load path: the visitor itself never reads through it) and the kind
        // of slot, and answers "this one". a_clear: see Clear.
        template <class Test>
        void VisitSpellRefs(RE::Actor* a_actor, Clear a_clear, Tally& a_t, Test&& a_test) {
            const bool clear = a_clear != Clear::kCount;
            auto& rd = a_actor->GetActorRuntimeData();
            for (auto& sel : rd.selectedSpells) {
                if (sel && a_test(static_cast<const RE::TESForm*>(sel), Ref::kSelected)) {
                    ++a_t.selected;
                    if (clear) sel = nullptr;
                }
            }
            if (rd.selectedPower && a_test(static_cast<const RE::TESForm*>(rd.selectedPower), Ref::kPower)) {
                ++a_t.power;
                if (clear) rd.selectedPower = nullptr;
            }
            for (std::uint32_t i = 0; i < rd.addedSpells.size();) {
                auto* sp = rd.addedSpells[i];
                if (sp && a_test(static_cast<const RE::TESForm*>(sp), Ref::kKnown)) {
                    ++a_t.known;
                    if (clear) {
                        rd.addedSpells.erase(rd.addedSpells.begin() + i);
                        continue;
                    }
                }
                ++i;
            }
            for (auto* mc : rd.magicCasters) {
                if (!mc || !mc->currentSpell || !a_test(static_cast<const RE::TESForm*>(mc->currentSpell), Ref::kCaster))
                    continue;
                ++a_t.caster;
                if (!clear) continue;
                const bool midCast = mc->state.get() != RE::MagicCaster::State::kNone;
                if (midCast && a_clear == Clear::kLivePool) {
                    // A bare null would leave the cast's state, sounds and timers standing with no spell
                    // (review F3 of f2fa89b). End the cast the engine's way; it nulls currentSpell itself.
                    if (!InterruptSeatOk()) {
                        ++a_t.casterLeft;   // the caller says so (ERROR); the slot is not half-cleared
                        continue;
                    }
                    mc->InterruptCast(false);
                    ++a_t.interrupted;
                } else if (midCast) {
                    ++a_t.casterMidCast;   // the load repair: said by the caller
                }
                if (mc->currentSpell) mc->currentSpell = nullptr;
            }
            if (auto* proc = rd.currentProcess) {
                for (std::uint32_t i = 0; i < proc->equippedForms.size();) {
                    auto* obj = proc->equippedForms[i].object;
                    if (obj && a_test(static_cast<const RE::TESForm*>(obj), Ref::kEquipList)) {
                        ++a_t.equipList;
                        if (clear) {
                            proc->equippedForms.erase(proc->equippedForms.begin() + i);
                            continue;
                        }
                    }
                    ++i;
                }
                for (auto& h : proc->equippedObjects) {
                    if (h && a_test(static_cast<const RE::TESForm*>(h), Ref::kHand)) {
                        ++a_t.hand;
                        if (clear) h = nullptr;
                    }
                }
            }
        }

        // The actors a pass covers, once each, held by reference for its length: every actor of the
        // four process lists, the player, and every actor that owns or earlier owned a minted pool
        // slot (looked up by id: a follower with no AI process is in no process list; review F2 of
        // f2fa89b). NOT covered, and the log lines say "process-list actors + slot owners", never
        // "every actor": an actor outside the process lists that never owned a slot, e.g. an ally
        // that still carries a proxied buff after it left the lists. Main thread.
        std::vector<RE::NiPointer<RE::Actor>> SweepActors() {
            std::vector<RE::NiPointer<RE::Actor>>  out;
            std::unordered_set<const RE::Actor*>   seen;
            auto add = [&](RE::NiPointer<RE::Actor> a_p) {
                if (a_p && seen.insert(a_p.get()).second) out.push_back(std::move(a_p));
            };
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                for (auto* arr : { &pl->highActorHandles, &pl->middleHighActorHandles, &pl->middleLowActorHandles,
                                   &pl->lowActorHandles })
                    for (auto& h : *arr) add(h.get());
            }
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) add(RE::NiPointer<RE::Actor>(pc));
            auto addId = [&](RE::FormID a_id) {
                if (auto* a = a_id ? RE::TESForm::LookupByID<RE::Actor>(a_id) : nullptr) add(RE::NiPointer<RE::Actor>(a));
            };
            for (std::uint32_t i = 0; i < MintedCount(); ++i) {
                addId(g_slot[i].owner);
                for (const auto o : g_slot[i].pastOwners) addId(o);
            }
            return out;
        }

        // The minted pool slot whose form is this pointer, or -1. Pointer compares only.
        int SlotOf(const RE::TESForm* a_form) {
            if (!a_form) return -1;
            for (std::uint32_t i = 0; i < MintedCount(); ++i)
                if (static_cast<const RE::TESForm*>(g_slot[i].form) == a_form) return static_cast<int>(i);
            return -1;
        }

        std::string PoolFormsText() {
            std::string s;
            for (std::uint32_t i = 0; i < MintedCount(); ++i)
                if (g_slot[i].form) s += fmt::format("{}0x{}", s.empty() ? "" : ", ", apmf::log::Hex(g_slot[i].form->GetFormID()));
            return s;
        }

        // The real spell that owns `a_effect`: the slot's current source, else one it mirrored
        // earlier this session (a re-pointed slot's old effect, APMF-B58). nullptr = none of them.
        RE::SpellItem* SourceOf(const Slot& a_s, const RE::Effect* a_effect) {
            if (!a_effect) return nullptr;
            auto owns = [&](RE::FormID a_id) -> RE::SpellItem* {
                auto* sp = a_id ? RE::TESForm::LookupByID<RE::SpellItem>(a_id) : nullptr;
                if (!sp) return nullptr;
                for (auto* e : sp->effects)
                    if (e == a_effect) return sp;
                return nullptr;
            };
            if (auto* sp = owns(a_s.source)) return sp;
            for (auto it = a_s.past.rbegin(); it != a_s.past.rend(); ++it)
                if (auto* sp = owns(*it)) return sp;
            return nullptr;
        }

        // ---- load repair ----
        const char* RefName(Ref a_k) {
            switch (a_k) {
            case Ref::kSelected:  return "selected-spell slot";
            case Ref::kPower:     return "selected power";
            case Ref::kKnown:     return "spell-list entry";
            case Ref::kCaster:    return "caster current spell";
            case Ref::kEquipList: return "equipped-list entry";
            default:              return "hand slot";
            }
        }

        // May a slot of this kind hold a form of this type? Asked only of a pointer already proven
        // to be a live form (review F5 of f2fa89b: "the map answers this pointer" also holds when the
        // dead form's address was reused by a new form, and the load resolves a saved 0xFF id to
        // whatever form carries it then, e.g. a placed reference).
        //   selectedSpells[] and MagicCaster::currentSpell are MagicItem*: the five MagicItem classes
        //     (ENCH, SPEL, SCRL, ALCH, INGR). The caster's own LoadGame applies this very test
        //     (RTDynamicCast TESForm -> MagicItem, AE 0x5BE9F0).
        //   addedSpells is SpellItem*: SPEL, and SCRL (ScrollItem derives from SpellItem).
        //   selectedPower is TESForm*: a power (SPEL) or a shout (SHOU); DeselectSpell tests it for
        //     SPEL before the compare.
        //   equippedForms / the two hand slots are TESForm*. Their writer is 39192 (AE 0x6D85F0:
        //     append {form, BGSEquipSlot*}; a left / right hand slot also writes the hand), called by
        //     the three Actor setters 38928 / 38929 / 38930 with the slot the form equips to (two
        //     more callers, 24736 and 39430, not read further). So the set is the form classes that carry a
        //     BGSEquipType (WEAP, ARMO, LIGH, SPEL, SCRL, SHOU, ALCH, INGR), plus AMMO kept as allowed
        //     because a wrong "cannot hold" costs an unequip. The set is deliberately the wide one:
        //     it exists to refuse references, cells and other non-equippable forms, not to police
        //     what a hand holds.
        bool TypeFits(Ref a_k, RE::FormType a_t) {
            using T = RE::FormType;
            const bool magic = a_t == T::Spell || a_t == T::Scroll || a_t == T::Enchantment || a_t == T::AlchemyItem ||
                               a_t == T::Ingredient;
            switch (a_k) {
            case Ref::kSelected:
            case Ref::kCaster: return magic;
            case Ref::kKnown:  return a_t == T::Spell || a_t == T::Scroll;
            case Ref::kPower:  return a_t == T::Spell || a_t == T::Shout;
            default:
                return a_t == T::Weapon || a_t == T::Armor || a_t == T::Light || a_t == T::Spell || a_t == T::Scroll ||
                       a_t == T::Shout || a_t == T::AlchemyItem || a_t == T::Ingredient || a_t == T::Ammo;
            }
        }

        enum class Life : std::uint8_t { kLive, kLiveRuntime, kDead, kUnverifiable };

        // The 4 bytes where a form keeps its id, read through a pointer that may be dead. Freed
        // memory can be unmapped, so the read is guarded. (No C++ object lives in this function.)
        bool ReadFormIdGuarded(const RE::TESForm* a_p, RE::FormID& a_out) noexcept {
            __try {
                a_out = *reinterpret_cast<const volatile RE::FormID*>(reinterpret_cast<const std::uint8_t*>(a_p) + 0x14);
                return true;
            } __except (1) {   // EXCEPTION_EXECUTE_HANDLER
                a_out = 0;
                return false;
            }
        }

        // See "LIVENESS WITHOUT TRUST" above. kUnverifiable: the memory reads id 0, which the form
        // map never holds -- a form that was never registered cannot be told from garbage, so it is
        // left alone and said.
        Life Classify(const RE::TESForm* a_p, RE::FormID& a_idRead) {
            if (!ReadFormIdGuarded(a_p, a_idRead)) return Life::kDead;
            if (a_idRead == 0) return Life::kUnverifiable;
            if (RE::TESForm::LookupByID(a_idRead) != a_p) return Life::kDead;
            return a_idRead >= 0xFF000000 ? Life::kLiveRuntime : Life::kLive;
        }

        struct LoadScan {
            Tally         dead;                  // cleared
            std::uint32_t liveRuntime  = 0;      // live 0xFF forms in a slot (watched)
            std::uint32_t unverifiable = 0;
            std::uint32_t deadEffects  = 0;      // reported only (never counted by the watch)
            std::uint32_t wrongType    = 0;      // of `dead`: live forms of a type the slot cannot hold
            std::string   deadText, runtimeText;
        };

        void ScanActorAfterLoad(RE::Actor* a_actor, const char* a_when, bool a_quiet, LoadScan& a_r,
                                std::unordered_map<const RE::TESForm*, Life>& a_memo) {
            auto classify = [&](const RE::TESForm* a_p, RE::FormID& a_id) {
                if (auto it = a_memo.find(a_p); it != a_memo.end() && it->second == Life::kLive) return Life::kLive;
                const Life l = Classify(a_p, a_id);
                a_memo[a_p]  = l;
                return l;
            };
            LoadScan r;
            VisitSpellRefs(a_actor, Clear::kDirect, r.dead, [&](const RE::TESForm* a_p, Ref a_kind) {
                RE::FormID id   = 0;
                const Life life = classify(a_p, id);
                // Proven live: only now is anything but the id read. A live form of a type this slot
                // cannot hold is the dead form's ADDRESS (or, at the load itself, its ID) reused by
                // another form: treated as dead (review F5 of f2fa89b).
                if (life == Life::kLive || life == Life::kLiveRuntime) {
                    const auto type = a_p->GetFormType();
                    if (!TypeFits(a_kind, type)) {
                        ++r.wrongType;
                        r.deadText += fmt::format("{}{} (a LIVE form 0x{} of type 0x{}, which a {} cannot hold: a reused "
                                                  "id or address)",
                                                  r.deadText.empty() ? "" : ", ", fmt::ptr(a_p),
                                                  apmf::log::Hex(a_p->GetFormID()),
                                                  apmf::log::Hex(static_cast<std::uint32_t>(type)), RefName(a_kind));
                        return true;
                    }
                }
                switch (life) {
                case Life::kDead:
                    r.deadText += fmt::format("{}{} (its memory reads id 0x{})", r.deadText.empty() ? "" : ", ",
                                              fmt::ptr(a_p), apmf::log::Hex(id));
                    return true;
                case Life::kLiveRuntime:
                    ++r.liveRuntime;
                    r.runtimeText += fmt::format("{}0x{}", r.runtimeText.empty() ? "" : ", ", apmf::log::Hex(a_p->GetFormID()));
                    return false;
                case Life::kUnverifiable:
                    ++r.unverifiable;
                    return false;
                default:
                    return false;
                }
            });
            // Active effects are reported, never repaired, so the watch (which re-runs this scan on
            // every pump with no expiry) does not walk them: one report per load pass is the whole
            // value, and a per-pump ERROR would flood the log (review F1 of f2fa89b).
            if (!a_quiet) {
                if (auto* list = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (!ae || !ae->spell) continue;
                        RE::FormID id = 0;
                        if (classify(static_cast<const RE::TESForm*>(ae->spell), id) == Life::kDead) ++r.deadEffects;
                    }
                }
            }
            const char* name = a_actor->GetName();
            if (r.dead.Total() != 0)
                spdlog::warn("[castproxy] LOAD REPAIR ({}): 0x{} \"{}\" held pointer(s) to DESTROYED form(s){} -- {} -- "
                             "CLEARED: {}{}. (A save written with a runtime form in the slot, INVARIANTS #19; the "
                             "next reader of that slot would have crashed.)",
                             a_when, apmf::log::Hex(a_actor->GetFormID()), name ? name : "",
                             r.wrongType != 0 ? fmt::format(" ({} of them a live form of a type the slot cannot hold)", r.wrongType)
                                              : "",
                             r.deadText, r.dead.Text(),
                             r.dead.casterMidCast != 0
                                 ? fmt::format("; {} caster(s) were MID-CAST on it: the spell pointer is nulled by a direct "
                                               "write and the caster's cast state is left (no engine call is safe on a "
                                               "dead spell)", r.dead.casterMidCast)
                                 : "");
            if (!a_quiet && r.deadEffects != 0)
                spdlog::error("[castproxy] load check ({}): 0x{} \"{}\" carries {} active effect(s) whose spell pointer is "
                              "a DESTROYED form -- NOT repaired (the effect's own Effect* died with that form; no engine "
                              "path is safe on it). The game may crash when that effect updates.",
                              a_when, apmf::log::Hex(a_actor->GetFormID()), name ? name : "", r.deadEffects);
            if (!a_quiet && r.liveRuntime != 0)
                spdlog::info("[castproxy] load check ({}): 0x{} \"{}\" holds live runtime form(s) [{}] in {} spell slot(s) -- "
                             "not touched (another mod's form may be legitimate); watched on every pump until it leaves "
                             "the slot or is destroyed.",
                             a_when, apmf::log::Hex(a_actor->GetFormID()), name ? name : "", r.runtimeText, r.liveRuntime);
            if (!a_quiet && r.unverifiable != 0)
                spdlog::warn("[castproxy] load check ({}): 0x{} \"{}\" has {} spell-slot pointer(s) whose memory reads form "
                             "id 0 -- a form outside the form map cannot be verified; left alone.",
                             a_when, apmf::log::Hex(a_actor->GetFormID()), name ? name : "", r.unverifiable);
            a_r.dead.hand      += r.dead.hand;
            a_r.dead.equipList += r.dead.equipList;
            a_r.dead.selected  += r.dead.selected;
            a_r.dead.power     += r.dead.power;
            a_r.dead.known     += r.dead.known;
            a_r.dead.caster    += r.dead.caster;
            a_r.liveRuntime    += r.liveRuntime;
            a_r.unverifiable   += r.unverifiable;
            a_r.deadEffects    += r.deadEffects;
        }

        void LoadWatchTick(std::uint32_t a_gen) {
            if (a_gen != g_loadWatchGen) return;   // a tick queued before a world swap
            g_loadWatchPosted = false;
            std::unordered_map<const RE::TESForm*, Life> memo;
            for (std::size_t i = 0; i < g_loadWatch.size();) {
                auto*    actor = RE::TESForm::LookupByID<RE::Actor>(g_loadWatch[i]);
                LoadScan r;
                if (actor) ScanActorAfterLoad(actor, "watch", true, r, memo);
                if (!actor || r.liveRuntime == 0) {
                    spdlog::info("[castproxy] load watch: 0x{} dropped ({}).", apmf::log::Hex(g_loadWatch[i]),
                                 !actor ? "the actor is gone"
                                 : r.dead.Total() != 0 ? "its runtime form was destroyed; repaired above"
                                                       : "no runtime form is in its spell slots any more");
                    g_loadWatch.erase(g_loadWatch.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
            }
            if (!g_loadWatch.empty()) {
                g_loadWatchPosted = true;
                apmf::mainthread::Post([a_gen] { LoadWatchTick(a_gen); });
            }
        }

        // Count (never change) what a save taken now would capture: spell-slot references to pool
        // forms and active effects keyed to one. The co-save callback's second check.
        struct SaveCount {
            std::uint32_t actors = 0, holders = 0, refs = 0, effects = 0;
            std::string   who;
        };
        SaveCount CountPoolRefs() {
            SaveCount c;
            for (auto& ap : SweepActors()) {
                auto* actor = ap.get();
                ++c.actors;
                Tally t;
                VisitSpellRefs(actor, Clear::kCount, t, [](const RE::TESForm* a_p, Ref) { return SlotOf(a_p) >= 0; });
                std::uint32_t fx = 0;
                if (auto* list = actor->AsMagicTarget()->GetActiveEffectList())
                    for (auto* ae : *list)
                        if (ae && SlotOf(ae->spell) >= 0) ++fx;
                if (t.Total() == 0 && fx == 0) continue;
                ++c.holders;
                c.refs    += t.Total();
                c.effects += fx;
                c.who     += fmt::format("{}0x{} ({}{}{} active effect(s))", c.who.empty() ? "" : "; ",
                                         apmf::log::Hex(actor->GetFormID()), t.Total() != 0 ? t.Text() : "",
                                         t.Total() != 0 ? ", " : "", fx);
            }
            return c;
        }

    }

    void PreSaveSweep() {
        if (!g_rekeyed.empty()) {
            // A kSaveGame whose co-save callback never came (the save did not complete). Those effects
            // stay keyed to their source spell: valid, only the seats' "already applied" bound no
            // longer sees them as the proxy's.
            spdlog::warn("[castproxy] save sweep: {} active effect(s) re-keyed for an earlier save were never keyed back "
                         "(its co-save callback did not run) -- they stay keyed to their source spell.",
                         g_rekeyed.size());
            g_rekeyed.clear();
        }
        g_sweptForSave = true;
        if (MintedCount() == 0) return;   // no runtime form exists: nothing a save could capture

        const bool    direct = apmf::allowance::RuntimeSupported();
        std::uint32_t actors = 0, holders = 0, refs = 0, directRefs = 0, effects = 0, unresolved = 0, castersLeft = 0;
        for (auto& ap : SweepActors()) {
            auto* actor = ap.get();
            ++actors;
            const char* name = actor->GetName();

            // 1. Which pool forms does any of this actor's spell slots name?
            Tally                        before;
            std::vector<RE::SpellItem*>  forms;
            VisitSpellRefs(actor, Clear::kCount, before, [&](const RE::TESForm* a_p, Ref) {
                const int i = SlotOf(a_p);
                if (i < 0) return false;
                if (std::find(forms.begin(), forms.end(), g_slot[i].form) == forms.end()) forms.push_back(g_slot[i].form);
                return true;
            });
            if (before.Total() != 0) {
                ++holders;
                refs += before.Total();
                // 2. The engine's own paths (the calls Unref makes).
                for (auto* f : forms) {
                    actor->DeselectSpell(f);
                    actor->RemoveSpell(f);
                }
                // 3. Re-read every slot; write what the engine left (see the disassembly above).
                Tally left;
                VisitSpellRefs(actor, direct ? Clear::kLivePool : Clear::kCount, left,
                               [](const RE::TESForm* a_p, Ref) { return SlotOf(a_p) >= 0; });
                if (left.Total() == 0) {
                    spdlog::info("[castproxy] save sweep: 0x{} \"{}\" referenced {} pool form(s) ({}) -- cleared by "
                                 "DeselectSpell + RemoveSpell.",
                                 apmf::log::Hex(actor->GetFormID()), name ? name : "", forms.size(), before.Text());
                } else if (direct) {
                    directRefs += left.Total() - left.casterLeft;
                    spdlog::warn("[castproxy] save sweep: 0x{} \"{}\" referenced {} pool form(s) ({}); DeselectSpell + "
                                 "RemoveSpell LEFT {} -- cleared by direct member write{}.",
                                 apmf::log::Hex(actor->GetFormID()), name ? name : "", forms.size(), before.Text(),
                                 left.Text(),
                                 left.interrupted != 0
                                     ? fmt::format(" ({} caster(s) were mid-cast: ended by InterruptCast first)", left.interrupted)
                                     : "");
                    if (left.casterLeft != 0) {
                        castersLeft += left.casterLeft;
                        spdlog::error("[castproxy] save sweep: 0x{} \"{}\": {} caster(s) are MID-CAST on a pool form and the "
                                      "InterruptCast address did not verify on this build -- the caster is LEFT AS IT IS "
                                      "(a bare null would leave a cast running with no spell): THE SAVE WILL HOLD A "
                                      "RUNTIME FORM in that caster.",
                                      apmf::log::Hex(actor->GetFormID()), name ? name : "", left.casterLeft);
                    }
                } else {
                    spdlog::error("[castproxy] save sweep: 0x{} \"{}\" STILL references a pool form after DeselectSpell + "
                                  "RemoveSpell ({}), and the direct repair is refused on this runtime (offsets proven on "
                                  "1.6.1170 / 1.5.97 / 1.7.104 only) -- THE SAVE WILL HOLD A RUNTIME FORM.",
                                  apmf::log::Hex(actor->GetFormID()), name ? name : "", left.Text());
                }
            }

            // 4. Active effects keyed to a pool form: key them to the real source spell for the save.
            if (auto* list = actor->AsMagicTarget()->GetActiveEffectList()) {
                for (auto* ae : *list) {
                    if (!ae) continue;
                    const int si = SlotOf(ae->spell);
                    if (si < 0) continue;
                    auto* src = SourceOf(g_slot[si], ae->effect);
                    if (!src) {
                        ++unresolved;
                        spdlog::error("[castproxy] save sweep: 0x{} \"{}\" carries an active effect of pool form 0x{} whose "
                                      "Effect belongs to none of the spells that form mirrored this session -- NOT "
                                      "re-keyed: THE SAVE WILL HOLD A RUNTIME FORM in that effect.",
                                      apmf::log::Hex(actor->GetFormID()), name ? name : "",
                                      apmf::log::Hex(g_slot[si].form->GetFormID()));
                        continue;
                    }
                    g_rekeyed.push_back({ actor->GetFormID(), ae, g_slot[si].form, src });
                    ae->spell = src;
                    ++effects;
                }
            }
        }
        spdlog::info("[castproxy] save sweep (kSaveGame, before the engine writes; on the Drain thread: {}): {} actor(s) checked "
                     "(the process lists, the player, every current or earlier slot owner) against {} minted pool form(s) "
                     "[{}] -- {} spell-slot reference(s) on {} actor(s) cleared ({} by direct write, {} mid-cast caster(s) "
                     "LEFT), {} active effect(s) keyed to their source spell for the save ({} unresolved). ReteachLive "
                     "re-teaches live claims on the next pump.",
                     apmf::hook::OnMainThread() ? "yes" : "NO", actors, MintedCount(), PoolFormsText(), refs, holders,
                     directRefs, castersLeft, effects, unresolved);
    }

    bool SaveWrittenCheck() {
        const bool swept = g_sweptForSave;
        g_sweptForSave   = false;
        if (MintedCount() == 0) {
            g_rekeyed.clear();   // cannot hold anything: a re-key needs a minted form
            return false;
        }
        bool late = false;
        if (!swept) {
            // This SKSE build ran the co-save callback without a kSaveGame message first. The actor
            // records are already written, so sweeping now protects nothing in THIS save; it is done
            // anyway so the state matches every other save, and said loudly.
            spdlog::warn("[castproxy] save check: the co-save callback ran with NO kSaveGame message before it -- the save "
                         "sweep is running LATE (after the actor records). THE .ESS MAY ALREADY HOLD a runtime form.");
            PreSaveSweep();
            g_sweptForSave = false;
            late           = true;
        }
        const SaveCount c = CountPoolRefs();
        if (c.refs != 0 || c.effects != 0)
            spdlog::warn("[castproxy] save check (co-save callback, after the actor records): {} spell-slot reference(s) and "
                         "{} active effect(s) STILL name a pool form [{}] on {} actor(s): {} -- THE .ESS MAY ALREADY HOLD "
                         "IT (INVARIANTS #19). Something re-taught or re-equipped it between kSaveGame and the write.",
                         c.refs, c.effects, PoolFormsText(), c.holders, c.who);
        // Key the effects back to their proxy (their record is written). Each one is found again on
        // its target's own list by address, and must still be keyed to the source this sweep wrote.
        std::uint32_t back = 0, gone = 0;
        for (const auto& r : g_rekeyed) {
            bool done = false;
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(r.target); actor && SlotOf(r.proxy) >= 0) {
                if (auto* list = actor->AsMagicTarget()->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (ae != r.ae) continue;
                        if (ae->spell == r.source) {
                            ae->spell = r.proxy;
                            done      = true;
                        }
                        break;
                    }
                }
            }
            done ? ++back : ++gone;
        }
        g_rekeyed.clear();
        if (c.refs == 0 && c.effects == 0)
            spdlog::info("[castproxy] save check (co-save callback, after the actor records): on the {} actor(s) checked "
                         "(the process lists, the player, every current or earlier slot owner) no spell slot and no "
                         "active effect named any of the {} minted pool form(s) [{}] when the records were written -- "
                         "CLEAN for those. An actor outside the process lists that never owned a slot is NOT checked. "
                         "{} active effect(s) keyed back to their proxy{}.",
                         c.actors, MintedCount(), PoolFormsText(), back,
                         gone != 0 ? fmt::format(" ({} no longer on their target: left as they are)", gone) : "");
        else if (back != 0 || gone != 0)
            spdlog::info("[castproxy] save check: {} active effect(s) keyed back to their proxy, {} no longer on their "
                         "target.", back, gone);
        return late;
    }

    void RepairAfterLoad(const char* a_when) {
        if (!apmf::allowance::RuntimeSupported()) {
            static bool s_said = false;   // main thread
            if (!s_said) {
                s_said = true;
                spdlog::warn("[castproxy] load repair REFUSED on this runtime: the actor / process member offsets are proven "
                             "on 1.6.1170, 1.5.97 and 1.7.104 only. A save holding a runtime form in a hand is not "
                             "repaired here.");
            }
            return;
        }
        LoadScan                                      total;
        std::unordered_map<const RE::TESForm*, Life>  memo;
        std::uint32_t                                 actors = 0, repaired = 0, watched = 0;
        for (auto& ap : SweepActors()) {
            auto* actor = ap.get();
            ++actors;
            LoadScan r;
            ScanActorAfterLoad(actor, a_when, false, r, memo);
            if (r.dead.Total() != 0) ++repaired;
            if (r.liveRuntime != 0) {
                const auto id = actor->GetFormID();
                if (std::find(g_loadWatch.begin(), g_loadWatch.end(), id) == g_loadWatch.end()) {
                    if (g_loadWatch.size() < kLoadWatchCap) {
                        g_loadWatch.push_back(id);
                    } else if (!g_loadWatchFull) {
                        g_loadWatchFull = true;
                        spdlog::warn("[castproxy] load watch is FULL ({} actors hold a live runtime form in a spell slot) -- "
                                     "0x{} and any further actor are NOT watched: a form of theirs destroyed later is not "
                                     "repaired.",
                                     kLoadWatchCap, apmf::log::Hex(id));
                    }
                }
                ++watched;
            }
            total.dead.hand      += r.dead.hand;
            total.dead.equipList += r.dead.equipList;
            total.dead.selected  += r.dead.selected;
            total.dead.power     += r.dead.power;
            total.dead.known     += r.dead.known;
            total.dead.caster    += r.dead.caster;
            total.deadEffects    += r.deadEffects;
            total.unverifiable   += r.unverifiable;
        }
        spdlog::info("[castproxy] load repair ({}; on the Drain thread: {}): {} actor(s) checked -- {} pointer(s) to destroyed or wrong-type forms "
                     "cleared on {} actor(s){}{}; {} active effect(s) with a destroyed spell reported; {} actor(s) hold a live "
                     "runtime form in a spell slot ({} watched).",
                     a_when, apmf::hook::OnMainThread() ? "yes" : "NO", actors, total.dead.Total(), repaired,
                     total.dead.Total() != 0 ? ": " : "", total.dead.Total() != 0 ? total.dead.Text() : "",
                     total.deadEffects, watched, g_loadWatch.size());
        if (!g_loadWatch.empty() && !g_loadWatchPosted) {
            g_loadWatchPosted = true;
            const auto gen    = g_loadWatchGen;
            apmf::mainthread::Post([gen] { LoadWatchTick(gen); });
        }
    }

    void ReteachLive() {
        for (std::uint32_t i = 0; i < MintedCount(); ++i) {
            auto& s = g_slot[i];
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
