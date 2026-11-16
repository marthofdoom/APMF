#include "PCH.h"
#include "core/CombatOps.h"

#include "channels/TargetPin.h"   // Layer0Installed: the 0xE4 seat the drain rides
#include "core/Allowance.h"       // SeatVerified, RuntimeSupported
#include "core/Clock.h"
#include "core/Hook.h"            // P0 Sample
#include "core/Log.h"
#include "core/MainThread.h"      // the posted completions

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// See core/CombatOps.h for the mechanism and the disassembly it rests on.

namespace {

    using apmf::log::Hex;
    using namespace apmf::combatops;

    // Unpaused game time an op may wait with its actor still holding a controller before main concludes the
    // actor's combat update is not running (principle 9: the drain's real cadence is ONE frame -- the walker
    // pushes every member's UpdateCombat job every frame -- so 1 s of running game time is ~60 missed updates).
    constexpr float         kUndrainedSec = 1.0f;
    constexpr std::uint64_t kStatsEveryMs = 60000;

    struct Entry {
        std::uintptr_t    cc       = 0;    // the controller main decided to stop (identity only, never dereferenced)
        const char*       who      = "";   // the first requester (a literal)
        std::uint64_t     queuedMs = 0;
        float             waitSec  = 0.0f;  // unpaused game time waited (main only, under g_mx)
        std::vector<Done> dones;
    };

    std::mutex                              g_mx;   // LEAF: no engine call, no log, no other lock under it
    std::unordered_map<RE::FormID, Entry>   g_ops;
    std::atomic<std::uint32_t>              g_count{ 0 };   // g_ops.size(): the drain's one relaxed pre-gate

    std::atomic<bool>        g_available{ false };
    std::atomic<const char*> g_unavailable{ "before kDataLoaded (combatops::Install runs there)" };
    std::uintptr_t           g_vtChar = 0;   // the verified Character vtable (written once at Install)
    std::atomic<bool>        g_faulted{ false };
    int                      g_sync = 0;      // SyncScope depth (main only)

    // Counters: per 60 s window (atomics: the drain writes some) and session totals (main).
    struct Counters {
        std::atomic<std::uint64_t> queued{ 0 }, coalesced{ 0 }, drained{ 0 }, drainSkipped{ 0 }, gone{ 0 },
            undrained{ 0 }, faultedMain{ 0 }, save{ 0 }, dropped{ 0 };
    };
    Counters      g_win;
    std::uint64_t g_tot[9]{};
    std::uint64_t g_statsLastMs = 0;
    bool          g_anyEver = false;   // RULE C: the line prints once anything was ever queued

    bool IsCharacter(const RE::Actor* a) { return a && g_vtChar && *reinterpret_cast<const std::uintptr_t*>(a) == g_vtChar; }

    std::uintptr_t ControllerOf(const RE::Actor* a) {
        return a ? reinterpret_cast<std::uintptr_t>(a->GetActorRuntimeData().combatController) : 0;
    }

    // Run the op for `a_actor` HERE, on the calling thread (the actor's own update, or main in the save / degrade
    // cases). The controller is re-read and compared with the one the decision was made for.
    Result Execute(RE::Actor* a_actor, RE::FormID a_id, std::uintptr_t a_cc, How a_how) {
        Result r{};
        r.actor = a_id;
        r.how   = a_how;
        const std::uintptr_t now = ControllerOf(a_actor);
        if (!a_actor) {
            r.why = "the actor no longer resolves";
        } else if (now == 0) {
            r.why = "the controller was already gone (the engine ended the fight first)";
        } else if (now != a_cc) {
            r.why = "a DIFFERENT controller (a new fight began after the decision): not stopped";
        } else if (!IsCharacter(a_actor)) {
            r.why = "not a Character vtable: StopCombat not called";
        } else {
            a_actor->StopCombat();   // Character slot 0xE5 (AE 0x6B70A0 / SE 0x625920 / 1.7.104 0x6C9B20)
            r.stopped = true;
        }
        return r;
    }

    void Deliver(std::vector<Done>& a_dones, const Result& a_r) {
        for (auto& d : a_dones) {
            if (!d) continue;
            try {
                d(a_r);
            } catch (const std::exception& e) {
                spdlog::error("[combatops] 0x{} a completion threw ({}) -- the rest still run.", Hex(a_r.actor), e.what());
            } catch (...) {
                spdlog::error("[combatops] 0x{} a completion threw a non-standard exception -- the rest still run.",
                              Hex(a_r.actor));
            }
        }
    }

    // Main: take `a_id`'s entry out of the queue (nothing if a drain took it first).
    bool Take(RE::FormID a_id, Entry& a_out) {
        std::scoped_lock lk(g_mx);
        const auto it = g_ops.find(a_id);
        if (it == g_ops.end()) return false;
        a_out = std::move(it->second);
        g_ops.erase(it);
        g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_relaxed);
        return true;
    }

    // Main: run one entry inline and deliver its completions inline.
    void RunOnMain(RE::FormID a_id, Entry& a_e, How a_how) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
        if (ControllerOf(actor) != 0) apmf::hook::Sample(apmf::hook::Site::kStopCombatMain);   // P0
        const Result r = Execute(actor, a_id, a_e.cc, a_how);
        Deliver(a_e.dones, r);
    }

    void Stats(std::uint64_t a_now) {
        if (g_statsLastMs == 0) { g_statsLastMs = a_now; return; }
        if (a_now - g_statsLastMs < kStatsEveryMs) return;
        const std::uint64_t secs = (a_now - g_statsLastMs) / 1000;
        g_statsLastMs = a_now;
        std::uint64_t w[9] = { g_win.queued.exchange(0), g_win.coalesced.exchange(0), g_win.drained.exchange(0),
                               g_win.drainSkipped.exchange(0), g_win.gone.exchange(0), g_win.undrained.exchange(0),
                               g_win.faultedMain.exchange(0), g_win.save.exchange(0), g_win.dropped.exchange(0) };
        bool any = false;
        for (int i = 0; i < 9; ++i) { g_tot[i] += w[i]; any = any || w[i] != 0; }
        if (!g_anyEver && !any) return;
        g_anyEver = true;
        spdlog::info("[combatops] {} s: StopCombat queued {} (joined a queued one {}), run on the actor's own combat "
                     "update {} (skipped there {}), cancelled with no controller left {}, UNDRAINED -> main {}, faulted "
                     "-> main {}, run inline at a save {}, dropped at a load {}; pending now {}. Session: {}/{}/{}/{}/{}/{}/"
                     "{}/{}/{}.",
                     secs, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], g_count.load(std::memory_order_relaxed),
                     g_tot[0], g_tot[1], g_tot[2], g_tot[3], g_tot[4], g_tot[5], g_tot[6], g_tot[7], g_tot[8]);
    }

}

namespace apmf::combatops {

    void Install() {
        static bool s_tried = false;
        if (s_tried) return;
        s_tried = true;
        const char* why = nullptr;
        if (REL::Module::IsVR() || !apmf::allowance::RuntimeSupported()) {
            why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (!apmf::targetpin::Layer0Installed()) {
            why = "Character::UpdateCombat (0xE4) layer 0 is not installed ([TargetPin] bTargetPin=0, or its row refused; "
                  "see the [ch.20] lines): there is no seat on the actor's own combat update to run a StopCombat from";
        } else {
            const REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
            if (!apmf::allowance::SeatVerified(vtChar.address(), "CombatOps.Character.StopCombat(0xE5)"))
                why = "the self-check refused the Character vtable (the StopCombat identity check)";
            else
                g_vtChar = vtChar.address();
        }
        if (why) {
            g_unavailable.store(why, std::memory_order_release);
            spdlog::warn("[combatops] owner-thread StopCombat NOT available -- {}. The held engagement's StopCombat and "
                         "the combat substrate refuse by name.",
                         why);
            return;
        }
        g_unavailable.store(nullptr, std::memory_order_release);
        g_available.store(true, std::memory_order_release);
        spdlog::info("[combatops] owner-thread StopCombat ON: a StopCombat Harbinger decides runs on the actor's own "
                     "Character::UpdateCombat (0xE4, the outermost Harbinger layer, before the chain); main runs it "
                     "only inside a save, or after {:.1f} s of game time with the actor's combat update not running "
                     "(an ERROR).",
                     static_cast<double>(kUndrainedSec));
    }

    bool Available() { return g_available.load(std::memory_order_acquire); }

    const char* UnavailableWhy() {
        const char* w = g_unavailable.load(std::memory_order_acquire);
        return w ? w : "available";
    }

    const char* HowText(How a_how) {
        switch (a_how) {
        case How::kOwnerUpdate:   return "on the actor's own combat update";
        case How::kSaveSync:      return "inline on main inside the save";
        case How::kMainUndrained: return "on MAIN: the actor's combat update never ran (UNDRAINED)";
        case How::kMainFaulted:   return "on MAIN: the owner-thread drain faulted earlier this session";
        case How::kGone:          return "cancelled on main: no controller left";
        case How::kDropped:       return "dropped at a load";
        default:                  return "not run";
        }
    }

    bool StopCombat(RE::Actor* a_actor, const char* a_who, Done a_done) {
        if (!Available() || !a_actor) return false;
        const std::uintptr_t cc = ControllerOf(a_actor);
        if (cc == 0) return false;
        const RE::FormID id = a_actor->GetFormID();
        g_anyEver = true;
        // The save (and a faulted drain): inline, on main, now.
        if (g_sync > 0 || g_faulted.load(std::memory_order_acquire)) {
            const How how = g_sync > 0 ? How::kSaveSync : How::kMainFaulted;
            Entry e{};
            Take(id, e);   // a queued op for this actor joins this run (its completions too)
            e.cc  = cc;    // the decision made now names the controller the actor has now
            e.who = a_who ? a_who : "";
            if (a_done) e.dones.push_back(std::move(a_done));
            if (how == How::kSaveSync) g_win.save.fetch_add(1, std::memory_order_relaxed);
            else {
                g_win.faultedMain.fetch_add(1, std::memory_order_relaxed);
                spdlog::error("[combatops] 0x{} StopCombat ({}) made ON MAIN: the owner-thread drain faulted earlier this "
                              "session (see the ERROR above).",
                              Hex(id), a_who ? a_who : "");
            }
            RunOnMain(id, e, how);
            return true;
        }
        bool joined = false;
        {
            std::scoped_lock lk(g_mx);
            auto [it, fresh] = g_ops.try_emplace(id);
            Entry& e = it->second;
            if (fresh) {
                e.cc       = cc;
                e.who      = a_who ? a_who : "";
                e.queuedMs = apmf::clock::MonotonicMs();
            } else {
                joined = true;
                e.cc   = cc;   // the latest decision names the controller the actor has now
            }
            if (a_done) e.dones.push_back(std::move(a_done));
            g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_release);
        }
        (joined ? g_win.coalesced : g_win.queued).fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool Pending(RE::FormID a_actor) {
        if (g_count.load(std::memory_order_acquire) == 0) return false;
        std::scoped_lock lk(g_mx);
        return g_ops.find(a_actor) != g_ops.end();
    }

    void DrainFor(RE::Actor* a_this) {
        if (g_count.load(std::memory_order_acquire) == 0 || !a_this) return;
        if (g_faulted.load(std::memory_order_relaxed)) return;   // main runs them now (StopCombat / Poll)
        try {
            const RE::FormID id = a_this->GetFormID();
            Entry            e{};
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_ops.find(id);
                if (it == g_ops.end()) return;
                e = std::move(it->second);
                g_ops.erase(it);
                g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_release);
            }
            const Result r = Execute(a_this, id, e.cc, How::kOwnerUpdate);
            (r.stopped ? g_win.drained : g_win.drainSkipped).fetch_add(1, std::memory_order_relaxed);
            if (!e.dones.empty())
                apmf::mainthread::Post([dones = std::move(e.dones), r]() mutable { Deliver(dones, r); });
        } catch (const std::exception& e) {
            if (!g_faulted.exchange(true))
                spdlog::error("[combatops] EXCEPTION in the owner-thread drain ({}) -- the drain is STOPPED for the rest of "
                              "this session; every StopCombat Harbinger decides from now on is made ON MAIN with an ERROR "
                              "line (the pre-2026-11-16 exposure, APMF-B26). Report this log.",
                              e.what());
        } catch (...) {
            if (!g_faulted.exchange(true))
                spdlog::error("[combatops] EXCEPTION (non-standard) in the owner-thread drain -- the drain is STOPPED for the "
                              "rest of this session; every StopCombat Harbinger decides from now on is made ON MAIN with an "
                              "ERROR line (the pre-2026-11-16 exposure, APMF-B26). Report this log.");
        }
    }

    void Poll(float a_delta) {
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_count.load(std::memory_order_acquire) != 0) {
            const float dt = a_delta > 0.0f && a_delta < 1.0f ? a_delta : 0.0f;   // a hitch never counts as 1 s at once
            std::vector<std::pair<RE::FormID, float>> ids;
            {
                std::scoped_lock lk(g_mx);   // leaf: copy out, look up after
                ids.reserve(g_ops.size());
                for (auto& [id, e] : g_ops) {
                    e.waitSec += dt;
                    ids.emplace_back(id, e.waitSec);
                }
            }
            const bool faultedNow = g_faulted.load(std::memory_order_relaxed);
            std::vector<RE::FormID> gone, late;
            for (const auto& [id, waited] : ids) {
                // Only the controller's ADDRESS is read here (its presence), never through it.
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (ControllerOf(a) == 0) gone.push_back(id);
                else if (waited >= kUndrainedSec || faultedNow) late.push_back(id);
            }
            for (const auto id : gone) {
                Entry e{};
                if (!Take(id, e)) continue;   // a drain took it in between: its result is posted
                g_win.gone.fetch_add(1, std::memory_order_relaxed);
                Result r{};
                r.actor = id;
                r.how   = How::kGone;
                r.why   = "the actor no longer has a combat controller (the engine ended the fight, a kill or an unload): "
                          "nothing left to stop";
                Deliver(e.dones, r);
            }
            for (const auto id : late) {
                Entry e{};
                if (!Take(id, e)) continue;
                const bool faulted = g_faulted.load(std::memory_order_relaxed);
                (faulted ? g_win.faultedMain : g_win.undrained).fetch_add(1, std::memory_order_relaxed);
                if (!faulted)
                    spdlog::error("[combatops] 0x{} StopCombat ({}) UNDRAINED: queued {} ms ago, {:.1f} s of game time ran "
                                  "and the actor still holds a controller, but its Character::UpdateCombat never ran (no "
                                  "UpdateCombat job, or a hook above Harbinger's layers that does not call down). Made ON "
                                  "MAIN now: its own combat update is not running, so it cannot meet this free.",
                                  Hex(id), e.who, now - e.queuedMs, static_cast<double>(e.waitSec));
                RunOnMain(id, e, faulted ? How::kMainFaulted : How::kMainUndrained);
            }
        }
        Stats(now);
    }

    void FlushForSave() {
        apmf::hook::Sample(apmf::hook::Site::kSaveFlush);   // P0: is anything combat-shaped running inside the save call
        if (g_count.load(std::memory_order_acquire) == 0) return;
        std::unordered_map<RE::FormID, Entry> all;
        {
            std::scoped_lock lk(g_mx);
            all.swap(g_ops);
            g_count.store(0, std::memory_order_release);
        }
        for (auto& [id, e] : all) {
            g_win.save.fetch_add(1, std::memory_order_relaxed);
            RunOnMain(id, e, How::kSaveSync);
        }
        spdlog::info("[combatops] save: {} queued StopCombat(s) run inline before the engine writes (kSaveGame).", all.size());
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        {
            std::scoped_lock lk(g_mx);
            n = g_ops.size();
            g_ops.clear();
            g_count.store(0, std::memory_order_release);
        }
        if (n) {
            g_win.dropped.fetch_add(n, std::memory_order_relaxed);
            spdlog::info("[combatops] {} -- dropped {} queued StopCombat(s) unrun (the outgoing world's actors; no engine "
                         "call).",
                         a_why, n);
        }
    }

    SyncScope::SyncScope() { ++g_sync; }
    SyncScope::~SyncScope() { --g_sync; }

}
