#include "PCH.h"
#include "core/Log.h"
#include "core/ControlMap.h"
#include "core/Registry.h"
#include "core/Clock.h"
#include "channels/CastCompose.h"   // castcompose::ExtractFromPackage (ch.8b FromPackage read)

namespace apmf {

    namespace {
        constexpr std::uint64_t kObsEvery = 60;   // per-NPC observability ~1/s @ 60fps

        const char* PkgTypeName(RE::TESPackage* pkg) {
            if (!pkg) return "<none>";
            const char* n = pkg->GetObjectTypeName();
            return n ? n : "<unnamed>";
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
        if (op.intent == APMF_API::kIntent_Cast) {
            // Flags: the kCast op carries them in castFlags; a degenerate
            // RequestEx(kIntent_Cast) carries them in param.ival (kept in parity).
            castFlags  = (op.kind == PendingOp::Kind::kCast) ? op.castFlags
                                                             : static_cast<std::uint32_t>(op.param.ival);
            castProxy  = op.castProxy;
            castTarget = op.castTarget;
            RE::FormID spell = op.param.form;
            if (castFlags & APMF_API::kCastFlag_FromPackage) {
                RE::FormID outSpell = 0, outTarget = 0;
                if (!apmf::castcompose::ExtractFromPackage(op.param.form, outSpell, outTarget)) {
                    spdlog::warn("[ch.8b] cast-from-package: no spell input on 0x{} -- REFUSED "
                                 "(op dropped; package never run/offered/evaluated). The client "
                                 "should pass the spell directly.", apmf::log::Hex(op.param.form));
                    return false;   // handle was never registered in m_index -> never dangling
                }
                spell = outSpell;
                if (castTarget == 0) castTarget = outTarget;   // client's own target wins if it named one
            }
            effParam.form = spell;
            std::uint32_t ttl = (op.kind == PendingOp::Kind::kCast) ? op.ttlMs : 0;
            if (ttl == 0) ttl = APMF_API::kCastDefaultTtlMs;
            if (ttl > APMF_API::kCastMaxTtlMs) ttl = APMF_API::kCastMaxTtlMs;
            expiresMs = apmf::clock::MonotonicMs() + ttl;
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

        // Before adding this claim, note the incumbent owner's basis (if any). Seed
        // from the first EXISTING claim -- never a 0.0 floor -- so a negative-basis
        // incumbent arbitrates correctly (a 0.0 floor would let a claim below the true
        // max but above 0 wrongly "win", and mislog owner basis=0.0). Mirrors
        // ApplyRelease's ownerOf (best = &cs.front(), then max). When cc->claims is
        // empty (freshChannel) the 0.0 is unused -- the freshChannel path Engages, and
        // oldBest is only read on the additional-claim (non-empty) path below.
        float oldBest = cc->claims.empty() ? 0.0f : cc->claims.front().basis;
        for (auto& c : cc->claims) oldBest = (c.basis > oldBest) ? c.basis : oldBest;

        Claim newClaim{ op.handle, op.basis, effParam };
        newClaim.castProxy  = castProxy;
        newClaim.castTarget = castTarget;
        newClaim.castFlags  = castFlags;
        newClaim.expiresMs  = expiresMs;
        cc->claims.push_back(newClaim);
        m_index[op.handle] = { op.actor, channel };

        if (freshChannel) {
            channel->Engage(op.actor, actor, effParam);   // 0 -> 1: apply the source-block once
            spdlog::info("[ctl] 0x{} '{}' + ch.{} {} ENGAGED (h={}, basis={:.1f}, form=0x{}). NPCs controlled: {}.",
                         apmf::log::Hex(op.actor), actor->GetName() ? actor->GetName() : "?",
                         channel->ChannelNo(), channel->Name(), op.handle, op.basis,
                         apmf::log::Hex(effParam.form), map.size());
        } else {
            // Additional claim on an already-engaged channel: arbitrate by basis
            // (higher wins; tie -> earliest, so the incumbent keeps ownership unless
            // this claim's basis is STRICTLY higher). On a real owner change, hand a
            // parameterized channel the new winner's payload (parameterless channels
            // no-op OnOwnerChanged; the claim just refcounts the engagement).
            const bool newOwner = (op.basis > oldBest);
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
                for (auto& c : cs) if (c.basis > best->basis) best = &c;   // strict: tie keeps earliest
                return best;
            };
            const Handle oldOwner = [&] { Claim* o = ownerOf(claims); return o ? o->handle : APMF_API::kInvalidHandle; }();

            for (auto it = claims.begin(); it != claims.end(); ++it) {
                if (it->handle == handle) { claims.erase(it); break; }
            }
            if (claims.empty()) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);   // may be null
                channel->Release(formID, actor);   // 1 -> 0: restore the AI
                spdlog::info("[ctl] 0x{} - ch.{} {} RELEASED (h={}).",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(), handle);
                npc.channels.erase(ccIt);
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

            // Find this claim, and the current owner (highest basis; tie -> earliest).
            Claim* self = nullptr;
            Claim* best = &claims.front();
            for (auto& c : claims) {
                if (c.handle == handle)     self = &c;
                if (c.basis  > best->basis) best = &c;   // strict: tie keeps earliest
            }
            if (!self) return false;   // handle not in this channel (should not happen)

            self->param = param;   // update the stored param regardless of ownership
            if (best == self) {
                // This claim OWNS the channel -> re-point it in place (same handle,
                // no release/re-engage). A parameterized channel switches its held
                // target/spell; a parameterless channel no-ops OnOwnerChanged.
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);   // may be null
                channel->OnOwnerChanged(formID, actor, param);
                spdlog::info("[ctl] 0x{} ~ ch.{} {} REPOINT (h={}, form=0x{}).",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(),
                             handle, apmf::log::Hex(param.form));
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
            // Winner = highest basis; tie -> earliest (same rule everywhere else).
            const Claim* best = &cs.claims.front();
            for (const auto& c : cs.claims) {
                if (c.basis > best->basis) best = &c;
            }
            outSpell = best->param.form;
            outProxy = best->castProxy;
            if (outFlags) *outFlags = best->castFlags;
            return true;
        }
        return false;   // controlled, but not on the cast channel
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
