#include "PCH.h"
#include "core/SelfBlast.h"
#include "core/Sightline.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>

// ============================================================================
// ch.25 SELF BLAST, PARTY HOLD: the engine facts. See core/SelfBlast.h.
// Reads only. No hook, no engine write, no address of its own: the ray is core/Sightline.cpp's
// verified bhkWorld::PickObject / GetbhkWorld / WorldScale rows (Sightline armed or no ray).
// ============================================================================

namespace apmf::selfblast {

    namespace {
        std::atomic<const RE::Setting*> g_unitsPerFoot{ nullptr };
        std::atomic<const char*>        g_why{ "before kDataLoaded (fMagicUnitsPerFoot is read there)" };

        float UnitsPerFoot() {
            const auto* s = g_unitsPerFoot.load(std::memory_order_acquire);
            return s ? s->data.f : std::numeric_limits<float>::infinity();   // unarmed claims are refused; never 0
        }

        bool Harms(const RE::BGSExplosion* a_x) {
            return a_x && (!(a_x->data.damage <= 0.0f) || a_x->formEnchanting ||
                           a_x->data.flags.any(RE::BGSExplosionData::Flag::kChain));
        }

        // What stops a blast here: STATIC, ANIMSTATIC, TREES, TERRAIN, GROUND. The engine's ray also stops at bodies
        // and props; those are left out (the safe side: who stands behind one is still counted).
        bool WorldLayer(RE::COL_LAYER a_l) {
            return a_l == RE::COL_LAYER::kStatic || a_l == RE::COL_LAYER::kAnimStatic || a_l == RE::COL_LAYER::kTrees ||
                   a_l == RE::COL_LAYER::kTerrain || a_l == RE::COL_LAYER::kGround;
        }

        // Keeps the nearest hit on world geometry and steps past everything else (the caster's own body first of
        // all). The closest-hit layout bhkWorld::PickObject expects at bhkPickData +0xA8: the same contract as
        // core/Sightline.cpp LosCollector, proven there on all three builds.
        class WallCollector : public RE::hkpRayHitCollector {
        public:
            void AddRayHit(const RE::hkpCdBody& a_body, const RE::hkpShapeRayCastCollectorOutput& a_hit) override {
                if (!(a_hit.hitFraction < rayHit.hitFraction)) return;
                const RE::hkpCdBody* root = &a_body;
                while (root->parent) root = root->parent;
                const auto* coll = static_cast<const RE::hkpCollidable*>(root);
                if (coll->broadPhaseHandle.type == 2 ||   // a phantom (an overlap volume)
                    !WorldLayer(static_cast<RE::COL_LAYER>(coll->broadPhaseHandle.collisionFilterInfo & 0x7F)))
                    return;
                rayHit.normal         = a_hit.normal;
                rayHit.hitFraction    = a_hit.hitFraction;
                rayHit.extraInfo      = a_hit.extraInfo;
                rayHit.shapeKey       = a_hit.shapeKey;
                rayHit.rootCollidable = coll;
                earlyOutHitFraction   = a_hit.hitFraction;
            }
            ~WallCollector() override = default;

            RE::hkpWorldRayCastOutput rayHit;   // +0x10
        };
        static_assert(offsetof(WallCollector, rayHit) == 0x10, "PickObject reads the collector's rayHit at +0x10");
        static_assert(offsetof(RE::bhkPickData, rayHitCollectorA8) == 0xA8);

        float Height(RE::Actor* a) {   // as core/Sightline.cpp reads it (Actor::GetHeight writes a cache)
            const float h = (a->GetBoundMax().z - a->GetBoundMin().z) * a->GetBaseHeight();
            return std::isfinite(h) && h > 1.0f ? h : 120.0f;
        }
    }

    const char* Install() {
        auto* gsc = RE::GameSettingCollection::GetSingleton();
        auto* s   = gsc ? gsc->GetSetting("fMagicUnitsPerFoot") : nullptr;
        if (!s || !std::isfinite(s->data.f) || s->data.f <= 0.0f) {
            g_why.store("the game setting fMagicUnitsPerFoot was not found or is not a positive number");
        } else {
            g_unitsPerFoot.store(s, std::memory_order_release);
            g_why.store(nullptr);
        }
        return g_why.load();
    }

    const char* NotArmedReason() { return g_why.load(); }

    Blast BlastOfItem(RE::MagicItem* a_item, bool a_dual) {
        if (!a_item || a_item->GetDelivery() != RE::MagicSystem::Delivery::kSelf) return {};
        constexpr std::size_t kMax = 64;
        EffectFacts           facts[kMax];
        std::size_t           n = 0;
        for (auto* eff : a_item->effects) {
            if (!eff || !eff->baseEffect) continue;
            const auto& d = eff->baseEffect->data;
            EffectFacts f;
            f.hostile     = eff->baseEffect->IsHostile();
            f.detrimental = eff->baseEffect->IsDetrimental();
            f.noArea      = d.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoArea);
            f.areaFeet    = eff->effectItem.area;
            // The explosion the engine places (1.6.1170 0x146140, scale 0x1462B0): the dual-cast one when dual
            // casting and the record has one, else the effect's own; the dual-cast scale applies to both unless
            // the dual-cast record's own explosion does not inherit it.
            const RE::BGSExplosion* x     = d.explosion;
            float                   scale = 1.0f;
            if (a_dual) {
                const auto* dd       = d.dualCastData;
                const bool  ownsDual = dd && dd->data.pExplosion;
                if (ownsDual) x = dd->data.pExplosion;
                if (!ownsDual || dd->data.flags.any(RE::BGSDualCastDataDEF::Flags::kExplosionInheritScale))
                    scale = d.dualCastScale;
            }
            if (Harms(x)) {
                f.explosionHarms = true;
                f.explosionUnits = x->data.radius * (scale > 1.0f ? scale : 1.0f);
            }
            if (n == kMax) {   // past the bound: fold into the last, never dropped
                auto& l = facts[kMax - 1];
                l.hostile |= f.hostile;
                l.detrimental |= f.detrimental;
                l.noArea &= f.noArea;
                l.areaFeet = std::max(l.areaFeet, f.areaFeet);
                l.explosionHarms |= f.explosionHarms;
                l.explosionUnits = std::max(l.explosionUnits, f.explosionUnits);
            } else {
                facts[n++] = f;
            }
        }
        return BlastOf(facts, n, UnitsPerFoot(), a_item->IgnoreLOS());
    }

    bool WallBetween(RE::Actor* a_caster, RE::Actor* a_other) {
        if (!apmf::sightline::Armed() || !a_caster || !a_other || !a_other->GetCharController()) return false;
        auto* cell  = a_caster->GetParentCell();
        auto* ocell = a_other->GetParentCell();
        if (!cell || !cell->IsAttached() || !ocell || !ocell->IsAttached()) return false;
        auto* world = cell->GetbhkWorld();
        if (!world || (ocell != cell && ocell->GetbhkWorld() != world)) return false;
        std::uint32_t info = 0;
        a_other->GetCollisionFilterInfo(info);
        const std::uint32_t filter = (info & 0xFFFF0000u) | static_cast<std::uint32_t>(RE::COL_LAYER::kSpellExplosion);

        const RE::NiPoint3 cp = a_caster->GetPosition(), op = a_other->GetPosition();
        const float        ch = Height(a_caster);
        const RE::NiPoint3 to{ op.x, op.y, op.z + Height(a_other) * 0.5f };
        const float        yaw = a_caster->GetAngleZ();
        const RE::NiPoint3 from[3] = {
            { cp.x, cp.y, cp.z + ch * 0.6f },
            { cp.x, cp.y, cp.z + ch * 0.95f },
            { cp.x + std::sin(yaw) * 56.0f, cp.y + std::cos(yaw) * 56.0f, cp.z + ch * 0.75f },   // an outstretched hand
        };
        const float scale = RE::bhkWorld::GetWorldScale();
        for (const auto& f : from) {
            WallCollector   collector;
            RE::bhkPickData pick;
            pick.rayInput.from                        = f * scale;
            pick.rayInput.to                          = to * scale;
            pick.rayInput.enableShapeCollectionFilter = false;
            pick.rayInput.filterInfo                  = filter;
            pick.rayHitCollectorA8 = reinterpret_cast<RE::hkpClosestRayHitCollector*>(&collector);
            world->PickObject(pick);   // takes the world's read lock itself (core/Sightline.h LOCK SCOPE)
            if (pick.unkC0 || !collector.rayHit.rootCollidable) return false;   // not cast, or the way is open
        }
        return true;
    }

}
