#include "PCH.h"
#include "core/Sightline.h"
#include "core/Allowance.h"   // SeatVerified(), RuntimeSupported(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/Hook.h"        // OnMainThread()
#include "core/Log.h"

#include <algorithm>
#include <cmath>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/TargetPin.cpp and channels/Travel.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// See core/Sightline.h for the design and APMF_API.h ("ABI v18: LINE OF SIGHT AND
// AWARENESS") for the contract.

namespace apmf::sightline {

    namespace {

        using apmf::log::Hex;

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // ---- the ray (MFO Sightline's field-proven own-ray recipe, 2026-09-30) ----
        constexpr float kFeetLift      = 16.0f;    // the feet sample sits this far off the floor
        constexpr float kTorsoFrac     = 0.55f;
        constexpr float kHeadFrac      = 0.90f;
        constexpr float kNominalHeight = 120.0f;   // a height that reads as nothing falls back to this
        constexpr float kTargetMargin  = 48.0f;    // each ray ends this short of its point (the target's capsule)

        // ---- the table ----
        constexpr std::size_t   kSlots  = 256;     // power of two
        constexpr std::size_t   kProbe  = 16;      // a key lives in one of these consecutive slots
        constexpr std::size_t   kRing   = 64;      // power of two
        constexpr std::uint64_t kOccRunTrustMs = 3000;   // two OCCLUDED further apart do not "agree"
        constexpr std::uint64_t kUnavailLogMs  = 10000;  // per pair
        constexpr std::uint64_t kHeartbeatMs   = 30000;
        static_assert((kSlots & (kSlots - 1)) == 0 && (kRing & (kRing - 1)) == 0);

        std::atomic<bool>        g_armed{ false };
        std::atomic<bool>        g_installTried{ false };
        std::atomic<const char*> g_notArmedReason{ "before kDataLoaded (the service arms there)" };

        // One pair slot: a SEQLOCK over {key, word, measuredMs}. The writer is the main
        // thread only. `askedMs` is OUTSIDE the seqlock payload: any reader may store its
        // own timestamp there (relaxed); the worst a stale store does is keep a pair that
        // just changed hands warm for one more interest window.
        struct Slot {
            std::atomic<std::uint32_t> seq{ 0 };
            std::atomic<std::uint64_t> key{ 0 };          // (viewer << 32) | target; 0 = empty
            std::atomic<std::uint64_t> word{ 0 };         // Pack(verdict, sample, why, occRun)
            std::atomic<std::uint64_t> measuredMs{ 0 };   // 0 = never measured
            std::atomic<std::uint64_t> askedMs{ 0 };
        };
        Slot g_slots[kSlots];

        std::atomic<std::uint64_t> g_ring[kRing];
        std::atomic<std::uint32_t> g_ringIdx{ 0 };

        // Heartbeat counters (relaxed; read on the main thread).
        std::atomic<std::uint64_t> g_asks{ 0 }, g_queued{ 0 }, g_measures{ 0 }, g_rays{ 0 }, g_visible{ 0 },
            g_occluded{ 0 }, g_unavail{ 0 }, g_pickSkipped{ 0 }, g_tableFull{ 0 }, g_dropped{ 0 },
            g_seatHold{ 0 }, g_seatPass{ 0 }, g_seatStop{ 0 };

        // Main-thread only (no lock): per-pair unavailability log throttle.
        std::unordered_map<std::uint64_t, std::uint64_t> g_unavailLog;
        std::uint64_t                                    g_lastHeartbeatMs = 0;
        std::uint64_t                                    g_lastTableFullLogMs = 0;

        // Elapsed ms, clamped at 0. askedMs is stored by OTHER threads with their own clock read,
        // which can be a hair LATER than this thread's `now`: a raw `now - t` would wrap to a
        // huge age and drop a live pair.
        constexpr std::uint64_t Since(std::uint64_t a_now, std::uint64_t a_t) { return a_now > a_t ? a_now - a_t : 0; }

        constexpr std::uint64_t Key(RE::FormID a_viewer, RE::FormID a_target) {
            return (static_cast<std::uint64_t>(a_viewer) << 32) | a_target;
        }
        constexpr RE::FormID KeyViewer(std::uint64_t k) { return static_cast<RE::FormID>(k >> 32); }
        constexpr RE::FormID KeyTarget(std::uint64_t k) { return static_cast<RE::FormID>(k & 0xFFFFFFFFu); }

        std::size_t Home(std::uint64_t k) {
            std::uint64_t z = k + 0x9E3779B97F4A7C15ull;   // splitmix64 finalizer
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return static_cast<std::size_t>(z) & (kSlots - 1);
        }

        constexpr std::uint64_t Pack(std::uint32_t a_verdict, std::uint32_t a_sample, std::uint32_t a_why,
                                     std::uint32_t a_occRun) {
            return (static_cast<std::uint64_t>(a_verdict & 0xFF)) | (static_cast<std::uint64_t>(a_sample & 0xFF) << 8) |
                   (static_cast<std::uint64_t>(a_why & 0xFF) << 16) |
                   (static_cast<std::uint64_t>(std::min<std::uint32_t>(a_occRun, 0xFFFF)) << 32);
        }

        Reading Unpack(std::uint64_t a_word, std::uint64_t a_measuredMs, std::uint64_t a_nowMs) {
            Reading r;
            r.verdict = static_cast<std::uint32_t>(a_word & 0xFF);
            r.sample  = static_cast<std::uint32_t>((a_word >> 8) & 0xFF);
            r.why     = static_cast<std::uint32_t>((a_word >> 16) & 0xFF);
            r.occRun  = static_cast<std::uint32_t>((a_word >> 32) & 0xFFFF);
            if (a_measuredMs == 0) {
                r.verdict = APMF_API::kLos_Unknown;
                r.ageMs   = 0xFFFFFFFFu;
            } else {
                const std::uint64_t age = a_nowMs >= a_measuredMs ? a_nowMs - a_measuredMs : 0;
                r.ageMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(age, 0xFFFFFFFEull));
            }
            return r;
        }

        // ---- seqlock (writer: main thread only) ----
        void Publish(Slot& s, std::uint64_t a_key, std::uint64_t a_word, std::uint64_t a_measuredMs) {
            const std::uint32_t q = s.seq.load(std::memory_order_relaxed);
            s.seq.store(q + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            s.key.store(a_key, std::memory_order_relaxed);
            s.word.store(a_word, std::memory_order_relaxed);
            s.measuredMs.store(a_measuredMs, std::memory_order_relaxed);
            s.seq.store(q + 2, std::memory_order_release);
        }

        // Reader: true and the payload when the slot holds a_key in a consistent read.
        bool TryReadSlot(const Slot& s, std::uint64_t a_key, std::uint64_t& a_word, std::uint64_t& a_measuredMs) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const std::uint32_t s1 = s.seq.load(std::memory_order_acquire);
                if (s1 & 1u) continue;
                const std::uint64_t k = s.key.load(std::memory_order_relaxed);
                const std::uint64_t w = s.word.load(std::memory_order_relaxed);
                const std::uint64_t m = s.measuredMs.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s.seq.load(std::memory_order_relaxed) != s1) continue;
                if (k != a_key) return false;
                a_word       = w;
                a_measuredMs = m;
                return true;
            }
            return false;   // persistently torn: report not-found this time
        }

        // MAIN THREAD: the slot holding a_key, or nullptr. The writer reads its own data.
        Slot* FindMain(std::uint64_t a_key) {
            const std::size_t h = Home(a_key);
            for (std::size_t i = 0; i < kProbe; ++i) {
                Slot& s = g_slots[(h + i) & (kSlots - 1)];
                if (s.key.load(std::memory_order_relaxed) == a_key) return &s;
            }
            return nullptr;
        }

        // MAIN THREAD: the slot for a_key, inserting it (verdict Unknown) when absent. An
        // empty slot in the probe window is used first, else the one asked for longest ago
        // if nobody asked for it within kLosInterestMs. nullptr = the window is full of live
        // pairs (counted, warned, rate-limited); the ask comes again on the next read.
        Slot* EnsureMain(std::uint64_t a_key, std::uint64_t a_nowMs) {
            if (Slot* s = FindMain(a_key)) {
                s->askedMs.store(a_nowMs, std::memory_order_relaxed);
                return s;
            }
            const std::size_t h = Home(a_key);
            Slot*             victim = nullptr;
            std::uint64_t     oldest = ~0ull;
            for (std::size_t i = 0; i < kProbe; ++i) {
                Slot& s = g_slots[(h + i) & (kSlots - 1)];
                if (s.key.load(std::memory_order_relaxed) == 0) {
                    victim = &s;
                    break;
                }
                const std::uint64_t asked = s.askedMs.load(std::memory_order_relaxed);
                if (Since(a_nowMs, asked) > APMF_API::kLosInterestMs && asked < oldest) {
                    oldest = asked;
                    victim = &s;
                }
            }
            if (!victim) {
                g_tableFull.fetch_add(1, std::memory_order_relaxed);
                if (Since(a_nowMs, g_lastTableFullLogMs) >= kUnavailLogMs) {
                    g_lastTableFullLogMs = a_nowMs;
                    spdlog::warn("[los] pair 0x{} -> 0x{} NOT tracked: its {} table slots all hold pairs asked for in "
                                 "the last {} ms (table {} slots). It reads UNKNOWN until one frees.",
                                 Hex(KeyViewer(a_key)), Hex(KeyTarget(a_key)), kProbe, APMF_API::kLosInterestMs,
                                 kSlots);
                }
                return nullptr;
            }
            Publish(*victim, a_key, Pack(APMF_API::kLos_Unknown, 0, 0, 0), 0);
            victim->askedMs.store(a_nowMs, std::memory_order_relaxed);
            return victim;
        }

        // ---- the own ray (MAIN THREAD ONLY) ----
        struct RayOut {
            std::uint32_t verdict = APMF_API::kLos_Unavailable;
            std::uint32_t sample  = 0;
            std::uint32_t why     = APMF_API::kLosWhy_None;
        };

        bool LoadedRef(const RE::Actor* a) {
            return a && !a->IsDeleted() && !a->IsDisabled() && a->Is3DLoaded();
        }

        RayOut CastOwnRay(RE::Actor* a_viewer, RE::Actor* a_target) {
            RayOut out;
            if (!LoadedRef(a_viewer) || a_viewer->IsDead()) {
                out.why = APMF_API::kLosWhy_ViewerNotLoaded;
                return out;
            }
            if (!LoadedRef(a_target)) {
                out.why = APMF_API::kLosWhy_TargetNotLoaded;
                return out;
            }
            auto* vcell = a_viewer->GetParentCell();
            if (!vcell || !vcell->IsAttached()) {
                out.why = APMF_API::kLosWhy_NoWorld;
                return out;
            }
            auto* world = vcell->GetbhkWorld();   // verified row Sightline.TESObjectCELL.GetbhkWorld
            if (!world) {
                out.why = APMF_API::kLosWhy_NoWorld;
                return out;
            }
            auto* tcell = a_target->GetParentCell();
            if (!tcell || !tcell->IsAttached()) {
                out.why = APMF_API::kLosWhy_TargetNotLoaded;
                return out;
            }
            if (tcell != vcell && tcell->GetbhkWorld() != world) {   // an interior seen from outside, etc.
                out.verdict = APMF_API::kLos_Occluded;
                out.why     = APMF_API::kLosWhy_OtherWorld;
                return out;
            }
            // The viewer's own capsule must be excluded, which needs its collision group; a
            // viewer without a character controller has none to exclude (Actor::
            // GetCollisionFilterInfo would write 0, and the ray would stop on its own body).
            if (!a_viewer->GetCharController()) {
                out.why = APMF_API::kLosWhy_NoController;
                return out;
            }
            std::uint32_t info = 0;
            a_viewer->GetCollisionFilterInfo(info);   // verified row; the engine returns &info (ignored)
            const std::uint32_t filter =
                ((info >> 16) << 16) | static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);

            // The eye: the engine's own origin (Character slot 0xC2: position + eye height). No
            // facing is read anywhere -- `dir` is discarded.
            RE::NiPoint3 eye{}, dir{};
            a_viewer->GetEyeVector(eye, dir, false);

            // The target: feet / torso / head. TESObjectREFR::GetHeight() const is the
            // READ-ONLY height (bound height x scale; Character slots 0x73 / 0x74). Actor::
            // GetHeight is not used: it writes the high process's cached height.
            const RE::NiPoint3 feet = a_target->GetPosition();
            float              h    = static_cast<const RE::TESObjectREFR*>(a_target)->GetHeight();
            if (!std::isfinite(h) || h <= 1.0f) h = kNominalHeight;
            const RE::NiPoint3 samples[3] = {
                { feet.x, feet.y, feet.z + kFeetLift },
                { feet.x, feet.y, feet.z + h * kTorsoFrac },
                { feet.x, feet.y, feet.z + h * kHeadFrac },
            };

            const float scale = RE::bhkWorld::GetWorldScale();   // verified row Sightline.bhkWorld.WorldScale
            bool        skipped = false;
            for (std::uint32_t i = 0; i < 3; ++i) {
                const RE::NiPoint3 seg = samples[i] - eye;
                const float        len = seg.Length();
                if (!std::isfinite(len)) continue;
                if (len <= kTargetMargin) {   // point-blank: nothing can stand in the gap
                    out.verdict = APMF_API::kLos_Visible;
                    out.sample  = i + 1;
                    return out;
                }
                const float        f  = (len - kTargetMargin) / len;
                const RE::NiPoint3 to{ eye.x + seg.x * f, eye.y + seg.y * f, eye.z + seg.z * f };

                RE::bhkPickData pick;
                pick.rayInput.from                        = eye * scale;
                pick.rayInput.to                          = to * scale;
                pick.rayInput.enableShapeCollectionFilter = false;
                pick.rayInput.filterInfo                  = filter;
                world->PickObject(pick);   // takes the world's read lock itself (header: LOCK SCOPE)
                g_rays.fetch_add(1, std::memory_order_relaxed);
                if (pick.unkC0) {          // the engine's early-out: nothing was cast (header: PICK SKIPPED)
                    skipped = true;
                    continue;
                }
                if (!pick.rayOutput.HasHit()) {
                    out.verdict = APMF_API::kLos_Visible;
                    out.sample  = i + 1;
                    return out;
                }
            }
            if (skipped) {   // not every ray was cast: no verdict can be claimed
                g_pickSkipped.fetch_add(1, std::memory_order_relaxed);
                out.verdict = APMF_API::kLos_Unavailable;
                out.why     = APMF_API::kLosWhy_PickSkipped;
                return out;
            }
            out.verdict = APMF_API::kLos_Occluded;
            return out;
        }

        // MAIN THREAD: store a fresh measurement into `s` (which holds a_key) and log a
        // transition. Returns what was stored.
        Reading Store(Slot& s, std::uint64_t a_key, const RayOut& a_ray, std::uint64_t a_nowMs) {
            const Reading prev = Unpack(s.word.load(std::memory_order_relaxed),
                                        s.measuredMs.load(std::memory_order_relaxed), a_nowMs);
            std::uint32_t occRun = 0;
            if (a_ray.verdict == APMF_API::kLos_Occluded) {
                const bool agrees = prev.verdict == APMF_API::kLos_Occluded && prev.ageMs <= kOccRunTrustMs;
                occRun = agrees ? prev.occRun + 1 : 1;
            }
            Publish(s, a_key, Pack(a_ray.verdict, a_ray.sample, a_ray.why, occRun), a_nowMs);
            g_measures.fetch_add(1, std::memory_order_relaxed);

            const RE::FormID v = KeyViewer(a_key), t = KeyTarget(a_key);
            switch (a_ray.verdict) {
            case APMF_API::kLos_Visible:  g_visible.fetch_add(1, std::memory_order_relaxed); break;
            case APMF_API::kLos_Occluded: g_occluded.fetch_add(1, std::memory_order_relaxed); break;
            default:                      g_unavail.fetch_add(1, std::memory_order_relaxed); break;
            }
            if (a_ray.verdict == APMF_API::kLos_Unavailable) {
                if (g_unavailLog.size() > 512) g_unavailLog.clear();
                auto& last = g_unavailLog[a_key];
                if (Since(a_nowMs, last) >= kUnavailLogMs) {
                    last = a_nowMs;
                    spdlog::warn("[los] 0x{} -> 0x{}: UNAVAILABLE ({}) -- the own ray could not be cast. NOT treated "
                                 "as visible: an own-line-of-sight cast holds, a pin pauses, GetLineOfSight says "
                                 "UNAVAILABLE. (per pair, every {} s at most)",
                                 Hex(v), Hex(t), WhyName(a_ray.why), kUnavailLogMs / 1000);
                }
            } else if (prev.verdict != a_ray.verdict || prev.ageMs > APMF_API::kLosFreshMs) {
                // Transition-only: a stable verdict at refresh cadence would flood the log.
                if (a_ray.verdict == APMF_API::kLos_Visible)
                    spdlog::info("[los] 0x{} -> 0x{}: VISIBLE (own ray, {} clear)", Hex(v), Hex(t),
                                 SampleName(a_ray.sample));
                else
                    spdlog::info("[los] 0x{} -> 0x{}: OCCLUDED ({})", Hex(v), Hex(t),
                                 a_ray.why == APMF_API::kLosWhy_OtherWorld ? "target in another havok world"
                                                                           : "own ray, all 3 rays blocked");
            }
            return Unpack(Pack(a_ray.verdict, a_ray.sample, a_ray.why, occRun), a_nowMs, a_nowMs);
        }

        void MeasureKey(Slot& s, std::uint64_t a_key, std::uint64_t a_nowMs) {
            auto* v = RE::TESForm::LookupByID<RE::Actor>(KeyViewer(a_key));
            auto* t = RE::TESForm::LookupByID<RE::Actor>(KeyTarget(a_key));
            RayOut ray;
            if (!v) {
                ray.why = APMF_API::kLosWhy_ViewerNotLoaded;
            } else if (!t) {
                ray.why = APMF_API::kLosWhy_TargetNotLoaded;
            } else {
                ray = CastOwnRay(v, t);
            }
            Store(s, a_key, ray, a_nowMs);
        }

        void Heartbeat(std::uint64_t a_nowMs) {
            if (Since(a_nowMs, g_lastHeartbeatMs) < kHeartbeatMs) return;
            g_lastHeartbeatMs = a_nowMs;
            std::size_t tracked = 0;
            for (const auto& s : g_slots)
                if (s.key.load(std::memory_order_relaxed) != 0) ++tracked;
            const auto x = [](std::atomic<std::uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
            const auto asks = x(g_asks), queued = x(g_queued), meas = x(g_measures), rays = x(g_rays),
                       vis = x(g_visible), occ = x(g_occluded), un = x(g_unavail), skip = x(g_pickSkipped),
                       full = x(g_tableFull), drop = x(g_dropped), hold = x(g_seatHold), pass = x(g_seatPass),
                       stop = x(g_seatStop);
            if (tracked == 0 && asks == 0 && meas == 0) return;   // nothing asked for: stay quiet
            spdlog::info("[los] heartbeat {} s: tracked {} pair(s); asks {} (new {}), measured {} ({} rays): visible {}, "
                         "occluded {}, unavailable {} (pick skipped {}); dropped idle {}, table full {}; seats: "
                         "passed {}, held/paused {}, channels stopped {}.",
                         kHeartbeatMs / 1000, tracked, asks, queued, meas, rays, vis, occ, un, skip, drop, full, pass,
                         hold, stop);
        }

    }

    const char* VerdictName(std::uint32_t a_v) {
        switch (a_v) {
        case APMF_API::kLos_Unknown:     return "UNKNOWN";
        case APMF_API::kLos_Visible:     return "VISIBLE";
        case APMF_API::kLos_Occluded:    return "OCCLUDED";
        case APMF_API::kLos_Unavailable: return "UNAVAILABLE";
        case APMF_API::kLos_Unsupported: return "UNSUPPORTED";
        default:                         return "?";
        }
    }

    const char* WhyName(std::uint32_t a_w) {
        switch (a_w) {
        case APMF_API::kLosWhy_None:            return "none";
        case APMF_API::kLosWhy_BadArgs:         return "bad arguments";
        case APMF_API::kLosWhy_ViewerNotLoaded: return "viewer not a loaded, live actor";
        case APMF_API::kLosWhy_TargetNotLoaded: return "target not a loaded actor";
        case APMF_API::kLosWhy_NoWorld:         return "no attached cell / havok world";
        case APMF_API::kLosWhy_NoController:    return "viewer has no character controller";
        case APMF_API::kLosWhy_OtherWorld:      return "target in another havok world";
        case APMF_API::kLosWhy_TableFull:       return "pair table full";
        case APMF_API::kLosWhy_PickSkipped:     return "the engine skipped the pick";
        default:                                return "?";
        }
    }

    const char* SampleName(std::uint32_t a_s) {
        switch (a_s) {
        case 1:  return "feet";
        case 2:  return "torso";
        case 3:  return "head";
        default: return "none";
        }
    }

    void Install() {
        if (g_installTried.exchange(true)) return;
        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the ray's calls are verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (GetPrivateProfileIntA("Sightline", "bOwnLineOfSight", 1, kIni) == 0) {
            why = "[Sightline] bOwnLineOfSight=0 in Data/SKSE/Plugins/APMF.ini";
        } else {
            // mit-3.7 F1: every engine address the ray uses must be a verified row. Check all
            // before arming any: a ray with one unverified call is refused whole.
            bool ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RE::VTABLE_bhkWorld[0] }.address(),
                                                    "Sightline.bhkWorld (PickObject slot 0x33)");
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RE::VTABLE_Character[0] }.address(),
                                               "Sightline.Character (GetEyeVector 0xC2, GetBoundMin/Max 0x73/0x74)") && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(36559, 37560) }.address(),
                                               "Sightline.Actor.GetCollisionFilterInfo") && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(18536, 18995) }.address(),
                                               "Sightline.TESObjectCELL.GetbhkWorld") && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(231896, 188105) }.address(),
                                               "Sightline.bhkWorld.WorldScale") && ok;
            if (!ok) why = "the mit-3.7 self-check refused an address the ray uses (listed above)";
        }
        if (why) {
            g_notArmedReason.store(why, std::memory_order_release);
            spdlog::warn("[los] own line of sight NOT armed -- {}. GetLineOfSight answers UNSUPPORTED, SenseActor "
                         "Unsupported, and kCastFlag_OwnLineOfSight / kTargetPin_OwnLineOfSight are REFUSED at the "
                         "request.", why);
            return;
        }
        g_armed.store(true, std::memory_order_release);
        spdlog::info("[los] own line of sight armed on {}: {} pair slots, re-measure >= {} ms, <= {} pairs/frame (3 rays "
                     "each, no outer lock: PickObject read-locks the world itself), fresh {} ms, dropped after {} ms "
                     "unasked.",
                     REL::Module::get().version().string("."), kSlots, APMF_API::kLosRefreshMs,
                     APMF_API::kLosMaxPairsPerFrame, APMF_API::kLosFreshMs, APMF_API::kLosInterestMs);
    }

    bool Armed() { return g_armed.load(std::memory_order_acquire); }

    const char* NotArmedReason() {
        if (Armed()) return "armed";
        const char* r = g_notArmedReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    Reading Read(RE::FormID a_viewer, RE::FormID a_target, bool a_ask) {
        Reading r;
        if (!Armed()) {   // a bit that reached a seat without the service (a Repoint added it): say so
            r.verdict = APMF_API::kLos_Unsupported;
            return r;
        }
        if (!a_viewer || !a_target || a_viewer == a_target) return r;
        const std::uint64_t key = Key(a_viewer, a_target);
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (a_ask) g_asks.fetch_add(1, std::memory_order_relaxed);
        const std::size_t h = Home(key);
        for (std::size_t i = 0; i < kProbe; ++i) {
            Slot&         s = g_slots[(h + i) & (kSlots - 1)];
            std::uint64_t w = 0, m = 0;
            if (TryReadSlot(s, key, w, m)) {
                if (a_ask) s.askedMs.store(now, std::memory_order_relaxed);
                return Unpack(w, m, now);
            }
        }
        if (a_ask) {   // not tracked: hand the key to the main thread (lossy under a burst; re-asked next read)
            const std::uint32_t idx = g_ringIdx.fetch_add(1, std::memory_order_relaxed) & (kRing - 1);
            g_ring[idx].store(key, std::memory_order_release);
            g_queued.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }

    std::uint32_t FreshVerdict(const Reading& a_r) {
        if (a_r.verdict == APMF_API::kLos_Unsupported) return APMF_API::kLos_Unsupported;
        if (a_r.verdict == APMF_API::kLos_Unknown || a_r.ageMs > APMF_API::kLosFreshMs) return APMF_API::kLos_Unknown;
        return a_r.verdict;
    }

    std::uint32_t GetLineOfSight(RE::FormID a_viewer, RE::FormID a_target, APMF_API::APMF_LosInfo* a_out) {
        std::uint32_t verdict = APMF_API::kLos_Unknown;
        Reading       r;
        if (!Armed()) {
            verdict = APMF_API::kLos_Unsupported;
        } else if (!a_viewer || !a_target || a_viewer == a_target) {
            verdict = APMF_API::kLos_Unavailable;
            r.why   = APMF_API::kLosWhy_BadArgs;
        } else {
            r       = Read(a_viewer, a_target, true);
            verdict = FreshVerdict(r);
        }
        if (a_out && a_out->size >= sizeof(APMF_API::APMF_LosInfo)) {
            a_out->verdict     = verdict;
            a_out->ageMs       = r.ageMs;
            a_out->occludedRun = r.occRun;
            a_out->sample      = verdict == APMF_API::kLos_Visible ? r.sample : 0;
            a_out->why         = r.why;
        }
        return verdict;
    }

    Reading MeasureNow(RE::Actor* a_viewer, RE::Actor* a_target, bool& a_reused) {
        a_reused = false;
        Reading out;
        if (!Armed() || !a_viewer || !a_target || a_viewer == a_target) return out;
        const std::uint64_t key = Key(a_viewer->GetFormID(), a_target->GetFormID());
        const std::uint64_t now = apmf::clock::MonotonicMs();
        Slot*               s   = EnsureMain(key, now);
        if (s) {
            const std::uint64_t m = s->measuredMs.load(std::memory_order_relaxed);
            if (m != 0 && Since(now, m) < APMF_API::kLosRefreshMs) {
                a_reused = true;
                return Unpack(s->word.load(std::memory_order_relaxed), m, now);
            }
        }
        const RayOut ray = CastOwnRay(a_viewer, a_target);
        if (s) return Store(*s, key, ray, now);
        // Not trackable right now (table full): answer with the measurement, store nothing.
        g_measures.fetch_add(1, std::memory_order_relaxed);
        return Unpack(Pack(ray.verdict, ray.sample, ray.why, ray.verdict == APMF_API::kLos_Occluded ? 1 : 0), now,
                      now);
    }

    void Pump() {
        if (!Armed() || !apmf::hook::OnMainThread()) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();

        // 1. Pairs readers asked for that are not tracked yet.
        for (auto& cell : g_ring) {
            const std::uint64_t k = cell.exchange(0, std::memory_order_acq_rel);
            if (k != 0) EnsureMain(k, now);
        }

        // 2. Drop pairs nobody asked for; collect the stale ones.
        std::uint32_t idx[kSlots];
        std::size_t   n = 0;
        for (std::size_t i = 0; i < kSlots; ++i) {
            Slot&               s = g_slots[i];
            const std::uint64_t k = s.key.load(std::memory_order_relaxed);
            if (k == 0) continue;
            if (Since(now, s.askedMs.load(std::memory_order_relaxed)) > APMF_API::kLosInterestMs) {
                Publish(s, 0, 0, 0);
                s.askedMs.store(0, std::memory_order_relaxed);
                g_unavailLog.erase(k);
                g_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const std::uint64_t m = s.measuredMs.load(std::memory_order_relaxed);
            if (m == 0 || Since(now, m) >= APMF_API::kLosRefreshMs) idx[n++] = static_cast<std::uint32_t>(i);
        }

        // 3. Re-measure the oldest few (never-measured first).
        if (n > APMF_API::kLosMaxPairsPerFrame) {
            std::partial_sort(idx, idx + APMF_API::kLosMaxPairsPerFrame, idx + n, [](std::uint32_t a, std::uint32_t b) {
                return g_slots[a].measuredMs.load(std::memory_order_relaxed) <
                       g_slots[b].measuredMs.load(std::memory_order_relaxed);
            });
            n = APMF_API::kLosMaxPairsPerFrame;
        }
        for (std::size_t i = 0; i < n; ++i) {
            Slot&               s = g_slots[idx[i]];
            const std::uint64_t k = s.key.load(std::memory_order_relaxed);
            if (k != 0) MeasureKey(s, k, now);
        }

        Heartbeat(now);
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        for (auto& s : g_slots) {
            if (s.key.load(std::memory_order_relaxed) != 0) ++n;
            Publish(s, 0, 0, 0);
            s.askedMs.store(0, std::memory_order_relaxed);
        }
        for (auto& cell : g_ring) cell.store(0, std::memory_order_relaxed);
        g_unavailLog.clear();
        if (n != 0) spdlog::info("[los] {} -- dropped {} tracked line-of-sight pair(s).", a_why, n);
    }

    void NoteSeatHold() { g_seatHold.fetch_add(1, std::memory_order_relaxed); }
    void NoteSeatPass() { g_seatPass.fetch_add(1, std::memory_order_relaxed); }
    void NoteSeatStop() { g_seatStop.fetch_add(1, std::memory_order_relaxed); }

}
