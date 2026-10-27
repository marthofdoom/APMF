#pragma once

#include <cstdint>

// ============================================================================
// core/PapyrusCaller -- ClickUp 86e3q082a, "Attribute Papyrus calls to the owning plugin
// (rank scripts per mod, not one DirectCalls bucket)". STEP 1: THE SEAT + A PASSIVE PROBE.
//
// WHAT IT ANSWERS. Today every Papyrus equip that reaches the ch.17 sink ranks as ONE source,
// "DirectCalls" (75): the sink only sees return addresses, and a Papyrus native looks the same
// whichever script called it. An entry detour on EquipObject (FollowerEquipControl.dll, the SOS
// report of 2026-10-17) even hides that, so every caller reads External(<that dll>). The plan is
// to rank a script as the PLUGIN that owns it, so the user can rank a follower mod's scripts high
// and leave every other script low (marth 2026-10-13: "follower-mod scripts and console should
// rank high, but not every script").
//
// THE SEAT (#17: write_vfunc only). Every Papyrus native function object (the engine's template
// instantiations, skse64's, every CommonLib plugin's) is an IFunction whose vtable slot 0x0F is
// Call(this, const BSTSmartPointer<Stack>&, ErrorLogger*, VirtualMachine*, bool). Each such
// vtable, found from the VM's own type table, gets slot 0x0F wrapped (chaining: the thunk calls
// whatever the slot held). The thunk records "a Papyrus native is running on THIS thread" (the
// function, its stack, the VM) for the duration of the call and restores the previous record on
// return. It changes no argument and no result. Verified (1.6.1170 disassembly, agentlog
// apmf-papyrus-attribution-2.md): Actor.EquipItem's native (AE 54661) calls EquipObject
// synchronously, inside that Call; the engine's Call (AE 104651 / SE 97923 / 1.7.104 0x14A9C10)
// reads Stack+0x60 (top frame), frame+0x28 (self), Stack+0x80 (stack id).
//
// THE PROBE (principle 5: observe the path executing before anything ranks on it). At the ch.17
// sink, for an equip it already logs, ProbeEquip writes what the attribution WOULD be: the native,
// the calling script and function, the object the script is bound to (a form, an alias's quest,
// an active effect's magic effect) and its plugin files (the defining one and the winning
// override). It changes NO decision: the sink still ranks exactly as before.
//
// GATES. VR refused; exactly 1.6.1170 / 1.5.97 / 1.7.104 (allowance::RuntimeSupported); SeatVerified
// on row "PapyrusCaller.NativeFunctionBase.Call" (its signature runs through the layout reads above);
// APMF.ini [Probe] bPapyrusCaller (default 0 = no seat at all; a field test turns it on in its INI).
//
// THREADING. Init: SKSEPluginLoad. Install: MAIN thread (kDataLoaded, kPostLoadGame, kNewGame).
// The thunk runs on whatever thread the VM calls a native from (the main thread, or a VM thread for
// a native callable from tasklets); its state is thread_local. Describe/ProbeEquip: the thread the
// native runs on (the sink's thread), reading only that thread's own stack.
// ============================================================================

namespace apmf::papyruscaller {

    // SKSEPluginLoad: registers with SKSE's Papyrus interface only to learn the VM pointer (no
    // function is bound, no id is resolved).
    void Init();

    // MAIN thread. Seats every native-function vtable the VM's type table holds that is not seated
    // yet (idempotent; a later call seats only vtables seen for the first time). `why` names the
    // moment in the log ("kDataLoaded", "kPostLoadGame", "kNewGame").
    void Install(const char* a_why);

    // True once the seat is installed (any thread).
    bool Installed();

    // THE PROBE, at the ch.17 sink (core/EquipSink.cpp), for an equip the sink already attributes
    // and logs: `a_path` is the sink's own caller label (a path name or the external module),
    // `a_scriptPath` whether the sink classified it as a Papyrus path (AE 54661 / SE 53861 or
    // skse64), `a_rankedAs` the source name the sink ranks it as today. Logs, rate-limited, what
    // the Papyrus attribution would be. Never throws, never changes anything.
    void ProbeEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, bool a_scriptPath,
                    const char* a_rankedAs) noexcept;

}
