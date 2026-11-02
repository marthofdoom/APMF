#pragma once

#include "core/Sources.h"

#include <cstdint>

// ============================================================================
// core/PapyrusCaller -- ClickUp 86e3q082a, "Attribute Papyrus calls to the owning plugin
// (rank scripts per mod, not one DirectCalls bucket)". STEP 1: THE SEAT + A PASSIVE PROBE.
// STEP 2: THE RANKING (ScriptSource below): a Papyrus call that reaches a Harbinger seat ranks as
// the PLUGIN that owns the calling script, registered as its own source by file name.
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
// override), and since step 2 the owning file and the rule that found it. The probe line itself
// changes nothing (the ranking is ScriptSource's, below).
//
// STEP 2, THE OWNING FILE (marth 2026-10-27 ruling 2: "the file that OWNS THE SCRIPT, the plugin whose
// record version carries the live script list; if that cannot be determined, the defining plugin").
// The engine reads a record's VMAD into the bind policy under THAT FORM'S OWN handle (1.6.1170
// disassembly: a form's Load switches on 'VMAD' -> 14654(form, file) -> the handle policy's
// GetHandleForObject(formType, form) -> the bind policy's 104189(handle, reader)), and the live record
// of a form is its winning override, GetFile(-1). So the owner is the winning file of the form whose
// record carries the script:
//   an alias's script    -> its quest (the quest's VMAD holds the alias scripts)  -> quest GetFile(-1)
//   an effect's script   -> its magic effect                                      -> MGEF GetFile(-1)
//   any other non-reference form (a quest, ...)                                   -> GetFile(-1)
//   a reference          -> its own record OR its base object's (a script on an NPC_ base is bound to
//                           every reference of it). A created reference (no file) -> the base's
//                           GetFile(-1). A placed reference whose winning file EQUALS its base's -> that
//                           file. Otherwise the two records are both candidates and which one lists the
//                           script is not readable without the bind policy's private parsed VMAD (no
//                           verified layout), so it is UNDETERMINED: the ruling's fallback, the
//                           reference's defining plugin GetFile(0), logged as "ambiguous".
//
// [Harbinger] iPapyrusOwnerFile (marth 2026-11-01, "both, toggle between"): 0 (default) = the rule
// above, the winning override. 1 = the owner form's DEFINING plugin, GetFile(0) (an alias's quest, an
// effect's MGEF, the form itself, a created reference's base). Every attributed line names both files.
// SAFETY (review 2026-11-01): a form owner is read under the all-forms read lock, a reference also through
// a counted reference. An ActiveEffect (not refcounted, dispelled on the main thread) is read only on the
// main thread: off it, an effect script is not attributed.
//
// STEP 2, WHEN (APMF-B210, "the native must be the one that acts"). An attribution is used only
// when the running native is the one that reached the seat: on the two Equip facets, the native is
// Actor.EquipItem / EquipItemEx / EquipItemById (an equip made while an unrelated native runs on that
// thread, e.g. AddItem firing a container-changed sink in a C++ mod that equips, is NOT blamed on
// that script); on every other facet, the running native's vtable lives in the SAME module as the
// seat's recovered caller (the skse64 rows: an skse64 native is running, and skse64 code called).
// Console commands are not natives (no record): they stay DirectCalls. A native with no script frame
// below it (a DLL dispatching it), an unbound / static caller, or an owner with no file stays
// DirectCalls too.
//
// GATES. VR refused; exactly 1.6.1170 / 1.5.97 / 1.7.104 (allowance::RuntimeSupported); SeatVerified
// on row "PapyrusCaller.NativeFunctionBase.Call" (its signature runs through the layout reads above).
// APMF.ini [Harbinger] bPapyrusAttribution (default 1): the seat and the ranking. [Probe]
// bPapyrusCaller (default 0): the per-equip PROBE line. The seat installs when either is 1; the
// ranking needs bPapyrusAttribution=1. Both 0 = no vtable written, Papyrus ranks as DirectCalls.
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

    // STEP 2. The source a Papyrus call running on THIS thread ranks as on `a_facet`: the plugin that
    // owns the calling script (Sources::Register(<file>, kPlugin): one row per plugin, the
    // unregistered default priority, 20, until APMF_Sources.ini or the menu says otherwise), or
    // kNoSource when the call is not attributable (seat off, no native running, the running native is
    // not the one that acts on `a_facet` -- see the header --, no script frame, no owner file). The
    // caller keeps its own source then (DirectCalls). `a_callerAddr` is the seat's recovered caller
    // (a return address), used by the same-module rule on the non-Equip facets. Logs one line per
    // (facet, plugin) at most every 10 s when it ranks a call. Any thread; reads only this thread's
    // own stack; never throws.
    // On the two Equip facets the equip must also BE the native's own request (review 2026-11-01): `a_actor` /
    // `a_item` (the equip's actor and item FormIDs) must equal the native frame's self and its argument 0;
    // anything else (an equip a DLL's TESEquipEvent sink makes inside it) is not attributed. Unused (0) on
    // the other facets.
    apmf::sources::SourceId ScriptSource(apmf::sources::Facet a_facet, std::uintptr_t a_callerAddr,
                                         RE::FormID a_actor = 0, RE::FormID a_item = 0) noexcept;

    // THE PROBE, at the ch.17 sink (core/EquipSink.cpp), for an equip the sink already attributes
    // and logs: `a_path` is the sink's own caller label (a path name or the external module),
    // `a_scriptPath` whether the sink classified it as a Papyrus path (AE 54661 / SE 53861 or
    // skse64), `a_rankedAs` the source name the sink ranks it as today. Logs, rate-limited, what
    // the Papyrus attribution is (owner, rule). [Probe] bPapyrusCaller only. Never throws, never
    // changes anything.
    void ProbeEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, bool a_scriptPath,
                    const char* a_rankedAs) noexcept;

}
