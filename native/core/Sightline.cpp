#include "PCH.h"
#include "core/Sightline.h"
#include "core/Allowance.h"   // SeatVerified(), RuntimeSupported(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"   // OwnLosCastPairs: the standing cast claims that keep their pair measured
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
        // Each ray ends this far short of its point, so the TARGET's own capsule is never what
        // blocks it (a kCharController ray stops on any actor capsule). Review SEV-3 (6d84bab): a
        // fixed 48u cleared a humanoid but not a giant, a mammoth or a dragon, whose capsule
        // reaches far past 48u from its axis -- every ray hit the target itself and read OCCLUDED.
        // The margin is now sized per target from its OWN bound box: the horizontal half-diagonal
        // of GetBoundMin/GetBoundMax (Character slots 0x73/0x74, the same box and the same scale
        // TESObjectREFR::GetHeight uses -- disassembly, all three builds: min = c - e, max = c + e
        // of the process's bound object, middleHigh +0x180, centre +0x18 / half-extent +0x24),
        // times the reference's base scale, plus kMarginPad, never below kMinTargetMargin. The
        // half-DIAGONAL (not the larger half-extent) because the box is not rotated with the
        // actor, so the capsule can sit across either axis.
        constexpr float kMinTargetMargin = 48.0f;    // the humanoid value MFO field-proved
        constexpr float kMarginPad       = 16.0f;    // clearance past the bound's edge
        constexpr float kMaxTargetMargin = 1024.0f;  // a bound that reads larger than this is nonsense: clamp

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
        // fix/apmf-los-false-occlusion: what the own ray's hits were (relaxed; heartbeat).
        std::atomic<std::uint64_t> g_targetBody{ 0 }, g_volumePass{ 0 }, g_viewerPass{ 0 }, g_claimWarm{ 0 },
            g_occDetailSuppressed{ 0 };

        // Sightline.TESHavokUtilities.FindCollidableRef verified (LOG ONLY: names the blocker).
        std::atomic<bool> g_refLookupOk{ false };

        // The standing own-line-of-sight cast claims are re-read this often (main thread, Pump):
        // well inside kLosInterestMs, so a pair stays tracked for exactly as long as its claim.
        constexpr std::uint64_t kClaimWarmMs = 250;
        std::uint64_t                                    g_lastClaimWarmMs = 0;
        std::vector<std::pair<RE::FormID, RE::FormID>> g_claimPairs;   // main thread only (reused buffer)

        constexpr std::uint64_t kTransitionLogMs = 2000;   // [los] VISIBLE/OCCLUDED lines, per pair

        // Main-thread only (no lock): per-pair log throttles.
        std::unordered_map<std::uint64_t, std::uint64_t> g_unavailLog;
        std::unordered_map<std::uint64_t, std::uint64_t> g_transLog;
        std::unordered_map<std::uint64_t, std::uint64_t> g_occDetailLog;   // per pair: the OCCLUDED detail line
        std::atomic<std::uint64_t>                       g_transSuppressed{ 0 }, g_syncDeferred{ 0 };

        // MeasureNow's per-frame budget (review SEV-4): Pump() advances the frame, MeasureNow
        // counts its own ray-casting measurements in it. Main thread only.
        std::uint64_t g_frame = 0, g_syncFrame = ~0ull;
        std::uint32_t g_syncThisFrame = 0;
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

        // The collision layer names, from the engine's own COLL records (Skyrim.esm, BNAM = the
        // index). For the log only.
        const char* LayerName(std::uint32_t a_layer) {
            static constexpr const char* kNames[] = {
                "UNIDENTIFIED", "STATIC", "ANIMSTATIC", "TRANSPARENT", "CLUTTER", "WEAPON", "PROJECTILE",
                "SPELL", "BIPED", "TREES", "PROPS", "WATER", "TRIGGER", "TERRAIN", "TRAP", "NONCOLLIDABLE",
                "CLOUDTRAP", "GROUND", "PORTAL", "DEBRIS_SMALL", "DEBRIS_LARGE", "ACOUSTIC_SPACE", "ACTORZONE",
                "PROJECTILEZONE", "GASTRAP", "SHELLCASING", "TRANSPARENT_SMALL", "INVISIBLE_WALL",
                "TRANSPARENT_SMALL_ANIM", "WARD", "CHARCONTROLLER", "STAIRHELPER", "DEADBIP", "BIPED_NO_CC",
                "AVOIDBOX", "COLLISIONBOX", "CAMERASPHERE", "DOORDETECTION", "CONEPROJECTILE", "CAMERA",
                "ITEMPICKER", "LOS", "PATHINGPICK", "CUSTOMPICK1", "CUSTOMPICK2", "SPELLEXPLOSION",
                "DROPPINGPICK", "DEADACTORZONE", "TRIGGER_FALLINGTRAP", "NAVCUT", "CRITTER", "SPELLTRIGGER",
                "LIVING_AND_DEAD_ACTORS", "DETECTION", "TRAP_TRIGGER",
            };
            return a_layer < std::size(kNames) ? kNames[a_layer] : "?";
        }

        // hkpTypedBroadPhaseHandle::type (collidable +0x24 +4 = +0x28) for a PHANTOM. Disassembly
        // (AE): the engine's own pick helpers read the same byte -- 0xE82F7C `cmp [coll+0x28],1`
        // returns the owner as a rigid body, 0xE82FAC `cmp [coll+0x28],2` returns it as a phantom
        // (also 0x3FF3FB).
        constexpr std::int8_t kBroadPhasePhantom = 2;

        // THE HIT FILTER (fix/apmf-los-false-occlusion). A ray-hit collector the engine's own
        // bhkWorld::PickObject casts with when bhkPickData +0xA8 holds one (disassembly, all three
        // builds: AE 0xE866F6 / SE 0xDA7716 / 1.7.104 0x104C016 load +0xA8; PickObject resets it
        // as a closest-hit collector -- +0x08 earlyOut, +0x20 hitFraction, +0x60 root collidable --
        // casts with hkpWorld::castRay(input, collector), then copies +0x10..+0x60 into
        // rayOutput when +0x60 is set). Havok calls slot 0 for every hit, in any order:
        // hkpClosestRayHitCollector's slot 0 (AE 0xB1D9F0 / SE 0xA5BE20 / 1.7.104 0xB3A200) is
        // addRayHit(this, cdBody, hitInfo), walks cdBody->parent (+0x18) to the root collidable,
        // and keeps the nearest. This one keeps the nearest hit that can BLOCK, and steps past:
        //   * the VIEWER's own body (its collision system group -- the filter group already
        //     excludes it; this is the belt to that brace, counted so a miss shows), and
        //   * a PHANTOM that is not a character controller: trigger boxes, acoustic spaces, actor
        //     zones, trap triggers, gas clouds. A phantom is an overlap volume; nothing walking
        //     (and no spell) is stopped by one. The character-controller layer collides with all
        //     of them (Skyrim.esm COLL L_CHARCONTROLLER: TRIGGER, ACOUSTIC_SPACE, ACTORZONE,
        //     TRAP_TRIGGER, GASTRAP, CLOUDTRAP ...), which is how a draugr tomb's wake triggers
        //     could read as walls. An actor's capsule (CHARCONTROLLER, phantom or not), a ragdoll
        //     bone and every solid body still block.
        // The TARGET's own body is kept as a hit: the caller reads it as "the ray reached it".
        class LosCollector : public RE::hkpRayHitCollector {
        public:
            void AddRayHit(const RE::hkpCdBody& a_body, const RE::hkpShapeRayCastCollectorOutput& a_hit) override {
                if (!(a_hit.hitFraction < rayHit.hitFraction)) return;   // not nearer than the kept hit
                const RE::hkpCdBody* root = &a_body;
                while (root->parent) root = root->parent;
                const auto*         coll  = static_cast<const RE::hkpCollidable*>(root);
                const std::uint32_t info  = coll->broadPhaseHandle.collisionFilterInfo;
                const std::uint32_t layer = info & 0x7F;
                if (viewerGroup != 0 && (info >> 16) == viewerGroup) {
                    ++viewerHits;
                    return;
                }
                if (coll->broadPhaseHandle.type == kBroadPhasePhantom &&
                    layer != static_cast<std::uint32_t>(RE::COL_LAYER::kCharController)) {
                    ++volumes;
                    if (a_hit.hitFraction < volumeFraction) {
                        volumeFraction = a_hit.hitFraction;
                        volume         = coll;
                    }
                    return;
                }
                rayHit.normal         = a_hit.normal;
                rayHit.hitFraction    = a_hit.hitFraction;
                rayHit.extraInfo      = a_hit.extraInfo;
                rayHit.shapeKey       = a_hit.shapeKey;
                rayHit.rootCollidable = coll;
                earlyOutHitFraction   = a_hit.hitFraction;   // prune what lies beyond it
            }
            ~LosCollector() override = default;

            RE::hkpWorldRayCastOutput  rayHit;              // +0x10: the closest-hit layout PickObject expects
            std::uint32_t              viewerGroup = 0;
            std::uint32_t              viewerHits  = 0;
            std::uint32_t              volumes     = 0;
            float                      volumeFraction = 1.0f;
            const RE::hkpCollidable*   volume      = nullptr;
        };
        static_assert(offsetof(LosCollector, rayHit) == 0x10, "PickObject reads the collector's rayHit at +0x10");
        static_assert(offsetof(RE::bhkPickData, rayHitCollectorA8) == 0xA8, "PickObject's closest-hit collector slot");
        static_assert(offsetof(RE::hkpCdBody, parent) == 0x18 && offsetof(RE::hkpCollidable, broadPhaseHandle) == 0x24,
                      "the hkpCdBody / hkpCollidable fields the collector reads");

        // One ray's account, for the OCCLUDED detail line.
        struct RayNote {
            enum : std::uint8_t { kNotCast = 0, kClear, kTargetBody, kBlocked, kSkipped };
            std::uint8_t             state    = kNotCast;
            std::int8_t              bpType   = 0;
            std::uint32_t            layer    = 0;
            std::uint32_t            group    = 0;
            float                    hitDist  = 0.0f;   // eye -> the hit
            float                    total    = 0.0f;   // eye -> the sample point
            const RE::hkpCollidable* coll     = nullptr;
            std::uint32_t            volumes  = 0;
            float                    volDist  = 0.0f;
            const RE::hkpCollidable* volume   = nullptr;
            std::uint32_t            viewerHits = 0;
        };

        struct RayOut {
            std::uint32_t verdict = APMF_API::kLos_Unavailable;
            std::uint32_t sample  = 0;
            std::uint32_t why     = APMF_API::kLosWhy_None;
            float         margin  = 0.0f;   // the end margin used (0 = no ray reached that point); for the log
            std::uint32_t viewerGroup = 0, targetGroup = 0;
            RayNote       rays[3];
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
            out.viewerGroup = info >> 16;
            // The target's own collision group (its controller's; its ragdoll shares it): a ray
            // whose nearest blocking hit carries it has reached the target. 0 when the target has
            // no controller -- then no hit is read as the target's (the end margin still applies).
            if (a_target->GetCharController()) {
                std::uint32_t tinfo = 0;
                a_target->GetCollisionFilterInfo(tinfo);
                out.targetGroup = tinfo >> 16;
            }

            // The eye: the engine's own origin (Character slot 0xC2: position + eye height). No
            // facing is read anywhere -- `dir` is discarded.
            RE::NiPoint3 eye{}, dir{};
            a_viewer->GetEyeVector(eye, dir, false);

            // The target: feet / torso / head, and the end margin, from ONE read of its bound box
            // (Character slots 0x73 / 0x74) and its base scale -- exactly the inputs of the
            // read-only TESObjectREFR::GetHeight() const (bound height x scale). Actor::GetHeight
            // is not used: it writes the high process's cached height.
            const RE::NiPoint3 feet  = a_target->GetPosition();
            const RE::NiPoint3 bmin  = a_target->GetBoundMin();
            const RE::NiPoint3 bmax  = a_target->GetBoundMax();
            const float        base  = a_target->GetBaseHeight();
            float              h     = (bmax.z - bmin.z) * base;
            if (!std::isfinite(h) || h <= 1.0f) h = kNominalHeight;
            const float hx = 0.5f * (bmax.x - bmin.x), hy = 0.5f * (bmax.y - bmin.y);
            float       margin = std::sqrt(hx * hx + hy * hy) * base + kMarginPad;
            if (!std::isfinite(margin)) margin = kMinTargetMargin;
            margin = std::clamp(margin, kMinTargetMargin, kMaxTargetMargin);
            out.margin = margin;
            const RE::NiPoint3 samples[3] = {
                { feet.x, feet.y, feet.z + kFeetLift },
                { feet.x, feet.y, feet.z + h * kTorsoFrac },
                { feet.x, feet.y, feet.z + h * kHeadFrac },
            };

            const float scale = RE::bhkWorld::GetWorldScale();   // verified row Sightline.bhkWorld.WorldScale
            bool        skipped = false;
            for (std::uint32_t i = 0; i < 3; ++i) {
                RayNote&           note = out.rays[i];
                const RE::NiPoint3 seg  = samples[i] - eye;
                const float        len  = seg.Length();
                if (!std::isfinite(len)) continue;
                note.total = len;
                if (len <= margin) {   // inside the target's own bound: nothing can stand in the gap
                    note.state  = RayNote::kClear;
                    out.verdict = APMF_API::kLos_Visible;
                    out.sample  = i + 1;
                    return out;
                }
                const float        f  = (len - margin) / len;
                const RE::NiPoint3 to{ eye.x + seg.x * f, eye.y + seg.y * f, eye.z + seg.z * f };

                LosCollector collector;
                collector.viewerGroup = out.viewerGroup;
                RE::bhkPickData pick;
                pick.rayInput.from                        = eye * scale;
                pick.rayInput.to                          = to * scale;
                pick.rayInput.enableShapeCollectionFilter = false;
                pick.rayInput.filterInfo                  = filter;
                pick.rayHitCollectorA8 = reinterpret_cast<RE::hkpClosestRayHitCollector*>(&collector);
                world->PickObject(pick);   // takes the world's read lock itself (header: LOCK SCOPE)
                g_rays.fetch_add(1, std::memory_order_relaxed);
                if (pick.unkC0) {          // the engine's early-out: nothing was cast (header: PICK SKIPPED)
                    note.state = RayNote::kSkipped;
                    skipped    = true;
                    continue;
                }
                const float rayLen = len - margin;
                note.volumes       = collector.volumes;
                note.volume        = collector.volume;
                note.volDist       = collector.volumeFraction * rayLen;
                note.viewerHits    = collector.viewerHits;
                if (collector.volumes) g_volumePass.fetch_add(collector.volumes, std::memory_order_relaxed);
                if (collector.viewerHits) g_viewerPass.fetch_add(collector.viewerHits, std::memory_order_relaxed);
                const RE::hkpCollidable* hit = collector.rayHit.rootCollidable;
                if (!hit) {
                    note.state  = RayNote::kClear;
                    out.verdict = APMF_API::kLos_Visible;
                    out.sample  = i + 1;
                    return out;
                }
                note.coll    = hit;
                note.layer   = hit->broadPhaseHandle.collisionFilterInfo & 0x7F;
                note.group   = hit->broadPhaseHandle.collisionFilterInfo >> 16;
                note.bpType  = hit->broadPhaseHandle.type;
                note.hitDist = collector.rayHit.hitFraction * rayLen;
                if (out.targetGroup != 0 && note.group == out.targetGroup) {   // the ray reached the target's body
                    note.state = RayNote::kTargetBody;
                    g_targetBody.fetch_add(1, std::memory_order_relaxed);
                    out.verdict = APMF_API::kLos_Visible;
                    out.sample  = i + 1;
                    return out;
                }
                note.state = RayNote::kBlocked;
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

        // MAIN THREAD, right after the pick (same frame, no yield): who owns a collidable.
        std::string DescribeCollidable(const RE::hkpCollidable* a_coll, const RayOut& a_ray, RE::FormID a_viewer,
                                       RE::FormID a_target) {
            if (!a_coll) return "none";
            const std::uint32_t info  = a_coll->broadPhaseHandle.collisionFilterInfo;
            const std::uint32_t layer = info & 0x7F, group = info >> 16;
            const char* body = a_coll->broadPhaseHandle.type == kBroadPhasePhantom ? "phantom"
                             : a_coll->broadPhaseHandle.type == 1               ? "body"
                                                                                  : "other";
            const char* who = (a_ray.viewerGroup && group == a_ray.viewerGroup)   ? "the VIEWER's own"
                            : (a_ray.targetGroup && group == a_ray.targetGroup) ? "the TARGET's own"
                                                                                  : nullptr;
            std::string ref = "ref ?";
            if (g_refLookupOk.load(std::memory_order_relaxed)) {
                auto* r = RE::TESHavokUtilities::FindCollidableRef(*a_coll);   // verified row (log only)
                if (!r) {
                    ref = "no ref";
                } else {
                    const RE::FormID fid  = r->GetFormID();
                    auto*            base = r->GetBaseObject();
                    const char*      nm   = r->GetName();
                    ref = fmt::format("ref 0x{} {} '{}'", Hex(fid), base ? RE::FormTypeToString(base->GetFormType()) : "?",
                                      nm ? nm : "");
                    if (auto* actor = r->As<RE::Actor>()) {
                        const char* rel = fid == a_viewer ? "the viewer"
                                        : fid == a_target ? "the target"
                                        : actor->IsPlayerRef() ? "the PLAYER"
                                        : actor->IsPlayerTeammate() ? "a player TEAMMATE"
                                        : actor->IsDead() ? "a dead actor"
                                                          : "another actor";
                        ref += fmt::format(" [actor: {}]", rel);
                    }
                }
            }
            return fmt::format("L_{}({}) {} group {}{}{} {}", LayerName(layer), layer, body, group, who ? " = " : "",
                               who ? who : "", ref);
        }

        // MAIN THREAD: WHAT stopped each of the three rays of an OCCLUDED measurement (every
        // OCCLUDED one, not only transitions; at most one line per pair per kTransitionLogMs, the
        // rest counted in the heartbeat). Called in the same frame as the pick, no yield between.
        void LogOccludedDetail(std::uint64_t a_key, const RayOut& a_ray, std::uint64_t a_nowMs) {
            if (g_occDetailLog.size() > 1024) g_occDetailLog.clear();
            auto& last = g_occDetailLog[a_key];
            if (last != 0 && Since(a_nowMs, last) < kTransitionLogMs) {
                g_occDetailSuppressed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            last = a_nowMs;
            const RE::FormID v = KeyViewer(a_key), t = KeyTarget(a_key);
            std::string      rays;
            std::uint32_t    volumes = 0, viewerHits = 0;
            const RayNote*   nearestVol = nullptr;
            for (std::uint32_t i = 0; i < 3; ++i) {
                const RayNote& n = a_ray.rays[i];
                volumes += n.volumes;
                viewerHits += n.viewerHits;
                if (n.volume && (!nearestVol || n.volDist < nearestVol->volDist)) nearestVol = &n;
                rays += fmt::format("{}{}: ", i ? "; " : "", SampleName(i + 1));
                if (n.state == RayNote::kBlocked)
                    rays += fmt::format("{} at {:.0f}u of {:.0f}u", DescribeCollidable(n.coll, a_ray, v, t),
                                        n.hitDist, n.total);
                else
                    rays += n.state == RayNote::kNotCast ? "not cast" : "?";
            }
            spdlog::info("[los] 0x{} -> 0x{}: OCCLUDED by -- {} (hit distance of eye-to-point distance; end margin "
                         "{:.0f}u; viewer group {}, target group {}). Passed through: {} volume hit(s){}{}, {} of the "
                         "viewer's own body. (per pair, every {} s at most)",
                         Hex(v), Hex(t), rays, a_ray.margin, a_ray.viewerGroup, a_ray.targetGroup, volumes,
                         nearestVol ? ", nearest " : "",
                         nearestVol ? fmt::format("{} at {:.0f}u", DescribeCollidable(nearestVol->volume, a_ray, v, t),
                                                  nearestVol->volDist)
                                    : std::string{},
                         viewerHits, kTransitionLogMs / 1000);
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
                // Transition-only: a stable verdict at refresh cadence would flood the log. And
                // (review SEV-4) at most one transition line per pair per kTransitionLogMs: a
                // borderline line flickering at the 250 ms refresh would still flood it. The
                // suppressed ones are counted in the heartbeat; the table always holds the truth.
                if (g_transLog.size() > 1024) g_transLog.clear();
                auto& last = g_transLog[a_key];
                if (Since(a_nowMs, last) < kTransitionLogMs) {
                    g_transSuppressed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    last = a_nowMs;
                    if (a_ray.verdict == APMF_API::kLos_Visible)
                        spdlog::info("[los] 0x{} -> 0x{}: VISIBLE (own ray, {} clear, end margin {:.0f}u){}", Hex(v),
                                     Hex(t), SampleName(a_ray.sample), a_ray.margin,
                                     a_ray.sample >= 1 && a_ray.sample <= 3 &&
                                             a_ray.rays[a_ray.sample - 1].state == RayNote::kTargetBody
                                         ? " -- the ray's nearest hit was the target's own body"
                                         : "");
                    else
                        spdlog::info("[los] 0x{} -> 0x{}: OCCLUDED ({}, end margin {:.0f}u)", Hex(v), Hex(t),
                                     a_ray.why == APMF_API::kLosWhy_OtherWorld ? "target in another havok world"
                                                                               : "own ray, all 3 rays blocked",
                                     a_ray.margin);
                }
            }
            if (a_ray.verdict == APMF_API::kLos_Occluded && a_ray.why != APMF_API::kLosWhy_OtherWorld)
                LogOccludedDetail(a_key, a_ray, a_nowMs);
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
                       stop = x(g_seatStop), quiet = x(g_transSuppressed), deferred = x(g_syncDeferred),
                       tbody = x(g_targetBody), vol = x(g_volumePass), vself = x(g_viewerPass), warm = x(g_claimWarm),
                       occQuiet = x(g_occDetailSuppressed);
            if (tracked == 0 && asks == 0 && meas == 0 && deferred == 0) return;   // nothing asked for: stay quiet
            spdlog::info("[los] heartbeat {} s: tracked {} pair(s); asks {} (new {}), measured {} ({} rays): visible {}, "
                         "occluded {}, unavailable {} (pick skipped {}); dropped idle {}, table full {}; seats: "
                         "passed {}, held/paused {}, channels stopped {}; SenseActor sight over the per-frame cap {}; "
                         "transition lines rate-limited away {}. Rays: nearest hit the target's own body (read as "
                         "reached) {}, volume hits passed through {}, viewer's own body passed {}; OCCLUDED detail "
                         "lines rate-limited away {}; standing own-line-of-sight cast claims kept warm {} pair-ask(s).",
                         kHeartbeatMs / 1000, tracked, asks, queued, meas, rays, vis, occ, un, skip, drop, full, pass,
                         hold, stop, deferred, quiet, tbody, vol, vself, occQuiet, warm);
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
            // LOG ONLY (fix/apmf-los-false-occlusion): names the reference an OCCLUDED ray stopped
            // on. Never decides a verdict, so a refusal drops only the name from the log line.
            g_refLookupOk.store(apmf::allowance::SeatVerified(
                                    REL::Relocation<std::uintptr_t>{ RE::Offset::TESHavokUtilities::FindCollidableRef }.address(),
                                    "Sightline.TESHavokUtilities.FindCollidableRef (log only)"),
                                std::memory_order_relaxed);
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

    Reading MeasureNow(RE::Actor* a_viewer, RE::Actor* a_target, bool& a_reused, bool& a_deferred) {
        a_reused   = false;
        a_deferred = false;
        Reading out;
        if (!Armed() || !a_viewer || !a_target || a_viewer == a_target) return out;
        const std::uint64_t key = Key(a_viewer->GetFormID(), a_target->GetFormID());
        const std::uint64_t now = apmf::clock::MonotonicMs();
        Slot*               s   = EnsureMain(key, now);   // also queues the pair for the pump
        if (s) {
            const std::uint64_t m = s->measuredMs.load(std::memory_order_relaxed);
            if (m != 0 && Since(now, m) < APMF_API::kLosRefreshMs) {
                a_reused = true;
                return Unpack(s->word.load(std::memory_order_relaxed), m, now);
            }
        }
        // The per-frame cap (review SEV-4): past kLosMaxSyncPairsPerFrame ray-casting measurements
        // this frame, no ray. A FRESH stored verdict still answers; otherwise UNKNOWN (not seen),
        // and the pump measures the pair (EnsureMain above marked it asked-for).
        if (g_syncFrame != g_frame) {
            g_syncFrame     = g_frame;
            g_syncThisFrame = 0;
        }
        if (g_syncThisFrame >= APMF_API::kLosMaxSyncPairsPerFrame) {
            a_deferred = true;
            g_syncDeferred.fetch_add(1, std::memory_order_relaxed);
            if (s) {
                const Reading r = Unpack(s->word.load(std::memory_order_relaxed),
                                         s->measuredMs.load(std::memory_order_relaxed), now);
                if (r.verdict != APMF_API::kLos_Unknown && r.ageMs <= APMF_API::kLosFreshMs) return r;
            }
            return out;
        }
        ++g_syncThisFrame;
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
        ++g_frame;   // a new frame: MeasureNow's per-frame budget starts over

        // 1. Pairs readers asked for that are not tracked yet.
        for (auto& cell : g_ring) {
            const std::uint64_t k = cell.exchange(0, std::memory_order_acq_rel);
            if (k != 0) EnsureMain(k, now);
        }

        // 1b. The STANDING own-line-of-sight cast claims ask for their pair themselves
        // (fix/apmf-los-false-occlusion). Seat 0x06's polls alone kept a pair asked-for only
        // while the engine polled it, and its gaps (2-5 s in field session 1006c) ran past
        // kLosInterestMs: the pair was dropped between polls and every poll read UNKNOWN -- a
        // cast refused indefinitely. Now a pair is tracked from the claim's publish (measured on
        // the next pump, never-measured first) for exactly as long as the claim stands, and
        // re-measured every kLosRefreshMs like any asked-for pair, so the seat's kLosFreshMs
        // read finds a measurement at most ~kLosRefreshMs old (+ the per-frame queue).
        if (g_lastClaimWarmMs == 0 || Since(now, g_lastClaimWarmMs) >= kClaimWarmMs) {
            g_lastClaimWarmMs = now;
            g_claimPairs.clear();
            apmf::ControlMap::Get().OwnLosCastPairs(g_claimPairs);
            for (const auto& [viewer, target] : g_claimPairs) {
                EnsureMain(Key(viewer, target), now);   // inserts, or stamps askedMs
                g_claimWarm.fetch_add(1, std::memory_order_relaxed);
            }
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
                g_transLog.erase(k);
                g_occDetailLog.erase(k);
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
        g_transLog.clear();
        g_occDetailLog.clear();
        g_claimPairs.clear();
        g_lastClaimWarmMs = 0;
        if (n != 0) spdlog::info("[los] {} -- dropped {} tracked line-of-sight pair(s).", a_why, n);
    }

    void NoteSeatHold() { g_seatHold.fetch_add(1, std::memory_order_relaxed); }
    void NoteSeatPass() { g_seatPass.fetch_add(1, std::memory_order_relaxed); }
    void NoteSeatStop() { g_seatStop.fetch_add(1, std::memory_order_relaxed); }

}
