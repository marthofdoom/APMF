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

    // ---- Client/API side (ANY thread): enqueue only, never touch the map. ----

    Handle ControlMap::EnqueueRequest(RE::FormID actor, Intent intent, float basis) {
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

    // ---- Game thread ONLY. ----

    void ControlMap::Drain() {
        // Move the queued ops out under the lock, then apply them lock-free.
        std::vector<PendingOp> ops;
        {
            std::scoped_lock lock(m_qmx);
            if (m_queue.empty()) { /* fall through to the sweep */ }
            else ops.swap(m_queue);
        }
        for (const auto& op : ops) {
            if (op.kind == PendingOp::Kind::kRequest) ApplyRequest(op);
            else                                      ApplyRelease(op.handle);
        }

        // Sweep controlled NPCs that have unloaded (they stop calling OnActorUpdate,
        // so only this periodic pass reclaims them). Cheap: iterates the (small)
        // control map, never all NPCs.
        for (auto it = m_map.begin(); it != m_map.end();) {
            auto& ctl = it->second;
            if (ctl.handle.get()) { ++it; continue; }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->first);   // may be null
            for (auto& cs : ctl.channels) {
                if (cs.channel) cs.channel->Release(it->first, actor);
                for (auto& c : cs.claims) m_index.erase(c.handle);
            }
            spdlog::info("[ctl] 0x{} unloaded -- released {} channel(s), dropped from control.",
                         apmf::log::Hex(it->first), ctl.channels.size());
            it = m_map.erase(it);
        }
    }

    void ControlMap::ApplyRequest(const PendingOp& op) {
        auto* channel = Registry::Get().ChannelForIntent(op.intent);
        if (!channel) return;   // (already checked at enqueue; defensive)

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(op.actor);
        if (!actor) {
            spdlog::warn("[ctl] request h={} ignored -- actor 0x{} not found/loaded.",
                         op.handle, apmf::log::Hex(op.actor));
            return;
        }

        auto&      npc     = m_map[op.actor];
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

        cc->claims.push_back(Claim{ op.handle, op.basis });
        m_index[op.handle] = { op.actor, channel };

        if (freshChannel) {
            channel->Engage(op.actor, actor);   // 0 -> 1: apply the source-block once
            spdlog::info("[ctl] 0x{} '{}' + ch.{} {} ENGAGED (h={}, basis={:.1f}). NPCs controlled: {}.",
                         apmf::log::Hex(op.actor), actor->GetName() ? actor->GetName() : "?",
                         channel->ChannelNo(), channel->Name(), op.handle, op.basis, m_map.size());
        } else {
            // Additional claim on an already-engaged channel: arbitrate by basis
            // (higher wins; tie -> earliest). Parameterless channels do not change
            // behavior on winner change; the claim refcounts the engagement.
            float best = cc->claims.front().basis;
            for (auto& c : cc->claims) best = (c.basis > best) ? c.basis : best;
            spdlog::info("[ctl] 0x{} + ch.{} {} additional claim (h={}, basis={:.1f}); {} claim(s), "
                         "owner basis={:.1f}.", apmf::log::Hex(op.actor), channel->ChannelNo(), channel->Name(),
                         op.handle, op.basis, cc->claims.size(), best);
        }
    }

    void ControlMap::ApplyRelease(Handle handle) {
        auto idxIt = m_index.find(handle);
        if (idxIt == m_index.end()) return;   // unknown/stale/already-released
        const RE::FormID formID  = idxIt->second.first;
        Channel*         channel = idxIt->second.second;
        m_index.erase(idxIt);

        auto npcIt = m_map.find(formID);
        if (npcIt == m_map.end()) return;
        auto& npc = npcIt->second;

        for (auto ccIt = npc.channels.begin(); ccIt != npc.channels.end(); ++ccIt) {
            if (ccIt->channel != channel) continue;
            auto& claims = ccIt->claims;
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
                spdlog::info("[ctl] 0x{} - ch.{} {} claim dropped (h={}); {} claim(s) remain.",
                             apmf::log::Hex(formID), channel->ChannelNo(), channel->Name(), handle, claims.size());
            }
            break;
        }
        if (npc.channels.empty()) m_map.erase(npcIt);
    }

    void ControlMap::OnActorUpdate(RE::Actor* actor) {
        if (m_map.empty()) return;                 // near-zero cost: nothing controlled
        auto it = m_map.find(actor->GetFormID());  // the single hash lookup
        if (it == m_map.end()) return;             // uncontrolled NPC -> done

        auto& npc = it->second;
        if (!npc.handle.get()) return;             // unloaded; the Drain sweep reclaims it

        for (auto& cs : npc.channels) {
            if (cs.channel) cs.channel->Tick(it->first, actor);   // most channels no-op (clean block)
        }

        if ((++npc.obsTick % kObsEvery) != 0) return;
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

    void ControlMap::ReleaseAll(const char* why) {
        // Drain any pending ops first so a just-enqueued claim is not orphaned, then
        // restore + drop every controlled NPC. Game/main thread only.
        {
            std::vector<PendingOp> ops;
            { std::scoped_lock lock(m_qmx); ops.swap(m_queue); }
            for (const auto& op : ops) {
                if (op.kind == PendingOp::Kind::kRequest) ApplyRequest(op);
                else                                      ApplyRelease(op.handle);
            }
        }
        if (m_map.empty()) return;
        const std::size_t n = m_map.size();
        for (auto& [formID, npc] : m_map) {
            // Fresh lookup by FormID -- never a cached raw pointer, so no UAF even at
            // kPreLoadGame with a torn-down actor (returns null for a deleted form).
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
            for (auto& cs : npc.channels) {
                if (cs.channel) cs.channel->Release(formID, actor);
            }
        }
        m_map.clear();
        m_index.clear();
        spdlog::info("[ctl] ReleaseAll ({}) -- {} controlled NPC(s) restored and cleared.", why, n);
    }

    void ControlMap::Clear() {
        std::scoped_lock lock(m_qmx);
        m_queue.clear();
        m_map.clear();
        m_index.clear();
    }

}
