#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// APMF (AI Package Management Framework) — SKSE inter-plugin C-ABI, v1.
//
// This is the ONLY file a client shares with APMF. APMF and a client (e.g. MFO)
// compile SEPARATELY and are STRICTLY separate DLLs; they interact ONLY through
// this header at runtime. Because the C++ ABI is NOT stable across two separately
// built DLLs, this surface is deliberately C-ABI: a POD struct of function
// pointers with POD argument types only (RE::FormID, a plain enum, floats). NO
// C++ classes, NO STL, NO vtable ever crosses the boundary.
//
// APPEND-ONLY CONTRACT. Once shipped, never change or reorder an existing field,
// enum value, or function-pointer slot in APMF_API_v1 — only APPEND (bump
// kABIVersion, add fields at the END). A client built against v1 must keep working
// against every later APMF. (Same discipline MFO applies to MEO_API.h.)
//
// ── How a client obtains the interface (chosen mechanism: exported query fn) ──
// APMF exports one undecorated C function, "APMF_GetInterface", returning a
// pointer to a static POD interface (no ownership, never freed). Fetch it once
// after SKSE load (e.g. your kPostLoad/kDataLoaded), then keep the pointer:
//
//     #include "APMF_API.h"
//     const APMF_API::APMF_API_v1* g_apmf = nullptr;
//     if (HMODULE h = GetModuleHandleA("APMF.dll")) {
//         auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
//             GetProcAddress(h, APMF_API::kGetInterfaceExport));
//         if (fn) g_apmf = fn(APMF_API::kABIVersion);   // nullptr on ABI mismatch
//     }
//     // If g_apmf is null, APMF is absent or too old — guard every call.
//
// (An exported query fn was chosen over the SKSE-messaging handshake MEO uses: it
// is synchronous, has no message-ordering or sender/receiver routing subtlety, and
// hands over a POD struct with no vtable. The struct-of-fn-pointers shape is the
// ABI contract; the transport is just how you get the pointer.)
//
// ── Threading ──
// Request/Release are SAFE FROM ANY THREAD. They capture POD (a FormID) and
// enqueue the work; APMF applies it on the game thread. A client's BSJobs worker
// may call them directly.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace RE {
    // Identical alias to CommonLib's RE::FormID; a duplicate identical using-alias
    // is legal, so this header composes with a full CommonLib include.
    using FormID = std::uint32_t;
}

namespace APMF_API {

    inline constexpr std::uint32_t kABIVersion = 1;

    // The exported query function's undecorated name and pointer type.
    // const APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion);
    inline constexpr const char* kGetInterfaceExport = "APMF_GetInterface";

    // A control claim handle. 0 is never a valid handle (returned on refusal).
    using Handle = std::uint32_t;
    inline constexpr Handle kInvalidHandle = 0;

    // What a client wants APMF to do to an NPC. Each maps to exactly one channel
    // family. APPEND-ONLY: never renumber; add new intents at the end.
    enum Intent : std::uint32_t {
        kIntent_None          = 0,
        kIntent_MovementBlock = 1,   // ch.1  full stand-still (block the move intent)
        kIntent_Disposition   = 2,   // ch.11 aggression/confidence/assistance/morality bias
        kIntent_Headtrack     = 3,   // ch.5  look-at (known-incomplete block)
        kIntent_SelectSpell   = 4,   // ch.8  own the cast selection (right hand)
        kIntent_WeaponDrawn   = 5,   // ch.4  draw/sheathe
        kIntent_Dialogue      = 6,   // ch.10 pause current dialogue (one-shot)
        kIntent_Gait          = 7,   // ch.1a gait scale (kSpeedMult)
        kIntent_Detection     = 8,   // ch.16 silent movement + reduced detect range (AVs)
        kIntent_Stance        = 9,   // ch.3  sneak/crouch
        kIntent_CombatTarget  = 10,  // ch.6  combat-target STEER (StartCombat)
        kIntent_Idle          = 11,  // ch.12 one-shot idle/animation
        kIntent_ShoutPower    = 12,  // ch.14 shout/power selection
        kIntent_Equipment     = 13,  // ch.15 unequip/equip a worn item (melee-vs-ranged lever)
    };

    // The interface: a POD struct of function pointers. NO vtable. `abiVersion`
    // is the first field so a client can sanity-check the layout it received.
    struct APMF_API_v1 {
        std::uint32_t abiVersion;   // == kABIVersion of the APMF that produced it

        // Register a control claim: engage `intent`'s channel on the NPC `actor`
        // (a FormID) at arbitration weight `basis`. Returns a handle to release
        // later, or kInvalidHandle if no channel serves `intent`. When two clients
        // claim the SAME channel on the SAME NPC, the higher `basis` owns it; on a
        // tie the earlier claim owns it. The channel stays engaged until the LAST
        // claim is released. Thread-safe (enqueues; applied on the game thread).
        Handle (*Request)(RE::FormID actor, Intent intent, float basis);

        // Release a claim previously returned by Request. When the last claim on a
        // channel is released, APMF restores the AI (un-blocks). No-op on an
        // unknown/stale handle. Thread-safe (enqueues). (Complete is a synonym for
        // Release; there is one release operation.)
        void (*Release)(Handle handle);
    };

    // Function-pointer type for GetProcAddress(kGetInterfaceExport).
    using GetInterface_t = const APMF_API_v1* (*)(std::uint32_t abiVersion);

}
