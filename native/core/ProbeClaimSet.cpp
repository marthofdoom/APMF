#include "PCH.h"
#include "core/ProbeClaimSet.h"

#include <array>

// ============================================================================
// See ProbeClaimSet.h for the design. Plain scan-then-CAS over a fixed array --
// no ordering guarantees beyond "each slot's own value is atomically consistent",
// which is all a throwaway probe's claim set needs.
// ============================================================================

namespace apmf::probeclaim {

    namespace {
        std::array<std::atomic<RE::FormID>, kCap> g_slots{};
    }

    bool Contains(RE::FormID id) {
        if (id == 0) return false;
        for (auto& s : g_slots) {
            if (s.load(std::memory_order_relaxed) == id) return true;
        }
        return false;
    }

    bool Add(RE::FormID id) {
        if (id == 0 || Contains(id)) return false;
        for (auto& s : g_slots) {
            RE::FormID expected = 0;
            if (s.compare_exchange_strong(expected, id, std::memory_order_relaxed)) return true;
        }
        return false;   // capacity full
    }

    bool Remove(RE::FormID id) {
        if (id == 0) return false;
        for (auto& s : g_slots) {
            if (s.load(std::memory_order_relaxed) == id) {
                s.store(0, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    bool Toggle(RE::FormID id) {
        if (Remove(id)) return false;
        return Add(id);
    }

    void Clear() {
        for (auto& s : g_slots) s.store(0, std::memory_order_relaxed);
    }

    std::size_t Count() {
        std::size_t n = 0;
        for (auto& s : g_slots) {
            if (s.load(std::memory_order_relaxed) != 0) ++n;
        }
        return n;
    }

}
