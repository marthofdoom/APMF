#include "PCH.h"
#include "core/CastGuard.h"
#include "core/FacetAttribution.h"
#include "core/Log.h"
#include "core/MoveOrder.h"
#include "core/MoveSuppress.h"
#include "core/MoveSuppressLogic.h"
#include "channels/CombatApproach.h"

#include <array>
#include <atomic>

// ============================================================================
// NO NEW MELEE ATTACK WHILE A STRICT ORDER TRAVELS OR RETURNS (marth 2026-11-25: "they should travel before doing
// anything else"; field: a melee follower ordered to a point stayed in a chain of swings, whose root motion owns the
// body, and never arrived).
//
// WHERE. core/ActionGate.cpp's act() seat, beside ch.25's HoldWeaponAttack: the same ForceFail act() / pop() PAIR.
// Leaves: Attack, AttackLow, Bash, SpecialAttack, BlockAttack; AttackFromCover unless he holds a bow or a crossbow.
// RangedAttack and every cast leaf pass. Only an ENTRY is refused: a swing that runs is never cut (a cut leaves the
// graph attacking). ch.31's bash / power bash / power attack are refused by the same answer (core/CombatMove.cpp).
// NOT covered: a chain the animation itself extends (SCAR), which never enters a tree leaf (its own diagnosis).
//
// WHY IT CANNOT STRAND HIM. The answer is re-made at every movement pick (0.25 s), and it is "no" unless ALL hold:
//   released / dead / left combat / a load : the order's slot is gone (OrderOwner answers none, at once)
//   arrived          : the pick answered Stand with phase Standing
//   a wait           : the short-stop wait outside the leash answers Stand, not travel
//   not carried, yielded, essential down, flying, leaving water : the pick is the engine's
//   the engine stopped asking : the last pick is older than kFreshMs
//   blocked          : kStallMs without moving kStallMove -> attacks allowed for kRestMs, then one more try
//   timeout          : kLegMs in one leg -> the same. Both bounds belong to the ACTOR, a re-point does not restart them
// and the strict claim outranks Engine on the CombatAction facet (the player's priorities), like ch.7 and ch.25.
//
// THREADING. One entry per actor, written by his combat thread (atomics), read there and by the game thread (ch.31).
// No lock, no form lookup. Log lines are rate limited (LineDue) and cannot throw into the tree.
// ============================================================================
namespace apmf::moveorder {

    namespace {
        using apmf::log::Hex;
        using namespace apmf::movelogic;
        static_assert(kLeafMelee == apmf::castguard::kWeaponMelee && kLeafRanged == apmf::castguard::kWeaponRanged);

        constexpr std::size_t   kEntries = 4 * APMF_API::kMoveMaxOrders;

        struct Entry {
            std::atomic<std::uint32_t> fid{ 0 };
            std::atomic<std::uint64_t> pickMs{ 0 };
            std::atomic<bool>          active{ false }, rangedWeapon{ false };
            std::atomic<std::uint32_t> refused{ 0 };   // since the last "resume" line
            SuppressClock              clock{};         // his combat thread only
        };
        std::array<Entry, kEntries> g_e{};
        std::atomic<std::uint64_t>  g_refused{ 0 }, g_moves{ 0 }, g_stalls{ 0 }, g_legs{ 0 }, g_stood{ 0 }, g_full{ 0 };

        Entry* FindEntry(RE::FormID a_fid) {
            for (auto& e : g_e)
                if (e.fid.load(std::memory_order_acquire) == a_fid) return &e;
            return nullptr;
        }
        // `a_fid`'s own combat thread. An entry stays his for the process (never handed on: its clock is plain data);
        // a full table suppresses nobody new and is counted.
        Entry* TakeEntry(RE::FormID a_fid) {
            if (auto* e = FindEntry(a_fid)) return e;
            for (auto& e : g_e) {
                std::uint32_t none = 0;
                if (e.fid.compare_exchange_strong(none, a_fid, std::memory_order_acq_rel)) return &e;
            }
            g_full.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        // Travelling under a live strict order that outranks Engine. `a_order` = its claim.
        Entry* Travelling(RE::FormID a_fid, APMF_API::Handle& a_order) {
            a_order = APMF_API::kInvalidHandle;
            if (a_fid == 0 || !AnyStrict()) return nullptr;
            Entry* const e = FindEntry(a_fid);
            if (!e || !e->active.load(std::memory_order_acquire)) return nullptr;
            const auto now = apmf::combatapproach::WorldMs(), last = e->pickMs.load(std::memory_order_relaxed);
            if (now > last && now - last > SuppressTuning::kFreshMs) return nullptr;
            a_order = OrderOwner(a_fid);
            return a_order != APMF_API::kInvalidHandle ? e : nullptr;
        }

        bool Outranks(RE::FormID a_fid, APMF_API::Handle a_order) {
            apmf::facetattr::Verdict v;
            try {
                v = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, a_order, apmf::sources::kEngineSource, a_fid, "");
            } catch (...) {
                return true;   // the decision could not be recorded: the default-priority answer (ch.7's rule, APMF-B71)
            }
            if (v.enforce) return true;
            g_stood.fetch_add(1, std::memory_order_relaxed);
            try {
                if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombatAction, a_fid, 24u << 8 | 2u))
                    spdlog::info("[move] 0x{} travels under strict order h={} and may attack: {} priority {} does not outrank Engine "
                                 "(priority {}) on the CombatAction facet (APMF_Sources.ini). At most once per 5 s per actor.",
                                 Hex(a_fid), a_order, apmf::facetattr::NameOr(v.client, "an internal claim"), v.claimPrio, v.srcPrio);
            } catch (...) {
            }
            return false;
        }
    }

    void NotePick(RE::FormID a_fid, RE::Actor* a_actor, bool a_travel, bool a_stood, std::uint64_t a_worldMs) {
        if (a_fid == 0) return;
        Entry* const e = a_travel ? TakeEntry(a_fid) : FindEntry(a_fid);
        if (!e || (a_travel && !a_actor)) return;
        RE::NiPoint3 p{};
        if (a_actor) p = a_actor->data.location;
        if (a_travel) {
            const auto* obj  = a_actor->GetEquippedObject(false);
            const auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;
            e->rangedWeapon.store(weap && (weap->IsBow() || weap->IsCrossbow()), std::memory_order_relaxed);
        }
        SuppressEnd end = SuppressEnd::kNone;
        const bool  on  = e->clock.Pick(a_travel, a_stood, p.x, p.y, p.z, a_worldMs, end);
        e->pickMs.store(a_worldMs, std::memory_order_relaxed);
        const bool was = e->active.exchange(on, std::memory_order_acq_rel);
        if (end == SuppressEnd::kStall) g_stalls.fetch_add(1, std::memory_order_relaxed);
        if (end == SuppressEnd::kLeg) g_legs.fetch_add(1, std::memory_order_relaxed);
        if (!was || on) return;
        const std::uint32_t n = e->refused.exchange(0, std::memory_order_relaxed);
        if (n == 0 && end == SuppressEnd::kNone) return;
        try {
            if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombatAction, a_fid, 24u << 8 | (end == SuppressEnd::kNone ? 3u : 4u), 1000))
                spdlog::info("[move] 0x{} may attack again ({}): {} attack(s) were refused on this leg.", Hex(a_fid),
                             end == SuppressEnd::kStall ? "BLOCKED: he travels and does not move"
                             : end == SuppressEnd::kLeg ? "TIMEOUT: this leg is too long"
                             : a_stood                  ? "he stands"
                                                        : "he is not travelling any more",
                             n);
        } catch (...) {
        }
    }

    bool SuppressAttack(RE::FormID a_fid, std::uint8_t a_kind) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        Entry* const     e = Travelling(a_fid, h);
        if (!e || !MeleeLeaf(a_kind, e->rangedWeapon.load(std::memory_order_relaxed)) || !Outranks(a_fid, h)) return false;
        e->refused.fetch_add(1, std::memory_order_relaxed);
        g_refused.fetch_add(1, std::memory_order_relaxed);
        try {
            if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombatAction, a_fid, 24u << 8 | 5u))
                spdlog::info("[move] 0x{} attack REFUSED: he travels under strict order h={} (a running swing is not cut; attacks resume "
                             "when he stands). At most once per 5 s per actor.", Hex(a_fid), h);
        } catch (...) {
        }
        return true;
    }

    bool SuppressCombatMove(RE::FormID a_fid, APMF_API::Handle& a_order) {
        if (!Travelling(a_fid, a_order) || !Outranks(a_fid, a_order)) return false;
        g_moves.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void SuppressReset() {
        for (auto& e : g_e) e.active.store(false, std::memory_order_relaxed);
    }

    void SuppressHeartbeat() {
        spdlog::info("[move] heartbeat, attacks while travelling: leaves refused {}, ch.31 moves refused {}, allowed again blocked {} / "
                     "timeout {}, stood by priority {}, table full {}.", g_refused.load(), g_moves.load(), g_stalls.load(), g_legs.load(),
                     g_stood.load(), g_full.load());
    }

}
