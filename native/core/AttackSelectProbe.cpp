#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/Allowance.h"
#include "core/BehaviorTreeCatalog.h"
#include "core/AttackSelectProbe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Win32 INI read -- the same hand-declared extern every INI-gated file in this project uses.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// [Probe] bAttackSelect -- PASSIVE ATTACK-SELECTION PROBE (ClickUp 86e3bw775, 2026-10-27).
//
// WHY. The combat-behaviour facet (an NPC's attack selection composed with a client's
// declared intent) is designed against engine decision points read from the disassembly.
// Principle 5: a path the disassembly shows is not yet a path that runs. This probe shows,
// per combatant in a real fight, which of those points run and what they choose, before
// any seat is built on them. It changes nothing.
//
// WHAT THE ENGINE DOES (read on the unpacked 1.6.1170 executable; the offsets this file
// reads were checked on 1.5.97 and 1.7.104 as noted; agentlog apmf-combat-behaviour-facet.md):
//
//   1. THE HAND / BASH CONTEXT. `CombatBehaviorTreeCreateContextNode2<CombatBehaviorContextMelee,
//      MemberFunc<EquipContext::GetItem>, ATTACK_TYPE>` (vtable AE 0x18E3EE8 id 213763 / SE
//      0x169E440 id 266745 / 1.7.104 0x19624B0) and its `<CombatBehaviorContextBlock, ...>`
//      twin (AE 0x18DB4B8 id 212569 / SE 0x1694C78 id 265984 / 1.7.104 0x1959A80). Their
//      act() (slot 0x02; AE 0x8A5100 / 0x86C3B0) reads the node's ATTACK_TYPE at node+0x30
//      (AE 0x8A5183 `mov r8d,[rsi+0x30]`; SE 0x80E90D `lea r9,[rcx+0x30]`; 1.7.104 0x8BA653),
//      pushes a 0x40-byte context on the thread's data stack, constructs it (AE 0x8A25B0:
//      ctx+0x04 = ATTACK_TYPE), points the thread's context window (+0x128 header, +0x130
//      offset) at it and descends to the first child (AE 0x85DDC0: cur_node = child, phase 2).
//      The MELEE node then GATHERS (AE 0x8A3960 / SE 0x80D1D0): it walks the actor's attack
//      data map and keeps an entry when (flags bit 1, bash) == (type == 1) and (flags bit 3,
//      left) == (type == 2), pushing 0x30-byte candidates into an array at ctx+0x18 whose
//      count is the u32 at ctx+0x28. So type 0 = the RIGHT-hand attacks, 1 = BASH, 2 = the
//      LEFT-hand attacks, and ctx+0x28 is the size of the pool the pick draws from.
//      THE PROBE: a pass-through thunk at slot 0x02 of those two vtables (the only seats this
//      probe adds; rows CtxNode.Melee / CtxNode.Block). It reads node+0x30 before the call
//      and, after it, the new context's type (ctx+0x04, must equal the node's) and the melee
//      pool (ctx+0x28), bounded by the data-stack header's top exactly as ch.7's ranged probe
//      bounds its window read (ActionGate.cpp RangedProbeEquipLeaf).
//
//   2. THE LEAF. ch.7's act() thunk already sits on all 70 leaves; for the watched ones
//      (kWatched below) it calls BeforeAct / AfterAct around its own body. "Started" = the
//      thread still stands on the leaf after act() (cur_node, +0x138, unchanged): the
//      failure path is SetFailed(1) + Ascend, which moves cur_node to the parent.
//      CombatBehaviorAttack's act (AE 0x8A5230) pushes 1 byte and tail-jumps into its attempt
//      (AE 0x8A4080 / SE 0x80D8C0 / 1.7.104 0x8B9550): actor checks, then StartAttack
//      (AE 0x8A28B0 id 49170 / SE 0x80C020 id 48139) on the CURRENT context, read through the
//      same window (AE 0x8A41A5 / SE 0x80DA06 / 1.7.104 0x8B9675). A successful start leaves
//      the leaf running; a failed one fails and ascends.
//
//   3. THE PICK. Inside StartAttack the engine weighs every candidate (AE 0x8DCD30:
//      ATKD attackChance x a combat-style multiplier that applies only while the target is
//      staggered or, for a power attack, blocking), draws one at weighted random, and runs it
//      through CombatAnimation::Execute (AE +0x435 = 0x8A2CE5). Right after a successful
//      StartAttack the attempt reads the actor's current attack data
//      (AE 0x6B77F0: actor+0xF8 process -> +0x10 high -> +0x258 attackData; SE 0x625FE0 reads
//      actor+0xF0; 1.7.104 0x6CA270 +0xF8) and tests its flags bit 2 (power). The probe makes
//      the SAME read after a started leaf (CommonLib GetActorRuntimeData().currentProcess ->
//      high -> attackData, which resolve to those offsets) and reports the entry's event and
//      flags, and whether the pointer changed during the act (a framework that replaces the
//      Execute call site, such as SCAR, may start an attack without changing it: the field
//      line says so instead of guessing).
//
// SCOPE. iAttackSelectScope: 0 (default) = the player's teammates only; 1 = every combatant.
// The player has no combat behaviour tree and is never seen.
//
// THREADS + COST. Every record is made on a combat AI thread: one actor-handle resolve, the
// reads above, and a short leaf lock to update the actor's counters and append a bounded
// record (no engine call, no form lookup, no log under it). The main thread drains the
// records in Heartbeat() and prints. Detail lines: the first kDetailPerFight per actor per
// fight (a fight closes after kFightGapMs without a record), session cap kLineCap.
// ============================================================================

namespace apmf::atkprobe {

    namespace {

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // The leaves whose act() is an attack-selection point: melee, bash, block, ranged,
        // flying, and the magic / shout choice the same tree makes beside them.
        constexpr std::array<const char*, 15> kWatched{ {
            "CombatBehaviorAttack",
            "CombatBehaviorAttackLow",
            "CombatBehaviorAttackFromCover",
            "CombatBehaviorSpecialAttack",
            "CombatBehaviorBash",
            "CombatBehaviorBlockAttack",
            "CombatBehaviorBlock",
            "CombatBehaviorRangedAttack",
            "CombatBehaviorGroundAttack",
            "CombatBehaviorFlyingAttack",
            "CombatBehaviorPerchAttack",
            "CombatBehaviorFallbackToRanged",
            "CombatBehaviorCastImmediateSpell",
            "CombatBehaviorCastConcentrationSpell",
            "CombatBehaviorCastShout",
        } };
        // Whether the leaf's act() can start an attack, so the actor's attack data is read after it
        // (Block, FallbackToRanged and the three cast leaves do not, so they are counted only).
        constexpr std::array<bool, 15> kReadsPick{ { true, true, true, true, true, true, false, true, true, true, true,
                                                     false, false, false, false } };
        constexpr int kLeafAttack = 0;   // kWatched index of CombatBehaviorAttack (the only one read for its context)
        constexpr std::size_t kNumLeaves = kWatched.size();
        static_assert(kReadsPick.size() == kNumLeaves, "one kReadsPick entry per watched leaf");

        // Context nodes (section 1 of the file header). VariantID(SE, AE, VR); VR is refused.
        constexpr REL::VariantID kCtxMeleeVt(266745, 213763, 0);
        constexpr REL::VariantID kCtxBlockVt(265984, 212569, 0);
        constexpr std::uintptr_t kNodeAttackType = 0x30;   // u32 ATTACK_TYPE in the node
        constexpr std::uintptr_t kCtxType        = 0x04;   // u32 ATTACK_TYPE in the context
        constexpr std::uintptr_t kCtxPoolCount   = 0x28;   // u32 candidate count (melee context)

        // CombatBehaviorThread context window (ActionGate.cpp RangedProbeEquipLeaf has the evidence).
        constexpr std::uintptr_t kThreadWindowPtr = 0x128;
        constexpr std::uintptr_t kThreadWindowOff = 0x130;
        constexpr std::uintptr_t kStackTop        = 0x0C;   // data-stack header {base +0, capacity +8, top +0xC}

        constexpr std::uint64_t kSummaryMs     = 20000;
        constexpr std::uint64_t kTotalsMs      = 30000;
        constexpr std::uint64_t kFightGapMs    = 30000;
        constexpr std::uint32_t kDetailPerFight = 24;
        constexpr std::uint64_t kLineCap       = 4000;
        constexpr std::size_t   kQueueCap      = 1024;
        constexpr std::size_t   kMaxActors     = 128;
        constexpr std::size_t   kMaxPickKeys   = 24;
        constexpr std::size_t   kEventLen      = 48;

        std::atomic<bool> g_armed{ false };
        std::atomic<bool> g_ctxArmed{ false };
        bool              g_allScope = false;   // Install only, then read-only

        std::array<std::uintptr_t, kNumLeaves> g_leafVt{};   // Install only, then read-only

        struct CtxSeat { std::uintptr_t vt = 0; std::uintptr_t orig = 0; bool melee = false; };
        std::array<CtxSeat, 2> g_ctx{};                     // Install only, then read-only
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_ctxOrigMap;   // Install only (InstallOnVtables' out-map)

        enum class Kind : std::uint8_t { kLeaf, kCtxMelee, kCtxBlock };

        struct Rec
        {
            std::uint64_t ms      = 0;
            RE::FormID    fid     = 0;
            Kind          kind    = Kind::kLeaf;
            std::int8_t   leaf    = -1;
            std::uint8_t  started = 0;
            std::uint8_t  changed = 0;
            std::uint32_t ctxType = 0xFFFFFFFF;   // 0xFFFFFFFF = not read
            std::uint32_t pool    = 0xFFFFFFFF;
            std::uint32_t flags   = 0;
            char          event[kEventLen]{};
        };

        struct Stats
        {
            std::array<std::uint32_t, kNumLeaves> acts{};
            std::array<std::uint32_t, kNumLeaves> started{};
            std::array<std::uint32_t, 3>          ctxMelee{};
            std::array<std::uint64_t, 3>          poolSum{};
            std::array<std::uint32_t, 3>          ctxBlock{};
            std::uint32_t                         ctxOdd   = 0;   // a node ATTACK_TYPE outside 0..2
            std::uint32_t                         picks    = 0;
            std::uint32_t                         power    = 0;
            std::uint32_t                         bash     = 0;
            std::uint32_t                         left     = 0;
            std::uint32_t                         unchanged = 0;
            std::vector<std::pair<std::string, std::uint32_t>> pickHist;   // event -> count, bounded
            std::uint64_t lastRecMs      = 0;
            std::uint64_t lastSummaryMs  = 0;
            std::uint32_t detailThisFight = 0;
            std::uint32_t fight          = 0;
            bool          dirty          = false;
        };

        std::mutex                              g_mx;   // leaf lock: no engine call, no log under it
        std::unordered_map<RE::FormID, Stats>   g_stats;
        std::deque<Rec>                         g_queue;
        std::uint64_t                           g_queueDrops = 0;
        std::uint64_t                           g_actorDrops = 0;

        std::atomic<std::uint64_t> g_leafActs{ 0 };      // watched leaf act()s seen (any actor, before scope)
        std::atomic<std::uint64_t> g_ctxActs{ 0 };       // context-node act()s seen (any actor, before scope)
        std::atomic<std::uint64_t> g_recorded{ 0 };      // records kept (in scope)
        std::atomic<std::uint64_t> g_unresolved{ 0 };    // act()s whose actor did not resolve
        std::atomic<std::uint64_t> g_ctxBoundMiss{ 0 };  // context reads the data-stack bound refused
        std::uint64_t              g_lines         = 0;  // main thread only
        std::uint64_t              g_lastTotalsMs  = 0;  // main thread only

        const char* LeafShort(int a_idx) {
            if (a_idx < 0 || static_cast<std::size_t>(a_idx) >= kNumLeaves) return "?";
            std::string_view n = kWatched[static_cast<std::size_t>(a_idx)];
            constexpr std::string_view kPrefix = "CombatBehavior";
            if (n.starts_with(kPrefix)) n.remove_prefix(kPrefix.size());
            return n.data();   // a suffix of a string literal: still NUL-terminated
        }
        const char* TypeName(std::uint32_t a_t) {
            switch (a_t) {
            case 0:  return "Right";
            case 1:  return "Bash";
            case 2:  return "Left";
            default: return "?";
            }
        }

        // The deliberating actor: the same two +0x158 hypotheses, in the same order, as ch.7's
        // ResolveDeliberatingActor (ActionGate.cpp), kept separate so the probe adds nothing to it.
        RE::NiPointer<RE::Actor> ResolveActor(void* a_control) {
            if (!a_control) return {};
            auto* tc = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);
            void* owner = tc->master_controller;
            if (!owner) return {};
            if (auto a = reinterpret_cast<apmf::cbt::ControllerMini*>(owner)->attackerHandle.get()) return a;
            void* hop = *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(owner) + 0x20);
            if (hop) {
                if (auto b = reinterpret_cast<apmf::cbt::ControllerMini*>(hop)->attackerHandle.get()) return b;
            }
            return {};
        }

        bool InScope(RE::Actor* a_actor) {
            if (!a_actor || a_actor->IsPlayerRef()) return false;
            return g_allScope || a_actor->IsPlayerTeammate();
        }

        // The actor's current attack data: the same three reads the engine's attempt makes right
        // after a successful StartAttack (file header, section 3). nullptr when any link is null.
        const RE::BGSAttackData* CurrentAttackData(RE::Actor* a_actor) {
            if (!a_actor) return nullptr;
            auto* proc = a_actor->GetActorRuntimeData().currentProcess;
            if (!proc || !proc->high) return nullptr;
            return proc->high->attackData.get();
        }

        // The context the thread's window points at, when [ctx, ctx + a_need) lies below the
        // data-stack top. 0 on any doubt.
        std::uintptr_t WindowContext(void* a_control, std::uint32_t a_need) {
            const auto thread = reinterpret_cast<std::uintptr_t>(a_control);
            auto* const hdr   = *reinterpret_cast<std::uintptr_t* const*>(thread + kThreadWindowPtr);
            if (!hdr) return 0;
            const auto base = *hdr;
            if (!base) return 0;
            const auto off = *reinterpret_cast<const std::uint32_t*>(thread + kThreadWindowOff);
            const auto top = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(hdr) + kStackTop);
            if (static_cast<std::uint64_t>(off) + a_need > top) return 0;
            return base + off;
        }

        void CopyEvent(const RE::BGSAttackData* a_ad, char (&a_out)[kEventLen]) {
            a_out[0] = '\0';
            if (!a_ad) return;
            const char* s = a_ad->event.c_str();
            if (!s) return;
            std::size_t i = 0;
            for (; i + 1 < kEventLen && s[i]; ++i) a_out[i] = s[i];
            a_out[i] = '\0';
        }

        // Leaf lock held by the caller.
        Stats* StatsFor(RE::FormID a_fid, std::uint64_t a_now) {
            auto it = g_stats.find(a_fid);
            if (it == g_stats.end()) {
                if (g_stats.size() >= kMaxActors) {
                    ++g_actorDrops;
                    return nullptr;
                }
                it = g_stats.emplace(a_fid, Stats{}).first;
            }
            auto& s = it->second;
            if (s.lastRecMs == 0 || a_now - s.lastRecMs > kFightGapMs) {
                ++s.fight;
                s.detailThisFight = 0;
            }
            s.lastRecMs = a_now;
            s.dirty     = true;
            return &s;
        }

        void Push(const Rec& a_r) {
            if (g_queue.size() >= kQueueCap) {
                ++g_queueDrops;
                return;
            }
            g_queue.push_back(a_r);
        }

        // ---- the context-node seats (file header, section 1) ----
        void* CtxActThunk(void* a_this, void* a_control) {
            const auto vt = *reinterpret_cast<std::uintptr_t*>(a_this);
            const CtxSeat* seat = nullptr;
            for (const auto& c : g_ctx)
                if (c.vt == vt) seat = &c;
            // Installed on exactly these two vtables, whose originals were stored before the write, so a
            // seat is always found. If it ever is not, there is no act() to call: logged once, loudly.
            if (!seat || !seat->orig) {
                static std::atomic<bool> once{ false };
                if (!once.exchange(true)) spdlog::critical("[atk-sel] context thunk reached on an unknown vtable 0x{:X}", vt);
                return a_control;
            }
            auto orig = reinterpret_cast<apmf::cbt::Act_t>(seat->orig);

            if (!g_ctxArmed.load(std::memory_order_acquire)) return orig(a_this, a_control);
            g_ctxActs.fetch_add(1, std::memory_order_relaxed);

            const auto nodeType = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_this) + kNodeAttackType);
            auto actor = ResolveActor(a_control);
            void* ret  = orig(a_this, a_control);   // the engine's act(), untouched

            if (!actor) {
                g_unresolved.fetch_add(1, std::memory_order_relaxed);
                return ret;
            }
            if (!InScope(actor.get())) return ret;

            Rec r{};
            r.ms   = apmf::clock::MonotonicMs();
            r.fid  = actor->GetFormID();
            r.kind = seat->melee ? Kind::kCtxMelee : Kind::kCtxBlock;
            r.ctxType = nodeType;
            if (seat->melee) {
                if (const auto ctx = WindowContext(a_control, static_cast<std::uint32_t>(kCtxPoolCount + 4)); ctx != 0) {
                    const auto ctxType = *reinterpret_cast<const std::uint32_t*>(ctx + kCtxType);
                    if (ctxType == nodeType) r.pool = *reinterpret_cast<const std::uint32_t*>(ctx + kCtxPoolCount);
                } else {
                    g_ctxBoundMiss.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_recorded.fetch_add(1, std::memory_order_relaxed);

            std::scoped_lock lk(g_mx);
            Stats* s = StatsFor(r.fid, r.ms);
            if (!s) return ret;
            if (nodeType <= 2) {
                if (seat->melee) {
                    ++s->ctxMelee[nodeType];
                    if (r.pool != 0xFFFFFFFF) s->poolSum[nodeType] += r.pool;
                } else {
                    ++s->ctxBlock[nodeType];
                }
            } else {
                ++s->ctxOdd;
            }
            if (s->detailThisFight < kDetailPerFight) {
                ++s->detailThisFight;
                Push(r);
            }
            return ret;
        }

    }   // namespace

    bool Armed() { return g_armed.load(std::memory_order_acquire); }

    int LeafIndex(std::uintptr_t a_vtable) {
        for (std::size_t i = 0; i < kNumLeaves; ++i)
            if (g_leafVt[i] && g_leafVt[i] == a_vtable) return static_cast<int>(i);
        return -1;
    }

    void BeforeAct(int a_leaf, void* a_control, ActPre& a_pre) {
        g_leafActs.fetch_add(1, std::memory_order_relaxed);
        a_pre.leaf  = a_leaf;
        a_pre.actor = ResolveActor(a_control);
        if (!a_pre.actor) {
            g_unresolved.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!InScope(a_pre.actor.get())) {
            a_pre.actor.reset();
            return;
        }
        a_pre.fid        = a_pre.actor->GetFormID();
        if (kReadsPick[static_cast<std::size_t>(a_leaf)]) a_pre.pickBefore = CurrentAttackData(a_pre.actor.get());
    }

    void AfterAct(const ActPre& a_pre, void* a_node, void* a_control) {
        if (!a_pre.actor || a_pre.fid == 0) return;
        auto* tc = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);

        Rec r{};
        r.ms      = apmf::clock::MonotonicMs();
        r.fid     = a_pre.fid;
        r.kind    = Kind::kLeaf;
        r.leaf    = static_cast<std::int8_t>(a_pre.leaf);
        r.started = tc && tc->curNode == a_node ? 1 : 0;
        if (r.started) {
            // The attempt read its context through this window before StartAttack (AE 0x8A41A5);
            // a started Attack proves that context was the one StartAttack used.
            if (a_pre.leaf == kLeafAttack) {
                if (const auto ctx = WindowContext(a_control, static_cast<std::uint32_t>(kCtxPoolCount + 4)); ctx != 0) {
                    r.ctxType = *reinterpret_cast<const std::uint32_t*>(ctx + kCtxType);
                    r.pool    = *reinterpret_cast<const std::uint32_t*>(ctx + kCtxPoolCount);
                } else {
                    g_ctxBoundMiss.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (kReadsPick[static_cast<std::size_t>(a_pre.leaf)]) {
                const auto* ad = CurrentAttackData(a_pre.actor.get());
                r.changed = ad != a_pre.pickBefore ? 1 : 0;
                if (ad) {
                    r.flags = ad->data.flags.underlying();
                    CopyEvent(ad, r.event);
                }
            }
        }
        g_recorded.fetch_add(1, std::memory_order_relaxed);

        std::scoped_lock lk(g_mx);
        Stats* s = StatsFor(r.fid, r.ms);
        if (!s) return;
        const auto li = static_cast<std::size_t>(a_pre.leaf);
        ++s->acts[li];
        if (r.started) {
            ++s->started[li];
            if (r.event[0] != '\0' && r.changed) {
                ++s->picks;
                if (r.flags & (1u << 2)) ++s->power;
                if (r.flags & (1u << 1)) ++s->bash;
                if (r.flags & (1u << 3)) ++s->left;
                auto hit = std::find_if(s->pickHist.begin(), s->pickHist.end(),
                                        [&](const auto& p) { return p.first == r.event; });
                if (hit != s->pickHist.end()) {
                    ++hit->second;
                } else if (s->pickHist.size() < kMaxPickKeys) {
                    s->pickHist.emplace_back(r.event, 1u);
                }
            } else if (r.event[0] != '\0') {
                ++s->unchanged;
            }
        }
        if (s->detailThisFight < kDetailPerFight) {
            ++s->detailThisFight;
            Push(r);
        }
    }

    void Install() {
        if (GetPrivateProfileIntA("Probe", "bAttackSelect", 0, kIni) == 0) {
            spdlog::info("[atk-sel] NOT armed -- [Probe] bAttackSelect=0 (default). Nothing installed.");
            return;
        }
        if (REL::Module::IsVR()) {
            spdlog::warn("[atk-sel] VR runtime -- NOT armed (the leaf and context vtables are verified on SE/AE only).");
            return;
        }
        if (!allowance::RuntimeSupported()) {
            spdlog::error("[atk-sel] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- NOT armed (REFUSED).",
                          REL::Module::get().version().string("."));
            return;
        }
        static bool installed = false;
        if (installed) return;
        installed = true;
        g_allScope = GetPrivateProfileIntA("Probe", "iAttackSelectScope", 0, kIni) == 1;

        // The leaf half rides ch.7's act() thunk: only the vtable addresses are needed here.
        int leaves = 0;
        for (std::size_t w = 0; w < kNumLeaves; ++w) {
            for (const auto& l : apmf::cbt::kLeaves) {
                if (std::string_view(l.name) != kWatched[w]) continue;
                REL::Relocation<std::uintptr_t> vt{ l.vtbl };
                g_leafVt[w] = vt.address();
                ++leaves;
                break;
            }
            if (!g_leafVt[w]) spdlog::warn("[atk-sel] '{}' is not in apmf::cbt::kLeaves -- not watched.", kWatched[w]);
        }

        // The context-node seats: pass-through act() thunks, SeatVerified + RTTI-derivation checked.
        REL::Relocation<void*> expectedTD{ apmf::cbt::RTTI_CombatBehaviorTreeNode };
        // Each seat's ORIGINAL slot 0x02 is read BEFORE the thunk is written, so the thunk can never
        // run without the act() it must call (a skipped act() would unbalance the data stack). The
        // install's own "previous entry" must equal that read; a mismatch is logged loudly.
        {
            REL::Relocation<std::uintptr_t> m{ kCtxMeleeVt }, b{ kCtxBlockVt };
            g_ctx[0] = { m.address(), *reinterpret_cast<const std::uintptr_t*>(m.address() + 0x02 * sizeof(void*)), true };
            g_ctx[1] = { b.address(), *reinterpret_cast<const std::uintptr_t*>(b.address() + 0x02 * sizeof(void*)), false };
        }
        const std::array<REL::VariantID, 2> ctxVts{ kCtxMeleeVt, kCtxBlockVt };
        const int nCtx = allowance::InstallOnVtables(ctxVts, 0x02, &CtxActThunk, expectedTD.get(), "atk-probe", g_ctxOrigMap);
        for (const auto& c : g_ctx) {
            const auto it = g_ctxOrigMap.find(c.vt);
            if (it == g_ctxOrigMap.end()) {
                spdlog::warn("[atk-sel] context seat vt 0x{:X} ({}) NOT installed (refused above) -- that context is not observed.",
                             c.vt - REL::Module::get().base(), c.melee ? "melee" : "block");
            } else if (it->second != c.orig) {
                spdlog::error("[atk-sel] context seat vt 0x{:X}: slot 0x02 changed between the read and the write "
                              "(0x{:X} -> 0x{:X}) -- the thunk calls the earlier value.", c.vt - REL::Module::get().base(),
                              c.orig, it->second);
            }
        }
        g_ctxArmed.store(nCtx > 0, std::memory_order_release);
        g_armed.store(leaves > 0, std::memory_order_release);

        const auto mod = [](const char* a_n) { return REX::W32::GetModuleHandleA(a_n) != nullptr ? "y" : "n"; };
        spdlog::info("[atk-sel] ARMED (PASSIVE, [Probe] bAttackSelect=1, scope={}) -- {} of {} leaves watched through "
                     "ch.7's act() thunk; context seats installed {}/2 (melee ATTACK_TYPE + gathered pool, block "
                     "ATTACK_TYPE). Never denies or changes a call. Set bAttackSelect=0 before a release cut.",
                     g_allScope ? "every combatant" : "player teammates", leaves, kNumLeaves, nCtx);
        spdlog::info("[atk-sel] frameworks SCAR.dll={} BFCO.dll={} valhallaCombat.dll={} Precision.dll={} "
                     "OpenAnimationReplacer.dll={} DynamicAnimationReplacer.dll={} PayloadInterpreter.dll={}",
                     mod("SCAR.dll"), mod("BFCO.dll"), mod("valhallaCombat.dll"), mod("Precision.dll"),
                     mod("OpenAnimationReplacer.dll"), mod("DynamicAnimationReplacer.dll"), mod("PayloadInterpreter.dll"));
    }

    void Heartbeat() {
        if (!g_armed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();

        std::vector<Rec> batch;
        struct Sum { RE::FormID fid; Stats s; };
        std::vector<Sum> sums;
        std::uint64_t qDrops = 0, aDrops = 0;
        {
            std::scoped_lock lk(g_mx);
            batch.assign(g_queue.begin(), g_queue.end());
            g_queue.clear();
            for (auto& [fid, s] : g_stats) {
                if (!s.dirty || now - s.lastSummaryMs < kSummaryMs) continue;
                s.lastSummaryMs = now;
                s.dirty         = false;
                sums.push_back({ fid, s });
            }
            qDrops = g_queueDrops;
            aDrops = g_actorDrops;
        }

        const auto nameOf = [](RE::FormID a_fid) -> std::string {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            const char* n = a ? a->GetName() : nullptr;
            return n ? n : "";
        };

        for (const auto& r : batch) {
            if (g_lines >= kLineCap) break;
            ++g_lines;
            if (g_lines == kLineCap)
                spdlog::warn("[atk-sel] line cap ({}) reached -- detail lines stop; summaries and totals continue.", kLineCap);
            const auto name = nameOf(r.fid);
            if (r.kind == Kind::kLeaf) {
                if (r.ctxType != 0xFFFFFFFF) {
                    spdlog::info("[atk-sel] 0x{} '{}' LEAF {} started={} ctx={}({}) pool={} pick='{}' flags=0x{} power={} bash={} left={} new={}",
                                 apmf::log::Hex(r.fid), name, LeafShort(r.leaf), r.started, TypeName(r.ctxType), r.ctxType,
                                 r.pool, r.event, apmf::log::Hex(r.flags, 2), (r.flags >> 2) & 1, (r.flags >> 1) & 1,
                                 (r.flags >> 3) & 1, r.changed);
                } else if (r.started && r.event[0] != '\0') {
                    spdlog::info("[atk-sel] 0x{} '{}' LEAF {} started=1 pick='{}' flags=0x{} power={} bash={} left={} new={}",
                                 apmf::log::Hex(r.fid), name, LeafShort(r.leaf), r.event, apmf::log::Hex(r.flags, 2),
                                 (r.flags >> 2) & 1, (r.flags >> 1) & 1, (r.flags >> 3) & 1, r.changed);
                } else {
                    spdlog::info("[atk-sel] 0x{} '{}' LEAF {} started={}", apmf::log::Hex(r.fid), name, LeafShort(r.leaf), r.started);
                }
            } else {
                spdlog::info("[atk-sel] 0x{} '{}' CONTEXT {} type={}({}){}", apmf::log::Hex(r.fid), name,
                             r.kind == Kind::kCtxMelee ? "melee" : "block", TypeName(r.ctxType), r.ctxType,
                             r.kind == Kind::kCtxMelee
                                 ? (r.pool != 0xFFFFFFFF ? fmt::format(" pool={}", r.pool) : std::string(" pool=unread"))
                                 : std::string());
            }
        }

        for (const auto& [fid, s] : sums) {
            std::string leaves;
            for (std::size_t i = 0; i < kNumLeaves; ++i) {
                if (s.acts[i] == 0) continue;
                leaves += fmt::format("{}{}={}/{}", leaves.empty() ? "" : " ", LeafShort(static_cast<int>(i)), s.started[i], s.acts[i]);
            }
            const auto avg = [&](std::size_t t) {
                return s.ctxMelee[t] ? fmt::format("{:.1f}", static_cast<double>(s.poolSum[t]) / s.ctxMelee[t]) : std::string("-");
            };
            auto hist = s.pickHist;
            std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string picks;
            for (std::size_t i = 0; i < hist.size() && i < 10; ++i)
                picks += fmt::format("{}{}={}", picks.empty() ? "" : " ", hist[i].first, hist[i].second);
            spdlog::info("[atk-sel] 0x{} '{}' SUMMARY fight#{} | leaves started/acts: {} | ctx melee R={} Bash={} L={} "
                         "(avg pool {}/{}/{}) block R={} Bash={} L={} odd={} | picks={} power={} bash={} left={} "
                         "unchanged={} | {}",
                         apmf::log::Hex(fid), nameOf(fid), s.fight, leaves.empty() ? "(none)" : leaves, s.ctxMelee[0],
                         s.ctxMelee[1], s.ctxMelee[2], avg(0), avg(1), avg(2), s.ctxBlock[0], s.ctxBlock[1],
                         s.ctxBlock[2], s.ctxOdd, s.picks, s.power, s.bash, s.left, s.unchanged,
                         picks.empty() ? "(no new pick)" : picks);
        }

        if (now - g_lastTotalsMs >= kTotalsMs) {
            g_lastTotalsMs = now;
            spdlog::info("[atk-sel] H leafActs={} ctxActs={} recorded={} unresolved={} ctxBoundMiss={} queueDrops={} "
                         "actorDrops={} lines={}",
                         g_leafActs.load(std::memory_order_relaxed), g_ctxActs.load(std::memory_order_relaxed),
                         g_recorded.load(std::memory_order_relaxed), g_unresolved.load(std::memory_order_relaxed),
                         g_ctxBoundMiss.load(std::memory_order_relaxed), qDrops, aDrops, g_lines);
        }
    }

}
