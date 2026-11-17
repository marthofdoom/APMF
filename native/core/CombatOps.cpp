#include "PCH.h"
#include "core/CombatOps.h"

#include "core/Allowance.h"       // SeatVerified, RuntimeSupported, IsRuntime1_7_104
#include "core/Clock.h"
#include "core/Hook.h"            // P0 Sample
#include "core/Log.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// See core/CombatOps.h for the mechanism and the disassembly it rests on (Docs/research/stopcombat-engine-road-re.md).

namespace {

    using apmf::log::Hex;
    using namespace apmf::combatops;

    // Unpaused game time a posted op may keep the SAME controller address before main answers "not run". Principle 9:
    // the real cadence is ONE frame -- the engine drains its task queue every frame, in the "Post process" job while
    // the game runs and in Main::Update while it is paused -- so 1 s of running game time is ~60 missed drains.
    constexpr float         kNotRunSec    = 1.0f;
    constexpr std::uint64_t kStatsEveryMs = 60000;

    // The engine's sender of task-queue message 0x2B: void(TaskQueueInterface*, Actor*). AE id 36960 (0x656530),
    // SE id 35985 (0x5C57A0); 1.7.104 has no table id for it, the raw RVA is verified instead (VerifiedAddresses
    // row "CombatOps.TaskQueue.QueueStopCombat" on all three).
    constexpr std::uintptr_t k17QueueStopCombat = 0x668F20;
    using QueueStopCombat_t = void (*)(RE::TaskQueueInterface*, RE::Actor*);
    REL::Relocation<QueueStopCombat_t> g_queueStopCombat;   // written once at Install (engine road only)

    enum class Road : std::uint8_t {
        kNone,          // not available: StopCombat() refuses
        kEngineQueue,   // the engine's own message, through the engine's own sender
        kMainFallback,  // the sender's row was refused: main pump, FLAGGED (re-opens APMF-B26)
    };
    std::atomic<Road> g_road{ Road::kNone };

    struct Entry {
        std::uintptr_t    cc       = 0;     // the controller main decided to stop (identity only, never dereferenced)
        const char*       who      = "";    // the first requester (a literal)
        std::uint64_t     queuedMs = 0;
        float             waitSec  = 0.0f;  // unpaused game time waited since it was posted (main only, under g_mx)
        std::uint64_t     seq      = 0;     // this entry's identity (a ResetAll / a new entry for the actor never matches)
        bool              posted   = false; // the engine has the message. False: its task queue did not exist yet
        std::vector<Done> dones;
    };

    std::mutex                              g_mx;   // LEAF: no engine call, no log, no other lock under it
    std::unordered_map<RE::FormID, Entry>   g_ops;
    std::atomic<std::uint32_t>              g_count{ 0 };   // g_ops.size(): Pending's one relaxed pre-gate
    std::uint64_t                           g_seq = 0;      // under g_mx: the next entry's seq

    std::atomic<const char*> g_unavailable{ "before kDataLoaded (combatops::Install runs there)" };
    std::uintptr_t           g_vtChar = 0;   // the verified Character vtable (written once at Install)
    int                      g_sync = 0;      // SyncScope depth (main only)
    bool                     g_noQueueLogged = false;   // main only

    // Counters: per 60 s window and session totals (main only; atomics kept for the one line's exchange).
    struct Counters {
        std::atomic<std::uint64_t> asked{ 0 }, coalesced{ 0 }, engineRan{ 0 }, atPost{ 0 }, notRun{ 0 }, gone{ 0 },
            save{ 0 }, fallbackMain{ 0 }, dropped{ 0 };
    };
    Counters      g_win;
    std::uint64_t g_tot[9]{};
    std::uint64_t g_statsLastMs = 0;
    bool          g_anyEver = false;   // RULE C: the line prints once anything was ever asked

    bool IsCharacter(const RE::Actor* a) { return a && g_vtChar && *reinterpret_cast<const std::uintptr_t*>(a) == g_vtChar; }

    std::uintptr_t ControllerOf(const RE::Actor* a) {
        return a ? reinterpret_cast<std::uintptr_t>(a->GetActorRuntimeData().combatController) : 0;
    }

    // MAIN ONLY, and only inside the save (FlushForSave / SyncScope) or on the flagged fallback road: Actor::StopCombat
    // called by Harbinger itself. The controller is re-read and compared with the one the decision was made for.
    Result ExecuteOnMain(RE::Actor* a_actor, RE::FormID a_id, std::uintptr_t a_cc, How a_how) {
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
            apmf::hook::Sample(apmf::hook::Site::kStopCombatMain);   // P0
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

    // Main: take `a_id`'s entry out of the table. `a_seq` != 0: only that entry (a newer one for the actor stays).
    bool Take(RE::FormID a_id, Entry& a_out, std::uint64_t a_seq = 0) {
        std::scoped_lock lk(g_mx);
        const auto it = g_ops.find(a_id);
        if (it == g_ops.end() || (a_seq != 0 && it->second.seq != a_seq)) return false;
        a_out = std::move(it->second);
        g_ops.erase(it);
        g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_release);
        return true;
    }

    // THE POST (main). The engine's own sender builds message 0x2B for the actor's handle and hands it to the engine's
    // task queue; the engine runs Actor::StopCombat at its own drain (core/CombatOps.h). Nothing of the controller is
    // read here but its address, after the call.
    enum class Posted : std::uint8_t {
        kQueued,    // the engine has the message
        kRanNow,    // the controller was gone as the sender returned: the engine ran the message on this thread
        kNoQueue,   // TaskQueueInterface does not exist (yet): nothing sent
    };
    Posted PostToEngine(RE::Actor* a_actor) {
        auto* tq = RE::TaskQueueInterface::GetSingleton();
        if (!tq) return Posted::kNoQueue;
        g_queueStopCombat(tq, a_actor);
        return ControllerOf(a_actor) == 0 ? Posted::kRanNow : Posted::kQueued;
    }

    Result EngineResult(RE::FormID a_id, How a_how) {
        Result r{};
        r.actor   = a_id;
        r.stopped = true;
        r.how     = a_how;
        return r;
    }

    void WarnRanNow(RE::FormID a_id, const char* a_who) {
        g_win.atPost.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[combatops] 0x{} StopCombat ({}): the controller was GONE as the engine's sender returned -- the "
                     "engine ran its message on this thread at once (its task queue was off or refused the push), or the "
                     "fight ended that instant. If it ran here, this one StopCombat was made on MAIN beside the NPC updates "
                     "(the APMF-B26 exposure), by the engine's own rule.",
                     Hex(a_id), a_who ? a_who : "");
    }

    void ErrorNoQueue() {
        if (g_noQueueLogged) return;
        g_noQueueLogged = true;
        spdlog::error("[combatops] the engine's TaskQueueInterface does not exist yet: a StopCombat cannot be posted. It "
                      "is retried every frame; one still unposted after {:.1f} s of game time is answered \"not stopped\".",
                      static_cast<double>(kNotRunSec));
    }

    void Stats(std::uint64_t a_now) {
        if (g_statsLastMs == 0) { g_statsLastMs = a_now; return; }
        if (a_now - g_statsLastMs < kStatsEveryMs) return;
        const std::uint64_t secs = (a_now - g_statsLastMs) / 1000;
        g_statsLastMs = a_now;
        std::uint64_t w[9] = { g_win.asked.exchange(0), g_win.coalesced.exchange(0), g_win.engineRan.exchange(0),
                               g_win.atPost.exchange(0), g_win.notRun.exchange(0), g_win.gone.exchange(0),
                               g_win.save.exchange(0), g_win.fallbackMain.exchange(0), g_win.dropped.exchange(0) };
        bool any = false;
        for (int i = 0; i < 9; ++i) { g_tot[i] += w[i]; any = any || w[i] != 0; }
        if (!g_anyEver && !any) return;
        g_anyEver = true;
        spdlog::info("[combatops] {} s: StopCombat asked {} (joined a pending one {}), run by the engine from its task "
                     "queue {}, gone at the post (WARNING) {}, NOT RUN -> not stopped (ERROR) {}, cancelled unposted {}, "
                     "inline at a save {}, on MAIN by the fallback road (ERROR) {}, dropped at a load {}; pending now {}. "
                     "Session: {}/{}/{}/{}/{}/{}/{}/{}/{}.",
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
        } else {
            const REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
            if (!apmf::allowance::SeatVerified(vtChar.address(), "CombatOps.Character.StopCombat(0xE5)"))
                why = "the self-check refused the Character vtable (the StopCombat identity check)";
            else
                g_vtChar = vtChar.address();
        }
        if (why) {
            g_unavailable.store(why, std::memory_order_release);
            spdlog::warn("[combatops] StopCombat NOT available -- {}. The held engagement's StopCombat and the combat "
                         "substrate refuse by name.",
                         why);
            return;
        }
        // The engine's sender of message 0x2B. 1.7.104 has no table id for it: the raw RVA, verified by its own row.
        const bool on17 = apmf::allowance::IsRuntime1_7_104();
        const REL::Relocation<QueueStopCombat_t> sender =
            on17 ? REL::Relocation<QueueStopCombat_t>{ REL::Offset(k17QueueStopCombat) }
                 : REL::Relocation<QueueStopCombat_t>{ RELOCATION_ID(35985, 36960) };
        g_unavailable.store(nullptr, std::memory_order_release);
        if (apmf::allowance::SeatVerified(sender.address(), "CombatOps.TaskQueue.QueueStopCombat")) {
            g_queueStopCombat = sender;
            g_road.store(Road::kEngineQueue, std::memory_order_release);
            spdlog::info("[combatops] StopCombat ON, the engine's road: a StopCombat Harbinger decides is posted as the "
                         "engine's own task-queue message 0x2B through the engine's own sender (0x{:X}) and the ENGINE "
                         "runs Actor::StopCombat where it drains that queue (the Post process job; Main::Update while "
                         "paused). Harbinger calls StopCombat itself only inside a save. Nothing runs on a combat job. A "
                         "posted one whose controller is unchanged after {:.1f} s of game time is answered \"not stopped\" "
                         "with an ERROR.",
                         sender.address() - REL::Module::get().base(), static_cast<double>(kNotRunSec));
        } else {
            g_road.store(Road::kMainFallback, std::memory_order_release);
            spdlog::error("[combatops] the self-check REFUSED the engine's StopCombat sender (row "
                          "CombatOps.TaskQueue.QueueStopCombat) -- FLAGGED WORKAROUND for this session: every StopCombat "
                          "Harbinger decides is made ON MAIN from its pump (the 0.20.3 road), which runs beside the NPC "
                          "updates and RE-OPENS APMF-B26 (a free under that actor's own update). Each one is an ERROR "
                          "line. Report this log.");
        }
    }

    bool Available() { return g_road.load(std::memory_order_acquire) != Road::kNone; }

    const char* UnavailableWhy() {
        const char* w = g_unavailable.load(std::memory_order_acquire);
        return w ? w : "available";
    }

    const char* HowText(How a_how) {
        switch (a_how) {
        case How::kEngineQueue:  return "by the engine, from its own task queue (message 0x2B)";
        case How::kEngineAtPost: return "by the engine at the request (its task queue ran the message at once)";
        case How::kSaveSync:     return "inline on main inside the save";
        case How::kMainFallback: return "on MAIN from the pump: the engine sender's row was refused (the flagged workaround)";
        case How::kGone:         return "cancelled on main: no controller left";
        case How::kDropped:      return "dropped at a load";
        default:                 return "not run";
        }
    }

    bool StopCombat(RE::Actor* a_actor, const char* a_who, Done a_done, bool* a_inline) {
        if (a_inline) *a_inline = false;
        const Road road = g_road.load(std::memory_order_acquire);
        if (road == Road::kNone || !a_actor) return false;
        const RE::FormID     id  = a_actor->GetFormID();
        const std::uintptr_t cc  = ControllerOf(a_actor);   // the POINTER only (the engine may be freeing it)
        const bool           inl = g_sync > 0 || road == Road::kMainFallback;
        const char*          who = a_who ? a_who : "";
        g_anyEver = true;
        // An op already pending for this actor (and not about to be answered inline here): this request JOINS it, its
        // completion with it; no caller's completion is dropped (review F5 of 75e1f75). The engine has ONE message.
        // Not joined: a posted op whose controller is no longer the one the actor holds now. The engine ran that one
        // and a NEW fight began before Poll looked; it is answered here and this decision, about the new fight, is posted.
        if (!inl) {
            bool  joined = false;
            Entry ran{};
            bool  stale = false;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_ops.find(id);
                if (it != g_ops.end()) {
                    if (it->second.posted && cc != 0 && cc != it->second.cc) {
                        stale = true;
                        ran   = std::move(it->second);
                        g_ops.erase(it);
                        g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_release);
                    } else {
                        joined = true;
                        if (a_done) it->second.dones.push_back(std::move(a_done));
                    }
                }
            }
            if (joined) {
                g_win.coalesced.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (stale) {
                g_win.engineRan.fetch_add(1, std::memory_order_relaxed);
                Deliver(ran.dones, EngineResult(id, How::kEngineQueue));
            }
        }
        // The save, and the flagged fallback road: inline, on main, now.
        if (inl) {
            Entry      e{};
            const bool had = Take(id, e);   // a pending op for this actor is answered here (its completions too)
            if (had && (cc == 0 || (e.posted && cc != e.cc))) {
                // The engine already ran the pending op's message (Poll had not looked yet).
                g_win.engineRan.fetch_add(1, std::memory_order_relaxed);
                Deliver(e.dones, EngineResult(id, How::kEngineQueue));
                e.dones.clear();
                if (cc == 0) {   // and nothing is left for this request either
                    if (a_inline) *a_inline = true;
                    if (a_done) e.dones.push_back(std::move(a_done));
                    Deliver(e.dones, EngineResult(id, How::kEngineQueue));
                    return true;
                }
            } else if (cc == 0) {
                return false;
            }
            if (a_done) e.dones.push_back(std::move(a_done));
            if (a_inline) *a_inline = true;
            const How how = g_sync > 0 ? How::kSaveSync : How::kMainFallback;
            if (how == How::kSaveSync) {
                g_win.save.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_win.fallbackMain.fetch_add(1, std::memory_order_relaxed);
                spdlog::error("[combatops] 0x{} StopCombat ({}) made ON MAIN from the pump: the engine sender's row was "
                              "refused at Install (the flagged workaround, APMF-B26 re-opened).",
                              Hex(id), who);
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            Deliver(e.dones, ExecuteOnMain(actor, id, cc, how));   // the decision made now names the controller he has now
            return true;
        }
        if (cc == 0 || !IsCharacter(a_actor)) return false;
        // The engine's road. The entry is in the table BEFORE the post, so Pending() already covers the free.
        std::uint64_t seq = 0;
        {
            std::scoped_lock lk(g_mx);
            Entry& e   = g_ops[id];   // fresh: only main inserts, and the join above found none
            e.cc       = cc;
            e.who      = who;
            e.queuedMs = apmf::clock::MonotonicMs();
            e.seq      = seq = ++g_seq;
            e.posted   = true;
            if (a_done) e.dones.push_back(std::move(a_done));
            g_count.store(static_cast<std::uint32_t>(g_ops.size()), std::memory_order_release);
        }
        g_win.asked.fetch_add(1, std::memory_order_relaxed);
        switch (PostToEngine(a_actor)) {
        case Posted::kQueued:
            break;
        case Posted::kRanNow: {
            Entry e{};
            if (Take(id, e, seq)) {
                WarnRanNow(id, who);
                if (a_inline) *a_inline = true;
                Deliver(e.dones, EngineResult(id, How::kEngineAtPost));
            }
            break;
        }
        case Posted::kNoQueue: {
            ErrorNoQueue();
            std::scoped_lock lk(g_mx);
            const auto it = g_ops.find(id);
            if (it != g_ops.end() && it->second.seq == seq) it->second.posted = false;   // Poll posts it
            break;
        }
        }
        return true;
    }

    bool Attach(RE::FormID a_actor, Done a_done) {
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_ops.find(a_actor);
            if (it == g_ops.end()) return false;
            if (a_done) it->second.dones.push_back(std::move(a_done));
        }
        g_win.coalesced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool InCombat(RE::Actor* a_actor) {
        if (!a_actor) return false;
        if (Pending(a_actor->GetFormID())) return ControllerOf(a_actor) != 0;   // the POINTER only (#4a)
        return a_actor->IsInCombat();
    }

    bool Pending(RE::FormID a_actor) {
        if (g_count.load(std::memory_order_acquire) == 0) return false;
        std::scoped_lock lk(g_mx);
        return g_ops.find(a_actor) != g_ops.end();
    }

    void Poll(float a_delta) {
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_count.load(std::memory_order_acquire) != 0) {
            const float dt = a_delta > 0.0f && a_delta < 1.0f ? a_delta : 0.0f;   // a hitch never counts as 1 s at once
            struct Seen {
                RE::FormID     id;
                std::uint64_t  seq;
                std::uintptr_t cc;
                float          waited;
                bool           posted;
            };
            std::vector<Seen> seen;
            {
                std::scoped_lock lk(g_mx);   // leaf: copy out, look up after
                seen.reserve(g_ops.size());
                for (auto& [id, e] : g_ops) {
                    e.waitSec += dt;
                    seen.push_back({ id, e.seq, e.cc, e.waitSec, e.posted });
                }
            }
            for (const auto& sn : seen) {
                // Only the controller's ADDRESS is read here (its presence, its identity), never through it.
                auto*                a     = RE::TESForm::LookupByID<RE::Actor>(sn.id);
                const std::uintptr_t ccNow = ControllerOf(a);
                if (sn.posted) {
                    if (ccNow == 0 || ccNow != sn.cc) {
                        // The engine ran the message (or ended that fight itself first). A different controller is a
                        // new fight begun since: the one the decision named is gone all the same.
                        Entry e{};
                        if (!Take(sn.id, e, sn.seq)) continue;
                        g_win.engineRan.fetch_add(1, std::memory_order_relaxed);
                        Deliver(e.dones, EngineResult(sn.id, How::kEngineQueue));
                    } else if (sn.waited >= kNotRunSec) {
                        Entry e{};
                        if (!Take(sn.id, e, sn.seq)) continue;
                        g_win.notRun.fetch_add(1, std::memory_order_relaxed);
                        spdlog::error("[combatops] 0x{} StopCombat ({}) NOT RUN: the engine was sent its message {} ms ago, "
                                      "{:.1f} s of game time ran, and the actor still holds a controller at the same "
                                      "address (the engine did not run the message, or it did and a new controller took "
                                      "the old one's address). Nothing is made on main: its callers get \"not stopped\" "
                                      "and decide again.",
                                      Hex(sn.id), e.who, now - e.queuedMs, static_cast<double>(e.waitSec));
                        Result r{};
                        r.actor = sn.id;
                        r.how   = How::kEngineQueue;
                        r.why   = "the engine was sent the message and the same controller was still there 1 s of game "
                                  "time later (NOT RUN): not stopped";
                        Deliver(e.dones, r);
                    }
                    continue;
                }
                // Not posted yet (the engine's task queue did not exist at the request).
                if (ccNow == 0 || ccNow != sn.cc || !IsCharacter(a)) {
                    Entry e{};
                    if (!Take(sn.id, e, sn.seq)) continue;
                    g_win.gone.fetch_add(1, std::memory_order_relaxed);
                    Result r{};
                    r.actor = sn.id;
                    r.how   = How::kGone;
                    r.why   = "the controller the decision named was gone before the engine could be asked: nothing left "
                              "to stop";
                    Deliver(e.dones, r);
                    continue;
                }
                const Posted posted = PostToEngine(a);
                if (posted == Posted::kQueued) {
                    std::scoped_lock lk(g_mx);
                    const auto it = g_ops.find(sn.id);
                    if (it != g_ops.end() && it->second.seq == sn.seq) {
                        it->second.posted  = true;
                        it->second.waitSec = 0.0f;   // kNotRunSec counts from the post
                    }
                } else if (posted == Posted::kRanNow) {
                    Entry e{};
                    if (!Take(sn.id, e, sn.seq)) continue;
                    WarnRanNow(sn.id, e.who);
                    Deliver(e.dones, EngineResult(sn.id, How::kEngineAtPost));
                } else if (sn.waited >= kNotRunSec) {
                    Entry e{};
                    if (!Take(sn.id, e, sn.seq)) continue;
                    g_win.notRun.fetch_add(1, std::memory_order_relaxed);
                    spdlog::error("[combatops] 0x{} StopCombat ({}) NOT POSTED: the engine's TaskQueueInterface still does "
                                  "not exist after {:.1f} s of game time. Its callers get \"not stopped\" and decide again.",
                                  Hex(sn.id), e.who, static_cast<double>(e.waitSec));
                    Result r{};
                    r.actor = sn.id;
                    r.how   = How::kNone;
                    r.why   = "the engine's task queue did not exist (NOT POSTED): not stopped";
                    Deliver(e.dones, r);
                }
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
        std::size_t inlineRun = 0, engineRan = 0;
        for (auto& [id, e] : all) {
            auto*                actor = RE::TESForm::LookupByID<RE::Actor>(id);
            const std::uintptr_t ccNow = ControllerOf(actor);
            if (e.posted && (ccNow == 0 || ccNow != e.cc)) {
                // The engine ran its message already (Poll had not looked yet).
                ++engineRan;
                g_win.engineRan.fetch_add(1, std::memory_order_relaxed);
                Deliver(e.dones, EngineResult(id, How::kEngineQueue));
                continue;
            }
            // Still the decided controller (the engine has not drained since the post, or was never asked): made here,
            // inline, before the engine writes. The engine's message, if one is queued, then finds no controller.
            ++inlineRun;
            g_win.save.fetch_add(1, std::memory_order_relaxed);
            Deliver(e.dones, ExecuteOnMain(actor, id, e.cc, How::kSaveSync));
        }
        spdlog::info("[combatops] save: {} pending StopCombat(s) answered before the engine writes (kSaveGame): {} made "
                     "inline on main, {} the engine had already run.",
                     all.size(), inlineRun, engineRan);
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
            spdlog::info("[combatops] {} -- dropped {} pending StopCombat(s) (the outgoing world's actors; no engine call, "
                         "no completion).",
                         a_why, n);
        }
    }

    SyncScope::SyncScope() { ++g_sync; }
    SyncScope::~SyncScope() { --g_sync; }

}
