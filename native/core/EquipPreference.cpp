#include "PCH.h"
#include "core/EquipPreference.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"          // Categorize: the one category map (hand bits of an item)
#include "core/FacetAttribution.h"   // Outranks / InsideSource / CastClaimHandle: the priority sliders
#include "core/HandBlock.h"          // HeldFor: a hand a live cast claim holds (D3)
#include "core/Log.h"
#include "core/Sources.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

// See core/EquipPreference.h.

namespace apmf::equippref {

    namespace {

        // ---- the engine layout this file reads (header comment: disassembled on all three builds) ----
        static_assert(offsetof(RE::CombatController, inventory) == 0x10,
                      "CombatController::inventory moved -- AE 0x558A80 / SE 0x4FD3EB read it at +0x10");
        static_assert(offsetof(RE::CombatInventory, parentController) == 0x00,
                      "CombatInventory::parentController moved -- AE 0x80F40B reads [inv] as the controller");
        static_assert(offsetof(RE::CombatInventory, inventoryItems) == 0x08,
                      "CombatInventory::inventoryItems moved -- AE 0x80F3E2 walks inv+8 in 0x18 strides");
        static_assert(sizeof(RE::BSTArray<RE::NiPointer<RE::CombatInventoryItem>>) == 0x18,
                      "BSTArray stride is not 0x18 -- the engine walks inventoryItems in 0x18 strides");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10, "CombatInventoryItem::item moved");
        static_assert(offsetof(RE::CombatInventoryItem, itemScore) == 0x18,
                      "CombatInventoryItem::itemScore moved -- AE 0x80F415 stores the 0x0C answer at +0x18");
        static_assert(offsetof(RE::CombatInventoryItem, itemSlot) == 0x20, "CombatInventoryItem::itemSlot moved");

        constexpr int           kCategories   = 7;     // CombatInventory::inventoryItems[7], the loop count 7 at AE 0x80F3E6
        constexpr std::uint32_t kMaxListItems = 512;   // a sanity bound on one category list (never seen above a few dozen)
        constexpr std::size_t   kStateCap     = 4096;
        constexpr std::size_t   kDeclCap      = 4096;
        constexpr std::uint64_t kLineHeartbeatMs = 15000;
        constexpr std::uint64_t kHeartbeatMs     = 30000;
        constexpr float         kLogChangeRel    = 0.05f;   // re-log a steady lift only past the engine's own +-2% wobble

        // ---- arming ----
        std::atomic<bool>               g_steer{ false };
        std::atomic<bool>               g_armedKnown{ false };   // Arm ran (kDataLoaded)
        std::atomic<int>                g_weaponSeats{ 0 };
        std::atomic<int>                g_magicSeats{ 0 };
        std::atomic<RE::BGSEquipSlot*>  g_leftHand{ nullptr };
        std::atomic<RE::BGSEquipSlot*>  g_rightHand{ nullptr };

        // ---- the declarations: copy-on-write, published (the ControlMap RCU shape, INVARIANTS #12) ----
        struct Decl {
            std::uint32_t                 count = 0;
            APMF_API::APMF_EquipPrefEntry e[APMF_API::kMaxEquipPreference]{};
        };
        using DeclMap = std::unordered_map<APMF_API::Handle, Decl>;
        std::mutex                                    g_writeMx;   // writers only (any thread); never held by a reader
        std::atomic<std::shared_ptr<const DeclMap>>   g_pub{ std::make_shared<const DeclMap>() };
        std::atomic<std::uint32_t>                    g_declCount{ 0 };   // relaxed pre-gate: published declarations

        // F4 (review of 7cda290): the header's contract. A declaration is accepted only for a handle a
        // kIntent_EquipPreference request returned (NoteClaim, from ClientAPI), is TYPE-CHECKED on the
        // main thread before the seats ever see it (Pump -> ApplyPending: the form lookup takes the
        // engine's forms-map lock, so never on a client or combat thread), and is DROPPED when its
        // claim ends by ANY road (client Release -> Forget; Harbinger releasing it, an unload sweep,
        // a revert -> the 1 s Prune, which asks ControlMap::IsClaimLive). All under g_writeMx.
        struct Known { std::uint64_t requestedMs = 0; bool everLive = false; };
        std::unordered_map<APMF_API::Handle, Known>   g_known;     // live-or-pending ch.26 handles
        std::unordered_map<APMF_API::Handle, Decl>    g_pending;   // declared, not yet type-checked
        std::atomic<bool>                             g_anyPending{ false };
        std::uint64_t                                 g_lastPruneMs = 0;   // main thread only (Pump)
        constexpr std::uint64_t kPruneEveryMs   = 1000;
        constexpr std::uint64_t kNeverLiveGrace = 10000;   // a request is drained next frame; 10 s is generous

        // ---- per-actor state (GetState) + the line dedup: one LEAF mutex, never held across an engine call ----
        struct ActorState {
            APMF_API::Handle owner      = APMF_API::kInvalidHandle;
            std::uint32_t    state      = APMF_API::kEquipPrefState_None;
            RE::FormID       lastForm   = 0;
            std::uint32_t    lastSlot   = APMF_API::kEquipSlot_Default;
            float            engine     = 0.0f;
            float            rival      = 0.0f;
            float            result     = 0.0f;
            std::uint64_t    lastMs     = 0;
            std::uint32_t    lifts = 0, withheld = 0, forcedDenies = 0, forceAnomalies = 0;
            std::uint32_t    lostPasses = 0;   // F7: a lift that did not top the engine's list the next pass
        };
        struct LineState {
            std::uint64_t ms = 0; std::uint32_t state = 0; float result = 0.0f;
            std::uint32_t lastScored = 0;   // the state of the previous scoring, logged or not (F7)
        };
        std::mutex                                       g_stMx;
        std::unordered_map<RE::FormID, ActorState>       g_state;
        std::unordered_map<std::uint64_t, LineState>     g_lines;

        // ---- totals (relaxed; the heartbeat and the menu) ----
        std::atomic<std::uint64_t> g_lifts{ 0 }, g_castSteers{ 0 }, g_leading{ 0 }, g_zero{ 0 }, g_held{ 0 },
            g_withheld{ 0 }, g_unreadable{ 0 }, g_forcedDenies{ 0 }, g_forceAnomalies{ 0 }, g_forceOwn{ 0 },
            g_lostPasses{ 0 }, g_ignoredForms{ 0 }, g_droppedDecls{ 0 };
        std::uint64_t g_lastHeartbeatMs = 0;   // main thread only (Pump)
        Totals        g_lastBeat{};            // main thread only (Pump)

        std::shared_ptr<const DeclMap> Load() { return g_pub.load(std::memory_order_acquire); }

        void Publish(std::shared_ptr<const DeclMap> a_map) {
            std::uint32_t n = 0;
            for (const auto& [h, d] : *a_map) n += d.count ? 1u : 0u;
            g_pub.store(std::move(a_map), std::memory_order_release);
            g_declCount.store(n, std::memory_order_relaxed);
        }

        char HandOf(const RE::BGSEquipSlot* a_slot) {
            if (!a_slot) return '?';
            if (a_slot == g_rightHand.load(std::memory_order_acquire)) return 'R';
            if (a_slot == g_leftHand.load(std::memory_order_acquire)) return 'L';
            return '?';
        }

        std::uint32_t SlotOf(char a_hand) {
            return a_hand == 'R' ? APMF_API::kEquipSlot_Right : a_hand == 'L' ? APMF_API::kEquipSlot_Left
                                                                              : APMF_API::kEquipSlot_Default;
        }

        bool EntryCovers(const APMF_API::APMF_EquipPrefEntry& a_e, char a_hand) {
            switch (a_e.slot) {
            case APMF_API::kEquipSlot_Right: return a_hand == 'R';
            case APMF_API::kEquipSlot_Left:  return a_hand == 'L';
            default:                         return true;   // Default (either hand); Set() normalised anything else
            }
        }

        // Everything one scoring needs to know about what the actor's claims lift, read ONCE.
        struct Views {
            APMF_API::Handle prefHandle = APMF_API::kInvalidHandle;
            const Decl*      decl       = nullptr;   // into `map`, kept alive by it
            std::shared_ptr<const DeclMap> map;
            RE::FormID drivenR = 0, drivenL = 0, drivenU = 0;   // forms live cast claims drive, per hand
        };

        RE::FormID Driven(RE::FormID a_actor, apmf::CastHand a_hand) {
            apmf::CastSeatClaim seat{};
            if (!apmf::ControlMap::Get().TryGetCastSeatClaimForHand(a_actor, a_hand, seat)) return 0;
            return seat.proxy ? seat.proxy : seat.spell;
        }

        // The rank a claim of this actor lifts (form, hand) at: 0..255, or -1 = not lifted.
        int RankOf(const Views& a_v, RE::FormID a_form, char a_hand) {
            if (!a_form) return -1;
            int r = -1;
            if ((a_hand == 'R' && a_form == a_v.drivenR) || (a_hand == 'L' && a_form == a_v.drivenL) ||
                (a_hand == '?' && a_form == a_v.drivenU))
                r = 0;
            if (a_v.decl) {
                for (std::uint32_t i = 0; i < a_v.decl->count; ++i) {
                    const auto& e = a_v.decl->e[i];
                    if (e.form != a_form || !EntryCovers(e, a_hand)) continue;
                    r = r < 0 ? e.rank : std::min<int>(r, e.rank);
                }
            }
            return r;
        }

        // The best score the engine holds for another item of `a_this`'s category list that this
        // actor's claims do not lift at an equal or better rank. false = the list could not be
        // validated (the seat then does not lift).
        // F7: `a_topIsRival` -- the list's FIRST item is a competitor (not this item, not one lifted at an
        // equal or better rank). The rescore loop scores every item BEFORE it re-sorts the list (AE
        // 0x80F444 after the 0x80F400 loop), and the sort is descending, so list[0] is still the previous
        // pass's winner: if this item was lifted then, it lost that pass.
        bool BestRival(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc, const Views& a_v, int a_rank,
                       float& a_best, int& a_cat, bool& a_topIsRival) {
            a_best        = 0.0f;
            a_cat         = -1;
            a_topIsRival  = false;
            auto* inv = a_cc->inventory;
            if (!inv || inv->parentController != a_cc) return false;   // not the list this controller rescores
            const int cat = static_cast<int>(a_this->GetCategory());   // vfunc 0x0B, int(this), a leaf (header)
            if (cat < 0 || cat >= kCategories) return false;
            a_cat = cat;
            const auto&         list = inv->inventoryItems[cat];
            const std::uint32_t n    = list.size();
            if (n > kMaxListItems) return false;
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::CombatInventoryItem* const p = list[i].get();
                if (!p || p == a_this) continue;
                const RE::FormID form = p->item ? p->item->GetFormID() : 0;
                const int        r    = RankOf(a_v, form, HandOf(p->itemSlot.equipSlot));
                if (r >= 0 && r <= a_rank) continue;   // lifted at an equal or better rank: not a competitor
                if (i == 0) a_topIsRival = true;
                if (p->itemScore > a_best) a_best = p->itemScore;
            }
            return true;
        }

        const char* StateName(std::uint32_t s) {
            switch (s) {
            case APMF_API::kEquipPrefState_Declared:   return "declared";
            case APMF_API::kEquipPrefState_Lifted:     return "LIFTED";
            case APMF_API::kEquipPrefState_Leading:    return "leading (no lift needed)";
            case APMF_API::kEquipPrefState_ZeroScore:  return "engine score 0 (never lifted)";
            case APMF_API::kEquipPrefState_HandHeld:   return "hand held by a cast claim (D3: not lifted)";
            case APMF_API::kEquipPrefState_Outranked:  return "OUTRANKED (the source inside the seat keeps its answer)";
            case APMF_API::kEquipPrefState_Unreadable: return "engine list unreadable (not lifted)";
            case APMF_API::kEquipPrefState_Released:   return "released";
            default:                                   return "none";
            }
        }

        // Record one scoring (after every engine read) and say whether it deserves a line.
        bool Record(RE::FormID a_actor, APMF_API::Handle a_owner, std::uint32_t a_state, RE::FormID a_form, char a_hand,
                    float a_engine, float a_rival, float a_result, bool a_topIsRival) {
            const auto       now = apmf::clock::MonotonicMs();
            std::scoped_lock lk(g_stMx);
            if (g_state.size() >= kStateCap && !g_state.contains(a_actor)) g_state.clear();
            auto& st    = g_state[a_actor];
            st.owner    = a_owner;
            st.state    = a_state;
            st.lastForm = a_form;
            st.lastSlot = SlotOf(a_hand);
            st.engine   = a_engine;
            st.rival    = a_rival;
            st.result   = a_result;
            st.lastMs   = now;
            if (a_state == APMF_API::kEquipPrefState_Lifted && st.lifts != UINT32_MAX) ++st.lifts;
            if (a_state == APMF_API::kEquipPrefState_Outranked && st.withheld != UINT32_MAX) ++st.withheld;

            if (g_lines.size() >= kStateCap) g_lines.clear();
            const std::uint64_t key = (static_cast<std::uint64_t>(a_actor) << 32 | a_form) ^
                                      (static_cast<std::uint64_t>(static_cast<unsigned char>(a_hand)) << 56);
            auto& ln = g_lines[key];
            // F7: this item was LIFTED at its previous scoring and a competitor still heads the engine's
            // list (sorted after that pass): the lift lost that pass (a rival rose by more than the step,
            // or the engine's own order beat it). Counted, shown in the heartbeat and on the menu.
            if (ln.lastScored == APMF_API::kEquipPrefState_Lifted && a_topIsRival) {
                if (st.lostPasses != UINT32_MAX) ++st.lostPasses;
                g_lostPasses.fetch_add(1, std::memory_order_relaxed);
            }
            ln.lastScored = a_state;
            const bool first   = ln.ms == 0;
            const bool changed = ln.state != a_state ||
                                 std::fabs(a_result - ln.result) > kLogChangeRel * std::max(1.0f, std::fabs(ln.result));
            if (!first && !changed && now - ln.ms < kLineHeartbeatMs) return false;
            ln.ms     = now;
            ln.state  = a_state;
            ln.result = a_result;
            return true;
        }

        void Bump(std::uint32_t& a_c) { if (a_c != UINT32_MAX) ++a_c; }
    }

    // ======================================================================================
    // arming
    // ======================================================================================

    void Arm(bool a_steer, int a_weaponSeats, int a_magicSeats, RE::BGSEquipSlot* a_left, RE::BGSEquipSlot* a_right) {
        g_leftHand.store(a_left, std::memory_order_release);
        g_rightHand.store(a_right, std::memory_order_release);
        g_weaponSeats.store(a_weaponSeats, std::memory_order_relaxed);
        g_magicSeats.store(a_magicSeats, std::memory_order_relaxed);
        g_steer.store(a_steer && (a_weaponSeats > 0 || a_magicSeats > 0), std::memory_order_release);
        g_armedKnown.store(true, std::memory_order_release);
        spdlog::info("[ch.26] equip preference / score steer {}: [AiCastSeats] EnableScoreSteer={} (default 1, D5), "
                     "{} weapon-class + {} spell/staff CalculateScore seat(s) hooked. A lift is the engine's own score "
                     "one step ({:.0f}% + {:.2f}) above the best competitor in its engine category, bound by the Equip "
                     "(preferences) and Cast (the ch.8b spell steer) priority sliders.",
                     SteerOn() ? "ARMED" : "OFF", a_steer ? 1 : 0, a_weaponSeats, a_magicSeats,
                     APMF_API::kEquipPrefStepRel * 100.0f, APMF_API::kEquipPrefStepAbs);
    }

    bool SteerOn() { return g_steer.load(std::memory_order_acquire); }

    // ======================================================================================
    // the client API
    // ======================================================================================

    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* /*a_param*/) {
        if (a_actor == 0) return "actor 0";
        if (a_actor == 0x14) return "the actor is the player (a preference steers an NPC's combat AI)";
        if (!g_armedKnown.load(std::memory_order_acquire))
            return "the score seats are not armed yet (a request before kDataLoaded)";
        if (!SteerOn())
            return g_weaponSeats.load(std::memory_order_relaxed) == 0 && g_magicSeats.load(std::memory_order_relaxed) == 0
                       ? "no CalculateScore seat is installed on this runtime (see the [aicastseats] install lines)"
                       : "[AiCastSeats] EnableScoreSteer=0: Harbinger biases no engine score";
        return nullptr;
    }

    void NoteClaim(APMF_API::Handle a_h) {
        if (a_h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lk(g_writeMx);
        if (g_known.size() >= kDeclCap) {
            spdlog::error("[ch.26] h={} not tracked: {} ch.26 handles are live or pending (cap). Its declarations "
                          "will be ignored.", a_h, g_known.size());
            return;
        }
        g_known[a_h] = Known{ apmf::clock::MonotonicMs(), false };
    }

    void Set(APMF_API::Handle a_h, const APMF_API::APMF_EquipPrefEntry* a_entries, std::uint32_t a_count) {
        if (a_h == APMF_API::kInvalidHandle) return;
        Decl d{};
        std::uint32_t dropped = 0, badSlot = 0, zero = 0;
        if (a_entries) {
            for (std::uint32_t i = 0; i < a_count; ++i) {
                auto e = a_entries[i];   // copied synchronously: the client's pointer is never retained
                if (e.form == 0) { ++zero; continue; }
                if (d.count >= APMF_API::kMaxEquipPreference) { ++dropped; continue; }
                if (e.slot != APMF_API::kEquipSlot_Default && e.slot != APMF_API::kEquipSlot_Right &&
                    e.slot != APMF_API::kEquipSlot_Left) {
                    ++badSlot;
                    e.slot = APMF_API::kEquipSlot_Default;
                }
                e.reserved[0] = e.reserved[1] = 0;
                d.e[d.count++] = e;
            }
        }
        {
            std::scoped_lock lk(g_writeMx);
            // The header's contract: a handle that is not a live (or just-requested) kIntent_EquipPreference
            // claim is a silent no-op. Nothing is stored for it, so nothing can leak.
            if (!g_known.contains(a_h)) return;
            g_pending[a_h] = d;   // the newest declaration wins; type-checked on the main thread (Pump)
            g_anyPending.store(true, std::memory_order_release);
        }
        if (dropped || badSlot || zero)
            spdlog::warn("[ch.26] h={} declaration: {} entr(ies) over the cap of {} DROPPED, {} unknown slot value(s) "
                         "read as any hand, {} zero form(s) skipped.",
                         a_h, dropped, APMF_API::kMaxEquipPreference, badSlot, zero);
    }

    void Forget(APMF_API::Handle a_h) {
        if (a_h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lk(g_writeMx);
        if (!g_known.erase(a_h)) return;   // not a ch.26 handle: nothing to do
        g_pending.erase(a_h);
        const auto cur = Load();
        if (!cur->contains(a_h)) return;
        auto next = std::make_shared<DeclMap>(*cur);
        next->erase(a_h);
        Publish(std::move(next));
    }

    void ResetAll(const char* a_why) {
        {
            std::scoped_lock lk(g_writeMx);
            g_known.clear();
            g_pending.clear();
            g_anyPending.store(false, std::memory_order_relaxed);
            Publish(std::make_shared<const DeclMap>());
        }
        {
            std::scoped_lock lk(g_stMx);
            g_state.clear();
            g_lines.clear();
        }
        spdlog::info("[ch.26] reset ({}): declarations, tracked claims and per-actor state dropped.", a_why ? a_why : "?");
    }

    namespace {
        // MAIN thread (Pump). Type-checks every pending declaration and publishes the result in one
        // generation. A form that is not a weapon (a staff included), a shield or a torch -- one that has
        // no hand in the one category map -- can never be scored by the combat scorer as a hand item: it
        // is IGNORED and named once in this declaration's line. A spell is ignored too: the ch.8b cast
        // claim is how a client prefers a spell.
        void ApplyPending() {
            DeclMap batch;
            {
                std::scoped_lock lk(g_writeMx);
                batch.swap(g_pending);
            }
            if (batch.empty()) return;
            std::vector<std::pair<APMF_API::Handle, Decl>> checked;
            checked.reserve(batch.size());
            for (auto& [h, raw] : batch) {
                Decl        d{};
                std::string items, ignored;
                for (std::uint32_t i = 0; i < raw.count; ++i) {
                    const auto&  e    = raw.e[i];
                    RE::TESForm* f    = RE::TESForm::LookupByID(e.form);
                    const auto*  b    = f ? f->As<RE::TESBoundObject>() : nullptr;
                    const auto   hand = b ? (apmf::equipsink::Categorize(b, nullptr) &
                                             (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left))
                                          : 0u;
                    if (!hand) {
                        g_ignoredForms.fetch_add(1, std::memory_order_relaxed);
                        ignored += fmt::format("{}0x{}{}", ignored.empty() ? "" : ", ", apmf::log::Hex(e.form),
                                               f ? fmt::format(" (form type 0x{})", apmf::log::Hex(
                                                                   static_cast<std::uint32_t>(f->GetFormType()), 2))
                                                 : std::string(" (no such form)"));
                        continue;
                    }
                    d.e[d.count++] = e;
                    items += fmt::format("{}0x{} '{}' {} rank {}", items.empty() ? "" : ", ", apmf::log::Hex(e.form),
                                         f->GetName() ? f->GetName() : "",
                                         e.slot == APMF_API::kEquipSlot_Right ? "R" :
                                         e.slot == APMF_API::kEquipSlot_Left  ? "L" : "any hand",
                                         e.rank);
                }
                if (d.count == 0 && raw.count == 0)
                    spdlog::info("[ch.26] h={} declaration CLEARED: nothing is lifted for it.", h);
                else
                    spdlog::info("[ch.26] h={} declared {} preferred item(s): {}{}.", h, d.count,
                                 items.empty() ? "none" : items,
                                 ignored.empty() ? std::string()
                                                 : fmt::format(". IGNORED (not a weapon, staff, shield or torch: the "
                                                               "combat scorer never scores it as a hand item): {}",
                                                               ignored));
                checked.emplace_back(h, d);
            }
            std::scoped_lock lk(g_writeMx);
            const auto cur  = Load();
            auto       next = std::make_shared<DeclMap>(*cur);
            bool       changed = false;
            for (const auto& [h, d] : checked) {
                if (!g_known.contains(h)) continue;   // released while it waited: never published
                if (g_pending.contains(h)) continue;  // a newer declaration arrived: it is applied next frame
                if (d.count == 0) changed |= next->erase(h) != 0;
                else { (*next)[h] = d; changed = true; }
            }
            if (changed) Publish(std::move(next));
        }

        // MAIN thread (Pump), once a second. F4: drop the declaration of a claim that ended by ANY road
        // -- released by the client, released or swept by Harbinger, the actor unloaded -- which is the
        // one fact ControlMap::IsClaimLive answers (an RCU read, no lock held across it). A handle never
        // seen live within kNeverLiveGrace (its request never drained) goes too.
        void Prune(std::uint64_t a_now) {
            std::vector<APMF_API::Handle> handles;
            {
                std::scoped_lock lk(g_writeMx);
                if (g_known.empty() && Load()->empty()) return;
                handles.reserve(g_known.size());
                for (const auto& [h, k] : g_known) handles.push_back(h);
            }
            std::vector<std::pair<APMF_API::Handle, bool>> live;
            live.reserve(handles.size());
            auto& cm = apmf::ControlMap::Get();
            for (const auto h : handles) live.emplace_back(h, cm.IsClaimLive(h));

            std::vector<APMF_API::Handle> dead;
            std::scoped_lock lk(g_writeMx);
            for (const auto& [h, isLive] : live) {
                const auto it = g_known.find(h);
                if (it == g_known.end()) continue;
                if (isLive) { it->second.everLive = true; continue; }
                if (it->second.everLive || a_now - it->second.requestedMs > kNeverLiveGrace) {
                    dead.push_back(h);
                    g_known.erase(it);
                    g_pending.erase(h);
                }
            }
            const auto cur  = Load();
            auto       next = std::make_shared<DeclMap>(*cur);
            bool       changed = false;
            for (auto it = next->begin(); it != next->end();) {   // also anything published for an untracked handle
                if (!g_known.contains(it->first)) { it = next->erase(it); changed = true; }
                else ++it;
            }
            if (changed) Publish(std::move(next));
            if (!dead.empty()) {
                g_droppedDecls.fetch_add(dead.size(), std::memory_order_relaxed);
                std::string list;
                for (const auto h : dead) list += fmt::format("{}{}", list.empty() ? "" : ", ", h);
                spdlog::info("[ch.26] claim(s) ended (released, swept or never drained): h={} -- declaration(s) "
                             "dropped.", list);
            }
        }
    }

    std::uint32_t GetState(RE::FormID a_actor, APMF_API::APMF_EquipPrefInfo* a_out) {
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        const bool winning = a_actor != 0 && apmf::ControlMap::Get().TryGetOwningClaimBasis(
                                                 a_actor, APMF_API::kIntent_EquipPreference, p, basis, &h);
        std::uint32_t count = 0;
        if (winning) {
            const auto map = Load();
            if (const auto it = map->find(h); it != map->end()) count = it->second.count;
        }
        ActorState st{};
        bool       have = false;
        {
            std::scoped_lock lk(g_stMx);
            if (const auto it = g_state.find(a_actor); it != g_state.end()) { st = it->second; have = true; }
        }
        std::uint32_t state = APMF_API::kEquipPrefState_None;
        if (!winning) state = have ? APMF_API::kEquipPrefState_Released : APMF_API::kEquipPrefState_None;
        else if (!have || st.owner != h || st.lastMs == 0) state = APMF_API::kEquipPrefState_Declared;
        else state = st.state;

        if (a_out && a_out->size >= APMF_API::kEquipPrefInfoV22Size) {
            const auto now     = apmf::clock::MonotonicMs();
            a_out->state        = state;
            a_out->actor        = a_actor;
            a_out->ownerHandle  = winning ? h : 0;
            a_out->flags        = winning ? static_cast<std::uint32_t>(p.ival) : 0;
            a_out->entryCount   = count;
            a_out->lastForm     = have ? st.lastForm : 0;
            a_out->lastSlot     = have ? st.lastSlot : APMF_API::kEquipSlot_Default;
            a_out->lastEngineScore = have ? st.engine : 0.0f;
            a_out->lastRivalScore  = have ? st.rival : 0.0f;
            a_out->lastResultScore = have ? st.result : 0.0f;
            a_out->msSinceScored   = (have && st.lastMs) ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                                               now - st.lastMs, 0xFFFFFFFEull))
                                                         : 0xFFFFFFFFu;
            a_out->lifts          = have ? st.lifts : 0;
            a_out->withheld       = have ? st.withheld : 0;
            a_out->forcedDenies   = have ? st.forcedDenies : 0;
            a_out->forceAnomalies = have ? st.forceAnomalies : 0;
        }
        return state;
    }

    // ======================================================================================
    // the score seats
    // ======================================================================================

    float OnScore(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc, RE::Actor* a_actor, float a_engine,
                  std::uintptr_t a_origFn, bool a_magicSeat, ScoreOutcome& a_out) {
        a_out = ScoreOutcome{};
        if (!a_this || !a_cc || !a_actor || !SteerOn()) return a_engine;
        auto& cm = apmf::ControlMap::Get();
        if (cm.ControlledCount() == 0) return a_engine;   // nothing claimed anywhere: one relaxed load
        const bool anyDecl = g_declCount.load(std::memory_order_relaxed) != 0;
        if (!a_magicSeat && !anyDecl) return a_engine;    // a weapon is lifted only by a ch.26 declaration

        RE::TESForm* const form   = a_this->item;
        const RE::FormID   formId = form ? form->GetFormID() : 0;
        if (!formId) return a_engine;
        const RE::FormID fid  = a_actor->GetFormID();
        const char       hand = HandOf(a_this->itemSlot.equipSlot);
        a_out.hand            = hand;

        // ---- is THIS item lifted, and by which claim? ----
        Views v;
        APMF_API::APMF_Param prefParam{};
        if (anyDecl) {
            float basis = 0.0f;
            if (cm.TryGetOwningClaimBasis(fid, APMF_API::kIntent_EquipPreference, prefParam, basis, &v.prefHandle)) {
                v.map = Load();
                if (const auto it = v.map->find(v.prefHandle); it != v.map->end() && it->second.count) v.decl = &it->second;
            }
        }
        bool       castSteer = false;
        if (a_magicSeat) {
            const auto ch = hand == 'R' ? apmf::CastHand::kRight : hand == 'L' ? apmf::CastHand::kLeft
                                                                               : apmf::CastHand::kUnknown;
            const RE::FormID d = Driven(fid, ch);
            castSteer          = d != 0 && d == formId;
        }
        int rank = castSteer ? 0 : -1;
        if (!castSteer && v.decl) {
            for (std::uint32_t i = 0; i < v.decl->count; ++i) {
                const auto& e = v.decl->e[i];
                if (e.form == formId && EntryCovers(e, hand)) rank = rank < 0 ? e.rank : std::min<int>(rank, e.rank);
            }
        }
        if (rank < 0) return a_engine;   // not a lifted item: the engine's answer verbatim
        a_out.castSteer = castSteer;

        const APMF_API::Handle owner = castSteer ? apmf::facetattr::CastClaimHandle(fid) : v.prefHandle;
        bool topIsRival = false;   // F7, set by BestRival
        auto finish = [&](std::uint32_t a_state, float a_rival, float a_result) {
            // The ch.8b spell steer keeps its own SCORE-STEER line (core/AiCastSeats.cpp) and counters, and
            // stays out of the per-actor ch.26 state (GetEquipPreferenceState reports ch.26 only).
            if (castSteer) return a_result;
            if (Record(fid, owner, a_state, formId, hand, a_engine, a_rival, a_result, topIsRival)) {
                try {
                    spdlog::info("[ch.26] 0x{} '{}' {} item=0x{} '{}' hand={} cat={} rank={} engineScore={:.3f} "
                                 "best-competitor={:.3f} -> {:.3f} (claim h={})",
                                 apmf::log::Hex(fid), a_actor->GetName() ? a_actor->GetName() : "?",
                                 StateName(a_state), apmf::log::Hex(formId), form->GetName() ? form->GetName() : "?",
                                 hand, a_out.category, rank, a_engine, a_rival, a_result, owner);
                } catch (...) {
                }
            }
            return a_result;
        };

        // D1: never lift what the engine scored 0 (a bow without ammo, an unusable item).
        if (!(a_engine > 0.0f)) {
            g_zero.fetch_add(1, std::memory_order_relaxed);
            return finish(APMF_API::kEquipPrefState_ZeroScore, 0.0f, a_engine);
        }

        // D3: a hand held by a live cast claim carries only the spell. A ch.26 weapon / shield /
        // torch / staff competing for it is not lifted (the cast steer IS that claim's action).
        if (!castSteer) {
            const auto* bound = form->As<RE::TESBoundObject>();
            const std::uint32_t competes = bound ? (apmf::equipsink::Categorize(bound, a_this->itemSlot.equipSlot) &
                                                    (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left))
                                                 : 0u;
            apmf::handblock::HandHold hold{};
            if (competes && apmf::handblock::HeldFor(fid, competes, formId, apmf::handblock::SpellLike(form), hold)) {
                g_held.fetch_add(1, std::memory_order_relaxed);
                return finish(APMF_API::kEquipPrefState_HandHeld, 0.0f, a_engine);
            }
        }

        // D2 / D5: bound by the priority sliders. The competing source is whatever answered beneath
        // this seat (the engine, or a module hooked inside it): the lift replaces ITS answer.
        const auto facet = castSteer ? apmf::sources::Facet::kCast : apmf::sources::Facet::kEquip;
        bool       wins  = true;
        try {
            wins = apmf::facetattr::Outranks(facet, owner, apmf::facetattr::InsideSource(a_origFn));
        } catch (...) {
            wins = false;   // the registry threw: never lift on an unknown ranking
        }
        if (!wins) {
            g_withheld.fetch_add(1, std::memory_order_relaxed);
            return finish(APMF_API::kEquipPrefState_Outranked, 0.0f, a_engine);
        }

        // D1: one step above the best competitor in the engine's own category list.
        v.drivenR = Driven(fid, apmf::CastHand::kRight);
        v.drivenL = Driven(fid, apmf::CastHand::kLeft);
        v.drivenU = Driven(fid, apmf::CastHand::kUnknown);
        float best = 0.0f;
        int   cat  = -1;
        if (!BestRival(a_this, a_cc, v, rank, best, cat, topIsRival)) {
            g_unreadable.fetch_add(1, std::memory_order_relaxed);
            a_out.category = cat;
            return finish(APMF_API::kEquipPrefState_Unreadable, 0.0f, a_engine);
        }
        a_out.category = cat;
        a_out.rival    = best;
        const float target = best > 0.0f ? best * (1.0f + APMF_API::kEquipPrefStepRel) + APMF_API::kEquipPrefStepAbs : 0.0f;
        if (target <= a_engine) {
            g_leading.fetch_add(1, std::memory_order_relaxed);
            return finish(APMF_API::kEquipPrefState_Leading, best, a_engine);
        }
        a_out.lifted = true;
        (castSteer ? g_castSteers : g_lifts).fetch_add(1, std::memory_order_relaxed);
        return finish(APMF_API::kEquipPrefState_Lifted, best, target);
    }

    // ======================================================================================
    // the equip sink (kEquipPref_Force)
    // ======================================================================================

    bool AnyDeclared() { return g_declCount.load(std::memory_order_relaxed) != 0; }

    bool ForcedFor(RE::FormID a_actor, ForceView& a_out) {
        a_out = ForceView{};
        if (!AnyDeclared() || a_actor == 0) return false;
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_EquipPreference, p, basis, &h))
            return false;
        if (!(static_cast<std::uint32_t>(p.ival) & APMF_API::kEquipPref_Force)) return false;
        const auto map = Load();
        const auto it  = map->find(h);
        if (it == map->end() || it->second.count == 0) return false;
        a_out.handle = h;
        a_out.count  = it->second.count;
        std::copy_n(it->second.e, it->second.count, a_out.entries);
        return true;
    }

    std::uint32_t ForceRefusal(const ForceView& a_view, RE::FormID a_item, std::uint32_t a_competes) {
        std::uint32_t refused = 0;
        struct Side { std::uint32_t bit; char hand; };
        constexpr Side kSides[] = { { APMF_API::kEquipCat_Right, 'R' }, { APMF_API::kEquipCat_Left, 'L' } };
        for (const auto& s : kSides) {
            if (!(a_competes & s.bit)) continue;
            bool covered = false, preferred = false;
            for (std::uint32_t i = 0; i < a_view.count; ++i) {
                const auto& e = a_view.entries[i];
                if (!EntryCovers(e, s.hand)) continue;
                covered = true;
                if (e.form == a_item) preferred = true;
            }
            // An item preferred for ANY hand it competes for passes: a one-hander equipped with an
            // EitherHand / null slot competes for both hands (Categorize's conservative rule), and the
            // preferred sword must not be refused because the OTHER hand prefers a dagger.
            if (preferred) return 0;
            if (covered) refused |= s.bit;
        }
        return refused;
    }

    void NoteForceDeny(RE::FormID a_actor) {
        g_forcedDenies.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk(g_stMx);
        if (const auto it = g_state.find(a_actor); it != g_state.end()) Bump(it->second.forcedDenies);
        else if (g_state.size() < kStateCap) Bump(g_state[a_actor].forcedDenies);
    }

    void NoteForceAnomaly(RE::FormID a_actor) {
        g_forceAnomalies.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk(g_stMx);
        if (const auto it = g_state.find(a_actor); it != g_state.end()) Bump(it->second.forceAnomalies);
        else if (g_state.size() < kStateCap) Bump(g_state[a_actor].forceAnomalies);
    }

    void NoteForceOwnClient(RE::FormID /*a_actor*/) { g_forceOwn.fetch_add(1, std::memory_order_relaxed); }

    // ======================================================================================
    // observability
    // ======================================================================================

    Totals GetTotals() {
        Totals t;
        t.lifts          = g_lifts.load(std::memory_order_relaxed);
        t.castSteers     = g_castSteers.load(std::memory_order_relaxed);
        t.leading        = g_leading.load(std::memory_order_relaxed);
        t.zero           = g_zero.load(std::memory_order_relaxed);
        t.held           = g_held.load(std::memory_order_relaxed);
        t.withheld       = g_withheld.load(std::memory_order_relaxed);
        t.unreadable     = g_unreadable.load(std::memory_order_relaxed);
        t.forcedDenies   = g_forcedDenies.load(std::memory_order_relaxed);
        t.forceAnomalies = g_forceAnomalies.load(std::memory_order_relaxed);
        t.forceOwnClient = g_forceOwn.load(std::memory_order_relaxed);
        t.lostPasses     = g_lostPasses.load(std::memory_order_relaxed);
        t.ignoredForms   = g_ignoredForms.load(std::memory_order_relaxed);
        t.droppedDecls   = g_droppedDecls.load(std::memory_order_relaxed);
        return t;
    }

    void Pump() {
        if (!g_armedKnown.load(std::memory_order_relaxed)) return;
        if (g_anyPending.exchange(false, std::memory_order_acq_rel)) ApplyPending();   // F4: type-check, publish
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastPruneMs >= kPruneEveryMs) {   // F4: drop declarations of claims that ended
            g_lastPruneMs = now;
            Prune(now);
        }
        if (now - g_lastHeartbeatMs < kHeartbeatMs) return;
        g_lastHeartbeatMs = now;
        const Totals t    = GetTotals();
        const Totals  o   = g_lastBeat;   // by value: g_lastBeat is overwritten below
        const bool moved  = t.lifts != o.lifts || t.castSteers != o.castSteers || t.leading != o.leading ||
                           t.zero != o.zero || t.held != o.held || t.withheld != o.withheld ||
                           t.unreadable != o.unreadable || t.forcedDenies != o.forcedDenies ||
                           t.forceAnomalies != o.forceAnomalies || t.forceOwnClient != o.forceOwnClient ||
                           t.lostPasses != o.lostPasses || t.ignoredForms != o.ignoredForms ||
                           t.droppedDecls != o.droppedDecls;
        g_lastBeat = t;
        if (!moved && g_declCount.load(std::memory_order_relaxed) == 0) return;
        const auto line = fmt::format(
            "[ch.26] HEARTBEAT {} declaration(s); since load: lifted {} (+{} ch.8b spell steers), a lift that LOST the "
            "next pass {}, leading {}, engine score 0 {}, hand held by a cast claim {}, OUTRANKED {}, list unreadable "
            "{}; force: denied {}, own client's equip {}, ANOMALIES {}; ignored forms {}, declarations dropped with "
            "their claim {}.",
            g_declCount.load(std::memory_order_relaxed), t.lifts, t.castSteers, t.lostPasses, t.leading, t.zero, t.held,
            t.withheld, t.unreadable, t.forcedDenies, t.forceOwnClient, t.forceAnomalies, t.ignoredForms,
            t.droppedDecls);
        if (t.forceAnomalies != o.forceAnomalies || t.unreadable != o.unreadable) spdlog::warn("{}", line);
        else spdlog::info("{}", line);
    }

}
