#pragma once

namespace apmf::log {
    // Set up spdlog -> Data/SKSE/Plugins/APMF.log (flush-every-line so a CTD keeps
    // the trail). Called once from SKSEPluginLoad.
    void Setup();

    // Format `value` as uppercase ASCII hex, zero-padded to at least `width` digits.
    // Built BY HAND (no `{:X}` presentation spec) and logged via the string path
    // (`{}`), so it is IMMUNE to the hex-presentation formatting bug seen on the deck
    // (v0.2.2): APMF's build rendered every `{:08X}` as raw garbage bytes and
    // corrupted the log to binary, while decimal `{}` and string `{}` rendered clean
    // -- the identical toolchain formats `{:X}` fine for MFO, so the trigger is
    // APMF-build-specific and not statically isolable. This helper sidesteps it: an
    // ASCII-hex std::string logged as a string always renders correctly.
    //   Use: spdlog::info("... 0x{} ...", apmf::log::Hex(id));
    std::string Hex(std::uint64_t value, int width = 8);
}
