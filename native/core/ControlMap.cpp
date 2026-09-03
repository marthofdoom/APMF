#include "PCH.h"
#include "core/Log.h"
#include "core/ControlMap.h"
#include "core/Registry.h"

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

    // ---- Writer thread ONLY (Drain/ApplyRequest/ApplyRelease/ApplyRepoint/
    // ReleaseAll/Clear). OnActorUpdate below is the exception -- ANY thread. ----

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
            bool anyUnloaded = false;
            for (const auto& kv : *m_current) {
                if (!kv.second.handle.get()) { anyUnloaded = true; break; }
            }
            if (!anyUnloaded) return;
        }

        // RCU: build a private working copy of the last-published snapshot -- only
        // reached when there's real work (an op arrived, or something unloaded).
        // Deep-copies the (small, controlled-NPCs-only) map -- cheap -- and is
        // published ONLY if something actually changed.
        MapType next = *m_current;
        bool changed = false;
        for (const auto& op : ops) {
            switch (op.kind) {
            case PendingOp::Kind::kRequest: changed |= ApplyRequest(op, next);                  break;
            case PendingOp::Kind::kRelease: changed |= ApplyRelease(op.handle, next);           break;
            case PendingOp::Kind::kRepoint: changed |= ApplyRepoint(op.handle, op.param, next); break;
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

        cc->claims.push_back(Claim{ op.handle, op.basis, op.param });
        m_index[op.handle] = { op.actor, channel };

        if (freshChannel) {
            channel->Engage(op.actor, actor, op.param);   // 0 -> 1: apply the source-block once
            spdlog::info("[ctl] 0x{} '{}' + ch.{} {} ENGAGED (h={}, basis={:.1f}, form=0x{}). NPCs controlled: {}.",
                         apmf::log::Hex(op.actor), actor->GetName() ? actor->GetName() : "?",
                         channel->ChannelNo(), channel->Name(), op.handle, op.basis,
                         apmf::log::Hex(op.param.form), map.size());
        } else {
            // Additional claim on an already-engaged channel: arbitrate by basis
            // (higher wins; tie -> earliest, so the incumbent keeps ownership unless
            // this claim's basis is STRICTLY higher). On a real owner change, hand a
            // parameterized channel the new winner's payload (parameterless channels
            // no-op OnOwnerChanged; the claim just refcounts the engagement).
            const bool newOwner = (op.basis > oldBest);
            if (newOwner) channel->OnOwnerChanged(op.actor, actor, op.param);
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
                case PendingOp::Kind::kRequest: ApplyRequest(op, next);                  break;
                case PendingOp::Kind::kRelease: ApplyRelease(op.handle, next);           break;
                case PendingOp::Kind::kRepoint: ApplyRepoint(op.handle, op.param, next); break;
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
