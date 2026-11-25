// Host test of native/core/HoldActivity.h (the held engagement watchdog's activity reader).
//   g++ -std=c++20 -O2 -Wall -Wextra -I native tools/hold_activity/hold_activity_test.cpp -o /path/t && /path/t
#include "core/HoldActivity.h"

#include <cstdio>
#include <initializer_list>

using namespace apmf::holdlogic;
static int g_fail = 0, g_n = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static std::uint32_t Count(std::initializer_list<std::uint32_t> a_states) {
    Activity      a;
    std::uint32_t n = 0;
    for (auto s : a_states) n += a.Step(s, 0, 0).attacks;
    return n;
}

int main() {
    CHECK(Count({ 0, 0, 0 }) == 0);
    CHECK(Count({ 0, 1, 2, 3, 5, 0 }) == 1);               // one swing
    CHECK(Count({ 0, 2, 0, 2, 0 }) == 2);
    CHECK(Count({ 0, 2, 3, 2, 3, 2 }) == 3);               // a chain that never reads None
    CHECK(Count({ 0, 2, 5, 1, 2 }) == 2);
    CHECK(Count({ 0, 2, 6 }) == 2 && Count({ 0, 6, 0 }) == 1);   // a bash
    // The archer of 2026-11-25: never None. Draw, release, draw, release: each counts.
    CHECK(Count({ 8, 9, 10, 10, 11, 12, 14, 8, 10, 11, 13, 9, 10, 12 }) == 6);
    CHECK(Count({ 0, 10, 10, 10, 10 }) == 1);               // drawn and held: one draw, then nothing
    CHECK(Count({ 10, 14 }) == 2);                          // the sample skipped the release itself
    CHECK(Count({ 14, 10 }) == 2);                          // and the None between two arrows
    CHECK(Count({ 0, 15, 16, 17, 15, 17 }) == 4);
    {   // a chained swing sampled in the same state: the attack entry changed
        Activity a;
        CHECK(a.Step(2, 0x10, 0).attacks == 1 && a.Step(2, 0x10, 0).attacks == 0 && a.Step(2, 0x20, 0).attacks == 1);
        CHECK(a.Step(2, 0, 0).attacks == 0);
    }
    {   // a drawn bow whose entry changes is not a shot
        Activity a;
        a.Step(10, 0x10, 0);
        CHECK(!a.Step(10, 0x20, 0).Any());
    }
    {   // casts: per caster, restarts counted, progress and a held charge not
        Activity a;
        CHECK(a.Step(0, 0, 0x003).casts == 1);
        CHECK(a.Step(0, 0, 0x006).casts == 0 && a.Step(0, 0, 0x006).casts == 0);
        CHECK(a.Step(0, 0, 0x003).casts == 1);
        CHECK(a.Step(0, 0, 0x036).casts == 1);
        CHECK(a.Step(0, 0, 0x000).casts == 0);
    }
    {   // the watchdog's question: 20 s of 250 ms samples, an archer shooting without ever reading None
        Activity      a;
        std::uint64_t last = 0;
        const std::uint32_t cyc[] = { 8, 9, 10, 10, 10, 11, 12, 14 };
        bool idle = false;
        a.Step(14, 0, 0);
        for (std::uint64_t t = 0; t < 40000; t += 250) {
            if (a.Step(cyc[(t / 250) % 8], 0, 0).Any()) last = t;
            idle = idle || t - last >= 20000;
        }
        CHECK(!idle);
        for (std::uint64_t t = 40000; t < 80000; t += 250) {   // drawn and never loosed: still ends
            if (a.Step(10, 0, 0).Any()) last = t;
            idle = idle || t - last >= 20000;
        }
        CHECK(idle);
    }
    std::printf(g_fail ? "hold_activity: %d FAILED of %d\n" : "hold_activity: PASS (%d failed of %d)\n", g_fail, g_n);
    return g_fail != 0;
}
