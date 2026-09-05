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
        // Attach a bounded allow-set of spell FormIDs to an existing claim (same
        // handle), clamped to APMF_API::kMaxSpellAllowList and copied synchronously.
        // `forms` may be null (== count 0, clears the allow-set). Applied at the
        // next Drain: no-op on an unknown handle or a claim not on the
        // kIntent_SelectSpell channel. See APMF_API_v4 / SetSpellAllowList.
        void   EnqueueSetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count);

        // ABI v5 (ch.8b, kIntent_Cast): claim the cast-EXECUTION facet for a bounded
        // TTL window. `req` (may be null) is COPIED synchronously; APMF never retains
        // the client pointer. Allocates + returns a handle synchronously (like
        // EnqueueRequest); the claim is applied at the next Drain, where a
        // kCastFlag_FromPackage request extracts spell+target on the WRITER thread
        // (form lookups legal there) and REFUSES -- dropping the op, never running the
        // package -- if the package carries no readable spell input. Returns
        // kInvalidHandle only if no channel serves kIntent_Cast.
        Handle EnqueueCast(RE::FormID actor, float basis, const APMF_API::APMF_CastRequest* req);

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
        // Overload that also hands back the winning claim's allow-set (ch.8's
        // SetSpellAllowList addition, APMF_API_v4). `outAllowSet` (may be null to
        // skip it) must have room for at least APMF_API::kMaxSpellAllowList
        // RE::FormIDs; `outAllowCount` receives how many were copied (0 if the
        // winning claim has no allow-set). The allow-set is copied OUT BY VALUE
        // (bounded, <=32 uint32_t -- cheap) rather than handed back as a
        // pointer/span into the snapshot's internal Claim storage, so the result
        // has no RCU-lifetime coupling to this call's local snapshot generation
        // once TryGetOwningClaim returns -- the same "copy out, don't alias"
        // discipline `outParam` already uses for the primary param, just applied
        // to the allow-set too. Internal C++ only -- NOT part of the C-ABI, free
        // to change shape.
        bool TryGetOwningClaim(RE::FormID actor, Intent intent, APMF_API::APMF_Param& outParam,
                               RE::FormID* outAllowSet, std::uint32_t& outAllowCount) const;

        // ch.8b (kIntent_Cast): hand back the winning cast claim's spell (param.form)
        // AND its runtime FF-form proxy (castProxy) -- the two FormIDs the cast gates
        // (CastGate/EquipGate via Allowance::AllowedCast) allow while the claim
        // stands. Same RCU reader discipline as TryGetOwningClaim (any thread; relaxed
        // pre-gate, one acquire-load, one hash lookup on a frozen snapshot). Returns
        // false (and leaves outputs 0) when the actor has no winning kIntent_Cast
        // claim. castProxy is not expressible through APMF_Param, hence this dedicated
        // read. `outFlags` (optional, default nullptr -- existing callers unaffected)
        // additionally hands back the claim's raw CastFlags (APMF_API::kCastFlag_*),
        // e.g. kCastFlag_LeftHand -- the per-hand deny (2026-09-0x, INVARIANTS #18)
        // reads this to scope the narrowing to the claimed hand only. Internal C++
        // only -- not part of the C-ABI.
        bool TryGetCastClaim(RE::FormID actor, RE::FormID& outSpell, RE::FormID& outProxy,
                             std::uint32_t* outFlags = nullptr) const;

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
            // ch.8 SetSpellAllowList (APMF_API_v4): a bounded ADDITIONAL allow-set
            // of spell FormIDs a kIntent_SelectSpell claim's AI may also cast,
            // beyond `param.form`. Fixed-size POD array (no heap, no std::vector) so
            // Claim stays trivially copyable -- required for the RCU snapshot's
            // per-Publish deep-copy (NpcCtl's copy ctor deep-copies `channels`,
            // which deep-copies every ChannelCtl::claims, which deep-copies every
            // Claim by value). altCount == 0 (the default) means "no allow-set" --
            // a claim that never calls SetSpellAllowList reads byte-identically to
            // before this field existed.
            RE::FormID     altForms[APMF_API::kMaxSpellAllowList]{};
            std::uint32_t  altCount = 0;
            // ch.8b cast-execution claim (APMF_API_v5, kIntent_Cast). Appended at the
            // END so the RCU deep-copy stays a trivially-copyable POD copy. All zero
            // for every non-cast claim -- byte-identical to before these existed.
            // expiresMs is the ONLY TTL in the map: 0 = no TTL (all existing intents);
            // nonzero = a bounded cast claim the Drain TTL pass auto-releases at expiry
            // (never a re-assert -- design.md §5a "never a standing hold").
            RE::FormID     castProxy  = 0;   // second allowed FormID for the cast facet
            RE::FormID     castTarget = 0;   // record only -- APMF never aims
            std::uint32_t  castFlags  = 0;   // kCastFlag_*
            std::uint64_t  expiresMs  = 0;   // 0 = no TTL; nonzero = monotonic-ms deadline
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
            enum class Kind : std::uint8_t { kRequest, kRelease, kRepoint, kSetAllowList, kCast } kind{};
            Handle               handle = APMF_API::kInvalidHandle;
            RE::FormID           actor  = 0;         // request/cast only
            Intent               intent = APMF_API::kIntent_None;   // request/cast only
            float                basis  = 0.0f;      // request/cast only
            APMF_API::APMF_Param param  = {};        // request/repoint/cast (copied at enqueue;
                                                     //   cast: form = spell, ival = flags)
            // kCast only (APMF_API_v5): the rich cast payload, copied at enqueue. On a
            // degenerate RequestEx(kIntent_Cast) these stay 0 and ApplyRequest reads
            // flags from param.ival with a default TTL. See EnqueueCast/ApplyRequest.
            RE::FormID           castProxy  = 0;
            RE::FormID           castTarget = 0;
            std::uint32_t        castFlags  = 0;
            std::uint32_t        ttlMs      = 0;
            // kSetAllowList only: copied synchronously at enqueue, same discipline
            // as `param` above. altCount is pre-clamped to kMaxSpellAllowList by
            // EnqueueSetSpellAllowList so Apply never has to re-check the bound.
            RE::FormID           altForms[APMF_API::kMaxSpellAllowList]{};
            std::uint32_t        altCount = 0;
        };

        // All four apply against the WRITER's private working copy (`map`), never a
        // member -- writer thread only, called from Drain()/ReleaseAll() before that
        // copy is (maybe) published. Return whether they actually changed `map`, so
        // the caller only Publish()es on a real change (copy-on-CHANGE, not
        // copy-every-frame).
        bool ApplyRequest(const PendingOp& op, MapType& map);
        bool ApplyRelease(Handle handle, MapType& map);
        bool ApplyRepoint(Handle handle, const APMF_API::APMF_Param& param, MapType& map);
        // Writer-thread-only, mirrors ApplyRepoint's shape exactly: look the claim
        // up via m_index, write altForms/altCount on the matching Claim regardless
        // of whether it currently OWNS the channel (non-owning semantics, same as
        // Repoint) -- takes effect immediately if it owns (no engine write to make;
        // Allowance::Allowed reads the stored claim directly), otherwise if/when it
        // later wins arbitration. No-op on an unknown handle or a claim not on the
        // kIntent_SelectSpell channel.
        bool ApplySetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count, MapType& map);

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
