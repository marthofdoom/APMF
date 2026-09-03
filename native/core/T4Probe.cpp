#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/T4Probe.h"

#include "RE/B/BGSAction.h"
#include "RE/T/TESActionData.h"

#include <array>
#include <cstring>

// ============================================================================
// See T4Probe.h. `RE::ActionInput`/`RE::BGSActionData`/`RE::TESActionData`/
// `RE::BGSAction` ARE real, shipped CommonLibSSE-NG classes (unlike T1's
// combat behavior tree) -- verified present at the colorglass-pinned commit
// `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` this project actually builds
// against, so this probe uses them directly, no local RE:: extension needed.
// ============================================================================

namespace apmf::t4probe {

    namespace {

        constexpr std::uint32_t kClaimKey = 0x42;   // F8 -- claim/toggle T4 observe on the aimed NPC

        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_usingVtable{ false };   // which seat won (for logging/help text only)
        std::atomic<RE::FormID> g_claimActor{ 0 };

        std::unordered_map<std::uintptr_t, std::uintptr_t> g_vtOrig;   // vtable path

        using Process_t = bool (*)(void*);

        std::atomic<std::uint64_t> g_hits{ 0 };

        // Dedupe "first seen" logging -- LOCK-FREE (never a mutex on a combat-adjacent
        // thunk path). A fixed-capacity flat set: scan-then-append. A benign, rare race
        // (two threads both pass the scan before either commits) can double-log an
        // action at most once -- a cosmetic risk only, never a correctness/safety one,
        // acceptable for a throwaway instrumentation probe. Capacity exceeded => stop
        // deduping (still logs, just no longer "first seen only") rather than drop data.
        constexpr std::size_t kSeenCap = 256;
        std::array<std::atomic<RE::FormID>, kSeenCap> g_seen{};
        std::atomic<std::size_t> g_seenCount{ 0 };

        bool MarkFirstSeen(RE::FormID a_id) {
            const std::size_t n = g_seenCount.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < n && i < kSeenCap; ++i) {
                if (g_seen[i].load(std::memory_order_relaxed) == a_id) return false;
            }
            const std::size_t idx = g_seenCount.fetch_add(1, std::memory_order_acq_rel);
            if (idx < kSeenCap) g_seen[idx].store(a_id, std::memory_order_release);
            return true;
        }

        RE::Actor* CrosshairActor() {
            if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                if (auto ref = pick->targetActor.get()) {
                    auto* a = ref->As<RE::Actor>();
                    if (a && !a->IsPlayerRef()) return a;
                }
            }
            return nullptr;
        }

        void LogAction(RE::TESActionData* a_this) {
            if (!a_this) return;
            const RE::FormID claim = g_claimActor.load(std::memory_order_relaxed);
            if (claim == 0) return;

            RE::TESObjectREFR* source = a_this->source.get();
            if (!source || source->GetFormID() != claim) return;

            g_hits.fetch_add(1, std::memory_order_relaxed);

            RE::BGSAction* action = a_this->action;
            const RE::FormID actionId = action ? action->GetFormID() : 0;

            if (!MarkFirstSeen(actionId)) return;   // per-action first-seen only -- avoids flooding a per-frame seat

            RE::TESObjectREFR* target = a_this->target.get();
            const char* edid = action ? action->GetFormEditorID() : nullptr;
            spdlog::info("[t4probe] FIRST SEEN action='{}' (0x{}) priority(raw unk20)={} source=0x{} target=0x{}.",
                         (edid && *edid) ? edid : "<no editorid>", apmf::log::Hex(actionId),
                         a_this->unk20, apmf::log::Hex(claim), apmf::log::Hex(target ? target->GetFormID() : 0));
        }

        bool VtThunk(void* a_this) {
            const auto vt = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto it = g_vtOrig.find(vt);
            if (it == g_vtOrig.end()) return false;   // foreign -- benign default, never observed in practice
            const bool result = reinterpret_cast<Process_t>(it->second)(a_this);
            LogAction(reinterpret_cast<RE::TESActionData*>(a_this));
            return result;
        }

        struct CallSiteHook {
            static bool thunk(RE::TESActionData* a_this) {
                const bool result = func(a_this);
                LogAction(a_this);
                return result;
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Try both documented sub-offsets of valhallaCombat's known
        // TESActionData::Process call site (ALLOWANCE-TEMPLATE.md §3 T4 row:
        // "RELOCATION_ID(48139,49170)+0x4D7/0x435"); accept whichever byte at
        // the resolved address is actually 0xE8 (CALL rel32) -- self-
        // validating rather than trusting which of the two is SE vs AE (the
        // doc's SE/AE prose labels were found transposed relative to the
        // real header convention elsewhere in this research -- see
        // CombatBehaviorRE.h's file header note -- so this is verified at
        // runtime, never assumed).
        std::uintptr_t ResolveCallSite(std::ptrdiff_t& usedOffset) {
            for (const std::ptrdiff_t off : { std::ptrdiff_t{ 0x4D7 }, std::ptrdiff_t{ 0x435 } }) {
                REL::Relocation<std::uintptr_t> site{ RELOCATION_ID(48139, 49170), off };
                const std::uintptr_t addr = site.address();
                if (addr && *reinterpret_cast<const std::uint8_t*>(addr) == 0xE8) {
                    usedOffset = off;
                    return addr;
                }
            }
            return 0;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t4probe] VR runtime -- the RELOCATION_ID call site + VTABLE_TESActionData index are "
                         "SE/AE-only verified; T4 probe NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        std::ptrdiff_t usedOffset = 0;
        const std::uintptr_t callSite = ResolveCallSite(usedOffset);
        if (!callSite) {
            spdlog::warn("[t4probe] neither call-site sub-offset (+0x4D7 / +0x435 off RELOCATION_ID(48139,49170)) "
                         "resolved to a 0xE8 CALL byte on this build -- T4 UNRESOLVED, not installed. This is the "
                         "'read the rel32' step failing at the source; report + do not build further on T4 "
                         "until the real call site is found.");
            return;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(callSite + 1), sizeof(rel));
        const std::uintptr_t callTarget = callSite + 5 + static_cast<std::intptr_t>(rel);

        REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_TESActionData[0] };
        constexpr std::size_t kProcessSlot = 5;
        const std::uintptr_t slot5 = *reinterpret_cast<std::uintptr_t*>(vt.address() + kProcessSlot * sizeof(void*));

        const bool isVirtual = (callTarget == slot5);
        spdlog::info("[t4probe] call site RELOCATION_ID(48139,49170)+0x{} = module+0x{}; rel32 target = module+0x{}; "
                     "VTABLE_TESActionData[0] slot 5 (Process) = module+0x{}. {}",
                     apmf::log::Hex(static_cast<std::uint64_t>(usedOffset), 3),
                     apmf::log::Hex(callSite - REL::Module::get().base(), 8),
                     apmf::log::Hex(callTarget - REL::Module::get().base(), 8),
                     apmf::log::Hex(slot5 - REL::Module::get().base(), 8),
                     isVirtual ? "MATCH -- Process() IS reached virtually; hooking VTABLE_TESActionData[0] slot 5 "
                                 "(catches every caller, not just valhalla's site)."
                               : "MISMATCH -- devirtualised; hooking the callee entry directly via the trampoline "
                                 "at valhalla's known call site (catches calls routed through THIS site only -- "
                                 "an honest, documented coverage limitation, not a full seat guarantee).");

        g_usingVtable.store(isVirtual, std::memory_order_relaxed);
        if (isVirtual) {
            REL::Relocation<void*> expectedTD{ RE::RTTI_TESActionData };
            const REL::VariantID kVtables[] = { RE::VTABLE_TESActionData[0] };
            const int n = allowance::InstallOnVtables(kVtables, kProcessSlot,
                                                       &VtThunk, expectedTD.get(), "t4probe", g_vtOrig);
            spdlog::info("[t4probe] ARMED (vtable path): {} vtable(s) hooked.", n);
        } else {
            SKSE::AllocTrampoline(64);
            auto& trampoline = SKSE::GetTrampoline();
            CallSiteHook::func = trampoline.write_call<5>(callTarget, CallSiteHook::thunk);
            spdlog::info("[t4probe] ARMED (call-site path): entry module+0x{} hooked via a 5-byte call patch "
                         "at valhalla's own known site.", apmf::log::Hex(callTarget - REL::Module::get().base(), 8));
        }
        spdlog::info("[t4probe] F8 (DIK 0x{}) claims/toggles the aimed NPC as `source`; logs the first-seen "
                     "BGSAction per unique action while claimed.", apmf::log::Hex(kClaimKey, 2));
    }

    void OnHotkey(std::uint32_t a_code) {
        if (!g_installed.load(std::memory_order_relaxed) || a_code != kClaimKey) return;

        const RE::FormID cur = g_claimActor.load(std::memory_order_relaxed);
        if (cur != 0) {
            g_claimActor.store(0, std::memory_order_relaxed);
            spdlog::info("[t4probe] RELEASED claim on 0x{}.", apmf::log::Hex(cur));
            return;
        }
        auto* actor = CrosshairActor();
        if (!actor) {
            spdlog::warn("[t4probe] claim REFUSED -- aim the crosshair at an NPC (not the player) first.");
            return;
        }
        g_seenCount.store(0, std::memory_order_relaxed);   // reset the dedupe set for the new claim
        g_claimActor.store(actor->GetFormID(), std::memory_order_relaxed);
        spdlog::info("[t4probe] CLAIMED 0x{} '{}' as `source` -- watch for FIRST SEEN action lines across "
                     "combat/sandbox/dialogue/player-command.", apmf::log::Hex(actor->GetFormID()),
                     actor->GetName() ? actor->GetName() : "?");
    }

    void OncePerFrame() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const RE::FormID claim = g_claimActor.load(std::memory_order_relaxed);
        if (claim == 0) return;

        static std::uint32_t s_frames = 0;
        if ((++s_frames % 300) != 0) return;   // ~5s @ 60fps
        const std::size_t seenCount = g_seenCount.load(std::memory_order_relaxed);
        const std::size_t distinct = seenCount < kSeenCap ? seenCount : kSeenCap;
        spdlog::info("[t4probe] census: claim=0x{} hits={} distinctActions={} seat={}.", apmf::log::Hex(claim),
                     g_hits.load(std::memory_order_relaxed), distinct,
                     g_usingVtable.load(std::memory_order_relaxed) ? "vtable" : "call-site");
    }

    void ClearOnPreLoad() {
        g_claimActor.store(0, std::memory_order_relaxed);
    }

}
