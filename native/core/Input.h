#pragma once

namespace apmf::input {
    // Register the DirectInput keyboard sink that routes hotkeys to the arbiter.
    // Test surface only (design.md Section 7: the real driver is the client API).
    void Register();

    // Log the hotkey help block (enumerates every registered channel's keys).
    void LogHelp();
}
