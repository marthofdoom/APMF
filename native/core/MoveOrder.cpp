#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/BehaviorTreeCatalog.h"
#include "core/Hook.h"
#include "core/MoveOrder.h"
#include "channels/CombatApproach.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string_view>
#include <unordered_map>

// Win32 INI read -- the same hand-declared extern every INI-gated file in this project uses.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// THE STRICT MOVEMENT ORDER (H-M1 piece P-A; MFO Docs/research/2026-11/hm1-seat-brief.md).
//
// The Movement Selector is ONE node shared by every fighter. Every 0.25 s, and at enter, it asks its ten child links in
// order for a value (vfunc 0x0A) and takes the first non-zero one (pick AE 0x85AEE0 / SE 0x7C4080 / 1.7.104 0x870110).
// While an order stands on an actor Harbinger answers four of those questions, for that actor only:
//   V0 child 0 (Essential Down, a native 10000 s wait): YES to stand.
//   V2 child 2 (Flee), V3 child 3 (Hide): NO.
//   V4 child 4 (Return To Combat Area, the native go-to-centre): YES to travel.
// Every other answer is the engine's own. Nothing is failed and no child is entered by Harbinger (no act / pop is
// touched): the selector starts and interrupts its children exactly as it does for its own reasons. The point is the
// centre of the actor's standard combat area, carried there by ch.24's S1 (channels/CombatApproach.cpp): travel is
// answered ONLY while his current area carries exactly the order's target, else the engine answers (NotCarried).
//
// B1 / B2 bracket the selector's act (0x02) and update (0x04), where the pick runs, and note thread and node in
// thread-local storage; the value seats get only `this` and answer only inside a bracket on THE node, whose identity is
// structural (IsMovementSelector), never the shared vtable or the name.
//
// WHAT THE BUILDER MAKES (read on all three, builder AE 0x8AD390 / SE 0x8166E0 / 1.7.104 0x8C2900): link +0x28 of child 0
// is the Idle<float> node itself (+0x28 = 10000.0f); link +0x28 of child 4 is a CombatBehaviorTreeLinkNode whose +0x28
// is the tree name "ReturnToCombatArea": its act finds that tree and descends to its leaf.
//
// THREADING. The seats run on the actor's own combat thread. They read the slot table (seqlock, game-thread writer) and
// write the slot's runtime block (atomics; one actor, one job). No lock, no allocation, no form lookup; the engine
// calls are the attacker handle lookup and the two value questions put to Flight and Exit Water (the engine's own next
// calls). No log except one-time lines. Everything else is game thread.
// ============================================================================

namespace apmf::moveorder {

    namespace {

        using apmf::log::Hex;
        using namespace apmf::movelogic;
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        constexpr std::size_t   kMaxSlots    = APMF_API::kMoveMaxOrders;
        constexpr std::uint64_t kQuarantineMs = 2000;   // a cleared slot is not given to another actor sooner
        constexpr std::size_t   kPhases      = 9;

        constexpr std::uint32_t kChildren = 10;
        enum Child : std::size_t { kEssentialDown = 0, kFlight = 1, kFlee = 2, kHide = 3, kReturn = 4, kExitWater = 5, kIdN = 6 };
        constexpr std::array<std::size_t, 4> kValueSeats{ kEssentialDown, kFlee, kHide, kReturn };
        constexpr std::array<const char*, kIdN> kChildName{ "Essential Down", "Flight", "Flee", "Hide", "Return To Combat Area",
                                                            "Exit Water" };
        // Children asked through their own nodes before any answer is forced: an engine YES there passes.
        struct Ask { std::size_t child; std::uint32_t bit; };
        constexpr std::array<Ask, 2> kAskChildren{ { { kFlight, APMF_API::kMovePass_Flight },
                                                     { kExitWater, APMF_API::kMovePass_ExitWater } } };

        // ---- node layout (Docs/VERIFIED-ADDRESSES.md "MoveOrder reads") ----
        constexpr std::size_t kNodeName     = 0x08;
        constexpr std::size_t kSelChildren  = 0x18;
        constexpr std::size_t kSelCount     = 0x20;
        constexpr std::size_t kLinkChild    = 0x28;
        constexpr std::size_t kIdleSeconds  = 0x28;
        constexpr std::size_t kLinkNodeTree = 0x28;
        constexpr std::size_t kSlotValue    = 0x0A;

        using Value_t = std::uint32_t (*)(void*);

        template <class T>
        T At(const void* a_p, std::size_t a_off) {
            T v;
            std::memcpy(&v, static_cast<const std::byte*>(a_p) + a_off, sizeof(T));
            return v;
        }
        std::uintptr_t Vt(const void* a_p) { return a_p ? At<std::uintptr_t>(a_p, 0) : 0; }

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_why{ "before kDataLoaded (the seats install there)" };
        // Written once in Install, before g_installed.
        std::uintptr_t                   g_vtSel = 0, g_vtIdle = 0, g_vtLinkNode = 0;
        std::array<std::uintptr_t, kIdN> g_vtLink{};
        std::uintptr_t                   g_origAct = 0, g_origUpdate = 0;
        std::array<std::uintptr_t, kIdN> g_origValue{};

        std::atomic<void*>         g_moveSel{ nullptr };      // THE node
        std::atomic<std::uint32_t> g_selSeen{ 0 };            // identifications (a second one replaces the cache)
        std::atomic<bool>          g_selRefused{ false };     // a same-named node failed the test and none passed
        std::atomic<std::uint32_t> g_strictLive{ 0 };
        std::array<std::atomic<bool>, kIdN> g_obsForced{};

        // RULE C, printed even at zero.
        std::atomic<std::uint64_t> t_brackets{ 0 }, t_stand{ 0 }, t_travel{ 0 }, t_orig{ 0 }, t_flee{ 0 }, t_hide{ 0 },
            t_foreign{ 0 }, t_readBusy{ 0 };

        // ---- the slot table ----
        struct Run {   // combat thread (the actor's own) writes; any thread reads
            std::atomic<std::uint32_t> seenSerial{ 0 };
            std::atomic<std::uint32_t> phase{ APMF_API::kMovePhase_Pending };
            std::atomic<float>         dist{ -1.0f };
            std::atomic<std::uint64_t> phaseSinceMs{ 0 }, lastPickMs{ 0 };
            std::atomic<std::uint32_t> passed{ 0 }, arrived{ 0 }, displaced{ 0 };
            std::atomic<std::uint32_t> stand{ 0 }, travel{ 0 }, orig{ 0 }, flee{ 0 }, hide{ 0 }, down{ 0 };
            std::array<std::atomic<std::uint64_t>, kPhases> phaseMs{};
            std::atomic<float>         maxStand{ 0.0f }, arriveDist{ -1.0f };
            std::atomic<std::uint64_t> walkBackMs{ 0 };
            std::atomic<std::uint32_t> walkBacks{ 0 };
        };
        struct Book {   // GAME THREAD only
            Snapshot      snap{};
            std::uint64_t publishedMs = 0, clearedMs = 0, lineMs = 0;
            std::uint32_t phase = APMF_API::kMovePhase_None, arrived = 0, displaced = 0;
            std::uint32_t stands = 0;
            bool          silentSaid = false;
        };
        std::array<Slot, kMaxSlots> g_slots{};
        std::array<Run, kMaxSlots>  g_run{};
        std::array<Book, kMaxSlots> g_book{};
        std::atomic<std::uint32_t>  g_hi{ 0 };   // one past the highest slot used since the last reset
        std::atomic<std::uint32_t>  g_gen{ 0 };
        std::uint32_t               g_serial = 0;   // GAME THREAD only

        int FindSlot(RE::FormID a_fid) {
            if (a_fid == 0) return -1;
            const std::uint32_t hi = g_hi.load(std::memory_order_acquire);
            for (std::uint32_t i = 0; i < hi; ++i)
                if (g_slots[i].Fid() == a_fid) return static_cast<int>(i);
            return -1;
        }

        const char* PhaseName(std::uint32_t a_p) {
            switch (a_p) {
            case APMF_API::kMovePhase_None:        return "none";
            case APMF_API::kMovePhase_Pending:     return "PENDING";
            case APMF_API::kMovePhase_Travelling:  return "TRAVELLING";
            case APMF_API::kMovePhase_Standing:    return "STANDING";
            case APMF_API::kMovePhase_Returning:   return "RETURNING";
            case APMF_API::kMovePhase_Blocked:     return "BLOCKED";
            case APMF_API::kMovePhase_Yielded:     return "YIELDED";
            case APMF_API::kMovePhase_NotCarried:  return "NOT CARRIED";
            case APMF_API::kMovePhase_Unreachable: return "UNREACHABLE";
            default:                               return "?";
            }
        }
        const char* ModeName(std::uint32_t a_m) {
            constexpr const char* n[] = { "Free", "Stand", "GoTo", "Kite", "Disengage", "Path" };
            return a_m < std::size(n) ? n[a_m] : "?";
        }

        // ---- which node is THE Movement Selector ----
        void* ChildOf(const void* a_node, std::size_t a_i) {
            auto* const arr = At<void* const*>(a_node, kSelChildren);
            return arr ? arr[a_i] : nullptr;
        }
        bool Structural(const void* a_node) {
            if (Vt(a_node) != g_vtSel || At<std::uint32_t>(a_node, kSelCount) != kChildren) return false;
            for (std::size_t i = 0; i < kIdN; ++i) {
                const void* link = ChildOf(a_node, i);
                if (!link || Vt(link) != g_vtLink[i]) return false;
            }
            const void* wait = At<const void*>(ChildOf(a_node, kEssentialDown), kLinkChild);
            if (Vt(wait) != g_vtIdle || At<float>(wait, kIdleSeconds) != 10000.0f) return false;
            const void* ret = At<const void*>(ChildOf(a_node, kReturn), kLinkChild);
            if (Vt(ret) != g_vtLinkNode) return false;
            const char* tree = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::byte*>(ret) + kLinkNodeTree)->c_str();
            return tree && std::string_view(tree) == "ReturnToCombatArea";
        }
        bool IsMovementSelector(const void* a_node, void* a_thread) {
            if (a_node == g_moveSel.load(std::memory_order_acquire)) return true;
            if (!Structural(a_node)) {
                if (!g_moveSel.load(std::memory_order_relaxed) && !g_selRefused.load(std::memory_order_relaxed) &&
                    Vt(a_node) == g_vtSel && At<std::uint32_t>(a_node, kSelCount) == kChildren) {
                    const char* nm = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::byte*>(a_node) + kNodeName)->c_str();
                    if (nm && std::string_view(nm) == "Movement Selector" && !g_selRefused.exchange(true)) {
                        try {
                            spdlog::error("[move] a 10-child node named 'Movement Selector' is NOT the engine's (a child link or "
                                          "its node has a foreign class): another mod rebuilt the movement tree. Strict orders "
                                          "are not carried while no engine node is seen. (Logged once.)");
                        } catch (...) {
                        }
                    }
                }
                return false;
            }
            if (g_moveSel.exchange(const_cast<void*>(a_node), std::memory_order_acq_rel) == a_node) return true;
            const std::uint32_t n = g_selSeen.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5) {
                RE::NiPointer<RE::Actor> actor;
                apmf::cbt::ResolveController(a_thread, actor);
                try {
                    spdlog::info("[move] Movement Selector identified (#{}): node {:p}, 10 children, first seen on actor 0x{} "
                                 "(principle 5).", n, a_node, Hex(actor ? actor->GetFormID() : 0));
                } catch (...) {
                }
            }
            return true;
        }

        // ---- the pick bracket ----
        struct Sel {
            void*      thread  = nullptr;
            void*      node    = nullptr;
            bool       ours    = false;
            bool       found   = false;   // Find ran
            bool       decided = false;
            Pick       pick    = Pick::kOrig;
            int        slot    = -1;
            RE::FormID fid     = 0;
        };
        thread_local Sel t_sel;

        struct Who {
            RE::CombatController*    ctl = nullptr;
            RE::NiPointer<RE::Actor> actor;
        };
        // The selector thread's own controller, [[thread+0x158]+0x20] (the read every leaf makes), and its attacker.
        Who WhoIs(void* a_thread) {
            Who   w;
            auto* ctl = apmf::cbt::ResolveController(a_thread, w.actor);
            void* own = a_thread ? reinterpret_cast<apmf::cbt::TreeControl*>(a_thread)->master_controller : nullptr;
            if (!ctl || !own || At<void*>(own, 0x20) != static_cast<void*>(ctl)) w.actor.reset();   // not a CombatController
            else w.ctl = reinterpret_cast<RE::CombatController*>(ctl);
            return w;
        }

        void Reset(Run& a_r, std::uint32_t a_serial, std::uint64_t a_now) {
            a_r.phase.store(APMF_API::kMovePhase_Pending, std::memory_order_relaxed);
            a_r.dist.store(-1.0f, std::memory_order_relaxed);
            a_r.phaseSinceMs.store(a_now, std::memory_order_relaxed);
            a_r.lastPickMs.store(a_now, std::memory_order_relaxed);
            for (auto* c : { &a_r.passed, &a_r.arrived, &a_r.displaced, &a_r.stand, &a_r.travel, &a_r.orig, &a_r.flee, &a_r.hide,
                             &a_r.down, &a_r.walkBacks })
                c->store(0, std::memory_order_relaxed);
            for (auto& p : a_r.phaseMs) p.store(0, std::memory_order_relaxed);
            a_r.maxStand.store(0.0f, std::memory_order_relaxed);
            a_r.arriveDist.store(-1.0f, std::memory_order_relaxed);
            a_r.walkBackMs.store(0, std::memory_order_relaxed);
            a_r.seenSerial.store(a_serial, std::memory_order_release);
        }

        // Book the time since the last pick to the phase it ran in, then move to `a_phase`.
        void Account(Run& a_r, std::uint32_t a_phase, std::uint64_t a_now) {
            const std::uint32_t was = a_r.phase.load(std::memory_order_relaxed);
            const std::uint64_t gap = std::min(a_now - std::min(a_now, a_r.lastPickMs.load(std::memory_order_relaxed)), MoveTuning::kPickGapMs);
            if (was < kPhases) a_r.phaseMs[was].fetch_add(gap, std::memory_order_relaxed);
            if (was == APMF_API::kMovePhase_Returning) a_r.walkBackMs.fetch_add(gap, std::memory_order_relaxed);
            a_r.lastPickMs.store(a_now, std::memory_order_relaxed);
            if (was == a_phase) return;
            a_r.phase.store(a_phase, std::memory_order_relaxed);
            a_r.phaseSinceMs.store(a_now, std::memory_order_relaxed);
        }

        // Find the order of the actor this bracket belongs to (once per bracket).
        bool Find(Who* a_who) {
            if (!t_sel.found) {
                t_sel.found   = true;
                const Who w   = WhoIs(t_sel.thread);
                t_sel.fid     = w.actor ? w.actor->GetFormID() : 0;
                t_sel.slot    = w.ctl ? FindSlot(t_sel.fid) : -1;
                if (a_who) *a_who = w;
            } else if (a_who && t_sel.slot >= 0) {
                *a_who = WhoIs(t_sel.thread);
            }
            return t_sel.slot >= 0;
        }

        Pick DecideNow() {
            Who who;
            if (!Find(&who) || !who.ctl || !who.actor) return Pick::kOrig;
            Snapshot s;
            if (!g_slots[static_cast<std::size_t>(t_sel.slot)].Read(t_sel.fid, s)) {
                t_readBusy.fetch_add(1, std::memory_order_relaxed);
                return Pick::kOrig;
            }
            if (!Strict(s.mode)) return Pick::kOrig;
            Run&                r   = g_run[static_cast<std::size_t>(t_sel.slot)];
            const std::uint64_t now = apmf::combatapproach::WorldMs();
            if (r.seenSerial.load(std::memory_order_relaxed) != s.serial) Reset(r, s.serial, now);

            // The engine's own answers that an order never overrides (asked through their own nodes, so hooks compose).
            std::uint32_t passed = 0;
            for (const auto& a : kAskChildren) {
                void* const link = ChildOf(t_sel.node, a.child);
                const auto  fn   = reinterpret_cast<Value_t>((*reinterpret_cast<std::uintptr_t**>(link))[kSlotValue]);
                if (fn(link) != 0) passed |= a.bit;
            }
            r.passed.store(passed, std::memory_order_relaxed);

            // Travel is the engine's path to ITS CURRENT area's centre: only while that is this order's target.
            const auto& tgt = s.points[0];   // Path's waypoint is P-D
            using apmf::combatapproach::Carry;
            const Carry carry = passed ? Carry::kNot : apmf::combatapproach::CarryOf(who.ctl, tgt.data(), s.leashR);
            if (carry != Carry::kCarried) {
                Account(r, passed || carry == Carry::kOutranked ? APMF_API::kMovePhase_Yielded : APMF_API::kMovePhase_NotCarried, now);
                r.orig.fetch_add(1, std::memory_order_relaxed);
                t_orig.fetch_add(1, std::memory_order_relaxed);
                return Pick::kOrig;
            }

            const auto& p  = who.actor->data.location;
            const float dx = p.x - tgt[0], dy = p.y - tgt[1], dz = p.z - tgt[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            Runtime     rt;
            rt.phase          = r.phase.load(std::memory_order_relaxed);
            rt.arrivedCount   = r.arrived.load(std::memory_order_relaxed);
            rt.displacedCount = r.displaced.load(std::memory_order_relaxed);
            rt.phaseSinceMs   = r.phaseSinceMs.load(std::memory_order_relaxed);
            const std::uint32_t was  = rt.phase;
            const Pick          pick = Step(Order{ s.mode, s.flags, s.leashR, s.arriveR }, rt, dist, now);
            r.dist.store(dist, std::memory_order_relaxed);
            r.arrived.store(rt.arrivedCount, std::memory_order_relaxed);
            r.displaced.store(rt.displacedCount, std::memory_order_relaxed);
            if (was == APMF_API::kMovePhase_Standing) r.maxStand.store(std::max(r.maxStand.load(std::memory_order_relaxed), dist), std::memory_order_relaxed);
            if (was != APMF_API::kMovePhase_Standing && rt.phase == APMF_API::kMovePhase_Standing) r.arriveDist.store(dist, std::memory_order_relaxed);
            if (was == APMF_API::kMovePhase_Returning && rt.phase == APMF_API::kMovePhase_Standing) r.walkBacks.fetch_add(1, std::memory_order_relaxed);
            Account(r, rt.phase, now);
            switch (pick) {
            case Pick::kStand:  r.stand.fetch_add(1, std::memory_order_relaxed);  t_stand.fetch_add(1, std::memory_order_relaxed);  break;
            case Pick::kTravel: r.travel.fetch_add(1, std::memory_order_relaxed); t_travel.fetch_add(1, std::memory_order_relaxed); break;
            default:            r.orig.fetch_add(1, std::memory_order_relaxed);   t_orig.fetch_add(1, std::memory_order_relaxed);   break;
            }
            return pick;
        }
        Pick Decide() {
            if (!t_sel.decided) {
                t_sel.decided = true;   // first: the questions DecideNow puts to other children are never re-entered
                t_sel.pick    = DecideNow();
            }
            return t_sel.pick;
        }

        void Forced(std::size_t a_child, const char* a_what) {
            if (g_obsForced[a_child].load(std::memory_order_relaxed) || g_obsForced[a_child].exchange(true)) return;
            try {
                spdlog::info("[move] seat OBSERVED: child {} ({}) forced {} for 0x{} (first of the session; principle 5).", a_child,
                             kChildName[a_child], a_what, Hex(t_sel.fid));
            } catch (...) {
            }
        }

        template <std::size_t I>
        std::uint32_t ValueThunk(void* a_link) {
            if (!a_link || Vt(a_link) != g_vtLink[I]) {   // foreign object: the benign default (#17)
                t_foreign.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            const std::uint32_t a = reinterpret_cast<Value_t>(g_origValue[I])(a_link);
            if (!t_sel.ours || a_link != ChildOf(t_sel.node, I)) return a;
            if constexpr (I == kEssentialDown) {
                if (a != 0) {   // really down: the engine's own wait
                    if (Find(nullptr)) {
                        Run& r = g_run[static_cast<std::size_t>(t_sel.slot)];
                        r.passed.store(APMF_API::kMovePass_EssentialDown, std::memory_order_relaxed);
                        r.down.fetch_add(1, std::memory_order_relaxed);
                    }
                    return a;
                }
                if (Decide() != Pick::kStand) return 0;
                Forced(I, "YES (stand)");
                return 1;
            } else if constexpr (I == kReturn) {
                if (Decide() != Pick::kTravel) return a;
                Forced(I, "YES (travel)");
                return 1;
            } else {
                if (Decide() == Pick::kOrig) return a;
                if (a != 0) {
                    Run& r = g_run[static_cast<std::size_t>(t_sel.slot)];
                    (I == kFlee ? r.flee : r.hide).fetch_add(1, std::memory_order_relaxed);
                    (I == kFlee ? t_flee : t_hide).fetch_add(1, std::memory_order_relaxed);
                    Forced(I, "NO (the order beats it)");
                }
                return 0;
            }
        }

        template <class F>
        void Bracket(void* a_node, void* a_thread, F a_call) {
            const Sel saved = t_sel;
            t_sel           = Sel{};
            t_sel.thread    = a_thread;
            t_sel.node      = a_node;
            t_sel.ours      = a_thread && IsMovementSelector(a_node, a_thread);
            if (t_sel.ours) t_brackets.fetch_add(1, std::memory_order_relaxed);
            a_call();
            t_sel = saved;
        }
        void* SelActThunk(void* a_node, void* a_thread) {
            const auto orig = reinterpret_cast<apmf::cbt::Act_t>(g_origAct);
            if (Vt(a_node) != g_vtSel) {   // foreign object: no original to recover
                t_foreign.fetch_add(1, std::memory_order_relaxed);
                return a_thread;
            }
            if (g_strictLive.load(std::memory_order_relaxed) == 0) return orig(a_node, a_thread);
            void* ret = nullptr;
            Bracket(a_node, a_thread, [&] { ret = orig(a_node, a_thread); });
            return ret;
        }
        void SelUpdateThunk(void* a_node, void* a_thread) {
            const auto orig = reinterpret_cast<apmf::cbt::Update_t>(g_origUpdate);
            if (Vt(a_node) != g_vtSel) {
                t_foreign.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (g_strictLive.load(std::memory_order_relaxed) == 0) return orig(a_node, a_thread);
            Bracket(a_node, a_thread, [&] { orig(a_node, a_thread); });
        }

        // ---- install guards ----
        constexpr std::uint8_t kActAE[]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                               0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };
        constexpr std::uint8_t kActSE[]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x4C, 0x8D, 0x41, 0x28, 0x48, 0x8B, 0xCB };
        constexpr std::uint8_t kUpdateAE[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x0D };
        constexpr std::uint8_t kUpdateSE[] = { 0x8B, 0x8A, 0x20, 0x01, 0x00, 0x00, 0x48, 0x03, 0x0A, 0xE9 };
        constexpr std::uint8_t kForward[]  = { 0x48, 0x8B, 0x49, 0x28, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x10 };

        // The slot's CURRENT target is in the game's code and starts with these bytes: the engine's own function, not
        // replaced by anybody (a replaced slot refuses the facet, it is never chained blind).
        bool EngineTarget(std::uintptr_t a_vt, std::size_t a_slot, std::span<const std::uint8_t> a_head, const char* a_what) {
            const auto t    = *reinterpret_cast<const std::uintptr_t*>(a_vt + sizeof(void*) * a_slot);
            const auto text = REL::Module::get().segment(REL::Segment::textx);
            const bool in   = t >= text.address() && t + 32 < text.address() + text.size();
            if (in && std::equal(a_head.begin(), a_head.end(), reinterpret_cast<const std::uint8_t*>(t))) return true;
            spdlog::error("[move] {} slot 0x{:X} does not hold the engine's own function ({}): refused.", a_what, a_slot,
                          in ? "its first bytes differ" : "another module replaced it");
            return false;
        }

        void Refuse(const char* a_why, bool a_error) {
            g_why.store(a_why, std::memory_order_release);
            spdlog::log(a_error ? spdlog::level::err : spdlog::level::warn,
                        "[move] strict movement orders NOT SERVED -- {}. kApproach_Strict claims are REFUSED (plain ch.24 claims "
                        "are not affected).", a_why);
        }

        template <std::size_t I>
        bool InstallValue(REL::VariantID a_id, const void* a_td) {
            std::unordered_map<std::uintptr_t, std::uintptr_t> o;
            // The original is stored BEFORE the write: another thread may ask the link the instant it lands.
            g_origValue[I] = *reinterpret_cast<const std::uintptr_t*>(g_vtLink[I] + sizeof(void*) * kSlotValue);
            const std::array<REL::VariantID, 1> one{ a_id };
            return allowance::InstallOnVtables(one, kSlotValue, &ValueThunk<I>, a_td, "moveorder-value", o) == 1;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) return Refuse("VR runtime (the seats are verified on 1.6.1170, 1.5.97 and 1.7.104 only)", false);
        if (!allowance::RuntimeSupported())
            return Refuse("runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (the seats are verified on those three only)", false);
        if (GetPrivateProfileIntA("MoveOrder", "bStrictOrders", 1, kIni) == 0)
            return Refuse("[MoveOrder] bStrictOrders=0 in Data/SKSE/Plugins/APMF.ini", false);

        // SE id, AE id (1.7.104 resolves the AE id through the fork's table; the pinned fork 961e880a carries all).
        const std::array<REL::VariantID, kIdN> linkId{ {
            REL::VariantID(266897, 213963, 0), REL::VariantID(266898, 213965, 0), REL::VariantID(266899, 213967, 0),
            REL::VariantID(266656, 213655, 0), REL::VariantID(266900, 213969, 0), REL::VariantID(266901, 213971, 0) } };
        REL::VariantID selId{}, idleId{};
        for (const auto& l : apmf::cbt::kParamLeaves) {
            if (std::string_view(l.name) == "CombatBehaviorDynamicSelector_ConditionalChildSelector<float>") selId = l.vtbl;
            if (std::string_view(l.name) == "CombatBehaviorIdle<float>") idleId = l.vtbl;
        }
        static_assert(apmf::combatapproach::kPointCoordBound == kCoordBound);

        // VERIFY EVERYTHING BEFORE WRITING ANYTHING.
        REL::Relocation<void*> td{ apmf::cbt::RTTI_CombatBehaviorTreeNode };
        bool ok = allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(td.get()), "MoveOrder RTTI base");
        auto row = [&](const REL::VariantID& a_id, std::uintptr_t& a_out, const char* a_name) {
            a_out = REL::Relocation<std::uintptr_t>{ a_id }.address();
            if (!allowance::SeatVerified(a_out, a_name)) { ok = false; return; }
            if (!allowance::DerivesFrom(a_out, td.get())) {
                spdlog::error("[move] {} does not derive CombatBehaviorTreeNode: refused.", a_name);
                ok = false;
            }
        };
        row(selId, g_vtSel, "MoveOrder.Selector (0x02 / 0x04)");
        row(idleId, g_vtIdle, "MoveOrder.Idle<float> (identity)");
        row(RE::VTABLE_CombatBehaviorTreeLinkNode[0], g_vtLinkNode, "MoveOrder.LinkNode (identity)");
        constexpr std::array<const char*, kIdN> rowName{ "MoveOrder.Link.EssentialDown", "MoveOrder.Link.Flight (identity)",
                                                         "MoveOrder.Link.Flee", "MoveOrder.Link.Hide", "MoveOrder.Link.Return",
                                                         "MoveOrder.Link.ExitWater (identity)" };
        for (std::size_t i = 0; i < kIdN; ++i) row(linkId[i], g_vtLink[i], rowName[i]);
        if (!ok) return Refuse("the address self-check refused a row (listed above)", true);

        const bool se = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97);
        ok = EngineTarget(g_vtSel, 0x02, se ? std::span<const std::uint8_t>(kActSE) : std::span<const std::uint8_t>(kActAE), "the selector vtable");
        ok = EngineTarget(g_vtSel, 0x04, se ? std::span<const std::uint8_t>(kUpdateSE) : std::span<const std::uint8_t>(kUpdateAE), "the selector vtable") && ok;
        for (const auto i : kValueSeats) {
            ok = EngineTarget(g_vtLink[i], 0x02, kForward, kChildName[i]) && ok;     // link +0x28 is the child
            ok = EngineTarget(g_vtLink[i], kSlotValue, {}, kChildName[i]) && ok;     // in the game's own code
        }
        if (!ok) return Refuse("a byte guard failed (listed above)", true);

        std::unordered_map<std::uintptr_t, std::uintptr_t> oAct, oUpd;
        const std::array<REL::VariantID, 1> sel{ selId };
        g_origAct    = *reinterpret_cast<const std::uintptr_t*>(g_vtSel + sizeof(void*) * 0x02);
        g_origUpdate = *reinterpret_cast<const std::uintptr_t*>(g_vtSel + sizeof(void*) * 0x04);
        ok = InstallValue<kEssentialDown>(linkId[kEssentialDown], td.get());
        ok = InstallValue<kFlee>(linkId[kFlee], td.get()) && ok;
        ok = InstallValue<kHide>(linkId[kHide], td.get()) && ok;
        ok = InstallValue<kReturn>(linkId[kReturn], td.get()) && ok;
        ok = allowance::InstallOnVtables(sel, 0x02, &SelActThunk, td.get(), "moveorder-act", oAct) == 1 && ok;
        ok = allowance::InstallOnVtables(sel, 0x04, &SelUpdateThunk, td.get(), "moveorder-update", oUpd) == 1 && ok;
        // Unreachable after the checks above. A partial install stays inert (every thunk chains: no order can go live).
        if (!ok) return Refuse("a seat did not install after it verified (listed above)", true);

        const auto base = REL::Module::get().base();
        spdlog::info("[move] B1 / B2 selector act / update brackets on vtable RVA 0x{:X} (act 0x{:X}, update 0x{:X}).", g_vtSel - base,
                     g_origAct - base, g_origUpdate - base);
        for (const auto i : kValueSeats)
            spdlog::info("[move] value seat child {} ({}) on vtable RVA 0x{:X} (engine value 0x{:X}).", i, kChildName[i], g_vtLink[i] - base,
                         g_origValue[i] - base);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[move] 6 seats installed, strict orders SERVED while ch.24 is (ABI v48 piece P-A: Stand). ch.7's categories "
                     "name attack and cast leaves only: the wait (Idle<float>) and the Return To Combat Area leaf are in none, so no "
                     "ch.7 deny can fail a child the selector starts here.");
    }

    bool Served() { return g_installed.load(std::memory_order_acquire) && apmf::combatapproach::Installed(); }
    const char* NotServedReason() {
        if (!g_installed.load(std::memory_order_acquire)) return g_why.load(std::memory_order_acquire);
        return apmf::combatapproach::NotInstalledReason();
    }
    std::uint32_t Caps() { return Served() ? static_cast<std::uint32_t>(APMF_API::kMoveCaps_Strict) : 0u; }
    std::uint32_t NextGen() { return g_gen.fetch_add(1, std::memory_order_relaxed) + 1; }

    bool HasRoom(RE::FormID a_fid) {
        return g_strictLive.load(std::memory_order_relaxed) < kMaxSlots || FindSlot(a_fid) >= 0;
    }

    bool Publish(Snapshot a_s, std::uint64_t a_worldMs) {
        int i = FindSlot(a_s.fid);
        if (i >= 0) {
            Book& b = g_book[static_cast<std::size_t>(i)];
            a_s.serial = b.snap.gen == a_s.gen ? b.snap.serial : ++g_serial;
            if (a_s == b.snap) return true;
            const bool fresh = a_s.serial != b.snap.serial;
            b.snap = a_s;
            g_slots[static_cast<std::size_t>(i)].Write(a_s);
            if (!fresh) return true;   // the carried point moved (a ch.23 leash clamp): the same order
        } else {
            for (std::size_t k = 0; k < kMaxSlots && i < 0; ++k)
                if (g_book[k].snap.fid == 0 && (g_book[k].clearedMs == 0 || a_worldMs - g_book[k].clearedMs >= kQuarantineMs))
                    i = static_cast<int>(k);
            for (std::size_t k = 0; k < kMaxSlots && i < 0; ++k)
                if (g_book[k].snap.fid == 0) i = static_cast<int>(k);
            if (i < 0) return false;
            a_s.serial = ++g_serial;
            g_book[static_cast<std::size_t>(i)]      = Book{};
            g_book[static_cast<std::size_t>(i)].snap = a_s;
            g_slots[static_cast<std::size_t>(i)].Write(a_s);
            if (g_hi.load(std::memory_order_relaxed) < static_cast<std::uint32_t>(i) + 1)
                g_hi.store(static_cast<std::uint32_t>(i) + 1, std::memory_order_release);
            g_strictLive.fetch_add(1, std::memory_order_relaxed);
        }
        Book& b       = g_book[static_cast<std::size_t>(i)];
        b.publishedMs = a_worldMs;
        b.lineMs      = a_worldMs;
        b.phase       = APMF_API::kMovePhase_Pending;
        b.arrived = b.displaced = b.stands = 0;
        b.silentSaid = false;
        spdlog::info("[move] 0x{} order #{} PUBLISHED: {} at {:.0f},{:.0f},{:.0f}, leash {:.0f}, arrive {:.0f} (claim h={}, slot {}).",
                     Hex(a_s.fid), a_s.gen, ModeName(a_s.mode), a_s.points[0][0], a_s.points[0][1], a_s.points[0][2], a_s.leashR,
                     a_s.arriveR, a_s.owner, i);
        return true;
    }

    void Clear(RE::FormID a_fid, const char* a_why, std::uint64_t a_worldMs) {
        const int i = FindSlot(a_fid);
        if (i < 0) return;
        const auto k = static_cast<std::size_t>(i);
        const Book b = g_book[k];
        g_slots[k].Write(Snapshot{});
        g_book[k]           = Book{};
        g_book[k].clearedMs = a_worldMs;
        g_strictLive.fetch_sub(1, std::memory_order_relaxed);
        const Run& r = g_run[k];
        if (r.seenSerial.load(std::memory_order_acquire) != b.snap.serial) {
            spdlog::info("[move] 0x{} order #{} CLEARED ({}): the engine never asked for his movement under it.", Hex(a_fid), b.snap.gen, a_why);
            return;
        }
        const auto ms = [&](std::size_t p) { return r.phaseMs[p].load(std::memory_order_relaxed); };
        const std::uint32_t wb = r.walkBacks.load(std::memory_order_relaxed);
        spdlog::info("[move] 0x{} order #{} SUMMARY ({}): {} for {} ms; travelling {} / standing {} / returning {} / yielded {} / not "
                     "carried {} ms; picks stand {} / travel {} / engine {}; max dist while standing {:.0f}, last arrival at {:.0f}; "
                     "displaced {}, walked back {} (mean {} ms); Flee denied {}, Hide denied {}, essential down {}.",
                     Hex(a_fid), b.snap.gen, a_why, ModeName(b.snap.mode), a_worldMs - b.publishedMs, ms(APMF_API::kMovePhase_Travelling),
                     ms(APMF_API::kMovePhase_Standing), ms(APMF_API::kMovePhase_Returning), ms(APMF_API::kMovePhase_Yielded),
                     ms(APMF_API::kMovePhase_NotCarried), r.stand.load(), r.travel.load(), r.orig.load(), r.maxStand.load(),
                     r.arriveDist.load(), r.displaced.load(), wb, wb ? r.walkBackMs.load() / wb : 0, r.flee.load(), r.hide.load(),
                     r.down.load());
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        for (std::size_t k = 0; k < kMaxSlots; ++k) {
            if (g_book[k].snap.fid == 0) { g_book[k].clearedMs = 0; continue; }
            ++n;
            g_slots[k].Write(Snapshot{});
            g_book[k] = Book{};
        }
        g_strictLive.store(0, std::memory_order_relaxed);
        g_hi.store(0, std::memory_order_release);
        if (n != 0) spdlog::info("[move] {} -- dropped {} strict order(s).", a_why, n);
    }

    void PollLines(RE::FormID a_fid, std::uint64_t a_worldMs) {
        const int i = FindSlot(a_fid);
        if (i < 0) return;
        Book&      b = g_book[static_cast<std::size_t>(i)];
        const Run& r = g_run[static_cast<std::size_t>(i)];
        if (r.seenSerial.load(std::memory_order_acquire) != b.snap.serial) {
            if (!b.silentSaid && a_worldMs - b.publishedMs > MoveTuning::kSilentMs) {
                b.silentSaid = true;
                spdlog::error("[move] 0x{} order #{} is {} s old and the selector was never bracketed for him (brackets this session {}, "
                              "node {}): the order does NOTHING.", Hex(a_fid), b.snap.gen, MoveTuning::kSilentMs / 1000, t_brackets.load(),
                              g_moveSel.load() ? "identified" : "never identified");
            }
            return;
        }
        const std::uint32_t phase = r.phase.load(std::memory_order_relaxed), arrived = r.arrived.load(std::memory_order_relaxed),
                            disp  = r.displaced.load(std::memory_order_relaxed);
        const float dist = r.dist.load(std::memory_order_relaxed);
        if (disp != b.displaced) {
            b.displaced = disp;
            spdlog::info("[move] 0x{} DISPLACED (#{}): {:.0f} from the point, past the leash {:.0f}; walking back.", Hex(a_fid), disp, dist,
                         b.snap.leashR);
        }
        if (phase != b.phase) {
            const bool back = b.phase == APMF_API::kMovePhase_Returning;
            if (phase == APMF_API::kMovePhase_Standing) {
                ++b.stands;
                spdlog::info("[move] 0x{} {}: {:.0f} from the point, {} ms since the order went live.", Hex(a_fid), back ? "RETURNED" : "ARRIVED",
                             dist, a_worldMs - b.publishedMs);
            } else {
                spdlog::info("[move] 0x{} {} -> {} (dist {:.0f}, engine passed 0x{:X}).", Hex(a_fid), PhaseName(b.phase), PhaseName(phase), dist,
                             r.passed.load(std::memory_order_relaxed));
            }
            b.phase = phase;
        }
        b.arrived = arrived;
        if (a_worldMs - b.lineMs < MoveTuning::kLineMs) return;
        b.lineMs = a_worldMs;
        spdlog::info("[move] 0x{} {} {}: dist {:.0f} of {:.0f},{:.0f},{:.0f}, leash {:.0f}, arrive {:.0f}; picks stand {} / travel {} / engine "
                     "{}; Flee denied {}, Hide denied {}, displaced {}, blocks 0 (P-B).", Hex(a_fid), ModeName(b.snap.mode), PhaseName(phase), dist,
                     b.snap.points[0][0], b.snap.points[0][1], b.snap.points[0][2], b.snap.leashR, b.snap.arriveR, r.stand.load(), r.travel.load(),
                     r.orig.load(), r.flee.load(), r.hide.load(), disp);
    }

    void HeartbeatLine() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        spdlog::info("[move] heartbeat: {} live order(s); selector brackets {}, identified {} time(s){}; answers stand {} / travel {} / engine {}; "
                     "Flee denied {}, Hide denied {}; foreign objects {}, busy slot reads {}.", g_strictLive.load(), t_brackets.load(),
                     g_selSeen.load(), g_selRefused.load() ? " (a foreign 'Movement Selector' was refused)" : "", t_stand.load(), t_travel.load(),
                     t_orig.load(), t_flee.load(), t_hide.load(), t_foreign.load(), t_readBusy.load());
    }

    std::uint32_t PhaseOf(RE::FormID a_fid) { return GetState(a_fid, nullptr); }

    std::uint32_t GetState(RE::FormID a_fid, APMF_API::APMF_MoveOrderInfo* a_out) {
        APMF_API::APMF_MoveOrderInfo info{};
        info.size = sizeof(info);
        info.dist = -1.0f;
        Snapshot  s;
        const int i = FindSlot(a_fid);
        if (i >= 0 && g_slots[static_cast<std::size_t>(i)].Read(a_fid, s)) {
            const Run& r     = g_run[static_cast<std::size_t>(i)];
            info.mode        = s.mode;
            info.flags       = s.flags;
            info.ownerHandle = s.owner;
            info.gen         = s.gen;
            if (r.seenSerial.load(std::memory_order_acquire) != s.serial) {
                info.phase = g_selRefused.load(std::memory_order_relaxed) && !g_moveSel.load(std::memory_order_relaxed)
                                 ? APMF_API::kMovePhase_NotCarried : APMF_API::kMovePhase_Pending;
            } else {
                info.phase          = r.phase.load(std::memory_order_relaxed);
                info.dist           = r.dist.load(std::memory_order_relaxed);
                const auto now      = apmf::combatapproach::WorldMs();
                info.sinceMs        = static_cast<std::uint32_t>(now - std::min(now, r.phaseSinceMs.load(std::memory_order_relaxed)));
                info.passedThrough  = r.passed.load(std::memory_order_relaxed);
                info.arrivedCount   = r.arrived.load(std::memory_order_relaxed);
                info.displacedCount = r.displaced.load(std::memory_order_relaxed);
            }
        }
        if (a_out && a_out->size >= APMF_API::kMoveOrderInfoV48Size) {
            const std::uint32_t callerSize = a_out->size;
            std::memcpy(a_out, &info, APMF_API::kMoveOrderInfoV48Size);
            a_out->size = callerSize;
        }
        return info.phase;
    }

    std::uint32_t SetOrder(APMF_API::Handle a_claim, const APMF_API::APMF_MoveOrder* a_order) {
        using namespace APMF_API;
        if (!apmf::hook::OnMainThread()) return kMoveOrder_WrongThread;
        if (!Served()) return kMoveOrder_NotInstalled;
        std::uint32_t flags = 0;
        float         r     = 0.0f;
        if (!apmf::combatapproach::FindClaim(a_claim, flags, r)) return kMoveOrder_BadHandle;
        if ((flags & kApproach_Strict) == 0) return kMoveOrder_NotStrict;
        Order o;
        if (const std::uint32_t v = Validate(a_order, r, Caps(), o); v != kMoveOrder_Ok) return v;
        // Piece P-A serves the implicit Stand order only; changing a live order is P-C. Said, never faked.
        static std::atomic<bool> s_said{ false };
        if (!s_said.exchange(true))
            spdlog::warn("[move] SetMoveOrder: this build serves the implicit Stand order only (mode {} answered ModeNotServed; logged "
                         "once).", ModeName(o.mode));
        return kMoveOrder_ModeNotServed;
    }

}
