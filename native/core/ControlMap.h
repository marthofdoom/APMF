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
// every frame. An UNCONTROLLED NPC pays near-zero: a relaxed atomic<size_t> check
// (m_anyControlled) that skips the shared_ptr traffic entirely, then at most one
// atomic snapshot load + ONE hash lookup that misses -- no allocation, no iteration
// over all NPCs. Only a controlled NPC runs its channels' per-tick work (and most
// channels no-op there).
//
// THREADING -- RCU SNAPSHOT (INVARIANTS #12/#13). Field evidence ([threadcheck],
// 2026-09-02) proved the 0xAD hook does NOT run on one serial thread: a Character
// seat fired on a different worker thread than the PlayerCharacter/Drain seat. The
// WRITER side stays single-threaded (Drain on the PlayerCharacter seat, and the
// SKSE revert/kPreLoadGame callbacks -- all confirmed the same MAIN thread; only the
// per-NPC Character seats are parallelized across workers). The READER side
// (OnActorUpdate) is NOT single-threaded. So instead of one mutable map read
// unlocked, the map is published as an immutable snapshot:
//   * API calls only ENQUEUE a small POD op under a brief queue lock (m_queue/m_qmx)
//     -- unchanged, any thread, never touch the map.
//   * The WRITER (Drain/ReleaseAll/Clear -- all on the single writer thread) builds
//     a private COPY of the last-published map (m_current), applies ops/the unload
//     sweep/a wipe against that copy, and -- ONLY if something actually changed --
//     atomically PUBLISHES it (Publish(): m_published.store(newSnapshot, release)).
//     A no-op Drain frame allocates nothing.
//   * READERS (OnActorUpdate, any thread) atomically LOAD a local shared_ptr copy of
//     m_published (acquire) and read that frozen generation lock-free: no torn
//     reads, no UAF even if the writer publishes a newer generation mid-call (the
//     reader's local shared_ptr keeps its own generation alive via refcount).
//   * Every NpcCtl field is read-only to readers once published, with ONE sanctioned
//     exception -- NpcCtl::obsTick is a `mutable std::atomic<uint64_t>` (relaxed) so
//     OnActorUpdate's per-tick counter can still fetch_add through the const
//     snapshot. No other field may ever be reader-mutated; a future channel's Tick()
//     must not write anything else in NpcCtl/ChannelCtl/Claim.
//   * m_index and m_queue are UNCHANGED: m_index has no reader (Drain-thread-only,
//     never consulted by OnActorUpdate) so it needs no snapshot wrapper; m_queue
//     stays m_qmx-guarded exactly as before.
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
        // `param` (may be null) is COPIED synchronously into the queued op — APMF
        // never retains the client's pointer; null == a zero param (channel default).
        Handle EnqueueRequest(RE::FormID actor, Intent intent, float basis,
                              const APMF_API::APMF_Param* param);
        void   EnqueueRelease(Handle handle);
        // Re-point an existing claim's param in place (same handle). `param` copied
        // synchronously; null == no-op. Applied at the next Drain. See APMF_API_v3.
        void   EnqueueRepoint(Handle handle, const APMF_API::APMF_Param* param);

        // ---- Hot path: ANY thread (RCU reader -- field-proven the 0xAD Character
        // seat is not single-threaded; see the header comment above). ----
        // One snapshot load + one lookup; ticks the engaged channels of a controlled NPC.
        void OnActorUpdate(RE::Actor* actor);

        // ---- Allowance-template read: ANY thread (RCU reader). See
        // Docs/ALLOWANCE-TEMPLATE.md §3/§5 -- the T2 hooks (core/Allowance.h's
        // InstallOnVtables/Allowed) call this from combat-thread thunks. Look up
        // whether `actor` has a winning claim on `intent`'s channel; if so, copy
        // its APMF_Param out and return true. Same RCU discipline as
        // OnActorUpdate: relaxed pre-gate, one acquire-load, one hash lookup on a
        // frozen snapshot generation. Read-only -- does NOT touch obsTick (a
        // separate reader path from OnActorUpdate), never blocks, no
        // follower-list touch, no mutation of anything.
        bool TryGetOwningClaim(RE::FormID actor, Intent intent,
                               APMF_API::APMF_Param& outParam) const;

        // ---- Observability/probe use only (Docs/SPEC-PACKAGE-HOLD.md §4): live
        // Actor* for every actor CURRENTLY claimed on `intent`'s channel (unloaded
        // NPCs filtered via the same npc.handle.get() liveness check OnActorUpdate/
        // TryGetOwningClaim already use). Same RCU reader discipline (any thread) --
        // relaxed pre-gate, one acquire-load of a frozen snapshot generation. The
        // controlled set is small (INVARIANTS #13) so this returns a fresh vector
        // rather than an iterator; meant for a ~250ms-cadence diagnostic poll, not a
        // hot per-frame path. ----
        std::vector<RE::Actor*> ClaimedActors(Intent intent) const;

        // ---- Writer thread ONLY (the PlayerCharacter/Drain seat and the SKSE
        // revert/preload callbacks -- all the same MAIN thread). ----
        // Once per frame: apply queued ops, then sweep unloaded controlled NPCs;
        // publishes a new snapshot only if something changed.
        void Drain();
        // Restore + clear every controlled NPC (disengage-all / kPreLoadGame).
        void ReleaseAll(const char* why);

        // Wipe all control state WITHOUT restoring (revert / new game): the actors
        // are being replaced, so a restore is wrong -- the co-saved AV ledger
        // (core/AvLedger) handles the incoming save's overrides on post-load.
        void Clear();

        // Observability: how many NPCs are currently controlled (any thread; relaxed).
        std::size_t ControlledCount() const { return m_anyControlled.load(std::memory_order_relaxed); }

        // [threadcheck retirement] one-time disclosure of whether the RCU snapshot
        // pointer is actually lock-free on this toolchain -- see Hook.cpp. Exposed
        // here so Hook::Install can log it without reaching into ControlMap's guts.
        static bool SnapshotIsLockFree();

    private:
        struct Claim {
            Handle               handle = APMF_API::kInvalidHandle;
            float                basis  = 0.0f;
            APMF_API::APMF_Param param  = {};   // what this claim wants the channel to act on
        };
        struct ChannelCtl {
            Channel*           channel = nullptr;
            std::vector<Claim> claims;   // engaged <=> !claims.empty()
        };
        // RCU snapshot node. Once published, every field here is READ-ONLY to reader
        // threads except `obsTick` -- the one sanctioned exception (see the header
        // comment above). `mutable` + atomic lets OnActorUpdate fetch_add it through
        // the const NpcCtl& a snapshot lookup hands back.
        struct NpcCtl {
            RE::ActorHandle                    handle;
            RE::FormID                         pkgAtCapture = 0;
            std::vector<ChannelCtl>            channels;     // only engaged channels
            mutable std::atomic<std::uint64_t> obsTick{ 0 };

            NpcCtl() = default;
            // std::atomic disables the implicit copy ctor -- the writer thread needs
            // to deep-copy NpcCtl (building the next snapshot generation from the
            // last-published one), so make the copy explicit: obsTick is copied via
            // a relaxed load (it is a pure logging-cadence counter with no ordering
            // dependency on anything else).
            NpcCtl(const NpcCtl& o)
                : handle(o.handle), pkgAtCapture(o.pkgAtCapture), channels(o.channels),
                  obsTick(o.obsTick.load(std::memory_order_relaxed)) {}
            // No move ctor/assignment and no copy-assignment declared: the atomic
            // member makes them ill-formed to default, and nothing in ControlMap
            // needs them -- unordered_map builds nodes in place (operator[]/emplace)
            // and the one deep-copy site (MapType next = *m_current) uses the copy
            // ctor above, never assignment.
        };
        using MapType = std::unordered_map<RE::FormID, NpcCtl>;

        struct PendingOp {
            enum class Kind : std::uint8_t { kRequest, kRelease, kRepoint } kind{};
            Handle               handle = APMF_API::kInvalidHandle;
            RE::FormID           actor  = 0;         // request only
            Intent               intent = APMF_API::kIntent_None;   // request only
            float                basis  = 0.0f;      // request only
            APMF_API::APMF_Param param  = {};        // request/repoint (copied at enqueue)
        };

        // All three apply against the WRITER's private working copy (`map`), never a
        // member -- writer thread only, called from Drain()/ReleaseAll() before that
        // copy is (maybe) published. Return whether they actually changed `map`, so
        // the caller only Publish()es on a real change (copy-on-CHANGE, not
        // copy-every-frame).
        bool ApplyRequest(const PendingOp& op, MapType& map);
        bool ApplyRelease(Handle handle, MapType& map);
        bool ApplyRepoint(Handle handle, const APMF_API::APMF_Param& param, MapType& map);

        // Writer-thread-only choke point: takes ownership of the finished working
        // copy, makes it the new immutable snapshot, and publishes it for readers.
        // The ONLY place m_current/m_published/m_anyControlled are written.
        void Publish(MapType&& next);

        // Writer-thread-only "last published" handle -- Drain/ReleaseAll/Clear always
        // build their next working copy from *m_current (never touched by readers).
        std::shared_ptr<const MapType> m_current = std::make_shared<const MapType>();

        // Reader-visible RCU snapshot. Written ONLY via Publish() (writer thread);
        // read via a LOCAL shared_ptr copy + acquire load from OnActorUpdate (any
        // thread) -- see the header comment above and ControlMap.cpp.
        std::atomic<std::shared_ptr<const MapType>> m_published{ m_current };

        // Relaxed pre-gate so an uncontrolled world never touches m_published at all
        // (INVARIANTS #13) -- mirrors m_current->size(), updated only in Publish().
        std::atomic<std::size_t> m_anyControlled{ 0 };

        std::unordered_map<Handle, std::pair<RE::FormID, Channel*>>   m_index;   // writer-thread-only, no reader -- unchanged, no snapshot wrapper needed

        // Cross-thread queue (the ONLY shared-with-workers state).
        std::vector<PendingOp> m_queue;
        std::mutex             m_qmx;
        std::atomic<Handle>    m_nextHandle{ 1 };   // 0 is kInvalidHandle
    };

}
