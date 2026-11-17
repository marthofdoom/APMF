#include "PCH.h"
#include "core/MainThread.h"

#include "core/Clock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <mutex>
#include <vector>

// See MainThread.h for the design. This TU is the queue only.
namespace apmf::mainthread {

    namespace {
        // One queued task. `afterDrain` (PostAfterDrain): it must not run before the NEXT ControlMap::Drain has
        // published. Pump() runs it like any other (Pump always follows a Drain); DrainForSave() stops in front of it.
        struct Task {
            std::function<void()> fn;
            bool                  afterDrain = false;
        };
        std::mutex        g_mx;
        std::vector<Task> g_queue;

        // True from the first Pump() after a Discard() (or after start-up) until the next Discard(). DrainForSave
        // refuses while it is false: the tasks the load handlers posted for "the first pump after load" keep that
        // seat (review F4 of the marker sweep: not inside a message handler, the first player Update of the loaded
        // world). Any thread reads it; main writes it.
        std::atomic<bool> g_pumpedSinceDiscard{ false };
        bool              g_running = false;   // main only: a batch is being run (Pump or DrainForSave)

        // Review F2 of 75e1f75: a throwing task (main only).
        std::uint64_t g_threwLastMs = 0, g_threwUnlogged = 0, g_threwTotal = 0;
        void TaskThrew(const char* a_what) {
            ++g_threwTotal;
            const std::uint64_t now = apmf::clock::MonotonicMs();
            if (g_threwLastMs != 0 && now - g_threwLastMs < 10000) {
                ++g_threwUnlogged;
                return;
            }
            g_threwLastMs = now;
            spdlog::error("[mainthread] a POSTED TASK THREW ({}) -- that task is lost; every other task of the batch still "
                          "runs. {} posted task(s) threw this session ({} not logged since the last line). Report this log.",
                          a_what ? a_what : "a non-standard exception", g_threwTotal, g_threwUnlogged);
            g_threwUnlogged = 0;
        }
    }

    namespace {
        // Main. Run a batch in order, each task under its own guard (review F2 of 75e1f75).
        void RunBatch(std::vector<Task>& a_batch) {
            struct Running {
                bool was;
                Running() : was(g_running) { g_running = true; }
                ~Running() { g_running = was; }
            } running;
            for (auto& t : a_batch) {
                if (!t.fn) continue;
                try {
                    t.fn();
                } catch (const std::exception& e) {
                    TaskThrew(e.what());
                } catch (...) {
                    TaskThrew(nullptr);
                }
            }
        }
    }

    void Post(std::function<void()> fn) {
        std::scoped_lock lk(g_mx);
        g_queue.push_back(Task{ std::move(fn), false });
    }

    void PostAfterDrain(std::function<void()> fn) {
        std::scoped_lock lk(g_mx);
        g_queue.push_back(Task{ std::move(fn), true });
    }

    void Pump() {
        g_pumpedSinceDiscard.store(true, std::memory_order_release);
        std::vector<Task> local;
        {
            std::scoped_lock lk(g_mx);
            if (g_queue.empty()) return;
            local.swap(g_queue);
        }
        RunBatch(local);
    }

    SaveDrain DrainForSave() {
        SaveDrain r;
        if (g_running) {
            r.refused = "it was called from inside a posted task";
            return r;
        }
        if (!g_pumpedSinceDiscard.load(std::memory_order_acquire)) {
            r.refused = "no pump has run since the load (the first-pump tasks keep their seat)";
            return r;
        }
        std::vector<Task> local;
        {
            std::scoped_lock lk(g_mx);
            if (g_queue.empty()) return r;
            local.swap(g_queue);   // ONCE: the bound. What a task posts from here on is not in this batch.
            // THE FIFO PREFIX: stop in front of the first task that waits for the next Drain, and put it and everything
            // behind it back at the FRONT of the live queue, in order, before any task of the prefix runs (so what the
            // prefix posts queues behind them). No task ever runs ahead of one posted before it.
            std::size_t n = 0;
            while (n < local.size() && !local[n].afterDrain) ++n;
            if (n < local.size()) {
                r.kept = local.size() - n;
                g_queue.insert(g_queue.begin(), std::make_move_iterator(local.begin() + static_cast<std::ptrdiff_t>(n)),
                               std::make_move_iterator(local.end()));
                local.resize(n);
            }
        }
        r.ran = local.size();
        RunBatch(local);
        return r;
    }

    std::size_t Discard() {
        // Swap out under the lock, destroy the callables OUTSIDE it (identical
        // discipline to Pump: a task's captured state must never be destroyed while
        // g_mx is held, in case a destructor Post()s).
        g_pumpedSinceDiscard.store(false, std::memory_order_release);   // DrainForSave waits for the next world's first Pump
        std::vector<Task> local;
        {
            std::scoped_lock lk(g_mx);
            local.swap(g_queue);
        }
        return local.size();
    }

}
