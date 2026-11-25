#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/Allowance.h"
#include "core/MoveProbe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

// Win32 INI read -- the same hand-declared extern every INI-gated file in this project uses.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See core/MoveProbe.h. RECORDS: one per actor with a live ch.24 claim, created and dropped on the
// GAME thread (unique lock); the combat threads take the lock shared and touch atomics only. g_mx is a
// LEAF: nothing is called out while it is held.
//
// P2's reads, each from the disassembly of all three builds (1.6.1170 / 1.5.97 / 1.7.104):
//   the leaf's frame = [thread + 0x00] + dword [thread + 0x120]: +0x00 CombatPath*, +0x08 the area
//       (update 0x8AD2C0 / 0x816470 -> 0x8103E0 / 0x8C2830; act 0x8AC700 / 0x813C70 / 0x8C1C70)
//   CombatPath: +0x14 status, +0x18 the status its last Update ENTERED with, +0x1C speed type, +0x20 a
//       time, +0x24 the timeout compared with now - (+0x20), +0x28 / +0x2C last re-request and its period
//       (Update 0x8C37A0 / 0x82C540 / 0x8D8DA0, ctor 0x8C35A0 / 0x82C460 / 0x8D8BA0)
//   its request = vfunc 0x05, `mov rax,[rcx+38h]; ret` (0x8AB080). NOT called: the bytes are compared, then
//       +0x38 is read. Request: +0x58 goal, +0x8C pad, +0x82 bit 1 and +0xBA bit 0 = the area is not asked
//       (0x55B83C / 0x4FFEEC / 0x5636AC)
//   area: +0x2C active, +0x30 centre, +0x48 radius (channels/CombatApproach.cpp).
// SetFailed and Ascend (the leaf's status 5 exit, 0x85E0A0 / 0x85DD70) only write thread words: the frame is
// popped later by the runner, so it is still there when the leaf's own update returns. It is read only when
// the thread's frame offset is what it was before the call; anything else is counted and not read.
// ============================================================================

namespace apmf::moveprobe {

    namespace {

        using apmf::log::Hex;
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        constexpr std::size_t kLeafN =
            apmf::cbt::kLeaves.size() + apmf::cbt::kSearchLeaves.size() + apmf::cbt::kParamLeaves.size();

        constexpr std::uint8_t kMove   = 1;   // starts a path (the plan's table 1.3)
        constexpr std::uint8_t kAttack = 2;   // a weapon attack leaf

        std::atomic<bool>        g_on{ false };
        std::atomic<std::size_t> g_watching{ 0 };
        std::unordered_map<std::uintptr_t, std::uint32_t> g_idx;   // leaf vtable -> index; written once, Install
        std::array<const char*, kLeafN>  g_name{};
        std::array<std::uint8_t, kLeafN> g_kind{};
        std::uint32_t                    g_rtcaIdx = ~0u;

        // ---- P2 ----
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_origUpdate;   // written once, Install
        std::atomic<std::uint64_t> g_p2Updates{ 0 }, g_p2Fail{ 0 }, g_p2Anomaly{ 0 }, g_p2Unwatched{ 0 };

        struct Bucket { float toward = 0.0f, away = 0.0f; std::uint32_t n = 0; };
        enum Body : std::size_t { kFree = 0, kAttacking, kHitReaction, kAnimDriven, kBodyN };
        constexpr const char* kBodyName[kBodyN] = { "free", "ATTACK STATE", "HIT REACTION", "ANIMATION DRIVEN" };

        struct Rec {
            // GAME THREAD only.
            APMF_API::Handle claim = 0;
            std::uint64_t    startMs = 0, lastMs = 0;
            RE::NiPoint3     pos{};
            float            startDist = 0.0f, dist = 0.0f, minDist = 0.0f;
            bool             bodyAtLast = false;
            std::size_t      lastBody = kFree;
            std::array<Bucket, kBodyN> body{};
            std::array<Bucket, kLeafN> byLeaf{};          // by the last movement leaf entered
            Bucket           noLeaf{};
            // Combat threads.
            std::array<std::atomic<std::uint32_t>, kLeafN> acts{};
            std::atomic<std::uint32_t> lastMove{ ~0u };
            std::atomic<std::uint64_t> rtcaActMs{ 0 };
            std::atomic<std::uint32_t> attackLeaf{ ~0u };  // the running attack leaf (act seen, pop not yet)
            std::atomic<std::uint64_t> attackMs{ 0 };
            std::atomic<std::uintptr_t> aimVt{ 0 };        // the current aim controller's vtable, 0 = none
            std::atomic<std::uint64_t> aimMs{ 0 };         // when S1 last read it (0 = never)
            std::atomic<std::uint32_t> aimChanges{ 0 };
            std::atomic<std::uint32_t> p2Updates{ 0 }, p2Build{ 0 }, p2Follow{ 0 }, p2Lines{ 0 };
            std::atomic<std::uint64_t> p2LineMs{ 0 };
        };
        constexpr std::uint32_t kP2LinesPerClaim = 40;
        constexpr std::uint64_t kP2LineMs        = 250;

        std::shared_mutex                                     g_mx;
        std::unordered_map<RE::FormID, std::unique_ptr<Rec>> g_recs;

        template <class T>
        T At(const void* a_p, std::size_t a_off) {
            T v;
            std::memcpy(&v, static_cast<const std::byte*>(a_p) + a_off, sizeof(T));
            return v;
        }

        // The class of a game vtable: its RVA, and its RTTI name when it lies in the game image.
        std::string VtText(std::uintptr_t a_vt) {
            if (a_vt == 0) return "none";
            static const std::uintptr_t base = REL::Module::get().base();
            const auto rd = REL::Module::get().segment(REL::Segment::rdata);
            if (a_vt < rd.address() + sizeof(void*) || a_vt >= rd.address() + rd.size())
                return fmt::format("0x{:X} (not a game vtable)", a_vt);
            const char* name = nullptr;
            if (auto* col = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(a_vt - sizeof(void*)))
                if (auto* td = col->typeDescriptor.get()) name = td->mangled_name();
            return fmt::format("RVA 0x{:X} {}", a_vt - base, name ? name : "?");
        }

        RE::NiPointer<RE::Actor> ActorOf(void* a_thread) {
            auto* owner = reinterpret_cast<apmf::cbt::TreeControl*>(a_thread)->master_controller;
            if (!owner) return nullptr;
            auto* ctrl = At<void*>(owner, 0x20);
            return ctrl ? reinterpret_cast<apmf::cbt::ControllerMini*>(ctrl)->attackerHandle.get() : nullptr;
        }

        // ---- P2: the Return To Combat Area leaf's update (slot 0x04). OBSERVE-ONLY, chains always. ----
        void ReturnUpdateThunk(void* a_this, void* a_thread) {
            const auto it = g_origUpdate.find(*reinterpret_cast<std::uintptr_t*>(a_this));
            if (it == g_origUpdate.end()) return;   // foreign vtable: no original to recover
            const auto orig = reinterpret_cast<apmf::cbt::Update_t>(it->second);
            const bool look = a_thread && g_watching.load(std::memory_order_relaxed) != 0;
            const std::uint32_t frameBefore = look ? At<std::uint32_t>(a_thread, 0x120) : 0;

            orig(a_this, a_thread);

            g_p2Updates.fetch_add(1, std::memory_order_relaxed);
            if (!look) return;
            if (At<std::uint32_t>(a_thread, 0x120) != frameBefore) {
                g_p2Anomaly.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto* data = At<const std::byte*>(a_thread, 0x00);
            if (!data) return;
            const auto* path = At<const void*>(data + frameBefore, 0x00);
            const auto* area = At<const void*>(data + frameBefore, 0x08);
            if (!path) return;
            const auto status = At<std::int32_t>(path, 0x14);
            const auto actor  = ActorOf(a_thread);
            if (!actor) return;
            const RE::FormID fid = actor->GetFormID();
            const auto       now = apmf::clock::MonotonicMs();
            std::uint64_t    actMs = 0;
            std::uint32_t    updates = 0;
            bool             line = false;
            const auto       prev = At<std::int32_t>(path, 0x18);
            {
                std::shared_lock lk(g_mx);
                const auto rit = g_recs.find(fid);
                if (rit == g_recs.end()) {
                    if (status == 5) g_p2Unwatched.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                Rec& r  = *rit->second;
                updates = r.p2Updates.fetch_add(1, std::memory_order_relaxed) + 1;
                if (status != 5) return;
                (prev == 5 ? r.p2Build : r.p2Follow).fetch_add(1, std::memory_order_relaxed);
                actMs = r.rtcaActMs.load(std::memory_order_relaxed);
                const auto last = r.p2LineMs.load(std::memory_order_relaxed);
                if (now - last >= kP2LineMs && r.p2Lines.load(std::memory_order_relaxed) < kP2LinesPerClaim) {
                    r.p2LineMs.store(now, std::memory_order_relaxed);
                    r.p2Lines.fetch_add(1, std::memory_order_relaxed);
                    line = true;
                }
            }
            g_p2Fail.fetch_add(1, std::memory_order_relaxed);
            if (!line) return;

            std::string req = "request unread (its getter is not the engine's)";
            {
                const auto* vt     = At<const void* const*>(path, 0x00);
                const auto* getter = static_cast<const std::uint8_t*>(vt[5]);
                constexpr std::uint8_t kGetter[] = { 0x48, 0x8B, 0x41, 0x38, 0xC3 };
                if (getter && std::memcmp(getter, kGetter, sizeof(kGetter)) == 0) {
                    const auto* rq = At<const void*>(path, 0x38);
                    if (!rq) req = "no request";
                    else {
                        const auto  g82 = At<std::uint8_t>(rq, 0x82), gBA = At<std::uint8_t>(rq, 0xBA);
                        const float gx = At<float>(rq, 0x58), gy = At<float>(rq, 0x5C), gz = At<float>(rq, 0x60);
                        const float pad = At<float>(rq, 0x8C);
                        req = fmt::format("request +82=0x{:02X} +BA=0x{:02X} (area asked: {}), goal {:.0f},{:.0f},{:.0f} pad {:.0f}",
                                          g82, gBA, (g82 & 2) == 0 && (gBA & 1) == 0 ? "yes" : "NO", gx, gy, gz, pad);
                        if (area) {
                            const float dx = gx - At<float>(area, 0x30), dy = gy - At<float>(area, 0x34),
                                        dz = gz - At<float>(area, 0x38);
                            req += fmt::format(", goal to area centre {:.0f} of R {:.0f}",
                                               std::sqrt(dx * dx + dy * dy + dz * dz), At<float>(area, 0x48));
                        }
                    }
                }
            }
            const auto* st = actor->AsActorState();
            spdlog::info("[moveprobe P2] 0x{} '{}' RETURN PATH STATUS 5: previous status {} ({}), update {} of this claim's, "
                         "{} ms after the leaf's act; path +1C={} +20={:.3f} +24={:.3f} +28={:.3f} +2C={:.3f}; {}; area {}; "
                         "body: attack state {}, knock state {}, staggered {}, weapon {}.",
                         Hex(fid), actor->GetName() ? actor->GetName() : "?", prev,
                         prev == 5 ? "failed at BUILD / start" : "failed while FOLLOWING or at a re-request", updates,
                         actMs != 0 ? now - actMs : 0, At<std::uint32_t>(path, 0x1C), At<float>(path, 0x20),
                         At<float>(path, 0x24), At<float>(path, 0x28), At<float>(path, 0x2C), req,
                         area ? (At<std::uint8_t>(area, 0x2C) == 0 ? "switched OFF by this" : "still active") : "none",
                         static_cast<std::uint32_t>(st->GetAttackState()), static_cast<std::uint32_t>(st->GetKnockState()),
                         static_cast<std::uint32_t>(st->actorState2.staggered), st->IsWeaponDrawn() ? "drawn" : "sheathed");
        }

        std::size_t BodyOf(RE::Actor* a_actor) {   // GAME THREAD
            const auto* st = a_actor->AsActorState();
            if (st->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || st->actorState2.staggered) return kHitReaction;
            if (st->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) return kAttacking;
            return a_actor->IsAnimationDriven() ? kAnimDriven : kFree;
        }

        void Add(Bucket& a_b, float a_toward) {
            (a_toward >= 0.0f ? a_b.toward : a_b.away) += std::fabs(a_toward);
            ++a_b.n;
        }

        std::string Summary(RE::FormID a_id, const Rec& r, const char* a_why) {
            std::string out = fmt::format("[moveprobe P1] 0x{} claim h={} SUMMARY ({}): {:.1f} s, distance to the point {:.0f} -> "
                                          "{:.0f} (nearest {:.0f}); displacement toward / away from the point by body state:",
                                          Hex(a_id), r.claim, a_why, (r.lastMs - r.startMs) / 1000.0f, r.startDist, r.dist, r.minDist);
            for (std::size_t b = 0; b < kBodyN; ++b)
                out += fmt::format(" {} +{:.0f} / -{:.0f} ({} samples){}", kBodyName[b], r.body[b].toward, r.body[b].away,
                                   r.body[b].n, b + 1 < kBodyN ? "," : ";");
            out += " by the last movement leaf entered (body free only):";
            if (r.noLeaf.n) out += fmt::format(" none yet +{:.0f} / -{:.0f}", r.noLeaf.toward, r.noLeaf.away);
            for (std::size_t i = 0; i < kLeafN; ++i)
                if (r.byLeaf[i].n)
                    out += fmt::format(" {} +{:.0f} / -{:.0f} ({})", g_name[i], r.byLeaf[i].toward, r.byLeaf[i].away, r.byLeaf[i].n);
            out += "; leaf acts entered:";
            std::vector<std::pair<std::uint32_t, std::size_t>> a;
            for (std::size_t i = 0; i < kLeafN; ++i)
                if (const auto n = r.acts[i].load(std::memory_order_relaxed)) a.emplace_back(n, i);
            std::sort(a.rbegin(), a.rend());
            for (const auto& [n, i] : a) out += fmt::format(" {} {}", g_name[i], n);
            out += fmt::format("; [P2] return updates {}, status 5 at build {}, while following {}; [P3] aim holder changes {}, "
                               "last {}; since load: return updates {}, status 5 {}, on unwatched actors {}, frame anomalies {}.",
                               r.p2Updates.load(), r.p2Build.load(), r.p2Follow.load(), r.aimChanges.load(),
                               VtText(r.aimVt.load()), g_p2Updates.load(), g_p2Fail.load(), g_p2Unwatched.load(), g_p2Anomaly.load());
            return out;
        }

    }

    bool On() { return g_on.load(std::memory_order_relaxed); }
    bool Watching() { return g_watching.load(std::memory_order_relaxed) != 0; }

    void NoteAct(std::uintptr_t a_vt, RE::FormID a_actor) {
        if (a_actor == 0) return;
        const auto it = g_idx.find(a_vt);
        if (it == g_idx.end()) return;
        const auto       i   = it->second;
        const auto       now = apmf::clock::MonotonicMs();
        std::shared_lock lk(g_mx);
        const auto       rit = g_recs.find(a_actor);
        if (rit == g_recs.end()) return;
        Rec& r = *rit->second;
        r.acts[i].fetch_add(1, std::memory_order_relaxed);
        if (g_kind[i] & kMove) r.lastMove.store(i, std::memory_order_relaxed);
        if (i == g_rtcaIdx) r.rtcaActMs.store(now, std::memory_order_relaxed);
        if (g_kind[i] & kAttack) {
            r.attackMs.store(now, std::memory_order_relaxed);
            r.attackLeaf.store(i, std::memory_order_relaxed);
        }
    }

    void NotePop(std::uintptr_t a_vt, RE::FormID a_actor) {
        if (a_actor == 0) return;
        const auto it = g_idx.find(a_vt);
        if (it == g_idx.end() || (g_kind[it->second] & kAttack) == 0) return;
        std::shared_lock lk(g_mx);
        if (const auto rit = g_recs.find(a_actor); rit != g_recs.end()) {
            auto want = it->second;
            rit->second->attackLeaf.compare_exchange_strong(want, ~0u, std::memory_order_relaxed);
        }
    }

    void NoteAim(RE::FormID a_actor, RE::CombatController* a_cc) {
        // S1 runs inside this controller's own UpdateAreas, on the thread that swaps currentAimController
        // (AE 0x55A360 / SE 0x4FED51 / 1.7.104 0x5621D0), so the pointer cannot be replaced under this read.
        const auto* aim = reinterpret_cast<const std::uintptr_t*>(a_cc->GetRuntimeData().currentAimController);
        const std::uintptr_t vt = aim ? *aim : 0;
        std::shared_lock lk(g_mx);
        const auto rit = g_recs.find(a_actor);
        if (rit == g_recs.end()) return;
        Rec& r = *rit->second;
        if (r.aimVt.exchange(vt, std::memory_order_relaxed) != vt) r.aimChanges.fetch_add(1, std::memory_order_relaxed);
        r.aimMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
    }

    void Sample(RE::FormID a_id, RE::Actor* a_actor, APMF_API::Handle a_claim, const RE::NiPoint3& a_point) {
        if (!On()) return;
        const auto         now  = apmf::clock::MonotonicMs();
        const RE::NiPoint3 pos  = a_actor->GetPosition();
        const float        dist = pos.GetDistance(a_point);
        const std::size_t  body = BodyOf(a_actor);   // an engine call (the graph read): before the lock
        std::string        line;
        {
            std::unique_lock lk(g_mx);
            auto& slot = g_recs[a_id];
            if (slot && slot->claim != a_claim) {
                line = Summary(a_id, *slot, "another claim took over");
                slot.reset();
            }
            if (!slot) {
                slot = std::make_unique<Rec>();
                Rec& r = *slot;
                r.claim = a_claim; r.startMs = r.lastMs = now; r.pos = pos;
                r.startDist = r.dist = r.minDist = dist;
                r.lastBody = body;
                g_watching.store(g_recs.size(), std::memory_order_relaxed);
            } else {
                Rec&        r      = *slot;
                const float moved  = pos.GetDistance(r.pos);
                const float toward = r.pos.GetDistance(a_point) - dist;   // against the point as it is NOW (a moving X adds nothing)
                if (now - r.lastMs <= 500 && moved <= 400.0f && std::isfinite(toward)) {   // no gap (a pause, a load), no teleport
                    // Either end of the sample in a state puts it there: a lunge that ended inside the sample is the lunge's.
                    const std::size_t b = (std::max)(body, r.lastBody) == kFree ? kFree : (body != kFree ? body : r.lastBody);
                    Add(r.body[b], toward);
                    if (b == kFree) {
                        const auto lm = r.lastMove.load(std::memory_order_relaxed);
                        Add(lm < kLeafN ? r.byLeaf[lm] : r.noLeaf, toward);
                    }
                }
                r.pos = pos; r.lastMs = now; r.dist = dist; r.lastBody = body;
                r.minDist = (std::min)(r.minDist, dist);
            }
        }
        if (!line.empty()) spdlog::info("{}", line);
    }

    void Finish(RE::FormID a_id, const char* a_why) {
        if (!On()) return;
        std::string line;
        {
            std::unique_lock lk(g_mx);
            const auto it = g_recs.find(a_id);
            if (it == g_recs.end()) return;
            line = Summary(a_id, *it->second, a_why);
            g_recs.erase(it);
            g_watching.store(g_recs.size(), std::memory_order_relaxed);
        }
        spdlog::info("{}", line);
    }

    void Reset(const char* a_why) {
        if (!On()) return;
        std::size_t n = 0;
        {
            std::unique_lock lk(g_mx);
            n = g_recs.size();
            g_recs.clear();
            g_watching.store(0, std::memory_order_relaxed);
        }
        if (n != 0) spdlog::info("[moveprobe] {} record(s) dropped without a summary ({}).", n, a_why);
    }

    std::string AimText(RE::FormID a_id) {
        if (!On()) return {};
        std::uintptr_t vt = 0;
        std::uint64_t  aimMs = 0, atkMs = 0;
        std::uint32_t  leaf = ~0u;
        {
            std::shared_lock lk(g_mx);
            const auto it = g_recs.find(a_id);
            if (it == g_recs.end()) return {};
            vt    = it->second->aimVt.load(std::memory_order_relaxed);
            aimMs = it->second->aimMs.load(std::memory_order_relaxed);
            leaf  = it->second->attackLeaf.load(std::memory_order_relaxed);
            atkMs = it->second->attackMs.load(std::memory_order_relaxed);
        }
        const auto now = apmf::clock::MonotonicMs();
        return fmt::format(" [moveprobe P3] aim holder {}; attack leaf {}.",
                           aimMs == 0 ? std::string{ "not read yet" } : fmt::format("{} ({} ms ago)", VtText(vt), now - aimMs),
                           leaf < kLeafN ? fmt::format("RUNNING {} for {} ms", g_name[leaf], now - atkMs) : std::string{ "not running" });
    }

    void Install() {
        if (GetPrivateProfileIntA("Probe", "bMoveProbe", 0, kIni) == 0) {
            spdlog::info("[moveprobe] OFF ([Probe] bMoveProbe=0): nothing installed.");
            return;
        }
        if (REL::Module::IsVR() || !allowance::RuntimeSupported()) {
            spdlog::error("[moveprobe] REFUSED: runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104.",
                          REL::Module::get().version().string("."));
            return;
        }
        std::uint32_t k = 0;
        auto add = [&](const char* a_name, const REL::VariantID& a_vt) {
            g_name[k] = a_name;
            g_idx[REL::Relocation<std::uintptr_t>{ a_vt }.address()] = k++;
        };
        for (const auto& l : apmf::cbt::kLeaves) add(l.name, l.vtbl);
        for (const auto& l : apmf::cbt::kSearchLeaves) add(l.name, l.vtbl);
        for (const auto& l : apmf::cbt::kParamLeaves) add(l.name, l.vtbl);
        // The leaves that start a path (plan table 1.3 rows 1 to 12, 15, 16, 23) and the weapon attack leaves.
        static constexpr std::string_view kMoves[] = {
            "Advance", "Reposition", "Backoff", "Circle", "CircleDistant", "Fallback", "FallbackToRanged", "Surround", "Strafe",
            "MaintainOptimalRange", "FindAttackLocation", "FindLateralAttackLocation", "FindAllyAttackLocation", "FindCover",
            "AttackFromCover", "WaitBehindCover", "Chase", "Flank", "FlankDistant", "Stalk", "Search", "SearchCenter",
            "SearchLocation", "SearchWander", "SearchInvestigateDoor", "Flee", "FleeToAlly", "FleeToCover", "FleeThroughDoor",
            "Hide", "ReturnToCombatArea", "ExitWater", "DodgeThreat", "FindWeapon", "AcquireItem", "FollowPath", "PursueTarget" };
        static constexpr std::string_view kAttacks[] = { "Attack", "AttackLow", "Bash", "SpecialAttack", "RangedAttack",
                                                         "AttackFromCover", "BlockAttack" };
        std::uint32_t moves = 0, attacks = 0;
        for (std::uint32_t i = 0; i < k; ++i) {
            std::string_view n = g_name[i];
            if (constexpr std::string_view pre = "CombatBehavior"; n.starts_with(pre)) n.remove_prefix(pre.size());
            n = n.substr(0, n.find('<'));   // the parameterised nodes: Flee<float>, AcquireItem<AcquireWeapon>, FollowPath<...>
            if (std::ranges::find(kMoves, n) != std::end(kMoves)) { g_kind[i] |= kMove; ++moves; }
            if (std::ranges::find(kAttacks, n) != std::end(kAttacks)) { g_kind[i] |= kAttack; ++attacks; }
            if (n == "ReturnToCombatArea") g_rtcaIdx = i;
        }
        // P2: one chaining vfunc seat, RTTI-checked and refused when the leaf's vtable did not verify.
        bool p2 = false;
        if (g_rtcaIdx < apmf::cbt::kLeaves.size()) {
            const std::array<REL::VariantID, 1> rtca{ apmf::cbt::kLeaves[g_rtcaIdx].vtbl };
            REL::Relocation<std::uintptr_t> vt{ rtca[0] };
            REL::Relocation<void*>          td{ apmf::cbt::RTTI_CombatBehaviorTreeNode };
            if (allowance::SeatVerified(vt.address(), "MoveProbe.ReturnToCombatArea.Update(0x04)"))
                p2 = allowance::InstallOnVtables(rtca, 0x04, &ReturnUpdateThunk, td.get(), "moveprobe-p2", g_origUpdate) == 1;
        }
        g_on.store(true, std::memory_order_release);
        spdlog::info("[moveprobe] ON ([Probe] bMoveProbe=1, passive): {} leaves indexed ({} movers, {} weapon attacks); P1 one "
                     "summary per ch.24 claim, P3 on the ch.24 gait line.", k, moves, attacks);
        if (p2) spdlog::info("[moveprobe] P2 seat on the Return To Combat Area leaf update (0x04), observe-only, chaining.");
        else spdlog::error("[moveprobe] P2 REFUSED: the Return To Combat Area leaf update seat did not verify or install.");
    }

}
