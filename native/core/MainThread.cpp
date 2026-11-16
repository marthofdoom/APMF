#include "PCH.h"
#include "core/MainThread.h"

#include "core/Clock.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <vector>

// See MainThread.h for the design. This TU is the queue only.
namespace apmf::mainthread {

    namespace {
        std::mutex                         g_mx;
        std::vector<std::function<void()>> g_queue;

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

    void Post(std::function<void()> fn) {
        std::scoped_lock lk(g_mx);
        g_queue.push_back(std::move(fn));
    }

    void Pump() {
        std::vector<std::function<void()>> local;
        {
            std::scoped_lock lk(g_mx);
            if (g_queue.empty()) return;
            local.swap(g_queue);
        }
        for (auto& fn : local) {
            try {
                fn();
            } catch (const std::exception& e) {
                TaskThrew(e.what());
            } catch (...) {
                TaskThrew(nullptr);
            }
        }
    }

    std::size_t Discard() {
        // Swap out under the lock, destroy the callables OUTSIDE it (identical
        // discipline to Pump: a task's captured state must never be destroyed while
        // g_mx is held, in case a destructor Post()s).
        std::vector<std::function<void()>> local;
        {
            std::scoped_lock lk(g_mx);
            local.swap(g_queue);
        }
        return local.size();
    }

}
