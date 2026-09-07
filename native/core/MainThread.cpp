#include "PCH.h"
#include "core/MainThread.h"

#include <cstddef>
#include <mutex>
#include <vector>

// See MainThread.h for the design. This TU is the queue only.
namespace apmf::mainthread {

    namespace {
        std::mutex                         g_mx;
        std::vector<std::function<void()>> g_queue;
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
        for (auto& fn : local) fn();
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
