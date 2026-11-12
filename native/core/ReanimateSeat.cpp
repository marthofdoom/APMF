#include "PCH.h"
#include "core/ReanimateSeat.h"

#include "APMF_API.h"
#include "core/Allowance.h"
#include "core/CastSeats.h"   // MatchClaim: the one claim test every caster seat uses
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

// PCH does not pull in <Windows.h> (core/CastSeats.cpp does the same).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// ch.8b -- the Reanimate corpse seat (ABI v42, kCastFlag_CorpseTarget). See the header for
// the engine facts and the design. Everything here runs on the combat thread except Install.
// ============================================================================

namespace apmf::reanimateseat {

    namespace {

        // ---- LAYOUT (read from the three executables, see the header) ----
        // CombatMagicCasterReanimate: aimController +0x20, reanimateTarget +0x28 (a 4-byte actor
        // handle). 0x06 and 0x0D read and write exactly these on 1.6.1170, 1.5.97 and 1.7.104.
        static_assert(offsetof(RE::CombatMagicCasterReanimate, aimController) == 0x20,
                      "CombatMagicCasterReanimate::aimController moved -- re-read slots 0x06 / 0x0D");
        static_assert(offsetof(RE::CombatMagicCasterReanimate, reanimateTarget) == 0x28,
                      "CombatMagicCasterReanimate::reanimateTarget moved -- re-read slots 0x06 / 0x0A / 0x0D");
        static_assert(sizeof(RE::ActorHandle) == 4, "the corpse handle is a 4-byte write in the engine");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point (0x68)");

        constexpr std::size_t    kCheckStartCast     = 0x06;
        constexpr std::size_t    kSetupAimController = 0x0D;
        constexpr std::uintptr_t kCasterAim          = 0x20;   // CombatMagicCasterReanimate::aimController
        constexpr std::uintptr_t kCasterCorpse       = 0x28;   // CombatMagicCasterReanimate::reanimateTarget
        // CombatAimController: +0x28 its CombatController, +0x30 the aim-target override (an actor
        // handle). The same two offsets core/CastSeats.cpp uses, and the ones Reanimate's own 0x0D
        // reads (`mov rdx,[rdx+0x28]`) and writes (`mov [rcx+0x30],eax`) on all three builds.
        constexpr std::uintptr_t kAimController      = 0x28;
        constexpr std::uintptr_t kAimTargetOverride  = 0x30;
        // The engine's own keep test on the cached corpse: form flags Deleted (0x20) or Disabled
        // (0x800) set -> not kept (`test dword ptr [rbx+0x10], 0x820`).
        constexpr std::uint32_t  kGoneFlags          = 0x820;

        constexpr std::uint64_t kLogThrottleMs = 1500;

        using CheckStartCast_t     = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        using SetupAimController_t = void (*)(RE::CombatMagicCaster*, void*);

        std::atomic<bool>                                  g_installed{ false };
        std::atomic<const char*>                           g_whyNot{ "the Reanimate corpse seat has not been installed yet" };
        std::atomic<std::uintptr_t>                        g_aimVtable{ 0 };   // VTABLE_CombatProjectileAimController, verified
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_startOrig;        // read-only after Install
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_aimOrig;          // read-only after Install

        // PASSIVE (principle 5): does each seat run at all, and how often does a corpse claim
        // answer. [seat 0x06, seat 0x0D] x [calls, claim kept / rewritten, claim refused].
        std::array<std::array<std::atomic<std::uint32_t>, 3>, 2> g_counts{};
        std::array<std::atomic<bool>, 2>                         g_firstCall{};
        std::array<std::atomic<bool>, 2>                         g_firstClaim{};

        std::mutex                                       g_rlMx;   // leaf lock, never held across an engine call
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;

        bool LogDue(RE::FormID a_actor, std::uint32_t a_kind) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) | a_kind;
            std::scoped_lock lk(g_rlMx);
            auto& last = g_lastLogMs[key];
            if (now - last < kLogThrottleMs) return false;
            last = now;
            return true;
        }

        std::uint32_t* CorpseSlot(RE::CombatMagicCaster* a_this) {
            return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_this) + kCasterCorpse);
        }

        // The aim-target override of `a_aim`, or null when `a_aim` is not EXACTLY a
        // CombatProjectileAimController (the same identity guard as core/CastSeats.cpp's 0x0D).
        std::uint32_t* AimSlot(void* a_aim) {
            const auto expected = g_aimVtable.load(std::memory_order_relaxed);
            if (!a_aim || expected == 0 || *reinterpret_cast<std::uintptr_t*>(a_aim) != expected) return nullptr;
            return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_aim) + kAimTargetOverride);
        }

        void NoteCall(int a_seat, std::uint32_t a_slotIdx) {
            g_counts[a_seat][0].fetch_add(1, std::memory_order_relaxed);
            if (!g_firstCall[a_seat].exchange(true, std::memory_order_relaxed))
                spdlog::info("[ch.8b reanimate] FIRST call of the Reanimate caster's seat 0x{:02X} since install "
                             "(proves the seat executes; a corpse claim is answered here).",
                             a_slotIdx);
        }

        // Does a live corpse claim (kCastFlag_CorpseTarget) drive this caster's spell?
        bool CorpseClaim(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc, std::uintptr_t a_prev, bool a_count,
                         apmf::castseats::ClaimMatch& a_m) {
            if (!a_this || !a_cc || !a_this->magicItem) return false;
            // The flag is read FIRST, on a plain claim read, so a claim without it (target 0, the
            // engine's own corpse) is never ranked or counted here: this seat does not answer it.
            {
                const auto attPtr = a_cc->attackerHandle.get();
                auto*      actor  = attPtr.get();
                if (!actor) return false;
                apmf::CastSeatClaim c{};
                if (!apmf::ControlMap::Get().TryGetCastSeatClaimForForm(actor->GetFormID(), a_this->magicItem->GetFormID(), c))
                    return false;
                if ((c.flags & APMF_API::kCastFlag_CorpseTarget) == 0) return false;
                if (c.flags & APMF_API::kCastFlag_DenyHandOnly) return false;
                if (c.target == 0 || c.target == actor->GetFormID() || !c.targetHandle) return false;
            }
            if (!apmf::castseats::MatchClaim(a_this, a_cc, a_prev, a_count, a_m)) return false;
            // Re-tested on the claim MatchClaim copied out (a second RCU read; the claim may have changed).
            if ((a_m.claim.flags & APMF_API::kCastFlag_CorpseTarget) == 0) return false;
            if (a_m.claim.flags & APMF_API::kCastFlag_DenyHandOnly) return false;
            return a_m.claim.target != 0 && a_m.claim.target != a_m.actor;
        }

        std::uintptr_t StartPrev(std::uintptr_t a_vt) {
            const auto it = g_startOrig.find(a_vt);
            return it != g_startOrig.end() ? it->second : 0;
        }

        // ====================================================================
        // SEAT 0x06 -- WHETHER, and which corpse the engine's own keep test judges.
        // ====================================================================
        bool CheckStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            if (!a_this) return false;
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_startOrig.find(vt);
            if (oit == g_startOrig.end()) return false;   // foreign vtable: benign "don't start", touch nothing
            const auto orig = reinterpret_cast<CheckStartCast_t>(oit->second);
            if (!g_installed.load(std::memory_order_relaxed)) return orig(a_this, a_cc);
            NoteCall(0, static_cast<std::uint32_t>(kCheckStartCast));

            apmf::castseats::ClaimMatch m{};
            if (!CorpseClaim(a_this, a_cc, oit->second, true, m)) return orig(a_this, a_cc);

            // The claim's corpse goes in BEFORE the engine runs, so the engine's own keep test
            // (resolves, dead, not disabled / deleted, its validity test) is what judges it.
            auto* const         corpse = CorpseSlot(a_this);
            const std::uint32_t want   = m.claim.targetHandle.native_handle();
            *corpse = want;
            const bool          native = orig(a_this, a_cc);
            const std::uint32_t after  = *corpse;
            const bool firstClaim = !g_firstClaim[0].exchange(true, std::memory_order_relaxed);

            if (after == want) {
                // Kept (or never reached: the engine's earlier gates said NO and left +0x28 alone).
                if (native) {
                    // The keep branch does not write the aim override; the search branch does. Set it
                    // as the search branch would have.
                    void* aim = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(a_this) + kCasterAim);
                    if (auto* slot = AimSlot(aim)) *slot = want;
                    g_counts[0][1].fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_counts[0][2].fetch_add(1, std::memory_order_relaxed);
                }
                if (firstClaim || LogDue(m.actor, native ? 0x0601u : 0x0602u))
                    spdlog::info("[ch.8b reanimate 0x06] 0x{} CheckStartCast -> {} (spell 0x{}, claimed corpse 0x{}): {}",
                                 apmf::log::Hex(m.actor), native ? "YES" : "NO", apmf::log::Hex(m.driven),
                                 apmf::log::Hex(m.claim.target),
                                 native ? "the engine's own keep test accepted the claim's corpse."
                                        : "the engine's own gates before the corpse test said no (its base start "
                                          "check, or the spell's effect already on the caster); the claim stands.");
                return native;
            }

            // Replaced: the engine's keep test refused the claim's corpse and its own search ran.
            // Its pick (or 0) is in +0x28 and in aim+0x30. Take it out again: the engine writes 0 to
            // both when its search finds nothing, and the answer is NO.
            *corpse = 0;
            void* aim = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(a_this) + kCasterAim);
            if (auto* slot = AimSlot(aim); slot && *slot == after) *slot = 0;
            g_counts[0][2].fetch_add(1, std::memory_order_relaxed);
            if (firstClaim || LogDue(m.actor, 0x0603u))
                spdlog::warn("[ch.8b reanimate 0x06] 0x{} CheckStartCast -> NO (spell 0x{}): the engine's own keep test "
                             "REFUSED the claimed corpse 0x{} (it does not resolve, is not dead, is disabled or deleted, "
                             "or the spell cannot raise it). The engine's own pick (handle 0x{}) was removed and does "
                             "not cast; the claim stands.",
                             apmf::log::Hex(m.actor), apmf::log::Hex(m.driven), apmf::log::Hex(m.claim.target),
                             apmf::log::Hex(after));
            return false;
        }

        // ====================================================================
        // SEAT 0x0D -- WHERE. The engine searches again here, so its pick is replaced after it.
        // ====================================================================
        void SetupAimControllerThunk(RE::CombatMagicCaster* a_this, void* a_aim) {
            if (!a_this) return;
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_aimOrig.find(vt);
            if (oit == g_aimOrig.end()) return;   // foreign vtable: touch nothing
            const auto orig = reinterpret_cast<SetupAimController_t>(oit->second);

            orig(a_this, a_aim);   // ENGINE FIRST: its search, +0x28, +0x20 = aim, aim+0x30, aim+0x3C |= 0x200

            if (!g_installed.load(std::memory_order_relaxed)) return;
            NoteCall(1, static_cast<std::uint32_t>(kSetupAimController));

            auto* const aimSlot = AimSlot(a_aim);
            if (!aimSlot) return;   // not a projectile aim controller: never a blind read or write

            auto* cc = *reinterpret_cast<RE::CombatController**>(reinterpret_cast<std::uintptr_t>(a_aim) + kAimController);
            apmf::castseats::ClaimMatch m{};
            if (!CorpseClaim(a_this, cc, StartPrev(vt), false, m)) return;

            auto* const         corpse = CorpseSlot(a_this);
            const std::uint32_t engine = *corpse;
            const std::uint32_t want   = m.claim.targetHandle.native_handle();
            // The claim's corpse must still be one: resolves, dead, not disabled or deleted (the
            // engine's own keep preconditions; its validity test ran at 0x06).
            bool ok = false;
            {
                const auto ptr = m.claim.targetHandle.get();
                if (auto* a = ptr.get())
                    ok = a->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kDead &&
                         (a->GetFormFlags() & kGoneFlags) == 0;
            }
            const std::uint32_t put = ok ? want : 0;
            *corpse  = put;
            *aimSlot = put;
            g_counts[1][ok ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
            const bool firstClaim = !g_firstClaim[1].exchange(true, std::memory_order_relaxed);
            if (ok) {
                if (firstClaim || LogDue(m.actor, 0x0D01u))
                    spdlog::info("[ch.8b reanimate 0x0D] 0x{} aim -> claimed corpse 0x{} (handle 0x{}); the engine's "
                                 "own search had picked handle 0x{}{} (spell 0x{}).",
                                 apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.target), apmf::log::Hex(want),
                                 apmf::log::Hex(engine), engine == want ? ", the same corpse" : "",
                                 apmf::log::Hex(m.driven));
            } else if (firstClaim || LogDue(m.actor, 0x0D02u)) {
                spdlog::warn("[ch.8b reanimate 0x0D] 0x{} aim: the claimed corpse 0x{} is no longer a corpse (it does "
                             "not resolve, is not dead, or is disabled / deleted). The engine's own pick (handle "
                             "0x{}) was removed; this cast has no corpse (spell 0x{}).",
                             apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.target), apmf::log::Hex(engine),
                             apmf::log::Hex(m.driven));
            }
        }

        bool ReadIniFlag(const char* a_key, long a_default) {
            return GetPrivateProfileIntA("CastSeats", a_key, a_default, "Data/SKSE/Plugins/APMF.ini") != 0;
        }

    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

    const char* NotInstalledReason() { return g_whyNot.load(std::memory_order_relaxed); }

    void Install() {
        static std::atomic<bool> once{ false };
        if (once.exchange(true)) return;

        if (REL::Module::IsVR()) {
            g_whyNot.store("VR runtime: the Reanimate caster's slots and layout are not verified there");
            spdlog::warn("[ch.8b reanimate] VR runtime -- the Reanimate corpse seat was NOT installed. A "
                         "kCastFlag_CorpseTarget request is refused.");
            return;
        }
        if (!allowance::RuntimeSupported()) {
            g_whyNot.store("this runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104");
            spdlog::error("[ch.8b reanimate] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- the Reanimate "
                          "corpse seat was NOT installed (REFUSED). A kCastFlag_CorpseTarget request is refused.",
                          REL::Module::get().version().string("."));
            return;
        }
        if (!ReadIniFlag("EnableReanimateSeat", 1)) {
            g_whyNot.store("[CastSeats] EnableReanimateSeat=0");
            spdlog::info("[ch.8b reanimate] disabled by [CastSeats] EnableReanimateSeat=0 -- a kCastFlag_CorpseTarget "
                         "request is refused; a reanimate claim with target 0 casts at the engine's own corpse.");
            return;
        }

        // Everything both seats depend on is checked BEFORE the first write, so they install
        // together or not at all.
        REL::Relocation<std::uintptr_t> casterVt{ RE::VTABLE_CombatMagicCasterReanimate[0] };
        REL::Relocation<void*>          casterTD{ RE::RTTI_CombatMagicCaster };
        REL::Relocation<std::uintptr_t> aimVt{ RE::VTABLE_CombatProjectileAimController[0] };
        REL::Relocation<void*>          aimTD{ RE::RTTI_CombatAimController };
        if (!allowance::SeatVerified(casterVt.address(), "ReanimateSeat.CombatMagicCasterReanimate") ||
            !allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(casterTD.get()), "ReanimateSeat.RTTI.CombatMagicCaster") ||
            !allowance::DerivesFrom(casterVt.address(), casterTD.get()) ||
            !allowance::SeatVerified(aimVt.address(), "ReanimateSeat.CombatProjectileAimController") ||
            !allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(aimTD.get()), "ReanimateSeat.RTTI.CombatAimController") ||
            !allowance::DerivesFrom(aimVt.address(), aimTD.get())) {
            g_whyNot.store("the self-check refused the Reanimate caster vtable or the aim controller vtable");
            spdlog::error("[ch.8b reanimate] the Reanimate caster vtable or the projectile aim controller vtable did not "
                          "verify on this runtime -- the Reanimate corpse seat was NOT installed (REFUSED). A "
                          "kCastFlag_CorpseTarget request is refused.");
            return;
        }
        g_aimVtable.store(aimVt.address(), std::memory_order_relaxed);

        const std::array<REL::VariantID, 1> vts{ RE::VTABLE_CombatMagicCasterReanimate[0] };
        const int nStart = allowance::InstallOnVtables(vts, kCheckStartCast, &CheckStartCastThunk, casterTD.get(),
                                                       "ch.8b-reanimate06", g_startOrig);
        const int nAim   = nStart == 1 ? allowance::InstallOnVtables(vts, kSetupAimController, &SetupAimControllerThunk,
                                                                     casterTD.get(), "ch.8b-reanimate0D", g_aimOrig)
                                       : 0;
        if (nStart != 1 || nAim != 1) {
            // Unreachable after the checks above. The thunks chain to the engine while g_installed
            // is false, so a half install changes nothing.
            g_whyNot.store("the Reanimate caster vtable refused a seat at install");
            spdlog::error("[ch.8b reanimate] seat install incomplete (0x06 {}/1, 0x0D {}/1) -- the Reanimate corpse "
                          "seat stays OFF (its thunks chain to the engine). A kCastFlag_CorpseTarget request is refused.",
                          nStart, nAim);
            return;
        }
        g_installed.store(true, std::memory_order_relaxed);
        spdlog::info("[ch.8b reanimate] Reanimate corpse seat installed: 0x06 CheckStartCast and 0x0D SetupAimController "
                     "on the CombatMagicCasterReanimate vtable. While a kIntent_Cast claim with kCastFlag_CorpseTarget "
                     "stands, the engine's own reanimate cast is aimed at the claim's corpse, and only if the engine's "
                     "own keep test accepts it.");
    }

}
