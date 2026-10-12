#include "PCH.h"
#include "core/PackageSources.h"
#include "core/Allowance.h"   // RuntimeSupported, SeatVerified
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Sources.h"

#include <intrin.h>   // _ReturnAddress

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// This DLL's own image base (the MSVC linker symbol).
extern "C" char __ImageBase;

// See core/PackageSources.h.

namespace apmf::packagesources {

    namespace {

        using apmf::log::Hex;
        using apmf::sources::Facet;

        constexpr const char*   kIni            = "Data/SKSE/Plugins/APMF.ini";
        constexpr std::uint64_t kPollMs         = 500;
        constexpr std::uint64_t kHeartbeatMs    = 60000;
        constexpr std::size_t   kMaxCallers     = 32;
        constexpr std::size_t   kProbeLineCap   = 200;
        constexpr std::size_t   kSceneSlot      = 0x4A;   // Actor::GetCurrentScene (both runtimes)

        std::atomic<bool> g_probeInstalled{ false };

        // -- capture poll (main thread) --
        std::unordered_map<RE::FormID, RE::FormID> g_lastReported;   // actor -> running package last logged
        std::uint64_t                              g_lastPollMs = 0;

        // -- the 0x4A probe --
        struct CallerStat { std::uintptr_t rva; std::uint64_t hits; };
        std::mutex                          g_probeMx;
        std::vector<CallerStat>             g_callers;                // distinct callers seen for claimed actors
        std::unordered_map<std::uint64_t, bool> g_firstSeen;          // (actor, caller rva) -> logged
        std::atomic<std::uint64_t>          g_probeHits{ 0 };          // claimed actor, a scene stood
        std::atomic<std::uint64_t>          g_probeLines{ 0 };
        std::uint64_t                       g_lastHeartbeatMs = 0;     // main thread
        std::uintptr_t                      g_selfLo = 0, g_selfHi = 0;

        struct Recognized {
            apmf::sources::SourceId src = apmf::sources::kNoSource;
            const char*             how = "";
            RE::FormID              scene = 0, quest = 0;
        };

        Recognized Recognize(const RE::TESPackage* pkg, const RE::BGSScene* scene) {
            Recognized r;
            if (scene) {
                r.scene = scene->GetFormID();
                r.quest = scene->parentQuest ? scene->parentQuest->GetFormID() : 0;
                r.how   = "scene";
                if (const auto* f = scene->GetFile(0)) r.src = apmf::sources::Register(f->GetFilename(), apmf::sources::Kind::kPlugin);
                return r;
            }
            if (!pkg) return r;
            r.quest = pkg->ownerQuest ? pkg->ownerQuest->GetFormID() : 0;
            if (const auto* f = pkg->GetFile(0)) {
                r.how = r.quest ? "quest package" : "package";
                r.src = apmf::sources::Register(f->GetFilename(), apmf::sources::Kind::kPlugin);
            } else {
                r.how = "package created at runtime";
                r.src = apmf::sources::kEngineSource;
            }
            return r;
        }

        // The claim the package facet is enforced FOR: the offer's own client, or -- for
        // ch.19's internal offer, which no client filed -- the winning travel claim's.
        int ClaimPrio(RE::FormID actor, APMF_API::Handle offerH, std::string& clientName) {
            APMF_API::Handle h = offerH;
            if (apmf::sources::ClientForHandle(h) == apmf::sources::kNoSource) {
                APMF_API::APMF_Param p{};
                float                b  = 0.0f;
                APMF_API::Handle     th = APMF_API::kInvalidHandle;
                if (ControlMap::Get().TryGetOwningClaimBasis(actor, APMF_API::kIntent_Travel, p, b, &th)) h = th;
            }
            apmf::sources::SourceId c = apmf::sources::kNoSource;
            const int prio = apmf::sources::ClaimPriority(h, Facet::kPackage, &c);
            clientName = c != apmf::sources::kNoSource ? std::string(apmf::sources::Name(c)) : std::string("an internal claim");
            return prio;
        }

        struct SceneProbe {
            static RE::BGSScene* thunk(const RE::Actor* a_this) {
                RE::BGSScene* r = func(a_this);
                if (!r || !a_this || ControlMap::Get().ControlledCount() == 0) return r;
                const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                if (ret >= g_selfLo && ret < g_selfHi) return r;   // Harbinger's own poll asking
                try {
                    Observe(a_this, r, ret);
                } catch (...) {
                }
                return r;
            }
            static void Observe(const RE::Actor* a, const RE::BGSScene* scene, std::uintptr_t ret) {
                APMF_API::APMF_Param claim{};
                if (!ControlMap::Get().TryGetOwningClaim(a->GetFormID(), APMF_API::kIntent_OfferPackage, claim) ||
                    claim.form == 0)
                    return;
                g_probeHits.fetch_add(1, std::memory_order_relaxed);
                const std::uintptr_t base = REL::Module::get().base();
                const std::uintptr_t rva  = ret - base;
                bool first = false;
                {
                    std::scoped_lock lk(g_probeMx);
                    bool found = false;
                    for (auto& c : g_callers)
                        if (c.rva == rva) { ++c.hits; found = true; break; }
                    if (!found && g_callers.size() < kMaxCallers) g_callers.push_back({ rva, 1 });
                    const std::uint64_t key = (static_cast<std::uint64_t>(a->GetFormID()) << 32) ^ rva;
                    if (g_firstSeen.size() < 4096) first = g_firstSeen.emplace(key, true).second;
                }
                if (!first || g_probeLines.fetch_add(1, std::memory_order_relaxed) >= kProbeLineCap) return;
                const auto* f = scene->GetFile(0);
                spdlog::info("[scene-probe] 0x{} GetCurrentScene (0x4A) -> scene 0x{} ({}) asked from SkyrimSE+0x{} while "
                             "the package claim 0x{} stands. PROBE ONLY: the answer is returned unchanged.",
                             Hex(a->GetFormID()), Hex(scene->GetFormID()),
                             f ? f->GetFilename() : std::string_view("no file"), Hex(rva, 0), Hex(claim.form));
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        void Heartbeat(std::uint64_t now) {
            if (!g_probeInstalled.load(std::memory_order_relaxed)) return;
            if (g_lastHeartbeatMs == 0) g_lastHeartbeatMs = now;
            if (now - g_lastHeartbeatMs < kHeartbeatMs) return;
            g_lastHeartbeatMs = now;
            std::vector<CallerStat> v;
            {
                std::scoped_lock lk(g_probeMx);
                v = g_callers;
            }
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.hits > b.hits; });
            std::string top;
            for (std::size_t i = 0; i < v.size() && i < 8; ++i)
                top += fmt::format("{}+0x{:X}x{}", i ? ", " : "", v[i].rva, v[i].hits);
            spdlog::info("[scene-probe] H claimed-actor scene answers {} from {} distinct caller(s): {}",
                         g_probeHits.load(std::memory_order_relaxed), v.size(), top.empty() ? "none" : top);
        }

    }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        if (REL::Module::IsVR() || !apmf::allowance::RuntimeSupported()) {
            spdlog::warn("[scene-probe] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 (VR included) -- the 0x4A "
                         "scene probe is NOT installed. The package capture poll still runs.",
                         REL::Module::get().version().string("."));
            return;
        }
        if (GetPrivateProfileIntA("Harbinger", "bSceneProbe", 1, kIni) == 0) {
            spdlog::info("[scene-probe] [Harbinger] bSceneProbe=0 -- the 0x4A scene probe is NOT installed.");
            return;
        }
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        if (!apmf::allowance::SeatVerified(vtChar.address(), "PackageSources.Character.GetCurrentScene(probe)")) {
            spdlog::error("[scene-probe] NOT installed (self-check refused the Character vtable).");
            return;
        }
        {   // our own image range, so the capture poll's own GetCurrentScene calls are not probed
            const auto  lo  = reinterpret_cast<std::uintptr_t>(&__ImageBase);
            const auto* dos = reinterpret_cast<const std::uint8_t*>(lo);
            const auto  pe  = *reinterpret_cast<const std::uint32_t*>(dos + 0x3C);
            // IMAGE_NT_HEADERS64: Signature(4) + FileHeader(20) + OptionalHeader.SizeOfImage at +56
            const auto  sizeOfImage = *reinterpret_cast<const std::uint32_t*>(dos + pe + 4 + 20 + 56);
            g_selfLo = lo;
            g_selfHi = lo + sizeOfImage;
        }
        SceneProbe::func = vtChar.write_vfunc(kSceneSlot, SceneProbe::thunk);
        apmf::sources::NoteVfunc(Facet::kPackage, "PackageSources.Character.GetCurrentScene(probe)", vtChar.address(),
                                 kSceneSlot, SKSE::stl::unrestricted_cast<std::uintptr_t>(&SceneProbe::thunk),
                                 SceneProbe::func.address());
        g_probeInstalled.store(true, std::memory_order_release);
        spdlog::info("[scene-probe] installed OBSERVE-ONLY on Character::GetCurrentScene (slot 0x{:X}, chaining): for an "
                     "actor holding a package claim it logs each distinct caller that asks while a scene stands. It "
                     "changes nothing ([Harbinger] bSceneProbe).",
                     kSceneSlot);
    }

    void Poll() {
        const auto now = apmf::clock::MonotonicMs();
        Heartbeat(now);
        if (ControlMap::Get().ControlledCount() == 0) return;
        if (now - g_lastPollMs < kPollMs) return;
        g_lastPollMs = now;
        try {
            const auto actors = ControlMap::Get().ClaimedActors(APMF_API::kIntent_OfferPackage);
            for (auto* actor : actors) {
                if (!actor) continue;
                const RE::FormID     id = actor->GetFormID();
                APMF_API::APMF_Param claim{};
                float                basis = 0.0f;
                APMF_API::Handle     h     = APMF_API::kInvalidHandle;
                if (!ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_OfferPackage, claim, basis, &h) ||
                    claim.form == 0)
                    continue;
                const auto* cur   = actor->GetCurrentPackage();
                const auto  curId = cur ? cur->GetFormID() : 0;
                if (!cur || curId == claim.form) {
                    g_lastReported.erase(id);
                    continue;
                }
                const RE::BGSScene* scene = actor->GetCurrentScene();
                const Recognized    rec   = Recognize(cur, scene);
                if (rec.src == apmf::sources::kNoSource) {
                    if (g_lastReported[id] != curId) {
                        g_lastReported[id] = curId;
                        apmf::sources::Unattributed(Facet::kPackage, id,
                                                    "a scene with no plugin file runs instead of the claimed package");
                    }
                    continue;
                }
                apmf::sources::NoteHit(rec.src, Facet::kPackage);
                if (g_lastReported[id] == curId) continue;   // logged on the transition only
                g_lastReported[id] = curId;
                std::string client;
                const int   claimPrio = ClaimPrio(id, h, client);
                const int   srcPrio   = apmf::sources::Priority(rec.src, Facet::kPackage);
                const bool  outranked = claimPrio > srcPrio;
                if (outranked) apmf::sources::NoteOverruled(rec.src, Facet::kPackage);
                else           apmf::sources::NoteStood(rec.src, Facet::kPackage);
                spdlog::info("[sources] Package 0x{} captured {}: its {} runs 0x{}{}{} instead of the claimed 0x{}. "
                             "Priority {} vs the claim's {} ({}): {}",
                             Hex(id), apmf::sources::Name(rec.src), rec.how, Hex(curId),
                             rec.scene ? fmt::format(" (scene 0x{})", Hex(rec.scene)) : std::string(),
                             rec.quest ? fmt::format(" (quest 0x{})", Hex(rec.quest)) : std::string(), Hex(claim.form),
                             srcPrio, claimPrio, client,
                             outranked ? (rec.scene ? "the claim OUTRANKS it -- would hold the scene off; the scene seat is "
                                                      "PROBE-ONLY in this build, nothing is enforced"
                                                    : "the claim OUTRANKS it -- not enforced (only scenes are held off, "
                                                      "and that seat is probe-only in this build)")
                                       : "it stands");
            }
            if (g_lastReported.size() > 4096) g_lastReported.clear();   // never unbounded
        } catch (...) {
        }
    }

    void ResetAll() {
        g_lastReported.clear();
        std::scoped_lock lk(g_probeMx);
        g_firstSeen.clear();
    }

}
