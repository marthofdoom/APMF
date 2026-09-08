#include "PCH.h"
#include "core/Log.h"
#include "core/ControlMap.h"
#include "core/Registry.h"
#include "core/Clock.h"
#include "channels/CastCompose.h"   // castcompose::ExtractFromPackage (ch.8b FromPackage read)
#include "core/CastProxy.h"         // castproxy::Acquire/Free (ch.8b kSelf delivery-flip, writer thread)
#include "core/MainThread.h"        // mainthread::Post (defer proxy teardown past this Drain's Publish)

namespace apmf {

    namespace {
        constexpr std::uint64_t kObsEvery = 60;   // per-NPC observability ~1/s @ 60fps

        const char* PkgTypeName(RE::TESPackage* pkg) {
            if (!pkg) return "<none>";
            const char* n = pkg->GetObjectTypeName();
            return n ? n : "<unnamed>";
        }

        // feat/per-hand-cast-claims: does a claim carrying `castFlags` occupy
        // `hand`? A kCastFlag_DualCast claim occupies BOTH hands at once (matches
        // either query); otherwise the claim's own hand hint decides (default
        // right, per APMF_API.h's CastFlags comment) -- never anything the QUERIER
        // does. Never called with `hand == CastHand::kUnknown` -- every call site
        // forwards that case to the unscoped (any-hand) reads before reaching here.
        bool ClaimOccupiesHand(std::uint32_t castFlags, CastHand hand) {
            if (castFlags & APMF_API::kCastFlag_DualCast) return true;
            const CastHand claimHand =
                (castFlags & APMF_API::kCastFlag_LeftHand) ? CastHand::kLeft : CastHand::kRight;
            return claimHand == hand;
        }

        // feat/per-hand-cast-claims: does `castFlags` describe a kCastFlag_DualCast
        // claim? Small shared predicate for the dual-vs-single-hand mutual
        // exclusivity check in ApplyRequest below.
        bool IsDualCastFlags(std::uint32_t castFlags) {
            return (castFlags & APMF_API::kCastFlag_DualCast) != 0;
        }
    }

    ControlMap& ControlMap::Get() {
        static ControlMap s_instance;
        return s_instance;
    }

    bool ControlMap::SnapshotIsLockFree() {
        // Query on a throwaway instance -- is_lock_free() only depends on the atomic
        // specialization (the platform's shared_ptr atomic implementation), not on
        // the pointee, so this is representative of m_published without touching it.
        std::atomic<std::shared_ptr<const MapType>> probe;
        return probe.is_lock_free();
    }

    // ---- Writer-thread-only RCU publish. The ONE place m_current/m_published/
    // m_anyControlled are written. See ControlMap.h for the full model. ----
    void ControlMap::Publish(MapType&& next) {
        const std::size_t n = next.size();
        m_current = std::make_shared<const MapType>(std::move(next));
        m_published.store(m_current, std::memory_order_release);   // pairs with OnActorUpdate's acquire load
        m_anyControlled.store(n, std::memory_order_relaxed);
    }

    // ---- Client/API side (ANY thread): enqueue only, never touch the map. ----

    Handle ControlMap::EnqueueRequest(RE::FormID actor, Intent intent, float basis,
                                      const APMF_API::APMF_Param* param) {
        // Registry is immutable after load, so this read is thread-safe.
        if (!Registry::Get().ChannelForIntent(intent)) {
            spdlog::warn("[api] Request REFUSED -- no channel serves intent {} (actor 0x{}).",
                         static_cast<std::uint32_t>(intent), apmf::log::Hex(actor));
            return APMF_API::kInvalidHandle;
        }
        const Handle h = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
        PendingOp op{};
        op.kind   = PendingOp::Kind::kRequest;
        op.handle = h;
        op.actor  = actor;
        op.intent = intent;
        op.basis  = basis;
        if (param) op.param = *param;   // COPY synchronously; never retain the client pointer
        {
            std::scoped_lock lock(m_qmx);
            m_queue.push_back(op);
        }
        return h;
    }

    void ControlMap::EnqueueRelease(Handle handle) {
        if (handle == APMF_API::kInvalidHandle) return;
        PendingOp op{};
        op.kind   = PendingOp::Kind::kRelease;
        op.handle = handle;
        {
            std::scoped_lock lock(m_qmx);
            m_queue.push_back(op);
        }
    }

    void ControlMap::EnqueueRepoint(Handle handle, const APMF_API::APMF_Param* param) {
        if (handle == APMF_API::kInvalidHandle || !param) return;
        PendingOp op{};
        op.kind   = PendingOp::Kind::kRepoint;
        op.handle = handle;
        op.param  = *param;   // COPY synchronously; never retain the client pointer
        {
            std::scoped_lock lock(m_qmx);
            m_queue.push_back(op);
        }
    }

    // ABI v4 (ch.8's SetSpellAllowList): attach a bounded allow-set to an existing
    // claim. `forms` is READ AND COPIED synchronously here -- APMF never retains
    // the client's pointer, same contract as RequestEx/Repoint's `param`. Clamped
    // to kMaxSpellAllowList at enqueue time (not in Apply) so the queued op itself
    // is already bounded -- no unbounded write possible downstream.
    void ControlMap::EnqueueSetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count) {
        if (handle == APMF_API::kInvalidHandle) return;
        PendingOp op{};
        op.kind   = PendingOp::Kind::kSetAllowList;
        op.handle = handle;
        if (forms && count > 0) {
            op.altCount = (count < APMF_API::kMaxSpellAllowList) ? count : APMF_API::kMaxSpellAllowList;
            for (std::uint32_t i = 0; i < op.altCount; ++i) op.altForms[i] = forms[i];
        }
        // else: altCount stays 0 -- CLEARS the allow-set on Apply (forms==nullptr
        // or count==0 both mean "no allow-set").
        {
            std::scoped_lock lock(m_qmx);
            m_queue.push_back(op);
        }
    }

    // ABI v5 (ch.8b, kIntent_Cast): claim the cast-EXECUTION facet for a bounded
    // window. Mirrors EnqueueRequest's shape (allocate a handle synchronously,
    // enqueue a POD op applied at the next Drain) but carries the rich cast payload.
    // `req` is COPIED synchronously here -- APMF never retains the client pointer.
    // Any FromPackage extraction happens LATER on the writer thread inside
    // ApplyRequest (form lookups are legal there, not here off-thread).
    Handle ControlMap::EnqueueCast(RE::FormID actor, float basis,
                                   const APMF_API::APMF_CastRequest* req) {
        if (!Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast)) {
            spdlog::warn("[api] RequestCast REFUSED -- no channel serves kIntent_Cast (actor 0x{}).",
                         apmf::log::Hex(actor));
            return APMF_API::kInvalidHandle;
        }
        const Handle h = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
        PendingOp op{};
        op.kind   = PendingOp::Kind::kCast;
        op.handle = h;
        op.actor  = actor;
        op.intent = APMF_API::kIntent_Cast;
        op.basis  = basis;
        if (req) {
            op.param.form  = req->spell;                        // form = spell (or the package if FromPackage)
            op.param.ival  = static_cast<std::int32_t>(req->flags);   // keep param.ival == castFlags (degenerate-form parity)
            op.castProxy   = req->proxy;
            op.castTarget  = req->target;
            op.castFlags   = req->flags;
            op.ttlMs       = req->ttlMs;
        }
        {
            std::scoped_lock lock(m_qmx);
            m_queue.push_back(op);
        }
        return h;
    }

    // ---- Writer thread ONLY (Drain/ApplyRequest/ApplyRelease/ApplyRepoint/
    // ApplySetSpellAllowList/ReleaseAll/Clear). OnActorUpdate below is the
    // exception -- ANY thread. ----

    void ControlMap::Drain() {
        // Move the queued ops out under the lock, then apply them lock-free.
        std::vector<PendingOp> ops;
        {
            std::scoped_lock lock(m_qmx);
            if (m_queue.empty()) { /* fall through to the sweep */ }
            else ops.swap(m_queue);
        }

        // Cheap READ-ONLY pre-check against the currently-published map: is there
        // any work at all this frame? Avoids the deep copy-on-write below on the
        // (common) no-op frame -- ops empty AND nothing unloaded -- so a truly quiet
        // frame allocates nothing at all, not even the working copy.
        if (ops.empty()) {
            bool anyWork = false;
            const auto nowMs = apmf::clock::MonotonicMs();
            for (const auto& kv : *m_current) {
                if (!kv.second.handle.get()) { anyWork = true; break; }   // unloaded -> sweep
                // A bounded cast claim whose window elapsed needs an auto-release pass
                // even with no queued op (design.md §5a TTL, NOT a re-assert loop).
                for (const auto& cc : kv.second.channels) {
                    for (const auto& cl : cc.claims) {
                        if (cl.expiresMs != 0 && nowMs >= cl.expiresMs) { anyWork = true; break; }
                    }
                    if (anyWork) break;
                }
                if (anyWork) break;
            }
            if (!anyWork) return;
        }

        // RCU: build a private working copy of the last-published snapshot -- only
        // reached when there's real work (an op arrived, or something unloaded).
        // Deep-copies the (small, controlled-NPCs-only) map -- cheap -- and is
        // published ONLY if something actually changed.
        MapType next = *m_current;
        bool changed = false;
        for (const auto& op : ops) {
            switch (op.kind) {
            case PendingOp::Kind::kRequest:      changed |= ApplyRequest(op, next);                  break;
            case PendingOp::Kind::kRelease:      changed |= ApplyRelease(op.handle, next);           break;
            case PendingOp::Kind::kRepoint:      changed |= ApplyRepoint(op.handle, op.param, next); break;
            case PendingOp::Kind::kSetAllowList: changed |= ApplySetSpellAllowList(op.handle, op.altForms, op.altCount, next); break;
            case PendingOp::Kind::kCast:         changed |= ApplyRequest(op, next);                  break;   // ch.8b -- ApplyRequest handles the cast branch
            }
        }

        // TTL expiry pass (ch.8b, design.md §5a): auto-RELEASE -- the opposite of a
        // re-assert -- every bounded cast claim whose window has elapsed, so a
        // crashed/forgetful client can never leave a standing cast hold. Collect
        // first (do not mutate `next` while iterating it), then ApplyRelease each.
        {
            const auto nowMs = apmf::clock::MonotonicMs();
            std::vector<std::pair<Handle, RE::FormID>> expired;
            for (const auto& [fid, ctl] : next) {
                for (const auto& cc : ctl.channels) {
                    for (const auto& cl : cc.claims) {
                        if (cl.expiresMs != 0 && nowMs >= cl.expiresMs)
                            expired.emplace_back(cl.handle, fid);
                    }
                }
            }
            for (const auto& [h, fid] : expired) {
                if (ApplyRelease(h, next)) {
                    changed = true;
                    spdlog::info("[ch.8b] cast claim 0x{} expired (h={}) -- auto-released.",
                                 apmf::log::Hex(fid), h);
                }
            }
        }

        // Sweep controlled NPCs that have unloaded (they stop calling OnActorUpdate,
        // so only this periodic pass reclaims them). Cheap: iterates the (small)
        // control map, never all NPCs.
        for (auto it = next.begin(); it != next.end();) {
            auto& ctl = it->second;
            if (ctl.handle.get()) { ++it; continue; }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->first);   // may be null
            for (auto& cs : ctl.channels) {
                if (cs.channel) cs.channel->Release(it->first, actor);
                for (auto& c : cs.claims) m_index.erase(c.handle);
            }
            spdlog::info("[ctl] 0x{} unloaded -- released {} channel(s), dropped from control.",
                         apmf::log::Hex(it->first), ctl.channels.size());
            it = next.erase(it);
            changed = true;
        }

        if (changed) Publish(std::move(next));
        // else: `next` is discarded here -- zero reader-visible churn, no publish.
    }

    bool ControlMap::ApplyRequest(const PendingOp& op, MapType& map) {
        auto* channel = Registry::Get().ChannelForIntent(op.intent);
        if (!channel) return false;   // (already checked at enqueue; defensive)

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(op.actor);
        if (!actor) {
            spdlog::warn("[ctl] request h={} ignored -- actor 0x{} not found/loaded.",
                         op.handle, apmf::log::Hex(op.actor));
            return false;
        }

        // ch.8b (kIntent_Cast): resolve the effective spell/target, extract a
        // FromPackage request, and stamp a bounded TTL -- all BEFORE map[op.actor]
        // creates an entry, so a refusal leaves NO spurious NpcCtl behind. APMF makes
        // NO cast write here; it only records the claim the gates will read.
        APMF_API::APMF_Param effParam   = op.param;
        RE::FormID           castProxy  = 0;
        RE::FormID           castTarget = 0;
        std::uint32_t        castFlags  = 0;
        std::uint64_t        expiresMs  = 0;
        // The clamped TTL actually granted below -- stored on the Claim so
        // ApplyRepoint can renew the window with this claim's OWN length (see
        // Claim::ttlMs in core/ControlMap.h). 0 for every non-cast claim.
        std::uint32_t        castTtlMs  = 0;
        RE::ActorHandle      castTargetHandle{};
        // The form the CLIENT named, when the claim ends up storing a DIFFERENT one
        // (kCastFlag_FromPackage only -- see Claim::castSrcForm in core/ControlMap.h).
        // 0 for every other claim, whose stored form IS the client's own.
        RE::FormID           castSrcForm = 0;
        // feat/per-hand-cast-claims: handles of conflicting kIntent_Cast claims a
        // WINNING dual-vs-single-hand collision (below) must evict once the new
        // claim is actually inserted -- populated in the early conflict check,
        // consumed right before `cc->claims.push_back(newClaim)` (Phase 2), so the
        // eviction happens with the new claim's OWN final castProxy already known
        // (needed to tell whether an evicted claim's proxy is about to be reused in
        // place rather than actually freed -- core/CastProxy.cpp's pool is one
        // shared slot PER OWNER, not per hand).
        std::vector<Handle> toEvict;
        if (op.intent == APMF_API::kIntent_Cast) {
            // Flags: the kCast op carries them in castFlags; a degenerate
            // RequestEx(kIntent_Cast) carries them in param.ival (kept in parity).
            castFlags  = (op.kind == PendingOp::Kind::kCast) ? op.castFlags
                                                             : static_cast<std::uint32_t>(op.param.ival);
            castProxy  = op.castProxy;
            castTarget = op.castTarget;
            RE::FormID spell = op.param.form;

            // ---- DENY-ONLY HAND CLAIM (kCastFlag_DenyHandOnly, 2026-09-06) ----
            // The client claimed this hand purely to DENY it: it drives nothing
            // there and the engine is never asked to arm anything. DECLARE ->
            // ENFORCE (CLAUDE.md principle 4): the driven form, the proxy and the
            // target are forced to NONE here, at the one place a claim is built, so
            // "drives nothing" is a property of the stored claim rather than a rule
            // every downstream reader has to remember. That single fact is what
            // makes every cast SEAT skip this claim for free -- they all key on a
            // driven form (TryGetCastSeatClaimForForm ignores a 0 driven form) or on
            // a resolved target handle (which stays invalid because castTarget is 0),
            // so none of them can ever seat, drive or classify for it. No
            // delivery-flip proxy is minted either (the mint below is gated on a
            // resolved target). The DENY half is the whole point and is untouched:
            // the claim still occupies its hand, so core/Allowance.cpp's per-hand
            // reads deny every other spell/staff competing for that hand.
            const bool denyHandOnly = (castFlags & APMF_API::kCastFlag_DenyHandOnly) != 0;
            if (denyHandOnly) {
                if (spell != 0 || castProxy != 0 || castTarget != 0) {
                    spdlog::info("[ch.8b] 0x{} deny-only hand claim (h={}) also carried spell 0x{} / proxy "
                                 "0x{} / target 0x{} -- all IGNORED. A kCastFlag_DenyHandOnly claim drives "
                                 "NOTHING by definition; it only denies the hand to everything else.",
                                 apmf::log::Hex(op.actor), op.handle, apmf::log::Hex(spell),
                                 apmf::log::Hex(castProxy), apmf::log::Hex(castTarget));
                }
                spell      = 0;
                castProxy  = 0;
                castTarget = 0;
            } else if (castFlags & APMF_API::kCastFlag_FromPackage) {
                RE::FormID outSpell = 0, outTarget = 0;
                if (!apmf::castcompose::ExtractFromPackage(op.param.form, outSpell, outTarget)) {
                    spdlog::warn("[ch.8b] cast-from-package: no spell input on 0x{} -- REFUSED "
                                 "(op dropped; package never run/offered/evaluated). The client "
                                 "should pass the spell directly.", apmf::log::Hex(op.param.form));
                    return false;   // handle was never registered in m_index -> never dangling
                }
                spell = outSpell;
                if (castTarget == 0) castTarget = outTarget;   // client's own target wins if it named one
                // Remember the PACKAGE the client named. The claim is about to store
                // the EXTRACTED spell instead, a FormID the client never sees, so
                // without this a plain Repoint heartbeat -- which necessarily carries
                // the package -- reads as a spell SWAP in ApplyRepoint and is refused
                // loudly on every call (F5-2). See Claim::castSrcForm.
                castSrcForm = op.param.form;
            }
            effParam.form = spell;
            std::uint32_t ttl = (op.kind == PendingOp::Kind::kCast) ? op.ttlMs : 0;
            if (ttl == 0) ttl = APMF_API::kCastDefaultTtlMs;
            if (ttl > APMF_API::kCastMaxTtlMs) ttl = APMF_API::kCastMaxTtlMs;
            expiresMs = apmf::clock::MonotonicMs() + ttl;
            castTtlMs = ttl;   // the GRANTED (already-clamped) length ApplyRepoint renews with

            // ---- DUAL vs SINGLE-HAND MUTUAL EXCLUSIVITY (feat/per-hand-cast-claims)
            // Two concurrent kIntent_Cast claims on one actor now coexist when they
            // occupy DIFFERENT hands (kCastFlag_LeftHand vs the right-hand default) --
            // see ApplyRequest's tail and the *ForHand/*ForForm reads below. A
            // kCastFlag_DualCast claim occupies BOTH hands at once, so it can NEVER
            // coexist with a left- or right-hand single-hand claim: arbitrate the
            // collision by basis (same rule as everywhere else), but the LOSER is
            // REFUSED outright here -- never left in the claims list as a non-owning
            // claim waiting for the winner to release (that would be the same
            // silent-coexistence-of-incompatible-claims bug class this whole pass
            // exists to close). Same-shape claims (two lefts, two rights, or two
            // duals) are NOT touched by this block -- they keep the normal
            // non-exclusive basis arbitration every other channel already uses.
            // Checked BEFORE the target-resolve/proxy-mint work below so a claim
            // that is about to be refused never wastes a delivery-flip mint.
            //
            // DENY-ONLY CLAIMS ARE OUTSIDE THIS COLLISION, IN BOTH DIRECTIONS
            // (2026-09-06). The exclusivity above is about two claims wanting the
            // engine to ARM different things in overlapping hands. A
            // kCastFlag_DenyHandOnly claim asks the engine to arm nothing at all --
            // its spell, proxy and target are 0 by construction -- so it cannot
            // collide with anything, and treating it as a single-hand claim produced
            // two real breakages:
            //   * a higher-basis DUAL claim marked the standing floor `toEvict`, and
            //     Phase 2 erased it -- so the floored hand REOPENED for a full client
            //     tick after every dual cast, which is precisely the recurring
            //     unclaimed-gap class (RC2) the floor exists to remove;
            //   * a floor requested while a dual claim stands was REFUSED at
            //     `op.basis <= bestConflictBasis`, and (mirror image) a standing floor
            //     REFUSED an incoming dual claim outright at the same test -- so with a
            //     client using one uniform basis for every claim, a follower with a
            //     floor standing could never dual-cast at all.
            // Coexistence is harmless because the per-hand readers already rank the
            // two correctly with the shared comparator: on the hand the dual claim
            // occupies it outranks (or, at an equal basis, displaces) the floor, and
            // when it ends the floor is still standing underneath with no gap.
            {
                const bool newIsDual = IsDualCastFlags(castFlags);
                if (auto npcIt = map.find(op.actor); !denyHandOnly && npcIt != map.end()) {
                    for (const auto& cc2 : npcIt->second.channels) {
                        if (cc2.channel != channel) continue;
                        float bestConflictBasis = 0.0f;
                        bool  anyConflict       = false;
                        for (const auto& cl : cc2.claims) {
                            if (cl.castFlags & APMF_API::kCastFlag_DenyHandOnly) continue;   // drives nothing -- cannot collide
                            if (IsDualCastFlags(cl.castFlags) == newIsDual) continue;   // same shape -- not this collision
                            if (!anyConflict || cl.basis > bestConflictBasis) bestConflictBasis = cl.basis;
                            anyConflict = true;
                        }
                        if (!anyConflict) break;
                        if (op.basis <= bestConflictBasis) {
                            spdlog::warn("[ch.8b] 0x{} {}-cast claim (h={}, basis {:.1f}) REFUSED -- conflicts "
                                         "with an existing {}-cast claim (basis {:.1f}); a dual-cast and a "
                                         "single-hand claim cannot coexist on one actor (arbitrated by basis, "
                                         "the loser is refused outright, never silently merged/replaced).",
                                         apmf::log::Hex(op.actor), newIsDual ? "dual" : "single-hand", op.handle,
                                         op.basis, newIsDual ? "single-hand" : "dual", bestConflictBasis);
                            return false;   // handle was never registered in m_index -> never dangling
                        }
                        // The new claim wins the collision -- remember every conflicting
                        // claim's HANDLE (not index: nothing reorders `cc2.claims` between
                        // now and Phase 2, but a handle survives that better than an index
                        // would if this logic ever moves) for eviction in Phase 2, once the
                        // new claim's own castProxy is finally resolved.
                        for (const auto& cl : cc2.claims) {
                            if (cl.castFlags & APMF_API::kCastFlag_DenyHandOnly) continue;   // never evict a floor
                            if (IsDualCastFlags(cl.castFlags) != newIsDual) toEvict.push_back(cl.handle);
                        }
                        break;
                    }
                }
            }

            // ---- DENY-ONLY BASIS: THE TIE IS NOW ENFORCED; A STRICT INVERSION
            // IS STILL THE CLIENT'S CALL, AND IS STILL SAID OUT LOUD (2026-09-06) ----
            // The EQUAL-basis case is no longer a contract the client has to remember:
            // ControlMap.h::BetterClaim -- the ONE comparator every winner-selection
            // in this class now uses -- makes a deny-only claim LOSE to a driving one
            // at an equal basis. That is enforcement of a fact the client declared
            // (the flag says this claim drives nothing), not a rank APMF invented, and
            // it is what makes the flag usable by a client that issues every claim at
            // one uniform basis: a floor issued FIRST no longer denies the gambit that
            // follows it.
            //
            // What is NOT decided for the client is a STRICT inversion -- a floor
            // requested at a basis genuinely ABOVE a driving claim. There the client
            // really did say "this hand stays shut, outranking that cast", so the
            // floor wins and APMF re-ranks nothing. But that is also EXACTLY the
            // failure that is hardest to read from a log (a follower with two valid
            // gambits casting one and then standing still, because its own floor
            // outranks its own cast), so it is detected here, at claim time, and said
            // loudly (CLAUDE.md principles 5 and 7 -- observe it, never mask it). The
            // outcome is DEFINED in both cases; this warn reports a defined outcome
            // the client is unlikely to have wanted, it does not report an undefined
            // one.
            //
            // The check is deliberately NOT hand-scoped: the actor-wide seat reads
            // (TryGetCastSeatClaim -- core/EquipGate.cpp's rescore-dirty trigger,
            // core/CastClassify.cpp's diagnostic) pick the actor-wide best claim, so a
            // floor that outranks a DRIVING claim on the OTHER hand masks those too.
            {
                const auto nowMs = apmf::clock::MonotonicMs();
                if (auto npcIt = map.find(op.actor); npcIt != map.end()) {
                    for (const auto& cc2 : npcIt->second.channels) {
                        if (cc2.channel != channel) continue;
                        for (const auto& cl : cc2.claims) {
                            if (cl.expiresMs != 0 && nowMs >= cl.expiresMs) continue;   // already gone
                            const bool clDenyOnly = (cl.castFlags & APMF_API::kCastFlag_DenyHandOnly) != 0;
                            if (clDenyOnly == denyHandOnly) continue;   // only a floor-vs-driver pair matters
                            const float floorBasis  = denyHandOnly ? op.basis : cl.basis;
                            const float driveBasis  = denyHandOnly ? cl.basis : op.basis;
                            // <= : an EQUAL basis is now DEFINED in the driver's favour
                            // by ControlMap.h::BetterClaim, so it is no longer a
                            // violation and no longer warned about. Only a floor
                            // STRICTLY above a driving claim actually self-denies.
                            if (floorBasis <= driveBasis) continue;
                            spdlog::warn("[ch.8b] 0x{} DENY-ONLY FLOOR OUTRANKS A LIVE CAST: the deny-only "
                                         "hand claim (basis {:.1f}) is STRICTLY ABOVE a live DRIVING cast "
                                         "claim (basis {:.1f}) on this actor, so the floor wins the "
                                         "arbitration and the client is about to deny its OWN cast (the "
                                         "follower will hold the hand and cast nothing). That outcome is "
                                         "DEFINED, not undefined -- APMF is doing exactly what the bases "
                                         "asked for -- but it is almost never what a client wants. Request "
                                         "the deny-only floor at a basis AT OR BELOW your real cast claims: "
                                         "at an EQUAL basis the driving claim now wins automatically. See "
                                         "kCastFlag_DenyHandOnly in APMF_API.h. Nothing is re-ranked or "
                                         "refused here; the claim stands as asked.",
                                         apmf::log::Hex(op.actor), floorBasis, driveBasis);
                        }
                        break;
                    }
                }
            }

            // ---- Resolve the target ONCE, here, on the WRITER thread ------------
            // The engine seats (core/CastSeats.cpp) run on the COMBAT thread, where a
            // `TESForm::LookupByID` would take the engine's own forms-map lock. This is
            // the seat where form lookups are already legal, so the FormID is resolved
            // to a native ActorHandle now and the seats pay only a handle-table read.
            // An unresolvable/non-actor target leaves the handle invalid -- the seats
            // then behave exactly as they do for a dead target (0x0A hands back nothing
            // to redirect, 0x07 stops the channel), never a guess.
            if (castTarget != 0) {
                if (auto* tgt = RE::TESForm::LookupByID<RE::Actor>(castTarget)) {
                    castTargetHandle = tgt->GetHandle();
                } else {
                    spdlog::warn("[ch.8b] cast claim on 0x{}: target 0x{} is not a loadable Actor -- "
                                 "the seats will not redirect (the AI keeps its own target).",
                                 apmf::log::Hex(op.actor), apmf::log::Hex(castTarget));
                }
            }

            // ---- kSelf DELIVERY FLIP: mint + transient-teach the proxy ----------
            // Disassembly-CERTAIN (FindTargets 0x5bc160 @0x5bc98a): a kSelf-delivery
            // spell ALWAYS lands on the caster's own reference -- the Self branch never
            // reads `desiredTarget`, so seat 0x0A cannot aim it at an ally. The only
            // honest answer is a delivery-flipped COPY the AI selects instead. Minted
            // HERE, before the claim is published, so a seat can never see a claim
            // naming a proxy the actor does not yet know (core/CastProxy.h). A client
            // that fabricated its OWN proxy (req.proxy != 0) is left alone.
            if (castProxy == 0 && castTargetHandle && castTarget != op.actor) {
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spell)) {
                    if (sp->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
                        // Only the PROSPECTIVE WINNER may mint. The pool is keyed by
                        // OWNER (one slot per actor -- a dual cast shares one form), so a
                        // second, LOSING cast claim on the same actor would otherwise
                        // re-target the winner's proxy at ITS spell and the AI would cast
                        // the wrong thing. Peek at the already-published/working map
                        // WITHOUT creating an entry (map.find, never operator[]) --
                        // deliberate: this whole block runs before `map[op.actor]` for
                        // exactly that reason. A loser simply gets no proxy, which is
                        // correct: it is not driving anything.
                        float bestBasis = 0.0f;
                        bool  haveBest  = false;
                        if (auto npcIt = map.find(op.actor); npcIt != map.end()) {
                            for (const auto& cc2 : npcIt->second.channels) {
                                if (cc2.channel != channel) continue;
                                for (const auto& cl : cc2.claims) {
                                    if (!haveBest || cl.basis > bestBasis) { bestBasis = cl.basis; haveBest = true; }
                                }
                            }
                        }
                        if (haveBest && op.basis < bestBasis) {
                            spdlog::info("[ch.8b] 0x{} cast claim (basis {:.1f}) loses to the incumbent "
                                         "(basis {:.1f}) -- no delivery-flip proxy minted for it (the pool is "
                                         "per-actor; re-targeting it would corrupt the winner's cast).",
                                         apmf::log::Hex(op.actor), op.basis, bestBasis);
                        } else {
                            castProxy = apmf::castproxy::Acquire(op.actor, sp);
                            if (castProxy == 0)
                                spdlog::warn("[ch.8b] 0x{} claimed a kSelf spell 0x{} at another actor but "
                                             "no delivery-flip proxy could be minted -- the seats will NOT "
                                             "force the original form (it would land on the caster). The "
                                             "claim still stands as a plain deny.",
                                             apmf::log::Hex(op.actor), apmf::log::Hex(spell));
                        }
                    }
                }
            }
        }

        auto&      npc     = map[op.actor];
        const bool freshNpc = npc.channels.empty();
        if (freshNpc) {
            npc.handle       = actor->GetHandle();
            auto* pkg        = actor->GetCurrentPackage();
            npc.pkgAtCapture = pkg ? pkg->GetFormID() : 0;
        }

        // Find (or create) the per-channel control entry for this channel.
        ChannelCtl* cc = nullptr;
        for (auto& c : npc.channels) {
            if (c.channel == channel) { cc = &c; break; }
        }
        const bool freshChannel = (cc == nullptr);
        if (freshChannel) {
            npc.channels.push_back(ChannelCtl{ channel, {} });
            cc = &npc.channels.back();
        }

        // feat/per-hand-cast-claims Phase 2: evict every claim the early
        // dual-vs-single-hand collision check (above) found this WINNING claim
        // conflicts with. Done here (not in Phase 1) so `castProxy` for the new
        // claim is already fully resolved -- an evicted claim's own delivery-flip
        // proxy is freed ONLY when the new claim is not simply about to reuse the
        // exact same slot (core/CastProxy.cpp's pool is one shared slot PER OWNER;
        // `Acquire` above already re-targeted it in place when the new claim is
        // ALSO a kSelf-delivery cast on the same actor, in which case freeing it
        // here would tear down a proxy the new claim is actively using).
        for (const Handle evictHandle : toEvict) {
            for (auto it = cc->claims.begin(); it != cc->claims.end(); ++it) {
                if (it->handle != evictHandle) continue;
                const RE::FormID evictedProxy = it->castProxy;
                m_index.erase(it->handle);
                cc->claims.erase(it);
                spdlog::warn("[ch.8b] 0x{} cast claim (h={}) EVICTED -- refused by a higher-basis "
                             "{}-cast claim (h={}, basis {:.1f}); dual-cast and single-hand claims "
                             "cannot coexist on one actor.",
                             apmf::log::Hex(op.actor), evictHandle, IsDualCastFlags(castFlags) ? "dual" : "single-hand",
                             op.handle, op.basis);
                if (evictedProxy != 0 && evictedProxy != castProxy) {
                    apmf::mainthread::Post([actorId = op.actor, evictedProxy] {
                        // One hop past this Drain's Publish (INVARIANTS #20's release
                        // ordering) -- by now every seat already reads "claim gone" for
                        // the evicted handle. `castproxy::Free` is a plain "clear this
                        // owner's slot" call with no source-form check of its own, so
                        // this re-checks the slot still holds exactly the form being
                        // torn down before freeing it -- never un-teaching a DIFFERENT
                        // proxy some later claim on the same owner may have minted in
                        // the meantime.
                        if (apmf::castproxy::FormForOwner(actorId) == evictedProxy)
                            apmf::castproxy::Free(actorId);
                    });
                }
                break;
            }
        }

        // Build the claim BEFORE the incumbent scan (2026-09-06): the shared
        // BetterClaim comparator ranks two CLAIMS, not two bare basis floats, so the
        // new claim's own castFlags must exist by the time the comparison runs. Pure
        // reordering of already-computed values -- everything assigned here was
        // finalized in Phase 1 above (castProxy last, by the eviction block).
        Claim newClaim{ op.handle, op.basis, effParam };
        newClaim.castProxy        = castProxy;
        newClaim.castTarget       = castTarget;
        newClaim.castFlags        = castFlags;
        newClaim.expiresMs        = expiresMs;
        newClaim.ttlMs            = castTtlMs;
        newClaim.castTargetHandle = castTargetHandle;
        newClaim.castSrcForm      = castSrcForm;

        // Before adding this claim, find the incumbent OWNER (if any) -- seeded from
        // the first EXISTING claim, never a 0.0 floor, so a negative-basis incumbent
        // arbitrates correctly (a 0.0 floor would let a claim below the true max but
        // above 0 wrongly "win", and mislog owner basis=0.0). ONE comparator with
        // ApplyRelease's ownerOf, ApplyRepoint and the four cast reads
        // (ControlMap.h::BetterClaim): higher basis wins, and at an equal basis a
        // deny-only claim loses to a driving one. `newOwner` is resolved HERE, while
        // `oldBestClaim` still points into the un-reallocated vector -- push_back
        // below may move the storage, so nothing may dereference it afterwards.
        const Claim* oldBestClaim = nullptr;
        for (auto& c : cc->claims) {
            if (!oldBestClaim || BetterClaim(c, *oldBestClaim)) oldBestClaim = &c;
        }
        const float oldBest  = oldBestClaim ? oldBestClaim->basis : 0.0f;
        const bool  newOwner = !oldBestClaim || BetterClaim(newClaim, *oldBestClaim);

        cc->claims.push_back(newClaim);
        m_index[op.handle] = { op.actor, channel };

        if (freshChannel) {
            channel->Engage(op.actor, actor, effParam);   // 0 -> 1: apply the source-block once
            spdlog::info("[ctl] 0x{} '{}' + ch.{} {} ENGAGED (h={}, basis={:.1f}, form=0x{}). NPCs controlled: {}.",
                         apmf::log::Hex(op.actor), actor->GetName() ? actor->GetName() : "?",
                         channel->ChannelNo(), channel->Name(), op.handle, op.basis,
                         apmf::log::Hex(effParam.form), map.size());
        } else {
            // Additional claim on an already-engaged channel: arbitrate with the ONE
            // comparator (higher basis wins; at an equal basis a deny-only claim
            // loses to a driving one, otherwise the incumbent keeps ownership). On a
            // real owner change, hand a parameterized channel the new winner's
            // payload (parameterless channels no-op OnOwnerChanged; the claim just
            // refcounts the engagement). `newOwner` was resolved above, before
            // push_back could invalidate the incumbent pointer.
            if (newOwner) channel->OnOwnerChanged(op.actor, actor, effParam);
            spdlog::info("[ctl] 0x{} + ch.{} {} additional claim (h={}, basis={:.1f}); {} claim(s), "
                         "owner basis={:.1f}{}.", apmf::log::Hex(op.actor), channel->ChannelNo(), channel->Name(),
                         op.handle, op.basis, cc->claims.size(), newOwner ? op.basis : oldBest,
                         newOwner ? " (NEW OWNER)" : "");
        }
        return true;   // the claim was always pushed onto `map` above, regardless of path
    }

    bool ControlMap::ApplyRelease(Handle handle, MapType& map) {
        auto idxIt = m_index.find(handle);
        if (idxIt == m_index.end()) return false;   // unknown/stale/already-released
        const RE::FormID formID  = idxIt->second.first;
        Channel*         channel = idxIt->second.second;
        m_index.erase(idxIt);

        auto npcIt = map.find(formID);
        if (npcIt == map.end()) return false;
        auto& npc = npcIt->second;
        bool changed = false;

        for (auto ccIt = npc.channels.begin(); ccIt != npc.channels.end(); ++ccIt) {
            if (ccIt->channel != channel) continue;
            changed = true;   // this claim is in `map` and is about to be removed from it
            auto& claims = ccIt->claims;

            // Identify the owner (highest basis; tie -> earliest) BEFORE removal, so
            // a parameterized channel can be re-pointed at the new winner if the
            // owner is the claim leaving.
            auto ownerOf = [](std::vector<Claim>& cs) -> Claim* {
                Claim* best = cs.empty() ? nullptr : &cs.front();
                // ONE comparator, shared with ApplyRequest/ApplyRepoint and the four
                // cast reads (ControlMap.h::BetterClaim): higher basis wins; at an
                // equal basis a deny-only claim loses to a driving one; otherwise the
                // earliest keeps it.
                for (auto& c : cs) if (best && BetterClaim(c, *best)) best = &c;
                return best;
            };
            const Handle oldOwner = [&] { Claim* o = ownerOf(claims); return o ? o->handle : APMF_API::kInvalidHandle; }();

            // feat/per-hand-cast-claims: capture THIS claim's own delivery-flip
            // proxy (if any) before it is erased below -- see the teardown after
            // the if/else. Non-cast claims (and most cast claims) never set this,
            // so it stays 0 for everything but a released kSelf-delivery cast claim.
            RE::FormID departingProxy = 0;
            for (auto it = claims.begin(); it != claims.end(); ++it) {
                if (it->handle != handle) continue;
                departingProxy = it->castProxy;
                claims.erase(it);
                break;
            }
            if (claims.empty()) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);   // may be null
                channel->Release(formID, actor);   // 1 -> 0: restore the AI
                spdlog::info("[ctl] 0x{} - ch.{} {} RELEASED (h={}).",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(), handle);
                npc.channels.erase(ccIt);
                // channel->Release() (CastComposeChannel::Release, channels/CastCompose.cpp)
                // already defers castproxy::Free(formID) for exactly this "last claim on
                // the channel" case -- nothing more to do here.
            } else {
                // Claims remain: if the OWNER just left, the new winner takes over --
                // re-point a parameterized channel at its payload (no restore capture).
                if (handle == oldOwner) {
                    Claim* now = ownerOf(claims);
                    if (now) {
                        auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);   // may be null
                        channel->OnOwnerChanged(formID, actor, now->param);
                    }
                }
                spdlog::info("[ctl] 0x{} - ch.{} {} claim dropped (h={}); {} claim(s) remain.",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(), handle, claims.size());

                // feat/per-hand-cast-claims: a SECOND live claim on this channel
                // (the other hand) means the channel-level Release() above will NOT
                // fire for this departure -- with two concurrent kIntent_Cast claims
                // now possible, "the channel disengaged" and "this ONE claim's own
                // resources are done" are no longer the same event. A released claim's
                // own delivery-flip proxy (core/CastProxy.cpp, one shared slot PER
                // OWNER) must be torn down on ITS OWN release, independent of whatever
                // the OTHER hand's claim is doing -- releasing one hand must never
                // disturb the other, and must never LEAK the released hand's proxy
                // either (never mask a failure by just leaving it taught forever).
                if (departingProxy != 0) {
                    apmf::mainthread::Post([formID, departingProxy] {
                        // Re-check the slot still holds exactly the form being torn
                        // down (see the identical guard in ApplyRequest's eviction
                        // path) -- never un-teach a DIFFERENT proxy a later claim on
                        // the same owner minted in the meantime.
                        if (apmf::castproxy::FormForOwner(formID) == departingProxy)
                            apmf::castproxy::Free(formID);
                    });
                }
            }
            break;
        }
        if (npc.channels.empty()) map.erase(npcIt);
        return changed;
    }

    bool ControlMap::ApplyRepoint(Handle handle, const APMF_API::APMF_Param& param, MapType& map) {
        auto idxIt = m_index.find(handle);
        if (idxIt == m_index.end()) return false;   // unknown/stale
        const RE::FormID formID  = idxIt->second.first;
        Channel*         channel = idxIt->second.second;

        auto npcIt = map.find(formID);
        if (npcIt == map.end()) return false;
        auto& npc = npcIt->second;

        for (auto& cc : npc.channels) {
            if (cc.channel != channel) continue;
            auto& claims = cc.claims;
            if (claims.empty()) return false;

            // Find this claim, and the current owner. ONE comparator, shared with
            // ApplyRequest/ApplyRelease and the four cast reads
            // (ControlMap.h::BetterClaim): higher basis wins; at an equal basis a
            // deny-only claim loses to a driving one; otherwise the earliest keeps it.
            Claim* self = nullptr;
            Claim* best = &claims.front();
            for (auto& c : claims) {
                if (c.handle == handle)     self = &c;
                if (BetterClaim(c, *best))  best = &c;
            }
            if (!self) return false;   // handle not in this channel (should not happen)

            // ---- WHAT A REPOINT MAY AND MAY NOT CHANGE (2026-09-06) --------------
            // `self->param = param` used to run UNCONDITIONALLY, which made a plain
            // heartbeat -- the very thing the TTL-renewal below invites a client to
            // do -- able to rewrite the ONE field that decides what a cast claim
            // drives. Two distinct breakages, both closed here:
            //
            //  (1) A DENY-ONLY claim (kCastFlag_DenyHandOnly) has its spell, proxy
            //      and target FORCED TO NONE in ApplyRequest, and every seat skips it
            //      for free precisely because its driven form is 0. A Repoint carrying
            //      a form would put a driven form BACK onto a claim the client
            //      declared closed: core/EquipGate.cpp's 0x0F would then compute
            //      `driven = handSeat.spell` and ADMIT that spell on the floored hand,
            //      core/AiCastSeats.cpp's steer would bias it, and
            //      TryGetCastSeatClaimForForm would match it -- while
            //      core/CastGate.cpp's 0x0A still denies it (the deny-only branch in
            //      Allowance.cpp keys on the FLAG, not the form), so the AI equips,
            //      is refused with kMultipleCast, re-deliberates and equips again: a
            //      churn loop on a hand that was supposed to be silent. `form` is
            //      therefore pinned to 0 for a deny-only claim, at the same one place
            //      ApplyRequest pins it -- DECLARE -> ENFORCE (principle 4).
            //
            //  (2) A DRIVING cast claim's `param.form` is only HALF of a cast claim:
            //      castProxy (a delivery-flip form minted for THAT spell), castTarget,
            //      castTargetHandle and castFlags were all resolved together against
            //      it in ApplyRequest, and Repoint runs none of that. Letting a
            //      Repoint swap the form would leave the claim naming spell B while
            //      driving A's proxy at A's target -- a silently WRONG cast, and the
            //      hardest possible thing to read from a log. So a form change is
            //      REFUSED and said out loud (principle 7: never mask a failure); the
            //      client must Release + RequestCast to change what it casts. Every
            //      other param field still updates, and the TTL still renews, so a
            //      same-form heartbeat is completely unaffected.
            //
            //      ONE FORM THAT IS *NOT* A CHANGE (F5-2, 2026-09-07). On a
            //      kCastFlag_FromPackage claim the stored form is the spell APMF
            //      EXTRACTED from the client's package -- a FormID the client is never
            //      handed -- so the only thing a correct client can heartbeat with is
            //      the PACKAGE it requested with, and the test above flagged that as a
            //      swap on every single call. The claim now remembers the form the
            //      client named (Claim::castSrcForm) and treats it as the same-form
            //      shape: no refusal, no warning, nothing changed. That is not a
            //      loosening of the rule -- the extracted spell, its proxy, its target
            //      and its flags all stay exactly as RequestCast resolved them, which
            //      is precisely what the rule protects.
            //
            // Only kIntent_Cast claims are affected: for every other channel `param`
            // is written exactly as before.
            const bool isCastClaim =
                (channel == Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast));
            const bool denyOnly =
                isCastClaim && (self->castFlags & APMF_API::kCastFlag_DenyHandOnly) != 0;

            APMF_API::APMF_Param effParam = param;
            if (denyOnly) {
                if (param.form != 0) {
                    spdlog::warn("[ch.8b] 0x{} Repoint on a DENY-ONLY hand claim (h={}) carried form 0x{} "
                                 "-- IGNORED. A kCastFlag_DenyHandOnly claim drives NOTHING by definition; "
                                 "its driven form stays 0 for its whole life. Release it and RequestCast "
                                 "if you want that hand to actually cast something.",
                                 apmf::log::Hex(formID), handle, apmf::log::Hex(param.form));
                }
                effParam.form = 0;
            } else if (isCastClaim && self->castSrcForm != 0 && param.form == self->castSrcForm) {
                // A kCastFlag_FromPackage claim's HEARTBEAT (F5-2, 2026-09-07). The
                // client named a PACKAGE; ApplyRequest extracted the spell out of it
                // and stored THAT as param.form, a FormID the client is never told.
                // So the only form a correct client can heartbeat with is the package
                // it requested with -- and comparing that against the extracted spell
                // made every heartbeat look like a spell swap and print the REFUSED
                // warning below. Nothing is being changed here, so nothing is refused
                // and nothing is warned about: the claim keeps the spell it extracted
                // (the assignment is what makes that explicit rather than incidental)
                // and the TTL renews below, which is the whole point of the call.
                // A genuinely different form -- another package, or a bare spell --
                // still falls through to the refusal below, because it still means
                // proxy/target/flags resolved against something else.
                effParam.form = self->param.form;
            } else if (isCastClaim && param.form != self->param.form) {
                spdlog::warn("[ch.8b] 0x{} Repoint on a live cast claim (h={}) tried to change its spell "
                             "0x{} -> 0x{} -- REFUSED (the claim keeps 0x{}). A cast claim's proxy, target "
                             "and flags were all resolved against its ORIGINAL spell in RequestCast and a "
                             "Repoint re-runs none of that, so honouring this would drive the old spell's "
                             "proxy at the old target under a new name. Release this handle and RequestCast "
                             "the new spell. (Repoint is for HEARTBEATING the TTL and updating the non-form "
                             "param fields -- see Repoint in APMF_API.h.)",
                             apmf::log::Hex(formID), handle, apmf::log::Hex(self->param.form),
                             apmf::log::Hex(param.form), apmf::log::Hex(self->param.form));
                effParam.form = self->param.form;
            }

            self->param = effParam;   // update the stored param regardless of ownership

            // ---- TTL RENEWAL (2026-09-06 diagnosis RC2, CLAUDE.md principle 9) ----
            // A kIntent_Cast claim (the ONLY claim kind that carries a TTL) used to
            // die exactly ttlMs after RequestCast with nothing able to renew it, so a
            // client holding one cast for longer than that went UNCLAIMED for the
            // whole gap between the expiry and its next re-request -- measured at
            // 0.26-0.61 s every 6 s in the field, with a foreign spell observed
            // equipping and charging 110 ms into one of those gaps. A Repoint is the
            // client saying "I still want this window", so it now MOVES the deadline
            // to now + this claim's OWN granted (already-clamped) length. Done
            // regardless of ownership, for exactly the same reason `param` above is:
            // the stored claim is the thing every reader consults, owner or not.
            // NOT a standing hold and NOT a mask (principle 7): the claim still dies
            // ttlMs after the client's LAST call, so a crashed or forgetful client
            // loses it on the same schedule as before -- design.md 5a intact. Only a
            // client that keeps ASKING keeps the window.
            //
            // An ALREADY-LAPSED claim is NOT resurrected. Drain applies queued ops
            // BEFORE its TTL auto-release pass, so without the liveness test below a
            // Repoint arriving after the deadline (a client that stalled, or frames
            // that never ran during a load) would move a dead claim's deadline
            // forward and it would survive that sweep -- after every reader had
            // already been answering "gone" (the *ForHand/*ForForm reads treat an
            // elapsed deadline as gone without waiting for the sweep). Expiry stays
            // final: renewal EXTENDS a live window, it never reopens a closed one.
            const auto          nowMs     = apmf::clock::MonotonicMs();
            const std::uint32_t renewedMs =
                (self->expiresMs != 0 && self->ttlMs != 0 && nowMs < self->expiresMs) ? self->ttlMs : 0;
            if (renewedMs != 0) self->expiresMs = nowMs + renewedMs;

            if (best == self) {
                // This claim OWNS the channel -> re-point it in place (same handle,
                // no release/re-engage). A parameterized channel switches its held
                // target/spell; a parameterless channel no-ops OnOwnerChanged.
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);   // may be null
                // `effParam`, not the raw `param`: the channel must be handed exactly
                // what the claim now STORES, or a refused/pinned form would still
                // reach the channel and the log would report a form the claim does
                // not hold.
                channel->OnOwnerChanged(formID, actor, effParam);
                spdlog::info("[ctl] 0x{} ~ ch.{} {} REPOINT (h={}, form=0x{}, TTL renewed +{} ms "
                             "[0 = no TTL on this claim, or its window had already lapsed]).",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(),
                             handle, apmf::log::Hex(effParam.form), renewedMs);
            }
            return true;   // self->param was written above regardless of ownership
        }
        return false;   // handle's channel not found on this NPC (should not happen)
    }

    // ABI v4 (ch.8's SetSpellAllowList). Writer thread only -- looks the claim up
    // via m_index exactly as ApplyRepoint does, then writes altForms/altCount on
    // the matching Claim. Restricted to kIntent_SelectSpell: a handle whose claim
    // lives on any OTHER channel is a silent no-op (the allow-set concept only
    // means something for cast-select). Updates the STORED claim regardless of
    // whether it currently OWNS the channel -- same non-owning semantics as
    // Repoint (§4.3): no engine write to make either way (allow-set is read
    // straight off the stored claim by Allowance::Allowed, never pushed to the
    // engine), so unlike Repoint's OnOwnerChanged call there is nothing to fire
    // even when this claim IS the owner -- the widening is entirely inside the
    // read side.
    bool ControlMap::ApplySetSpellAllowList(Handle handle, const RE::FormID* forms, std::uint32_t count,
                                            MapType& map) {
        auto idxIt = m_index.find(handle);
        if (idxIt == m_index.end()) return false;   // unknown/stale

        auto* expected = Registry::Get().ChannelForIntent(APMF_API::kIntent_SelectSpell);
        if (!expected || idxIt->second.second != expected) return false;   // not a SelectSpell claim

        const RE::FormID formID  = idxIt->second.first;
        Channel*         channel = idxIt->second.second;

        auto npcIt = map.find(formID);
        if (npcIt == map.end()) return false;
        auto& npc = npcIt->second;

        for (auto& cc : npc.channels) {
            if (cc.channel != channel) continue;
            for (auto& c : cc.claims) {
                if (c.handle != handle) continue;
                // count is already clamped to kMaxSpellAllowList by
                // EnqueueSetSpellAllowList -- defensive re-clamp here anyway so this
                // function is safe to call with an unclamped count from any future
                // caller (never an unbounded write into the fixed altForms array).
                c.altCount = (count < APMF_API::kMaxSpellAllowList) ? count : APMF_API::kMaxSpellAllowList;
                for (std::uint32_t i = 0; i < c.altCount; ++i) c.altForms[i] = forms ? forms[i] : 0;
                spdlog::info("[ctl] 0x{} ~ ch.{} {} SET-ALLOW-LIST (h={}, {} form(s)).",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(),
                             handle, c.altCount);
                return true;
            }
            return false;   // handle not among this channel's claims (should not happen)
        }
        return false;   // handle's channel not found on this NPC (should not happen)
    }

    void ControlMap::OnActorUpdate(RE::Actor* actor) {
        // ANY thread (field-proven: the Character 0xAD seat is not single-threaded).
        // Relaxed pre-gate: near-zero cost while nothing is controlled -- no atomic
        // shared_ptr traffic at all until m_anyControlled goes non-zero.
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return;

        // Acquire-load a LOCAL shared_ptr copy of the published snapshot. This
        // freezes one generation for the rest of this call: even if Drain publishes
        // a newer one concurrently, `snap` keeps this generation alive (refcount) and
        // every read below sees a fully-built, self-consistent map -- no torn reads.
        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor->GetFormID());   // the single hash lookup
        if (it == snap->end()) return;               // uncontrolled NPC -> done

        const NpcCtl& npc = it->second;   // read-only view; see ControlMap.h for the obsTick exception
        if (!npc.handle.get()) return;             // unloaded; the Drain sweep reclaims it

        for (auto& cs : npc.channels) {
            if (cs.channel) cs.channel->Tick(it->first, actor);   // most channels no-op (clean block)
        }

        // The ONE reader-side mutation: obsTick is `mutable std::atomic`, so this is
        // safe through the const NpcCtl& above even though everything else in the
        // snapshot is read-only (ControlMap.h). Relaxed: a pure logging-cadence
        // counter with no other memory dependent on its ordering.
        if ((npc.obsTick.fetch_add(1, std::memory_order_relaxed) + 1) % kObsEvery != 0) return;
        auto*      pkg  = actor->GetCurrentPackage();
        RE::FormID id   = pkg ? pkg->GetFormID() : 0;
        const bool same = (id == npc.pkgAtCapture);
        std::string engaged;
        for (auto& cs : npc.channels) {
            if (!cs.channel) continue;
            if (!engaged.empty()) engaged += ',';
            engaged += cs.channel->Name();
        }
        spdlog::info("[obs] 0x{} pkg=0x{}({}) [{}] engaged=[{}]",
                     apmf::log::Hex(actor->GetFormID()), apmf::log::Hex(id), PkgTypeName(pkg),
                     same ? "PACKAGE STABLE" : "PACKAGE CHANGED!!", engaged.c_str());
    }

    bool ControlMap::TryGetOwningClaim(RE::FormID actor, Intent intent,
                                       APMF_API::APMF_Param& outParam) const {
        // ANY thread -- the allowance-template T2 thunks (core/Allowance.h,
        // CastGate.cpp/EquipGate.cpp) call this from combat-thread hooks. Same
        // RCU discipline as OnActorUpdate: relaxed pre-gate so an uncontrolled
        // world costs one relaxed load, then one acquire-load of a LOCAL snapshot
        // copy + one hash lookup on that frozen generation -- no torn reads, no
        // UAF even if Drain publishes a newer generation mid-call. Never touches
        // obsTick (a separate reader path from OnActorUpdate) or anything else.
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;   // uncontrolled actor -> the common case

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;   // unloaded; the Drain sweep reclaims it

        // Registry is immutable after load -- safe to consult from any thread.
        auto* channel = Registry::Get().ChannelForIntent(intent);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            // Winner = highest basis; tie -> earliest -- the SAME arbitration
            // rule as ApplyRequest's oldBest / ApplyRelease's ownerOf.
            // Deliberately NOT ControlMap.h::BetterClaim: this generic reader is
            // never called for kIntent_Cast (its callers pass MovementBlock,
            // CombatAction, SelectSpell, Equipment or OfferPackage), and every
            // non-cast claim has castFlags == 0, so the comparator's deny-only tie
            // rule could not change an answer here. Left as the plain strict compare
            // so the cast-specific rule lives only where cast claims do.
            const Claim* best = &cs.claims.front();
            for (const auto& c : cs.claims) {
                if (c.basis > best->basis) best = &c;
            }
            outParam = best->param;
            return true;
        }
        return false;   // this NPC is controlled, but not on this channel
    }

    bool ControlMap::TryGetOwningClaim(RE::FormID actor, Intent intent, APMF_API::APMF_Param& outParam,
                                       RE::FormID* outAllowSet, std::uint32_t& outAllowCount) const {
        // Same RCU discipline/pre-gate/lookup as the 3-arg overload above -- this
        // is a separate, independent snapshot load + lookup (not a wrapper around
        // the other overload) so both stay simple single-pass reads with no shared
        // mutable state between them.
        outAllowCount = 0;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(intent);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            const Claim* best = &cs.claims.front();
            for (const auto& c : cs.claims) {
                if (c.basis > best->basis) best = &c;
            }
            outParam = best->param;
            // Copy the allow-set OUT BY VALUE (bounded, <=kMaxSpellAllowList
            // uint32_t) rather than handing back a pointer/span into `best`
            // (snapshot-owned storage) -- see ControlMap.h's comment on this
            // overload: the copy has no RCU-lifetime coupling to `snap` once this
            // call returns, so it stays valid however long the caller keeps it,
            // even across a concurrent Drain/Publish.
            if (outAllowSet && best->altCount > 0) {
                outAllowCount = best->altCount;
                for (std::uint32_t i = 0; i < outAllowCount; ++i) outAllowSet[i] = best->altForms[i];
            }
            return true;
        }
        return false;   // this NPC is controlled, but not on this channel
    }

    bool ControlMap::TryGetCastClaim(RE::FormID actor, RE::FormID& outSpell, RE::FormID& outProxy,
                                     std::uint32_t* outFlags) const {
        // Same RCU reader discipline as TryGetOwningClaim (any thread): relaxed
        // pre-gate, one acquire-load of a LOCAL frozen snapshot, one hash lookup.
        // Reads the winning kIntent_Cast claim's spell (param.form) + castProxy --
        // the two FormIDs Allowance::AllowedCast permits while the claim stands --
        // and (optionally) its CastFlags (e.g. kCastFlag_LeftHand) for hand-scoped
        // callers (CastGate/EquipGate's per-hand deny, INVARIANTS #18).
        outSpell = 0;
        outProxy = 0;
        if (outFlags) *outFlags = 0;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            // Winner = the ONE comparator (ControlMap.h::BetterClaim), the same rule
            // every other winner-selection in this class uses: higher basis wins; at
            // an equal basis a deny-only claim loses to a driving one; otherwise the
            // earliest keeps it.
            //
            // ---- THE CANDIDATE SET IS THE *LIVE* CLAIMS (F5-3, 2026-09-07) --------
            // A claim whose TTL has elapsed is treated as ALREADY GONE, without
            // waiting for the Drain auto-release pass to publish it away. That rule is
            // older than this comment; what changed is WHERE it is applied. It used to
            // run as a single test on the winner AFTER the comparator had already
            // picked it, which silently made a lapsed claim able to speak for the
            // whole channel: with a high-basis LAPSED claim and a lower-basis LIVE one
            // standing together -- ordinary while a client re-requests at one uniform
            // basis, or holds a floor under a gambit -- the winner-only test returned
            // false and the LIVE claim's answer vanished with it, for up to a frame,
            // until the sweep ran. On this reader (Allowance::AllowedCast) that means
            // the live claim's DENY is dropped and the AI is free on a hand a live
            // claim is holding; on the seat readers it means the live claim is not
            // driven at all. Both are the "unclaimed gap" class the TTL floor exists
            // to remove, manufactured by the very test that enforces the TTL.
            //
            // So skip lapsed claims as CANDIDATES and let the comparator rank what is
            // actually live. Nothing else moves: with no lapsed claim in the list this
            // is byte-identical to the old code, an all-lapsed list still returns
            // false, and the comparator itself is untouched -- it never sees a claim
            // it would have been asked to rank before. The four winner-selecting cast
            // reads all do this the same way, or they would disagree about who owns a
            // hand (ControlMap.h::BetterClaim's "they MUST all agree").
            const auto nowMs = apmf::clock::MonotonicMs();
            const Claim* best = nullptr;
            for (const auto& c : cs.claims) {
                if (c.expiresMs != 0 && nowMs >= c.expiresMs) continue;   // lapsed -- already gone
                if (!best || BetterClaim(c, *best)) best = &c;
            }
            if (!best) return false;   // every claim here has lapsed -- nothing live to answer for

            outSpell = best->param.form;
            outProxy = best->castProxy;
            if (outFlags) *outFlags = best->castFlags;
            return true;
        }
        return false;   // controlled, but not on the cast channel
    }

    bool ControlMap::TryGetCastSeatClaim(RE::FormID actor, CastSeatClaim& out) const {
        // The FIVE-SEAT read (core/CastSeats.cpp, core/EquipGate.cpp). Same RCU reader
        // discipline as TryGetCastClaim above -- ANY thread; relaxed pre-gate, ONE
        // acquire-load of a LOCAL frozen snapshot generation, ONE hash lookup, then
        // everything the seats need copied OUT BY VALUE so nothing aliases
        // snapshot-owned storage past this call (INVARIANTS #12).
        //
        // RELEASE ORDERING (Docs/INVARIANTS.md #20). A release removes the claim from
        // the writer's private working copy and only then Publish()es it, so a reader
        // sees EITHER the old generation (claim live, seats answer) OR the new one
        // (claim gone -> `false` here -> every seat chains to the engine). There is no
        // torn intermediate and no stale-claim window: the cleared claim is published
        // BEFORE anything else the release does (the proxy teardown is deliberately
        // deferred one main-thread hop past the publish -- channels/CastCompose.cpp).
        out = CastSeatClaim{};
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            // Winner = the ONE comparator (ControlMap.h::BetterClaim), the same rule
            // every other winner-selection in this class uses -- ranking only the LIVE
            // claims (F5-3; the full rationale is on TryGetCastClaim above). A claim
            // whose TTL has elapsed is treated as ALREADY GONE without waiting for the
            // Drain auto-release pass: that pass runs once per frame, a combat-thread
            // seat can run several times inside that frame, and a seat answering from
            // an expired claim is exactly the "unprotected drive" hazard the TTL exists
            // to prevent. Skipping lapsed candidates rather than testing only the
            // winner keeps that guarantee AND stops a lapsed claim from hiding a live
            // lower-basis one for up to a frame.
            const auto nowMs = apmf::clock::MonotonicMs();
            const Claim* best = nullptr;
            for (const auto& c : cs.claims) {
                if (c.expiresMs != 0 && nowMs >= c.expiresMs) continue;   // lapsed -- already gone
                if (!best || BetterClaim(c, *best)) best = &c;
            }
            if (!best) return false;   // every claim here has lapsed -- nothing live to answer for

            out.spell        = best->param.form;
            out.proxy        = best->castProxy;
            out.target       = best->castTarget;
            out.targetHandle = best->castTargetHandle;
            out.flags        = best->castFlags;
            out.expiresMs    = best->expiresMs;
            return true;
        }
        return false;   // controlled, but not on the cast channel
    }

    bool ControlMap::TryGetCastClaimForHand(RE::FormID actor, CastHand hand, RE::FormID& outSpell,
                                            RE::FormID& outProxy, std::uint32_t* outFlags) const {
        // feat/per-hand-cast-claims. `kUnknown` is byte-identical to the unscoped
        // overload (the pre-existing actor-wide floor for a caller that cannot
        // resolve a hand) -- see ControlMap.h.
        if (hand == CastHand::kUnknown) return TryGetCastClaim(actor, outSpell, outProxy, outFlags);

        outSpell = 0;
        outProxy = 0;
        if (outFlags) *outFlags = 0;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            // Winner = highest basis AMONG CLAIMS THAT OCCUPY `hand` (a
            // kCastFlag_DualCast claim occupies both); a claim on the OTHER hand is
            // simply not a candidate here -- never mixed into this arbitration, and
            // never able to mask this hand's own claim regardless of its basis.
            // Lapsed claims are not candidates -- same treat-as-gone rule, applied to
            // the candidate SET rather than only to the winner (F5-3; rationale on
            // TryGetCastClaim above). It matters most here: this hand's live floor
            // being masked by a lapsed claim on the same hand is precisely how an
            // ALLOWANCE reader ends up dropping a deny the client still holds.
            const auto nowMs = apmf::clock::MonotonicMs();
            const Claim* best = nullptr;
            for (const auto& c : cs.claims) {
                if (!ClaimOccupiesHand(c.castFlags, hand)) continue;
                if (c.expiresMs != 0 && nowMs >= c.expiresMs) continue;   // lapsed -- already gone
                if (!best || BetterClaim(c, *best)) best = &c;   // ONE comparator (ControlMap.h)
            }
            if (!best) return false;   // no LIVE claim occupies this hand -- not this hand's business

            outSpell = best->param.form;
            outProxy = best->castProxy;
            if (outFlags) *outFlags = best->castFlags;
            return true;
        }
        return false;   // controlled, but not on the cast channel
    }

    bool ControlMap::TryGetCastSeatClaimForHand(RE::FormID actor, CastHand hand, CastSeatClaim& out) const {
        // feat/per-hand-cast-claims. `kUnknown` forwards to the unscoped overload,
        // byte-identical to its existing behavior (see ControlMap.h).
        if (hand == CastHand::kUnknown) return TryGetCastSeatClaim(actor, out);

        out = CastSeatClaim{};
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast);
        if (!channel) return false;

        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            if (cs.claims.empty()) return false;
            // Winner = highest basis AMONG CLAIMS THAT OCCUPY `hand` -- same
            // per-hand arbitration as TryGetCastClaimForHand above.
            // Lapsed claims are not candidates -- same TTL-as-gone treatment as
            // TryGetCastSeatClaim above, applied to the candidate SET (F5-3).
            const auto nowMs = apmf::clock::MonotonicMs();
            const Claim* best = nullptr;
            for (const auto& c : cs.claims) {
                if (!ClaimOccupiesHand(c.castFlags, hand)) continue;
                if (c.expiresMs != 0 && nowMs >= c.expiresMs) continue;   // lapsed -- already gone
                if (!best || BetterClaim(c, *best)) best = &c;   // ONE comparator (ControlMap.h)
            }
            if (!best) return false;   // no LIVE claim occupies this hand

            out.spell        = best->param.form;
            out.proxy        = best->castProxy;
            out.target       = best->castTarget;
            out.targetHandle = best->castTargetHandle;
            out.flags        = best->castFlags;
            out.expiresMs    = best->expiresMs;
            return true;
        }
        return false;   // controlled, but not on the cast channel
    }

    bool ControlMap::TryGetCastSeatClaimForForm(RE::FormID actor, RE::FormID form, CastSeatClaim& out) const {
        // feat/per-hand-cast-claims. core/CastSeats.cpp's four caster-vtable seats
        // and core/CastClassify.cpp's SEAT 0 never resolve a hand -- they already
        // know the candidate FORM they are deliberating about. Search every LIVE
        // (unexpired) claim on the cast channel for the one whose driven form
        // (proxy, else spell) equals `form`; NOT an arbitration -- the two
        // concurrent claims never compete for the same magic-item instance, so
        // there is nothing to pick a "winner" between here.
        out = CastSeatClaim{};
        if (form == 0) return false;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;

        const NpcCtl& npc = it->second;
        if (!npc.handle.get()) return false;

        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_Cast);
        if (!channel) return false;

        const auto nowMs = apmf::clock::MonotonicMs();
        for (const auto& cs : npc.channels) {
            if (cs.channel != channel) continue;
            for (const auto& c : cs.claims) {
                if (c.expiresMs != 0 && nowMs >= c.expiresMs) continue;   // gone -- do not let it match
                const RE::FormID driven = c.castProxy ? c.castProxy : c.param.form;
                if (driven == 0 || driven != form) continue;
                out.spell        = c.param.form;
                out.proxy        = c.castProxy;
                out.target       = c.castTarget;
                out.targetHandle = c.castTargetHandle;
                out.flags        = c.castFlags;
                out.expiresMs    = c.expiresMs;
                return true;
            }
            return false;
        }
        return false;   // controlled, but not on the cast channel
    }

    RE::FormID ControlMap::GetCastProxy(Handle handle) const {
        // ABI v6 observability (APMF_API_v6::GetCastProxy). Keyed by HANDLE ALONE
        // (the client may not have the actor FormID at every call site) -- scans
        // the small controlled set rather than a single hash lookup, same
        // trade-off ClaimedActors already makes. Never the hot per-tick path.
        if (handle == APMF_API::kInvalidHandle) return 0;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return 0;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        const auto nowMs = apmf::clock::MonotonicMs();
        for (const auto& [fid, npc] : *snap) {
            for (const auto& cs : npc.channels) {
                for (const auto& c : cs.claims) {
                    if (c.handle != handle) continue;
                    // Same "expired but not yet swept" treat-as-gone rule as
                    // TryGetCastSeatClaimForForm -- never hand back a proxy for a
                    // claim the writer's TTL pass is about to release.
                    if (c.expiresMs != 0 && nowMs >= c.expiresMs) return 0;
                    return c.castProxy;   // 0 by construction on every non-cast claim
                }
            }
        }
        return 0;   // unknown/stale/released handle
    }

    bool ControlMap::IsClaimLive(Handle handle) const {
        // ABI v6 observability (APMF_API_v6::IsClaimLive). Same handle-keyed scan
        // and TTL-as-gone treatment as GetCastProxy above.
        if (handle == APMF_API::kInvalidHandle) return false;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        const auto nowMs = apmf::clock::MonotonicMs();
        for (const auto& [fid, npc] : *snap) {
            for (const auto& cs : npc.channels) {
                for (const auto& c : cs.claims) {
                    if (c.handle != handle) continue;
                    if (c.expiresMs != 0 && nowMs >= c.expiresMs) return false;
                    return true;
                }
            }
        }
        return false;   // unknown/stale/released handle
    }

    std::vector<RE::Actor*> ControlMap::ClaimedActors(Intent intent) const {
        // Observability/probe use only (Docs/SPEC-PACKAGE-HOLD.md §4). Same RCU
        // discipline as TryGetOwningClaim: relaxed pre-gate, one acquire-load of a
        // LOCAL frozen snapshot copy. Read-only -- does not touch obsTick or
        // anything else; never called from the hot per-tick path.
        std::vector<RE::Actor*> out;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return out;

        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto* channel = Registry::Get().ChannelForIntent(intent);
        if (!channel) return out;

        for (const auto& [fid, npc] : *snap) {
            auto actor = npc.handle.get();
            if (!actor) continue;   // unloaded; the Drain sweep reclaims it
            for (const auto& cs : npc.channels) {
                if (cs.channel == channel && !cs.claims.empty()) { out.push_back(actor.get()); break; }
            }
        }
        return out;
    }

    void ControlMap::ReleaseAll(const char* why) {
        // Drain any pending ops first so a just-enqueued claim is not orphaned, then
        // restore + drop every controlled NPC. Writer thread only (see ControlMap.h:
        // confirmed the same MAIN thread as Drain by the [threadcheck] evidence).
        MapType next = *m_current;
        {
            std::vector<PendingOp> ops;
            { std::scoped_lock lock(m_qmx); ops.swap(m_queue); }
            for (const auto& op : ops) {
                switch (op.kind) {
                case PendingOp::Kind::kRequest:      ApplyRequest(op, next);                  break;
                case PendingOp::Kind::kRelease:      ApplyRelease(op.handle, next);           break;
                case PendingOp::Kind::kRepoint:      ApplyRepoint(op.handle, op.param, next); break;
                case PendingOp::Kind::kSetAllowList: ApplySetSpellAllowList(op.handle, op.altForms, op.altCount, next); break;
                case PendingOp::Kind::kCast:         ApplyRequest(op, next);                  break;
                }
            }
        }
        const std::size_t n = next.size();
        if (n != 0) {
            for (auto& [formID, npc] : next) {
                // Fresh lookup by FormID -- never a cached raw pointer, so no UAF even
                // at kPreLoadGame with a torn-down actor (returns null for a deleted
                // form). channel->Release() is where AV-ledger restore fires
                // (core/AvLedger) -- every one of these completes HERE, strictly
                // before the Publish(MapType{}) below, so a reader can never observe
                // a torn/partially-cleared map: it either still sees the prior
                // (about-to-be-replaced) generation in full, or the fully-empty one.
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
                for (auto& cs : npc.channels) {
                    if (cs.channel) cs.channel->Release(formID, actor);
                }
            }
        }
        m_index.clear();
        if (n != 0 || !m_current->empty()) {
            Publish(MapType{});   // atomically publish the wipe -- no reader ever sees a partial clear
            spdlog::info("[ctl] ReleaseAll ({}) -- {} controlled NPC(s) restored and cleared.", why, n);
        }
    }

    void ControlMap::Clear() {
        // Revert / new game: wipe WITHOUT restoring (see ControlMap.h -- the actors
        // are being replaced; the co-saved AV ledger handles the incoming save).
        // Writer thread only; no channel->Release() calls here by design (unchanged
        // from the pre-RCU behavior), so there is nothing to order against the
        // publish besides the queue clear.
        { std::scoped_lock lock(m_qmx); m_queue.clear(); }
        m_index.clear();
        if (!m_current->empty()) Publish(MapType{});
    }

}
