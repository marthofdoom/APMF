// Host test of native/core/MoveOrderLogic.h (the strict movement order, ABI v48). Needs no game and no CommonLib:
//   g++ -std=c++20 -O2 -pthread -Wall -Wextra -I native tools/move_order/move_order_test.cpp -o /path/t && /path/t
#include "core/MoveOrderLogic.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace APMF_API;
using namespace apmf::movelogic;

static int g_fail = 0, g_n = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static APMF_MoveOrder Ord(std::uint32_t a_mode) {
    APMF_MoveOrder o{};
    o.size = sizeof(o);
    o.mode = a_mode;
    return o;
}
template <class F>
static std::uint32_t V(std::uint32_t a_mode, F a_edit, std::uint32_t a_caps = kMoveCaps_Strict, float a_r = 64.0f) {
    auto  o = Ord(a_mode);
    Order out;
    a_edit(o);
    return Validate(&o, a_r, a_caps, out);
}

int main() {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    {   // Validation: one named result each.
        Order out;
        CHECK(Validate(nullptr, 64, 0, out) == kMoveOrder_BadSize);
        CHECK(V(kMove_Stand, [](auto& o) { o.size = 60; }) == kMoveOrder_BadSize);
        CHECK(V(kMove_Stand, [](auto& o) { o.size = 96; }) == kMoveOrder_Ok);
        CHECK(V(6, [](auto&) {}) == kMoveOrder_BadMode);
        CHECK(V(kMove_Disengage, [](auto&) {}, ~0u) == kMoveOrder_ModeNotServed);
        CHECK(V(kMove_Path, [](auto& o) { o.waypointCount = 2; }) == kMoveOrder_ModeNotServed);
        CHECK(V(kMove_Stand, [](auto& o) { o.flags = kMove_NoDodge; }, ~0u) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.flags = kMove_HoldAttacks; }, ~0u) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.flags = 1u << 9; }, ~0u) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.flags = kMove_AttackInPlace; }) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.flags = kMove_AttackInPlace; }, kMoveCaps_AttackInPlace) == kMoveOrder_Ok);
        CHECK(V(kMove_Stand, [](auto& o) { o.faceTarget = 1; }) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.reserved[4] = 1; }) == kMoveOrder_BadFlags);
        CHECK(V(kMove_Stand, [](auto& o) { o.arriveR = 7.9f; }) == kMoveOrder_BadRadii);
        CHECK(V(kMove_Stand, [](auto& o) { o.leashR = 23.0f; }) == kMoveOrder_BadRadii);
        CHECK(V(kMove_Stand, [=](auto& o) { o.leashR = inf; }) == kMoveOrder_BadRadii);
        CHECK(V(kMove_Stand, [=](auto& o) { o.arriveR = nan; }) == kMoveOrder_BadRadii);
        CHECK(V(kMove_Stand, [](auto& o) { o.leashR = -5.0f; }) == kMoveOrder_BadRadii);
        CHECK(V(kMove_Stand, [=](auto& o) { o.pos[2] = nan; }) == kMoveOrder_BadPoint);
        CHECK(V(kMove_Stand, [](auto& o) { o.pos[0] = 4100000.0f; }) == kMoveOrder_BadPoint);
        CHECK(V(kMove_Stand, [](auto& o) { o.waypointCount = 2; }) == kMoveOrder_BadWaypoint);
        CHECK(V(kMove_Path, [](auto& o) { o.waypointCount = 8; }, ~0u) == kMoveOrder_BadWaypoint);
        CHECK(V(kMove_Path, [](auto& o) { o.waypointCount = 3; o.waypointIndex = 3; }, ~0u) == kMoveOrder_BadWaypoint);
        auto o = Ord(kMove_GoTo);
        CHECK(Validate(&o, 64, 0, out) == kMoveOrder_Ok && out.leashR == 64.0f && out.arriveR == 24.0f);
        WaypointSeq s;
        CHECK(!s.Accept(1, 3));                                        // no start
        CHECK(s.Accept(0, 3) && !s.Accept(2, 3));                      // gap
        CHECK(!s.Accept(0, 8));                                        // more than 7
        CHECK(s.Accept(1, 3) && !s.Accept(1, 3) && !s.Complete());     // repeat
        CHECK(!s.Accept(2, 4) && s.Accept(2, 3) && s.Complete());      // wrong count, then whole
    }
    const Order stand{ kMove_Stand, 0, 64.0f, 24.0f };
    {   // Stand.
        Runtime r;
        CHECK(Step(stand, r, 100.0f, 1) == Pick::kTravel && r.phase == kMovePhase_Travelling);
        CHECK(Step(stand, r, 40.0f, 2) == Pick::kTravel && r.phase == kMovePhase_Travelling);   // inside the leash, not arrived
        CHECK(Step(stand, r, 24.0f, 3) == Pick::kStand && r.phase == kMovePhase_Standing && r.phaseSinceMs == 3);
        CHECK(Step(stand, r, 64.0f, 4) == Pick::kStand && r.displacedCount == 0);
        CHECK(Step(stand, r, 64.5f, 5) == Pick::kTravel && r.phase == kMovePhase_Returning && r.displacedCount == 1);
        CHECK(Step(stand, r, 50.0f, 6) == Pick::kTravel && r.phase == kMovePhase_Returning);
        CHECK(Step(stand, r, 10.0f, 7) == Pick::kStand && r.arrivedCount == 0);
        Runtime in;
        CHECK(Step(stand, in, 20.0f, 1) == Pick::kStand && in.phase == kMovePhase_Standing);
        int flips = 0;
        for (int i = 0; i < 1000; ++i) {   // held half way between the radii
            const auto before = in.phase;
            if (Step(stand, in, 44.0f, 2 + i) != Pick::kStand || in.phase != before) ++flips;
        }
        CHECK(flips == 0);
        for (const auto ph : { kMovePhase_Yielded, kMovePhase_NotCarried }) {   // back: by distance, no history
            Runtime b;
            b.phase = ph;
            CHECK(Step(stand, b, 30.0f, 9) == Pick::kTravel && b.phase == kMovePhase_Travelling);
            b.phase = ph;
            CHECK(Step(stand, b, 5.0f, 9) == Pick::kStand);
        }
        Runtime n;
        CHECK(Step(stand, n, nan, 1) == Pick::kTravel);
        Runtime f;
        CHECK(Step(Order{ kMove_Free, 0, 64, 24 }, f, 500.0f, 1) == Pick::kOrig && f.phase == kMovePhase_Pending);
        CHECK(Step(Order{ kMove_Disengage, 0, 64, 24 }, f, 500.0f, 1) == Pick::kOrig);
    }
    for (const auto mode : { kMove_GoTo, kMove_Kite }) {   // arrival reported once per order
        const Order o{ mode, 0, 64.0f, 24.0f };
        Runtime     r;
        Step(o, r, 300.0f, 1);
        CHECK(r.arrivedCount == 0);
        Step(o, r, 20.0f, 2);
        CHECK(r.arrivedCount == 1);
        Step(o, r, 90.0f, 3);
        Step(o, r, 20.0f, 4);
        CHECK(r.arrivedCount == 1 && r.displacedCount == 1);
    }
    {   // Seqlock: one writer rewriting the slot 1e6 times against 4 readers, 0 mixed snapshots.
        auto              slot = std::make_unique<Slot>();
        std::atomic<bool> stop{ false };
        std::atomic<long> mixed{ 0 }, good{ 0 };
        auto make = [](std::uint32_t i) {
            Snapshot s;
            s.fid = 0x14;
            s.owner = s.mode = s.flags = s.waypointCount = s.gen = s.serial = i;
            s.leashR = s.arriveR = static_cast<float>(i);
            for (auto& p : s.points) p = { static_cast<float>(i), static_cast<float>(i), static_cast<float>(i) };
            return s;
        };
        slot->Write(make(1));
        std::vector<std::thread> rd;
        for (int t = 0; t < 4; ++t)
            rd.emplace_back([&] {
                Snapshot s;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (!slot->Read(0x14, s)) continue;
                    if (s == make(s.gen)) good.fetch_add(1, std::memory_order_relaxed);
                    else mixed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (std::uint32_t i = 2; i < 1000000; ++i) slot->Write(make(i & 0xFFFFF));
        // Contended reads may all back off: give the readers an idle writer before stopping.
        while (good.load() < 1000) std::this_thread::yield();
        stop = true;
        for (auto& t : rd) t.join();
        CHECK(mixed.load() == 0);
        CHECK(good.load() >= 1000);
        Snapshot s;
        CHECK(!slot->Read(0x15, s) && !slot->Read(0, s));
    }
    std::printf(g_fail ? "move_order: %d FAILED of %d\n" : "move_order: PASS (%d failed of %d)\n", g_fail, g_n);
    return g_fail != 0;
}
