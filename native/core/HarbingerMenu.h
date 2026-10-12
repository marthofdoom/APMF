#pragma once

// ============================================================================
// core/HarbingerMenu -- HARBINGER 1a, the "Harbinger" page on SKSE Menu Framework 3.
//
// SMF is a RUNTIME dependency only (LoreRim ships 3.14.0, Tuxborn 3.18). Nothing of it is
// vendored: this file resolves the exports it uses by name from SKSEMenuFramework.dll
// (AddSectionItem and the cimgui `ig*` exports SMF's own consumer header
// `resources/SKSEMenuFramework.h` calls -- QTR-Modding/SKSE-Menu-Framework-3 @ dc91824,
// present since its initial commit), and refuses the page as a whole, loudly, if ANY is
// missing. SMF absent or refused -> Harbinger runs from APMF_Sources.ini with one loud
// log line (Q10).
//
// THE PAGE. Alerts (unattributed writes, Q4), a "Facets" tab with the master table per
// facet (winner, source, kind, hits, overruled/stood, seen-age, priority slider), one tab
// per source (its facets, counters and sliders), and a "Seats" tab (the census rows with
// a foreign module). Idle sources (Q14) are greyed and keep their priority.
//
// THREADING. The render callback runs on SMF's render thread: it reads only the source
// registry's lock-free counters and the census snapshot, and a slider only ENQUEUES its
// value (sources::EnqueueSetPriority), applied and saved by the main-thread pump.
// ============================================================================

namespace apmf::harbingermenu {

    // kDataLoaded, after sources::Init(). Any runtime: SMF decides whether it runs.
    void Register();

}
