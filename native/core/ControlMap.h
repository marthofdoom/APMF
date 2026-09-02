#pragma once
#include "APMF_API.h"
#include "core/Channel.h"

// ============================================================================
// APMF core -- the MULTI-NPC CONTROL MAP. The scalable heart of Phase 1: any
// number of NPCs controlled independently and simultaneously (design.md, ROADMAP
// Phase 1). Replaces v0.1.0's single crosshair-captured target.
//
// SHAPE. A hash map keyed by NPC FormID -> that NPC's control state (its engaged
// channels, and per channel the list of client claims). Client Requests engage a
// channel on an NPC; the channel stays engaged until the LAST claim is released.
//
// PERFORMANCE (INVARIANTS #13). The 0xAD hook calls OnActorUpdate for EVERY NPC
// every frame. An UNCONTROLLED NPC pays near-zero: an empty-map check, then at most
// ONE hash lookup that misses -- no allocation, no iteration over all NPCs. Only a
// controlled NPC runs its channels' per-tick work (and most channels no-op there).
//
// THREADING -- SINGLE-WRITER (INVARIANTS #12). The 0xAD hook runs on the GAME
// thread; client API calls (Request/Release) may arrive from a client's WORKER
// thread. So:
//   * API calls only ENQUEUE a small POD op under a brief queue lock. They never
//     touch the map.
//   * The map (and the handle index) is mutated ONLY on the game thread: Drain()
//     applies the queued ops once per frame, and ReleaseAll() clears everything
//     (both game/main thread). No other code path writes the map.
//   * The per-NPC per-frame hot path (OnActorUpdate) READS the map with NO lock --
//     safe because every writer is the same serial game thread.
// Handles are allocated with an atomic counter so Request() can return synchronously
// before the op is drained; ops are FIFO so a Release enqueued right after its
// Request is applied after it.
// ============================================================================

namespace apmf {

    using Handle = APMF_API::Handle;
    using Intent = APMF_API::Intent;

    class ControlMap {
    public:
        static ControlMap& Get();

        // ---- Client/API side: THREAD-SAFE (any thread). Enqueue only. ----
        // Allocate + return a handle synchronously; the claim is applied at the next
        // game-thread Drain. Returns kInvalidHandle if no channel serves `intent`.
        Handle EnqueueRequest(RE::FormID actor, Intent intent, float basis);
        void   EnqueueRelease(Handle handle);

        // ---- Game thread ONLY. ----
        // Hot path: one lookup; ticks the engaged channels of a controlled NPC.
        void OnActorUpdate(RE::Actor* actor);
        // Once per frame: apply queued ops, then sweep unloaded controlled NPCs.
        void Drain();
        // Restore + clear every controlled NPC (disengage-all / kPreLoadGame).
        void ReleaseAll(const char* why);

        // Observability: how many NPCs are currently controlled (game thread).
        std::size_t ControlledCount() const { return m_map.size(); }

    private:
        struct Claim {
            Handle handle = APMF_API::kInvalidHandle;
            float  basis  = 0.0f;
        };
        struct ChannelCtl {
            Channel*           channel = nullptr;
            std::vector<Claim> claims;   // engaged <=> !claims.empty()
        };
        struct NpcCtl {
            RE::ActorHandle         handle;
            RE::FormID              pkgAtCapture = 0;
            std::vector<ChannelCtl> channels;     // only engaged channels
            std::uint64_t           obsTick = 0;
        };
        struct PendingOp {
            enum class Kind : std::uint8_t { kRequest, kRelease } kind{};
            Handle     handle = APMF_API::kInvalidHandle;
            RE::FormID actor  = 0;         // request only
            Intent     intent = APMF_API::kIntent_None;   // request only
            float      basis  = 0.0f;      // request only
        };

        void ApplyRequest(const PendingOp& op);   // game thread
        void ApplyRelease(Handle handle);         // game thread

        // Game-thread state (NO lock -- single game-thread writer).
        std::unordered_map<RE::FormID, NpcCtl>                        m_map;
        std::unordered_map<Handle, std::pair<RE::FormID, Channel*>>   m_index;

        // Cross-thread queue (the ONLY shared-with-workers state).
        std::vector<PendingOp> m_queue;
        std::mutex             m_qmx;
        std::atomic<Handle>    m_nextHandle{ 1 };   // 0 is kInvalidHandle
    };

}
