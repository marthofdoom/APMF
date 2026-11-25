#pragma once

#include "APMF_API.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// ============================================================================
// THE STRICT MOVEMENT ORDER (ch.24 kApproach_Strict, ABI v48): the decision core.
//
// PURE LOGIC, std + APMF_API.h only, no engine type and no lock, so tools/move_order/move_order_test.cpp runs exactly
// this code. core/MoveOrder.cpp owns the seats, the slot table and every engine read.
// To add a mode: one row of Counts / Strict below. The tunables: MoveTuning.
// ============================================================================
namespace apmf::movelogic {

    // Labelled placeholders until the first field sessions are read (hm1-seat-brief 7.2).
    struct MoveTuning {
        static constexpr float         kDefaultArriveR = APMF_API::kMoveDefaultArriveR;
        static constexpr float         kMinArriveR     = APMF_API::kMoveMinArriveR;
        static constexpr std::uint32_t kMaxWaypoints   = APMF_API::kMoveMaxWaypoints;
        static constexpr std::uint64_t kLineMs         = 3000;    // per-actor status line
        static constexpr std::uint64_t kSilentMs       = 60000;   // live in combat and never asked: say so
        static constexpr std::uint64_t kPickGapMs      = 1000;    // a pick-to-pick gap counts at most this much phase time
        static constexpr std::uint64_t kStaleMs        = 1000;    // no pick for this long (the selector re-picks every 0.25 s): the
                                                                  //   phase of the last one is not reported any more
        static constexpr std::uint64_t kChangeLineMs   = 500;     // phase-change / displaced lines, per order
        static constexpr std::uint64_t kRepointLineMs  = 1000;    // PUBLISHED / re-point SUMMARY lines, per actor
        static constexpr std::uint32_t kShortStopEnds  = 2;       // ended returns in a row without progress
        static constexpr float         kProgress       = 8.0f;    // a return that brought him this much nearer was progress
        static constexpr std::uint64_t kShortHoldMs    = 1000;    // past the leash and no progress: stand this long, then try again
    };

    inline constexpr float kCoordBound = 4000000.0f;   // channels/CombatApproach.h kPointCoordBound (asserted there)

    // What the Movement Selector's value seats answer for one pick.
    enum class Pick : std::uint8_t {
        kOrig,     // every seat returns the engine's own answer
        kStand,    // child 0 (the native wait) says yes
        kTravel,   // child 4 (Return To Combat Area) says yes
        kDenyOnly, // Flee and Hide say no, every other seat returns the engine's own answer (never travel)
    };

    struct Order {
        std::uint32_t mode    = APMF_API::kMove_Free;
        std::uint32_t flags   = 0;
        float         leashR  = 0.0f;
        float         arriveR = 0.0f;
    };

    // The combat thread's per-order state (plain copy of the slot's atomics for one step).
    struct Runtime {
        std::uint32_t phase          = APMF_API::kMovePhase_Pending;
        std::uint32_t arrivedCount   = 0;
        std::uint32_t displacedCount = 0;
        std::uint64_t phaseSinceMs   = 0;
        // short stop (hm1-seat-brief section 6): returns that ended before arriveR
        std::uint32_t shortEnds  = 0;      // ended in a row without progress
        float         refDist    = 0.0f;   // the distance progress is measured from
        std::uint64_t holdUntilMs = 0;     // no progress outside the leash: he stands until then
        float         stopError  = 0.0f;   // the distance a short stop was accepted at (0 = none)
        std::uint32_t shortStops = 0;      // accepted
        std::uint32_t shortHolds = 0;      // outside the leash: 1 s waits
    };

    constexpr bool Strict(std::uint32_t a_mode) {
        return a_mode == APMF_API::kMove_Stand || a_mode == APMF_API::kMove_GoTo || a_mode == APMF_API::kMove_Kite ||
               a_mode == APMF_API::kMove_Path;
    }
    // Modes whose arrival is reported (once per order).
    constexpr bool Counts(std::uint32_t a_mode) { return Strict(a_mode) && a_mode != APMF_API::kMove_Stand; }

    inline void SetPhase(Runtime& a_r, std::uint32_t a_phase, std::uint64_t a_now) {
        if (a_r.phase == a_phase) return;
        a_r.phase        = a_phase;
        a_r.phaseSinceMs = a_now;
    }

    inline void BeginTravel(Runtime& a_r, std::uint32_t a_phase, float a_dist, std::uint64_t a_now) {
        SetPhase(a_r, a_phase, a_now);
        a_r.shortEnds   = 0;
        a_r.refDist     = a_dist;
        a_r.holdUntilMs = 0;
        a_r.stopError   = 0.0f;
    }

    // The order's target is NOT on his area (the area is off or somebody else's), or the engine was passed something.
    // A flee that already runs switches his area off, so the order could never be carried while it lasts: Flee and
    // Hide are refused and nothing else is answered. Only where the claim outranks the source it substitutes.
    constexpr Pick Uncarried(bool a_passed, bool a_outranked, bool a_fleeing, bool a_ranks) {
        return !a_passed && !a_outranked && a_fleeing && a_ranks ? Pick::kDenyOnly : Pick::kOrig;
    }

    // The phase to report `a_silentMs` after the last pick.
    constexpr std::uint32_t PhaseNow(std::uint32_t a_phase, std::uint32_t a_passed, std::uint64_t a_silentMs) {
        using namespace APMF_API;
        if (a_phase == kMovePhase_None || a_phase == kMovePhase_Pending || a_silentMs <= MoveTuning::kStaleMs) return a_phase;
        return (a_passed & kMovePass_EssentialDown) != 0 ? static_cast<std::uint32_t>(kMovePhase_Yielded)
                                                         : static_cast<std::uint32_t>(kMovePhase_NotCarried);
    }

    // One pick: the actor is `a_dist` from the current target, which his combat area carries. `a_returnEnded` = the
    // return child the previous pick started has ended (the selector is entered again, with that answer behind it).
    inline Pick Step(const Order& a_o, Runtime& a_r, float a_dist, std::uint64_t a_now, bool a_returnEnded = false) {
        using namespace APMF_API;
        if (!Strict(a_o.mode)) return Pick::kOrig;
        switch (a_r.phase) {
        case kMovePhase_Standing:
            if (a_dist <= a_o.leashR) return Pick::kStand;
            ++a_r.displacedCount;
            BeginTravel(a_r, kMovePhase_Returning, a_dist, a_now);
            return Pick::kTravel;
        case kMovePhase_Travelling:
        case kMovePhase_Returning:
            break;
        default:   // Pending, or back from Yielded / NotCarried: by distance, no history
            BeginTravel(a_r, kMovePhase_Travelling, a_dist, a_now);
            break;
        }
        if (!(a_dist <= a_o.arriveR)) {   // a NaN distance travels
            if (a_now < a_r.holdUntilMs) return Pick::kStand;
            if (a_r.holdUntilMs != 0) {   // the wait is over: a fresh try
                a_r.holdUntilMs = 0;
                a_r.refDist     = a_dist;
            }
            if (!a_returnEnded) return Pick::kTravel;
            if (a_r.refDist - a_dist >= MoveTuning::kProgress) {
                a_r.shortEnds = 0;
                a_r.refDist   = a_dist;
                return Pick::kTravel;
            }
            if (++a_r.shortEnds < MoveTuning::kShortStopEnds) return Pick::kTravel;
            a_r.shortEnds = 0;
            if (!(a_dist <= a_o.leashR)) {
                a_r.holdUntilMs = a_now + MoveTuning::kShortHoldMs;
                ++a_r.shortHolds;
                return Pick::kStand;   // the phase stays: he is not there
            }
            a_r.stopError = a_dist;
            ++a_r.shortStops;
        }
        if (Counts(a_o.mode) && a_r.arrivedCount == 0) a_r.arrivedCount = 1;
        SetPhase(a_r, kMovePhase_Standing, a_now);
        return Pick::kStand;
    }

    inline bool CoordOk(float a_v) { return a_v >= -kCoordBound && a_v <= kCoordBound; }   // false for NaN

    // The radii rule, after the defaults are filled in. The claim's own R goes through this too (arriveR = the default).
    inline bool RadiiOk(float a_leashR, float a_arriveR) {
        return std::isfinite(a_leashR) && std::isfinite(a_arriveR) && a_arriveR >= MoveTuning::kMinArriveR &&
               a_leashR >= a_arriveR;
    }

    // kMove_Path's call sequence (one per claim, game thread). Accepts index 0 at any time (a new path), then only the
    // next index for the same count.
    struct WaypointSeq {
        std::uint32_t next = 0, count = 0;
        bool Accept(std::uint32_t a_index, std::uint32_t a_count) {
            if (a_count < 2 || a_count > MoveTuning::kMaxWaypoints || a_index >= a_count) return false;
            if (a_index == 0) { next = 1; count = a_count; return true; }
            if (a_count != count || a_index != next) return false;
            ++next;
            return true;
        }
        bool Complete() const { return count != 0 && next == count; }
    };

    // Everything about an order that needs no state. `a_claimR` fills a zero leashR, `a_caps` = the served MoveCaps.
    inline std::uint32_t Validate(const APMF_API::APMF_MoveOrder* a_o, float a_claimR, std::uint32_t a_caps, Order& a_out) {
        using namespace APMF_API;
        if (!a_o || a_o->size < kMoveOrderV48Size) return kMoveOrder_BadSize;
        if (a_o->mode > kMove_Path) return kMoveOrder_BadMode;
        if (a_o->mode == kMove_Disengage) return kMoveOrder_ModeNotServed;
        if (a_o->mode == kMove_Path && !(a_caps & kMoveCaps_Path)) return kMoveOrder_ModeNotServed;
        const std::uint32_t served = (a_caps & kMoveCaps_AttackInPlace) ? static_cast<std::uint32_t>(kMove_AttackInPlace) : 0u;
        if ((a_o->flags & ~served) != 0 || a_o->faceTarget != 0) return kMoveOrder_BadFlags;
        for (const auto w : a_o->reserved)
            if (w != 0) return kMoveOrder_BadFlags;
        a_out = Order{ a_o->mode, a_o->flags, a_o->leashR == 0.0f ? a_claimR : a_o->leashR,
                       a_o->arriveR == 0.0f ? MoveTuning::kDefaultArriveR : a_o->arriveR };
        if (!RadiiOk(a_out.leashR, a_out.arriveR)) return kMoveOrder_BadRadii;
        if (a_o->mode != kMove_Free)
            for (const float v : a_o->pos)
                if (!CoordOk(v)) return kMoveOrder_BadPoint;
        if (a_o->mode == kMove_Path) {
            if (a_o->waypointCount < 2 || a_o->waypointCount > MoveTuning::kMaxWaypoints ||
                a_o->waypointIndex >= a_o->waypointCount)
                return kMoveOrder_BadWaypoint;
        } else if (a_o->waypointIndex != 0 || a_o->waypointCount != 0) {
            return kMoveOrder_BadWaypoint;
        }
        return kMoveOrder_Ok;
    }

    // ---- one published order: a seqlock. One writer (the game thread), any reader. -------------------------------
    struct Snapshot {
        std::uint32_t fid = 0, owner = 0, mode = 0, flags = 0;
        float         leashR = 0.0f, arriveR = 0.0f;
        std::uint32_t waypointCount = 0;   // 0 = one point
        std::uint32_t gen = 0;             // the client-visible order sequence
        std::uint32_t ranks = 0;           // 1 = the claim outranks the source its area substitutes (S1's last verdict)
        std::uint32_t serial = 0;          // changes on every (re)publication that must reset the runtime block
        std::array<std::array<float, 3>, MoveTuning::kMaxWaypoints> points{};

        bool operator==(const Snapshot&) const = default;
    };

    class Slot {
    public:
        // Writer only.
        void Write(const Snapshot& a_s) {
            m_seq.fetch_add(1, std::memory_order_acq_rel);   // odd: readers back off
            m_fid.store(a_s.fid, std::memory_order_release);
            m_owner.store(a_s.owner, std::memory_order_release);
            m_mode.store(a_s.mode, std::memory_order_release);
            m_flags.store(a_s.flags, std::memory_order_release);
            m_leashR.store(a_s.leashR, std::memory_order_release);
            m_arriveR.store(a_s.arriveR, std::memory_order_release);
            m_wpCount.store(a_s.waypointCount, std::memory_order_release);
            m_gen.store(a_s.gen, std::memory_order_release);
            m_ranks.store(a_s.ranks, std::memory_order_release);
            m_serial.store(a_s.serial, std::memory_order_release);
            for (std::size_t i = 0; i < a_s.points.size(); ++i)
                for (std::size_t k = 0; k < 3; ++k) m_points[i][k].store(a_s.points[i][k], std::memory_order_release);
            m_seq.fetch_add(1, std::memory_order_acq_rel);   // even: published
        }
        std::uint32_t Fid() const { return m_fid.load(std::memory_order_acquire); }
        // One consistent snapshot of `a_fid`'s order, or false (not his slot, or a writer was busy every time: that
        // pick is the engine's, never a mixed one).
        bool Read(std::uint32_t a_fid, Snapshot& a_out) const {
            for (int tries = 0; tries < 4; ++tries) {
                const std::uint32_t q0 = m_seq.load(std::memory_order_acquire);
                if (q0 & 1u) continue;
                Snapshot s;
                s.fid           = m_fid.load(std::memory_order_acquire);
                s.owner         = m_owner.load(std::memory_order_acquire);
                s.mode          = m_mode.load(std::memory_order_acquire);
                s.flags         = m_flags.load(std::memory_order_acquire);
                s.leashR        = m_leashR.load(std::memory_order_acquire);
                s.arriveR       = m_arriveR.load(std::memory_order_acquire);
                s.waypointCount = m_wpCount.load(std::memory_order_acquire);
                s.gen           = m_gen.load(std::memory_order_acquire);
                s.ranks         = m_ranks.load(std::memory_order_acquire);
                s.serial        = m_serial.load(std::memory_order_acquire);
                for (std::size_t i = 0; i < s.points.size(); ++i)
                    for (std::size_t k = 0; k < 3; ++k) s.points[i][k] = m_points[i][k].load(std::memory_order_acquire);
                if (m_seq.load(std::memory_order_acquire) != q0) continue;
                if (s.fid != a_fid || a_fid == 0) return false;
                a_out = s;
                return true;
            }
            return false;
        }

    private:
        std::atomic<std::uint32_t> m_seq{ 0 }, m_fid{ 0 }, m_owner{ 0 }, m_mode{ 0 }, m_flags{ 0 }, m_wpCount{ 0 }, m_gen{ 0 },
            m_ranks{ 0 }, m_serial{ 0 };
        std::atomic<float> m_leashR{ 0.0f }, m_arriveR{ 0.0f };
        std::array<std::array<std::atomic<float>, 3>, MoveTuning::kMaxWaypoints> m_points{};
    };

}
