#include "PCH.h"
#include "core/AvLedger.h"

#include <cmath>

namespace apmf::av {

    namespace {
        struct Entry {
            float prev    = 0.0f;   // value before our override (what we restore TO)
            float applied = 0.0f;   // value we set (current must still equal this to restore)
        };

        // (FormID << 32 | ActorValue) -> {prev, applied}.
        std::unordered_map<std::uint64_t, Entry> g_ledger;

        // Overrides read from a co-save, awaiting restore on kPostLoadGame.
        struct Pending { RE::FormID actor; std::uint32_t av; float prev; float applied; };
        std::vector<Pending> g_pending;

        std::uint64_t Key(RE::FormID f, RE::ActorValue av) {
            return (static_cast<std::uint64_t>(f) << 32) | static_cast<std::uint32_t>(av);
        }

        // Magnitude-aware float compare so the clobber guard is robust across AV
        // scales (aggression ~0-4, speed ~100, detect range ~1000).
        bool NearlyEqual(float a, float b) {
            return std::fabs(a - b) <= 1e-3f * (1.0f + std::fabs(b));
        }
    }

    void Override(RE::FormID id, RE::Actor* actor, RE::ActorValue av, float value) {
        if (!actor) return;
        auto* avo = actor->AsActorValueOwner();
        if (!avo) return;
        const std::uint64_t k = Key(id, av);
        if (auto it = g_ledger.find(k); it != g_ledger.end()) {
            it->second.applied = value;                    // keep the original prev
        } else {
            g_ledger.emplace(k, Entry{ avo->GetActorValue(av), value });   // capture prior ONCE
        }
        avo->SetActorValue(av, value);
    }

    void Restore(RE::FormID id, RE::Actor* actor, RE::ActorValue av) {
        auto it = g_ledger.find(Key(id, av));
        if (it == g_ledger.end()) return;
        if (actor) {
            if (auto* avo = actor->AsActorValueOwner()) {
                const float current = avo->GetActorValue(av);
                if (NearlyEqual(current, it->second.applied)) {
                    avo->SetActorValue(av, it->second.prev);   // still ours -> restore
                } else {
                    spdlog::info("[avledger] 0x{:08X} av={} changed externally ({:.2f} != applied {:.2f}); "
                                 "leaving the newer value.", id, static_cast<std::uint32_t>(av),
                                 current, it->second.applied);
                }
            }
        }
        g_ledger.erase(it);   // drop our record regardless (no leak on a deleted actor)
    }

    void Save(SKSE::SerializationInterface* intf) {
        if (!intf->OpenRecord(kRecordType, kRecordVersion)) {
            spdlog::warn("[avledger] OpenRecord failed -- AV overrides NOT co-saved this save.");
            return;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(g_ledger.size());
        bool ok = intf->WriteRecordData(&count, sizeof(count));
        for (const auto& [k, e] : g_ledger) {
            const RE::FormID    formID = static_cast<RE::FormID>(k >> 32);
            const std::uint32_t av     = static_cast<std::uint32_t>(k & 0xFFFFFFFF);
            ok = ok && intf->WriteRecordData(&formID, sizeof(formID));
            ok = ok && intf->WriteRecordData(&av, sizeof(av));
            ok = ok && intf->WriteRecordData(&e.prev, sizeof(e.prev));
            ok = ok && intf->WriteRecordData(&e.applied, sizeof(e.applied));
        }
        if (!ok) spdlog::error("[avledger] WriteRecordData failed mid-record -- co-save may be truncated.");
        else     spdlog::info("[avledger] co-saved {} outstanding AV override(s).", count);
    }

    void Load(SKSE::SerializationInterface* intf, std::uint32_t version) {
        if (version > kRecordVersion) {
            spdlog::warn("[avledger] co-save record v{} newer than known v{} -- skipping.", version, kRecordVersion);
            return;
        }
        std::uint32_t count = 0;
        if (!intf->ReadRecordData(&count, sizeof(count))) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID    formID  = 0;
            std::uint32_t av      = 0;
            float         prev    = 0.0f;
            float         applied = 0.0f;
            if (!intf->ReadRecordData(&formID, sizeof(formID)))   break;
            if (!intf->ReadRecordData(&av, sizeof(av)))           break;
            if (!intf->ReadRecordData(&prev, sizeof(prev)))       break;
            if (!intf->ReadRecordData(&applied, sizeof(applied))) break;
            RE::FormID resolved = 0;
            if (intf->ResolveFormID(formID, resolved))            // handle load-order remap
                g_pending.push_back(Pending{ resolved, av, prev, applied });
        }
        spdlog::info("[avledger] read {} co-saved AV override(s) to restore on post-load.", g_pending.size());
    }

    void ApplyPending() {
        if (g_pending.empty()) return;
        std::size_t restored = 0, skipped = 0;
        for (const auto& p : g_pending) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(p.actor);
            if (!actor) continue;                          // deleted -> nothing to restore
            auto* avo = actor->AsActorValueOwner();
            if (!avo) continue;
            const auto  av      = static_cast<RE::ActorValue>(p.av);
            const float current = avo->GetActorValue(av);
            if (NearlyEqual(current, p.applied)) { avo->SetActorValue(av, p.prev); ++restored; }
            else                                 { ++skipped; }   // changed externally -> keep newer
        }
        spdlog::info("[avledger] post-load: restored {} stranded AV override(s), left {} externally-changed.",
                     restored, skipped);
        g_pending.clear();
    }

    void Revert() {
        g_ledger.clear();
        g_pending.clear();
    }

}
