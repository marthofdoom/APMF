#include "PCH.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate (ABI v12 gate probe)
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PackageData.h"
#include "core/PositionCast.h"   // ABI v11: PlaceMarker / DeleteMarker for kTravel_ToPosition legs
#include "core/Registry.h"
#include "channels/Travel.h"
#include "channels/Travel_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

    // ---- PASSIVE GATE PROBE 2 ([Travel] bGateProbe2) -----------------------------------
    // Probe 1 (above) could not tell a gate from anything else: every ACTIVATOR reads
    // GetOpenState == Closed, and no stall in the 2026-09-26 field sessions sat next to a
    // gate. Probe 2 asks the questions a reachability query would have to rely on, per
    // BLOCKED leg, and CHANGES NOTHING (reads, logs, forgets):
    //   (1) the destination: its position, whether it lies over a navmesh triangle, and
    //       the vertical gap to that triangle (a chest under the floor, an item upstairs);
    //   (2) the navmesh at the stall and at the destination: navmesh form id, triangle
    //       index and flags, and that mesh's doorPortals / closedDoors / obstacles /
    //       triangleToObstacleMap sizes, plus whether the triangle is a key of that map;
    //   (4) every DOOR / ACTIVATOR / Obstacle-flagged reference whose BOUND BOX lies within
    //       kP2SegMargin of the stall->destination segment (not a disc around the stall),
    //       with the base's plugin, and for doors and activators their behaviour-graph
    //       state (the OpenClose state machine's current state) and their Havok collision
    //       (in the physics world, layer, motion type, the body node's height over the
    //       reference origin -- a raised portcullis shows here).
    //   (3) the engine's own path goal is NOT read: no layout for MovementPathManager /
    //       BSPathingSolution exists anywhere, and the RE is its own round (STATUS).
    // Layout (all verified on 1.6.1170 AND 1.5.97; spec.json layout_facts, VERIFIED-ADDRESSES):
    //   BSNavmesh (NavMesh + 0x28): vertices 0x10 (0xC each), triangles 0x28 (0x10 each:
    //   u16 vertices[3], flags u16 @0xC), doorPortals 0x58, closedDoors 0x70, obstacles 0xD0,
    //   triangleToObstacleMap 0xE8 (a BSTHashMap<u16, ptr>*) -- the BSNavmesh destructor and
    //   GetEdgeVertices on both builds. CommonLib's BSNavmesh declares exactly these.
    //   Havok members come from the engine's own hkClassMember reflection (identical on both):
    //   hkbBehaviorGraph.rootGenerator 0x80 / rootGeneratorClone 0x90, hkbStateMachine
    //   currentStateId 0x80 / isActive 0x88 / states 0x90, StateInfo name 0x60 / stateId 0x68,
    //   hkbCharacter.behaviorGraph 0x58, hkpWorldObject world 0x10 / collisionFilterInfo 0x4C,
    //   hkpRigidBody motion type 0x160. BShkbAnimationGraph.behaviorGraph (0x208) is NOT
    //   disassembled: it is read only when it equals characterInstance(0xC0).behaviorGraph
    //   and carries the verified hkbBehaviorGraph vtable. Every object is vtable-checked
    //   against a self-check row BEFORE any member of it is read.
    // GAME THREAD (Poll, at a BLOCKED end), the same seat as probe 1. What makes the navmesh
    // read safe is game-thread serialisation, NOT the cell spinLock (held only for the mesh
    // array): obstacle cuts are copy-on-write (ObstacleTaskData builds a NEW NavMesh on a
    // worker, 1.6.1170 0x4CE690, and publishes it through the manager's queue), and each mesh
    // is held by a strong reference while it is read. Bounded: once per actor per kP2RepeatMs
    // at the same place, kP2TriBudget triangles + vertices per PROBE, kP2MaxRefs refs.
    constexpr float         kP2SegMargin = 160.0f;
    constexpr std::size_t   kP2MaxRefs   = 10;
    constexpr std::size_t   kP2TriBudget = 262144;
    constexpr std::uint64_t kP2RepeatMs  = 60000;

}

    std::atomic<bool> g_probe2Ready{ false };
    std::uintptr_t    g_vtAnimGraph      = 0;   // written once at Install, before g_probe2Ready
    std::uintptr_t    g_vtBehaviorGraph  = 0;
    std::uintptr_t    g_vtStateMachine   = 0;
    // Last probe-2 per actor (GAME THREAD only): when and where.
    std::unordered_map<RE::FormID, std::pair<std::uint64_t, RE::NiPoint3>> g_lastProbe2;

namespace {

    template <class T>
    T Rd(std::uintptr_t a_addr) {
        return *reinterpret_cast<const T*>(a_addr);
    }

    // The attached cells to search around a point: a_cell, plus (outdoors) the loaded
    // exterior cells under the corners of a 1024u square, as probe 1 does.
    void P2Cells(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_p, std::vector<RE::TESObjectCELL*>& a_out) {
        auto add = [&](RE::TESObjectCELL* c) {
            if (c && c->IsAttached() && std::find(a_out.begin(), a_out.end(), c) == a_out.end()) a_out.push_back(c);
        };
        add(a_cell);
        if (!a_cell || !a_cell->IsExteriorCell()) return;
        auto*       tes = RE::TES::GetSingleton();
        const auto* ws  = a_cell->GetRuntimeData().worldSpace;
        for (const float dx : { -kProbeRadius, 0.0f, kProbeRadius }) {
            for (const float dy : { -kProbeRadius, 0.0f, kProbeRadius }) {
                auto* c = tes ? tes->GetCell(RE::NiPoint3{ a_p.x + dx, a_p.y + dy, a_p.z }) : nullptr;
                if (c && c->IsExteriorCell() && c->GetRuntimeData().worldSpace == ws) add(c);
            }
        }
    }

    struct MeshSpot {
        RE::FormID    mesh      = 0;
        std::uint32_t tri       = 0xFFFFFFFFu;
        std::uint16_t triFlags  = 0;
        float         gap       = 0.0f;     // point z minus the triangle's z there (+ = above)
        bool          onMesh    = false;    // a triangle lies under/over the point in XY
        float         nearVert  = -1.0f;    // 3D distance to the nearest vertex (any mesh)
        std::uint32_t doorPortals = 0, closedDoors = 0, obstacles = 0;
        std::int64_t  obsMap    = -1;       // -1 == no map allocated
        bool          mapped    = false;    // `tri` is a key of triangleToObstacleMap
        std::uint32_t meshes    = 0;
        std::size_t   tris      = 0;
        bool          budgetHit = false;
    };

    // Locates a_p on the loaded navmesh of a_cells. Among the triangles whose XY contains
    // the point, the one with the smallest |vertical gap| wins. Reads only.
    void P2Locate(const std::vector<RE::TESObjectCELL*>& a_cells, const RE::NiPoint3& a_p, MeshSpot& o,
                  std::size_t& a_work) {
        float bestAbs  = std::numeric_limits<float>::max();
        float bestVert = std::numeric_limits<float>::max();
        for (auto* cell : a_cells) {
            auto&               rd = cell->GetRuntimeData();
            RE::BSSpinLockGuard lock(rd.spinLock);
            auto*               arr = rd.navMeshes;
            if (!arr) continue;
            for (const auto& meshPtr : arr->navMeshes) {
                RE::NavMesh* nm = meshPtr.get();
                if (!nm) continue;
                // A strong ref while read, through the BSNavmesh base (its intrusive count;
                // BSTSmartPointer<NavMesh> is ambiguous: NavMesh inherits two operator deletes).
                const RE::BSTSmartPointer<RE::BSNavmesh> hold{ static_cast<RE::BSNavmesh*>(nm) };
                const RE::BSNavmesh&                     bn = *hold;
                ++o.meshes;
                const auto& vs = bn.vertices;
                const std::uint32_t vcount = vs.size();
                for (const auto& v : vs) {
                    if (++a_work > kP2TriBudget) {
                        o.budgetHit = true;
                        break;
                    }
                    const float d = a_p.GetDistance(v.location);
                    if (d < bestVert) bestVert = d;
                }
                bool   hitHere = false;
                std::uint32_t hitTri = 0;
                for (std::uint32_t t = 0; t < bn.triangles.size(); ++t) {
                    if (o.budgetHit || ++a_work > kP2TriBudget) {
                        o.budgetHit = true;
                        break;
                    }
                    ++o.tris;
                    const auto& tr = bn.triangles[t];
                    if (tr.triangleFlags.all(RE::BSNavmeshTriangle::TriangleFlag::kDeleted)) continue;
                    if (tr.vertices[0] >= vcount || tr.vertices[1] >= vcount || tr.vertices[2] >= vcount) continue;
                    const auto& p0 = vs[tr.vertices[0]].location;
                    const auto& p1 = vs[tr.vertices[1]].location;
                    const auto& p2 = vs[tr.vertices[2]].location;
                    const float den = (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
                    if (std::fabs(den) < 1e-6f) continue;
                    const float a = ((p1.y - p2.y) * (a_p.x - p2.x) + (p2.x - p1.x) * (a_p.y - p2.y)) / den;
                    const float b = ((p2.y - p0.y) * (a_p.x - p2.x) + (p0.x - p2.x) * (a_p.y - p2.y)) / den;
                    const float c = 1.0f - a - b;
                    constexpr float eps = -1e-4f;
                    if (a < eps || b < eps || c < eps) continue;
                    const float gap = a_p.z - (a * p0.z + b * p1.z + c * p2.z);
                    if (std::fabs(gap) < bestAbs) {
                        bestAbs    = std::fabs(gap);
                        o.gap      = gap;
                        o.tri      = t;
                        o.triFlags = tr.triangleFlags.underlying();
                        o.onMesh   = true;
                        hitHere    = true;
                        hitTri     = t;
                    }
                }
                if (hitHere) {
                    o.mesh        = nm->GetFormID();
                    o.doorPortals = bn.doorPortals.size();
                    o.closedDoors = bn.closedDoors.size();
                    o.obstacles   = bn.obstacles.size();
                    o.obsMap      = -1;
                    o.mapped      = false;
                    if (const auto* map = bn.triangleToObstacleMap) {
                        o.obsMap = static_cast<std::int64_t>(map->size());
                        for (const auto& e : *map) {
                            if (e.first == hitTri) {
                                o.mapped = true;
                                break;
                            }
                        }
                    }
                }
                if (o.budgetHit) break;
            }
            if (o.budgetHit) break;
        }
        if (bestVert < std::numeric_limits<float>::max()) o.nearVert = bestVert;
    }

    std::string P2SpotText(const MeshSpot& s) {
        if (s.meshes == 0) return "NO NAVMESH loaded in the searched cells";
        std::string t = s.onMesh ? std::format("ON-MESH nav 0x{} tri #{} flags 0x{} gap {:+.0f}u; mesh doorPortals {} "
                                               "closedDoors {} obstacles {} obstacleMap {} triMapped={}",
                                               Hex(s.mesh), s.tri, Hex(s.triFlags, 4), s.gap, s.doorPortals,
                                               s.closedDoors, s.obstacles, s.obsMap, s.mapped)
                                 : std::string("OFF-MESH (no triangle under or over it in XY)");
        t += std::format("; nearest vertex {:.0f}u; {} mesh(es), {} triangle(s) read{}", s.nearVert, s.meshes, s.tris,
                         s.budgetHit ? " -- BUDGET HIT, result partial" : "");
        return t;
    }

    // The OpenClose state machine's current state, read from a vtable-verified hkbStateMachine.
    std::string P2Sm(std::uintptr_t a_sm) {
        if (!a_sm) return "none";
        if (Rd<std::uintptr_t>(a_sm) != g_vtStateMachine) return "not-a-state-machine";
        const auto cur    = Rd<std::int32_t>(a_sm + 0x80);
        const bool active = Rd<bool>(a_sm + 0x88);
        const auto data   = Rd<std::uintptr_t>(a_sm + 0x90);
        const auto n      = Rd<std::int32_t>(a_sm + 0x98);
        std::string name  = "?";
        for (std::int32_t i = 0; data && i < n && i < 64; ++i) {
            const auto si = Rd<std::uintptr_t>(data + 8 * static_cast<std::uintptr_t>(i));
            if (si && Rd<std::int32_t>(si + 0x68) == cur) {
                const auto np = Rd<std::uintptr_t>(si + 0x60) & ~static_cast<std::uintptr_t>(1);
                name          = np ? reinterpret_cast<const char*>(np) : "";
                break;
            }
        }
        return std::format("state {} '{}' active={} ({} states)", cur, name, active, n);
    }

    std::string P2Graph(RE::TESObjectREFR* a_ref) {
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        if (!a_ref->GetAnimationGraphManager(mgr) || !mgr) return "graph none";
        std::string out = std::format("graphs {}", mgr->graphs.size());
        for (std::uint32_t i = 0; i < mgr->graphs.size() && i < 2; ++i) {
            const auto g = reinterpret_cast<std::uintptr_t>(mgr->graphs[i].get());
            if (!g) continue;
            if (Rd<std::uintptr_t>(g) != g_vtAnimGraph) {
                out += std::format(" [{}] not a BShkbAnimationGraph", i);
                continue;
            }
            const auto bg = Rd<std::uintptr_t>(g + 0x208);
            if (!bg || bg != Rd<std::uintptr_t>(g + 0xC0 + 0x58)) {
                out += std::format(" [{}] behaviorGraph layout DISAGREES -- not read", i);
                continue;
            }
            if (Rd<std::uintptr_t>(bg) != g_vtBehaviorGraph) {
                out += std::format(" [{}] behaviorGraph vtable mismatch -- not read", i);
                continue;
            }
            out += std::format(" [{}] live (clone): {}; template: {}", i, P2Sm(Rd<std::uintptr_t>(bg + 0x90)),
                               P2Sm(Rd<std::uintptr_t>(bg + 0x80)));
        }
        return out;
    }

    // Havok collision of the reference's 3D: up to 3 bodies from the first 64 nodes.
    std::string P2Collision(RE::TESObjectREFR* a_ref) {
        auto* root = a_ref->Get3D();
        if (!root) return "3D not loaded";
        std::vector<RE::NiAVObject*> stack{ root };
        std::string out;
        int         nodes = 0, bodies = 0;
        while (!stack.empty() && nodes < 64 && bodies < 3) {
            auto* node = stack.back();
            stack.pop_back();
            ++nodes;
            if (auto* co = node->collisionObject.get()) {
                auto* nco  = co->AsBhkNiCollisionObject();
                auto* body = nco ? nco->body.get() : nullptr;
                const auto wo = body ? reinterpret_cast<std::uintptr_t>(body->referencedObject.get()) : 0;
                if (wo) {
                    ++bodies;
                    const bool rigid  = body->AsBhkRigidBody() != nullptr;
                    const auto filter = Rd<std::uint32_t>(wo + 0x4C);
                    out += std::format("{}body on '{}' inWorld={} layer {} motion {} dz {:+.0f}", out.empty() ? "" : "; ",
                                       node->name.c_str(), Rd<std::uintptr_t>(wo + 0x10) != 0, filter & 0x7F,
                                       rigid ? std::format("{}", Rd<std::uint8_t>(wo + 0x160)) : std::string("n/a"),
                                       node->world.translate.z - root->world.translate.z);
                }
            }
            if (auto* n = node->AsNode()) {
                auto& ch = n->GetChildren();
                for (std::uint16_t i = 0; i < ch.capacity(); ++i)
                    if (auto* c = ch[i].get()) stack.push_back(c);
            }
        }
        return out.empty() ? std::format("no collision body in {} node(s)", nodes) : out;
    }

}

    // GAME THREAD (Poll, at a BLOCKED end). See the section header. a_dest is the leg's
    // destination (a reference, or null for a point leg); a_destPos its position.
    void GateProbe2(RE::FormID a_id, RE::Actor* a_actor, const RE::NiPoint3& a_stall, RE::TESObjectREFR* a_dest,
                    RE::FormID a_destId, bool a_haveDest, const RE::NiPoint3& a_destPos) {
        if (!g_probe2Ready.load(std::memory_order_acquire)) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (auto it = g_lastProbe2.find(a_id); it != g_lastProbe2.end() && now - it->second.first < kP2RepeatMs &&
                                               it->second.second.GetDistance(a_stall) < 256.0f)
            return;   // probe 1's "not repeated" line already says so
        g_lastProbe2[a_id] = { now, a_stall };
        const auto t0 = std::chrono::steady_clock::now();

        auto* cell = a_actor->GetParentCell();
        std::vector<RE::TESObjectCELL*> stallCells;
        P2Cells(cell, a_stall, stallCells);
        std::size_t work = 0;   // one triangle + vertex budget for the whole probe
        MeshSpot    stallSpot;
        P2Locate(stallCells, a_stall, stallSpot, work);

        MeshSpot                        destSpot;
        std::vector<RE::TESObjectCELL*> destCells;
        if (a_haveDest) {
            RE::TESObjectCELL* dc = a_dest ? a_dest->GetParentCell() : nullptr;
            if (!dc && cell && cell->IsExteriorCell()) {
                auto* tes = RE::TES::GetSingleton();
                dc        = tes ? tes->GetCell(a_destPos) : nullptr;
            }
            P2Cells(dc ? dc : cell, a_destPos, destCells);
            P2Locate(destCells, a_destPos, destSpot, work);
        }

        // (4) references whose bound box is near the stall->destination segment.
        struct SegHit {
            float                            dist;
            float                            along;
            bool                             box;
            RE::NiPointer<RE::TESObjectREFR> ref;
        };
        std::vector<SegHit> hits;
        float               segLen = 0.0f;
        if (a_haveDest) {
            std::vector<RE::TESObjectCELL*> cells = stallCells;
            for (auto* c : destCells)
                if (std::find(cells.begin(), cells.end(), c) == cells.end()) cells.push_back(c);
            const RE::NiPoint3 seg   = a_destPos - a_stall;
            segLen                   = seg.Length();
            const RE::NiPoint3 mid   = a_stall + seg * 0.5f;
            const std::uint32_t n    = std::clamp(static_cast<std::uint32_t>(segLen / 32.0f) + 1u, 2u, 256u);
            // Collect under the cell lock (types and flags only), measure after it.
            std::vector<RE::NiPointer<RE::TESObjectREFR>> cand;
            for (auto* c : cells) {
                c->ForEachReferenceInRange(mid, segLen * 0.5f + kP2SegMargin + 1024.0f, [&](RE::TESObjectREFR& r) {
                    const auto* b = r.GetBaseObject();
                    if (!b) return RE::BSContainer::ForEachResult::kContinue;
                    const bool gateType = b->GetFormType() == RE::FormType::Door ||
                                          b->GetFormType() == RE::FormType::Activator;
                    if (gateType || (b->GetFormFlags() & (1u << 25)) != 0)
                        cand.emplace_back(RE::NiPointer<RE::TESObjectREFR>(&r));
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            for (auto& rp : cand) {
                {
                    auto&       r    = *rp;
                    const auto* b    = r.GetBaseObject();
                    if (!b) continue;
                    auto*       root = r.Get3D();
                    const auto& bd   = b->boundData;
                    const bool  box  = root && (bd.boundMin.x != bd.boundMax.x || bd.boundMin.y != bd.boundMax.y ||
                                              bd.boundMin.z != bd.boundMax.z);
                    const RE::NiTransform inv = box ? root->world.Invert() : RE::NiTransform{};
                    const float           scl = box ? root->world.scale : 1.0f;
                    float best = std::numeric_limits<float>::max(), bestT = 0.0f;
                    for (std::uint32_t k = 0; k < n; ++k) {
                        const float        t = static_cast<float>(k) / static_cast<float>(n - 1);
                        const RE::NiPoint3 p = a_stall + seg * t;
                        float              d;
                        if (box) {
                            const RE::NiPoint3 l = inv * p;
                            const float dx = std::max({ bd.boundMin.x - l.x, 0.0f, l.x - bd.boundMax.x });
                            const float dy = std::max({ bd.boundMin.y - l.y, 0.0f, l.y - bd.boundMax.y });
                            const float dz = std::max({ bd.boundMin.z - l.z, 0.0f, l.z - bd.boundMax.z });
                            d              = std::sqrt(dx * dx + dy * dy + dz * dz) * scl;
                        } else {
                            d = p.GetDistance(r.GetPosition());
                        }
                        if (d < best) {
                            best  = d;
                            bestT = t;
                        }
                    }
                    if (best <= kP2SegMargin) hits.push_back(SegHit{ best, bestT * segLen, box, rp });
                }
            }
            std::sort(hits.begin(), hits.end(), [](const SegHit& x, const SegHit& y) { return x.along < y.along; });
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();

        // The compact line, then the detail block.
        spdlog::info("[travel-gate2] 0x{} BLOCKED at {:.0f},{:.0f},{:.0f} -> dest 0x{} {} | stall {} | dest {} | {} "
                     "door/activator/obstacle ref(s) within {:.0f}u of the {:.0f}u segment | {} us",
                     Hex(a_id), a_stall.x, a_stall.y, a_stall.z, Hex(a_destId),
                     a_haveDest ? std::format("at {:.0f},{:.0f},{:.0f}", a_destPos.x, a_destPos.y, a_destPos.z)
                                : std::string("(position unknown)"),
                     stallSpot.onMesh ? std::format("nav 0x{}#{} gap {:+.0f}", Hex(stallSpot.mesh), stallSpot.tri, stallSpot.gap)
                                      : std::string("OFF-MESH"),
                     !a_haveDest ? std::string("-")
                     : destSpot.onMesh ? std::format("nav 0x{}#{} gap {:+.0f}", Hex(destSpot.mesh), destSpot.tri, destSpot.gap)
                                       : std::string("OFF-MESH"),
                     hits.size(), kP2SegMargin, segLen, us);
        spdlog::info("[travel-gate2] 0x{}   stall: {}.", Hex(a_id), P2SpotText(stallSpot));
        if (a_haveDest) spdlog::info("[travel-gate2] 0x{}   dest:  {}.", Hex(a_id), P2SpotText(destSpot));
        for (std::size_t i = 0; i < hits.size() && i < kP2MaxRefs; ++i) {
            auto*       r    = hits[i].ref.get();
            const auto* base = r->GetBaseObject();
            if (!base) continue;
            const auto* file = base->GetFile(0);
            const auto  rf   = base->GetFormFlags();
            const bool  gate = base->GetFormType() == RE::FormType::Door || base->GetFormType() == RE::FormType::Activator;
            spdlog::info("[travel-gate2] 0x{}   #{} ref 0x{} base 0x{} [{}] {} recordFlags 0x{} obstacle={} navmeshFilter={} "
                         "GetOpenState={} disabled={} | {:.0f}u from the segment ({}), {:.0f}u along it | {} | {}",
                         Hex(a_id), i, Hex(r->GetFormID()), Hex(base->GetFormID()),
                         file ? std::string(file->GetFilename()) : std::string("?"), GateTypeName(base), Hex(rf, 8),
                         (rf & (1u << 25)) != 0, (rf & (1u << 26)) != 0,
                         OpenStateName(RE::BGSOpenCloseForm::GetOpenState(r)), r->IsDisabled(), hits[i].dist,
                         hits[i].box ? "bound box" : "origin, no 3D", hits[i].along, gate ? P2Graph(r) : std::string("-"),
                         P2Collision(r));
        }
        if (hits.size() > kP2MaxRefs)
            spdlog::info("[travel-gate2] 0x{}   ... {} more not listed.", Hex(a_id), hits.size() - kP2MaxRefs);
    }
