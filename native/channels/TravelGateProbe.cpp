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

    // ---- ABI v12: the PASSIVE GATE PROBE -------------------------------------------
    // Answers the two unknowns the loot design (2026-09-24, section 1d) needs before any
    // reachability work: (a) what the thing a follower stalls at IS (a DOOR portal, or an
    // ACTIVATOR with the Obstacle / NavMesh-filter record flags, like a lever portcullis),
    // and (b) whether the engine's own open state and events see it open. It CHANGES
    // NOTHING: it reads, logs and forgets (CLAUDE.md "probes must be fully passive").
    //   * At a BLOCKED end: every DOOR and ACTIVATOR reference within kProbeRadius of the
    //     stall, nearest first, capped at kProbeMaxRefs: base form, type, record flags
    //     (Obstacle 1<<25, NavMesh filter 1<<26), BGSOpenCloseForm::GetOpenState,
    //     disabled, distance.
    //   * Then, for kWatchMs, an OpenClose or Activate event whose reference is one of those
    //     or lies within kWatchRadius of the stall is logged (who, what, opened), and the
    //     watched references' GetOpenState is read again on the main thread right away and
    //     kRereadDelayMs later (a lever's gate animates for a few seconds).
    //   Bounded: at most one probe per actor per kProbeRepeatMs at the same place,
    //   kWatchMax live watches, kWatchLinesMax event lines per watch.
    // Engine calls: GetOpenState (1.6.1170 id 14288 0x1D4A50 / 1.5.97 id 14180 0x189750:
    // rcx = the ref, reads its base at +0x40 and switches on the base form type, answering
    // for ACTI/CONT/DOOR), ScriptEventSourceHolder::GetSingleton (id 14298 0x1D5230 / id
    // 14108 0x186790; the OpenClose source is +0x8F0, confirmed by SendOpenCloseEvent id
    // 14299/14190 `add rcx, 0x8F0`; Activate is +0x58, the offset the engine's own senders
    // use), TES::GetCell (the verified PositionCast row). All three are self-check rows;
    // the probe arms only when every one verifies on an exact 1.6.1170 / 1.5.97 build.
    constexpr std::size_t   kProbeMaxRefs   = 8;
    constexpr std::uint64_t kProbeRepeatMs  = 60000;
    constexpr float         kWatchRadius    = 2048.0f;
    constexpr std::size_t   kWatchMax       = 8;
    constexpr std::uint32_t kWatchLinesMax  = 24;
    constexpr std::uint64_t kRereadDelayMs  = 4000;

}

    std::atomic<bool> g_probeReady{ false };

    std::mutex                 g_watchMx;          // guards g_watches (sinks run on engine threads)
    std::vector<GateWatch>     g_watches;
    std::atomic<std::uint32_t> g_watchCount{ 0 };  // the sinks' relaxed pre-gate
    // Last probe per actor (GAME THREAD only): when and where.
    std::unordered_map<RE::FormID, std::pair<std::uint64_t, RE::NiPoint3>> g_lastProbe;

    const char* OpenStateName(RE::BGSOpenCloseForm::OPEN_STATE a_s) {
        switch (a_s) {
        case RE::BGSOpenCloseForm::OPEN_STATE::kNone:    return "none";
        case RE::BGSOpenCloseForm::OPEN_STATE::kOpen:    return "Open";
        case RE::BGSOpenCloseForm::OPEN_STATE::kOpening: return "Opening";
        case RE::BGSOpenCloseForm::OPEN_STATE::kClosed:  return "Closed";
        case RE::BGSOpenCloseForm::OPEN_STATE::kClosing: return "Closing";
        default:                                         return "?";
        }
    }

namespace {

    // The "space" a reference is in: its interior cell, or its exterior worldspace. Plain
    // member reads only (safe on the sinks' threads).
    RE::FormID SpaceOf(const RE::TESObjectREFR* a_ref) {
        const auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
        if (!cell) return 0;
        if (cell->IsInteriorCell()) return cell->GetFormID();
        const auto* ws = cell->GetRuntimeData().worldSpace;
        return ws ? ws->GetFormID() : 0;
    }

}

    const char* GateTypeName(const RE::TESBoundObject* a_base) {
        if (!a_base) return "?";
        switch (a_base->GetFormType()) {
        case RE::FormType::Door:      return "DOOR";
        case RE::FormType::Activator: return "ACTI";
        default:                      return "other";
        }
    }

namespace {

    // GAME THREAD. Re-read and log the open state of `a_refs` ("after" an event).
    void LogOpenStates(RE::FormID a_actor, const std::vector<WatchedRef>& a_refs, const char* a_when) {
        for (const auto& w : a_refs) {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(w.ref);
            auto* base = ref ? ref->GetBaseObject() : nullptr;
            if (!ref || !base || base->GetFormID() != w.base) {
                spdlog::info("[travel-gate] 0x{} {}: watched ref 0x{} no longer resolves to base 0x{}.", Hex(a_actor),
                             a_when, Hex(w.ref), Hex(w.base));
                continue;
            }
            const auto st = RE::BGSOpenCloseForm::GetOpenState(ref);
            spdlog::info("[travel-gate] 0x{} {}: ref 0x{} base 0x{} {} GetOpenState={}({}) disabled={}.", Hex(a_actor),
                         a_when, Hex(w.ref), Hex(w.base), GateTypeName(base), OpenStateName(st),
                         static_cast<std::uint32_t>(st), ref->IsDisabled());
        }
    }

    // GAME THREAD. Re-post until `a_dueMs`, then log (the idiom MainThread.h names for a
    // task that needs a later frame).
    void RereadLater(RE::FormID a_actor, std::vector<WatchedRef> a_refs, std::uint64_t a_dueMs) {
        apmf::mainthread::Post([a_actor, refs = std::move(a_refs), a_dueMs]() mutable {
            if (apmf::clock::MonotonicMs() < a_dueMs) {
                RereadLater(a_actor, std::move(refs), a_dueMs);
                return;
            }
            LogOpenStates(a_actor, refs, "4 s after the event");
        });
    }

}

    // ANY THREAD (the sinks). If `a_ref` is watched or near a watched stall, log one event
    // line and schedule the main-thread re-reads. Returns quietly otherwise.
    void OnGateEvent(const char* a_kind, RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_by, int a_opened) {
        if (!a_ref || g_watchCount.load(std::memory_order_relaxed) == 0) return;
        const RE::FormID   refId = a_ref->GetFormID();
        const auto*        base  = a_ref->GetBaseObject();
        const RE::FormID   space = SpaceOf(a_ref);
        const RE::NiPoint3 pos   = a_ref->GetPosition();
        const std::uint64_t now  = apmf::clock::MonotonicMs();

        RE::FormID              actor = 0;
        float                   dist  = 0.0f;
        bool                    listed = false;
        std::vector<WatchedRef> refs;
        {
            std::lock_guard lk(g_watchMx);
            for (auto& w : g_watches) {
                if (now >= w.untilMs || w.lines >= kWatchLinesMax) continue;
                bool inList = false;
                for (const auto& r : w.refs) inList = inList || r.ref == refId;
                const float d = pos.GetDistance(w.stall);
                if (!inList && (space == 0 || space != w.space || d > kWatchRadius)) continue;
                ++w.lines;
                actor  = w.actor;
                dist   = d;
                listed = inList;
                refs   = w.refs;
                break;
            }
        }
        if (actor == 0) return;

        const auto* player = RE::PlayerCharacter::GetSingleton();
        spdlog::info("[travel-gate] 0x{} {} EVENT: ref 0x{} base 0x{} {} {}by 0x{}{}, {:.0f}u from the BLOCKED stall, "
                     "{} a reference the probe listed.",
                     Hex(actor), a_kind, Hex(refId), Hex(base ? base->GetFormID() : 0), GateTypeName(base),
                     a_opened < 0 ? "" : (a_opened ? "OPENED " : "CLOSED "), Hex(a_by ? a_by->GetFormID() : 0),
                     (a_by && a_by == player) ? " (the player)" : "", dist, listed ? "IS" : "is NOT");
        // The event's own ref is read too, if the probe had not listed it.
        if (!listed && base) refs.push_back(WatchedRef{ refId, base->GetFormID() });
        apmf::mainthread::Post([actor, refs] { LogOpenStates(actor, refs, "right after the event"); });
        RereadLater(actor, refs, now + kRereadDelayMs);
    }

    // GAME THREAD (Poll, at a BLOCKED end). See the section header.
    void GateProbe(RE::FormID a_id, RE::Actor* a_actor, const RE::NiPoint3& a_stall) {
        if (!g_probeReady.load(std::memory_order_acquire)) return;   // why was logged once at Install
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (auto it = g_lastProbe.find(a_id); it != g_lastProbe.end() && now - it->second.first < kProbeRepeatMs &&
                                              it->second.second.GetDistance(a_stall) < 256.0f) {
            spdlog::info("[travel-gate] 0x{} BLOCKED again at the same place within {} s -- probe not repeated.",
                         Hex(a_id), kProbeRepeatMs / 1000);
            return;
        }
        g_lastProbe[a_id] = { now, a_stall };

        auto* cell = a_actor->GetParentCell();
        if (!cell) {
            spdlog::info("[travel-gate] 0x{} BLOCKED: the actor has no parent cell; nothing to scan.", Hex(a_id));
            return;
        }
        // The cells to scan: the actor's own, plus (outdoors) the loaded exterior cells
        // under the four corners of the probe square, resolved by coordinates with the
        // same verified TES::GetCell PositionCast uses. Only attached cells of the same
        // worldspace; each once.
        std::vector<RE::TESObjectCELL*> cells{ cell };
        if (cell->IsExteriorCell()) {
            auto*       tes = RE::TES::GetSingleton();
            const auto* ws  = cell->GetRuntimeData().worldSpace;
            for (const float dx : { -kProbeRadius, kProbeRadius }) {
                for (const float dy : { -kProbeRadius, kProbeRadius }) {
                    auto* c = tes ? tes->GetCell(RE::NiPoint3{ a_stall.x + dx, a_stall.y + dy, a_stall.z }) : nullptr;
                    if (!c || !c->IsExteriorCell() || !c->IsAttached() || c->GetRuntimeData().worldSpace != ws) continue;
                    if (std::find(cells.begin(), cells.end(), c) == cells.end()) cells.push_back(c);
                }
            }
        }

        struct Hit {
            float                            dist;
            RE::NiPointer<RE::TESObjectREFR> ref;
        };
        std::vector<Hit> hits;
        for (auto* c : cells) {
            c->ForEachReferenceInRange(a_stall, kProbeRadius, [&](RE::TESObjectREFR& r) {
                const auto* b = r.GetBaseObject();
                if (b && (b->GetFormType() == RE::FormType::Door || b->GetFormType() == RE::FormType::Activator))
                    hits.push_back(Hit{ r.GetPosition().GetDistance(a_stall), RE::NiPointer<RE::TESObjectREFR>(&r) });
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.dist < y.dist; });

        spdlog::info("[travel-gate] 0x{} BLOCKED at {:.0f},{:.0f},{:.0f} (cell 0x{}): {} DOOR/ACTI reference(s) within "
                     "{:.0f}u across {} cell(s){}.",
                     Hex(a_id), a_stall.x, a_stall.y, a_stall.z, Hex(cell->GetFormID()), hits.size(), kProbeRadius,
                     cells.size(), hits.size() > kProbeMaxRefs ? " -- the nearest are listed" : "");

        std::vector<WatchedRef> watched;
        for (std::size_t i = 0; i < hits.size() && i < kProbeMaxRefs; ++i) {
            auto*       r    = hits[i].ref.get();
            const auto* base = r->GetBaseObject();
            if (!base) continue;
            const std::uint32_t rf = base->GetFormFlags();
            const auto          st = RE::BGSOpenCloseForm::GetOpenState(r);
            const char*         ed = base->GetFormEditorID();
            const char*         nm = r->GetName();
            spdlog::info("[travel-gate] 0x{}   #{} ref 0x{} base 0x{} '{}' '{}' {} recordFlags 0x{} obstacle={} "
                         "navmeshFilter={} GetOpenState={}({}) disabled={} dist {:.0f}.",
                         Hex(a_id), i, Hex(r->GetFormID()), Hex(base->GetFormID()), ed ? ed : "", nm ? nm : "",
                         GateTypeName(base), Hex(rf, 8), (rf & (1u << 25)) != 0, (rf & (1u << 26)) != 0,
                         OpenStateName(st), static_cast<std::uint32_t>(st), r->IsDisabled(), hits[i].dist);
            watched.push_back(WatchedRef{ r->GetFormID(), base->GetFormID() });
        }

        {
            std::lock_guard lk(g_watchMx);
            std::erase_if(g_watches, [now](const GateWatch& w) { return now >= w.untilMs; });
            if (g_watches.size() >= kWatchMax) g_watches.erase(g_watches.begin());   // the oldest goes
            g_watches.push_back(GateWatch{ a_id, SpaceOf(a_actor), a_stall, now + kWatchMs, 0, std::move(watched) });
            g_watchCount.store(static_cast<std::uint32_t>(g_watches.size()), std::memory_order_relaxed);
        }
        spdlog::info("[travel-gate] 0x{} watching OPEN-CLOSE and ACTIVATE events on those references and within {:.0f}u "
                     "of the stall for {} min (at most {} event lines).",
                     Hex(a_id), kWatchRadius, kWatchMs / 60000, kWatchLinesMax);
    }
