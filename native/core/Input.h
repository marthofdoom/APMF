#pragma once

namespace apmf::input {
    // Register the DirectInput keyboard sink that routes hotkeys to the arbiter
    // and to the two diagnostic probes.
    //
    // CONFIG-GATED, DEFAULT OFF (CLAUDE.md "Probes must be FULLY PASSIVE: no
    // hotkeys, no toggles ... config-gated logging only, default OFF"). Nothing
    // here is armed in a shipped game unless the tester opts in with
    //
    //     [Input]
    //     EnableTestSurface=1
    //
    // in `Data/SKSE/Plugins/APMF.ini`. With the key absent (the shipped state) no
    // event sink is added at all, so NO keyboard scancode can reach a channel
    // claim, the native-bit toggle or the non-alias observe switch -- a stray
    // numpad press in a user's game cannot change framework behaviour.
    //
    // Test surface only (design.md Section 7: the real driver is the client API).
    void Register();

    // Log the hotkey help block (enumerates every registered channel's keys).
    // No-op unless Register() actually armed the surface.
    void LogHelp();
}
