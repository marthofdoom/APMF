#include "PCH.h"
#include "core/FacetAttribution.h"
#include "core/Clock.h"

#include <mutex>
#include <unordered_map>

// Harbinger 1b: the stack-scan caller recovery (kernel32 export, forwarded to ntdll). The same
// one-line import core/EquipSink.cpp uses (the PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long a_framesToSkip, unsigned long a_framesToCapture, void** a_backTrace, unsigned long* a_backTraceHash);

// See core/FacetAttribution.h.

namespace apmf::facetattr {

    namespace {
        constexpr unsigned long kScanFrames = 16;   // the plan's "up to 16 frames" (EquipSink's depth)
        constexpr std::size_t   kLineKeyCap = 4096;

        std::mutex                                       g_lineMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lineLast;   // key -> last line (monotonic ms)
    }

    Verdict Decide(Facet a_facet, APMF_API::Handle a_claim, SourceId a_src, RE::FormID a_actor,
                   const char* a_unknownDetail) {
        namespace src = apmf::sources;
        Verdict v;
        v.claimPrio = src::ClaimPriority(a_claim, a_facet, &v.client);
        if (a_src == src::kNoSource) {
            v.unknown = true;
            v.src     = src::kUnattributedSource;
            src::Unattributed(a_facet, a_actor, a_unknownDetail);   // the hit, the alert, the rate-limited line
            if (!src::OverrideUnknown(a_facet)) {                    // Q4: allowed, never denied
                src::NoteStood(src::kUnattributedSource, a_facet);
                return v;
            }
            v.srcPrio = src::Priority(src::kUnattributedSource, a_facet);
        } else {
            v.src = a_src;
            src::NoteHit(a_src, a_facet);
            v.srcPrio = a_src == v.client ? v.claimPrio : src::Priority(a_src, a_facet);
        }
        v.enforce = v.claimPrio > v.srcPrio;
        if (v.enforce) src::NoteOverruled(v.src, a_facet);
        else           src::NoteStood(v.src, a_facet);
        return v;
    }

    void Observe(Facet a_facet, SourceId a_src, RE::FormID a_actor, const char* a_unknownDetail) {
        namespace src = apmf::sources;
        if (a_src == src::kNoSource) {
            src::Unattributed(a_facet, a_actor, a_unknownDetail);
            src::NoteStood(src::kUnattributedSource, a_facet);
            return;
        }
        src::NoteHit(a_src, a_facet);
        src::NoteStood(a_src, a_facet);
    }

    Caller CallerBelow(std::uintptr_t a_siteRet) {
        Caller c;
        if (!a_siteRet) return c;
        void* frames[kScanFrames]{};
        const unsigned short n = RtlCaptureStackBackTrace(0, kScanFrames, frames, nullptr);
        for (unsigned short i = 0; i + 1 < n; ++i) {
            if (reinterpret_cast<std::uintptr_t>(frames[i]) != a_siteRet) continue;
            c.found = true;
            c.addr  = reinterpret_cast<std::uintptr_t>(frames[i + 1]);
            c.self  = apmf::sources::IsSelf(c.addr);
            c.src   = c.self ? apmf::sources::kNoSource : apmf::sources::ForAddress(c.addr, apmf::sources::Kind::kPlugin);
            break;
        }
        return c;
    }

    std::string NameOr(SourceId a_id, const char* a_none) {
        return a_id != apmf::sources::kNoSource ? std::string(apmf::sources::Name(a_id)) : std::string(a_none);
    }

    bool LineDue(Facet a_facet, RE::FormID a_actor, std::uint32_t a_key, std::uint64_t a_everyMs) {
        const std::uint64_t key = (static_cast<std::uint64_t>(a_actor) << 32) ^
                                  (static_cast<std::uint64_t>(a_facet) << 24) ^ (a_key & 0xFFFFFFu);
        const auto       now = apmf::clock::MonotonicMs();
        std::scoped_lock lk(g_lineMx);
        if (g_lineLast.size() >= kLineKeyCap) g_lineLast.clear();
        auto [it, fresh] = g_lineLast.try_emplace(key, now);
        if (fresh) return true;
        if (now - it->second < a_everyMs) return false;
        it->second = now;
        return true;
    }

}
