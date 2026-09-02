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

    // Root-cause probe for the v0.2.2 hex-garbling (INVARIANTS #16). Binary
    // forensics on the garbling build proved the DLL itself is correct (single
    // fmt, RIP-relative read-only digit table, table bytes intact in the CI
    // artifact) and the field log proved fmt's 16-byte UPPERCASE hex digit
    // table read as stable foreign bytes at runtime while decimal/float/string
    // formatting stayed clean -- i.e. the mapped .rdata page was wrong IN THAT
    // SESSION, not the build. This probe decides between the two remaining
    // causes: (a) one-off corruption of that deployed file copy, or (b) an
    // in-process writer clobbering the page after load.
    //
    // Call at each phase ("load", "data-loaded", "post-load-game"). It
    //   1. formats sentinels through fmt's REAL hex path ({:016X}/{:x} via
    //      fmt::format -- same compiled do_format_base2e + table the old {:X}
    //      log sites used; not a log-call spec, so #16 is not violated) plus a
    //      {:.2f} float control, and logs PASS or the raw output bytes;
    //   2. on the first call, scans THIS module's .rdata for the two fmt digit
    //      tables and caches their addresses; every call re-reads and dumps the
    //      16 bytes at the cached addresses, so a later corruption shows up
    //      with the exact foreign bytes + RVA (which pins the writer).
    // If the FIRST call already fails, the file/image was bad before any other
    // mod ran -> (a). If "load" passes and a later phase fails -> (b).
    void HexSelfTest(const char* phase);
}
