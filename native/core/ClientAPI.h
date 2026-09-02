#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF Layer 2 -- the CLIENT API boundary (design.md Section 2). This is the seam
// where a separate client DLL (MFO, future mods) drives NPCs WITHOUT claiming a
// package or an alias. It is an INTER-PLUGIN C-ABI: the public contract lives in
// APMF_API.h (the ONLY file a client shares), a POD struct of function pointers --
// no C++ class, no STL, no vtable crosses the DLL boundary (INVARIANTS #14).
//
// The in-process multi-NPC engine (core/ControlMap) is the IMPLEMENTATION behind
// this C interface: APMF_Request/APMF_Release forward to ControlMap's thread-safe
// enqueue path. A client obtains the interface via the exported query function
// APMF_GetInterface (see APMF_API.h for the client-side snippet). APMF holds ZERO
// client-specific code -- delete every client and APMF loses zero lines.
// ============================================================================

extern "C" __declspec(dllexport) const APMF_API::APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion);
