#include "PCH.h"
#include "channels/Idle.h"
#include "core/Allowance.h"       // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/CombatEntry.cpp and channels/Travel.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 12 -- IDLE / ANIMATION. SANCTIONED BOUNDED ONE-SHOT PROMOTE (INVARIANTS
// #0(c), CHANNEL-MAP ch.12): play an animation. This facet has no meaningful deny
// form (the AI's own idle manager is not gated) and no AI decision to arbitrate
// around -- "play this idle" is the requested action itself, not a selection input a
// combat/dialogue AI later decides on, so a single deterministic call at Engage is
// lawful under #0(c), not a stand-in awaiting conversion.
//
// v1 (param.form == 0, every ABI): the FORM-FREE graph path, one
// NotifyAnimationGraph("IdleForceDefaultState") at Engage. Unchanged in behaviour: no
// gate, no Release action, no owner-change action. param.target is not read.
//
// v2 (ABI v17, param.form != 0): "play THIS idle AT this target".
//   param.form   = a TESIdleForm (an IDLE record), REQUIRED for v2;
//   param.target = an optional reference the idle is played at (0 = none: the engine
//                  then uses the actor's own process target, AE 0x6DDF94).
//   ONE call at Engage (and one per owner change / Repoint, each a new declaration):
//   AIProcess::PlayIdle(actor, idle, target). ONE call at Release, and only when the
//   idle is still HELD: NotifyAnimationGraph("IdleForceDefaultState"). No Tick.
//
// THE ENGINE CALL (disassembly of both unpacked images, 2026-09-25; agent log
// apmf-idle-v2.md has the listings). The fork's AIProcess::PlayIdle is
// SetupSpecialIdle(this, actor, DEFAULT_OBJECT::kActionIdle = 64, idle, true, false,
// target), RELOCATION_ID(38290, 39256) = SE 0x64B140 / AE 0x6DDE70 (real bodies, no
// thunk). The engine's own Papyrus Actor.PlayIdle native (AE id 55105 / SE id 54395,
// "Cannot play a None idle on an actor") makes exactly that call on both runtimes:
// process from [actor+0xF8] AE / [actor+0xF0] SE, r8d = 0x40, arg5 = 1, arg6 = 0,
// arg7 = target. What the body does with it:
//   * returns false at once when process->high ([rcx+0x10]) is null -- so the actor
//     must be in high process (checked here before the call, and named);
//   * arg5 = true: the idle's OWN conditions are evaluated against (actor, target)
//     (idle+0x20, AE call 29888); a failing condition returns false. Vanilla
//     IdleActivatePickUpLow carries IsCarryable == 1 on the TARGET, so it is refused
//     against a container. IdleLockPick and IdleGive carry no conditions;
//   * the action is then PERFORMED (AE id 39625), which may return true because the
//     idle was QUEUED on the process, not because the graph already changed state. So
//     "PlayIdle returned true" means ACCEPTED, and this channel confirms the animation
//     separately from the graph's own events (below).
//
// HELD, AND THE RELEASE RESET. The IDLE record carries no usable loop flag (DATA
// loopMin / loopMax are 0 on IdleLockPick, IdleGive, IdleActivatePickUpLow and
// IdleForceDefaultState in Skyrim.esm) and the engine has no "current idle" query we
// can trust (the IsIdlePlaying condition function, AE id 21514, is a stub that always
// answers 0; middleHigh->lastIdlePlayed is the LAST idle, not the current one; and
// high->currentProcessIdle is CLEARED by a successful SetupSpecialIdle). What the
// engine DOES say is in the behaviour graph: a one-shot idle clip ends by raising the
// graph event "IdleStop" (mt_behavior: MT_LockPick is MODE_SINGLE_PLAY with an IdleStop
// trigger at the end of the clip, about 5.6 s; InteractionObjectState, the state the
// interaction idles play in, raises IdleStop on exit and returns to MT_Default_State on
// it). A looping idle raises nothing until something ends it. So:
//     HELD = PlayIdle accepted the idle AND the graph was seen ENTERING it (below) AND
//            the graph has not raised "IdleStop" since that entry.
// (Field 2026-09-25..28: IdleStop does reach this sink; every IdleGive release listed it.)
//
// ENTRY CONFIRMATION (field 2026-09-28: an IdleLockPick that PlayIdle accepted and the old
// code called "ANIMATION CONFIRMED" never played; ANY graph event used to count, and the
// events were the combat graph's own). "PlayIdle returned true" only means the engine
// accepted or QUEUED the action. The idle is CONFIRMED only when the actor's graph raises
// one of the idle's ENTRY EVENTS after the call: the notify events a transition on the
// idle's own animation event raises when it fires. They are read from the actor's live
// behaviour graph at the call (ResolveEntry, below), for any idle any client names:
//   for every state machine transition whose event is the idle's animEventName: the
//   destination state's enterNotifyEvents, the source state's exitNotifyEvents (not for a
//   wildcard), and the enterNotifyEvents of the start state of a state machine nested in
//   the destination. Vanilla + Tuxborn's Nemesis graphs (offline, same walk):
//   IdleLockPick -> MT_BehaviorGraph>InteractionObjectState, {IdleOffsetStop, OffsetStop};
//   IdleGive -> MT_RootBehavior MT_State>NonOffsetIdles, {IdleOffsetStop} (MT_State exit).
//   Every field IdleGive play listed IdleOffsetStop first; the lockpick listed neither.
// Outcomes, one line per play from Poll:
//   CONFIRMED      an entry event arrived (named, with its time);
//   NOT CONFIRMED  none arrived within kConfirmWaitMs -> WARN and Harbinger ENDS the claim
//                  (IsClaimLive goes false: the client sees the idle did not play);
//   NO TRANSITION  a complete walk found no transition on the idle's event -> the idle
//                  cannot play on this actor's graph -> WARN and the claim ENDS at the call;
//   UNCONFIRMABLE  no entry event could be resolved (the destination raises none, the walk
//                  was incomplete, or the graph could not be read) -> WARN, the claim stays,
//                  and HELD falls back to "no IdleStop since the call" (the pre-fix rule).
// An IdleStop that arrives BEFORE any entry event is not this idle's (the lockpick's came
// at +2961 ms from the combat graph) and is reported as such, never as the idle's end.
// The clip's length is NOT readable here (the template clip carries only an end-relative
// IdleStop trigger; the duration lives in the runtime animation binding) and the ABI has
// no field for a client to declare it, so the entry->IdleStop time is LOGGED for the
// reader to compare with the clip instead of being judged.
// THE GRAPH READ (GAME THREAD, once per play, read-only): BShkbAnimationGraph and
// hkbBehaviorGraph are identified by their SELF-CHECKED vtables (the rows Travel's gate
// probe 2 already verifies on 1.6.1170 and 1.5.97) and BShkbAnimationGraph.behaviorGraph
// (0x208) is read only when it equals characterInstance(0xC0).behaviorGraph (0x58), the
// probe-2 guard. Every other Havok object is identified by its MSVC RTTI name before a
// member is read. Only the behaviour TEMPLATES are walked (immutable after load; the
// manager is held by a smart pointer). Member offsets are the Havok 2010 x64 in-place
// packfile layout, read off the fixups of the shipped behaviour files; the reference
// generator's linked graph at +0x50 is what its own getChildren returns (AE 0xACCD40,
// SE 0xA0B170). Bounded by kWalkMaxNodes.
// FURNITURE GUARD (review F1): the reset is NEVER sent while the actor's sit/sleep state is
// not kNormal (sitting, sleeping, entering or leaving furniture): IdleForceDefaultState
// there would pop the actor out of the furniture pose. A false "held" is harmless only
// because of this guard plus the loaded / alive checks.
// A per-actor BSAnimationGraphEvent sink (the core/CastObserve.cpp precedent, whose
// clip-trigger events were captured on the deck 2026-09-04) records it. Release sends
// IdleForceDefaultState only when HELD, the actor is loaded and alive; otherwise it
// sends nothing and says why. The same sink records the entry evidence above.
//
// CROUCH (for clients; nothing here writes sneak). In mt_behavior the sneak locomotion
// lives INSIDE MT_Default_State, and IdleLockPick is an unconditional local wildcard on
// the root state machine to InteractionObjectState -> MT_LockPick, one clip, no sneak
// variant. So an idle played from a crouch leaves the sneak locomotion, plays the
// STANDING clip, and returns to MT_Default_State on IdleStop, where the sneak locomotion
// resumes (sneaking is actor state, not graph state). There is no kneeling lockpick in
// vanilla; a client that wants one needs a kneeling IDLE of its own.
//
// RELEASE AND END. The claim ENDS -- Harbinger releases it and logs the reason, repeated
// on the Release line -- when: the idle could not be played (the form is not an IDLE,
// the target is not a loaded reference, the actor is not loaded / dead / has no AI
// process or no high process data), the engine refused it (PlayIdle returned false),
// the actor's graph has no transition on the idle's event, the idle was NOT CONFIRMED
// (above), or the owner died (Poll). An unload is the ControlMap's own sweep (it releases every
// channel of an unloaded actor); a save load / revert / new game drops every claim
// (ControlMap::Clear) and ResetAll drops the entries. A client that wants another
// idle sends a Repoint or a new request.
//
// THREADING. Engage / OnOwnerChanged / Release run inside ControlMap::Drain on the
// confirmed-main seat. The play task runs from mainthread::Pump right after Drain
// PUBLISHES (it re-reads the published claim, the ch.21 precedent). Anything that ENDS a
// claim (EnqueueRelease) runs from Pump or Poll, never inside Drain. g_entries and
// g_sinks are touched on that one thread only. The anim sink's ProcessEvent runs on
// whatever thread updates the actor's graph, so its record (g_obs) is behind g_obsMx
// and holds only plain values; the sink makes no engine call.
//
// VERSION ROBUSTNESS. The only engine address called is SetupSpecialIdle's, a verified row
// on both runtimes (spec.json "Idle.AIProcess.SetupSpecialIdle"), refused unless it
// verifies; exact 1.6.1170 / 1.5.97 only; VR refused by name. The entry read uses three
// more EXISTING rows (vtables BShkbAnimationGraph / hkbBehaviorGraph / hkbStateMachine,
// both runtimes) and the RTTI names, which exist on both (checked in both unpacked
// images); if a row fails, confirmation is NOT ARMED (said once) and every play is
// UNCONFIRMABLE -- the play itself is unaffected. Raw reads: the actor's currentProcess
// and high pointer (fork accessors) and the Havok template members listed above.
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char*   kIni           = "Data/SKSE/Plugins/APMF.ini";
    constexpr std::uint64_t kConfirmWaitMs = 3000;   // no ENTRY event this long after an accepted play = NOT CONFIRMED
    constexpr std::size_t   kFirstTags     = 8;      // graph events kept for the confirmation line
    constexpr std::uint32_t kWalkMaxNodes  = 60000;  // vanilla + Tuxborn's Nemesis graphs: ~6000 nodes in 17 graphs

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the v2 path is gated there)" };

    bool IEquals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    }

    // ---- ENTRY RESOLUTION (GAME THREAD; see the header, "ENTRY CONFIRMATION"). ----
    // The three self-checked vtables, written once in Install before g_entryReady.
    std::atomic<bool>        g_entryReady{ false };
    std::atomic<const char*> g_entryNotArmed{ "before kDataLoaded" };
    std::uintptr_t           g_vtAnimGraph     = 0;
    std::uintptr_t           g_vtBehaviorGraph = 0;
    std::uintptr_t           g_vtStateMachine  = 0;

    template <class T>
    T Rd(std::uintptr_t a_addr) {
        return *reinterpret_cast<const T*>(a_addr);
    }

    enum class Kind : std::uint8_t {
        kOther,
        kStateMachine,
        kStateInfo,
        kTransitionArray,
        kEventPropertyArray,
        kModifierGenerator,
        kBlenderGenerator,   // also hkbPoseMatchingGenerator (derives from it; same children)
        kBlenderChild,
        kManualSelector,
        kBoneSwitch,
        kBoneSwitchData,
        kCyclicBlend,
        kOffsetAnimation,
        kStateTagging,
        kSyncClip,
        kBehaviorReference,
        kBehaviorGraph,
        kGraphData,
        kStringData,
        kClip,
    };

    // MSVC RTTI name -> kind. Every name checked present in both unpacked images.
    Kind KindFromName(std::string_view n) {
        static constexpr std::pair<std::string_view, Kind> kNames[] = {
            { ".?AVhkbStateMachine@@", Kind::kStateMachine },
            { ".?AVStateInfo@hkbStateMachine@@", Kind::kStateInfo },
            { ".?AVTransitionInfoArray@hkbStateMachine@@", Kind::kTransitionArray },
            { ".?AVEventPropertyArray@hkbStateMachine@@", Kind::kEventPropertyArray },
            { ".?AVhkbModifierGenerator@@", Kind::kModifierGenerator },
            { ".?AVhkbBlenderGenerator@@", Kind::kBlenderGenerator },
            { ".?AVhkbPoseMatchingGenerator@@", Kind::kBlenderGenerator },
            { ".?AVhkbBlenderGeneratorChild@@", Kind::kBlenderChild },
            { ".?AVhkbManualSelectorGenerator@@", Kind::kManualSelector },
            { ".?AVBSBoneSwitchGenerator@@", Kind::kBoneSwitch },
            { ".?AVBSBoneSwitchGeneratorBoneData@@", Kind::kBoneSwitchData },
            { ".?AVBSCyclicBlendTransitionGenerator@@", Kind::kCyclicBlend },
            { ".?AVBSOffsetAnimationGenerator@@", Kind::kOffsetAnimation },
            { ".?AVBSiStateTaggingGenerator@@", Kind::kStateTagging },
            { ".?AVBSSynchronizedClipGenerator@@", Kind::kSyncClip },
            { ".?AVhkbBehaviorReferenceGenerator@@", Kind::kBehaviorReference },
            { ".?AVhkbBehaviorGraph@@", Kind::kBehaviorGraph },
            { ".?AVhkbBehaviorGraphData@@", Kind::kGraphData },
            { ".?AVhkbBehaviorGraphStringData@@", Kind::kStringData },
            { ".?AVhkbClipGenerator@@", Kind::kClip },
        };
        for (const auto& [name, k] : kNames)
            if (name == n) return k;
        return Kind::kOther;
    }

    bool InSeg(std::uintptr_t a_p, const REL::Segment& a_s, std::size_t a_len) {
        return a_p >= a_s.address() && a_p + a_len <= a_s.address() + a_s.size();
    }

    // The object's class, from its vtable's CompleteObjectLocator (vtable - 8) -> type
    // descriptor name. The vtable, the locator and the descriptor must each lie in the game
    // image (.rdata / .rdata / .data) or the object is kOther and nothing more of it is read.
    // Cached per vtable (GAME THREAD only).
    std::unordered_map<std::uintptr_t, Kind> g_kinds;
    Kind KindOf(std::uintptr_t a_obj) {
        const auto vt = Rd<std::uintptr_t>(a_obj);
        if (const auto it = g_kinds.find(vt); it != g_kinds.end()) return it->second;
        Kind       k     = Kind::kOther;
        const auto& mod  = REL::Module::get();
        const auto rdata = mod.segment(REL::Segment::rdata);
        const auto data  = mod.segment(REL::Segment::data);
        if (vt >= sizeof(void*) && InSeg(vt - sizeof(void*), rdata, sizeof(void*))) {
            const auto col = Rd<std::uintptr_t>(vt - sizeof(void*));
            if (InSeg(col, rdata, sizeof(RE::RTTI::CompleteObjectLocator)) && Rd<std::uint32_t>(col) == 1) {
                const auto td = mod.base() + Rd<std::uint32_t>(col + 0x0C);   // COL.typeDescriptor (RVA)
                if (InSeg(td, data, 0x10 + 4)) {
                    const char* name = reinterpret_cast<const char*>(td + 0x10);   // type_info::_name
                    k                = KindFromName(std::string_view(name, strnlen(name, 96)));
                }
            }
        }
        // The two classes with a self-checked vtable must match it too (the RTTI walk and the
        // verified row agree, or the object is not trusted).
        if ((k == Kind::kStateMachine && vt != g_vtStateMachine) || (k == Kind::kBehaviorGraph && vt != g_vtBehaviorGraph))
            k = Kind::kOther;
        g_kinds.emplace(vt, k);
        return k;
    }

    bool Is(std::uintptr_t a_obj, Kind a_k) { return a_obj && KindOf(a_obj) == a_k; }

    // hkStringPtr: the char* with the "owned" flag in bit 0.
    const char* HkStr(std::uintptr_t a_field) {
        return reinterpret_cast<const char*>(Rd<std::uintptr_t>(a_field) & ~std::uintptr_t{ 1 });
    }

    struct EntryInfo {
        std::vector<std::string> events;        // the idle's entry events (deduplicated, case-insensitive)
        std::string              dests;         // "Machine>State" for the log
        std::uint32_t            transitions = 0;
        std::uint32_t            graphs = 0;
        std::uint32_t            nodes = 0;
        std::uint32_t            unknown = 0;   // generator nodes of a class the walk does not descend
        std::uint32_t            unlinked = 0;  // behaviour references with no linked graph
        bool                     truncated = false;
        std::string              unreadable;    // set when no walk happened, with the reason
        bool Complete() const { return unreadable.empty() && unknown == 0 && unlinked == 0 && !truncated && graphs != 0; }
    };

    void AddEvent(EntryInfo& a_out, const char* a_name) {
        if (!a_name || !*a_name) return;
        for (const auto& e : a_out.events)
            if (IEquals(e, a_name)) return;
        a_out.events.emplace_back(a_name);
    }

    // Walks the TEMPLATES of one actor graph. Offsets: Havok 2010 x64 in-place layout (header).
    class EntryWalk {
    public:
        EntryWalk(std::string_view a_event, EntryInfo& a_out) : event(a_event), out(a_out) {}

        void Graph(std::uintptr_t a_bg) {
            if (!a_bg || Rd<std::uintptr_t>(a_bg) != g_vtBehaviorGraph || !graphsSeen.insert(a_bg).second) return;
            ++out.graphs;
            // hkbBehaviorGraph.data 0x88 -> hkbBehaviorGraphData.stringData 0x78 -> eventNames hkArray 0x10.
            const auto data = Rd<std::uintptr_t>(a_bg + 0x88);
            const auto sd   = Is(data, Kind::kGraphData) ? Rd<std::uintptr_t>(data + 0x78) : 0;
            if (!Is(sd, Kind::kStringData)) return;
            Names names{ Rd<std::uintptr_t>(sd + 0x10), Rd<std::int32_t>(sd + 0x18) };
            std::vector<std::uintptr_t> stack{ Rd<std::uintptr_t>(a_bg + 0x80) };   // rootGenerator (template)
            while (!stack.empty()) {
                const auto o = stack.back();
                stack.pop_back();
                if (!o || !seen.insert(o).second) continue;
                if (++out.nodes > kWalkMaxNodes) {
                    out.truncated = true;
                    return;
                }
                switch (KindOf(o)) {
                case Kind::kStateMachine:
                    Machine(o, names, stack);
                    break;
                case Kind::kModifierGenerator:
                case Kind::kCyclicBlend:
                case Kind::kStateTagging:
                case Kind::kSyncClip:
                    stack.push_back(Rd<std::uintptr_t>(o + 0x50));
                    break;
                case Kind::kOffsetAnimation:
                    stack.push_back(Rd<std::uintptr_t>(o + 0x50));
                    stack.push_back(Rd<std::uintptr_t>(o + 0x60));
                    break;
                case Kind::kBlenderGenerator:
                    Children(o + 0x60, Kind::kBlenderChild, 0x30, stack);
                    break;
                case Kind::kManualSelector:
                    Children(o + 0x48, Kind::kOther, 0, stack);
                    break;
                case Kind::kBoneSwitch:
                    stack.push_back(Rd<std::uintptr_t>(o + 0x50));
                    Children(o + 0x58, Kind::kBoneSwitchData, 0x30, stack);
                    break;
                case Kind::kBehaviorReference:
                    if (const auto linked = Rd<std::uintptr_t>(o + 0x50); linked)
                        pending.push_back(linked);
                    else
                        ++out.unlinked;
                    break;
                case Kind::kClip:
                    break;
                default:
                    ++out.unknown;
                    break;
                }
            }
        }

        void Drain() {
            while (!pending.empty() && !out.truncated) {
                const auto bg = pending.back();
                pending.pop_back();
                Graph(bg);
            }
        }

    private:
        struct Names {
            std::uintptr_t data;
            std::int32_t   size;
            const char*    Get(std::int32_t i) const { return (i >= 0 && i < size) ? HkStr(data + 8 * static_cast<std::uintptr_t>(i)) : nullptr; }
        };

        // hkArray at a_arr (data 0x0, size 0x8) of object pointers. a_wrap != kOther: each element
        // is a wrapper of that kind whose generator sits at +a_genOff.
        void Children(std::uintptr_t a_arr, Kind a_wrap, std::uintptr_t a_genOff, std::vector<std::uintptr_t>& a_stack) {
            const auto p = Rd<std::uintptr_t>(a_arr);
            const auto n = Rd<std::int32_t>(a_arr + 8);
            if (!p || n <= 0) return;
            for (std::int32_t i = 0; i < n; ++i) {
                const auto x = Rd<std::uintptr_t>(p + 8 * static_cast<std::uintptr_t>(i));
                if (a_wrap == Kind::kOther)
                    a_stack.push_back(x);
                else if (Is(x, a_wrap))
                    a_stack.push_back(Rd<std::uintptr_t>(x + a_genOff));
            }
        }

        void Notify(std::uintptr_t a_arr, const Names& a_names) {   // an EventPropertyArray -> names
            if (!Is(a_arr, Kind::kEventPropertyArray)) return;
            const auto p = Rd<std::uintptr_t>(a_arr + 0x10);
            const auto n = Rd<std::int32_t>(a_arr + 0x18);
            for (std::int32_t i = 0; p && i < n; ++i)   // hkbEventProperty: id 0x0, payload 0x8; 0x10 each
                AddEvent(out, a_names.Get(Rd<std::int32_t>(p + 0x10 * static_cast<std::uintptr_t>(i))));
        }

        std::uintptr_t StateById(std::uintptr_t a_sm, std::int32_t a_id) {
            const auto p = Rd<std::uintptr_t>(a_sm + 0x90);
            const auto n = Rd<std::int32_t>(a_sm + 0x98);
            for (std::int32_t i = 0; p && i < n; ++i) {
                const auto s = Rd<std::uintptr_t>(p + 8 * static_cast<std::uintptr_t>(i));
                if (Is(s, Kind::kStateInfo) && Rd<std::int32_t>(s + 0x68) == a_id) return s;
            }
            return 0;
        }

        // The start state of a state machine nested (through single-child wrappers) in a_gen.
        void NestedStart(std::uintptr_t a_gen, const Names& a_names) {
            for (int depth = 0; a_gen && depth < 8; ++depth) {
                const Kind k = KindOf(a_gen);
                if (k == Kind::kStateMachine) {
                    const auto s = StateById(a_gen, Rd<std::int32_t>(a_gen + 0x68));   // startStateId
                    if (!s) return;
                    Notify(Rd<std::uintptr_t>(s + 0x40), a_names);
                    a_gen = Rd<std::uintptr_t>(s + 0x58);
                } else if (k == Kind::kModifierGenerator || k == Kind::kStateTagging || k == Kind::kCyclicBlend) {
                    a_gen = Rd<std::uintptr_t>(a_gen + 0x50);
                } else {
                    return;
                }
            }
        }

        // hkbStateMachine.TransitionInfoArray: hkArray 0x10 of 0x48-byte TransitionInfo
        // (eventId 0x30, toStateId 0x34).
        void Transitions(std::uintptr_t a_sm, std::uintptr_t a_arr, std::uintptr_t a_from, const Names& a_names) {
            if (!Is(a_arr, Kind::kTransitionArray)) return;
            const auto p = Rd<std::uintptr_t>(a_arr + 0x10);
            const auto n = Rd<std::int32_t>(a_arr + 0x18);
            for (std::int32_t i = 0; p && i < n; ++i) {
                const auto  t  = p + 0x48 * static_cast<std::uintptr_t>(i);
                const char* ev = a_names.Get(Rd<std::int32_t>(t + 0x30));
                if (!ev || !IEquals(ev, event)) continue;
                const auto dest = StateById(a_sm, Rd<std::int32_t>(t + 0x34));
                if (!dest) continue;
                ++out.transitions;
                Notify(Rd<std::uintptr_t>(dest + 0x40), a_names);                // dest enterNotifyEvents
                if (a_from) Notify(Rd<std::uintptr_t>(a_from + 0x48), a_names);   // source exitNotifyEvents
                NestedStart(Rd<std::uintptr_t>(dest + 0x58), a_names);
                if (out.dests.size() < 200) {
                    const char* smn = HkStr(a_sm + 0x38);
                    const char* stn = HkStr(dest + 0x60);
                    out.dests += fmt::format("{}{}>{}", out.dests.empty() ? "" : ", ", smn ? smn : "?", stn ? stn : "?");
                }
            }
        }

        // hkbStateMachine: states hkArray<StateInfo*> 0x90, wildcardTransitions 0xA0.
        // StateInfo: enter 0x40, exit 0x48, transitions 0x50, generator 0x58, name 0x60, id 0x68.
        void Machine(std::uintptr_t a_sm, const Names& a_names, std::vector<std::uintptr_t>& a_stack) {
            Transitions(a_sm, Rd<std::uintptr_t>(a_sm + 0xA0), 0, a_names);
            const auto p = Rd<std::uintptr_t>(a_sm + 0x90);
            const auto n = Rd<std::int32_t>(a_sm + 0x98);
            for (std::int32_t i = 0; p && i < n; ++i) {
                const auto s = Rd<std::uintptr_t>(p + 8 * static_cast<std::uintptr_t>(i));
                if (!Is(s, Kind::kStateInfo)) continue;
                a_stack.push_back(Rd<std::uintptr_t>(s + 0x58));
                Transitions(a_sm, Rd<std::uintptr_t>(s + 0x50), s, a_names);
            }
        }

        std::string_view                   event;
        EntryInfo&                         out;
        std::unordered_set<std::uintptr_t> seen;
        std::unordered_set<std::uintptr_t> graphsSeen;
        std::vector<std::uintptr_t>        pending;
    };

    // GAME THREAD, before the PlayIdle call. Never throws; a graph it cannot read says why.
    EntryInfo ResolveEntry(RE::Actor* a_actor, const char* a_event) {
        EntryInfo out;
        if (!g_entryReady.load(std::memory_order_acquire)) {
            out.unreadable = fmt::format("entry confirmation not armed ({})", g_entryNotArmed.load(std::memory_order_acquire));
            return out;
        }
        if (!a_event || !*a_event) {
            out.unreadable = "the idle names no animation event";
            return out;
        }
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;   // held for the whole walk
        if (!a_actor->GetAnimationGraphManager(mgr) || !mgr) {
            out.unreadable = "the actor has no animation graph";
            return out;
        }
        EntryWalk walk(a_event, out);
        for (std::uint32_t i = 0; i < mgr->graphs.size() && i < 2; ++i) {
            const auto g = reinterpret_cast<std::uintptr_t>(mgr->graphs[i].get());
            if (!g || Rd<std::uintptr_t>(g) != g_vtAnimGraph) continue;
            const auto bg = Rd<std::uintptr_t>(g + 0x208);
            if (!bg || bg != Rd<std::uintptr_t>(g + 0xC0 + 0x58)) continue;   // the probe-2 layout guard
            walk.Graph(bg);
            walk.Drain();
        }
        if (out.graphs == 0) out.unreadable = "no behaviour graph passed the layout / vtable checks";
        return out;
    }

    std::string EntryText(const EntryInfo& a_e) {
        std::string ev;
        for (const auto& e : a_e.events) ev += (ev.empty() ? "" : ", ") + e;
        return fmt::format("entry events [{}] from {} transition(s){}{}; walked {} graph(s), {} node(s){}{}{}",
                           ev.empty() ? std::string("none") : ev, a_e.transitions, a_e.dests.empty() ? "" : " to ", a_e.dests,
                           a_e.graphs, a_e.nodes, a_e.unknown ? fmt::format(", {} of an unwalked class", a_e.unknown) : "",
                           a_e.unlinked ? fmt::format(", {} unlinked reference(s)", a_e.unlinked) : "",
                           a_e.truncated ? ", TRUNCATED" : "");
    }

    // ---- The anim-graph observation (ANY THREAD for the sink, under g_obsMx). ----
    struct Obs {
        bool                     armed = false;
        std::uint32_t            gen = 0;        // the play this record belongs to
        std::uint64_t            playMs = 0;
        std::vector<std::string> entry;          // the idle's entry events (empty = unconfirmable)
        bool                     entered = false;
        std::uint64_t            enteredMs = 0;
        std::string              enterTag;
        bool                     idleStop = false;   // IdleStop after the entry (or, unconfirmable: any IdleStop)
        std::uint64_t            idleStopMs = 0;
        bool                     strayStop = false;  // an IdleStop BEFORE any entry event: not this idle's
        std::uint64_t            strayStopMs = 0;
        std::uint32_t            events = 0;
        std::vector<std::string> first;          // the first kFirstTags tags after the call
    };
    std::mutex                          g_obsMx;
    std::unordered_map<RE::FormID, Obs> g_obs;

    class IdleAnimSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
    public:
        explicit IdleAnimSink(RE::FormID a_fid) : fid(a_fid) {}

        // PASSIVE: record, never consume, never touch the engine. BSFixedString pools are
        // case-insensitive (the first-interned casing wins), so the tag is compared that way.
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            const char*            tagC = a_event->tag.c_str();
            const std::string_view tag  = tagC ? std::string_view(tagC) : std::string_view{};
            if (tag.empty()) return RE::BSEventNotifyControl::kContinue;
            std::scoped_lock lock(g_obsMx);
            const auto it = g_obs.find(fid);
            if (it == g_obs.end() || !it->second.armed) return RE::BSEventNotifyControl::kContinue;
            Obs& o = it->second;
            ++o.events;
            if (o.first.size() < kFirstTags) o.first.emplace_back(tag);
            bool entryNow = false;
            if (!o.entered) {
                for (const auto& e : o.entry) {
                    if (IEquals(tag, e)) {
                        o.entered   = true;
                        o.enteredMs = apmf::clock::MonotonicMs();
                        o.enterTag.assign(tag);
                        entryNow = true;
                        break;
                    }
                }
            }
            // An IdleStop is the idle's end only AFTER its entry (and not the very event that
            // proved the entry, e.g. an IdleStop that is a source state's exit notify). Before
            // any entry it belongs to something else. With no entry events to wait for
            // (unconfirmable) any IdleStop counts, the pre-fix rule.
            if (!entryNow && IEquals(tag, "IdleStop")) {
                if (o.entry.empty() || o.entered) {
                    if (!o.idleStop) {
                        o.idleStop   = true;
                        o.idleStopMs = apmf::clock::MonotonicMs();
                    }
                } else if (!o.strayStop) {
                    o.strayStop   = true;
                    o.strayStopMs = apmf::clock::MonotonicMs();
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::FormID fid;
    };

    // One sink per actor ever claimed with a form, kept alive for the session (GAME THREAD
    // ONLY). A registration left on a graph after its claim is harmless: the sink records
    // only while its Obs is armed.
    std::unordered_map<RE::FormID, std::unique_ptr<IdleAnimSink>> g_sinks;

    IdleAnimSink* SinkFor(RE::FormID id) {
        auto& s = g_sinks[id];
        if (!s) s = std::make_unique<IdleAnimSink>(id);
        return s.get();
    }

    void Arm(RE::FormID id, std::uint32_t gen, const std::vector<std::string>& entry) {
        std::scoped_lock lock(g_obsMx);
        Obs o{};
        o.armed  = true;
        o.gen    = gen;
        o.playMs = apmf::clock::MonotonicMs();
        o.entry  = entry;
        g_obs.insert_or_assign(id, std::move(o));
    }

    Obs Disarm(RE::FormID id) {
        std::scoped_lock lock(g_obsMx);
        const auto it = g_obs.find(id);
        if (it == g_obs.end()) return {};
        Obs o = std::move(it->second);
        g_obs.erase(it);
        return o;
    }

    Obs Peek(RE::FormID id) {
        std::scoped_lock lock(g_obsMx);
        const auto it = g_obs.find(id);
        return it == g_obs.end() ? Obs{} : it->second;
    }

    std::string TagList(const Obs& o) {
        std::string s;
        for (const auto& t : o.first) {
            if (!s.empty()) s += ", ";
            s += t;
        }
        return s.empty() ? std::string("none") : s;
    }

    // "an IdleStop at +N ms was NOT this idle's ..." or "".
    std::string StrayText(const Obs& o) {
        return o.strayStop ? fmt::format("; an IdleStop at +{} ms was NOT this idle's (it came before any entry event)",
                                          o.strayStopMs - o.playMs)
                           : std::string();
    }

    // ---- The per-actor v2 entry (GAME THREAD ONLY). ----
    struct Entry {
        RE::FormID    idle = 0;          // the claim's param.form this entry was resolved for
        RE::FormID    target = 0;        // the claim's param.target
        std::uint32_t gen = 0;           // bumps per ApplyV2; a posted task carries it
        std::uint32_t plays = 0;         // PlayIdle calls made
        std::uint32_t accepted = 0;      // ... that returned true
        std::uint32_t refused = 0;       // ... that returned false
        bool          live = false;      // the last call was accepted (Release may reset a held idle)
        bool          confirmLogged = false;
        bool          ending = false;    // Harbinger is releasing this claim
        std::string   idleName;          // EDID + anim event, for the logs
        std::string   endedReason;       // for the Release line
        std::string   entryText;         // the resolved entry events + walk stats (EntryText)
        std::string   unconfirmable;     // why no entry event could be resolved ("" = confirmable)
        bool          noTransition = false;   // a COMPLETE walk found no transition on the idle's event
    };

    std::unordered_map<RE::FormID, Entry> g_entries;
    std::atomic<std::size_t>              g_count{ 0 };   // Poll's pre-gate
    std::uint32_t                         g_nextGen = 0;

    // End the WINNING claim this entry was resolved for. MAIN SEAT, OUTSIDE Drain only. A
    // claim that has moved on (a newer ApplyV2) is left alone; that one owns the entry now.
    void EndClaim(RE::FormID id, std::uint32_t gen, const std::string& why) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.ending) return;
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_Idle, param, basis, &h) ||
            param.form != it->second.idle || param.target != it->second.target || h == APMF_API::kInvalidHandle)
            return;
        it->second.ending      = true;
        it->second.endedReason = why;
        apmf::ControlMap::Get().EnqueueRelease(h);
        spdlog::info("[ch.12] 0x{} idle claim ended: {} (idle 0x{}, target 0x{}, claim h={}). Harbinger released "
                     "the claim; a mod that wants another idle sends a new request.",
                     Hex(id), why, Hex(it->second.idle), Hex(it->second.target), h);
    }

    // The one engine call. Posted by ApplyV2(); runs from mainthread::Pump right after
    // Drain published the claim. Re-validates everything (game thread) and drops itself
    // (logged) if anything moved. Every outcome other than "accepted" ENDS the claim.
    void Play(RE::FormID id, std::uint32_t gen, const char* what) {
        const auto it = g_entries.find(id);
        if (it == g_entries.end() || it->second.gen != gen || it->second.ending) {
            spdlog::info("[ch.12] 0x{} idle play DROPPED (stale): a newer engage, a release or a load landed "
                         "before it ran.",
                         Hex(id));
            return;
        }
        const RE::FormID idleId = it->second.idle;
        const RE::FormID tf     = it->second.target;

        APMF_API::APMF_Param now{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_Idle, now) || now.form != idleId ||
            now.target != tf) {
            spdlog::info("[ch.12] 0x{} idle play 0x{} DROPPED (stale): the published winning claim now names idle "
                         "0x{} target 0x{}.",
                         Hex(id), Hex(idleId), Hex(now.form), Hex(now.target));
            return;
        }

        // VALIDATION, on the game thread.
        auto*              actor  = RE::TESForm::LookupByID<RE::Actor>(id);
        RE::TESForm*       iform  = RE::TESForm::LookupByID(idleId);
        auto*              idle   = iform ? iform->As<RE::TESIdleForm>() : nullptr;
        RE::TESObjectREFR* target = nullptr;
        RE::AIProcess*     proc   = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        std::string        why;
        // A Repoint is not validated synchronously (ControlMap::EnqueueRepoint), so a
        // form-carrying Repoint of a v1 claim can reach here with the v2 path down.
        if (!g_installed.load(std::memory_order_acquire))
            why = fmt::format("idle v2 is not available ({})", g_notInstalledReason.load(std::memory_order_acquire));
        else if (!actor)               why = "the actor does not resolve";
        else if (!actor->Is3DLoaded()) why = "the actor is not loaded";
        else if (actor->IsDead())      why = "the actor is dead";
        else if (!proc)                why = "the actor has no AI process";
        else if (!proc->high)          why = "the actor is not in high process (the engine plays no idle there)";
        else if (!iform)               why = "param.form does not resolve to a form";
        else if (!idle)
            why = fmt::format("param.form is not an IDLE record (form type {})",
                              static_cast<std::uint32_t>(iform->GetFormType()));
        if (why.empty() && tf != 0) {
            RE::TESForm* tform = RE::TESForm::LookupByID(tf);
            target             = tform ? tform->As<RE::TESObjectREFR>() : nullptr;
            if (tf == id)                   why = "param.target is the actor itself";
            else if (!tform)                why = "param.target does not resolve to a form";
            else if (!target)               why = "param.target is not a reference";
            else if (target->IsDeleted())   why = "param.target is deleted";
            else if (target->IsDisabled())  why = "param.target is disabled";
            else if (!target->Is3DLoaded()) why = "param.target is not loaded";
        }
        if (!why.empty()) {
            spdlog::warn("[ch.12] 0x{} idle {} 0x{} at target 0x{} NOT PLAYED: {}. No engine call was made.",
                         Hex(id), what, Hex(idleId), Hex(tf), why);
            EndClaim(id, gen, fmt::format("idle not played: {}", why));
            return;
        }

        const char* edid = idle->formEditorID.c_str();
        const char* evn  = idle->animEventName.c_str();
        it->second.idleName = fmt::format("{} (event {})", (edid && *edid) ? edid : "?", (evn && *evn) ? evn : "?");

        // The idle's entry events in THIS actor's graph (ENTRY CONFIRMATION, header). Read-only.
        {
            const EntryInfo ei     = ResolveEntry(actor, evn);
            Entry&          en     = it->second;
            en.entryText           = ei.unreadable.empty() ? EntryText(ei) : ei.unreadable;
            en.noTransition        = ei.Complete() && ei.transitions == 0;
            en.unconfirmable.clear();
            if (!en.noTransition && ei.events.empty())
                en.unconfirmable = !ei.unreadable.empty() ? ei.unreadable
                                   : ei.transitions == 0
                                       ? std::string("no transition on the idle's event was found and the walk was incomplete")
                                       : std::string("the transition(s) on the idle's event raise no notify event");
            // Observe from BEFORE the call: the graph may answer inside it. AddAnimationGraphEventSink
            // returns false when this sink is already on the graph (a re-point) or there is no graph.
            Arm(id, gen, ei.events);
        }
        const bool sinkAdded = actor->AddAnimationGraphEventSink(SinkFor(id));

        // THE CALL. Once. The fork's binding: RELOCATION_ID(38290, 39256).
        const bool ok = proc->PlayIdle(actor, idle, target);

        Entry& e = it->second;   // no insertion/erase since `it` was taken (PlayIdle never calls back into us)
        ++e.plays;
        ok ? ++e.accepted : ++e.refused;
        e.live          = ok;
        e.confirmLogged = false;

        if (!ok) {
            Disarm(id);
            spdlog::warn("[ch.12] 0x{} idle {} {} at target 0x{}: REQUESTED -> the engine REFUSED (PlayIdle returned "
                         "false: the idle's own conditions failed for this actor / target, or the actor is in a state "
                         "that plays no idle).",
                         Hex(id), what, e.idleName, Hex(tf));
            EndClaim(id, gen, "engine refused the idle (PlayIdle returned false)");
            return;
        }
        spdlog::info("[ch.12] 0x{} idle {} {} (0x{}) at target 0x{}: REQUESTED -> PlayIdle returned TRUE (accepted or "
                     "queued, not yet played; sink {}). Confirmation waits for: {}.",
                     Hex(id), what, e.idleName, Hex(idleId), Hex(tf),
                     sinkAdded ? "added" : "already on the graph, or no graph",
                     e.noTransition                     ? fmt::format("NOTHING (no transition on the idle's event: {})", e.entryText)
                     : e.unconfirmable.empty()          ? e.entryText
                     : e.unconfirmable == e.entryText ? fmt::format("NOTHING (unconfirmable: {})", e.unconfirmable)
                                                      : fmt::format("NOTHING (unconfirmable: {}; {})", e.unconfirmable, e.entryText));
    }

    // (Re)write this actor's entry and post ONE task. Inside Drain: nothing here writes the
    // ControlMap and nothing here calls the engine.
    void ApplyV2(RE::FormID id, const APMF_API::APMF_Param& param, const char* what) {
        Entry e{};
        if (const auto it = g_entries.find(id); it != g_entries.end()) {
            e.plays    = it->second.plays;
            e.accepted = it->second.accepted;
            e.refused  = it->second.refused;
        }
        e.idle   = param.form;
        e.target = param.target;
        e.gen    = ++g_nextGen;
        const std::uint32_t gen = e.gen;
        g_entries.insert_or_assign(id, std::move(e));
        g_count.store(g_entries.size(), std::memory_order_relaxed);

        spdlog::info("[ch.12] 0x{} idle claim {} -> idle 0x{} at target 0x{}: ONE AIProcess::PlayIdle call is queued "
                     "for the next main-thread pump (after this claim is published).",
                     Hex(id), what, Hex(param.form), Hex(param.target));
        apmf::mainthread::Post([id, gen, what] { Play(id, gen, what); });
    }

    // One phrase for the Release line: what the entry evidence said.
    std::string EntryState(const Entry& e, const Obs& o) {
        if (!e.live) return "n/a";
        if (o.entered) return fmt::format("CONFIRMED by '{}' at +{} ms", o.enterTag, o.enteredMs - o.playMs);
        if (e.noTransition) return "NO TRANSITION on the idle's event in this actor's graph";
        if (!e.unconfirmable.empty()) return fmt::format("unconfirmable ({})", e.unconfirmable);
        return "NOT observed";
    }

    // The Release-time reset decision, shared by Release and a form-0 owner change (review
    // F3). Sends IdleForceDefaultState ONLY for a held idle on a loaded, living actor that is
    // not in furniture (review F1). Returns the log text. Inside Drain, game thread.
    std::string ResetIfHeld(const Entry& e, const Obs& o, RE::Actor* actor) {
        if (!e.live) return "no reset (no idle was accepted)";
        if (e.noTransition) return "no reset (the actor's graph has no transition on the idle's event: nothing played)";
        if (!o.entry.empty() && !o.entered)
            return "no reset (the graph was never seen entering the idle: nothing of this claim's to stop)";
        if (o.idleStop)
            return o.entered ? fmt::format("no reset (not held: the graph raised IdleStop {} ms after the call, {} ms "
                                           "after the entry)",
                                           o.idleStopMs - o.playMs, o.idleStopMs - o.enteredMs)
                             : fmt::format("no reset (not held: the graph raised IdleStop {} ms after the call)",
                                           o.idleStopMs - o.playMs);
        if (!actor) return "no reset (held, but the actor did not resolve: unloaded or deleted)";
        if (!actor->Is3DLoaded()) return "no reset (held, but the actor is not loaded)";
        if (actor->IsDead()) return "no reset (held, but the actor is dead)";
        if (const auto sit = actor->AsActorState()->GetSitSleepState(); sit != RE::SIT_SLEEP_STATE::kNormal)
            return fmt::format("no reset (held, but the actor is in furniture: sit/sleep state {}; the reset would "
                               "pop it out)",
                               static_cast<std::uint32_t>(sit));
        const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
        return fmt::format("HELD ({}no IdleStop in {} ms) -> IdleForceDefaultState accepted={}",
                           o.entered ? fmt::format("entered at +{} ms, ", o.enteredMs - o.playMs) : std::string(),
                           apmf::clock::MonotonicMs() - o.playMs, ok);
    }

    class IdleChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "idle-anim"; }
        int              ChannelNo() const override { return 12; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Idle; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4E, "NumpadPlus : play a one-shot idle (force default state)" },
            };
            return keys;
        }

        // Posted names are string literals (the task outlives this call).
        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (param.form != 0) {   // v2 (ABI v17)
                ApplyV2(id, param, "ENGAGED");
                return;
            }
            // v1, unchanged.
            if (!actor) return;
            const bool ok = actor->NotifyAnimationGraph("IdleForceDefaultState");
            spdlog::info("[ch.12] 0x{} one-shot idle (IdleForceDefaultState) accepted={}.", apmf::log::Hex(id), ok);
        }

        // A new winning declaration (a Repoint, or another claim taking over). v2: one more
        // PlayIdle for the new idle. Form 0 with no v2 entry: nothing, as before v2 existed.
        // Form 0 over a v2 entry (review F3): the v2 idle is no longer declared, so it gets
        // the Release-style reset (held + furniture guard) and its entry is dropped; the v1
        // claim then has nothing, exactly as a fresh v1 owner change would.
        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (param.form != 0) {
                ApplyV2(id, param, "RE-POINTED");
                return;
            }
            const auto it = g_entries.find(id);
            if (it == g_entries.end()) return;
            const Entry e = it->second;
            g_entries.erase(it);
            g_count.store(g_entries.size(), std::memory_order_relaxed);
            const Obs o = Disarm(id);
            if (actor) actor->RemoveAnimationGraphEventSink(SinkFor(id));
            spdlog::info("[ch.12] 0x{} idle owner changed to a form-free (v1) claim: the v2 idle {} (0x{}) is dropped. "
                         "Reset: {}.",
                         Hex(id), e.idleName.empty() ? std::string("?") : e.idleName, Hex(e.idle),
                         ResetIfHeld(e, o, actor));
        }

        // Relinquish (INVARIANTS #5a). A v1 claim has no entry: nothing to restore, as before.
        void Release(RE::FormID id, RE::Actor* actor) override {
            const auto it = g_entries.find(id);
            if (it == g_entries.end()) return;
            const Entry e = it->second;
            g_entries.erase(it);
            g_count.store(g_entries.size(), std::memory_order_relaxed);

            const Obs o = Disarm(id);
            if (actor) actor->RemoveAnimationGraphEventSink(SinkFor(id));

            const std::string reset = ResetIfHeld(e, o, actor);
            spdlog::info("[ch.12] 0x{} idle released ({}) (idle {} 0x{}, target 0x{}): PlayIdle called {} time(s) -- "
                         "accepted {}, refused {}. Entry: {}. Release: {}. Graph events seen: {} [{}]{}.",
                         Hex(id),
                         e.endedReason.empty() ? std::string("by the client, an unload or a load")
                                               : fmt::format("ENDED BY HARBINGER: {}", e.endedReason),
                         e.idleName.empty() ? std::string("?") : e.idleName, Hex(e.idle), Hex(e.target), e.plays,
                         e.accepted, e.refused, EntryState(e, o), reset, o.events, TagList(o), StrayText(o));
        }
    };

}

namespace apmf::idle {

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (PlayIdle is verified on 1.6.1170 and 1.5.97 only)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (PlayIdle is verified on those two only)";
        } else if (GetPrivateProfileIntA("Idle", "bIdleV2", 1, kIni) == 0) {
            why = "[Idle] bIdleV2=0 in Data/SKSE/Plugins/APMF.ini";
        } else if (!apmf::allowance::SeatVerified(
                       REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38290, 39256) }.address(),
                       "Idle.AIProcess.SetupSpecialIdle")) {
            why = "the address self-check refused AIProcess::SetupSpecialIdle (PlayIdle)";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.12] idle v2 (param.form) NOT available -- {}. kIntent_Idle claims with a form are "
                         "REFUSED; the form-free v1 idle still works.",
                         why);
            return;
        }
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.12] idle v2 available: one AIProcess::PlayIdle(actor, idle, target) per engage / re-point, "
                     "on the main thread; IdleForceDefaultState at release only for a held idle. No hook.");

        // Entry confirmation: the three vtables the graph read identifies objects by (the rows
        // Travel's gate probe 2 verifies too). A refusal leaves every play UNCONFIRMABLE.
        const auto vtA = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BShkbAnimationGraph[0] }.address();
        const auto vtB = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbBehaviorGraph[0] }.address();
        const auto vtS = REL::Relocation<std::uintptr_t>{ RE::VTABLE_hkbStateMachine[0] }.address();
        bool       ok  = apmf::allowance::SeatVerified(vtA, "Idle.Confirm.BShkbAnimationGraph");
        ok             = apmf::allowance::SeatVerified(vtB, "Idle.Confirm.hkbBehaviorGraph") && ok;
        ok             = apmf::allowance::SeatVerified(vtS, "Idle.Confirm.hkbStateMachine") && ok;
        if (!ok) {
            g_entryNotArmed.store("the self-check refused a vtable the graph read identifies objects by",
                                  std::memory_order_release);
            spdlog::error("[ch.12] idle entry confirmation NOT ARMED -- the mit-3.7 self-check refused a vtable it "
                          "identifies objects by. Every idle play will be logged UNCONFIRMABLE.");
            return;
        }
        g_vtAnimGraph     = vtA;
        g_vtBehaviorGraph = vtB;
        g_vtStateMachine  = vtS;
        g_entryReady.store(true, std::memory_order_release);
        spdlog::info("[ch.12] idle entry confirmation armed: an idle counts as played only when the actor's graph "
                     "raises one of the notify events of a transition on the idle's own event (read from the graph "
                     "at the call); none within {} ms = NOT CONFIRMED and the claim ends.",
                     kConfirmWaitMs);
    }

    bool V2Installed() { return g_installed.load(std::memory_order_relaxed); }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Poll() {
        if (g_count.load(std::memory_order_relaxed) == 0) return;
        static std::uint64_t s_lastMs = 0;
        const std::uint64_t  now      = apmf::clock::MonotonicMs();
        if (now - s_lastMs < 250) return;
        s_lastMs = now;

        struct Snap { RE::FormID id; std::uint32_t gen; };
        std::vector<Snap> snaps;
        snaps.reserve(g_entries.size());
        for (const auto& [id, e] : g_entries)
            if (!e.ending && e.live) snaps.push_back({ id, e.gen });

        for (const auto& sn : snaps) {
            auto* owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            if (owner && owner->IsDead()) {
                EndClaim(sn.id, sn.gen, "owner dead");
                continue;
            }
            // The observation line, once per accepted play (principle 5).
            const auto it = g_entries.find(sn.id);
            if (it == g_entries.end() || it->second.gen != sn.gen || it->second.confirmLogged) continue;
            const Obs o = Peek(sn.id);
            if (!o.armed || o.gen != sn.gen) continue;
            Entry& e = it->second;
            if (o.entered) {
                // CONFIRMED: an entry event of THIS idle arrived after the call.
                e.confirmLogged = true;
                spdlog::info("[ch.12] 0x{} idle {}: ANIMATION CONFIRMED -- the graph raised the entry event '{}' at "
                             "+{} ms ({}). Events so far: {} [{}]{}.",
                             Hex(sn.id), e.idleName, o.enterTag, o.enteredMs - o.playMs, e.entryText, o.events,
                             TagList(o),
                             o.idleStop ? fmt::format("; IdleStop at +{} ms, {} ms after the entry (one-shot, ended by "
                                                      "itself: compare with the clip's length)",
                                                      o.idleStopMs - o.playMs, o.idleStopMs - o.enteredMs)
                                        : std::string("; no IdleStop yet (still playing, or held)"));
            } else if (e.noTransition) {
                // The idle cannot enter this graph at all: end the claim at once.
                e.confirmLogged = true;
                spdlog::warn("[ch.12] 0x{} idle {}: ANIMATION NOT CONFIRMED -- the actor's behaviour graph has NO "
                             "transition on the idle's event ({}), so it cannot play on this actor although PlayIdle "
                             "returned true.",
                             Hex(sn.id), e.idleName, e.entryText);
                EndClaim(sn.id, sn.gen, "idle not played: the actor's graph has no transition on its event");
            } else if (!e.unconfirmable.empty()) {
                if (now - o.playMs >= kConfirmWaitMs) {
                    e.confirmLogged = true;
                    spdlog::warn("[ch.12] 0x{} idle {}: UNCONFIRMABLE -- {}. PlayIdle returned true; whether the clip "
                                 "played is UNKNOWN (the claim stays; held = no IdleStop since the call). Graph events "
                                 "in {} ms: {} [{}]{}.",
                                 Hex(sn.id), e.idleName, e.unconfirmable, now - o.playMs, o.events, TagList(o),
                                 o.idleStop ? fmt::format("; IdleStop at +{} ms", o.idleStopMs - o.playMs) : std::string());
                }
            } else if (now - o.playMs >= kConfirmWaitMs) {
                // NOT CONFIRMED: none of the idle's entry events arrived. What did arrive is someone
                // else's (the 2026-09-28 lockpick: IdleStop, tailCombatState, attackStop, Pie...).
                e.confirmLogged = true;
                spdlog::warn("[ch.12] 0x{} idle {}: ANIMATION NOT CONFIRMED -- none of its entry events arrived in {} "
                             "ms ({}); PlayIdle returned true but the graph never entered the idle. Unrelated graph "
                             "events: {} [{}]{}. Harbinger ends the claim.",
                             Hex(sn.id), e.idleName, now - o.playMs, e.entryText, o.events, TagList(o), StrayText(o));
                EndClaim(sn.id, sn.gen,
                         fmt::format("idle not observed playing (no entry event in {} ms)", now - o.playMs));
            }
        }
    }

    void ResetAll(const char* why) {
        const std::size_t n = g_entries.size();
        g_entries.clear();
        g_count.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lock(g_obsMx);
            g_obs.clear();
        }
        if (n != 0) spdlog::info("[ch.12] {} -- dropped {} idle entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(IdleChannel);
