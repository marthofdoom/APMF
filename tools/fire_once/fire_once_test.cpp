// Host test of native/core/FireOnce.h (the ch.8b fire-once ledger, ABI v47). Needs no game and no CommonLib:
//   g++ -std=c++20 -Wall -Wextra -I native tools/fire_once/fire_once_test.cpp -o /path/fire_once_test && /path/fire_once_test
#include "core/FireOnce.h"

#include <algorithm>
#include <cstdio>

using apmf::fireonce::Handle;
using apmf::fireonce::Ledger;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static constexpr std::uint64_t kMax = 18000;

// The handles of one cast's fires, sorted, for the live set `a_live`.
static std::vector<Handle> Fires(const std::vector<apmf::fireonce::Match>& a_m, const std::vector<Handle>& a_live,
                                 std::uint64_t a_now) {
    bool any = false;
    auto out = apmf::fireonce::ToRecord(
        a_m, [&](Handle h) { return std::find(a_live.begin(), a_live.end(), h) != a_live.end(); }, a_now, kMax, any);
    CHECK(any == !a_live.empty());
    std::sort(out.begin(), out.end());
    return out;
}

int main() {
    CHECK(apmf::fireonce::SelfCheck());

    {   // Mystic Wind on a Restore caster: YES, fire, then every later poll is spent, whatever the event count.
        Ledger l;
        l.NoteYes(7, 0x14, 0xABC, 0);
        l.NoteYes(7, 0x14, 0xABC, 1);   // the engine polls 0x06 several times before it starts
        CHECK(!l.Spent(7));
        CHECK(l.NoteCast(0x14, 0xABC).size() == 1);
        for (int i = 0; i < 5; ++i) { l.NoteYes(7, 0x14, 0xABC, 2); l.NoteCast(0x14, 0xABC); CHECK(l.Spent(7)); }
    }
    {   // An interrupted cast raises no event: the claim is not spent and may start again.
        Ledger l;
        l.NoteYes(1, 1, 2, 0);
        CHECK(!l.Spent(1));
        l.NoteYes(1, 1, 2, 0);
        CHECK(!l.Spent(1));
        CHECK(l.NoteCast(1, 2).size() == 1);
    }
    {   // Two actors, same spell: independent.
        Ledger l;
        l.NoteYes(1, 100, 2, 0);
        l.NoteYes(2, 200, 2, 0);
        const auto f = l.NoteCast(200, 2);
        CHECK(f.size() == 1 && f[0].handle == 2);
        CHECK(!l.Spent(1) && l.Spent(2));
    }
    {   // A claim released unfired and its successor: one cast spends both, the live one alone is the fire.
        Ledger l;
        l.NoteYes(1, 1, 2, 0);
        l.NoteYes(3, 1, 2, 10);
        const auto f = l.NoteCast(1, 2);
        CHECK(f.size() == 2 && l.Spent(1) && l.Spent(3));
        CHECK(Fires(f, { 3 }, 20) == std::vector<Handle>{ 3 });
        CHECK(Fires(f, { 1, 3 }, 20) == (std::vector<Handle>{ 1, 3 }));
        CHECK(Fires(f, { 3 }, 10 + kMax + 1) == std::vector<Handle>{ 3 });   // a live claim has no age bound
    }
    {   // Every match ended: the newest keeps its Fired reason, the older ones do not get one.
        Ledger l;
        l.NoteYes(5, 1, 2, 100);
        l.NoteYes(9, 1, 2, 4000);
        l.NoteYes(7, 1, 2, 2000);
        CHECK(Fires(l.NoteCast(1, 2), {}, 4500) == std::vector<Handle>{ 9 });
        CHECK(l.NoteCast(1, 2).empty());   // all three are spent
    }
    {   // One old ended record: a much later cast of the form is not its fire. The LAST YES is what counts.
        Ledger l;
        l.NoteYes(1, 1, 2, 100);
        CHECK(Fires(l.NoteCast(1, 2), {}, 100 + kMax + 1).empty());
        Ledger r;
        r.NoteYes(1, 1, 2, 100);
        r.NoteYes(1, 1, 2, 5000);
        CHECK(Fires(r.NoteCast(1, 2), {}, 5000 + kMax) == std::vector<Handle>{ 1 });
        Ledger s;   // a YES stamped after the event's own clock read is recent, and never wraps
        s.NoteYes(1, 1, 2, 5000);
        s.NoteYes(1, 1, 2, 4000);
        CHECK(Fires(s.NoteCast(1, 2), {}, 4000) == std::vector<Handle>{ 1 });
        bool any = true;
        CHECK(apmf::fireonce::ToRecord({}, [](Handle) { return true; }, 0, kMax, any).empty() && !any);
    }
    {   // A successor the seats have not answered yet is NOT spent by its predecessor's cast.
        Ledger l;
        l.NoteYes(1, 1, 2, 0);
        l.NoteCast(1, 2);
        CHECK(!l.Spent(4));
        l.NoteYes(4, 1, 2, 0);
        CHECK(!l.Spent(4));
    }
    std::printf(g_fail ? "fire_once: %d FAILED\n" : "fire_once: PASS\n", g_fail);
    return g_fail != 0;
}
