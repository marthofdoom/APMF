#include "PCH.h"
#include "core/FacetAttribution.h"
#include "core/Clock.h"
#include "core/ControlMap.h"

#include <limits>
#include <shared_mutex>

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

        // InsideSource's cache: displaced function -> its raw source (before the client rule).
        // Bounded: a seat's displaced functions are a fixed handful per vtable.
        constexpr std::size_t                             kInsideCap = 1024;
        std::shared_mutex                                 g_insideMx;
        std::unordered_map<std::uintptr_t, SourceId>      g_inside;
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
            if (a_src == v.client) {
                // The claim's OWN client acting on its own claim (menu 2026-10-18): not an outside write, so no
                // Hit, no Blocked, no Passed -- the separate "own" counter. It ranks at the claim's priority
                // (a tie), so its write stands exactly as before.
                src::NoteOwn(a_src, a_facet);
                v.srcPrio = v.claimPrio;
                return v;
            }
            src::NoteHit(a_src, a_facet);
            v.srcPrio = src::Priority(a_src, a_facet);
        }
        v.enforce = v.claimPrio > v.srcPrio;
        if (v.enforce) {
            src::NoteOverruled(v.src, a_facet);
            src::NoteServed(v.client, a_facet);   // the claim's client was served (menu "Served"; kNoSource ignored)
        } else {
            src::NoteStood(v.src, a_facet);
        }
        return v;
    }

    void Observe(Facet a_facet, SourceId a_src, RE::FormID a_actor, const char* a_unknownDetail) {
        namespace src = apmf::sources;
        if (a_src == src::kNoSource) {
            src::Unattributed(a_facet, a_actor, a_unknownDetail);
            src::NoteObserved(src::kUnattributedSource, a_facet);
            return;
        }
        src::NoteHit(a_src, a_facet);
        src::NoteObserved(a_src, a_facet);
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

    SourceId InsideSource(std::uintptr_t a_prevFn) {
        namespace src = apmf::sources;
        if (!a_prevFn) return src::kEngineSource;
        SourceId raw   = src::kNoSource;
        bool     found = false;
        {
            std::shared_lock lk(g_insideMx);
            if (const auto it = g_inside.find(a_prevFn); it != g_inside.end()) {
                raw   = it->second;
                found = true;
            }
        }
        if (!found) {
            // ForAddress: SkyrimSE.exe -> Engine; APMF itself or no module -> kNoSource (told
            // apart by IsSelf); skse64 -> DirectCalls; any other module -> a hook source.
            if (src::IsSelf(a_prevFn)) raw = src::kEngineSource;   // another APMF layer: the engine's answer below it
            else raw = src::ForAddress(a_prevFn, src::Kind::kHook);
            if (raw == src::kDirectSource) {
                // skse64 hooked INSIDE the seat is a hook DLL, not "direct calls" (75): name it by module.
                char name[64]{};
                src::ModuleNameOf(a_prevFn, name, sizeof(name));
                raw = name[0] ? src::Register(name, src::Kind::kHook) : src::kNoSource;
            }
            std::unique_lock lk(g_insideMx);
            if (g_inside.size() < kInsideCap) g_inside.emplace(a_prevFn, raw);
        }
        if (raw != src::kNoSource && raw != src::kEngineSource && src::IsClient(raw)) return src::kEngineSource;
        return raw;
    }

    APMF_API::Handle CastClaimHandle(RE::FormID a_actor) {
        APMF_API::Handle h     = APMF_API::kInvalidHandle;
        float            basis = 0.0f;
        std::uint32_t    flags = 0;
        apmf::ControlMap::Get().CastFacetOutranks(a_actor, -std::numeric_limits<float>::infinity(), h, basis, flags);
        return h;
    }

    APMF_API::Handle SelectClaimHandle(RE::FormID a_actor) {
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_SelectSpell, p, basis, &h);
        return h;
    }

    bool Outranks(Facet a_facet, APMF_API::Handle a_claim, SourceId a_src) {
        namespace src = apmf::sources;
        SourceId  client    = src::kNoSource;
        const int claimPrio = src::ClaimPriority(a_claim, a_facet, &client);
        int       srcPrio   = 0;
        if (a_src == src::kNoSource) {
            if (!src::OverrideUnknown(a_facet)) return false;
            srcPrio = src::Priority(src::kUnattributedSource, a_facet);
        } else {
            srcPrio = a_src == client ? claimPrio : src::Priority(a_src, a_facet);
        }
        return claimPrio > srcPrio;
    }

    std::string WhyStands(Facet a_facet, const Verdict& a_v, APMF_API::Handle a_claim) {
        namespace src = apmf::sources;
        if (a_v.unknown && !src::OverrideUnknown(a_facet))
            return fmt::format("the answer came through a source Harbinger cannot name, and unknown sources are "
                               "ALLOWED on the {} facet ([UnknownSources] {}=allow; =override lets claim h={} "
                               "outrank it)",
                               src::FacetName(a_facet), src::FacetName(a_facet), a_claim);
        return fmt::format("claim h={} ({} priority {}) does not outrank {} (priority {}) on the {} facet "
                           "(APMF_Sources.ini)",
                           a_claim, NameOr(a_v.client, "an internal claim"), a_v.claimPrio, src::Name(a_v.src),
                           a_v.srcPrio, src::FacetName(a_facet));
    }

}
