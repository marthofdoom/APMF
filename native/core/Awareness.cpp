#include "PCH.h"
#include "core/Awareness.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/Hook.h"        // OnMainThread()
#include "core/Log.h"
#include "core/Sightline.h"

#include <cmath>
#include <cstring>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// See core/Awareness.h for the design and APMF_API.h ("ABI v18: LINE OF SIGHT AND
// AWARENESS") for the contract.

namespace apmf::awareness {

    namespace {

        using apmf::log::Hex;
        using APMF_API::APMF_AwarenessQuery;
        using APMF_API::APMF_AwarenessResult;

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // ---- layout guards for the noise read (verified on 1.6.1170, 1.5.97 and 1.7.104 in
        // AIProcess::SetActorsDetectionEvent: AE 0x6DF9B0 / SE 0x64CDF0 / 1.7.104 0x6F2560 read
        // [rcx+0x10] -> high, [high+0x3D8] -> the event, and write +0x00 level, +0x04/+0x08/+0x0C
        // location, +0x10 stamp, +0x14 ref; the allocation is 0x18 bytes). ----
        static_assert(offsetof(RE::AIProcess, high) == 0x10, "AIProcess::high moved");
        static_assert(offsetof(RE::HighProcessData, actorsGeneratedDetectionEvent) == 0x3D8,
                      "HighProcessData::actorsGeneratedDetectionEvent moved -- re-verify SetActorsDetectionEvent");
        static_assert(offsetof(RE::DetectionEvent, actionValue) == 0x00, "DetectionEvent level at +0x00");
        static_assert(offsetof(RE::DetectionEvent, location) == 0x04, "DetectionEvent location at +0x04");
        static_assert(offsetof(RE::DetectionEvent, timeStamp) == 0x10, "DetectionEvent stamp at +0x10");
        static_assert(offsetof(RE::DetectionEvent, ref) == 0x14, "DetectionEvent ref at +0x14");
        static_assert(sizeof(RE::DetectionEvent) == 0x18, "DetectionEvent is 0x18 bytes");

        constexpr std::uint64_t kNoiseLogMs     = 2000;    // [aware] noise, per actor
        constexpr std::uint64_t kHeartbeatMs    = 30000;
        constexpr std::uint64_t kStaleRecordMs  = 60000;   // a record not looked at for this long is dropped
        constexpr std::size_t   kMaxRecords     = 512;

        std::atomic<bool> g_installTried{ false };
        std::atomic<bool> g_ready{ false };
        std::atomic<bool> g_hearNoise{ true };

        // ---- main-thread-only state (SenseActor is main-thread-only; no lock) ----
        struct NoiseRec {
            bool          haveBaseline = false;
            std::uint32_t stampBits    = 0;       // the event's AI-clock stamp, compared bit-for-bit
            std::uint64_t newAtMs      = 0;       // when Harbinger saw the current noise appear; 0 = baseline
            std::int32_t  level        = -1;
            RE::NiPoint3  where{};
            std::uint64_t lastLookMs   = 0;
            std::uint64_t lastLogMs    = 0;
        };
        std::unordered_map<RE::FormID, NoiseRec> g_noise;

        struct PairState {
            std::uint32_t senses     = 0xFFFFFFFFu;   // the last LOGGED sense set; never answered
            std::uint64_t lastMs     = 0;
            std::uint64_t lastLogMs  = 0;             // review SEV-4: <= 1 senses line per pair per kSensesLogMs
        };
        constexpr std::uint64_t kSensesLogMs = 2000;
        std::unordered_map<std::uint64_t, PairState> g_pairs;

        struct Counters {
            std::uint64_t queries = 0, sensed = 0, sight = 0, hearNoise = 0, hearCombat = 0, proximity = 0,
                          engaged = 0, noises = 0, baselines = 0, quiet = 0;
        } g_c;   // main thread only
        std::atomic<std::uint64_t> g_refused{ 0 };      // any thread (a refusal can come from any thread)
        std::atomic<std::uint64_t> g_refuseLogMs{ 0 };  // any thread
        std::uint64_t g_lastHeartbeatMs = 0;

        constexpr std::uint64_t Since(std::uint64_t a_now, std::uint64_t a_t) { return a_now > a_t ? a_now - a_t : 0; }

        bool Finite(float a) { return std::isfinite(a); }

        bool LoadedActor(const RE::Actor* a) {
            return a && !a->IsDeleted() && !a->IsDisabled() && a->Is3DLoaded();
        }

        // Interior vs exterior, or two different interiors, are different spaces: distances
        // between them mean nothing. Member reads only.
        bool SameSpace(const RE::Actor* a, const RE::Actor* b) {
            const auto* ca = a->GetParentCell();
            const auto* cb = b->GetParentCell();
            if (!ca || !cb) return false;
            if (ca == cb) return true;
            if (ca->IsInteriorCell() || cb->IsInteriorCell()) return false;
            return ca->GetRuntimeData().worldSpace == cb->GetRuntimeData().worldSpace;
        }

        float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // Look at the target's engine noise record (see the header). Updates the novelty
        // record and returns it. `a_baseline` is set when this was the first look.
        const NoiseRec& LookAtNoise(RE::Actor* a_target, std::uint64_t a_now, bool& a_baseline) {
            a_baseline   = false;
            NoiseRec& r  = g_noise[a_target->GetFormID()];
            r.lastLookMs = a_now;

            RE::DetectionEvent ev{};
            bool               have = false;
            if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
                if (auto* high = proc->high) {
                    if (const auto* src = high->actorsGeneratedDetectionEvent) {
                        std::memcpy(&ev, src, sizeof(ev));   // one copy-out of the 0x18 bytes
                        have = true;
                    }
                }
            }
            std::uint32_t bits = 0;
            if (have) std::memcpy(&bits, &ev.timeStamp.timeStamp, sizeof(bits));

            if (!r.haveBaseline) {
                r.haveBaseline = true;
                r.stampBits    = have ? bits : 0;
                r.newAtMs      = 0;
                a_baseline     = true;
                ++g_c.baselines;
                return r;
            }
            if (!have || bits == r.stampBits) return r;   // nothing new

            r.stampBits = bits;
            r.newAtMs   = a_now;
            r.level     = static_cast<std::int32_t>(ev.actionValue);
            r.where     = ev.location;
            ++g_c.noises;
            if (Since(a_now, r.lastLogMs) >= kNoiseLogMs) {
                r.lastLogMs = a_now;
                RE::FormID refId = 0;
                if (ev.ref.native_handle() != 0) {
                    if (auto ref = ev.ref.get()) refId = ref->GetFormID();   // LookupReferenceByHandle (row)
                }
                spdlog::info("[aware] noise: 0x{} made a NEW engine noise -- level {} at {:.0f},{:.0f},{:.0f} (ref 0x{}). "
                             "(<= 1 line per actor per {} s)",
                             Hex(a_target->GetFormID()), r.level, ev.location.x, ev.location.y, ev.location.z,
                             Hex(refId), kNoiseLogMs / 1000);
            }
            return r;
        }

        std::string SenseText(std::uint32_t a_s, std::uint32_t a_d) {
            if (a_s == 0) return "nothing";
            std::string t;
            const auto add = [&t](const char* x) {
                if (!t.empty()) t += "+";
                t += x;
            };
            if (a_s & APMF_API::kSense_Sight) add("SIGHT");
            if (a_s & APMF_API::kSense_Hearing)
                add((a_d & APMF_API::kAwareDetail_HeardNoise) ? ((a_d & APMF_API::kAwareDetail_NearAnchor)
                                                                     ? "HEARING(noise, near anchor)"
                                                                     : "HEARING(noise)")
                                                              : ((a_d & APMF_API::kAwareDetail_NearAnchor)
                                                                     ? "HEARING(combat, near anchor)"
                                                                     : "HEARING(combat)"));
            if (a_s & APMF_API::kSense_Proximity) add("PROXIMITY");
            if (a_s & APMF_API::kSense_Engaged)
                add((a_d & APMF_API::kAwareDetail_EngagedAnchor) ? "ENGAGED(anchor)" : "ENGAGED(viewer)");
            return t;
        }

        void Heartbeat(std::uint64_t a_now) {
            if (Since(a_now, g_lastHeartbeatMs) < kHeartbeatMs) return;
            g_lastHeartbeatMs = a_now;
            const std::uint64_t refused = g_refused.exchange(0, std::memory_order_relaxed);
            if (g_c.queries == 0 && refused == 0) return;
            spdlog::info("[aware] heartbeat {} s: {} SenseActor call(s) ({} refused); sensed {} -- sight {}, hearing: "
                         "noise {} / combat {}, proximity {}, engaged {}; new engine noises seen {}, first looks "
                         "(baselines) {}; tracked actors {}; senses lines rate-limited away {}.",
                         kHeartbeatMs / 1000, g_c.queries, refused, g_c.sensed, g_c.sight, g_c.hearNoise,
                         g_c.hearCombat, g_c.proximity, g_c.engaged, g_c.noises, g_c.baselines, g_noise.size(),
                         g_c.quiet);
            g_c = Counters{};
        }

        void Prune(std::uint64_t a_now) {
            if (g_noise.size() > kMaxRecords)
                std::erase_if(g_noise, [a_now](const auto& kv) { return Since(a_now, kv.second.lastLookMs) > kStaleRecordMs; });
            if (g_pairs.size() > kMaxRecords)
                std::erase_if(g_pairs, [a_now](const auto& kv) { return Since(a_now, kv.second.lastMs) > kStaleRecordMs; });
        }

    }

    void Install() {
        if (g_installTried.exchange(true)) return;
        if (!apmf::sightline::Armed()) {
            spdlog::warn("[aware] SenseActor NOT armed -- the line-of-sight service is not ({}). Every call answers "
                         "Unsupported.", apmf::sightline::NotArmedReason());
            return;
        }
        // mit-3.7 F1: the handle lookup (currentCombatTarget, the noise's ref) and the Character
        // vtable (IsInCombat, slot 0xE3) must be verified addresses.
        bool ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(12204, 12332) }.address(),
                                                "Awareness.LookupReferenceByHandle");
        ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RE::VTABLE_Character[0] }.address(),
                                           "Awareness.Character (IsInCombat slot 0xE3)") && ok;
        if (!ok) {
            spdlog::error("[aware] SenseActor NOT armed -- the mit-3.7 self-check refused an address it uses (listed "
                          "above). Every call answers Unsupported.");
            return;
        }
        g_hearNoise.store(GetPrivateProfileIntA("Awareness", "bHearNoise", 1, kIni) != 0, std::memory_order_relaxed);
        g_ready.store(true, std::memory_order_release);
        spdlog::info("[aware] SenseActor armed (main thread only): sight = own ray (no cone), hearing = new engine noise "
                     "within the radius of the viewer or anchor ({}) or a target in combat nearby, proximity, engaged. "
                     "Noise window {} ms.",
                     g_hearNoise.load(std::memory_order_relaxed) ? "on" : "OFF by [Awareness] bHearNoise=0",
                     APMF_API::kAwareNoiseWindowMs);
    }

    std::uint32_t SenseActor(const APMF_AwarenessQuery* a_q, APMF_AwarenessResult* a_out) {
        if (!a_out || a_out->size < sizeof(APMF_AwarenessResult)) {
            spdlog::warn("[aware] SenseActor REFUSED: the result pointer is null or its size is below the v18 layout "
                         "({} bytes).", sizeof(APMF_AwarenessResult));
            return APMF_API::kQuery_BadArgs;
        }
        a_out->status        = APMF_API::kQuery_Failed;
        a_out->senses        = 0;
        a_out->detail        = 0;
        a_out->distance      = -1.0f;
        a_out->sightVerdict  = APMF_API::kLos_Unknown;
        a_out->sightSample   = 0;
        a_out->sightWhy      = 0;
        a_out->noiseLevel    = -1;
        a_out->noiseAgeSec   = -1.0f;
        a_out->noiseDistance = -1.0f;
        a_out->engagedWith   = 0;

        const auto refuse = [&](std::uint32_t a_status, const char* a_why) {
            a_out->status = a_status;
            g_refused.fetch_add(1, std::memory_order_relaxed);
            // Off-thread calls are a client wiring error: say so, but not once per frame.
            const std::uint64_t now  = apmf::clock::MonotonicMs();
            std::uint64_t       last = g_refuseLogMs.load(std::memory_order_relaxed);
            if (Since(now, last) >= 5000 && g_refuseLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
                spdlog::warn("[aware] SenseActor -> status {}: {} (<= 1 line per 5 s)", a_status, a_why);
            }
            return a_status;
        };

        if (!g_ready.load(std::memory_order_acquire) || !apmf::sightline::Armed())
            return refuse(APMF_API::kQuery_Unsupported, "not armed on this runtime (or before kDataLoaded)");
        if (!apmf::hook::OnMainThread())
            return refuse(APMF_API::kQuery_NotMainThread, "called off the main (player-Update) thread; nothing done");
        if (!a_q || a_q->size < sizeof(APMF_AwarenessQuery))
            return refuse(APMF_API::kQuery_BadArgs, "query null or its size is below the v18 layout");
        if (!a_q->viewer || !a_q->target || a_q->viewer == a_q->target)
            return refuse(APMF_API::kQuery_BadArgs, "viewer or target is 0, or they are the same actor");
        if (!Finite(a_q->sightRange) || a_q->sightRange < 0.0f || !Finite(a_q->hearRadius) || a_q->hearRadius < 0.0f ||
            !Finite(a_q->proximityRadius) || a_q->proximityRadius < 0.0f)
            return refuse(APMF_API::kQuery_BadArgs, "a range is negative or not finite");

        auto* viewer = RE::TESForm::LookupByID<RE::Actor>(a_q->viewer);
        auto* target = RE::TESForm::LookupByID<RE::Actor>(a_q->target);
        if (!LoadedActor(viewer) || viewer->IsDead())
            return refuse(APMF_API::kQuery_NoOrigin, "the viewer is not a loaded, live actor");
        if (!LoadedActor(target))
            return refuse(APMF_API::kQuery_NoOrigin, "the target is not a loaded actor");
        RE::Actor* anchor = nullptr;
        if (a_q->anchor && a_q->anchor != a_q->viewer && a_q->anchor != a_q->target) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_q->anchor);
            if (LoadedActor(a) && SameSpace(viewer, a)) anchor = a;   // not loaded / elsewhere = none
        }

        const std::uint64_t now = apmf::clock::MonotonicMs();
        ++g_c.queries;

        // Clamp the ranges (0 = the default). A clamp is the caller's mistake; say so, rarely.
        const auto pick = [now](float a_v, float a_def, float a_max, const char* a_name) {
            float v = a_v == 0.0f ? a_def : a_v;
            if (v > a_max) {
                static std::uint64_t s_last = 0;
                if (Since(now, s_last) >= 10000) {
                    s_last = now;
                    spdlog::warn("[aware] SenseActor: {} {:.0f} clamped to {:.0f}.", a_name, v, a_max);
                }
                v = a_max;
            }
            return v;
        };
        const float sightRange = pick(a_q->sightRange, APMF_API::kAwareDefaultSightRange, APMF_API::kAwareMaxSightRange,
                                      "sightRange");
        const float hearRadius = pick(a_q->hearRadius, APMF_API::kAwareDefaultHearRadius, APMF_API::kAwareMaxHearRadius,
                                      "hearRadius");
        const float proxRadius = pick(a_q->proximityRadius, APMF_API::kAwareDefaultProximity,
                                      APMF_API::kAwareMaxProximity, "proximityRadius");
        const std::uint32_t skip = a_q->flags;

        std::uint32_t senses = 0, detail = 0;
        a_out->status = APMF_API::kQuery_Ok;

        if (!SameSpace(viewer, target)) {
            detail |= APMF_API::kAwareDetail_OtherSpace;
        } else {
            const RE::NiPoint3 vpos = viewer->GetPosition();
            const RE::NiPoint3 tpos = target->GetPosition();
            const float        dist = Dist(vpos, tpos);
            a_out->distance         = dist;

            // PROXIMITY
            if (!(skip & APMF_API::kAware_NoProximity) && dist <= proxRadius) senses |= APMF_API::kSense_Proximity;

            // SIGHT (the own ray; reuses a verdict younger than kLosRefreshMs)
            if (!(skip & APMF_API::kAware_NoSight)) {
                if (dist > sightRange) {
                    detail |= APMF_API::kAwareDetail_SightOutOfRange;
                } else {
                    bool       reused = false, deferred = false;
                    const auto r      = apmf::sightline::MeasureNow(viewer, target, reused, deferred);
                    if (reused) detail |= APMF_API::kAwareDetail_SightReused;
                    if (deferred) detail |= APMF_API::kAwareDetail_SightDeferred;
                    a_out->sightVerdict = r.verdict;
                    if (r.verdict == APMF_API::kLos_Visible) {
                        senses |= APMF_API::kSense_Sight;
                        a_out->sightSample = r.sample;
                    } else if (r.verdict == APMF_API::kLos_Unavailable) {
                        detail |= APMF_API::kAwareDetail_SightUnavailable;
                        a_out->sightWhy = r.why;
                    } else if (r.verdict == APMF_API::kLos_Occluded) {
                        a_out->sightWhy = r.why;   // kLosWhy_OtherWorld or 0
                    }
                }
            }

            // In combat? (Character slot 0xE3) -- read once, used by HEARING(combat) and ENGAGED.
            const bool targetFighting = target->IsInCombat();

            // HEARING
            if (!(skip & APMF_API::kAware_NoHearing)) {
                bool heard = false;
                if (!g_hearNoise.load(std::memory_order_relaxed)) {
                    detail |= APMF_API::kAwareDetail_NoiseRouteOff;
                } else {
                    bool            baseline = false;
                    const NoiseRec& n        = LookAtNoise(target, now, baseline);
                    if (baseline) detail |= APMF_API::kAwareDetail_NoiseBaseline;
                    if (n.newAtMs != 0) {
                        a_out->noiseLevel  = n.level;
                        a_out->noiseAgeSec = static_cast<float>(Since(now, n.newAtMs)) / 1000.0f;
                        const float dv     = Dist(n.where, vpos);
                        const float da     = anchor ? Dist(n.where, anchor->GetPosition()) : -1.0f;
                        const bool  anchorNearer = anchor && da < dv;
                        a_out->noiseDistance = anchorNearer ? da : dv;
                        if (n.level > 0 && Since(now, n.newAtMs) <= APMF_API::kAwareNoiseWindowMs &&
                            a_out->noiseDistance <= hearRadius) {
                            heard = true;
                            detail |= APMF_API::kAwareDetail_HeardNoise;
                            if (anchorNearer) detail |= APMF_API::kAwareDetail_NearAnchor;
                            ++g_c.hearNoise;
                        }
                    }
                }
                if (!heard && targetFighting) {
                    const float dv = dist;
                    const float da = anchor ? Dist(tpos, anchor->GetPosition()) : -1.0f;
                    if (dv <= hearRadius || (anchor && da <= hearRadius)) {
                        heard = true;
                        detail |= APMF_API::kAwareDetail_HeardCombat;
                        if (dv > hearRadius) detail |= APMF_API::kAwareDetail_NearAnchor;
                        ++g_c.hearCombat;
                    }
                }
                if (heard) senses |= APMF_API::kSense_Hearing;
            }

            // ENGAGED: it is fighting the viewer or the anchor.
            if (!(skip & APMF_API::kAware_NoEngaged) && targetFighting) {
                const auto ct = target->GetActorRuntimeData().currentCombatTarget.get();   // NiPointer<Actor>
                if (ct && ct.get() == viewer) {
                    senses |= APMF_API::kSense_Engaged;
                    a_out->engagedWith = viewer->GetFormID();
                } else if (ct && anchor && ct.get() == anchor) {
                    senses |= APMF_API::kSense_Engaged;
                    detail |= APMF_API::kAwareDetail_EngagedAnchor;
                    a_out->engagedWith = anchor->GetFormID();
                }
            }
        }

        a_out->senses = senses;
        a_out->detail = detail;
        if (senses) ++g_c.sensed;
        if (senses & APMF_API::kSense_Sight) ++g_c.sight;
        if (senses & APMF_API::kSense_Proximity) ++g_c.proximity;
        if (senses & APMF_API::kSense_Engaged) ++g_c.engaged;

        // [aware] senses: transition-only per (viewer, target).
        auto& ps = g_pairs[(static_cast<std::uint64_t>(a_q->viewer) << 32) | a_q->target];
        // A change inside kSensesLogMs of the pair's last line is counted, not printed, and
        // ps.senses keeps the last PRINTED set, so a change that persists is printed once the
        // window passes; a flicker that returns to the printed set prints nothing.
        if (ps.senses != senses && Since(now, ps.lastLogMs) < kSensesLogMs) {
            ++g_c.quiet;
        } else if (ps.senses != senses) {
            ps.lastLogMs = now;
            spdlog::info("[aware] senses: 0x{} -> 0x{}: {} (d={:.0f}; sight {}{}; noise lvl {} age {:.1f}s at {:.0f}u; "
                         "anchor 0x{})",
                         Hex(a_q->viewer), Hex(a_q->target), SenseText(senses, detail), a_out->distance,
                         apmf::sightline::VerdictName(a_out->sightVerdict),
                         (detail & APMF_API::kAwareDetail_SightOutOfRange) ? " (out of range)" :
                         (detail & APMF_API::kAwareDetail_OtherSpace)      ? " (other space)" : "",
                         a_out->noiseLevel, a_out->noiseAgeSec, a_out->noiseDistance, Hex(anchor ? anchor->GetFormID() : 0));
            ps.senses = senses;
        }
        ps.lastMs = now;

        Prune(now);
        Heartbeat(now);
        return APMF_API::kQuery_Ok;
    }

    void ResetAll(const char* a_why) {
        const std::size_t n = g_noise.size();
        g_noise.clear();
        g_pairs.clear();
        if (n != 0) spdlog::info("[aware] {} -- dropped {} noise baseline(s).", a_why, n);
    }

}
