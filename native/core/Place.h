#pragma once
#include "APMF_API.h"
#include "core/Sources.h"

// ============================================================================
// ABI v28 PLACE (kIntent_Place): put an actor beside a reference, or at a point in
// an INTERIOR cell, with the engine's own move, ONCE, on the game thread. It works on
// an UNLOADED persistent actor. Docs/FACETS.md "Place", MAP.md core/Place.
//
// THE ENGINE ROAD (disassembly, all three builds): Papyrus ObjectReference.MoveTo
// queues a task whose Run (AE 56101 / SE 55572) calls TESObjectREFR::MoveTo_Impl
// (AE 56626 0xA447F0 / SE 56227 0x9AE5C0 / 1.7.104 0xA5D370) with the target's
// parent cell, its worldspace, the point and the rotation. For a Character that
// body calls StopInteractingQuick(true) and the actor relocation (AE 37313) with
// (point, rotation Z, cell, worldspace); the handle argument is read on the player
// branch only. Harbinger makes that same call (verified row
// Place.TESObjectREFR.MoveTo), on the game thread.
//
// NEVER A CLAIM (the kCastFlag_AtPosition shape): ClientAPI diverts the request here
// before the control map, so it holds no facet, switches nothing on and is never
// swept. The handle comes from ControlMap::NextHandle(), so it never collides with
// a claim's. The outcome is a per-handle record read by GetPlaceState (APMF_API_v28).
// ============================================================================

namespace apmf::place {

    // kDataLoaded: the runtime gate (exactly 1.6.1170 / 1.5.97 / 1.7.104, VR refused) and the
    // SeatVerified row. No hook, no INI switch.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // ANY THREAD (ClientAPI). The synchronous checks (installed, actor non-zero and not the player,
    // destination non-zero, finite offset / yaw, no unknown flag bit), then a Queued record and ONE
    // apmf::mainthread::Post of the move. NO form lookup here. `a_client` is the source that filed
    // `a_handle`, captured at the call (so a Release before the move does not orphan it). false = refused
    // (one WARN line; the caller returns kInvalidHandle and keeps no record).
    bool Enqueue(APMF_API::Handle a_handle, RE::FormID a_actor, const APMF_API::APMF_Param* a_param,
                 apmf::sources::SourceId a_client);

    // ANY THREAD. GetPlaceState (APMF_API_v28): copy under the leaf lock, no game call.
    std::uint32_t GetState(APMF_API::Handle a_handle, APMF_API::APMF_PlaceStateInfo* a_out);

    // Revert / kPreLoadGame: forget every record. A placement still queued is dropped with the
    // main-thread queue's Discard() in plugin.cpp; its record goes here (it reads Unknown after).
    void ResetAll(const char* a_why);

}
