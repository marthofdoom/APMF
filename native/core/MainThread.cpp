#include "PCH.h"
#include "core/MainThread.h"

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

}
