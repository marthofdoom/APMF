// Host test of native/core/SelfBlast.h (the ch.25 party hold on a Self blast). Needs no game and no CommonLib:
//   g++ -std=c++20 -O2 -Wall -Wextra -I native tools/self_blast/self_blast_test.cpp -o /path/t && /path/t
#include "core/SelfBlast.h"

#include <cstdio>

using namespace apmf::selfblast;

static int g_fail = 0, g_n = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static EffectFacts Eff(bool hostile, std::uint32_t ft) { EffectFacts e; e.hostile = hostile; e.areaFeet = ft; return e; }

int main() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // ---- the blast ----
    { const EffectFacts e[] = { Eff(true, 100) };                         // Fire Storm: 2200, not 2133
      CHECK(BlastOf(e, 1, 22.0f, false).radius == 2200.0f);
      CHECK(!BlastOf(e, 1, 22.0f, false).throughWalls);
      CHECK(BlastOf(e, 1, 22.0f, true).throughWalls);
      CHECK(BlastOf(e, 1, 10.0f, false).radius == 1000.0f);               // the setting, live
      CHECK(BlastOf(e, 1, inf, false).radius == inf); }
    { const EffectFacts e[] = { Eff(false, 100) };                        // Harmony-like: benign
      CHECK(BlastOf(e, 1, 22.0f, true).radius == 0.0f); }
    { EffectFacts e[] = { Eff(true, 0), Eff(false, 50) };                 // a benign effect's area is not harm
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 0.0f);
      e[1].detrimental = true;                                            // a detrimental one beside a hostile is
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 1100.0f);
      e[1].noArea = true;
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 0.0f); }
    { EffectFacts e[] = { Eff(true, 10), Eff(true, 15) };
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 330.0f);
      e[0].explosionUnits = 900.0f;                                       // cosmetic: not read
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 330.0f);
      e[0].explosionHarms = true;
      CHECK(BlastOf(e, 2, 22.0f, false).radius == 900.0f);
      CHECK(BlastOf(e, 2, 22.0f, false).throughWalls);
      e[0].explosionUnits = nan;
      CHECK(BlastOf(e, 2, 22.0f, false).radius == inf); }
    { EffectFacts e[1]; e[0].explosionHarms = true; e[0].explosionUnits = 300.0f;   // payload explosion only
      CHECK(BlastOf(e, 1, 22.0f, false).radius == 300.0f); }
    CHECK(BlastOf(nullptr, 0, 22.0f, false).radius == 0.0f);

    // ---- who ----
    CHECK(CountsAsParty(true, false, false, false, false));
    CHECK(CountsAsParty(false, true, false, false, false));
    CHECK(!CountsAsParty(false, false, true, false, false));
    CHECK(!CountsAsParty(false, false, false, true, false));
    CHECK(CountsAsParty(false, false, true, false, true));
    CHECK(CountsAsParty(false, false, false, true, true));
    CHECK(!CountsAsParty(false, false, false, false, true));

    // ---- the answer ----
    Blast b; b.radius = 300.0f;
    Reading r; r.found = r.party = true; r.measuredMs = 1000;
    CHECK(Judge(b, r, false, 1000, 250) == Answer::kClear);               // nobody
    CHECK(Judge(b, r, true, 1000, 250) == Answer::kUnknown);              // summons not measured yet
    Fold(r, 0x14, 300.0f + kPartyPad + 1.0f, false);
    CHECK(Judge(b, r, false, 1000, 250) == Answer::kClear);
    Fold(r, 0x15, 300.0f + kPartyPad, false);
    CHECK(Judge(b, r, false, 1000, 250) == Answer::kHold && r.openId == 0x15);
    CHECK(Judge(b, r, false, 1250, 250) == Answer::kHold);
    CHECK(Judge(b, r, false, 1251, 250) == Answer::kUnknown);             // stale is never clear
    CHECK(Judge(b, r, false, 999, 250) == Answer::kUnknown);
    { Reading u = r; u.found = false; CHECK(Judge(b, u, false, 1000, 250) == Answer::kUnknown); }
    { Reading u = r; u.party = false; CHECK(Judge(b, u, false, 1000, 250) == Answer::kUnknown); }
    { Reading u = r; u.measuredMs = 0; CHECK(Judge(b, u, false, 0, 250) == Answer::kUnknown); }
    { Blast none; CHECK(Judge(none, Reading{}, false, 5, 250) == Answer::kClear); }   // not a blast: never held
    // a sheltered member
    Reading s; s.found = s.party = s.summons = true; s.measuredMs = 1;
    Fold(s, 0x20, 50.0f, true);
    CHECK(Judge(b, s, true, 1, 250) == Answer::kClear);
    b.throughWalls = true;
    CHECK(Judge(b, s, true, 1, 250) == Answer::kHold);
    b.throughWalls = false;
    Fold(s, 0x21, 200.0f, false);                                         // a farther one in the open
    CHECK(Judge(b, s, true, 1, 250) == Answer::kHold && s.openId == 0x21 && s.anyId == 0x20);
    // broken numbers hold
    Reading q; q.found = q.party = true; q.measuredMs = 1;
    Fold(q, 0x30, nan, false);
    CHECK(Judge(b, q, false, 1, 250) == Answer::kHold);
    b.radius = inf;
    Reading far; far.found = far.party = true; far.measuredMs = 1;
    CHECK(Judge(b, far, false, 1, 250) == Answer::kClear);
    Fold(far, 0x31, 9000.0f, false);
    CHECK(Judge(b, far, false, 1, 250) == Answer::kHold);

    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
