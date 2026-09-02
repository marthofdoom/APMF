#include "PCH.h"
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
#include "core/AvLedger.h"

// ============================================================================
// APMF -- AI Package Management Framework. Entry point (thin).
//
//   core/    -- the extensible spine: the 0xAD arbiter hook, the multi-NPC
//               ControlMap, the channel Registry, the Arbiter (test surface +
//               per-frame drain), the input surface, the co-saved AV ledger, and
//               the real C-ABI client API (APMF_API.h / core/ClientAPI).
//   channels/ -- one small self-registering module per directable AI facet.
// See MAP.md for the module map and Docs/ARCHITECTURE.md for the design.
// ============================================================================

namespace {

    constexpr std::uint32_t kSerUniqueID = 'APMF';   // co-save owner tag

    // --- SKSE serialization: co-save the outstanding AV overrides so a
    // save-while-engaged is never stranded across a load (INVARIANTS #15). ---
    void OnSave(SKSE::SerializationInterface* intf) {
        apmf::av::Save(intf);
    }
    void OnLoad(SKSE::SerializationInterface* intf) {
        std::uint32_t type = 0, version = 0, length = 0;
        while (intf->GetNextRecordInfo(type, version, length)) {
            if (type == apmf::av::kRecordType) apmf::av::Load(intf, version);
        }
    }
    void OnRevert(SKSE::SerializationInterface*) {
        // New game / pre-load wipe: drop all control WITHOUT restoring (actors are
        // being replaced) and clear the ledger; the co-save's pending set restores
        // the incoming save's overrides on kPostLoadGame.
        apmf::ControlMap::Get().Clear();
        apmf::av::Revert();
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            apmf::hook::Install();
            if (!REL::Module::IsVR()) {          // no drain seat on VR -> no test surface
                apmf::input::Register();
                apmf::input::LogHelp();
            } else {
                spdlog::warn("[input] VR runtime -- hooks refused, input test surface NOT armed.");
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            apmf::Arbiter::Get().ReleaseAll("kPreLoadGame");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            apmf::av::ApplyPending();             // restore any stranded AV overrides
            break;
        default:
            break;
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    apmf::log::Setup();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== APMF {}.{}.{} loading -- game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID(kSerUniqueID);
        ser->SetSaveCallback(OnSave);
        ser->SetLoadCallback(OnLoad);
        ser->SetRevertCallback(OnRevert);
    }

    spdlog::info("=== APMF loaded (multi-NPC control map + channel registry + C-ABI client API) ===");
    return true;
}
