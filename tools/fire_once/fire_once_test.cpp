// Host test of native/core/FireOnce.h (the ch.8b fire-once ledger, ABI v47). Needs no game and no CommonLib:
//   g++ -std=c++20 -Wall -Wextra -I native tools/fire_once/fire_once_test.cpp -o /path/fire_once_test && /path/fire_once_test
#include "core/FireOnce.h"

#include <algorithm>
#include <cstdio>

using apmf::fireonce::Ledger;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main() {
    CHECK(apmf::fireonce::SelfCheck());

    {   // Mystic Wind on a Restore caster: YES, fire, then every later poll is spent, whatever the event count.
        Ledger l;
        l.NoteYes(7, 0x14, 0xABC);
        l.NoteYes(7, 0x14, 0xABC);   // the engine polls 0x06 several times before it starts
        CHECK(!l.Spent(7));
        CHECK(l.NoteCast(0x14, 0xABC).size() == 1);
        for (int i = 0; i < 5; ++i) { l.NoteYes(7, 0x14, 0xABC); l.NoteCast(0x14, 0xABC); CHECK(l.Spent(7)); }
    }
    {   // An interrupted cast raises no event: the claim is not spent and may start again.
        Ledger l;
        l.NoteYes(1, 1, 2);
        CHECK(!l.Spent(1));
        l.NoteYes(1, 1, 2);
        CHECK(!l.Spent(1));
        CHECK(l.NoteCast(1, 2).size() == 1);
    }
    {   // Two actors, same spell: independent.
        Ledger l;
        l.NoteYes(1, 100, 2);
        l.NoteYes(2, 200, 2);
        CHECK(l.NoteCast(200, 2) == std::vector<apmf::fireonce::Handle>{ 2 });
        CHECK(!l.Spent(1) && l.Spent(2));
    }
    {   // A claim released unfired and its successor: one cast spends both, the dead one is inert.
        Ledger l;
        l.NoteYes(1, 1, 2);
        l.NoteYes(3, 1, 2);
        auto f = l.NoteCast(1, 2);
        std::sort(f.begin(), f.end());
        CHECK((f == std::vector<apmf::fireonce::Handle>{ 1, 3 }));
    }
    {   // A successor the seats have not answered yet is NOT spent by its predecessor's cast.
        Ledger l;
        l.NoteYes(1, 1, 2);
        l.NoteCast(1, 2);
        CHECK(!l.Spent(4));
        l.NoteYes(4, 1, 2);
        CHECK(!l.Spent(4));
    }
    std::printf(g_fail ? "fire_once: %d FAILED\n" : "fire_once: PASS\n", g_fail);
    return g_fail != 0;
}
