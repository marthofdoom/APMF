#include "PCH.h"
#include "core/TorchSeat.h"
#include "core/Allowance.h"    // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"   // TryGetEquipSet: the ch.17 claim's owned / denied scope (one RCU read)
#include "core/EquipSink.h"    // ObserveOnly(): the INI observe-only switch the sink honours
#include "core/Hook.h"         // OnMainThread(): the one-time thread line
#include "core/Log.h"
#include "core/Sources.h"      // the Engine source's rank on the EquipWeapon facet; census of the call seat

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Win32, declared by hand (the PCH does not pull in <Windows.h>; core/Sources.cpp's one-line import).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// See core/TorchSeat.h for the contract. Evidence: agentlogs/diag-torch.md (the engine's
// torch updater and its two branches) and agentlogs/apmf-torch.md (this seat's RE: the one
// reference on each build, the byte checks, the other torch sources).

namespace apmf::torchseat {

    namespace {
        constexpr const char* kIni      = "Data/SKSE/Plugins/APMF.ini";
        constexpr const char* kSiteRow  = "TorchSeat.Site.AiUpdate";
        constexpr const char* kFnRow    = "TorchSeat.UpdateTorch";

        // Per runtime, read off our own disassembly (rule 11): the per-actor AI update's id and the
        // offset of its one E8 to the torch updater, and the updater's id. 1.7.104 is an AE-column build
        // whose MIT id table carries both AE ids (39637 -> 0x704C70, 39948 -> 0x7265C0); the 1.7.104 body
        // of the updater is instruction-identical to 1.6.1170's (154 insns) and the site sits at the same
        // offset, so the rows TorchSeat.* verify there through the table (VERIFIED-ADDRESSES.md).
        constexpr std::uint64_t kUpdateAE = 39637, kTorchAE = 39948;
        constexpr std::uint64_t kUpdateSE = 38606, kTorchSE = 38902;
        constexpr std::uintptr_t kSiteOffAE = 0x8B1;   // 1.6.1170 and 1.7.104
        constexpr std::uintptr_t kSiteOffSE = 0x798;   // 1.5.97

        // The engine's own read of the held left-hand item: AIProcess+0x08 middleHigh, +0x220 leftHand
        // (InventoryEntryData*), +0x00 object (identical on 1.6.1170 / 1.5.97 / 1.7.104: updater
        // +0x78 / +0x2C reads). Read only by the stow log, on the thread that is about to run the
        // engine's own identical read.
        static_assert(offsetof(RE::AIProcess, middleHigh) == 0x08, "AIProcess::middleHigh moved");
        static_assert(offsetof(RE::MiddleHighProcessData, leftHand) == 0x220, "MiddleHighProcessData::leftHand moved");

        using UpdateTorch_t = void (*)(RE::AIProcess*, RE::Actor*);
        std::atomic<UpdateTorch_t> g_orig{ nullptr };
        std::atomic<bool>          g_installed{ false };
        std::atomic<bool>          g_threadLogged{ false };

        // ---- log state (leaf mutex; nothing is called while it is held) ----
        // A DENY STREAK is keyed by (actor, ch.17 handle): handles are monotonic for the process and every
        // claim drops at kPreLoadGame / revert, so a new claim (or a new save) starts a new streak and is
        // logged. A streak ends on the first call for that actor that does not deny (logged with the number
        // of updater calls it skipped). Bounded: a full map is cleared (the worst case is one repeated line).
        struct Streak {
            APMF_API::Handle handle  = APMF_API::kInvalidHandle;
            std::uint64_t    skipped = 0;
            std::uint8_t     mode    = 0;   // 1 = denied, 2 = would-deny (observe-only), 3 = stood (ranked out)
        };
        constexpr std::size_t kStreakMax = 512;
        std::mutex                                 g_mx;
        std::unordered_map<RE::FormID, Streak>     g_streaks;
        // The stow log (a ch.17-claimed actor whose claim does not own Light): per actor at most one line
        // per kStowActorMs, and at most kStowBurst lines per kStowWindowMs over all actors (the suppressed
        // count rides the next line).
        constexpr std::uint64_t                    kStowActorMs  = 10000;
        constexpr std::uint64_t                    kStowWindowMs = 60000;
        constexpr std::uint32_t                    kStowBurst    = 30;
        std::unordered_map<RE::FormID, std::uint64_t> g_stowLast;
        std::uint64_t                              g_stowWindowStart = 0;
        std::uint32_t                              g_stowInWindow    = 0;
        std::uint32_t                              g_stowSuppressed  = 0;

        const RE::TESBoundObject* HeldLight(RE::AIProcess* a_process) {
            if (!a_process || !a_process->middleHigh) return nullptr;
            const auto* entry = a_process->middleHigh->leftHand;
            const auto* obj   = entry ? entry->object : nullptr;
            return obj && obj->GetFormType() == RE::FormType::Light ? obj : nullptr;
        }

        // The deny verdict for an actor whose ch.17 claim was read into `a_set`. Mirrors the sink's rules
        // for a LIGH equip (equipsink::DeclaredSetVerdict, rule 5, and RankEquipDeny for an Engine
        // source): Light owned and not denied, with a declared set; ranked on the EquipWeapon facet.
        enum class Verdict : std::uint8_t { kRun, kDeny, kWouldDeny, kStood };
        Verdict Decide(const apmf::EquipSetView& a_set) {
            if ((a_set.owned & APMF_API::kEquipCat_Light) == 0) return Verdict::kRun;
            if ((a_set.denied & APMF_API::kEquipCat_Light) != 0) return Verdict::kRun;   // denied is not owned
            if (a_set.count == 0) return Verdict::kRun;   // claimed, nothing declared: the sink enforces nothing
            namespace src = apmf::sources;
            const auto facet     = src::FacetForEquipCats(APMF_API::kEquipCat_Light | APMF_API::kEquipCat_Left);
            const int  claimPrio = src::ClaimPriority(a_set.handle, facet);
            const int  srcPrio   = src::Priority(src::kEngineSource, facet);
            if (claimPrio <= srcPrio) return Verdict::kStood;   // the user's sliders: the engine's torch stands
            const bool observe = apmf::equipsink::ObserveOnly() || (a_set.flags & APMF_API::kEquipAuth_ObserveOnly) != 0;
            return observe ? Verdict::kWouldDeny : Verdict::kDeny;
        }

        // Called with the verdict on every updater call for a ch.17-claimed actor. Logs a streak's start
        // and end only; counts the Engine source's hit + overruled / stood once per streak.
        void NoteVerdict(RE::Actor* a_actor, const apmf::EquipSetView& a_set, Verdict a_v) {
            const RE::FormID id   = a_actor->GetFormID();
            const auto       mode = static_cast<std::uint8_t>(a_v);   // kRun = 0
            Streak           ended{};
            bool             started = false, endedNow = false;
            {
                std::scoped_lock lk(g_mx);
                auto it = g_streaks.find(id);
                if (it != g_streaks.end() && (it->second.mode != mode || it->second.handle != a_set.handle)) {
                    ended    = it->second;
                    endedNow = true;
                    g_streaks.erase(it);
                    it = g_streaks.end();
                }
                if (mode != 0) {
                    if (it == g_streaks.end()) {
                        if (g_streaks.size() >= kStreakMax) g_streaks.clear();
                        it      = g_streaks.emplace(id, Streak{ a_set.handle, 0, mode }).first;
                        started = true;
                    }
                    ++it->second.skipped;
                }
            }
            try {
                namespace src = apmf::sources;
                const auto facet = src::FacetForEquipCats(APMF_API::kEquipCat_Light | APMF_API::kEquipCat_Left);
                if (endedNow && ended.mode == static_cast<std::uint8_t>(Verdict::kDeny)) {
                    spdlog::info("[apmf][torch] actor={} torch updater deny ENDED after {} skipped update(s) "
                                 "(claim 0x{} no longer owns Light, or a new claim): the engine's torch behaviour "
                                 "runs again",
                                 apmf::log::Hex(id), ended.skipped, apmf::log::Hex(ended.handle));
                }
                if (!started) return;
                const char* name = a_actor->GetName() ? a_actor->GetName() : "";
                switch (a_v) {
                case Verdict::kDeny:
                    src::NoteHit(src::kEngineSource, facet);
                    src::NoteOverruled(src::kEngineSource, facet);
                    spdlog::info("[apmf][torch] actor={} name='{}' claim=0x{} owns Light: the engine's torch updater "
                                 "is DENIED (both its furniture/sit stow and its light-level equip/unequip) until the "
                                 "claim stops owning Light. Logged once per streak.",
                                 apmf::log::Hex(id), name, apmf::log::Hex(a_set.handle));
                    break;
                case Verdict::kWouldDeny:
                    src::NoteHit(src::kEngineSource, facet);
                    spdlog::info("[apmf][torch] actor={} name='{}' claim=0x{} owns Light: torch updater would-deny "
                                 "(observe-only: bEquipObserveOnly or the claim's kEquipAuth_ObserveOnly); the engine "
                                 "runs. Logged once per streak.",
                                 apmf::log::Hex(id), name, apmf::log::Hex(a_set.handle));
                    break;
                case Verdict::kStood:
                    src::NoteHit(src::kEngineSource, facet);
                    src::NoteStood(src::kEngineSource, facet);
                    spdlog::info("[apmf][torch] actor={} name='{}' claim=0x{} owns Light but the Engine source "
                                 "outranks it on {} (the user's priorities): the torch updater STANDS. Logged once "
                                 "per streak.",
                                 apmf::log::Hex(id), name, apmf::log::Hex(a_set.handle), src::FacetName(facet));
                    break;
                default:
                    break;
                }
            } catch (...) {
            }
        }

        // A ch.17-claimed actor whose claim does not own Light: the engine ran; if it took the held torch
        // off, say so (rate-capped). Passive: nothing is changed.
        void NoteStow(RE::Actor* a_actor, const RE::TESBoundObject* a_before, const RE::TESBoundObject* a_after,
                      const apmf::EquipSetView& a_set) {
            if (!a_before || a_after == a_before) return;
            const RE::FormID id  = a_actor->GetFormID();
            const auto       now = apmf::clock::MonotonicMs();
            std::uint32_t    suppressed = 0;
            {
                std::scoped_lock lk(g_mx);
                if (now - g_stowWindowStart >= kStowWindowMs) {
                    g_stowWindowStart = now;
                    g_stowInWindow    = 0;
                }
                auto it = g_stowLast.find(id);
                if (it != g_stowLast.end() && now - it->second < kStowActorMs) return;
                if (g_stowInWindow >= kStowBurst) {
                    ++g_stowSuppressed;
                    return;
                }
                if (g_stowLast.size() >= kStreakMax) g_stowLast.clear();
                g_stowLast[id] = now;
                ++g_stowInWindow;
                suppressed       = g_stowSuppressed;
                g_stowSuppressed = 0;
            }
            try {
                std::uint32_t sit = 0;
                if (const auto* st = a_actor->AsActorState()) sit = static_cast<std::uint32_t>(st->GetSitSleepState());
                spdlog::info("[apmf][torch] actor={} name='{}' engine torch updater UNEQUIPPED {} '{}' (claim 0x{} "
                             "does not own Light: owned=0x{} denied=0x{}; sitSleepState={}: {}). Passive, one line "
                             "per actor per {} s, {} per minute ({} suppressed since the last line).",
                             apmf::log::Hex(id), a_actor->GetName() ? a_actor->GetName() : "",
                             apmf::log::Hex(a_before->GetFormID()), a_before->GetName() ? a_before->GetName() : "",
                             apmf::log::Hex(a_set.handle), apmf::log::Hex(a_set.owned, 2), apmf::log::Hex(a_set.denied, 2),
                             sit, sit != 0 ? "the furniture/sit stow" : "the light-level evaluator or an interaction",
                             kStowActorMs / 1000, kStowBurst, suppressed);
            } catch (...) {
            }
        }

        // THE THUNK, at the AI update's one call to the torch updater (the engine's own thread for that
        // actor's update). The player is never here (the caller is non-player only) and is guarded anyway.
        void Thunk(RE::AIProcess* a_process, RE::Actor* a_actor) {
            const auto orig = g_orig.load(std::memory_order_acquire);
            if (!a_actor || a_actor->IsPlayerRef()) { orig(a_process, a_actor); return; }
            apmf::EquipSetView set;
            if (!apmf::ControlMap::Get().TryGetEquipSet(a_actor->GetFormID(), set)) {
                orig(a_process, a_actor);   // #17a(5): no ch.17 claim -- untouched and unlogged
                return;
            }
            // Nothing may unwind into the engine's AI update: a throw anywhere here runs the engine (degrade).
            Verdict v = Verdict::kRun;
            try {
                if (!g_threadLogged.load(std::memory_order_relaxed) && !g_threadLogged.exchange(true)) {
                    // Logged at the first CLAIMED actor (by then the player seat has recorded the main thread).
                    spdlog::info("[apmf][torch] first torch-updater call for a ch.17-claimed actor: {} (every read "
                                 "here is the RCU snapshot, the source registry or a leaf mutex; any thread is safe)",
                                 apmf::hook::OnMainThread() ? "main thread" : "NOT the main thread (an actor-update worker)");
                }
                v = Decide(set);
                NoteVerdict(a_actor, set, v);
            } catch (...) {
                v = Verdict::kRun;   // a registry or log throw never denies (the degrade direction)
            }
            if (v == Verdict::kDeny) return;   // DENY: the engine's torch updater does not run for this actor
            const RE::TESBoundObject* before = nullptr;
            if (v == Verdict::kRun) before = HeldLight(a_process);
            orig(a_process, a_actor);
            if (before) {
                try {
                    NoteStow(a_actor, before, HeldLight(a_process), set);
                } catch (...) {
                }
            }
        }
    }  // namespace

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime";
        } else if (!apmf::allowance::RuntimeSupported()) {
            why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (GetPrivateProfileIntA("EquipAuthority", "bTorchDeny", 1, kIni) == 0) {
            why = "[EquipAuthority] bTorchDeny=0";
        }
        std::uintptr_t site = 0, fn = 0;
        if (!why) {
            // Per runtime (rule 11): 1.5.97 has its own ids and offset; 1.6.1170 and 1.7.104 take the AE ids
            // (1.7.104 through the fork's MIT id table) and the AE offset, proven identical there.
            const bool se = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97);
            site = REL::ID(se ? kUpdateSE : kUpdateAE).address() + (se ? kSiteOffSE : kSiteOffAE);
            fn   = REL::ID(se ? kTorchSE : kTorchAE).address();
            bool ok = apmf::allowance::SeatVerified(fn, kFnRow);
            ok      = apmf::allowance::SeatVerified(site, kSiteRow) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            // The bytes NOW (another DLL may have patched the site since the self-check): E8 -> the updater.
            const auto*  b   = reinterpret_cast<const std::uint8_t*>(site);
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            if (b[0] != 0xE8 || site + 5 + static_cast<std::intptr_t>(rel) != fn)
                why = "the AI update's call site no longer calls the torch updater (another patcher got there first)";
        }
        if (why) {
            spdlog::warn("[apmf][torch] torch seat NOT installed -- {}. The engine's torch updater runs for every actor: "
                         "a ch.17 claim that owns Light does not stop its furniture/sit stow or its light-level "
                         "unequip (its equips are still refused by the equip sink).",
                         why);
            return;
        }
        // Our own trampoline (never freed), from SKSE's branch pool when it has room, else a page near the module.
        static auto* s_tr = new SKSE::Trampoline("APMF.TorchSeat");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(32);
        if (mem) s_tr->set_trampoline(mem, 32);
        else s_tr->create(32);
        // The original is stored BEFORE the site is patched (the thunk can run the instant the E8 is rewritten),
        // and it is the VERIFIED call target, not whatever write_call reports.
        g_orig.store(reinterpret_cast<UpdateTorch_t>(fn), std::memory_order_release);
        const std::uintptr_t prev = s_tr->write_call<5>(site, &Thunk);
        if (prev != fn)
            spdlog::error("[apmf][torch] torch seat patch: the previous call target changed between the check and "
                          "the patch (0x{:X}, expected 0x{:X}); the verified updater is called.",
                          prev, fn);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kEquipWeapon, kSiteRow, site, prev);
        g_installed.store(true, std::memory_order_release);
        const auto base = REL::Module::get().base();
        spdlog::info("[apmf][torch] torch seat installed: the AI update's call (RVA 0x{:X}) to the torch updater "
                     "(RVA 0x{:X}). An actor whose ch.17 claim owns Light skips it (stow and evaluator both denied); "
                     "every other actor runs it untouched.",
                     site - base, fn - base);
    }

}  // namespace apmf::torchseat
