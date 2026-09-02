#include "PCH.h"
#include "core/ClientAPI.h"
#include "core/ControlMap.h"

// The C-ABI implementation behind APMF_API.h. These free functions forward to the
// in-process multi-NPC engine (core/ControlMap) over its thread-safe enqueue path,
// so a client's worker thread may call them directly. No client-specific code here.
namespace {

    // NO exception may cross the DLL boundary (APMF_API.h). A throw unwinding across
    // MFO's separately-compiled DLL is UB, so every exported body is a hard catch-all
    // (bad_alloc from m_queue.push_back, an spdlog throw, ...). A swallowed throw
    // degrades to "no control taken", never a crash in the client.
    APMF_API::Handle APMF_Request(RE::FormID actor, APMF_API::Intent intent, float basis) {
        try {
            return apmf::ControlMap::Get().EnqueueRequest(actor, intent, basis, nullptr);
        } catch (...) {
            return APMF_API::kInvalidHandle;
        }
    }

    // ABI v2: carries the POD param down to the channel (e.g. the cast-select spell).
    // The pointer is read+copied synchronously here; APMF never retains it.
    APMF_API::Handle APMF_RequestEx(RE::FormID actor, APMF_API::Intent intent, float basis,
                                    const APMF_API::APMF_Param* param) {
        try {
            return apmf::ControlMap::Get().EnqueueRequest(actor, intent, basis, param);
        } catch (...) {
            return APMF_API::kInvalidHandle;
        }
    }

    void APMF_Release(APMF_API::Handle handle) {
        try {
            apmf::ControlMap::Get().EnqueueRelease(handle);
        } catch (...) {
        }
    }

    // ABI v3: re-point an existing claim's param in place (same handle) -- e.g.
    // combat-target switches the held foe without a release/re-request. Copied
    // synchronously; APMF never retains the pointer.
    void APMF_Repoint(APMF_API::Handle handle, const APMF_API::APMF_Param* param) {
        try {
            apmf::ControlMap::Get().EnqueueRepoint(handle, param);
        } catch (...) {
        }
    }

    // The single static POD interface handed to clients. It is the NEWEST revision
    // (APMF_API_v3), constant-initialized (the pointers are to static functions), so
    // it is valid the instant the DLL loads. Because each revision's leading members
    // are exactly the previous revision's, a v1/v2 client reading it through its own
    // struct pointer sees only its prefix.
    constexpr APMF_API::APMF_API_v3 g_api{
        APMF_API::kABIVersion,
        &APMF_Request,
        &APMF_Release,
        &APMF_RequestEx,
        &APMF_Repoint,
    };

}

// Exported, undecorated (extern "C"). A client does
// GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface"). Returns the
// base type; a v2 client checks p->abiVersion and casts up to APMF_API_v2*.
extern "C" __declspec(dllexport) const APMF_API::APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion) {
    try {
        if (abiVersion > APMF_API::kABIVersion) {
            spdlog::warn("[api] APMF_GetInterface: client wants ABI v{} but APMF is v{} -- returning null.",
                         abiVersion, APMF_API::kABIVersion);
            return nullptr;   // we cannot satisfy a newer contract than we implement
        }
        spdlog::info("[api] APMF_GetInterface: handed v{} interface to a client (requested v{}).",
                     APMF_API::kABIVersion, abiVersion);
        // Hand back the base-type view of the newest struct; every shipped revision
        // shares v1's identical initial sequence, so this is layout-safe.
        return reinterpret_cast<const APMF_API::APMF_API_v1*>(&g_api);
    } catch (...) {
        return nullptr;   // an spdlog throw must not unwind into the client
    }
}
