#include "PCH.h"
#include "core/Hook.h"
#include "core/Log.h"
#include "core/SpaceQuery.h"

#include <algorithm>
#include <chrono>
#include <cmath>

// See core/SpaceQuery.h and APMF_API.h ("ABI v11: SPACE QUERIES") for the contract.

namespace apmf::spacequery {

    namespace {

        using apmf::log::Hex;
        using APMF_API::APMF_HostileQuery;
        using APMF_API::APMF_SpaceQuery;
        using APMF_API::APMF_SpaceResult;

        std::atomic<bool> g_ready{ false };

        // Geometry constants. Every one is named in APMF_API.h's FindEmptySpace text.
        constexpr float kChestHeight   = 64.0f;    // the walk ray and the down ray start this far above the feet
        constexpr float kKneeHeight    = 32.0f;    // the clearance rays run this far above the found ground
        constexpr float kRingStep      = 48.0f;    // a clearance-ring ground sample must sit within this of the point
        constexpr float kActorRadius   = 32.0f;    // added to the clearance for the live-actor test
        constexpr float kActorHeight   = 256.0f;   // an actor this far above/below the point cannot occupy it
        constexpr float kMaxDistance   = 2048.0f;
        constexpr float kMinClearance  = 16.0f;
        constexpr float kMaxClearance  = 512.0f;
        constexpr float kDefaultDrop   = 128.0f;
        constexpr float kMaxDrop       = 1024.0f;
        constexpr float kMaxRadius     = 4096.0f;

        bool Finite(float a) { return std::isfinite(a); }

        // The layers a point may stand on. Static, terrain and ground are the three the
        // engine itself accepts for a Target Location placement (CommonLib's
        // MagicCaster::TestProjectilePlacement: filter layers 0x01, 0x0D, 0x11). The stair
        // helper is added on purpose: it is the collision a character controller stands
        // on over stairs, and a char-controller ray meets it before the steps under it.
        bool IsGroundLayer(RE::COL_LAYER a_layer) {
            return a_layer == RE::COL_LAYER::kStatic || a_layer == RE::COL_LAYER::kTerrain ||
                   a_layer == RE::COL_LAYER::kGround || a_layer == RE::COL_LAYER::kStairHelper;
        }

        bool IsActorLayer(RE::COL_LAYER a_layer) {
            return a_layer == RE::COL_LAYER::kCharController || a_layer == RE::COL_LAYER::kBiped ||
                   a_layer == RE::COL_LAYER::kBipedNoCC || a_layer == RE::COL_LAYER::kDeadBip;
        }

        struct RayHit {
            bool          hit      = false;
            float         fraction = 1.0f;
            RE::COL_LAYER layer    = RE::COL_LAYER::kUnidentified;
        };

        // One closest-hit ray. Caller holds world->worldLock (read). bhkWorld::PickObject
        // (vfunc 0x33) resets rayOutput and casts into it when no collector is set, so
        // hitFraction and rootCollidable are the closest hit (ADDENDUM 2026-09-23).
        RayHit Cast(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
                    std::uint32_t a_filter) {
            const float    scale = RE::bhkWorld::GetWorldScale();
            RE::bhkPickData pick;
            pick.rayInput.from                        = a_from * scale;
            pick.rayInput.to                          = a_to * scale;
            pick.rayInput.enableShapeCollectionFilter = false;
            pick.rayInput.filterInfo                  = a_filter;
            a_world->PickObject(pick);
            RayHit r;
            if (pick.rayOutput.HasHit()) {
                r.hit      = true;
                r.fraction = pick.rayOutput.hitFraction;
                r.layer    = pick.rayOutput.rootCollidable->GetCollisionLayer();
            }
            return r;
        }

        // A live, enabled actor standing within `a_radius` (horizontal) of `a_point`
        // and within kActorHeight vertically. Returns its FormID or 0. Player included.
        RE::FormID FindOccupant(const RE::NiPoint3& a_point, float a_radius) {
            RE::FormID found = 0;
            const float r2 = a_radius * a_radius;
            const auto  test = [&](RE::Actor& a) {
                if (a.IsDead() || a.IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
                const auto p  = a.GetPosition();
                const float dx = p.x - a_point.x, dy = p.y - a_point.y;
                if (dx * dx + dy * dy <= r2 && std::fabs(p.z - a_point.z) <= kActorHeight) {
                    found = a.GetFormID();
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            };
            if (auto* pl = RE::ProcessLists::GetSingleton()) pl->ForEachHighActor(test);
            if (!found) {
                // The player is not assumed to be in the high list; testing it twice is harmless here.
                if (auto* player = RE::PlayerCharacter::GetSingleton()) test(*player);
            }
            return found;
        }

        const char* StatusName(std::uint32_t a_status) {
            switch (a_status) {
            case APMF_API::kQuery_Ok:            return "Ok";
            case APMF_API::kQuery_NotMainThread: return "NotMainThread";
            case APMF_API::kQuery_BadArgs:       return "BadArgs";
            case APMF_API::kQuery_Unsupported:   return "Unsupported";
            case APMF_API::kQuery_NoOrigin:      return "NoOrigin";
            case APMF_API::kQuery_NoWorld:       return "NoWorld";
            case APMF_API::kQuery_Blocked:       return "Blocked";
            case APMF_API::kQuery_NoGround:      return "NoGround";
            case APMF_API::kQuery_NotGround:     return "NotGround";
            case APMF_API::kQuery_Occupied:      return "Occupied";
            case APMF_API::kQuery_NoClearance:   return "NoClearance";
            case APMF_API::kQuery_Ledge:         return "Ledge";
            case APMF_API::kQuery_Failed:        return "Failed";
            default:                             return "?";
            }
        }

    }

    void Install() {
        if (g_ready.load(std::memory_order_relaxed)) return;
        if (REL::Module::IsVR()) {
            spdlog::warn("[space] queries NOT armed -- VR runtime (the pick and hostility calls were read on 1.6.1170 "
                         "and 1.5.97 only). Every v11 query answers Unsupported.");
            return;
        }
        const auto game = REL::Module::get().version();
        if (game != REL::Version{ 1, 6, 1170, 0 } && game != REL::Version{ 1, 5, 97, 0 }) {
            spdlog::warn("[space] queries NOT armed -- runtime {} is not verified (only 1.6.1170 and 1.5.97 are). "
                         "Every v11 query answers Unsupported.", game.string("."));
            return;
        }
        g_ready.store(true, std::memory_order_release);
        spdlog::info("[space] queries armed on {}: FindEmptySpace, FindHostilesInSpace (main thread only, read-only).",
                     game.string("."));
    }

    std::uint32_t FindEmptySpace(const APMF_SpaceQuery* a_q, APMF_SpaceResult* a_out) {
        // No output we may write -> nothing to report into; return the status only.
        if (!a_out || a_out->size < sizeof(APMF_SpaceResult)) {
            spdlog::warn("[space] FindEmptySpace REFUSED: the result pointer is null or its size is below the v11 "
                         "layout ({} bytes).", sizeof(APMF_SpaceResult));
            return APMF_API::kQuery_BadArgs;
        }
        a_out->status = APMF_API::kQuery_Failed;
        a_out->x = a_out->y = a_out->z = 0.0f;
        a_out->detail = 0;

        // Read the id for the log only once `size` says the caller's struct reaches it.
        const RE::FormID originId =
            (a_q && a_q->size >= offsetof(APMF_SpaceQuery, origin) + sizeof(RE::FormID)) ? a_q->origin : 0;
        const auto finish = [&](std::uint32_t a_status, std::uint32_t a_detail, std::string_view a_why) {
            a_out->status = a_status;
            a_out->detail = a_detail;
            if (a_status == APMF_API::kQuery_Ok) {
                // Success is logged at most once a second (review F6): a client may ask
                // every tick. Every refusal below stays one WARN line per call.
                static auto s_last = std::chrono::steady_clock::time_point{};   // main thread only
                const auto  now    = std::chrono::steady_clock::now();
                if (now - s_last >= std::chrono::seconds(1)) {
                    s_last = now;
                    spdlog::info("[space] FindEmptySpace origin 0x{} -> Ok at {:.0f},{:.0f},{:.0f} ({}) (rate-limited "
                                 "1/s)", Hex(originId), a_out->x, a_out->y, a_out->z, a_why);
                }
            } else {
                spdlog::warn("[space] FindEmptySpace origin 0x{} -> {} (detail 0x{}): {}", Hex(originId),
                             StatusName(a_status), Hex(a_detail), a_why);
            }
            return a_status;
        };

        if (!g_ready.load(std::memory_order_acquire))
            return finish(APMF_API::kQuery_Unsupported, 0, "queries are not armed on this runtime (or before kDataLoaded)");
        if (!apmf::hook::OnMainThread())
            return finish(APMF_API::kQuery_NotMainThread, 0, "called off the main (player-Update) thread; nothing done");
        if (!a_q || a_q->size < sizeof(APMF_SpaceQuery) || a_q->reserved != 0)
            return finish(APMF_API::kQuery_BadArgs, 0, "query null, size below the v11 layout, or reserved != 0");
        if (!Finite(a_q->distance) || !(a_q->distance > 0.0f) || !Finite(a_q->clearance) || !(a_q->clearance > 0.0f) ||
            !Finite(a_q->maxDrop) || a_q->maxDrop < 0.0f)
            return finish(APMF_API::kQuery_BadArgs, 0, "distance and clearance must be > 0, maxDrop >= 0, all finite");

        const std::uint32_t flags = a_q->flags;
        const bool fromPoint   = (flags & APMF_API::kSpaceFlag_OriginIsPoint) != 0;
        const bool useHeading  = (flags & APMF_API::kSpaceFlag_UseHeading) != 0;
        const bool clampToWall = (flags & APMF_API::kSpaceFlag_ClampToWall) != 0;
        if (fromPoint && (!Finite(a_q->originX) || !Finite(a_q->originY) || !Finite(a_q->originZ)))
            return finish(APMF_API::kQuery_BadArgs, 0, "kSpaceFlag_OriginIsPoint with a non-finite point");
        if (useHeading && !Finite(a_q->heading))
            return finish(APMF_API::kQuery_BadArgs, 0, "kSpaceFlag_UseHeading with a non-finite heading");

        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(originId);
        if (!ref || ref->IsDeleted() || ref->IsDisabled())
            return finish(APMF_API::kQuery_NoOrigin, 0, "origin is not a live reference");
        auto* cell = ref->GetParentCell();
        if (!cell || !cell->IsAttached())
            return finish(APMF_API::kQuery_NoWorld, 0, "origin's cell is not attached");
        auto* world = cell->GetbhkWorld();
        if (!world) return finish(APMF_API::kQuery_NoWorld, 0, "origin's cell has no havok world");

        // The rays skip the origin actor's own capsule: same non-zero system group
        // (MFO's field-proven Sightline recipe). A non-actor origin has no capsule.
        std::uint32_t filter = static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);
        if (auto* actor = ref->As<RE::Actor>()) {
            std::uint32_t info = 0;
            if (auto* cc = actor->GetCharController()) cc->GetCollisionFilterInfo(info);
            filter |= (info >> 16) << 16;
        }

        const RE::NiPoint3 feet    = fromPoint ? RE::NiPoint3{ a_q->originX, a_q->originY, a_q->originZ } : ref->GetPosition();
        const float        heading = useHeading ? a_q->heading : ref->GetAngleZ();
        const RE::NiPoint3 dir{ std::sin(heading), std::cos(heading), 0.0f };
        float       distance  = std::min(a_q->distance, kMaxDistance);
        const float clearance = std::clamp(a_q->clearance, kMinClearance, kMaxClearance);
        const float maxDrop   = a_q->maxDrop > 0.0f ? std::min(a_q->maxDrop, kMaxDrop) : kDefaultDrop;

        RE::NiPoint3 ground{};
        {
            RE::BSReadLockGuard lock(world->worldLock);

            // 1. The walk: chest height, origin -> point.
            const RE::NiPoint3 chest{ feet.x, feet.y, feet.z + kChestHeight };
            const RE::NiPoint3 walkTo{ chest.x + dir.x * distance, chest.y + dir.y * distance, chest.z };
            if (const auto w = Cast(world, chest, walkTo, filter); w.hit) {
                const float walked = w.fraction * distance - clearance;
                if (!clampToWall || walked <= 0.0f) {
                    return finish(APMF_API::kQuery_Blocked, static_cast<std::uint32_t>(w.layer),
                                  std::format("something (layer {}) {:.0f}u along a {:.0f}u walk{}",
                                              static_cast<std::uint32_t>(w.layer), w.fraction * distance, distance,
                                              clampToWall ? ", too close to clamp" : ""));
                }
                distance = walked;
            }

            // 2. The ground: straight down from the walk's end to feet - maxDrop.
            const RE::NiPoint3 downFrom{ chest.x + dir.x * distance, chest.y + dir.y * distance, chest.z };
            const RE::NiPoint3 downTo{ downFrom.x, downFrom.y, feet.z - maxDrop };
            const auto g = Cast(world, downFrom, downTo, filter);
            if (!g.hit)
                return finish(APMF_API::kQuery_NoGround, 0,
                              std::format("nothing within {:.0f}u below the origin's feet at the point", maxDrop));
            ground = { downFrom.x, downFrom.y, downFrom.z + (downTo.z - downFrom.z) * g.fraction };
            if (!IsGroundLayer(g.layer)) {
                if (IsActorLayer(g.layer)) {
                    if (const auto who = FindOccupant(ground, clearance + kActorRadius))
                        return finish(APMF_API::kQuery_Occupied, who, "the ground ray landed on an actor");
                }
                return finish(APMF_API::kQuery_NotGround, static_cast<std::uint32_t>(g.layer),
                              std::format("the ground ray hit layer {}, not static/terrain/ground/stairs",
                                          static_cast<std::uint32_t>(g.layer)));
            }

            // 3. Clearance: eight level rays at knee height.
            const RE::NiPoint3 knee{ ground.x, ground.y, ground.z + kKneeHeight };
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.78539816f;   // 45 degrees
                const RE::NiPoint3 to{ knee.x + std::sin(a) * clearance, knee.y + std::cos(a) * clearance, knee.z };
                if (const auto c = Cast(world, knee, to, filter); c.hit)
                    return finish(APMF_API::kQuery_NoClearance, static_cast<std::uint32_t>(c.layer),
                                  std::format("layer {} within {:.0f}u of the point", static_cast<std::uint32_t>(c.layer),
                                              c.fraction * clearance));
            }

            // 4. The ring: level ground at four points on the clearance circle.
            for (int i = 0; i < 4; ++i) {
                const float a = static_cast<float>(i) * 1.57079633f;   // 90 degrees
                const float x = ground.x + std::sin(a) * clearance, y = ground.y + std::cos(a) * clearance;
                const RE::NiPoint3 from{ x, y, ground.z + kRingStep };
                const RE::NiPoint3 to{ x, y, ground.z - kRingStep };
                const auto r = Cast(world, from, to, filter);
                if (!r.hit || !IsGroundLayer(r.layer))
                    return finish(APMF_API::kQuery_Ledge, r.hit ? static_cast<std::uint32_t>(r.layer) : 0,
                                  r.hit ? std::format("the clearance ring meets layer {}, not ground",
                                                      static_cast<std::uint32_t>(r.layer))
                                        : std::format("no ground within {:.0f}u at the clearance ring (a drop)",
                                                      kRingStep));
            }
        }

        // 5. Nobody standing there (outside the havok lock: this reads actors only).
        if (const auto who = FindOccupant(ground, clearance + kActorRadius))
            return finish(APMF_API::kQuery_Occupied, who, "a live actor inside the clearance");

        a_out->x = ground.x;
        a_out->y = ground.y;
        a_out->z = ground.z;
        return finish(APMF_API::kQuery_Ok, 0,
                      std::format("{:.0f}u along heading {:.2f}, clearance {:.0f}", distance, heading, clearance));
    }

    std::uint32_t FindHostilesInSpace(const APMF_HostileQuery* a_q, RE::FormID* a_outActors, std::uint32_t a_capacity,
                                      std::uint32_t* a_outCount, std::uint32_t* a_outTotal) {
        if (a_outCount) *a_outCount = 0;
        if (a_outTotal) *a_outTotal = 0;
        const RE::FormID sideId =
            (a_q && a_q->size >= offsetof(APMF_HostileQuery, side) + sizeof(RE::FormID)) ? a_q->side : 0;
        const auto refuse = [&](std::uint32_t a_status, std::string_view a_why) {
            spdlog::warn("[space] FindHostilesInSpace side 0x{} -> {}: {}", Hex(sideId), StatusName(a_status), a_why);
            return a_status;
        };

        if (!g_ready.load(std::memory_order_acquire))
            return refuse(APMF_API::kQuery_Unsupported, "queries are not armed on this runtime (or before kDataLoaded)");
        if (!apmf::hook::OnMainThread())
            return refuse(APMF_API::kQuery_NotMainThread, "called off the main (player-Update) thread; nothing done");
        if (!a_q || a_q->size < sizeof(APMF_HostileQuery) || !a_outCount || (a_capacity > 0 && !a_outActors))
            return refuse(APMF_API::kQuery_BadArgs, "query null or short, outCount null, or capacity > 0 with no array");
        if (!Finite(a_q->x) || !Finite(a_q->y) || !Finite(a_q->z) || !Finite(a_q->radius) || !(a_q->radius > 0.0f))
            return refuse(APMF_API::kQuery_BadArgs, "centre must be finite and radius > 0");

        auto* side = RE::TESForm::LookupByID<RE::Actor>(sideId);
        if (!side || !side->Is3DLoaded() || !side->GetParentCell())
            return refuse(APMF_API::kQuery_NoOrigin, "side is not a loaded actor");

        const float        radius = std::min(a_q->radius, kMaxRadius);
        const float        r2     = radius * radius;
        const RE::NiPoint3 centre{ a_q->x, a_q->y, a_q->z };
        auto* const        ws   = side->GetWorldspace();
        auto* const        cell = side->GetParentCell();

        auto* const player = RE::PlayerCharacter::GetSingleton();
        std::vector<std::pair<float, RE::FormID>> hits;
        const auto consider = [&](RE::Actor& a) {
            if (&a == side || a.IsDead() || a.IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            if (ws ? a.GetWorldspace() != ws : a.GetParentCell() != cell) return RE::BSContainer::ForEachResult::kContinue;
            const auto  p  = a.GetPosition();
            const float dx = p.x - centre.x, dy = p.y - centre.y, dz = p.z - centre.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) return RE::BSContainer::ForEachResult::kContinue;
            // The engine's own test (Offset::Actor::GetHostileToActor, SE 36537 / AE 37537;
            // Papyrus Actor.IsHostileToActor jumps straight to it), asked from the candidate.
            if (a.IsHostileToActor(side)) hits.emplace_back(d2, a.GetFormID());
            return RE::BSContainer::ForEachResult::kContinue;
        };
        // The player is considered exactly once: skipped inside the high-list pass (in
        // case a runtime lists it there) and tested on its own after.
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            pl->ForEachHighActor([&](RE::Actor& a) {
                return (player && &a == player) ? RE::BSContainer::ForEachResult::kContinue : consider(a);
            });
        }
        if (player) consider(*player);

        std::sort(hits.begin(), hits.end());
        const std::uint32_t total = static_cast<std::uint32_t>(hits.size());
        const std::uint32_t n     = std::min({ total, a_capacity, APMF_API::kMaxHostileResults });
        for (std::uint32_t i = 0; i < n; ++i) a_outActors[i] = hits[i].second;
        *a_outCount = n;
        if (a_outTotal) *a_outTotal = total;

        // Success is logged at most once a second: a client may ask every tick.
        static auto s_last = std::chrono::steady_clock::time_point{};   // main thread only
        const auto  now    = std::chrono::steady_clock::now();
        if (now - s_last >= std::chrono::seconds(1)) {
            s_last = now;
            spdlog::info("[space] FindHostilesInSpace side 0x{} at {:.0f},{:.0f},{:.0f} r{:.0f} -> {} hostile(s), {} "
                         "written{} (rate-limited 1/s)", Hex(sideId), centre.x, centre.y, centre.z, radius, total, n,
                         n > 0 ? std::format(", nearest 0x{}", Hex(a_outActors[0])) : std::string{});
        }
        return APMF_API::kQuery_Ok;
    }

}
