#pragma once
#include "APMF_API.h"
#include "core/Sources.h"

#include <cstdint>
#include <string>

// ============================================================================
// core/FacetAttribution -- HARBINGER 1b: the shared decision + caller recovery for the
// facets whose seats ALREADY existed before the source registry (plan
// _research/harbinger-controller-plan-2026-10-11.md, rows "Combat entry (21), re-entry deny
// (22)", "Combat actions (7), pursuit leash (23)", "Combat approach (24)").
//
// THE RULE, the same one ch.20 (Target restore), the Spectator seat and ch.25 (CastGuard)
// apply: at Harbinger's OWN seat, when a source other than the claiming client decides a
// value on a facet a client claimed, the source is NAMED (a hit on the registry), and where
// that seat already enforces, the enforcement stands ONLY when the claim's priority is
// STRICTLY above the source's (a tie goes to the source: its write stands). A source the seat
// cannot name is the degrade rule (Q4): allowed, alerted (Sources::Unattributed), never
// denied -- unless that facet's "Unknown sources can be overridden" is on, where it ranks at
// the Unattributed row's priority. No new seat, no new address, no ABI change.
//
// THREADING. Every function runs on any thread (combat jobs, BSJobs, the game thread): the
// registry reads are lock-free atomics, ClaimPriority takes the handle map's shared lock,
// LineDue one small mutex. Nothing here dereferences an engine object.
// ============================================================================

namespace apmf::facetattr {

    using apmf::sources::Facet;
    using apmf::sources::SourceId;

    struct Verdict {
        bool     enforce   = false;   // the claim outranks the source: the seat's deny / substitution stands
        bool     unknown   = false;   // the source could not be named (the degrade rule ran)
        int      claimPrio = 0;
        int      srcPrio   = 0;
        SourceId client    = apmf::sources::kNoSource;   // the claim's client (kNoSource = an internal claim)
        SourceId src       = apmf::sources::kNoSource;   // the source decided against (kUnattributedSource if unknown)
    };

    // At a seat that ENFORCES claim `a_claim` on `a_facet` against `a_src` (kNoSource = the seat
    // could not name it; `a_unknownDetail` then goes to Sources::Unattributed). Records the hit
    // and, per the outcome, NoteOverruled or NoteStood. The caller enforces iff `enforce`.
    // A source that IS the claim's own client ranks at the claim's priority (its write stands:
    // its own call must not be fought by its own claim).
    Verdict Decide(Facet a_facet, APMF_API::Handle a_claim, SourceId a_src, RE::FormID a_actor,
                   const char* a_unknownDetail);

    // At a seat that only OBSERVES the facet (no enforcement exists there): the hit and a
    // NoteStood (its write stood). kNoSource -> Sources::Unattributed(a_unknownDetail).
    void Observe(Facet a_facet, SourceId a_src, RE::FormID a_actor, const char* a_unknownDetail);

    // STACK-SCAN CALLER RECOVERY (the plan's RECOGNITION; core/EquipSink.cpp precedent). For a
    // seat reached from a known engine call site whose return address is `a_siteRet` (inside
    // engine function F): the frame AFTER the one equal to `a_siteRet` is F's caller. That
    // address resolves through Sources::ForAddress (SkyrimSE.exe -> Engine, skse64 ->
    // DirectCalls, any other module -> a plugin source named after its file). `found` is false
    // when no frame equals `a_siteRet` or the frame list ends there; `self` is true when the
    // caller is APMF itself; `src` is kNoSource for both and for an address in no module.
    struct Caller {
        bool           found = false;
        bool           self  = false;
        std::uintptr_t addr  = 0;   // the caller's return address (0 when not found)
        SourceId       src   = apmf::sources::kNoSource;
    };
    Caller CallerBelow(std::uintptr_t a_siteRet);

    // The registry name of a source, or `a_none` for kNoSource.
    std::string NameOr(SourceId a_id, const char* a_none);

    // Rate limit for the attribution lines: true at most once per `a_everyMs` per (facet, actor,
    // key). Bounded (cleared when it grows past a few thousand keys). Any thread.
    bool LineDue(Facet a_facet, RE::FormID a_actor, std::uint32_t a_key, std::uint64_t a_everyMs = 5000);

}
