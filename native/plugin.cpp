#include "PCH.h"
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Input.h"
#include "core/Arbiter.h"

// ============================================================================
// APMF -- AI Package Management Framework. Entry point (thin).
//
// This is the FIRST real modular APMF (not a prototype/probe). The architecture:
//   core/    -- the extensible spine: the 0xAD arbiter hook, the channel Registry,
//               the Arbiter (target + per-tick drive), the input test surface, and
//               the stubbed Layer-2 client API seam.
//   channels/ -- one small self-registering module per directable AI facet. Add a
//               facet = drop one file; nothing here changes.
// See MAP.md for the module map and Docs/ARCHITECTURE.md for the design.
// ============================================================================

namespace {

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            apmf::hook::Install();
            apmf::input::Register();
            apmf::input::LogHelp();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            apmf::Arbiter::Get().ReleaseAll("kPreLoadGame");
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

    spdlog::info("=== APMF loaded (modular core + channel registry) ===");
    return true;
}
