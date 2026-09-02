#include "PCH.h"
#include "core/ClientAPI.h"
#include "core/ControlMap.h"

// The C-ABI implementation behind APMF_API.h. These free functions forward to the
// in-process multi-NPC engine (core/ControlMap) over its thread-safe enqueue path,
// so a client's worker thread may call them directly. No client-specific code here.
namespace {

    APMF_API::Handle APMF_Request(RE::FormID actor, APMF_API::Intent intent, float basis) {
        return apmf::ControlMap::Get().EnqueueRequest(actor, intent, basis);
    }

    void APMF_Release(APMF_API::Handle handle) {
        apmf::ControlMap::Get().EnqueueRelease(handle);
    }

    // The single static POD interface handed to clients. Constant-initialized (the
    // pointers are to static functions), so it is valid the instant the DLL loads.
    constexpr APMF_API::APMF_API_v1 g_api{
        APMF_API::kABIVersion,
        &APMF_Request,
        &APMF_Release,
    };

}

// Exported, undecorated (extern "C"). A client does
// GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface").
extern "C" __declspec(dllexport) const APMF_API::APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion) {
    if (abiVersion > APMF_API::kABIVersion) {
        spdlog::warn("[api] APMF_GetInterface: client wants ABI v{} but APMF is v{} -- returning null.",
                     abiVersion, APMF_API::kABIVersion);
        return nullptr;   // we cannot satisfy a newer contract than we implement
    }
    spdlog::info("[api] APMF_GetInterface: handed v{} interface to a client (requested v{}).",
                 APMF_API::kABIVersion, abiVersion);
    return &g_api;
}
