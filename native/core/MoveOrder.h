#pragma once
#include "core/MoveOrderLogic.h"

#include <string>

// THE STRICT MOVEMENT ORDER (ch.24 kApproach_Strict, ABI v48). See core/MoveOrder.cpp. The channel
// (channels/CombatApproach.cpp) owns the claim and calls Publish / Clear; the seats read the slot table.
namespace apmf::moveorder {

    // kDataLoaded, BEFORE actiongate::Install (which chains its own act() over the selector vtable): verifies every row
    // and byte guard, then writes the six vtable seats, or none.
    void Install();

    // Strict orders are served: the seats are in and ch.24 is installed. Any thread.
    bool          Served();
    const char*   NotServedReason();
    std::uint32_t Caps();   // APMF_API::MoveCaps bits, 0 while not served

    // Any thread. Room for one more order on `a_fid` (he has a slot, or one is free).
    bool HasRoom(RE::FormID a_fid);

    // GAME THREAD. The order on `a_s.fid` is live with these values (a_s.serial is filled here). Writes only what
    // changed; a new a_s.gen starts the runtime block over. False = no free slot.
    bool Publish(movelogic::Snapshot a_s, std::uint64_t a_worldMs);
    // GAME THREAD. The order is not live any more (SUMMARY line; `a_repoint` = its successor follows, the line is throttled).
    // No-op without a slot.
    void Clear(RE::FormID a_fid, const char* a_why, std::uint64_t a_worldMs, bool a_repoint = false);
    void ResetAll(const char* a_why);
    std::uint32_t NextGen();

    // GAME THREAD (ch.24 Poll): the phase-change lines and the per-actor status line.
    void PollLines(RE::FormID a_fid, std::uint64_t a_worldMs);
    void HeartbeatLine();

    // Any thread.
    std::uint32_t PhaseOf(RE::FormID a_fid);
    std::uint32_t GetState(RE::FormID a_fid, APMF_API::APMF_MoveOrderInfo* a_out);
    // APMF_API_v48::SetMoveOrder.
    std::uint32_t SetOrder(APMF_API::Handle a_claim, const APMF_API::APMF_MoveOrder* a_order);

}
