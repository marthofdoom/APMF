#include "PCH.h"
#include "core/ClientAPI.h"

// STUB seam. See core/ClientAPI.h for why the bodies are intentionally empty.
namespace apmf::client {

    Handle Request(RE::Actor* actor, Intent intent, Basis basis) {
        spdlog::warn("[client] Request(actor=0x{:08X}, intent={}, basis={:.1f}) -- STUB, Layer-2 API not "
                     "built yet (design.md Section 7). No control taken.",
                     actor ? actor->GetFormID() : 0,
                     static_cast<std::uint32_t>(intent), basis.priority);
        return kInvalid;
    }

    void Complete(Handle handle) {
        spdlog::warn("[client] Complete(handle={}) -- STUB, Layer-2 API not built yet.", handle);
    }

}
