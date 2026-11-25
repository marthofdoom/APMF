// Host test of native/core/MoveOrderLogic.h (the strict movement order, ABI v48). Needs no game and no CommonLib:
//   g++ -std=c++20 -O2 -pthread -Wall -Wextra -I native tools/move_order/move_order_test.cpp -o /path/t && /path/t
#include "core/MoveOrderLogic.h"
#include "core/MoveSuppressLogic.h"

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
    {   // Short stop: two ended returns without progress.
        Runtime r;
        CHECK(Step(stand, r, 200.0f, 1) == Pick::kTravel && r.refDist == 200.0f);
        CHECK(Step(stand, r, 120.0f, 2) == Pick::kTravel && r.shortEnds == 0);                // an update pick counts nothing
        CHECK(Step(stand, r, 40.0f, 3, true) == Pick::kTravel && r.shortEnds == 0);           // ended, but it brought him nearer
        CHECK(Step(stand, r, 39.0f, 4, true) == Pick::kTravel && r.shortEnds == 1);
        CHECK(Step(stand, r, 39.0f, 5) == Pick::kTravel && r.shortEnds == 1);
        CHECK(Step(stand, r, 38.0f, 6, true) == Pick::kStand && r.phase == kMovePhase_Standing && r.stopError == 38.0f && r.shortStops == 1);
        CHECK(Step(stand, r, 64.0f, 7, true) == Pick::kStand && r.shortEnds == 0);            // standing: entered again, counts nothing
        CHECK(Step(stand, r, 70.0f, 8) == Pick::kTravel && r.phase == kMovePhase_Returning && r.stopError == 0.0f && r.refDist == 70.0f);
        CHECK(Step(stand, r, 69.0f, 9, true) == Pick::kTravel);
        CHECK(Step(stand, r, 69.0f, 10, true) == Pick::kStand && r.phase == kMovePhase_Returning && r.shortHolds == 1 && r.stopError == 0.0f);
        CHECK(Step(stand, r, 69.0f, 1009) == Pick::kStand && r.phase == kMovePhase_Returning);   // the wait
        CHECK(Step(stand, r, 69.0f, 1010) == Pick::kTravel && r.holdUntilMs == 0 && r.shortEnds == 0);
        CHECK(Step(stand, r, 69.0f, 1011, true) == Pick::kTravel && Step(stand, r, 69.0f, 1012, true) == Pick::kStand && r.shortHolds == 2);
        CHECK(Step(stand, r, 20.0f, 1013) == Pick::kStand && r.phase == kMovePhase_Standing);    // arrived inside the wait
        Runtime e;   // exactly the leash is inside, progress of exactly 8 counts
        Step(stand, e, 72.0f, 1);
        CHECK(Step(stand, e, 64.0f, 2, true) == Pick::kTravel && e.shortEnds == 0);
        CHECK(Step(stand, e, 64.0f, 3, true) == Pick::kTravel && Step(stand, e, 64.0f, 4, true) == Pick::kStand && e.stopError == 64.0f);
        Runtime q;   // a NaN distance never stands by a short stop
        Step(stand, q, nan, 1);
        Step(stand, q, nan, 2, true);
        CHECK(Step(stand, q, nan, 3, true) == Pick::kStand && q.phase == kMovePhase_Travelling && q.shortStops == 0);
        Runtime y;   // the wait does not survive a NotCarried interlude
        Step(stand, y, 90.0f, 1);
        Step(stand, y, 90.0f, 2, true);
        CHECK(Step(stand, y, 90.0f, 3, true) == Pick::kStand);
        y.phase = kMovePhase_NotCarried;
        CHECK(Step(stand, y, 90.0f, 4) == Pick::kTravel && y.holdUntilMs == 0);
    }
    {   // Not carried: a running flee is refused only by a claim that outranks the area's source.
        CHECK(Uncarried(false, false, true, true) == Pick::kDenyOnly);
        CHECK(Uncarried(false, false, true, false) == Pick::kOrig);    // the source stands (kSeatStood)
        CHECK(Uncarried(false, false, false, true) == Pick::kOrig);    // not fleeing
        CHECK(Uncarried(true, false, true, true) == Pick::kOrig);      // flight / leaving water passes
        CHECK(Uncarried(false, true, true, true) == Pick::kOrig);      // a higher-priority area
    }
    {   // The flee pops, the area comes back 1.0 s later, the selector asks every 0.25 s: refused all the way.
        std::uint64_t seen = 0;
        const auto    ask  = [&](bool a_isFleeing, std::uint64_t a_now) { return Uncarried(false, false, FleeLatched(seen, a_isFleeing, a_now), true); };
        CHECK(ask(false, 500) == Pick::kOrig);    // never fled: nothing latched
        CHECK(ask(true, 1000) == Pick::kDenyOnly);
        for (std::uint64_t now = 1250; now <= 2250; now += 250) CHECK(ask(false, now) == Pick::kDenyOnly);
        CHECK(ask(false, 2500) == Pick::kDenyOnly);
        CHECK(ask(false, 2501) == Pick::kOrig);   // a latched answer never extends the latch
        CHECK(ask(true, 3000) == Pick::kDenyOnly && seen == 3000);
        CHECK(Uncarried(true, false, FleeLatched(seen, false, 3100), true) == Pick::kOrig);   // the gates stay
        CHECK(Uncarried(false, true, FleeLatched(seen, false, 3100), true) == Pick::kOrig);
        CHECK(Uncarried(false, false, FleeLatched(seen, false, 3100), false) == Pick::kOrig);
        seen = 0;   // a new order (Reset)
        CHECK(ask(false, 3200) == Pick::kOrig);
    }
    {   // The reported phase once the engine stopped asking.
        CHECK(PhaseNow(kMovePhase_Standing, 0, 1000) == kMovePhase_Standing);
        CHECK(PhaseNow(kMovePhase_Standing, 0, 1001) == kMovePhase_NotCarried);
        CHECK(PhaseNow(kMovePhase_Standing, kMovePass_EssentialDown, 1001) == kMovePhase_Yielded);
        CHECK(PhaseNow(kMovePhase_Travelling, kMovePass_Flight, 5000) == kMovePhase_NotCarried);
        CHECK(PhaseNow(kMovePhase_Pending, 0, 99999) == kMovePhase_Pending);
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
    {   // The return's mean: a return that has not ended, or ended elsewhere, is in no mean (2026-11-25: "walked back 1, mean 9766").
        WalkBack w;
        for (int i = 0; i < 14; ++i) w.Book(kMovePhase_Returning, kMovePhase_Returning, 250);
        w.Book(kMovePhase_Returning, kMovePhase_Standing, 200);
        CHECK(w.done == 1 && w.MeanMs() == 3700 && w.openMs == 0);
        w.Book(kMovePhase_Standing, kMovePhase_Returning, 250);
        for (int i = 0; i < 24; ++i) w.Book(kMovePhase_Returning, kMovePhase_Returning, 250);
        CHECK(w.done == 1 && w.MeanMs() == 3700 && w.openMs == 6000);
        w.Book(kMovePhase_Returning, kMovePhase_NotCarried, 250);
        CHECK(w.done == 1 && w.doneMs == 3700 && w.openMs == 0);
        CHECK(WalkBack{}.MeanMs() == 0);
    }
    {   // The early look at the area: once per order, only an area the engine updates and can pick.
        CHECK(HurryDue(false, true, true, 1.0f));
        CHECK(!HurryDue(true, true, true, 1.0f) && !HurryDue(false, false, true, 1.0f) && !HurryDue(false, true, false, 1.0f));
        CHECK(!HurryDue(false, true, true, kHurryPeriod));
    }
    {   // Which leaf is a melee attack.
        CHECK(MeleeLeaf(kLeafMelee, false) && MeleeLeaf(kLeafMelee, true));   // an archer's bash too
        CHECK(!MeleeLeaf(kLeafRanged, true) && !MeleeLeaf(kLeafRanged, false) && !MeleeLeaf(0, false));
        CHECK(MeleeLeaf(kLeafMelee | kLeafRanged, false) && !MeleeLeaf(kLeafMelee | kLeafRanged, true));
    }
    {   // No new attack while travelling, and no way to strand him.
        SuppressClock c;
        SuppressEnd   e;
        std::uint64_t t = 1000;
        float         x = 0;
        for (int i = 0; i < 20; ++i, t += 250, x += 60) CHECK(c.Pick(true, false, x, 0, 0, t, e));   // he walks: refused
        CHECK(!c.Pick(false, true, x, 0, 0, t, e) && e == SuppressEnd::kNone);                        // arrived: at once
        // a wait outside the leash, not carried, yielded: not travel
        CHECK(!c.Pick(false, false, x, 0, 0, t += 250, e));
        // S1: a wait of 3 s (not carried / yielded / down) between two walks is not a stall: the first travel pick after it refuses
        c = {};
        t = 1000;
        CHECK(c.Pick(true, false, 0, 0, 0, t, e) && c.Pick(true, false, 5, 0, 0, t += 250, e));
        for (int i = 0; i < 12; ++i) CHECK(!c.Pick(false, false, 5, 0, 0, t += 250, e));
        CHECK(c.Pick(true, false, 5, 0, 0, t += 250, e) && e == SuppressEnd::kNone);
        // BLOCKED: travelling, not moving
        c = {};
        t = 1000;
        std::uint64_t on = 0, off = 0, run = 0, worst = 0;
        bool          stall = false;
        for (int i = 0; i < 400; ++i, t += 250) {
            const bool s = c.Pick(true, false, static_cast<float>(i % 2) * 20.0f, 0, 0, t, e);
            stall = stall || e == SuppressEnd::kStall;
            s ? ++on : ++off;
            run   = s ? run + 250 : 0;
            worst = run > worst ? run : worst;
        }
        CHECK(stall && worst <= SuppressTuning::kStallMs + 250 && off > on);
        // TIMEOUT: he moves for ever (a chain drags him in circles), re-pointed as often as the client likes
        c = {};
        t = 1000;
        run = worst = 0;
        bool leg = false;
        for (int i = 0; i < 400; ++i, t += 250) {
            const bool s = c.Pick(true, false, static_cast<float>(i * 60), 0, 0, t, e);
            leg   = leg || e == SuppressEnd::kLeg;
            run   = s ? run + 250 : 0;
            worst = run > worst ? run : worst;
        }
        CHECK(leg && worst <= SuppressTuning::kLegMs + 250);
        // a short wait does not restart the leg, standing does
        c = {};
        c.Pick(true, false, 0, 0, 0, 1000, e);
        c.Pick(false, false, 0, 0, 0, 1250, e);
        c.Pick(true, false, 500, 0, 0, 1500, e);
        CHECK(c.legMs == 1000);
        c.Pick(false, true, 500, 0, 0, 1750, e);
        c.Pick(true, false, 500, 0, 0, 2000, e);
        CHECK(c.legMs == 2000);
        // a rest ends by arriving, not only by the clock
        c = {};
        for (t = 1000; t <= 4000; t += 250) c.Pick(true, false, 0, 0, 0, t, e);
        CHECK(!c.Pick(true, false, 0, 0, 0, 4250, e));
        c.Pick(false, true, 0, 0, 0, 4500, e);
        CHECK(c.Pick(true, false, 0, 0, 0, 4750, e));
        // the selector stopped for seconds (another fight): a fresh leg
        CHECK(c.Pick(true, false, 0, 0, 0, 60000, e) && c.legMs == 60000);
    }
    {   // Seqlock: one writer rewriting the slot 1e6 times against 4 readers, 0 mixed snapshots.
        auto              slot = std::make_unique<Slot>();
        std::atomic<bool> stop{ false };
        std::atomic<long> mixed{ 0 }, good{ 0 };
        auto make = [](std::uint32_t i) {
            Snapshot s;
            s.fid = 0x14;
            s.owner = s.mode = s.flags = s.waypointCount = s.gen = s.ranks = s.serial = i;
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
