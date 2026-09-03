#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/CombatBehaviorRE.h"
#include "core/T1Probe.h"

#include <array>
#include <cstring>

// ============================================================================
// See T1Probe.h. Implementation notes:
//
// * InstallOnVtables (core/Allowance.h) is reused verbatim -- the SAME thunk
//   address is installed on all 70 leaf vtables at slot 2; the thunk itself
//   dispatches on the calling vtable's runtime address (g_orig lookup),
//   exactly the CastGate.cpp/EquipGate.cpp pattern, just with an
//   observation-only default instead of an allow/deny default.
// * SetFailed is resolved by disassembling CombatBehaviorForceFail's ORIGINAL
//   (pre-hook) act() body -- read from g_orig AFTER InstallOnVtables runs, not
//   from the live vtable slot (which our OWN thunk now occupies). ForceFail's
//   entire job is "call SetFailed(true); return control" (ALLOWANCE-
//   TEMPLATE.md §2 item 1: "CombatBehaviorForceFail is a shipped node doing
//   exactly this"), so the first CALL (0xE8) instruction in its compiled body
//   IS SetFailed on both SE and AE -- no Address-Library AE id needed.
// ============================================================================

extern "C" __declspec(dllimport) std::uint32_t __stdcall GetCurrentThreadId();

namespace apmf::t1probe {

    namespace {

        // Deck-pressable numpad range (F-keys aren't reachable on Steam Deck).
        // NumpadEnter is the SHARED claim/release key: T1Probe, T4Probe, and
        // AliasPkgProbe (0x49) all listen for the SAME scancode, so one press
        // claims/releases the aimed NPC across all three at once (each keeps
        // its own independent claim state but the same toggle logic drives
        // them in lockstep -- see Docs/PROBE-ALLOWANCE.md).
        constexpr std::uint32_t kClaimKey = 0x9C;   // NumpadEnter -- claim/toggle T1 observe on the aimed NPC (shared)
        constexpr std::uint32_t kDenyKey  = 0xB5;   // NumpadSlash -- toggle Phase-1 Attack-leaf deny on the claimed NPC

        std::atomic<bool> g_installed{ false };

        // vtable runtime address -> original act() (Phase-0 passthrough target).
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        // vtable runtime address -> our compile-time leaf name (cbt::kLeaves).
        std::unordered_map<std::uintptr_t, const char*>     g_leafName;

        std::uintptr_t g_attackVt   = 0;   // CombatBehaviorAttack's vtable runtime address
        std::atomic<std::uintptr_t> g_setFailed{ 0 };   // resolved SetFailed(control, bool)

        std::atomic<RE::FormID> g_claimActor{ 0 };
        std::atomic<bool>       g_denyAttack{ false };

        // ---- Phase-0 observability (thunk writes, OncePerFrame summarises) ----
        std::atomic<std::uint64_t> g_hitsClaimed{ 0 };     // act() calls seen for the claimed actor (any leaf)
        std::atomic<std::uint64_t> g_hitsAttackDenied{ 0 };
        std::atomic<std::uint32_t> g_lastThread{ 0 };
        std::array<std::atomic<bool>, 70> g_leafFirstLogged{};
        std::atomic<bool> g_ambiguityLogged{ false };

        RE::Actor* CrosshairActor() {
            if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                if (auto ref = pick->targetActor.get()) {
                    auto* a = ref->As<RE::Actor>();
                    if (a && !a->IsPlayerRef()) return a;
                }
            }
            return nullptr;
        }

        // Find the (already-hooked) original act() for `vtRuntimeAddr`, disassemble
        // its first CALL rel32 (0xE8) within a small window, and return the
        // absolute target address. Returns 0 if not found (logged as UNRESOLVED,
        // never guessed).
        std::uintptr_t FindFirstCallTarget(std::uintptr_t fnAddr, int window = 64) {
            if (!fnAddr) return 0;
            auto* bytes = reinterpret_cast<const std::uint8_t*>(fnAddr);
            for (int i = 0; i + 5 <= window; ++i) {
                if (bytes[i] == 0xE8) {
                    std::int32_t rel = 0;
                    std::memcpy(&rel, bytes + i + 1, sizeof(rel));
                    return fnAddr + static_cast<std::uintptr_t>(i) + 5 + static_cast<std::intptr_t>(rel);
                }
            }
            return 0;
        }

        void ResolveSetFailed() {
            const int ffIdx = apmf::cbt::ForceFailIndex();
            if (ffIdx < 0) {
                spdlog::warn("[t1probe] ForceFail leaf not found in the local table -- SetFailed NOT derived; Phase 1 DENY unavailable.");
                return;
            }
            REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[static_cast<std::size_t>(ffIdx)].vtbl };
            const auto oit = g_orig.find(vt.address());
            if (oit == g_orig.end()) {
                spdlog::warn("[t1probe] ForceFail vtable 0x{} has no recorded original (install skipped it, likely an RTTI mismatch) -- SetFailed NOT derived.",
                             apmf::log::Hex(vt.address(), 16));
                return;
            }
            const std::uintptr_t target = FindFirstCallTarget(oit->second);
            if (!target) {
                spdlog::warn("[t1probe] no CALL (0xE8) found in the first 64 bytes of ForceFail::act() -- SetFailed NOT derived "
                             "(the body may not be a simple call+return on this build). Phase 1 DENY unavailable.");
                return;
            }
            g_setFailed.store(target, std::memory_order_relaxed);
            const auto base = REL::Module::get().base();
            spdlog::info("[t1probe] SetFailed DERIVED from ForceFail::act() disassembly: module+0x{} (fn body at module+0x{}).",
                         apmf::log::Hex(target - base, 8), apmf::log::Hex(oit->second - base, 8));
            if (REL::Module::IsSE()) {
                // CPR's own header comments this call site as SkyrimSE.exe+0x7C6D30 on the
                // 1.5.97 build it ships for -- cross-check only (never used as the primary path).
                const std::uintptr_t cprSE = base + 0x7C6D30;
                spdlog::info("[t1probe] SE cross-check vs CPR's documented CombatBehaviorTreeControl::SetFailed "
                             "(module+0x7C6D30): {}.", target == cprSE ? "MATCH" : "MISMATCH -- derivation may be wrong, verify before trusting Phase 1");
            }
        }

        void* ActThunk(void* a_this, void* a_control) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) return a_control;   // foreign vtable -- benign passthrough, touch nothing
            auto orig = reinterpret_cast<apmf::cbt::Act_t>(oit->second);

            const RE::FormID claim = g_claimActor.load(std::memory_order_relaxed);
            if (claim == 0) return orig(a_this, a_control);   // near-zero cost: no claim, nothing to observe

            // Resolve the deliberating actor -- BOTH hypotheses (§5 ambiguity).
            RE::FormID fidA = 0, fidB = 0;
            if (a_control) {
                auto* tc = reinterpret_cast<apmf::cbt::TreeControl*>(a_control);
                void* p0x158 = tc->master_controller;
                if (p0x158) {
                    // Hypothesis A (CPR): p0x158 IS CombatController* directly.
                    auto* ctrlA = reinterpret_cast<apmf::cbt::ControllerMini*>(p0x158);
                    if (auto a = ctrlA->attackerHandle.get()) fidA = a->GetFormID();
                    // Hypothesis B (doc guess): p0x158 is CombatBehaviorController*;
                    // its OWN +0x20 holds a CombatController*; that struct's +0x28 is attackerHandle.
                    void* cbcPlus20 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p0x158) + 0x20);
                    if (cbcPlus20) {
                        auto* ctrlB = reinterpret_cast<apmf::cbt::ControllerMini*>(cbcPlus20);
                        if (auto b = ctrlB->attackerHandle.get()) fidB = b->GetFormID();
                    }
                }
            }

            if (!g_ambiguityLogged.exchange(true) && a_control) {
                spdlog::info("[t1probe] +0x158 AMBIGUITY: hypothesis A (control+0x158 == CombatController* directly, "
                             "attackerHandle@0x28) resolved actor=0x{}; hypothesis B (control+0x158 -> +0x20 -> "
                             "CombatController*, attackerHandle@0x28) resolved actor=0x{}. Claimed actor is 0x{}.",
                             apmf::log::Hex(fidA), apmf::log::Hex(fidB), apmf::log::Hex(claim));
            }

            if (fidA != claim && fidB != claim) return orig(a_this, a_control);   // not our claimed actor -- passthrough

            g_hitsClaimed.fetch_add(1, std::memory_order_relaxed);
            g_lastThread.store(GetCurrentThreadId(), std::memory_order_relaxed);

            const auto nit = g_leafName.find(vt);
            const char* ourName = nit != g_leafName.end() ? nit->second : "<unknown>";

            // First-hit-per-leaf log only (never per-call -- avoids flooding a
            // per-tick tree walk). GetName() is the leaf's own slot-1 virtual,
            // unhooked -- an ordinary, safe call.
            for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
                if (std::string_view(apmf::cbt::kLeaves[i].name) != ourName) continue;
                if (!g_leafFirstLogged[i].exchange(true)) {
                    using GetName_t = char* (*)(void*);
                    auto* getNameSlot = reinterpret_cast<GetName_t*>(vt + 1 * sizeof(void*));
                    const char* liveName = getNameSlot && *getNameSlot ? (*getNameSlot)(a_this) : nullptr;
                    spdlog::info("[t1probe] FIRST FIRE leaf='{}' (engine GetName()='{}') actor=0x{} thread={}.",
                                 ourName, liveName ? liveName : "<null>", apmf::log::Hex(claim), GetCurrentThreadId());
                }
                break;
            }

            // Phase 1: deny ONLY the Attack leaf, ONLY while armed, ONLY for the claim.
            if (vt == g_attackVt && g_denyAttack.load(std::memory_order_relaxed)) {
                const auto sf = g_setFailed.load(std::memory_order_relaxed);
                if (sf && a_control) {
                    g_hitsAttackDenied.fetch_add(1, std::memory_order_relaxed);
                    reinterpret_cast<void (*)(void*, bool)>(sf)(a_control, true);
                    return a_control;   // do NOT call orig -- this IS the deny
                }
                spdlog::warn("[t1probe] Phase 1 armed but SetFailed was never derived -- falling back to OBSERVE for this hit.");
            }

            return orig(a_this, a_control);
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t1probe] VR runtime -- the 70 leaf vtable indices are SE/AE-only verified; T1 probe NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        std::array<REL::VariantID, 70> vtables{};
        for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) vtables[i] = apmf::cbt::kLeaves[i].vtbl;

        REL::Relocation<void*> expectedTD{ apmf::cbt::RTTI_CombatBehaviorTreeNode };
        const int n = allowance::InstallOnVtables(vtables, 0x02, &ActThunk, expectedTD.get(), "t1probe", g_orig);

        for (std::size_t i = 0; i < apmf::cbt::kLeaves.size(); ++i) {
            REL::Relocation<std::uintptr_t> vt{ apmf::cbt::kLeaves[i].vtbl };
            if (g_orig.contains(vt.address())) g_leafName[vt.address()] = apmf::cbt::kLeaves[i].name;
            if (std::string_view(apmf::cbt::kLeaves[i].name) == "CombatBehaviorAttack") g_attackVt = vt.address();
        }

        ResolveSetFailed();

        spdlog::info("[t1probe] ARMED: {} of 70 leaf vtables hooked (slot 0x02, act/Enter, OBSERVE-only by default). "
                     "Attack vtable {}. NumpadEnter (DIK 0x{}, shared with T4/0x49) claims/toggles the aimed NPC; "
                     "NumpadSlash (DIK 0x{}) toggles Phase-1 Attack-leaf deny once claimed.",
                     n, g_attackVt ? "resolved" : "NOT resolved",
                     apmf::log::Hex(kClaimKey, 2), apmf::log::Hex(kDenyKey, 2));
    }

    void OnHotkey(std::uint32_t a_code) {
        if (!g_installed.load(std::memory_order_relaxed)) return;

        if (a_code == kDenyKey) {
            if (g_claimActor.load(std::memory_order_relaxed) == 0) {
                spdlog::warn("[t1probe] Phase 1 REFUSED -- claim an NPC with NumpadEnter first.");
                return;
            }
            const bool now = !g_denyAttack.load(std::memory_order_relaxed);
            g_denyAttack.store(now, std::memory_order_relaxed);
            spdlog::info("[t1probe] Phase 1 Attack-leaf deny {} for the claimed actor.", now ? "ENABLED" : "DISABLED");
            return;
        }
        if (a_code != kClaimKey) return;

        const RE::FormID cur = g_claimActor.load(std::memory_order_relaxed);
        if (cur != 0) {
            g_claimActor.store(0, std::memory_order_relaxed);
            g_denyAttack.store(false, std::memory_order_relaxed);
            spdlog::info("[t1probe] RELEASED claim on 0x{} (observation + any Phase-1 deny stop).", apmf::log::Hex(cur));
            return;
        }
        auto* actor = CrosshairActor();
        if (!actor) {
            spdlog::warn("[t1probe] claim REFUSED -- aim the crosshair at an NPC (not the player) first.");
            return;
        }
        g_claimActor.store(actor->GetFormID(), std::memory_order_relaxed);
        spdlog::info("[t1probe] CLAIMED 0x{} '{}' -- Phase 0 observation starts now (watch for FIRST FIRE lines).",
                     apmf::log::Hex(actor->GetFormID()), actor->GetName() ? actor->GetName() : "?");
    }

    void OncePerFrame() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const RE::FormID claim = g_claimActor.load(std::memory_order_relaxed);
        if (claim == 0) return;

        static std::uint32_t s_frames = 0;
        if ((++s_frames % 300) != 0) return;   // ~5s @ 60fps
        spdlog::info("[t1probe] census: claim=0x{} hits={} attackDenied={} lastThread={}.",
                     apmf::log::Hex(claim), g_hitsClaimed.load(std::memory_order_relaxed),
                     g_hitsAttackDenied.load(std::memory_order_relaxed), g_lastThread.load(std::memory_order_relaxed));
    }

    void ClearOnPreLoad() {
        g_claimActor.store(0, std::memory_order_relaxed);
        g_denyAttack.store(false, std::memory_order_relaxed);
    }

}
