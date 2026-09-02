#pragma once

namespace apmf::log {
    // Set up spdlog -> Data/SKSE/Plugins/APMF.log (flush-every-line so a CTD keeps
    // the trail). Called once from SKSEPluginLoad.
    void Setup();
}
