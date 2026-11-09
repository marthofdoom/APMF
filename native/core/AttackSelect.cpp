#include "PCH.h"
#include "core/AttackSelect.h"
#include "core/Allowance.h"          // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"         // TryGetOwningClaimBasis: the winning ch.18 claim (one RCU read)
#include "core/FacetAttribution.h"   // Decide: the claim vs the Engine source on the CombatAction facet
#include "core/Log.h"
#include "core/Sources.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Win32, declared by hand (the PCH does not pull in <Windows.h>; core/Sources.cpp's one-line import).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// ch.18 ATTACK SELECTION -- THE WEIGHT SEAT (ABI v34; Docs/INVARIANTS.md #17a, amended 2026-11-09).
//
// THE ENGINE PATH (read on the unpacked 1.6.1170 / 1.5.97 executables and the plaintext 1.7.104 one;
// agentlog apmf-ch18-attack-select.md). The combat behaviour tree's Attack attempt (AE 49178 0x8A4080,
// call at 0x8A41C8) and Bash attempt (call at AE 0x86B5E4) both call StartAttack(ctx), the only two
// callers on every build. StartAttack (AE 0x8A28B0 id 49170 / SE 0x80C020 id 48139 / 1.7.104 0x8B7D60,
// whose id is not in the fork's MIT table, so it is placed by raw RVA; its 1.7.104 body is
// instruction-identical to 1.6.1170's, 349 instructions with call and RIP targets normalised):
//   * keeps the context in a callee-saved register for the whole function: r13 on 1.6.1170 / 1.7.104
//     (`mov r13,rcx` at +0x40, its only write), r12 on 1.5.97 (`mov r12,rcx` at +0x3D, its only write);
//   * refreshes the context's entries (call 0x8A3D30 at +0x156: writes each entry's +0x24);
//   * reads the actor's stamina once, `ActorValueOwner(actor+0xB8 AE / +0xB0 SE)::GetActorValue(0x1A)`
//     (vtable slot 1, AE +0x1AE..+0x1C7, SE +0x1D9..+0x1F1), keeping `stamina > 0` in sil;
//   * walks the context's pool: array at ctx+0x18, 0x30-byte entries (entry+0 = BGSAttackData*), count
//     at ctx+0x28. Per entry, in this order: SKIP if 0 >= entry+0x24 (AE/SE +0x24E/+0x26F); SKIP unless
//     (now - entry+0x28) > entry+0x2C, `now` a float global read RIP-relative (AE +0x258 / SE +0x279);
//     when stamina <= 0, SKIP if the entry's stamina cost (0x4183E0) > 0; then
//         weight = W(actor, target, BGSAttackData*)     <-- THE SEAT: AE/1.7.104 +0x28D, SE +0x2AE
//     and SKIP if weight <= 0, else push (entry, weight) and add it to the total;
//   * with no candidate, fails (AE 0x8A2D9D / SE 0x80C642: the leaf fails); otherwise draws at weighted
//     random, then the angle test, then CombatAnimation::Execute (+0x435). SCAR and Valhalla replace
//     the Execute call site for annotated actors (S-D5, NOT this round: their pick bypasses this roll).
// W (AE 0x8DCD30 id 50675 / SE 0x845B30 id 49748 / 1.7.104 0x8F2830, raw) is float(Actor*, Actor*,
// BGSAttackData*): attackChance (+0x1C) times a combat-style multiplier that applies only while the
// target staggers or, for a power attack, blocks. Identical body on all three (84 instructions). It has
// exactly two E8 callers per build (this site and one elsewhere: AE 0x48BA38 / SE 0x430D8A / 1.7.104
// 0x492F58, not touched).
//
// THE SEAT. One write_call<5> at the site. The call goes to a 17-byte stub in our own trampoline,
// `mov r9, <ctx register>; jmp [rip+0] -> Thunk`, so Thunk receives the context as a 4th argument
// (r9 is volatile and unused by the 3-argument call; the stub pushes nothing, so Thunk sees the
// engine's own frame and 0x20 shadow space). Before writing, Install re-reads, per runtime: the E8
// and its target (W), the 10 argument-setup bytes ending at the E8, the pool walk bytes that name the
// context register, the per-entry availability and cooldown test bytes and the stamina read. Any
// difference (another patcher, a SCAR-class collision, a wrong build) REFUSES the seat.
//
// THE VERDICT (Thunk). The engine's weight first. A candidate the engine already rejects (<= 0) is
// returned untouched. For an actor whose WINNING kIntent_AttackSelection claim has a declaration:
//   1. the actor's window is sampled: the actor's current attack data (process -> high -> attackData,
//      the engine's own read after a start) changed since the last sample = one started attack; a
//      Power or Normal start is pushed into the window (exact repeats of the same entry are not seen,
//      2 of 44 in the 2026-10-30 field log: a documented approximation of the share);
//   2. the active deny = the declared `denied` | Power while the share is above powerShareMax |
//      Normal while it is below powerShareMin (both only once the window is full);
//   3. a class NOT governed, or not in the active deny, keeps the engine's weight;
//   4. marth 2026-11-09, "a regular attack must fire when stamina isnt allowing power": the SHARE
//      FLOOR's Normal deny applies only if some Power entry of THIS roll's pool passes the engine's own
//      per-entry tests above (entry+0x24 > 0, the cooldown compare, stamina > 0) and the engine's own
//      W > 0, and is not itself denied. With stamina <= 0 no Power entry counts as able (the engine
//      would admit only one whose stamina cost is 0; Harbinger does not call the cost function and
//      errs toward the Normal attack the ruling asks for). Otherwise Normal keeps the engine's weight;
//   5. THE FLOOR RULE: the deny is applied only if at least TWO entries of the context's pool stay
//      undenied (never an empty pool, never narrowed to one move); else the engine's weights stand for
//      the roll (logged at warn, rate-limited);
//   6. RANKING: the claim must outrank the Engine source on the CombatAction facet (facetattr::Decide);
//   7. observe-only ([AttackSelect] bObserveOnly or kAtkMix_ObserveOnly): counted, nothing denied;
//   8. otherwise the call answers 0.0f: the engine's draw never picks the entry.
// Nothing here throws into the engine: any exception keeps the engine's weight.
// ============================================================================

namespace apmf::attackselect {

    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        constexpr const char* kRowStart  = "AttackSelect.StartAttack";
        constexpr const char* kRowWeight = "AttackSelect.AttackWeight";
        constexpr const char* kRowSite   = "AttackSelect.Site.Weight";

        // ---- per-runtime facts (rule 11; every byte read off that runtime's executable) ----------
        struct Window {
            std::uintptr_t      off;
            const std::uint8_t* bytes;
            std::size_t         len;
        };
        struct RtSpec {
            const char*    name;
            std::uint64_t  startId;      // 0 = raw RVA (1.7.104)
            std::uint64_t  weightId;
            std::uintptr_t startRva;     // raw RVA when the id is 0
            std::uintptr_t weightRva;
            std::uintptr_t siteOff;      // the E8
            std::uint8_t   movR9[3];     // mov r9, <ctx register>
            Window         args;         // argument setup ending with the E8 opcode
            Window         loop;         // entry = ctx->array + i*0x30 (names the ctx register)
            Window         count;        // cmp edi, [ctx+0x28]
            Window         avail;        // comiss xmm7, [rbx+0x24]
            Window         stamina;      // lea avOwner; GetActorValue(0x1A); seta sil
            std::uintptr_t cdOff;        // movss xmm0,[rip+disp]; subss xmm0,[rbx+0x28]; comiss xmm0,[rbx+0x2C]
        };
        constexpr std::uint8_t kArgsAE[]  = { 0x4C, 0x8B, 0x03, 0x49, 0x8B, 0xD6, 0x49, 0x8B, 0xCF, 0xE8 };
        constexpr std::uint8_t kLoopAE[]  = { 0x48, 0x8D, 0x1C, 0x40, 0x48, 0xC1, 0xE3, 0x04, 0x49, 0x03, 0x5D, 0x18 };
        constexpr std::uint8_t kCountAE[] = { 0x41, 0x3B, 0x7D, 0x28 };
        constexpr std::uint8_t kStamAE[]  = { 0x4D, 0x8D, 0xA7, 0xB8, 0x00, 0x00, 0x00, 0x49, 0x8B, 0x04, 0x24, 0xBA, 0x1A,
                                              0x00, 0x00, 0x00, 0x49, 0x8B, 0xCC, 0xFF, 0x50, 0x08, 0x0F, 0x2F, 0xC7, 0x40,
                                              0x0F, 0x97, 0xC6 };
        constexpr std::uint8_t kArgsSE[]  = { 0x4C, 0x8B, 0x03, 0x49, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8 };
        constexpr std::uint8_t kLoopSE[]  = { 0x48, 0x8D, 0x1C, 0x40, 0x48, 0xC1, 0xE3, 0x04, 0x49, 0x03, 0x5C, 0x24, 0x18 };
        constexpr std::uint8_t kCountSE[] = { 0x41, 0x3B, 0x7C, 0x24, 0x28 };
        constexpr std::uint8_t kStamSE[]  = { 0x4D, 0x8D, 0xB5, 0xB0, 0x00, 0x00, 0x00, 0x49, 0x8B, 0x06, 0xBA, 0x1A, 0x00,
                                              0x00, 0x00, 0x49, 0x8B, 0xCE, 0xFF, 0x50, 0x08, 0x0F, 0x2F, 0xC7, 0x40, 0x0F,
                                              0x97, 0xC6 };
        constexpr std::uint8_t kAvail[]   = { 0x0F, 0x2F, 0x7B, 0x24 };
        constexpr std::uint8_t kCdHead[]  = { 0xF3, 0x0F, 0x10, 0x05 };
        constexpr std::uint8_t kCdTail[]  = { 0xF3, 0x0F, 0x5C, 0x43, 0x28, 0x0F, 0x2F, 0x43, 0x2C };

        constexpr RtSpec kAE{ "1.6.1170", 49170, 50675, 0, 0, 0x28D, { 0x4D, 0x8B, 0xCD },
                              { 0x284, kArgsAE, sizeof(kArgsAE) }, { 0x242, kLoopAE, sizeof(kLoopAE) },
                              { 0x2F2, kCountAE, sizeof(kCountAE) }, { 0x24E, kAvail, sizeof(kAvail) },
                              { 0x1AE, kStamAE, sizeof(kStamAE) }, 0x258 };
        constexpr RtSpec kSE{ "1.5.97", 48139, 49748, 0, 0, 0x2AE, { 0x4D, 0x8B, 0xCC },
                              { 0x2A5, kArgsSE, sizeof(kArgsSE) }, { 0x262, kLoopSE, sizeof(kLoopSE) },
                              { 0x315, kCountSE, sizeof(kCountSE) }, { 0x26F, kAvail, sizeof(kAvail) },
                              { 0x1D9, kStamSE, sizeof(kStamSE) }, 0x279 };
        // 1.7.104: neither id is in the fork's MIT id table (revision 4), so the two functions are placed by
        // raw RVA (rows "rva:0x8A28B0" / "rva:0x8DCD30" in the 1.7.104 table, generator-proven). Same offsets
        // and bytes as 1.6.1170 (instruction-identical body).
        constexpr RtSpec k17{ "1.7.104", 0, 0, 0x8B7D60, 0x8F2830, 0x28D, { 0x4D, 0x8B, 0xCD },
                              { 0x284, kArgsAE, sizeof(kArgsAE) }, { 0x242, kLoopAE, sizeof(kLoopAE) },
                              { 0x2F2, kCountAE, sizeof(kCountAE) }, { 0x24E, kAvail, sizeof(kAvail) },
                              { 0x1AE, kStamAE, sizeof(kStamAE) }, 0x258 };

        // The roll's context (identical on all three).
        constexpr std::uintptr_t kCtxArray   = 0x18;
        constexpr std::uintptr_t kCtxCount   = 0x28;
        constexpr std::uintptr_t kEntrySize  = 0x30;
        constexpr std::uintptr_t kEntryAvail = 0x24;
        constexpr std::uintptr_t kEntryLast  = 0x28;
        constexpr std::uintptr_t kEntryCd    = 0x2C;
        constexpr std::uint32_t  kPoolMax    = 256;   // sanity bound (the field shows 20)
        constexpr std::uint32_t  kMinUndenied = 2;    // the floor rule: never empty, never one move

        // BGSAttackData: flags at +0x18 (data) + 0x10 = +0x28 (W itself reads [r8+0x28]).
        static_assert(offsetof(RE::BGSAttackData, data) == 0x18, "BGSAttackData::data moved");
        static_assert(offsetof(RE::AttackData, flags) == 0x10, "AttackData::flags moved");
        constexpr std::uint32_t kFlagBash  = 1u << 1;
        constexpr std::uint32_t kFlagPower = 1u << 2;
        constexpr std::uint32_t kFlagLeft  = 1u << 3;   // CommonLib's kChargeAttack == BGSAttackData::IsLeftAttack

        using Weight_t = float (*)(RE::Actor*, RE::Actor*, RE::BGSAttackData*);

        std::atomic<Weight_t>      g_orig{ nullptr };
        std::atomic<bool>          g_installed{ false };
        std::atomic<const char*>   g_whyNot{ "not installed yet (before kDataLoaded)" };
        std::uintptr_t             g_site       = 0;   // Install only, then read-only
        std::uintptr_t             g_ourTarget  = 0;   //   the E8 target after our write
        const float*               g_now        = nullptr;
        bool                       g_iniObserve = false;
        std::atomic<bool>          g_siteLost{ false };

        // ---- totals (the heartbeat) ----
        std::atomic<std::uint64_t> t_governed{ 0 }, t_zeroed{ 0 }, t_floor{ 0 }, t_powerUnable{ 0 }, t_rank{ 0 },
            t_would{ 0 }, t_poolMiss{ 0 }, t_errors{ 0 };
        std::atomic<bool>          g_everClaimed{ false };
        std::uint64_t              g_lastBeatMs = 0;   // main thread only

        // ---- the declarations (any-thread writers under g_wmx; readers: the published snapshot) ----
        struct Decl {
            std::uint32_t governed = 0;
            std::uint32_t denied   = 0;
            float         maxShare = -1.0f;
            float         minShare = -1.0f;
            std::uint32_t window   = 8;
            std::uint32_t flags    = 0;
        };
        using DeclMap = std::unordered_map<APMF_API::Handle, Decl>;
        std::mutex                                   g_wmx;
        std::unordered_set<APMF_API::Handle>         g_claims;      // NoteClaim'd, not yet forgotten
        std::unordered_map<APMF_API::Handle, std::uint32_t> g_badLogged;   // last logged problem mask per handle
        DeclMap                                      g_declsW;      // writer copy
        std::atomic<std::shared_ptr<const DeclMap>>  g_decls{ std::make_shared<const DeclMap>() };
        std::atomic<bool>                            g_anyDecl{ false };

        void PublishLocked() {
            g_decls.store(std::make_shared<const DeclMap>(g_declsW), std::memory_order_release);
            g_anyDecl.store(!g_declsW.empty(), std::memory_order_release);
        }

        // ---- per-actor window + counters (leaf mutex; nothing is called while held) ----
        constexpr std::size_t kRing      = 32;
        constexpr std::size_t kActorsMax = 512;
        struct ActorState {
            std::uintptr_t   lastAtk = 0;
            std::array<std::uint8_t, kRing> ring{};   // 1 = Power, 0 = Normal, newest at (head-1)
            std::uint32_t    head = 0, fill = 0;
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            std::uint32_t    activeDeny = 0, loggedActive = 0xFFFFFFFFu;
            bool             governedSeen = false;
            std::uint32_t    zeroed = 0, floorStood = 0, rankStood = 0, wouldDeny = 0;
        };
        std::mutex                                 g_amx;
        std::unordered_map<RE::FormID, ActorState> g_actors;

        void Sat(std::uint32_t& c) { if (c != 0xFFFFFFFFu) ++c; }

        std::uint32_t FlagsOf(const RE::BGSAttackData* a) {
            return a ? a->data.flags.underlying() : 0;
        }
        std::uint32_t ClassOf(std::uint32_t f) {
            const bool b = (f & kFlagBash) != 0, p = (f & kFlagPower) != 0, l = (f & kFlagLeft) != 0;
            if (b) return p ? APMF_API::kAtkClass_BashPower : APMF_API::kAtkClass_Bash;
            if (l) return APMF_API::kAtkClass_Left;
            return p ? APMF_API::kAtkClass_Power : APMF_API::kAtkClass_Normal;
        }
        const char* ClassName(std::uint32_t c) {
            switch (c) {
            case APMF_API::kAtkClass_Normal:    return "Normal";
            case APMF_API::kAtkClass_Power:     return "Power";
            case APMF_API::kAtkClass_Bash:      return "Bash";
            case APMF_API::kAtkClass_BashPower: return "BashPower";
            case APMF_API::kAtkClass_Left:      return "Left";
            default:                            return "?";
            }
        }
        std::string ClassList(std::uint32_t m) {
            std::string s;
            for (std::uint32_t b = 1; b <= APMF_API::kAtkClass_Left; b <<= 1)
                if (m & b) { if (!s.empty()) s += '+'; s += ClassName(b); }
            return s.empty() ? std::string("none") : s;
        }

        // The window's share (Power / fill) once full, else -1.
        float Share(const ActorState& s, std::uint32_t window, std::uint32_t* outP = nullptr, std::uint32_t* outN = nullptr) {
            const std::uint32_t n = s.fill < window ? s.fill : window;
            std::uint32_t p = 0;
            for (std::uint32_t i = 0; i < n; ++i) p += s.ring[(s.head + kRing - 1 - i) % kRing];
            if (outP) *outP = p;
            if (outN) *outN = n - p;
            return (n >= window && n > 0) ? static_cast<float>(p) / static_cast<float>(n) : -1.0f;
        }

        const RE::BGSAttackData* CurrentAttack(RE::Actor* a) {
            auto* proc = a->GetActorRuntimeData().currentProcess;
            if (!proc || !proc->high) return nullptr;
            return proc->high->attackData.get();
        }

        // Step 4: some Power entry of this pool passes the engine's own per-entry tests and W > 0, and is not
        // itself in `a_deny`. Reads the engine's data in place on the thread that is running the roll.
        bool AnyPowerAble(RE::Actor* a_self, RE::Actor* a_target, const std::uint8_t* a_ctx, std::uint32_t a_deny,
                          Weight_t a_w) {
            if ((a_deny & APMF_API::kAtkClass_Power) != 0) return false;
            auto* avo = a_self->AsActorValueOwner();   // actor+0xB8 (1.6.1170 / 1.7.104) / +0xB0 (1.5.97), as the engine
            if (!avo || !(avo->GetActorValue(RE::ActorValue::kStamina) > 0.0f))
                return false;   // the engine's `seta sil`: with stamina <= 0 only a zero-cost entry passes
            const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(a_ctx + kCtxArray);
            const auto  n   = *reinterpret_cast<const std::uint32_t*>(a_ctx + kCtxCount);
            if (!arr || n > kPoolMax || !g_now) return false;
            const float now = *g_now;
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto* e   = arr + i * kEntrySize;
                auto*       atk = *reinterpret_cast<RE::BGSAttackData* const*>(e);
                if (!atk || ClassOf(FlagsOf(atk)) != APMF_API::kAtkClass_Power) continue;
                const float avail = *reinterpret_cast<const float*>(e + kEntryAvail);
                if (0.0f >= avail) continue;                                   // comiss xmm7,[rbx+0x24]; jae skip
                const float since = now - *reinterpret_cast<const float*>(e + kEntryLast);
                if (!(since > *reinterpret_cast<const float*>(e + kEntryCd))) continue;   // ...; jbe skip
                if (a_w(a_self, a_target, atk) > 0.0f) return true;            // the engine's own weight
            }
            return false;
        }

        // Step 5: (pool size, undenied entries, whether the candidate is in it).
        struct PoolCount { std::uint32_t total = 0, undenied = 0; bool found = false; bool ok = false; };
        PoolCount CountPool(const std::uint8_t* a_ctx, const RE::BGSAttackData* a_cand, std::uint32_t a_deny) {
            PoolCount r;
            if (!a_ctx) return r;
            const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(a_ctx + kCtxArray);
            const auto  n   = *reinterpret_cast<const std::uint32_t*>(a_ctx + kCtxCount);
            if (!arr || n == 0 || n > kPoolMax) return r;
            r.ok = true;
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto* atk = *reinterpret_cast<const RE::BGSAttackData* const*>(arr + i * kEntrySize);
                if (!atk) continue;
                ++r.total;
                if (atk == a_cand) r.found = true;
                if ((ClassOf(FlagsOf(atk)) & a_deny) == 0) ++r.undenied;
            }
            return r;
        }

        const char* NameOf(RE::Actor* a) {
            const char* n = a ? a->GetName() : nullptr;
            return n ? n : "";
        }

        bool LineDue(RE::FormID a_fid, std::uint32_t a_key) {
            return apmf::facetattr::LineDue(apmf::sources::Facet::kCombatAction, a_fid, 0x18000000u | a_key);
        }

        float Decide(RE::Actor* a_self, RE::Actor* a_target, RE::BGSAttackData* a_atk, const std::uint8_t* a_ctx,
                     float a_w, Weight_t a_orig) {
            const RE::FormID fid = a_self->GetFormID();
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(fid, APMF_API::kIntent_AttackSelection, p, basis, &h))
                return a_w;
            const auto decls = g_decls.load(std::memory_order_acquire);
            const auto it    = decls->find(h);
            if (it == decls->end()) return a_w;   // claimed, nothing declared: declare -> enforce
            const Decl d = it->second;

            // 1 + 2: sample the window, compute the active deny.
            const auto* cur = CurrentAttack(a_self);
            std::uint32_t active = d.denied, floorBit = 0;
            float         share  = -1.0f;
            bool          logActive = false;
            {
                std::scoped_lock lk(g_amx);
                auto st = g_actors.find(fid);
                if (st == g_actors.end()) {
                    if (g_actors.size() >= kActorsMax) g_actors.clear();
                    st = g_actors.emplace(fid, ActorState{}).first;
                }
                auto& s = st->second;
                const auto curv = reinterpret_cast<std::uintptr_t>(cur);
                if (curv != s.lastAtk) {
                    s.lastAtk = curv;
                    const auto c = cur ? ClassOf(FlagsOf(cur)) : 0u;
                    if (c == APMF_API::kAtkClass_Power || c == APMF_API::kAtkClass_Normal) {
                        s.ring[s.head] = c == APMF_API::kAtkClass_Power ? 1 : 0;
                        s.head         = (s.head + 1) % kRing;
                        if (s.fill < kRing) ++s.fill;
                    }
                }
                share = Share(s, d.window);
                if (share >= 0.0f) {
                    if (d.maxShare >= 0.0f && share > d.maxShare) active |= APMF_API::kAtkClass_Power;
                    if (d.minShare >= 0.0f && share < d.minShare && (d.denied & APMF_API::kAtkClass_Normal) == 0)
                        floorBit = APMF_API::kAtkClass_Normal;
                }
                active |= floorBit;
                s.handle       = h;
                s.activeDeny   = active;
                s.governedSeen = true;
                if (active != s.loggedActive) { s.loggedActive = active; logActive = true; }
            }
            t_governed.fetch_add(1, std::memory_order_relaxed);
            if (logActive && LineDue(fid, 1)) {
                spdlog::info("[ch.18] 0x{} '{}' claim h={}: active deny now {} (declared {}; share {:.2f} over window {}, "
                             "ceiling {:.2f}, floor {:.2f}). Logged at most once per 5 s per actor.",
                             apmf::log::Hex(fid), NameOf(a_self), h, ClassList(active), ClassList(d.denied), share,
                             d.window, d.maxShare, d.minShare);
            }

            // 3.
            const std::uint32_t cls = ClassOf(FlagsOf(a_atk));
            if ((d.governed & cls) == 0 || (active & cls) == 0) return a_w;

            // 4. The share floor's Normal deny needs a Power attack that can actually start in this roll.
            if (cls == APMF_API::kAtkClass_Normal && floorBit != 0 &&
                !AnyPowerAble(a_self, a_target, a_ctx, active, a_orig)) {
                t_powerUnable.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(fid, 2))
                    spdlog::info("[ch.18] 0x{} '{}' claim h={}: power share {:.2f} is under the floor {:.2f}, but no "
                                 "Power attack of this roll can start (stamina, cooldown or reach, judged by the engine's "
                                 "own tests): the Normal attack keeps its weight (marth 2026-11-09). Logged at most once "
                                 "per 5 s per actor.",
                                 apmf::log::Hex(fid), NameOf(a_self), h, share, d.minShare);
                active &= ~floorBit;
                if ((active & cls) == 0) return a_w;
            }

            // 5. The floor rule.
            const PoolCount pc = CountPool(a_ctx, a_atk, active);
            if (!pc.ok || !pc.found) {
                t_poolMiss.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(fid, 3))
                    spdlog::warn("[ch.18] 0x{} '{}': the candidate is not in the roll's context pool ({}): NOT denied, "
                                 "the engine's weight stands. The context register assumption failed at runtime; "
                                 "report this line. Logged at most once per 5 s per actor.",
                                 apmf::log::Hex(fid), NameOf(a_self), pc.ok ? "not found" : "pool unreadable");
                return a_w;
            }
            if (pc.undenied < kMinUndenied) {
                {
                    std::scoped_lock lk(g_amx);
                    if (auto st = g_actors.find(fid); st != g_actors.end()) Sat(st->second.floorStood);
                }
                t_floor.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(fid, 4))
                    spdlog::warn("[ch.18] 0x{} '{}' claim h={}: denying {} would leave {} of the context's {} attack(s) "
                                 "({}): the FLOOR RULE keeps the engine's weights for this roll. Logged at most once per "
                                 "5 s per actor.",
                                 apmf::log::Hex(fid), NameOf(a_self), h, ClassList(active), pc.undenied, pc.total,
                                 pc.undenied == 0 ? "an empty pool" : "a single move");
                return a_w;
            }

            // 6. Ranked against the Engine source on the CombatAction facet.
            const auto v = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, h, apmf::sources::kEngineSource,
                                                   fid, "");
            if (!v.enforce) {
                {
                    std::scoped_lock lk(g_amx);
                    if (auto st = g_actors.find(fid); st != g_actors.end()) Sat(st->second.rankStood);
                }
                t_rank.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(fid, 5))
                    spdlog::info("[ch.18] 0x{} a {} attack claim h={} would deny STANDS: {}. Logged at most once per 5 s "
                                 "per actor.",
                                 apmf::log::Hex(fid), ClassName(cls), h,
                                 apmf::facetattr::WhyStands(apmf::sources::Facet::kCombatAction, v, h));
                return a_w;
            }

            // 7 + 8.
            const bool observe = g_iniObserve || (d.flags & APMF_API::kAtkMix_ObserveOnly) != 0;
            {
                std::scoped_lock lk(g_amx);
                if (auto st = g_actors.find(fid); st != g_actors.end()) Sat(observe ? st->second.wouldDeny : st->second.zeroed);
            }
            (observe ? t_would : t_zeroed).fetch_add(1, std::memory_order_relaxed);
            return observe ? a_w : 0.0f;
        }

        float Thunk(RE::Actor* a_self, RE::Actor* a_target, RE::BGSAttackData* a_atk, const std::uint8_t* a_ctx) {
            const auto orig = g_orig.load(std::memory_order_acquire);
            const float w   = orig(a_self, a_target, a_atk);   // the engine's answer first
            if (!(w > 0.0f) || !g_anyDecl.load(std::memory_order_acquire)) return w;
            if (!a_self || !a_atk || a_self->IsPlayerRef()) return w;
            try {
                return Decide(a_self, a_target, a_atk, a_ctx, w, orig);
            } catch (...) {
                t_errors.fetch_add(1, std::memory_order_relaxed);
                return w;   // nothing unwinds into StartAttack; a throw keeps the engine's weight
            }
        }

        bool BytesAt(std::uintptr_t a_fn, const Window& a_w) {
            return std::memcmp(reinterpret_cast<const void*>(a_fn + a_w.off), a_w.bytes, a_w.len) == 0;
        }
    }  // namespace

    bool Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_whyNot.load(std::memory_order_acquire); }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        const char* why = nullptr;
        const RtSpec* rt = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        else if (GetPrivateProfileIntA("AttackSelect", "bAttackSelect", 1, kIni) == 0) why = "[AttackSelect] bAttackSelect=0";
        if (!why) {
            rt = apmf::allowance::IsRuntime1_7_104()                       ? &k17
               : REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)         ? &kSE
                                                                           : &kAE;
        }
        std::uintptr_t fn = 0, wfn = 0, site = 0;
        if (!why) {
            const auto base = REL::Module::get().base();
            fn   = rt->startId ? REL::ID(rt->startId).address() : base + rt->startRva;
            wfn  = rt->weightId ? REL::ID(rt->weightId).address() : base + rt->weightRva;
            site = fn + rt->siteOff;
            bool ok = apmf::allowance::SeatVerified(fn, kRowStart);
            ok      = apmf::allowance::SeatVerified(wfn, kRowWeight) && ok;
            // The call-site row needs the AE id in the 1.7.104 table, which it is not: there the E8 and its
            // target are proven by the byte checks below against the two verified function rows.
            if (rt->startId) ok = apmf::allowance::SeatVerified(site, kRowSite) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            const auto*  b   = reinterpret_cast<const std::uint8_t*>(site);
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            if (b[0] != 0xE8 || site + 5 + static_cast<std::intptr_t>(rel) != wfn)
                why = "StartAttack's weight call no longer calls the weight function (another patcher got there first)";
            else if (!BytesAt(fn, rt->args) || !BytesAt(fn, rt->loop) || !BytesAt(fn, rt->count) ||
                     !BytesAt(fn, rt->avail) || !BytesAt(fn, rt->stamina) ||
                     std::memcmp(reinterpret_cast<const void*>(fn + rt->cdOff), kCdHead, sizeof(kCdHead)) != 0 ||
                     std::memcmp(reinterpret_cast<const void*>(fn + rt->cdOff + 8), kCdTail, sizeof(kCdTail)) != 0)
                why = "StartAttack's bytes around the weight call differ from the verified ones (context register, "
                      "pool walk, availability / cooldown / stamina tests): another patcher or a different build";
        }
        if (why) {
            g_whyNot.store(why, std::memory_order_release);
            spdlog::warn("[ch.18] attack-selection seat NOT installed -- {}. Every kIntent_AttackSelection claim is "
                         "refused; the engine weighs every attack as it always does.",
                         why);
            return;
        }
        // The cooldown's `now` global, read off the instruction itself (RIP-relative, per build).
        {
            std::int32_t disp = 0;
            std::memcpy(&disp, reinterpret_cast<const void*>(fn + rt->cdOff + 4), sizeof(disp));
            g_now = reinterpret_cast<const float*>(fn + rt->cdOff + 8 + static_cast<std::intptr_t>(disp));
        }
        g_iniObserve = GetPrivateProfileIntA("AttackSelect", "bObserveOnly", 0, kIni) != 0;

        static auto* s_tr = new SKSE::Trampoline("APMF.AttackSelect");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(64);
        if (mem) s_tr->set_trampoline(mem, 64);
        else s_tr->create(64);
        // The stub: mov r9, <ctx register>; jmp qword [rip+0]; dq Thunk. RWX trampoline memory.
        auto* stub = static_cast<std::uint8_t*>(s_tr->allocate(17));
        stub[0] = rt->movR9[0]; stub[1] = rt->movR9[1]; stub[2] = rt->movR9[2];
        stub[3] = 0xFF; stub[4] = 0x25; stub[5] = stub[6] = stub[7] = stub[8] = 0x00;
        const auto thunk = reinterpret_cast<std::uintptr_t>(&Thunk);
        std::memcpy(stub + 9, &thunk, sizeof(thunk));

        g_orig.store(reinterpret_cast<Weight_t>(wfn), std::memory_order_release);   // before the patch
        const std::uintptr_t prev = s_tr->write_call<5>(site, reinterpret_cast<std::uintptr_t>(stub));
        if (prev != wfn)
            spdlog::error("[ch.18] seat patch: the previous call target changed between the check and the patch "
                          "(0x{:X}, expected 0x{:X}); the verified weight function is called.",
                          prev, wfn);
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
            g_ourTarget = site + 5 + static_cast<std::intptr_t>(rel);
        }
        g_site = site;
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kCombatAction, kRowSite, site, prev);
        g_installed.store(true, std::memory_order_release);
        g_whyNot.store("installed", std::memory_order_release);
        const auto base = REL::Module::get().base();
        spdlog::info("[ch.18] attack-selection seat installed on {}: StartAttack (RVA 0x{:X}) +0x{:X} -> the weight "
                     "function (RVA 0x{:X}), context in {}. Observe-only: {}. A denied attack class of a claimed actor "
                     "gets weight 0 in the engine's own draw; every other candidate and actor is untouched.",
                     rt->name, fn - base, rt->siteOff, wfn - base, rt == &kSE ? "r12" : "r13",
                     g_iniObserve ? "yes ([AttackSelect] bObserveOnly=1)" : "no");
    }

    bool SiteIntact() {
        if (!Installed()) return false;
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(g_site + 1), sizeof(rel));
        const bool ok = *reinterpret_cast<const std::uint8_t*>(g_site) == 0xE8 &&
                        g_site + 5 + static_cast<std::intptr_t>(rel) == g_ourTarget;
        if (!ok && !g_siteLost.exchange(true))
            spdlog::error("[ch.18] StartAttack's weight call (RVA 0x{:X}) was RE-PATCHED by another DLL after Harbinger "
                          "installed its seat: ch.18 denies now run only if that DLL calls on to Harbinger. Logged once.",
                          g_site - REL::Module::get().base());
        return ok;
    }

    const char* RefuseReason(RE::FormID actor) {
        if (!Installed()) return NotInstalledReason();
        if (actor == 0x14) return "the actor is the player (the player has no combat behaviour tree)";
        return nullptr;
    }

    void NoteClaim(APMF_API::Handle h) {
        if (h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lk(g_wmx);
        g_claims.insert(h);
        g_everClaimed.store(true, std::memory_order_relaxed);
    }

    void Set(APMF_API::Handle h, const APMF_API::APMF_AttackMix* mix) {
        APMF_API::APMF_AttackMix m{};
        const bool clear = mix == nullptr;
        if (!clear) std::memcpy(&m, mix, sizeof(m));   // read and copied synchronously
        std::uint32_t bad = 0;   // problem bits, logged when they change per handle
        std::string   what;
        Decl d;
        if (!clear) {
            if (m.size < sizeof(APMF_API::APMF_AttackMix)) { bad |= 1; what += " size too small (refused, declaration cleared);"; }
            if (m.governed & APMF_API::kAtkClass_DualWield) { bad |= 2; what += " DualWield is RESERVED (no ATKD flag marks it), masked off;"; }
            if (m.governed & ~(APMF_API::kAtkClass_All | APMF_API::kAtkClass_DualWield)) { bad |= 4; what += " unknown class bits masked off;"; }
            d.governed = m.governed & APMF_API::kAtkClass_All;
            if (m.denied & ~d.governed) { bad |= 8; what += " denied bits outside governed masked off (an undeclared class is never touched);"; }
            d.denied = m.denied & d.governed;
            auto shareOk = [](float f) { return f >= 0.0f && f <= 1.0f; };
            if (m.powerShareMax >= 0.0f) {
                if (!shareOk(m.powerShareMax)) { bad |= 16; what += " powerShareMax outside 0..1 ignored;"; }
                else if (!(d.governed & APMF_API::kAtkClass_Power)) { bad |= 32; what += " powerShareMax needs Power governed, ignored;"; }
                else d.maxShare = m.powerShareMax;
            }
            if (m.powerShareMin >= 0.0f) {
                if (!shareOk(m.powerShareMin)) { bad |= 64; what += " powerShareMin outside 0..1 ignored;"; }
                else if (!(d.governed & APMF_API::kAtkClass_Normal)) { bad |= 128; what += " powerShareMin needs Normal governed, ignored;"; }
                else d.minShare = m.powerShareMin;
            }
            if (d.maxShare >= 0.0f && d.minShare >= 0.0f && d.minShare > d.maxShare) {
                bad |= 256; what += " powerShareMin above powerShareMax: BOTH ignored;";
                d.maxShare = d.minShare = -1.0f;
            }
            d.window = m.window == 0 ? 8 : m.window;
            if (d.window < 4 || d.window > kRing) { bad |= 512; what += " window clamped to [4, 32];"; d.window = d.window < 4 ? 4u : static_cast<std::uint32_t>(kRing); }
            if (m.flags & ~static_cast<std::uint32_t>(APMF_API::kAtkMix_ObserveOnly)) { bad |= 1024; what += " unknown flag bits masked off;"; }
            d.flags = m.flags & APMF_API::kAtkMix_ObserveOnly;
        }
        bool logIt = false;
        {
            std::scoped_lock lk(g_wmx);
            if (!g_claims.contains(h)) return;   // a stale / unknown / non-ch.18 handle: silent no-op
            if (clear || (bad & 1)) g_declsW.erase(h);
            else g_declsW[h] = d;
            auto& last = g_badLogged[h];
            if (bad != last) { last = bad; logIt = bad != 0; }
            PublishLocked();
        }
        if (logIt)
            spdlog::warn("[ch.18] SetAttackMix h={}:{} applied: governed {} denied {} ceiling {:.2f} floor {:.2f} window {}. "
                         "(Logged when the problems change.)",
                         h, what, ClassList(d.governed), ClassList(d.denied), d.maxShare, d.minShare, d.window);
        else if (!clear)
            spdlog::debug("[ch.18] SetAttackMix h={}: governed {} denied {} ceiling {:.2f} floor {:.2f} window {}", h,
                          ClassList(d.governed), ClassList(d.denied), d.maxShare, d.minShare, d.window);
    }

    void Forget(APMF_API::Handle h) {
        std::scoped_lock lk(g_wmx);
        if (!g_claims.erase(h)) return;
        g_badLogged.erase(h);
        if (g_declsW.erase(h)) PublishLocked();
    }

    void ResetAll(const char* why) {
        {
            std::scoped_lock lk(g_wmx);
            g_claims.clear();
            g_badLogged.clear();
            g_declsW.clear();
            PublishLocked();
        }
        {
            std::scoped_lock lk(g_amx);
            g_actors.clear();
        }
        spdlog::debug("[ch.18] reset ({}): declarations, windows and counters dropped", why);
    }

    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_AttackMixInfo* out) {
        if (!Installed()) return APMF_API::kAttackMixState_Unavailable;
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(actor, APMF_API::kIntent_AttackSelection, p, basis, &h))
            return APMF_API::kAttackMixState_None;
        const auto decls = g_decls.load(std::memory_order_acquire);
        const auto it    = decls->find(h);
        if (it == decls->end()) return APMF_API::kAttackMixState_None;
        const Decl d = it->second;
        APMF_API::APMF_AttackMixInfo info{};
        std::uint32_t state = APMF_API::kAttackMixState_Declared;
        {
            std::scoped_lock lk(g_amx);
            if (auto st = g_actors.find(actor); st != g_actors.end()) {
                const auto& s = st->second;
                std::uint32_t pw = 0, nm = 0;
                info.powerShare   = Share(s, d.window, &pw, &nm);
                info.windowFill   = pw + nm;
                info.windowPower  = pw;
                info.windowNormal = nm;
                if (s.handle == h && s.governedSeen) {
                    state           = APMF_API::kAttackMixState_Governed;
                    info.activeDeny = s.activeDeny;
                }
                info.zeroed = s.zeroed; info.floorStood = s.floorStood; info.rankStood = s.rankStood;
                info.wouldDeny = s.wouldDeny;
            } else {
                info.powerShare = -1.0f;
            }
        }
        if (out && out->size >= sizeof(APMF_API::APMF_AttackMixInfo)) {
            info.size = sizeof(info); info.state = state; info.ownerHandle = h;
            info.governed = d.governed; info.denied = d.denied; info.powerShareMax = d.maxShare;
            info.powerShareMin = d.minShare; info.window = d.window;
            *out = info;
        }
        return state;
    }

    void Heartbeat() {
        if (!Installed() || !g_everClaimed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (g_lastBeatMs != 0 && now - g_lastBeatMs < 60000) return;
        g_lastBeatMs = now;
        std::size_t decls = 0, actors = 0;
        {
            std::scoped_lock lk(g_wmx);
            decls = g_declsW.size();
        }
        {
            std::scoped_lock lk(g_amx);
            actors = g_actors.size();
        }
        SiteIntact();
        spdlog::info("[ch.18] H declared={} actors={} governed={} zeroed={} floorStood={} powerUnable={} rankStood={} "
                     "wouldDeny={} poolMiss={} errors={} site={}",
                     decls, actors, t_governed.load(), t_zeroed.load(), t_floor.load(), t_powerUnable.load(),
                     t_rank.load(), t_would.load(), t_poolMiss.load(), t_errors.load(),
                     g_siteLost.load() ? "RE-PATCHED" : "ours");
    }

}  // namespace apmf::attackselect
