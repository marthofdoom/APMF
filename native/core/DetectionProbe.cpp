#include "PCH.h"
#include "core/DetectionProbe.h"
#include "core/Allowance.h"   // RuntimeSupported, SeatVerified
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Sources.h"

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// See core/DetectionProbe.h.

namespace apmf::detectionprobe {

    namespace {

        using apmf::log::Hex;

        constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
        constexpr std::uint64_t kHeartbeatMs  = 60000;
        constexpr std::uint64_t kPinLogMs     = 5000;
        constexpr std::int32_t  kSentinel     = 1000;   // SmartNPC's forced values (+1000 detect, -1000 suppress)

        // AE / 1.7.104 (AE column) and SE: {function id, offset}. Callee ids for the log only.
        constexpr std::uint64_t kFnAE = 42742, kFnSE = 41659;
        constexpr std::uint32_t kOffAE = 0x67B, kOffSE = 0x526;   // 1.7.104 uses the AE offset
        constexpr std::uintptr_t kFn17Rva = 0x7C5220;               // 1.7.104 raw RVA (see Install)

        using Detect_t = void (*)(RE::Actor*, RE::Actor*, std::int32_t*, void*, void*, void*, void*, void*, void*,
                                  void*);
        std::atomic<std::uintptr_t> g_next{ 0 };
        std::atomic<bool>           g_installed{ false };

        std::atomic<std::uint64_t> g_calls{ 0 };
        std::atomic<std::uint64_t> g_pinnedSourceCalls{ 0 };
        std::atomic<std::uint64_t> g_pinTargetCalls{ 0 };
        std::atomic<std::uint64_t> g_pinSentinelLow{ 0 }, g_pinSentinelHigh{ 0 };
        std::atomic<std::uint64_t> g_otherSentinelLow{ 0 }, g_otherSentinelHigh{ 0 };
        std::atomic<std::int32_t>  g_lastPinValue{ 0 };
        std::atomic<std::uint64_t> g_lastPinLogMs{ 0 };
        std::uint64_t              g_lastHeartbeatMs = 0;   // main thread
        apmf::sources::SourceId    g_siteOwner = apmf::sources::kNoSource;

        void Observe(RE::Actor* a_src, RE::Actor* a_tgt, const std::int32_t* a_value);

        void Thunk(RE::Actor* a_src, RE::Actor* a_tgt, std::int32_t* a_value, void* a4, void* a5, void* a6, void* a7,
                   void* a8, void* a9, void* a10) {
            reinterpret_cast<Detect_t>(g_next.load(std::memory_order_relaxed))(a_src, a_tgt, a_value, a4, a5, a6, a7,
                                                                               a8, a9, a10);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            try {
                Observe(a_src, a_tgt, a_value);
            } catch (...) {   // nothing may unwind into the engine's detection frame
            }
        }

        void Observe(RE::Actor* a_src, RE::Actor* a_tgt, const std::int32_t* a_value) {
            if (!a_src || !a_tgt || !a_value || apmf::ControlMap::Get().ControlledCount() == 0) return;
            APMF_API::APMF_Param pin{};
            if (!apmf::ControlMap::Get().TryGetOwningClaim(a_src->GetFormID(), APMF_API::kIntent_TargetPin, pin) ||
                pin.form == 0)
                return;
            g_pinnedSourceCalls.fetch_add(1, std::memory_order_relaxed);
            const std::int32_t v = *a_value;
            if (a_tgt->GetFormID() == pin.form) {
                g_pinTargetCalls.fetch_add(1, std::memory_order_relaxed);
                if (v <= -kSentinel) g_pinSentinelLow.fetch_add(1, std::memory_order_relaxed);
                if (v >= kSentinel) g_pinSentinelHigh.fetch_add(1, std::memory_order_relaxed);
                const auto last = g_lastPinValue.exchange(v, std::memory_order_relaxed);
                const auto now  = apmf::clock::MonotonicMs();
                auto       prev = g_lastPinLogMs.load(std::memory_order_relaxed);
                if ((last != v || now - prev >= kHeartbeatMs) && now - prev >= kPinLogMs &&
                    g_lastPinLogMs.compare_exchange_strong(prev, now, std::memory_order_relaxed)) {
                    spdlog::info("[detect-probe] 0x{} detecting its PIN 0x{}: value {} (the chain at the site ends in "
                                 "{}). PROBE ONLY: unchanged.",
                                 Hex(a_src->GetFormID()), Hex(pin.form), v,
                                 g_siteOwner >= 0 ? std::string(apmf::sources::Name(g_siteOwner)) : std::string("the engine"));
                }
            } else {
                if (v <= -kSentinel) g_otherSentinelLow.fetch_add(1, std::memory_order_relaxed);
                if (v >= kSentinel) g_otherSentinelHigh.fetch_add(1, std::memory_order_relaxed);
            }
        }

    }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        if (REL::Module::IsVR() || !apmf::allowance::RuntimeSupported()) {
            spdlog::warn("[detect-probe] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 (VR included) -- NOT "
                         "installed.",
                         REL::Module::get().version().string("."));
            return;
        }
        if (GetPrivateProfileIntA("Harbinger", "bDetectionProbe", 1, kIni) == 0) {
            spdlog::info("[detect-probe] [Harbinger] bDetectionProbe=0 -- NOT installed.");
            return;
        }
        // 1.7.104 has no Address Library and the fork's MIT id table has no row for 42742 (an id
        // miss is FATAL there): its function is a raw-RVA row (tools/verified_addresses
        // idmap-1.7.104-apmf.csv rva:0x7B0710 -> 0x7C5220, the same +0x67B call, context
        // byte-identical but the rel32). Checked first: RuntimeAEColumn() is true on 1.7.104 too.
        const bool           on17 = apmf::allowance::IsRuntime1_7_104();
        const bool           ae   = apmf::allowance::RuntimeAEColumn();
        const std::uintptr_t fn   = on17 ? REL::Offset(kFn17Rva).address() : REL::ID(ae ? kFnAE : kFnSE).address();
        const std::uintptr_t site = fn + (ae ? kOffAE : kOffSE);
        const bool verified = on17 ? apmf::allowance::SeatVerified(fn, "DetectionProbe.Function")
                                   : apmf::allowance::SeatVerified(site, "DetectionProbe.Site");
        if (!verified) {
            spdlog::error("[detect-probe] NOT installed (self-check refused the call site).");
            return;
        }
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            spdlog::error("[detect-probe] NOT installed: the site 0x{} does not hold an E8 call.",
                          Hex(site - REL::Module::get().base(), 0));
            return;
        }
        // A private trampoline, leaked on purpose (see the header).
        auto* tramp = new SKSE::Trampoline("apmf-detection-probe");
        tramp->create(64);
        // The previous target first, so the thunk never runs with no next: write_call returns the
        // displaced destination; set g_next from the current E8 BEFORE the patch.
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
            g_next.store(site + 5 + static_cast<std::intptr_t>(rel), std::memory_order_release);
            // The module the site already reaches (set before any thunk can read it).
            g_siteOwner = apmf::sources::ForAddress(g_next.load(), apmf::sources::Kind::kInterposer);
            if (g_siteOwner == apmf::sources::kEngineSource) g_siteOwner = apmf::sources::kNoSource;
        }
        const std::uintptr_t prev = tramp->write_call<5>(site, &Thunk);
        g_next.store(prev, std::memory_order_release);
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kDetection, "DetectionProbe.Site", site, prev);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[detect-probe] installed PASSIVE on the detection-value call site ({} {}+0x{:X}): every call goes "
                     "through unchanged to {}. Counts calls; logs the value a pinned actor's detection of its pin comes "
                     "out with ([Harbinger] bDetectionProbe).",
                     on17 ? "1.7.104 raw" : ae ? "AE" : "SE", on17 ? kFn17Rva : (ae ? kFnAE : kFnSE), ae ? kOffAE : kOffSE,
                     g_siteOwner >= 0 ? std::string(apmf::sources::Name(g_siteOwner)) + " (patched before Harbinger)"
                                      : std::string("the engine's detection routine"));
    }

    void Heartbeat() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (g_lastHeartbeatMs == 0) g_lastHeartbeatMs = now;
        if (now - g_lastHeartbeatMs < kHeartbeatMs) return;
        g_lastHeartbeatMs = now;
        spdlog::info("[detect-probe] H calls {} pinnedSourceCalls {} pinTargetCalls {} pin<=-1000 {} pin>=1000 {} "
                     "other<=-1000 {} other>=1000 {} lastPinValue {}",
                     g_calls.load(), g_pinnedSourceCalls.load(), g_pinTargetCalls.load(), g_pinSentinelLow.load(),
                     g_pinSentinelHigh.load(), g_otherSentinelLow.load(), g_otherSentinelHigh.load(),
                     g_lastPinValue.load());
    }

}
