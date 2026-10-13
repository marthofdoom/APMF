#include "PCH.h"
#include "channels/Idle.h"
#include "core/Allowance.h"       // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
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
//   destination state's enterNotifyEvents and the enterNotifyEvents of the start state of
//   a state machine nested in the destination; ONLY when none exist, the source state's
//   exitNotifyEvents (not for a wildcard). Each entry event also carries how many OTHER
//   transitions raise it; a confirmation on a shared one is logged WEAK evidence (review
//   18be6ec SEV-3.2; IdleOffsetStop is raised by ~105 others). Liveness is the same. Vanilla + Tuxborn's Nemesis graphs (offline, same walk):
//   IdleLockPick -> MT_BehaviorGraph>InteractionObjectState, {IdleOffsetStop, OffsetStop};
//   IdleGive -> MT_RootBehavior MT_State>NonOffsetIdles, {IdleOffsetStop} (MT_State exit).
//   Every field IdleGive play listed IdleOffsetStop first; the lockpick listed neither.
// Outcomes, one line per play from Poll:
//   CONFIRMED      an entry event arrived (named, with its time);
//   NOT CONFIRMED  none arrived within kConfirmWaitMs of WORLD time (and kConfirmMinFrames
//                  frames; a menu pause does not count) -> WARN and Harbinger ENDS the claim
//                  (IsClaimLive goes false: the client sees the idle did not play);
//   NO TRANSITION  a COMPLETE walk (EntryInfo::Blind() == 0: no unknown class, unlinked
//                  reference, out-of-range event id, unresolved target, wrong-class object or
//                  unwalked graph) found no transition on the idle's event -> the idle cannot
//                  play on this actor's graph -> WARN and the claim ENDS at the first Poll;
//   UNCONFIRMABLE  no entry event could be resolved (the destination raises none, the walk
//                  was incomplete, the graph could not be read, an event id fell outside its
//                  graph's name table = "id space broken", or [Idle] bIdleConfirm=0) -> WARN,
//                  the claim stays, and HELD falls back to "no IdleStop since the call".
// ABI v24: each outcome (and Pending / Accepted / Refused / NOT PLAYED before it) is also a per-HANDLE
// state a client reads with APMF_API_v24::GetIdleState, kept for a grace after the claim ends
// ("THE PER-HANDLE IDLE STATE", below the Entry table).
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
    constexpr std::uint64_t kConfirmWaitMs = 3000;   // no ENTRY event in this much WORLD time after an accepted play = NOT CONFIRMED
    constexpr std::uint32_t kConfirmMinFrames = 30;  // ... and at least this many world ticks (a hitch cannot end it)
    constexpr std::uint64_t kTickClampMs   = 100;    // one Poll-to-Poll gap counts at most this much world time
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
    std::atomic<std::uint32_t> g_entryNotArmedWhy{ APMF_API::kIdleUnconf_NotArmed };   // ABI v24: the same, as a code
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
        std::vector<std::string> events;        // the entry events waited for (enter ones when any exist)
        std::vector<std::string> enter;         // destination / nested-start ENTER events (preferred)
        std::vector<std::string> exits;         // source-state EXIT events (used only when no enter event exists)
        std::vector<std::uint32_t> shared;      // per events[i]: OTHER transitions that raise it too
        std::string              dests;         // "Machine>State" for the log
        std::uint32_t            transitions = 0;
        std::uint32_t            graphs = 0;
        std::uint32_t            nodes = 0;
        // BLIND SPOTS (review 18be6ec SEV-2): each one makes the walk INCOMPLETE, so a
        // "no transition" answer is only ever given after a walk that saw everything.
        std::uint32_t            unknown = 0;     // generator nodes of a class the walk does not descend
        std::uint32_t            unlinked = 0;    // behaviour references with no linked graph
        std::uint32_t            badIds = 0;      // event ids outside their graph's name table
        std::uint32_t            unresolved = 0;  // transitions whose target state is not in the machine
        std::uint32_t            skipped = 0;     // objects of the wrong class where a known one belongs
        std::uint32_t            badGraphs = 0;   // graphs failing the vtable / layout guard, or without names
        bool                     truncated = false;
        std::string              unreadable;    // set when no walk happened, with the reason
        std::uint32_t            unreadableWhy = APMF_API::kIdleUnconf_None;   // ABI v24: `unreadable` as a code
        std::uint32_t Blind() const { return unknown + unlinked + badIds + unresolved + skipped + badGraphs; }
        bool Complete() const { return unreadable.empty() && Blind() == 0 && !truncated && graphs != 0; }
    };

    void AddName(std::vector<std::string>& a_v, const char* a_name) {
        if (!a_name || !*a_name) return;
        for (const auto& e : a_v)
            if (IEquals(e, a_name)) return;
        a_v.emplace_back(a_name);
    }

    std::string Lower(std::string_view a_s) {
        std::string r(a_s);
        for (auto& c : r)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return r;
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
            if (!Is(sd, Kind::kStringData)) {
                ++out.badGraphs;
                return;
            }
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
                if (Rd<std::uintptr_t>(bg) != g_vtBehaviorGraph) {
                    ++out.badGraphs;   // a linked "graph" that is not one
                    continue;
                }
                Graph(bg);
            }
        }

        // After the walk: the events to wait for (ENTER events when any exist, else the
        // source EXIT events) and, for each, how many OTHER transitions raise it too.
        void Finish() {
            out.events = !out.enter.empty() ? out.enter : out.exits;
            out.shared.clear();
            for (const auto& e : out.events) {
                const auto it = raisedByOthers.find(Lower(e));
                out.shared.push_back(it == raisedByOthers.end() ? 0 : it->second);
            }
        }

    private:
        struct Names {
            std::uintptr_t data;
            std::int32_t   size;
            bool           Valid(std::int32_t i) const { return data && i >= 0 && i < size; }
            const char*    Get(std::int32_t i) const { return Valid(i) ? HkStr(data + 8 * static_cast<std::uintptr_t>(i)) : nullptr; }
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
                else if (x)
                    ++out.skipped;
            }
        }

        // An EventPropertyArray -> its names, appended to a_to.
        void Notify(std::uintptr_t a_arr, const Names& a_names, std::vector<std::string>& a_to) {
            if (!a_arr) return;
            if (!Is(a_arr, Kind::kEventPropertyArray)) {
                ++out.skipped;
                return;
            }
            const auto p = Rd<std::uintptr_t>(a_arr + 0x10);
            const auto n = Rd<std::int32_t>(a_arr + 0x18);
            for (std::int32_t i = 0; p && i < n; ++i) {   // hkbEventProperty: id 0x0, payload 0x8; 0x10 each
                const auto id = Rd<std::int32_t>(p + 0x10 * static_cast<std::uintptr_t>(i));
                if (!a_names.Valid(id)) {
                    ++out.badIds;
                    continue;
                }
                AddName(a_to, a_names.Get(id));
            }
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
        void NestedStart(std::uintptr_t a_gen, const Names& a_names, std::vector<std::string>* a_to) {
            for (int depth = 0; a_gen && depth < 8; ++depth) {
                const Kind k = KindOf(a_gen);
                if (k == Kind::kStateMachine) {
                    const auto s = StateById(a_gen, Rd<std::int32_t>(a_gen + 0x68));   // startStateId
                    if (!s) {
                        ++out.unresolved;
                        return;
                    }
                    Notify(Rd<std::uintptr_t>(s + 0x40), a_names, *a_to);
                    a_gen = Rd<std::uintptr_t>(s + 0x58);
                } else if (k == Kind::kModifierGenerator || k == Kind::kStateTagging || k == Kind::kCyclicBlend) {
                    a_gen = Rd<std::uintptr_t>(a_gen + 0x50);
                } else {
                    return;
                }
            }
        }

        // hkbStateMachine.TransitionInfoArray: hkArray 0x10 of 0x48-byte TransitionInfo
        // (eventId 0x30, toStateId 0x34). EVERY transition's id is range-checked (a broken id
        // space would make "no transition" a lie), and every transition's raised events are
        // counted so a shared entry event can be labelled weak evidence.
        void Transitions(std::uintptr_t a_sm, std::uintptr_t a_arr, std::uintptr_t a_from, const Names& a_names) {
            if (!a_arr) return;
            if (!Is(a_arr, Kind::kTransitionArray)) {
                ++out.skipped;
                return;
            }
            const auto p = Rd<std::uintptr_t>(a_arr + 0x10);
            const auto n = Rd<std::int32_t>(a_arr + 0x18);
            for (std::int32_t i = 0; p && i < n; ++i) {
                const auto t  = p + 0x48 * static_cast<std::uintptr_t>(i);
                const auto id = Rd<std::int32_t>(t + 0x30);
                if (!a_names.Valid(id)) {
                    ++out.badIds;
                    continue;
                }
                const char* ev   = a_names.Get(id);
                const bool  ours = ev && IEquals(ev, event);
                const auto  dest = StateById(a_sm, Rd<std::int32_t>(t + 0x34));
                if (!dest) {
                    ++out.unresolved;
                    continue;
                }
                std::vector<std::string> enter, exits;
                Notify(Rd<std::uintptr_t>(dest + 0x40), a_names, enter);             // dest enterNotifyEvents
                if (a_from) Notify(Rd<std::uintptr_t>(a_from + 0x48), a_names, exits);  // source exitNotifyEvents
                if (!ours) {
                    for (const auto& x : enter) ++raisedByOthers[Lower(x)];
                    for (const auto& x : exits) ++raisedByOthers[Lower(x)];
                    continue;
                }
                ++out.transitions;
                for (const auto& x : enter) AddName(out.enter, x.c_str());
                for (const auto& x : exits) AddName(out.exits, x.c_str());
                NestedStart(Rd<std::uintptr_t>(dest + 0x58), a_names, &out.enter);
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
                if (!Is(s, Kind::kStateInfo)) {
                    ++out.skipped;
                    continue;
                }
                a_stack.push_back(Rd<std::uintptr_t>(s + 0x58));
                Transitions(a_sm, Rd<std::uintptr_t>(s + 0x50), s, a_names);
            }
        }

        std::string_view                   event;
        EntryInfo&                         out;
        std::unordered_set<std::uintptr_t> seen;
        std::unordered_set<std::uintptr_t> graphsSeen;
        std::vector<std::uintptr_t>        pending;
        std::unordered_map<std::string, std::uint32_t> raisedByOthers;   // lower-case event -> other transitions
    };

    // GAME THREAD, before the PlayIdle call. Never throws; a graph it cannot read says why.
    EntryInfo ResolveEntry(RE::Actor* a_actor, const char* a_event) {
        EntryInfo out;
        if (!g_entryReady.load(std::memory_order_acquire)) {
            out.unreadable    = fmt::format("entry confirmation not armed ({})", g_entryNotArmed.load(std::memory_order_acquire));
            out.unreadableWhy = g_entryNotArmedWhy.load(std::memory_order_acquire);
            return out;
        }
        if (!a_event || !*a_event) {
            out.unreadable    = "the idle names no animation event";
            out.unreadableWhy = APMF_API::kIdleUnconf_NoEntryEvents;
            return out;
        }
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;   // held for the whole walk
        if (!a_actor->GetAnimationGraphManager(mgr) || !mgr) {
            out.unreadable    = "the actor has no animation graph";
            out.unreadableWhy = APMF_API::kIdleUnconf_GraphUnreadable;
            return out;
        }
        EntryWalk walk(a_event, out);
        for (std::uint32_t i = 0; i < mgr->graphs.size(); ++i) {
            const auto g = reinterpret_cast<std::uintptr_t>(mgr->graphs[i].get());
            if (!g) continue;
            if (i >= 2) {   // never seen on an NPC (1st/3rd person is 2): not walked, so the walk is incomplete
                ++out.badGraphs;
                continue;
            }
            if (Rd<std::uintptr_t>(g) != g_vtAnimGraph) {
                ++out.badGraphs;
                continue;
            }
            const auto bg = Rd<std::uintptr_t>(g + 0x208);
            if (!bg || bg != Rd<std::uintptr_t>(g + 0xC0 + 0x58) || Rd<std::uintptr_t>(bg) != g_vtBehaviorGraph) {
                ++out.badGraphs;   // the probe-2 layout guard
                continue;
            }
            walk.Graph(bg);
            walk.Drain();
        }
        walk.Finish();
        if (out.graphs == 0) {
            out.unreadable    = "no behaviour graph passed the layout / vtable checks";
            out.unreadableWhy = APMF_API::kIdleUnconf_GraphUnreadable;
        }
        return out;
    }

    std::string EntryText(const EntryInfo& a_e) {
        std::string ev;
        for (std::size_t i = 0; i < a_e.events.size(); ++i)
            ev += fmt::format("{}{} ({})", ev.empty() ? "" : ", ", a_e.events[i],
                              a_e.shared[i] ? fmt::format("also raised by {} other transition(s)", a_e.shared[i])
                                            : std::string("specific"));
        std::string blind;
        auto        add = [&](std::uint32_t n, const char* what) {
            if (n) blind += fmt::format(", {} {}", n, what);
        };
        add(a_e.unknown, "node(s) of an unwalked class");
        add(a_e.unlinked, "unlinked reference(s)");
        add(a_e.badIds, "OUT-OF-RANGE event id(s)");
        add(a_e.unresolved, "unresolved target state(s)");
        add(a_e.skipped, "object(s) of an unexpected class");
        add(a_e.badGraphs, "graph(s) not walked (guard / names / index)");
        return fmt::format("{} events [{}] from {} transition(s){}{}; walked {} graph(s), {} node(s){}{}{}",
                           a_e.enter.empty() && !a_e.exits.empty() ? "source-exit" : "entry",
                           ev.empty() ? std::string("none") : ev, a_e.transitions, a_e.dests.empty() ? "" : " to ", a_e.dests,
                           a_e.graphs, a_e.nodes, blind, a_e.truncated ? ", TRUNCATED" : "",
                           a_e.Complete() ? " (complete)" : " (INCOMPLETE)");
    }

    // ---- The anim-graph observation (ANY THREAD for the sink, under g_obsMx). ----
    struct Obs {
        bool                     armed = false;
        std::uint32_t            gen = 0;        // the play this record belongs to
        std::uint64_t            playMs = 0;
        RE::FormID               idle = 0;       // the idle this play is for
        std::vector<std::string> entry;          // the idle's entry events (empty = unconfirmable)
        bool                     entered = false;
        std::uint64_t            enteredMs = 0;
        std::string              enterTag;       // the RAW entry event that arrived for THIS play (no decoration)
        // CARRY (review idle-state F1/F2): a re-play of the SAME idle by the SAME claim handle while the
        // previous play had an entry (fresh or carried) and no IdleStop since. NOT evidence that this play
        // entered anything: `entered` stays false so a fresh entry is still recorded. Judged and reported
        // apart (kIdleState_Carried), never as CONFIRMED.
        APMF_API::Handle         handle = APMF_API::kInvalidHandle;   // the claim this play is for
        bool                     carried = false;
        std::string              carriedEvent;   // the RAW entry event of the play the carry came from
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
            // An IdleStop is the idle's end only AFTER its entry (or a carried one; and not the very event that
            // proved the entry, e.g. an IdleStop that is a source state's exit notify). Before
            // any entry it belongs to something else. With no entry events to wait for
            // (unconfirmable) any IdleStop counts, the pre-fix rule.
            if (!entryNow && IEquals(tag, "IdleStop")) {
                if (o.entry.empty() || o.entered || o.carried) {
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

    // A re-play (a Repoint) of the SAME idle by the SAME claim handle while the previous play
    // had an entry (fresh or itself carried) and no IdleStop has been seen since CARRIES it: the
    // graph may still be in the idle's state, where a transition to the same state may raise
    // nothing (MFO re-plays the lockpick once per clip length). That is an INFERENCE from "no
    // IdleStop seen since", not evidence, so the carry is recorded apart (`carried`) and this
    // play still records a FRESH entry if one arrives. Another handle (an owner change) never
    // inherits: its play starts a fresh observation (review idle-state F1).
    bool Arm(RE::FormID id, std::uint32_t gen, RE::FormID idle, APMF_API::Handle handle,
             const std::vector<std::string>& entry) {
        std::scoped_lock lock(g_obsMx);
        Obs o{};
        o.armed  = true;
        o.gen    = gen;
        o.playMs = apmf::clock::MonotonicMs();
        o.idle   = idle;
        o.handle = handle;
        o.entry  = entry;
        if (const auto it = g_obs.find(id); it != g_obs.end()) {
            const Obs& prev = it->second;
            if (prev.armed && prev.idle == idle && prev.handle == handle && handle != APMF_API::kInvalidHandle &&
                (prev.entered || prev.carried) && !prev.idleStop && !entry.empty()) {
                o.carried      = true;
                o.carriedEvent = prev.entered ? prev.enterTag : prev.carriedEvent;
            }
        }
        const bool carried = o.carried;
        g_obs.insert_or_assign(id, std::move(o));
        return carried;
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
        bool          weak = false;           // every entry event is also raised by other transitions
        std::vector<std::pair<std::string, std::uint32_t>> waitShared;   // entry event -> other transitions raising it
        std::uint64_t playWorldMs = 0;        // g_worldMs / g_worldFrames at the call
        std::uint64_t playFrame   = 0;
        // ABI v24 (the per-handle state, below).
        APMF_API::Handle handle = APMF_API::kInvalidHandle;   // the claim this entry last played for (Play)
        std::uint32_t unconfWhy     = APMF_API::kIdleUnconf_None;   // `unconfirmable` as a code
        bool          carriedOpen   = false;   // a carried play still watched for a fresh entry / an IdleStop
    };

    std::unordered_map<RE::FormID, Entry> g_entries;

    // WORLD-TICK CLOCK (review 18be6ec SEV-3.1). Poll runs from the player's Update seat
    // (Arbiter::OncePerFrame), which does not run while the game is paused in a menu; the
    // gap between two calls is clamped to kTickClampMs, so a pause, a load screen or a hitch
    // adds at most one clamp. The NOT CONFIRMED window is measured on this clock plus a frame
    // count, never on wall time. GAME THREAD.
    std::uint64_t g_worldMs     = 0;
    std::uint64_t g_worldFrames = 0;
    std::uint64_t g_lastTickMs  = 0;
    std::atomic<std::size_t>              g_count{ 0 };   // Poll's pre-gate
    std::uint32_t                         g_nextGen = 0;

    // ---- ABI v24: THE PER-HANDLE IDLE STATE (APMF_API_v24::GetIdleState). ----
    // One record per ch.12 claim HANDLE that carries a form. Writers: the client's thread
    // (NoteFiled / NoteRepoint / NoteReleased, from core/ClientAPI.cpp) and the game thread
    // (Play, Poll, EndClaim, Release, OnOwnerChanged, ResetAll). Reader: GetState, any thread.
    // g_stateMx is a LEAF lock: nothing else is acquired while it is held (not g_obsMx, not the
    // ControlMap), and no engine call is made under it. Records hold plain values only.
    //
    // LIFETIME, sized from the real cadence (principle 9: the grace is a floor on how long a
    // terminal state stays readable, never an expiry of live state). A record stays while its
    // claim is live (IsClaimLive), and for kStateGraceMs of WORLD time after its last change
    // once it is not. Terminal states are written at Poll's 250 ms cadence (or at the play, for
    // Refused / NotPlayed / NoTransition); MFO reads its taunt every 133 ms pump tick, with
    // ticks up to 400 ms apart. 10000 ms of world time is 40 Polls and 25 of those slowest
    // client ticks; a menu pause does not advance it, so a paused client misses nothing. Pruned
    // by Poll every 250 ms (a not-live record with no final cause is first stamped kIdleEnd_Gone,
    // restarting its grace: PruneStates); the IsClaimLive scan runs OUTSIDE the lock. Bounded by kMaxStates
    // (a pathological client): the oldest ended record, else the oldest, is evicted (logged).
    constexpr std::uint64_t kStateGraceMs  = APMF_API::kIdleStateGraceMs;
    constexpr std::size_t   kMaxStates     = 1024;
    constexpr std::uint64_t kStatePruneMs  = 250;
    constexpr std::uint64_t kStateGoneFloorMs = 1000;   // world ms before a not-live, un-ended record is stamped Gone
    static_assert(kConfirmWaitMs == APMF_API::kIdleConfirmWindowMs, "the header's informational window must match");

    struct StateRec {
        RE::FormID    actor = 0;
        RE::FormID    idle = 0;
        RE::FormID    target = 0;
        std::uint32_t state = APMF_API::kIdleState_Pending;
        std::uint32_t unconfWhy = APMF_API::kIdleUnconf_None;
        std::uint32_t endCause = APMF_API::kIdleEnd_None;
        std::uint32_t flags = 0;
        std::uint32_t gen = 0;            // the Entry gen of the play this state belongs to (0 = none yet)
        std::uint32_t plays = 0;
        std::uint32_t seq = 0;
        std::uint32_t msToEntry = APMF_API::kIdleMsNone;
        bool          played = false;     // PlayIdle was called for the current play
        std::uint64_t playWorldMs = 0;
        std::uint64_t endWorldMs = 0;     // valid when endCause != kIdleEnd_None
        std::uint64_t changeWorldMs = 0;  // the last change (the grace counts from it)
    };

    std::mutex                                     g_stateMx;
    std::unordered_map<APMF_API::Handle, StateRec> g_states;
    std::atomic<std::size_t>                       g_stateCount{ 0 };
    std::uint32_t                                  g_stateSeq = 0;           // under g_stateMx
    std::atomic<std::uint64_t>                     g_worldMsPub{ 0 };       // g_worldMs for other threads
    std::atomic<bool>                              g_stateCapLogged{ false };

    // Under g_stateMx. Stamp a change.
    void Touch(StateRec& r) {
        r.seq           = ++g_stateSeq;
        r.changeWorldMs = g_worldMsPub.load(std::memory_order_relaxed);
    }

    // Under g_stateMx. Make room for one insert at the cap.
    void EvictForInsert() {
        if (g_states.size() < kMaxStates) return;
        auto victim = g_states.end();
        for (auto it = g_states.begin(); it != g_states.end(); ++it) {
            if (victim == g_states.end()) { victim = it; continue; }
            const bool endedV = victim->second.endCause != APMF_API::kIdleEnd_None;
            const bool endedI = it->second.endCause != APMF_API::kIdleEnd_None;
            if (endedI != endedV ? endedI : it->second.changeWorldMs < victim->second.changeWorldMs) victim = it;
        }
        if (victim == g_states.end()) return;
        if (!g_stateCapLogged.exchange(true))
            spdlog::warn("[ch.12] idle state table at its cap ({} handles): the oldest record (h={}) is evicted and "
                         "reads kIdleState_Unknown. Said once per session.",
                         kMaxStates, victim->first);
        g_states.erase(victim);
    }

    // GAME THREAD (Play): a new play of `h` begins. Upserts; restarts the record.
    void StateBegin(APMF_API::Handle h, RE::FormID actor, RE::FormID idle, RE::FormID target, std::uint32_t gen) {
        if (h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lock(g_stateMx);
        auto it = g_states.find(h);
        if (it == g_states.end()) {
            EvictForInsert();
            it = g_states.emplace(h, StateRec{}).first;
        }
        StateRec& r  = it->second;
        r.actor      = actor;
        r.idle       = idle;
        r.target     = target;
        r.state      = APMF_API::kIdleState_Pending;
        r.unconfWhy  = APMF_API::kIdleUnconf_None;
        r.endCause   = APMF_API::kIdleEnd_None;
        r.flags      = 0;
        r.gen        = gen;
        r.msToEntry  = APMF_API::kIdleMsNone;
        r.played     = false;
        r.endWorldMs = 0;
        Touch(r);
        g_stateCount.store(g_states.size(), std::memory_order_relaxed);
    }

    // GAME THREAD: apply `fn` to `h`'s record if it still belongs to play `gen`.
    template <class F>
    void StateUpdate(APMF_API::Handle h, std::uint32_t gen, F&& fn) {
        if (h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lock(g_stateMx);
        const auto it = g_states.find(h);
        if (it == g_states.end() || it->second.gen != gen) return;
        StateRec& r      = it->second;
        const auto before = std::tie(r.state, r.unconfWhy, r.flags, r.msToEntry, r.plays, r.played, r.playWorldMs);
        const auto copy   = std::make_tuple(r.state, r.unconfWhy, r.flags, r.msToEntry, r.plays, r.played, r.playWorldMs);
        fn(r);
        if (before != copy) Touch(r);   // a change only (seq means "changed")
    }

    // ANY THREAD: `h`'s claim ended (or stopped being the one that plays). The first FINAL cause
    // wins; kIdleEnd_Superseded is not final (a later release or Harbinger end replaces it, and a
    // new play restarts the record).
    void StateEnd(APMF_API::Handle h, std::uint32_t cause) {
        if (h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lock(g_stateMx);
        const auto it = g_states.find(h);
        if (it == g_states.end()) return;
        StateRec& r = it->second;
        if (r.endCause != APMF_API::kIdleEnd_None && r.endCause != APMF_API::kIdleEnd_Superseded) return;
        if (r.endCause == cause) return;
        r.endCause   = cause;
        r.endWorldMs = g_worldMsPub.load(std::memory_order_relaxed);
        Touch(r);
    }

    // GAME THREAD (Poll), throttled. Two jobs (review idle-state F3):
    //  * a record whose claim is no longer live but carries no FINAL end cause (None, or Superseded)
    //    -- a claim released before its play ran, or a superseded one dropped by the unload sweep --
    //    is stamped kIdleEnd_Gone, which restarts its grace, so a client reads why and not Unknown.
    //    It waits kStateGoneFloorMs of world time after its last change first: a claim filed from a
    //    client thread is not live until the next Drain publishes it (one frame).
    //  * a record whose claim is gone and whose grace (from its last change) ran out is dropped.
    // IsClaimLive runs OUTSIDE the lock; a record changed in between (seq) is left for the next pass.
    void PruneStates() {
        const std::uint64_t now = g_worldMsPub.load(std::memory_order_relaxed);
        struct Cand { APMF_API::Handle h; std::uint32_t seq; bool stamp; };
        std::vector<Cand> cands;
        {
            std::scoped_lock lock(g_stateMx);
            for (const auto& [h, r] : g_states) {
                const bool open = r.endCause == APMF_API::kIdleEnd_None || r.endCause == APMF_API::kIdleEnd_Superseded;
                if (now - r.changeWorldMs >= (open ? kStateGoneFloorMs : kStateGraceMs)) cands.push_back({ h, r.seq, open });
            }
        }
        if (cands.empty()) return;
        std::vector<Cand> gone;
        for (const auto& c : cands)   // OUTSIDE the lock: IsClaimLive reads the ControlMap snapshot
            if (!apmf::ControlMap::Get().IsClaimLive(c.h)) gone.push_back(c);
        if (gone.empty()) return;
        std::scoped_lock lock(g_stateMx);
        for (const auto& c : gone) {
            const auto it = g_states.find(c.h);
            if (it == g_states.end() || it->second.seq != c.seq) continue;
            if (c.stamp) {
                it->second.endCause   = APMF_API::kIdleEnd_Gone;
                it->second.endWorldMs = now;
                Touch(it->second);   // the grace restarts from the stamp
            } else {
                g_states.erase(it);
            }
        }
        g_stateCount.store(g_states.size(), std::memory_order_relaxed);
    }

    // End the WINNING claim this entry was resolved for. MAIN SEAT, OUTSIDE Drain only. A
    // claim that has moved on (a newer ApplyV2) is left alone; that one owns the entry now.
    void EndClaim(RE::FormID id, std::uint32_t gen, const std::string& why,
                  std::uint32_t cause = APMF_API::kIdleEnd_Harbinger) {
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
        StateEnd(h, cause);   // ABI v24: the terminal state stays readable for the grace
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
        float                nowBasis = 0.0f;
        APMF_API::Handle     h        = APMF_API::kInvalidHandle;   // ABI v24: the claim this play is for
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_Idle, now, nowBasis, &h) ||
            now.form != idleId || now.target != tf) {
            spdlog::info("[ch.12] 0x{} idle play 0x{} DROPPED (stale): the published winning claim now names idle "
                         "0x{} target 0x{}.",
                         Hex(id), Hex(idleId), Hex(now.form), Hex(now.target));
            return;
        }
        // ABI v24: the claim that played here before (another handle) no longer plays; this one starts.
        if (it->second.handle != h) {
            StateEnd(it->second.handle, APMF_API::kIdleEnd_Superseded);
            it->second.handle = h;
        }
        StateBegin(h, id, idleId, tf, gen);

        // VALIDATION, on the game thread.
        auto*              actor  = RE::TESForm::LookupByID<RE::Actor>(id);
        RE::TESForm*       iform  = RE::TESForm::LookupByID(idleId);
        auto*              idle   = iform ? iform->As<RE::TESIdleForm>() : nullptr;
        RE::TESObjectREFR* target = nullptr;
        RE::AIProcess*     proc   = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        std::string        why;
        bool               dead = false;
        // A Repoint is not validated synchronously (ControlMap::EnqueueRepoint), so a
        // form-carrying Repoint of a v1 claim can reach here with the v2 path down.
        if (!g_installed.load(std::memory_order_acquire))
            why = fmt::format("idle v2 is not available ({})", g_notInstalledReason.load(std::memory_order_acquire));
        else if (!actor)               why = "the actor does not resolve";
        else if (!actor->Is3DLoaded()) why = "the actor is not loaded";
        else if (actor->IsDead()) {
            why  = "the actor is dead";
            dead = true;   // ABI v24 (review idle-state F7): the end cause is ActorDied
        }
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
            StateUpdate(h, gen, [](StateRec& r) { r.state = APMF_API::kIdleState_NotPlayed; });
            EndClaim(id, gen, fmt::format("idle not played: {}", why),
                     dead ? APMF_API::kIdleEnd_ActorDied : APMF_API::kIdleEnd_Harbinger);
            return;
        }

        const char* edid = idle->formEditorID.c_str();
        const char* evn  = idle->animEventName.c_str();
        it->second.idleName = fmt::format("{} (event {})", (edid && *edid) ? edid : "?", (evn && *evn) ? evn : "?");

        // The idle's entry events in THIS actor's graph (ENTRY CONFIRMATION, header). Read-only.
        bool carriedPlay = false;   // ABI v24: Arm carried the previous play's entry (same handle, same idle)
        {
            const EntryInfo ei     = ResolveEntry(actor, evn);
            Entry&          en     = it->second;
            en.entryText           = ei.unreadable.empty() ? EntryText(ei) : ei.unreadable;
            en.noTransition        = ei.Complete() && ei.transitions == 0;
            en.unconfirmable.clear();
            en.unconfWhy = APMF_API::kIdleUnconf_None;
            en.weak                = !ei.events.empty() && std::all_of(ei.shared.begin(), ei.shared.end(),
                                                                       [](std::uint32_t n) { return n != 0; });
            std::vector<std::string> waitFor = ei.events;
            en.waitShared.clear();
            for (std::size_t k = 0; k < ei.events.size(); ++k) en.waitShared.emplace_back(ei.events[k], ei.shared[k]);
            en.playWorldMs = g_worldMs;
            en.playFrame   = g_worldFrames;
            if (ei.badIds != 0) {
                // The graph's event ids do not fit its own name table: every name the walk read
                // is suspect, so nothing is judged (review 18be6ec SEV-2).
                en.unconfirmable = fmt::format("EVENT ID SPACE BROKEN: {} event id(s) outside the graph's name table",
                                               ei.badIds);
                en.unconfWhy     = APMF_API::kIdleUnconf_IdSpaceBroken;
                waitFor.clear();
                spdlog::error("[ch.12] 0x{} idle {}: the behaviour-graph read found {} event id(s) outside their "
                              "graph's name table -- the id space is not what the walk assumes. This play is "
                              "UNCONFIRMABLE ({}).",
                              Hex(id), it->second.idleName, ei.badIds, en.entryText);
            } else if (!en.noTransition && ei.events.empty()) {
                en.unconfirmable = !ei.unreadable.empty() ? ei.unreadable
                                   : ei.transitions == 0
                                       ? std::string("no transition on the idle's event was found and the walk was incomplete")
                                       : std::string("the transition(s) on the idle's event raise no notify event");
                en.unconfWhy     = !ei.unreadable.empty() ? ei.unreadableWhy
                                   : ei.transitions == 0  ? APMF_API::kIdleUnconf_WalkIncomplete
                                                          : APMF_API::kIdleUnconf_NoEntryEvents;
            }
            // Observe from BEFORE the call: the graph may answer inside it. AddAnimationGraphEventSink
            // returns false when this sink is already on the graph (a re-point) or there is no graph.
            carriedPlay = Arm(id, gen, idleId, h, waitFor);
        }
        const bool sinkAdded = actor->AddAnimationGraphEventSink(SinkFor(id));

        // THE CALL. Once. The fork's binding: RELOCATION_ID(38290, 39256).
        const bool ok = proc->PlayIdle(actor, idle, target);

        Entry& e = it->second;   // no insertion/erase since `it` was taken (PlayIdle never calls back into us)
        ++e.plays;
        ok ? ++e.accepted : ++e.refused;
        e.live          = ok;
        e.confirmLogged = false;
        e.carriedOpen = ok && carriedPlay;
        // ABI v24: what the call says now (Poll moves Accepted on to Confirmed / NotConfirmed).
        StateUpdate(h, gen, [&](StateRec& r) {
            ++r.plays;
            r.played      = true;
            r.playWorldMs = e.playWorldMs;
            r.unconfWhy   = ok && !e.noTransition && !e.unconfirmable.empty() ? e.unconfWhy : APMF_API::kIdleUnconf_None;
            r.flags       = ok && carriedPlay ? (r.flags | APMF_API::kIdleInfo_Carried) : r.flags;
            r.state       = !ok                        ? APMF_API::kIdleState_Refused
                            : e.noTransition           ? APMF_API::kIdleState_NoTransition
                            : !e.unconfirmable.empty() ? APMF_API::kIdleState_Unconfirmable
                            : carriedPlay              ? APMF_API::kIdleState_Carried
                                                       : APMF_API::kIdleState_Accepted;
        });

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
            e.handle   = it->second.handle;   // ABI v24: Play compares it with the claim it plays for
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
        if (o.carried)
            return fmt::format("CARRIED (no fresh entry; no IdleStop seen since '{}' of this claim's previous play)",
                               o.carriedEvent);
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
        if (!o.entry.empty() && !o.entered && !o.carried)
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
            StateEnd(e.handle, APMF_API::kIdleEnd_Superseded);   // ABI v24: its idle is no longer played
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
            // ABI v24: a Harbinger end was already recorded by EndClaim (the first final cause wins).
            StateEnd(e.handle, APMF_API::kIdleEnd_Released);

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
            why = "VR runtime (PlayIdle is verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {   // F2b: 1.7.104 proven (39256/39257 + graph classes identical)
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (PlayIdle is verified on those three only)";
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

        // Entry confirmation. Kill switch first (review 18be6ec SEV-3.3): off = no graph read at
        // all and every play UNCONFIRMABLE, said once here.
        if (GetPrivateProfileIntA("Idle", "bIdleConfirm", 1, kIni) == 0) {
            g_entryNotArmedWhy.store(APMF_API::kIdleUnconf_ConfirmOff, std::memory_order_release);
            g_entryNotArmed.store("[Idle] bIdleConfirm=0 in Data/SKSE/Plugins/APMF.ini", std::memory_order_release);
            spdlog::warn("[ch.12] idle entry confirmation OFF -- [Idle] bIdleConfirm=0. No behaviour-graph read; every "
                         "idle play is logged UNCONFIRMABLE and its claim is never ended for not playing.");
            return;
        }
        // The three vtables the graph read identifies objects by (the rows Travel's gate probe 2
        // verifies too). A refusal leaves every play UNCONFIRMABLE.
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
        {   // the world-tick clock advances on every call, entries or not
            const std::uint64_t t = apmf::clock::MonotonicMs();
            if (g_lastTickMs != 0) g_worldMs += (std::min)(t - g_lastTickMs, kTickClampMs);
            g_lastTickMs = t;
            ++g_worldFrames;
            g_worldMsPub.store(g_worldMs, std::memory_order_relaxed);
            // ABI v24: the state records outlive their entries by the grace, so they are pruned here.
            static std::uint64_t s_pruneMs = 0;
            if (g_stateCount.load(std::memory_order_relaxed) != 0 && t - s_pruneMs >= kStatePruneMs) {
                s_pruneMs = t;
                PruneStates();
            }
        }
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
                EndClaim(sn.id, sn.gen, "owner dead", APMF_API::kIdleEnd_ActorDied);
                continue;
            }
            // The observation line, once per accepted play (principle 5).
            const auto it = g_entries.find(sn.id);
            if (it == g_entries.end() || it->second.gen != sn.gen) continue;
            if (it->second.confirmLogged && !it->second.carriedOpen) continue;
            const Obs o = Peek(sn.id);
            if (!o.armed || o.gen != sn.gen) continue;
            Entry&              e       = it->second;
            // ABI v24 (review idle-state F1): a CARRIED play is watched past its verdict line until a
            // fresh entry (Confirmed, below) or an IdleStop. The IdleStop REFUTES the carry's premise
            // ("no IdleStop seen since"): flagged, the state stays Carried (no evidence either way for
            // this play), and the claim's liveness is unchanged (it never ended a carried play).
            if (e.carriedOpen && !o.entered && o.idleStop) {
                e.carriedOpen = false;
                StateUpdate(e.handle, sn.gen, [](StateRec& r) { r.flags |= APMF_API::kIdleInfo_CarryEnded; });
                spdlog::info("[ch.12] 0x{} idle {}: the CARRIED idle ended (IdleStop at +{} ms) with no fresh entry from "
                             "this play.",
                             Hex(sn.id), e.idleName, o.idleStopMs - o.playMs);
            }
            if (e.carriedOpen && o.entered) {   // a fresh entry, even after the carried verdict line: judge it below
                e.carriedOpen   = false;
                e.confirmLogged = false;
            }
            if (e.confirmLogged) continue;
            const std::uint64_t worldMs = g_worldMs - e.playWorldMs;
            const bool          over    = worldMs >= kConfirmWaitMs && g_worldFrames - e.playFrame >= kConfirmMinFrames;
            if (o.entered) {
                // CONFIRMED: an entry event of THIS idle arrived after the call.
                e.confirmLogged = true;
                // Evidence strength (review 18be6ec SEV-3.2): an entry event other transitions
                // raise too is WEAK evidence. Liveness is the same either way.
                // `enterTag` is the RAW event of a FRESH entry of this play (a carry never sets it).
                std::string strength = "specific evidence";
                bool        weak     = false;
                for (const auto& [name, n] : e.waitShared)
                    if (IEquals(name, o.enterTag) && n != 0) {
                        strength = fmt::format("WEAK evidence: '{}' is also raised by {} other transition(s)", name, n);
                        weak     = true;
                    }
                if (o.carried) strength += "; this play was armed with a carry, and the fresh entry supersedes it";
                // ABI v24: the same strength as a state.
                const std::uint32_t kind    = weak ? APMF_API::kIdleState_ConfirmedWeak : APMF_API::kIdleState_Confirmed;
                const std::uint64_t toEntry = o.enteredMs - o.playMs;
                StateUpdate(e.handle, sn.gen, [&](StateRec& r) {
                    r.state     = kind;
                    r.msToEntry = toEntry > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<std::uint32_t>(toEntry);
                });
                spdlog::info("[ch.12] 0x{} idle {}: ANIMATION CONFIRMED ({}) -- the graph raised the entry event '{}' at "
                             "+{} ms ({}). Events so far: {} [{}]{}.",
                             Hex(sn.id), e.idleName, strength, o.enterTag, o.enteredMs - o.playMs, e.entryText, o.events,
                             TagList(o),
                             o.idleStop ? fmt::format("; IdleStop at +{} ms, {} ms after the entry (one-shot, ended by "
                                                      "itself: compare with the clip's length)",
                                                      o.idleStopMs - o.playMs, o.idleStopMs - o.enteredMs)
                                        : std::string("; no IdleStop yet (still playing, or held)"));
            } else if (o.carried) {
                // CARRIED (review idle-state F1): the same handle re-played the same idle while its previous
                // play had an entry and no IdleStop has been seen since. An INFERENCE, not evidence: the state
                // reads kIdleState_Carried (set at the play), never CONFIRMED; a fresh entry above upgrades it,
                // an IdleStop flags kIdleInfo_CarryEnded (above). The claim stays, as it always did for a carry
                // (a transition to the state the graph is already in may raise nothing).
                if (over) {
                    e.confirmLogged = true;
                    spdlog::info("[ch.12] 0x{} idle {}: CARRIED, NOT CONFIRMED BY THIS PLAY -- no fresh entry event in {} "
                                 "ms of world time; carried from '{}' of this claim's previous play ({}). The claim "
                                 "stays. Events: {} [{}].",
                                 Hex(sn.id), e.idleName, worldMs, o.carriedEvent,
                                 o.idleStop ? fmt::format("its IdleStop came at +{} ms: the inherited idle ended",
                                                          o.idleStopMs - o.playMs)
                                            : std::string("no IdleStop seen since: the graph may still be in it"),
                                 o.events, TagList(o));
                }
            } else if (e.noTransition) {
                // The idle cannot enter this graph at all: end the claim at once.
                e.confirmLogged = true;
                spdlog::warn("[ch.12] 0x{} idle {}: ANIMATION NOT CONFIRMED -- the actor's behaviour graph has NO "
                             "transition on the idle's event ({}), so it cannot play on this actor although PlayIdle "
                             "returned true.",
                             Hex(sn.id), e.idleName, e.entryText);
                EndClaim(sn.id, sn.gen, "idle not played: the actor's graph has no transition on its event");
            } else if (!e.unconfirmable.empty()) {
                if (over) {
                    e.confirmLogged = true;
                    spdlog::warn("[ch.12] 0x{} idle {}: UNCONFIRMABLE -- {}. PlayIdle returned true; whether the clip "
                                 "played is UNKNOWN (the claim stays; held = no IdleStop since the call). Graph events "
                                 "in {} ms: {} [{}]{}.",
                                 Hex(sn.id), e.idleName, e.unconfirmable, now - o.playMs, o.events, TagList(o),
                                 o.idleStop ? fmt::format("; IdleStop at +{} ms", o.idleStopMs - o.playMs) : std::string());
                }
            } else if (over) {
                // NOT CONFIRMED: none of the idle's entry events arrived. What did arrive is someone
                // else's (the 2026-09-28 lockpick: IdleStop, tailCombatState, attackStop, Pie...).
                e.confirmLogged = true;
                StateUpdate(e.handle, sn.gen, [](StateRec& r) { r.state = APMF_API::kIdleState_NotConfirmed; });
                spdlog::warn("[ch.12] 0x{} idle {}: ANIMATION NOT CONFIRMED -- none of its entry events arrived in {} "
                             "ms of world time ({} frames, {} ms wall) ({}); PlayIdle returned true but the graph never "
                             "entered the idle. Unrelated graph events: {} [{}]{}. Harbinger ends the claim.",
                             Hex(sn.id), e.idleName, worldMs, g_worldFrames - e.playFrame, now - o.playMs, e.entryText,
                             o.events, TagList(o), StrayText(o));
                EndClaim(sn.id, sn.gen,
                         fmt::format("idle not observed playing (no entry event in {} ms of world time)", worldMs));
            }
        }
    }

    std::uint32_t GetState(APMF_API::Handle handle, APMF_API::APMF_IdleStateInfo* out) {
        APMF_API::APMF_IdleStateInfo info{};
        info.size             = sizeof(info);
        info.state            = APMF_API::kIdleState_Unknown;
        info.handle           = handle;
        info.msSincePlay      = APMF_API::kIdleMsNone;
        info.msToEntry        = APMF_API::kIdleMsNone;
        info.msSinceEnd       = APMF_API::kIdleMsNone;
        info.confirmWindowMs  = static_cast<std::uint32_t>(kConfirmWaitMs);
        if (handle != APMF_API::kInvalidHandle) {
            StateRec r{};
            bool     found = false;
            {
                std::scoped_lock lock(g_stateMx);
                if (const auto it = g_states.find(handle); it != g_states.end()) {
                    r     = it->second;
                    found = true;
                }
            }
            if (found) {
                const std::uint64_t now = g_worldMsPub.load(std::memory_order_relaxed);
                auto sat = [](std::uint64_t ms) { return ms > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<std::uint32_t>(ms); };
                info.state            = r.state;
                info.actor            = r.actor;
                info.idle             = r.idle;
                info.target           = r.target;
                info.unconfirmableWhy = r.unconfWhy;
                info.endCause         = r.endCause;
                info.flags            = r.flags;
                info.msSincePlay      = r.played ? sat(now - r.playWorldMs) : APMF_API::kIdleMsNone;
                info.msToEntry        = r.msToEntry;
                info.msSinceEnd       = r.endCause != APMF_API::kIdleEnd_None ? sat(now - r.endWorldMs) : APMF_API::kIdleMsNone;
                info.plays            = r.plays;
                info.seq              = r.seq;
            }
        }
        if (out && out->size >= APMF_API::kIdleStateInfoV24Size) {
            const std::uint32_t callerSize = out->size;
            std::memcpy(out, &info, APMF_API::kIdleStateInfoV24Size);
            out->size = callerSize;
        }
        return info.state;
    }

    void NoteFiled(APMF_API::Handle handle, RE::FormID actor, const APMF_API::APMF_Param* param) {
        if (handle == APMF_API::kInvalidHandle || !param || param->form == 0) return;
        std::scoped_lock lock(g_stateMx);
        if (g_states.contains(handle)) return;   // the game thread got there first
        EvictForInsert();
        StateRec r{};
        r.actor  = actor;
        r.idle   = param->form;
        r.target = param->target;
        Touch(r);
        g_states.emplace(handle, r);
        g_stateCount.store(g_states.size(), std::memory_order_relaxed);
    }

    void NoteRepoint(APMF_API::Handle handle, const APMF_API::APMF_Param* param) {
        if (handle == APMF_API::kInvalidHandle || !param || param->form == 0) return;
        std::scoped_lock lock(g_stateMx);
        const auto it = g_states.find(handle);
        if (it == g_states.end()) return;
        StateRec& r = it->second;
        if (r.endCause != APMF_API::kIdleEnd_None && r.endCause != APMF_API::kIdleEnd_Superseded) return;   // ended
        r.idle      = param->form;
        r.target    = param->target;
        r.state     = APMF_API::kIdleState_Pending;
        r.unconfWhy = APMF_API::kIdleUnconf_None;
        r.flags     = 0;
        r.gen       = 0;   // no Poll verdict for an older play applies any more
        r.msToEntry = APMF_API::kIdleMsNone;
        r.played    = false;
        Touch(r);
    }

    void NoteReleased(APMF_API::Handle handle) { StateEnd(handle, APMF_API::kIdleEnd_Released); }

    void ResetAll(const char* why) {
        const std::size_t n = g_entries.size();
        g_entries.clear();
        g_count.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lock(g_obsMx);
            g_obs.clear();
        }
        std::size_t ns = 0;
        {   // ABI v24: every handle of the replaced world is gone
            std::scoped_lock lock(g_stateMx);
            ns = g_states.size();
            g_states.clear();
            g_stateCount.store(0, std::memory_order_relaxed);
        }
        if (n != 0 || ns != 0)
            spdlog::info("[ch.12] {} -- dropped {} idle entr{} and {} idle state record(s).", why, n, n == 1 ? "y" : "ies",
                         ns);
    }

}

APMF_REGISTER_CHANNEL(IdleChannel);
