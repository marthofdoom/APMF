#include "PCH.h"
#include "core/ActorValueObserve.h"
#include "core/Allowance.h"   // SeatVerified / RuntimeSupported / RuntimeAEColumn (mit-3.7 F1, G1)
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/FacetAttribution.h"
#include "core/Log.h"
#include "core/Sources.h"

#include <intrin.h>   // _ReturnAddress

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same one-line
// import core/SpectatorGate.cpp uses.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// HARBINGER 1b -- the OBSERVE-ONLY ACTOR-VALUE SEAT (plan row "Gait (1a), disposition (11), detection
// and sound (16)", Q15 (b) "build every attribution hook in 1b", restricted to VIRTUAL seats).
//
// THE SEAT (disassembly of our unpacked executables, 2026-10-12; agentlog apmf-1b-observe.md).
// Character's ActorValueOwner sub-object vtable is CommonLib's VTABLE_Character[5]:
//   1.6.1170 id 207896 RVA 0x18A5FE8 (RTTI .?AVCharacter@@, COL offset 0xB8),
//   1.5.97   id 261402 RVA 0x165E4D0 (COL offset 0xB0),
//   1.7.104           RVA 0x1923CE8 (COL offset 0xB8; the fork's id map, re-verified by the generator).
// Each is a VerifiedAddresses row (spec "Character.ActorValueOwner" / ".SE"), derived by an RTTI walk
// of our own image and only then compared with the Address Library. Slots, read off the same images:
//   0x04 SetBaseActorValue  AE 0x6B25B0 / SE 0x621070: rcx = the ActorValueOwner sub-object, edx = av,
//        xmm2 = value; the body rebases `this` by -0xB8 (AE) / -0xB0 (SE) to the Actor.
//   0x05 ModActorValue      AE 0x6B2680 / SE 0x6210B0: the same three arguments (edx av, xmm2 value).
//   0x06 RestoreActorValue  AE 0x6B2770 / SE 0x6210F0: edx = the modifier (permanent / temporary /
//        damage), r8d = av, xmm3 = value; forwards to AE 0x6B27A0 / SE 0x621120 with a 5th arg 0.
//   0x07 SetActorValue      AE 0x3C6790 / SE 0x36D280: the base `mov rax,[rcx]; jmp [rax+0x20]`, a
//        TAIL JUMP into slot 0x04 -- so SetActorValue is seen at 0x04 with its own caller's return
//        address. Not hooked itself.
// CommonLib's ActorValueOwner.h declares the same shapes (no hidden out-slot; void returns).
//
// THE RULE (observe only; nothing here can change an answer). On a write whose actor value is one a
// claim channel holds (ch.16 MovementNoiseMult / DetectLifeRange -> Detection facet; ch.1a SpeedMult,
// ch.11 Aggression / Confidence / Assistance / Morality -> ActorValue facet), on an actor that holds the
// WINNING claim of that channel's intent, the CALLER (the thunk's own return address) is resolved by
// Sources::ForAddress: SkyrimSE.exe -> Engine (a Papyrus SetAV / ModAV native runs inside the exe, so it
// is Engine too, the APMF-B71 limit), skse64 -> DirectCalls, another DLL -> that DLL. APMF's own writes
// (core/AvLedger) are skipped. The write is counted (FacetAttribution::Observe: a hit and "stood") and
// logged once per 5 s per (actor, av). The original runs FIRST and always, with the original arguments.
//
// THREADING. Actor-value writes happen on any thread (the main thread, the script VM, BSJobs). The
// thunk reads the ControlMap snapshot (lock-free), the registry (lock-free; ForAddress takes its own
// mutex on a module's first sighting) and the actor's FormID. No engine call, no form lookup.
// ============================================================================

namespace {

    using apmf::log::Hex;
    using apmf::sources::Facet;

    constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
    constexpr std::size_t   kSlotSetBase  = 0x04;
    constexpr std::size_t   kSlotMod      = 0x05;
    constexpr std::size_t   kSlotRestore  = 0x06;
    constexpr std::uintptr_t kAvoOffsetAE = 0xB8;   // 1.6.1170 and 1.7.104 (COL offset of the sub-object)
    constexpr std::uintptr_t kAvoOffsetSE = 0xB0;   // 1.5.97
    constexpr std::uint64_t kHeartbeatMs  = 30000;

    std::atomic<bool>           g_installed{ false };
    std::uintptr_t              g_avoOffset = 0;   // written once at Install, before the vtable writes
    std::atomic<bool>           g_observed{ false };
    std::atomic<std::uint64_t>  g_calls{ 0 }, g_claimed{ 0 }, g_attributed{ 0 }, g_self{ 0 };
    std::atomic<std::uint64_t>  g_lastBeatMs{ 0 };

    // The claim channel that holds an actor value, and the facet its outside writes count on.
    bool Track(RE::ActorValue a_av, APMF_API::Intent& a_intent, Facet& a_facet) {
        switch (a_av) {
        case RE::ActorValue::kMovementNoiseMult:
        case RE::ActorValue::kDetectLifeRange:
            a_intent = APMF_API::kIntent_Detection;
            a_facet  = Facet::kDetection;
            return true;
        case RE::ActorValue::kSpeedMult:
            a_intent = APMF_API::kIntent_Gait;
            a_facet  = Facet::kActorValue;
            return true;
        case RE::ActorValue::kAggression:
        case RE::ActorValue::kConfidence:
        case RE::ActorValue::kAssistance:
        case RE::ActorValue::kMorality:
            a_intent = APMF_API::kIntent_Disposition;
            a_facet  = Facet::kActorValue;
            return true;
        default:
            return false;
        }
    }

    void Note(RE::ActorValueOwner* a_this, RE::ActorValue a_av, float a_value, const char* a_how,
              std::uintptr_t a_ret) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_observed.exchange(true, std::memory_order_relaxed))
            spdlog::info("[avobs] seat OBSERVED: Character's ActorValueOwner {} ran (first actor-value write of the "
                         "session through the seat; principle 5).",
                         a_how);
        APMF_API::Intent intent{};
        Facet            facet{};
        if (!a_this || !Track(a_av, intent, facet) || apmf::ControlMap::Get().ControlledCount() == 0) return;
        const auto*      actor = reinterpret_cast<const RE::Actor*>(reinterpret_cast<std::uintptr_t>(a_this) - g_avoOffset);
        const RE::FormID fid   = actor->GetFormID();
        APMF_API::APMF_Param claim{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(fid, intent, claim)) return;
        g_claimed.fetch_add(1, std::memory_order_relaxed);
        if (apmf::sources::IsSelf(a_ret)) {   // APMF's own ledger write (core/AvLedger)
            g_self.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto src = apmf::sources::ForAddress(a_ret, apmf::sources::Kind::kPlugin);
        apmf::facetattr::Observe(facet, src, fid, "an actor-value write from an address in no module");
        g_attributed.fetch_add(1, std::memory_order_relaxed);
        if (apmf::facetattr::LineDue(facet, fid, 0xA000u | (static_cast<std::uint32_t>(a_av) & 0xFFFu))) {
            const auto base = REL::Module::get().base();
            spdlog::info("[avobs] 0x{} {} av {} value {:.2f} by {} (caller RVA 0x{}) while a claim holds it on the {} "
                         "facet -- OBSERVED, it stands (this seat never denies). Logged at most once per 5 s per "
                         "(actor, av).",
                         Hex(fid), a_how, static_cast<std::uint32_t>(a_av), a_value,
                         apmf::facetattr::NameOr(src, "an unattributed caller"),
                         Hex(a_ret >= base ? a_ret - base : a_ret, 1), apmf::sources::FacetName(facet));
        }
    }

    struct SetBaseHook {
        static void thunk(RE::ActorValueOwner* a_this, RE::ActorValue a_av, float a_value) {
            func(a_this, a_av, a_value);   // the engine's own write, first and always
            try {
                Note(a_this, a_av, a_value, "SetBaseActorValue (0x04, also SetActorValue)",
                     reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct ModHook {
        static void thunk(RE::ActorValueOwner* a_this, RE::ActorValue a_av, float a_value) {
            func(a_this, a_av, a_value);
            try {
                Note(a_this, a_av, a_value, "ModActorValue (0x05)", reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct RestoreHook {
        static void thunk(RE::ActorValueOwner* a_this, RE::ACTOR_VALUE_MODIFIER a_mod, RE::ActorValue a_av,
                          float a_value) {
            func(a_this, a_mod, a_av, a_value);
            try {
                Note(a_this, a_av, a_value, "RestoreActorValue (0x06)",
                     reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

}

namespace apmf::avobserve {

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;
        const int   mode = static_cast<int>(GetPrivateProfileIntA("ActorValueObserve", "iActorValueSeat", 1, kIni));
        const char* why  = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the ActorValueOwner vtable row is verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (mode == 0) {
            why = "[ActorValueObserve] iActorValueSeat=0 in Data/SKSE/Plugins/APMF.ini";
        } else if (mode != 1) {
            why = "[ActorValueObserve] iActorValueSeat is not 0 or 1";
        }
        if (why) {
            spdlog::warn("[avobs] actor-value seat NOT installed -- {}. Outside writes to the actor values Harbinger "
                         "holds are not attributed (nothing is denied either way).",
                         why);
            return;
        }
        const bool ae = apmf::allowance::RuntimeAEColumn();
        REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_Character[5] };
        if (!apmf::allowance::SeatVerified(vt.address(), ae ? "Character.ActorValueOwner" : "Character.ActorValueOwner.SE")) {
            spdlog::error("[avobs] actor-value seat NOT installed (the self-check refused Character's ActorValueOwner "
                          "vtable). Outside actor-value writes are not attributed.");
            return;
        }
        g_avoOffset = ae ? kAvoOffsetAE : kAvoOffsetSE;
        SetBaseHook::func = vt.write_vfunc(kSlotSetBase, SetBaseHook::thunk);
        ModHook::func     = vt.write_vfunc(kSlotMod, ModHook::thunk);
        RestoreHook::func = vt.write_vfunc(kSlotRestore, RestoreHook::thunk);
        using apmf::sources::NoteVfunc;
        NoteVfunc(Facet::kActorValue, "ActorValueObserve.Character.AVO.SetBaseActorValue", vt.address(), kSlotSetBase,
                  SKSE::stl::unrestricted_cast<std::uintptr_t>(&SetBaseHook::thunk), SetBaseHook::func.address());
        NoteVfunc(Facet::kActorValue, "ActorValueObserve.Character.AVO.ModActorValue", vt.address(), kSlotMod,
                  SKSE::stl::unrestricted_cast<std::uintptr_t>(&ModHook::thunk), ModHook::func.address());
        NoteVfunc(Facet::kActorValue, "ActorValueObserve.Character.AVO.RestoreActorValue", vt.address(), kSlotRestore,
                  SKSE::stl::unrestricted_cast<std::uintptr_t>(&RestoreHook::thunk), RestoreHook::func.address());
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[avobs] actor-value seat installed (OBSERVE ONLY): Character ActorValueOwner vtable slots 0x04 / "
                     "0x05 / 0x06 (sub-object offset 0x{}). Outside writes to the actor values a winning claim holds "
                     "are named and counted; nothing is denied.",
                     Hex(g_avoOffset, 2));
    }

    void Heartbeat() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastBeatMs.load(std::memory_order_relaxed) < kHeartbeatMs) return;
        g_lastBeatMs.store(now, std::memory_order_relaxed);   // main-thread-only writer
        spdlog::info("[avobs] H calls={} onClaimedAv={} attributed={} apmfOwn={}", g_calls.load(), g_claimed.load(),
                     g_attributed.load(), g_self.load());
    }

}
