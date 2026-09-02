#include "PCH.h"
#include "core/AvLedger.h"

namespace apmf::av {

    namespace {
        // (FormID << 32 | ActorValue) -> the captured pre-override value.
        std::unordered_map<std::uint64_t, float> g_ledger;

        // Overrides read from a co-save, awaiting restore on kPostLoadGame.
        struct Pending { RE::FormID actor; std::uint32_t av; float prev; };
        std::vector<Pending> g_pending;

        std::uint64_t Key(RE::FormID f, RE::ActorValue av) {
            return (static_cast<std::uint64_t>(f) << 32) | static_cast<std::uint32_t>(av);
        }
    }

    void Override(RE::Actor* actor, RE::ActorValue av, float value) {
        if (!actor) return;
        auto* avo = actor->AsActorValueOwner();
        if (!avo) return;
        const std::uint64_t k = Key(actor->GetFormID(), av);
        if (!g_ledger.contains(k)) g_ledger[k] = avo->GetActorValue(av);   // capture prior ONCE
        avo->SetActorValue(av, value);
    }

    void Restore(RE::Actor* actor, RE::ActorValue av) {
        if (!actor) return;
        const std::uint64_t k = Key(actor->GetFormID(), av);
        auto it = g_ledger.find(k);
        if (it == g_ledger.end()) return;
        if (auto* avo = actor->AsActorValueOwner()) avo->SetActorValue(av, it->second);
        g_ledger.erase(it);
    }

    void Save(SKSE::SerializationInterface* intf) {
        if (!intf->OpenRecord(kRecordType, kRecordVersion)) return;
        const std::uint32_t count = static_cast<std::uint32_t>(g_ledger.size());
        intf->WriteRecordData(&count, sizeof(count));
        for (const auto& [k, prev] : g_ledger) {
            const RE::FormID    formID = static_cast<RE::FormID>(k >> 32);
            const std::uint32_t av     = static_cast<std::uint32_t>(k & 0xFFFFFFFF);
            intf->WriteRecordData(&formID, sizeof(formID));
            intf->WriteRecordData(&av, sizeof(av));
            intf->WriteRecordData(&prev, sizeof(prev));
        }
        spdlog::info("[avledger] co-saved {} outstanding AV override(s).", count);
    }

    void Load(SKSE::SerializationInterface* intf) {
        std::uint32_t count = 0;
        if (!intf->ReadRecordData(&count, sizeof(count))) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID    formID = 0;
            std::uint32_t av     = 0;
            float         prev   = 0.0f;
            if (!intf->ReadRecordData(&formID, sizeof(formID))) break;
            if (!intf->ReadRecordData(&av, sizeof(av)))         break;
            if (!intf->ReadRecordData(&prev, sizeof(prev)))     break;
            RE::FormID resolved = 0;
            if (intf->ResolveFormID(formID, resolved))          // handle load-order remap
                g_pending.push_back(Pending{ resolved, av, prev });
        }
        spdlog::info("[avledger] read {} co-saved AV override(s) to restore on post-load.", g_pending.size());
    }

    void ApplyPending() {
        if (g_pending.empty()) return;
        std::size_t restored = 0;
        for (const auto& p : g_pending) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(p.actor);
            if (!actor) continue;                          // deleted -> nothing to restore
            if (auto* avo = actor->AsActorValueOwner()) {
                avo->SetActorValue(static_cast<RE::ActorValue>(p.av), p.prev);
                ++restored;
            }
        }
        spdlog::info("[avledger] post-load: restored {} of {} stranded AV override(s).",
                     restored, g_pending.size());
        g_pending.clear();
    }

    void Revert() {
        g_ledger.clear();
        g_pending.clear();
    }

}
