#include "PCH.h"
#include "core/Log.h"
#include "core/AliasPkgProbe.h"

#include "RE/E/ExtraAliasInstanceArray.h"

// ============================================================================
// 0x49 package-offer probe implementation. See AliasPkgProbe.h + design.md §5a.
//
// THE MECHANISM UNDER TEST: the engine asks each actor "does an ALIAS give you a
// package right now?" via CheckForCurrentAliasPackage (vfunc 0x49). If we return a
// client's TESPackage*, the engine adopts it as current and runs it natively -- a
// package-tier promote with one OnPackageChange each way (design.md §5a). We hook
// 0x49 on VTABLE_Character ONLY (never PlayerCharacter -- the player must never be
// package-driven; the §0.38 scar).
//
// PHASES (marth field-tests on Cicero, owner quest 0x0009BE51, 3 deck cycles):
//   Phase 0  NO claim, ~60 s -- DOES THE HOOK FIRE? The make-or-break. The thunk
//            counts every call and the periodic summary logs the hit count, the
//            calling thread id (compare to the 0xAD thread), and the last returned
//            package (FormID + type). If the count stays ZERO, 0x49 is devirtualised
//            /inlined on this runtime: the mechanism is DEAD -- report and STOP, do
//            not build fallbacks.
//   Phase 1  ENGAGE (hotkey): GetCurrentPackage flips to the client pkg within one
//            eval; distance-to-target shrinks; ZERO APMF writes after the engage
//            EvaluatePackage; ExtraAliasInstanceArray identical before/after.
//   Phase 2  RELEASE (hotkey again): framework pkg returns; exactly one OnPackageChange.
//   Phase 3  save/load mid-claim: framework pkg after load; no latch; no CTD.
//
// THE CLIENT PACKAGE. APMF ships no ESP, so the offered package is identified by the
// compile-time constant kProbePackageForm below. Phase 0 needs NO package (it only
// asks whether the hook fires) and runs out of the box. For Phases 1-3, set
// kProbePackageForm to a REAL package FormID present in the deck load order (a
// travel-to-a-marker / sandbox package -- "target a marker, or targType 0 with a
// runtime handle") and rebuild the probe. Left 0 => Phase 0 only, with a warning.
// ============================================================================

// Win32 thread id for the "which thread is 0x49 on?" phase-0 check (compare to 0xAD).
// Declared by hand (probe-local); pointer-free, no header conflict.
extern "C" __declspec(dllimport) std::uint32_t __stdcall GetCurrentThreadId();

namespace apmf::probe {

    namespace {

        // ---- CONFIG (set for a full Phase 1-3 field build) ----
        // The client package to OFFER: `DefaultSandboxCurrentLocation256`
        // (Skyrim.esm, FormID 0x000956B8) -- a vanilla, radius-256,
        // "sandbox wherever the actor CURRENTLY is" package (PLDT type
        // reads current-location, not a fixed editor-placed marker) chosen
        // specifically because it cannot send the claimed NPC walking off
        // toward a marker that might be far away or behind a locked door --
        // safe to engage/release on any generic NPC anywhere. Verified by
        // parsing Skyrim.esm's own PACK group directly (EDID subrecord),
        // not guessed and not taken from a third-party list. See
        // Docs/PROBE-ALLOWANCE.md for the extraction method.
        constexpr RE::FormID       kProbePackageForm = 0x000956B8;
        // The test hotkeys (DirectInput scancodes). Probe/test hotkeys use the
        // numpad (F-keys are occupied by the game/modlist); SAME scancodes as
        // T1Probe's claim keys, so one press claims/releases the SAME actor across
        // both probes at once (see Docs/PROBE-ALLOWANCE.md).
        constexpr std::uint32_t    kProbeKey         = 0x9C;   // NumpadEnter -- claim/toggle the aimed NPC
        constexpr std::uint32_t    kProbeNearestKey  = 0x51;   // Numpad3 -- claim/toggle the NEAREST in-combat NPC, no aim needed

        // ---- Hook state ----
        std::atomic<bool>          g_armed{ false };

        // ---- The single-actor package-offer CLAIM (probe scope). Two atomics =>
        // lock-free read in the 0x49 thunk (game thread) + lock-free write from the
        // hotkey (input thread). One claim at a time is all the probe needs. ----
        std::atomic<RE::FormID>    g_claimActor{ 0 };   // 0 = no claim
        std::atomic<RE::FormID>    g_claimPkg{ 0 };     // the offered package's FormID

        // ---- Pending game-thread EvaluatePackage (queued by the hotkey, run in
        // OncePerFrame on the true game thread). ----
        enum class Pend : std::uint8_t { kNone, kEngage, kRelease };
        std::atomic<Pend>          g_pend{ Pend::kNone };
        std::atomic<RE::FormID>    g_pendActor{ 0 };

        // ---- Phase-0 observability (thunk writes, OncePerFrame summarises) ----
        std::atomic<std::uint64_t> g_hits{ 0 };          // total 0x49 calls seen
        std::atomic<std::uint64_t> g_redirects{ 0 };     // calls where we returned the client pkg
        std::atomic<std::uint32_t> g_thread{ 0 };        // thread id 0x49 fired on
        std::atomic<RE::FormID>    g_lastRetPkg{ 0 };     // last package returned (original path)
        std::atomic<bool>          g_firstLogged{ false };

        namespace Native {
            // Actor::EvaluatePackage(unk, resetAI) -- Address-Library bound, unbound in
            // CommonLib. Poke the AI to re-run package selection now (so a 0x49 redirect
            // takes within one eval). resetAI MUST be false (never a full AI reset).
            void EvaluatePackage(RE::Actor* a_actor) {
                using func_t = void (*)(RE::Actor*, bool, bool);
                static REL::Relocation<func_t> func{ RELOCATION_ID(36407, 37401) };
                func(a_actor, true, false);
            }

            const char* PkgType(RE::TESPackage* a_pkg) {
                if (!a_pkg) return "<null>";
                const char* n = a_pkg->GetObjectTypeName();
                return n ? n : "<unnamed>";
            }
        }

        // Phase 1/2 safety check: the alias-fill machinery (Packages.cpp-equivalent
        // in a real client) must be UNTOUCHED by a package-offer redirect -- 0x49
        // only changes what CheckForCurrentAliasPackage RETURNS, never writes an
        // alias. Read under the array's own BSReadWriteLock (never a mutation, a
        // count only) so this check itself can never race the real writer.
        std::size_t AliasArraySize(RE::Actor* a_actor) {
            if (!a_actor) return 0;
            auto* arr = a_actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
            if (!arr) return 0;
            RE::BSReadLockGuard lock(arr->lock);
            return arr->aliases.size();
        }

        // The crosshair-aimed NPC (never the player). Input-thread read, same as the
        // arbiter's test surface.
        RE::Actor* CrosshairActor() {
            if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                if (auto ref = pick->targetActor.get()) {
                    auto* a = ref->As<RE::Actor>();
                    if (a && !a->IsPlayerRef()) return a;
                }
            }
            return nullptr;
        }

        // No-aim claim target: the NEAREST NPC currently IN COMBAT to the player,
        // regardless of allegiance. Walks ProcessLists::highActorHandles (the
        // engine's own live high-actor set). Returns nullptr (never a random calm
        // NPC) if nothing nearby is actually in combat.
        RE::Actor* NearestCombatant() {
            auto* pl     = RE::ProcessLists::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!pl || !player) return nullptr;
            const RE::NiPoint3 playerPos = player->GetPosition();

            RE::Actor* best = nullptr;
            float      bestDist = 0.0f;
            for (auto& handle : pl->highActorHandles) {
                auto ptr = handle.get();
                RE::Actor* a = ptr.get();
                if (!a || a->IsPlayerRef() || !a->Is3DLoaded() || !a->IsInCombat()) continue;
                const float d = a->GetPosition().GetDistance(playerPos);
                if (!best || d < bestDist) { best = a; bestDist = d; }
            }
            return best;
        }

        // ---- The 0x49 hook (Character vtable ONLY) ----
        struct AliasPkgHook {
            static RE::TESPackage* thunk(RE::Actor* a_this) {
                RE::TESPackage* orig = func(a_this);   // the engine's real alias-package answer

                // Phase-0 census (cheap; no allocation, no lock).
                g_hits.fetch_add(1, std::memory_order_relaxed);
                g_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
                g_lastRetPkg.store(orig ? orig->GetFormID() : 0, std::memory_order_relaxed);
                if (!g_firstLogged.exchange(true)) {
                    spdlog::info("[probe0x49] FIRST HIT -- 0x49 CheckForCurrentAliasPackage FIRES "
                                 "(actor 0x{}, thread {}, returned pkg 0x{} {}). The mechanism is LIVE.",
                                 apmf::log::Hex(a_this->GetFormID()), GetCurrentThreadId(),
                                 apmf::log::Hex(orig ? orig->GetFormID() : 0), Native::PkgType(orig));
                }

                // REDIRECT: if this actor is our claim and we have a client package, offer
                // it instead. The engine adopts it and runs it natively (design.md §5a).
                const RE::FormID want = g_claimPkg.load(std::memory_order_relaxed);
                if (want != 0 && a_this->GetFormID() == g_claimActor.load(std::memory_order_relaxed)) {
                    if (auto* pkg = RE::TESForm::LookupByID<RE::TESPackage>(want)) {
                        g_redirects.fetch_add(1, std::memory_order_relaxed);
                        return pkg;
                    }
                }
                return orig;
            }
            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0x49;   // Actor::CheckForCurrentAliasPackage
        };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[probe0x49] VR runtime -- 0x49 index + EvaluatePackage reloc are SE/AE only; probe NOT armed.");
            return;
        }
        if (g_armed.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        AliasPkgHook::func = charVtbl.write_vfunc(AliasPkgHook::idx, AliasPkgHook::thunk);

        spdlog::info("[probe0x49] ARMED: hooked Character::CheckForCurrentAliasPackage (0x49). "
                     "PHASE 0 running now -- watch for 'FIRST HIT' (hook fires) then the periodic census. "
                     "Test hotkeys: DIK 0x{} toggles a package-offer claim on the aimed NPC; DIK 0x{} toggles "
                     "one on the NEAREST in-combat NPC (no aim needed).",
                     apmf::log::Hex(kProbeKey, 2), apmf::log::Hex(kProbeNearestKey, 2));
        if (kProbePackageForm == 0)
            spdlog::warn("[probe0x49] kProbePackageForm=0 -> PHASE 0 ONLY (hook-fire detection). Set it to a "
                         "real package FormID + rebuild for Phases 1-3 (engage/release/save-load).");
        else
            spdlog::info("[probe0x49] client package = 0x{} (Phases 1-3 armed).", apmf::log::Hex(kProbePackageForm));
    }

    void OnHotkey(std::uint32_t a_code) {
        if (!g_armed.load(std::memory_order_relaxed)) return;
        if (a_code != kProbeKey && a_code != kProbeNearestKey) return;

        // RELEASE if the aimed NPC (or any) is already claimed; else CLAIM the aimed
        // (or nearest-in-combat) NPC.
        const RE::FormID cur = g_claimActor.load(std::memory_order_relaxed);
        if (cur != 0) {
            g_pendActor.store(cur, std::memory_order_relaxed);
            g_claimPkg.store(0, std::memory_order_relaxed);
            g_claimActor.store(0, std::memory_order_relaxed);   // stop redirecting BEFORE the release eval
            g_pend.store(Pend::kRelease, std::memory_order_relaxed);
            spdlog::info("[probe0x49] RELEASE queued -- dropping the offer claim on 0x{} (framework package resumes).",
                         apmf::log::Hex(cur));
            return;
        }

        auto* actor = (a_code == kProbeKey) ? CrosshairActor() : NearestCombatant();
        if (!actor) {
            spdlog::warn("[probe0x49] claim REFUSED -- {}", a_code == kProbeKey
                         ? "aim the crosshair at an NPC (not the player) first."
                         : "no in-combat NPC found near the player.");
            return;
        }
        if (kProbePackageForm == 0) {
            spdlog::warn("[probe0x49] claim REFUSED -- kProbePackageForm=0 (Phase 0 only). Set a real package "
                         "FormID + rebuild to run Phases 1-3.");
            return;
        }
        const RE::FormID id = actor->GetFormID();
        g_claimPkg.store(kProbePackageForm, std::memory_order_relaxed);
        g_claimActor.store(id, std::memory_order_relaxed);        // redirect takes effect on the next 0x49
        g_pendActor.store(id, std::memory_order_relaxed);
        g_pend.store(Pend::kEngage, std::memory_order_relaxed);
        spdlog::info("[probe0x49] ENGAGE queued -- offering package 0x{} to 0x{} '{}'.",
                     apmf::log::Hex(kProbePackageForm), apmf::log::Hex(id),
                     actor->GetName() ? actor->GetName() : "?");
    }

    void OncePerFrame() {
        if (!g_armed.load(std::memory_order_relaxed)) return;

        // Apply a pending EvaluatePackage on the TRUE game thread (this seat is the
        // PlayerCharacter 0xAD tick). One eval per engage/release, resetAI=false.
        const Pend pend = g_pend.exchange(Pend::kNone, std::memory_order_relaxed);
        if (pend != Pend::kNone) {
            const RE::FormID id = g_pendActor.load(std::memory_order_relaxed);
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
                const std::size_t aliasBefore = AliasArraySize(actor);
                Native::EvaluatePackage(actor);
                auto* now = actor->GetCurrentPackage();
                const std::size_t aliasAfter = AliasArraySize(actor);
                spdlog::info("[probe0x49] {} applied on 0x{} -- EvaluatePackage(true,false); GetCurrentPackage now "
                             "0x{} {}. ExtraAliasInstanceArray size {}->{} ({}). {}",
                             pend == Pend::kEngage ? "ENGAGE" : "RELEASE", apmf::log::Hex(id),
                             apmf::log::Hex(now ? now->GetFormID() : 0), Native::PkgType(now),
                             aliasBefore, aliasAfter, aliasBefore == aliasAfter ? "UNCHANGED" : "CHANGED -- unexpected, investigate",
                             pend == Pend::kEngage
                                 ? "Phase 1: expect the CLIENT pkg + the actor sandboxing near its current spot + NO further APMF writes."
                                 : "Phase 2: expect the FRAMEWORK pkg back + exactly one OnPackageChange.");
            }
        }

        // Phase-0 periodic census (~ every 5 s @ 60 fps) so the make-or-break answer is
        // legible without per-call spam.
        static std::uint32_t s_frames = 0;
        if ((++s_frames % 300) != 0) return;
        const std::uint64_t hits = g_hits.load(std::memory_order_relaxed);
        if (hits == 0) {
            spdlog::warn("[probe0x49] PHASE 0: 0x49 has fired 0 times so far. If this stays 0, the vfunc is "
                         "devirtualised/inlined on this runtime -- the mechanism is DEAD (report + stop).");
        } else {
            spdlog::info("[probe0x49] PHASE 0 census: 0x49 hits={}, redirects={}, thread={}, last returned pkg=0x{}. "
                         "Hook is LIVE.", hits, g_redirects.load(std::memory_order_relaxed),
                         g_thread.load(std::memory_order_relaxed),
                         apmf::log::Hex(g_lastRetPkg.load(std::memory_order_relaxed)));
        }
    }

    void ClearOnPreLoad() {
        const RE::FormID cur = g_claimActor.exchange(0, std::memory_order_relaxed);
        g_claimPkg.store(0, std::memory_order_relaxed);
        g_pend.store(Pend::kNone, std::memory_order_relaxed);
        g_pendActor.store(0, std::memory_order_relaxed);
        if (cur != 0)
            spdlog::info("[probe0x49] Phase 3: dropped offer claim on 0x{} for kPreLoadGame -- no engine call "
                         "(actor is about to be replaced); the framework package resumes on the incoming save's "
                         "own first eval, no latch.", apmf::log::Hex(cur));
    }

}
