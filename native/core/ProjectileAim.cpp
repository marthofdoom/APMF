#include "PCH.h"
#include "core/ProjectileAim.h"
#include "core/Allowance.h"          // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"         // TryGetOwningClaimBasis: the winning ch.29 claim (one RCU read)
#include "core/FacetAttribution.h"   // Decide: the claim vs the Engine source on the CombatAction facet
#include "core/Log.h"
#include "core/Sources.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// Win32, declared by hand (the PCH does not pull in <Windows.h>; core/Sources.cpp's one-line import).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// ch.29 PROJECTILE AIM -- THE LAUNCH SEAT (ABI v36; card H5, marth 2026-11-10).
//
// THE ENGINE PATH (read on the unpacked 1.6.1170 / 1.5.97 executables and the plaintext 1.7.104 one;
// agentlog fable-h5-aim.md, dumps in scratchpad/h5re). An NPC's bow or crossbow shot is released by
// TESObjectWEAP::Fire(weapon, shooter, ammo, enchant, poison) (AE 0x286C90 id 18102 / SE 0x235240 id 17693 /
// 1.7.104 0x28C450, whose id is not in the fork's MIT table, so it is placed by raw RVA; its body is
// instruction-identical to 1.6.1170's up to stack-slot renames, 1230 of 1231 instructions). Fire:
//   * reads the launch origin (the weapon's aim node) and the shooter's pose angles;
//   * for an AI shooter with a combat controller, asks the controller for the AIM POINT of this weapon's
//     CombatProjectileAimController (AE 0x55AB80 / SE 0x4FF4B0 / 1.7.104 0x5629F0: the controller whose +0x10
//     is the weapon, in the array at CombatController+0x50, count +0x60; the point is +0x18..+0x20) and
//     OVERRIDES the pose angles with the direction origin -> aim point (AE 0x8566E0: angleZ = atan2(x, y),
//     angleX = -asin(z / len)), unless the projectile's PROJ flag kDisableCombatAimCorrection is set;
//   * for a bow / crossbow in a non-player's hands (and not alwaysHit) adds the SKILL CONE: a random radius in
//     [0, fBowNPCSpreadAngle x pi/180 x clamp((100 - Archery) / 100, 0, 1)) at a random angle, onto both
//     launch angles (AE 0x2878FC..0x28799D; the setting's only code reference);
//   * builds a Projectile::LaunchData on its stack (origin +0x08, projectile +0x20, shooter +0x28,
//     combatController +0x30 (Actor+0x160 AE / +0x158 SE), weapon +0x38, ammo +0x40, angleZ +0x48, angleX +0x4C,
//     desiredTarget +0x58, power 1.0 +0x94, autoAim +0x9E = 1) and calls Projectile::Launch(&handle, &data)
//     ONCE: AE +0xE60 (0x287AF0) / SE +0xE82 (0x2360C2) / 1.7.104 +0xE70 (0x28D2C0)   <-- THE SEAT.
// The aim point itself comes from the controller's solve (slot-2 thunk -> AE 0x7F7C40 id 44396 / SE 0x75D7B0
// id 43174 / 1.7.104 0x80C980, instruction-identical to AE, same object offsets on SE): an ITERATIVE BALLISTIC
// SOLVE WITH LEAD. Per iteration it takes the low-arc launch angle for the horizontal distance, height
// difference, the controller's launch speed (+0x50) and the projectile's gravity x the engine's gravity scale
// (AE 0x852690: disc = v^4 - g(2 dz v^2 + g d^2), tan = (v^2 - sqrt(disc)) / (g d), atan; pi/4 when out of
// range), the flight time d / (cos x speed) (0x852800), and PREDICTS the target's position after that time
// (0x853FB0: the movement interface, else TESObjectREFR::GetLinearVelocity x t) until the time converges
// (iCombatAimMaxIterations, fCombatAimDeltaThreshold). It stores the RAW LEAD POINT at +0x74..+0x7C and writes
// the aim point's z as origin.z + tan(angle) x d, so aiming at the point IS launching at the ballistic angle.
// It then draws a fresh RANDOM OFFSET into +0x8C..+0x94: uniform +-(+0xA0 x fCombatAimProjectileRandomOffset)
// per axis plus a body-part wobble scaled by +0xA0, where +0xA0 is written once at creation from
// fCombatRangedAimVariance (AE 0x5559AC, id 33135). Slot 7 (AE 0x7F6DA0) nudges +0x18 toward (+0x18 + random)
// every frame, so the point Fire launches at is the smoothed, SCATTERED one.
// => THE ENGINE LEADS MOVING TARGETS. What makes an NPC miss is the two deliberate scatters above.
//
// THE SEAT. One write_call<5> at Fire's Launch call. Before writing, Install re-reads, per runtime: the eight
// bytes of argument setup ending at the E8 (`lea rdx,[rbp+X]; lea rcx,[rbp+Y]`), the E8 and its target
// (Launch), and the solve's RIP-relative read of the gravity scale (the global the correction uses). Any
// difference REFUSES the seat.
//
// THE CORRECTION (Thunk, before Launch). For a non-player shooter whose WINNING kIntent_ProjectileAim claim's
// budget says this shot must land (misses + shots still in flight among the last 10 >= budget; budget 0 =
// every shot), and whose claim outranks the Engine source on the CombatAction facet:
//   P = the controller's raw lead point (+0x74), O = LaunchData.origin, v = the controller's launch speed
//   (+0x50), g = the projectile's gravity (BGSProjectile+0x84; 1.0 for a grenade-type, bit 17 of +0x80) x the
//   engine's gravity scale; d = |P - O|xy; the low-arc angle exactly as the solve computes it; aim =
//   (P.x, P.y, O.z + tan(angle) x d); angleZ = atan2(dx, dy), angleX = -asin(dz / len) exactly as Fire's own
//   direction-to-angles does. Written into LaunchData +0x48 / +0x4C. Both scatters are gone from that shot;
//   the lead, the ballistics, the draw / release animation, the arrow and its damage are the engine's.
// A shot the budget allows to miss is not touched. After Launch the shot is recorded (projectile handle, the
// target: the controller's +0x30 override handle, else CombatController+0x2C targetHandle; the projectile
// form). A TESHitEvent with cause = the shooter and that projectile form resolves the oldest matching shot
// (HIT on the target, or a hit on another actor); a shot older than [ProjectileAim] iShotWindowMs is a MISS.
// Nothing here throws into the engine: any exception leaves the launch as the engine built it.
// ============================================================================

namespace apmf::projectileaim {

    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        constexpr const char* kRowFire    = "ProjectileAim.WeaponFire";
        constexpr const char* kRowLaunch  = "ProjectileAim.ProjectileLaunch";
        constexpr const char* kRowSite    = "ProjectileAim.Site.Launch";
        constexpr const char* kRowSolve   = "ProjectileAim.AimSolve";
        constexpr const char* kRowGravity = "ProjectileAim.GravityScale";
        constexpr const char* kRowVtable  = "ProjectileAim.CombatProjectileAimController";

        // ---- verified offsets (every one read off all three executables; see the header) ----------
        constexpr std::uintptr_t kLdOrigin        = 0x08;   // Projectile::LaunchData (no runtime data: one layout)
        constexpr std::uintptr_t kLdProjectile    = 0x20;
        constexpr std::uintptr_t kLdShooter       = 0x28;
        constexpr std::uintptr_t kLdCombatCtl     = 0x30;
        constexpr std::uintptr_t kLdWeapon        = 0x38;
        constexpr std::uintptr_t kLdAngleZ        = 0x48;
        constexpr std::uintptr_t kLdAngleX        = 0x4C;
        constexpr std::uintptr_t kLdDesiredTarget = 0x58;
        constexpr std::uintptr_t kCcTargetHandle  = 0x2C;   // CombatController (CommonLib agrees; the solve reads it)
        constexpr std::uintptr_t kCcAimArray      = 0x50;   //   CombatProjectileAimController* [] (Fire's getter)
        constexpr std::uintptr_t kCcAimCount      = 0x60;
        constexpr std::uintptr_t kAcWeapon        = 0x10;   // CombatProjectileAimController
        constexpr std::uintptr_t kAcAimPoint      = 0x18;   //   the smoothed, scattered point Fire launches at
        constexpr std::uintptr_t kAcOverride      = 0x30;   //   ch.8b's aim target override handle (CastSeats)
        constexpr std::uintptr_t kAcFlags         = 0x3C;
        constexpr std::uintptr_t kAcSpeed         = 0x50;
        constexpr std::uintptr_t kAcLeadPoint     = 0x74;   //   the solve's raw lead point (no random offset)
        constexpr std::uintptr_t kAcRandom        = 0x8C;   //   the solve's random offset (3 floats)
        constexpr std::uintptr_t kAcVariance      = 0xA0;
        constexpr std::uintptr_t kPjFlagsTypes    = 0x80;   // BGSProjectile::data (flags u16 | types u16)
        constexpr std::uintptr_t kPjGravity       = 0x84;
        constexpr std::uint32_t  kPjGrenadeBit    = 17;     // types bit 1 (kGrenade) in the u32 at +0x80
        constexpr std::uint32_t  kAimArrayMax     = 32;     // bound on the controller walk

        // ---- per-runtime facts (rule 11; every byte read off that runtime's executable) ----------
        struct RtSpec {
            const char*         name;
            std::uint64_t       fireId, launchId, solveId, gravityId, vtableId;   // 0 = raw RVA (1.7.104)
            std::uintptr_t      fireRva, launchRva, solveRva, gravityRva, vtableRva;
            std::uintptr_t      siteOff;      // the E8 inside Fire
            std::uintptr_t      gravityOff;   // the solve's `mulss xmm10, [rip+disp]` reading the scale
            const std::uint8_t* siteLead;     // the 8 bytes before the E8
        };
        // lea rdx,[rbp+0x30]; lea rcx,[rbp-0x78]   (AE / 1.7.104)    lea rdx,[rbp+0x60]; lea rcx,[rbp-0x40]   (SE)
        constexpr std::uint8_t kSiteLeadAE[] = { 0x48, 0x8D, 0x55, 0x30, 0x48, 0x8D, 0x4D, 0x88 };
        constexpr std::uint8_t kSiteLeadSE[] = { 0x48, 0x8D, 0x55, 0x60, 0x48, 0x8D, 0x4D, 0xC0 };
        constexpr std::uint8_t kGravOpcode[] = { 0xF3, 0x44, 0x0F, 0x59, 0x15 };   // mulss xmm10, dword [rip+disp32]
        constexpr std::uint8_t kGravTail[]   = { 0xEB, 0x04 };                       // the jmp that follows it

        constexpr RtSpec kAE{ "1.6.1170", 18102, 44108, 44396, 405286, 210081, 0, 0, 0, 0, 0, 0xE60, 0x2A6, kSiteLeadAE };
        constexpr RtSpec kSE{ "1.5.97", 17693, 42928, 43174, 518746, 264187, 0, 0, 0, 0, 0, 0xE82, 0x2E5, kSiteLeadSE };
        constexpr RtSpec k17{ "1.7.104", 0, 0, 0, 0, 0, 0x28C450, 0x7F9300, 0x80C980, 0x3254BC0, 0x1943F48, 0xE70, 0x2A6,
                              kSiteLeadAE };

        using Launch_t = RE::ProjectileHandle* (*)(RE::ProjectileHandle*, void*);
        std::atomic<Launch_t>      g_orig{ nullptr };
        std::atomic<bool>          g_installed{ false };
        std::atomic<const char*>   g_whyNot{ "not installed yet (before kDataLoaded)" };
        std::uintptr_t             g_site         = 0;         // Install only, then read-only
        std::uintptr_t             g_ourTarget    = 0;
        std::uintptr_t             g_aimVtable    = 0;         // the verified CombatProjectileAimController vtable
        const float*               g_gravityScale = nullptr;   // the verified global the solve multiplies by
        bool                       g_iniObserve   = false;
        bool                       g_iniProbe     = false;
        std::uint64_t              g_windowMs     = 4000;
        std::atomic<bool>          g_siteLost{ false };
        std::atomic<bool>          g_everClaimed{ false };
        std::atomic<bool>          g_anyPending{ false };
        std::uint64_t              g_lastBeatMs = 0;           // main thread only

        // ---- totals (the heartbeat) ----
        std::atomic<std::uint64_t> t_shots{ 0 }, t_corrected{ 0 }, t_hits{ 0 }, t_hitsOther{ 0 }, t_misses{ 0 },
            t_uncorrectable{ 0 }, t_would{ 0 }, t_rank{ 0 }, t_errors{ 0 }, t_vtableMismatch{ 0 };

        // ---- per-actor window, pending shots and counters (leaf mutex; nothing is called while held) ----
        constexpr std::uint32_t kWindow     = 10;
        constexpr std::size_t   kPendingMax = 16;
        constexpr std::size_t   kActorsMax  = 256;
        struct Shot {
            std::uint32_t id        = 0;
            std::uint32_t handle    = 0;   // the launched projectile's handle (0 = none returned)
            RE::FormID    target    = 0;   // the controller's target at launch (0 = unknown)
            RE::FormID    proj      = 0;   // the projectile form
            std::uint64_t t0        = 0;
            bool          corrected = false;
            float         aim[3]{};        // the point aimed at (ours when corrected, else the controller's)
            float         origin[3]{};
        };
        struct ActorState {
            std::array<std::uint8_t, kWindow> ring{};   // 1 = miss, 0 = hit, newest at (head-1)
            std::uint32_t      head = 0, fill = 0;
            std::vector<Shot>  pending;
            std::uint32_t      nextShot = 1;
            APMF_API::Handle   handle   = APMF_API::kInvalidHandle;
            std::uint32_t      budget   = 10;
            bool               governed = false;
            std::uint32_t shots = 0, corrected = 0, hits = 0, hitsOther = 0, misses = 0, uncorrectable = 0,
                          wouldCorrect = 0, rankStood = 0;
        };
        std::mutex                                 g_amx;
        std::unordered_map<RE::FormID, ActorState> g_actors;

        void Sat(std::uint32_t& c) { if (c != 0xFFFFFFFFu) ++c; }

        std::uint32_t WindowMisses(const ActorState& s) {
            std::uint32_t m = 0;
            for (std::uint32_t i = 0; i < s.fill; ++i) m += s.ring[(s.head + kWindow - 1 - i) % kWindow];
            return m;
        }
        void PushOutcome(ActorState& s, bool miss) {
            s.ring[s.head] = miss ? 1 : 0;
            s.head         = (s.head + 1) % kWindow;
            if (s.fill < kWindow) ++s.fill;
        }
        void RefreshAnyPending() {
            bool any = false;
            for (const auto& [fid, s] : g_actors)
                if (!s.pending.empty()) { any = true; break; }
            g_anyPending.store(any, std::memory_order_release);
        }

        const char* NameOf(RE::TESObjectREFR* a) {
            const char* n = a ? a->GetName() : nullptr;
            return n ? n : "";
        }
        bool LineDue(RE::FormID a_fid, std::uint32_t a_key) {
            return apmf::facetattr::LineDue(apmf::sources::Facet::kCombatAction, a_fid, 0x29000000u | a_key);
        }
        constexpr double kRad2Deg = 57.29577951308232;

        template <class T>
        T Rd(const void* a_base, std::uintptr_t a_off) {
            T v{};
            std::memcpy(&v, static_cast<const std::uint8_t*>(a_base) + a_off, sizeof(T));
            return v;
        }
        template <class T>
        void Wr(void* a_base, std::uintptr_t a_off, const T& a_v) {
            std::memcpy(static_cast<std::uint8_t*>(a_base) + a_off, &a_v, sizeof(T));
        }

        // The weapon's aim controller in the shooter's combat controller, as Fire's own getter finds it (entry
        // +0x10 == the weapon, flags bit 1 clear), with the per-call vtable identity check (the CastSeats pattern).
        const std::uint8_t* FindAimController(const std::uint8_t* a_cc, const void* a_weapon, const char*& a_why) {
            if (!a_cc) { a_why = "no combat controller on the launch"; return nullptr; }
            const auto n   = Rd<std::uint32_t>(a_cc, kCcAimCount);
            const auto arr = Rd<const std::uint8_t* const*>(a_cc, kCcAimArray);
            if (!arr || n == 0 || n > kAimArrayMax) { a_why = "no aim controllers on the combat controller"; return nullptr; }
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint8_t* e = arr[i];
                if (!e || Rd<const void*>(e, kAcWeapon) != a_weapon) continue;
                if ((Rd<std::uint32_t>(e, kAcFlags) >> 1) & 1) continue;   // the getter skips these too
                if (Rd<std::uintptr_t>(e, 0) != g_aimVtable) {
                    t_vtableMismatch.fetch_add(1, std::memory_order_relaxed);
                    a_why = "the weapon's aim controller is not a CombatProjectileAimController (vtable mismatch)";
                    return nullptr;
                }
                return e;
            }
            a_why = "no aim controller for this weapon";
            return nullptr;
        }

        RE::FormID FidOfHandle(std::uint32_t a_h) {
            if (!a_h) return 0;
            const auto p = RE::Actor::LookupByHandle(a_h);
            return p ? p->GetFormID() : 0;
        }

        // The engine's own low-arc solve (AE 0x852690) and aim-z rule (the solve, 0x7F8016..0x7F8056), on the
        // controller's raw lead point. Returns false (with a reason) when the inputs cannot be trusted.
        struct Fix {
            float angZ = 0, angX = 0, aim[3]{}, speed = 0, grav = 0;
            double angleDeg = 0;
        };
        bool ComputeFix(const std::uint8_t* a_ld, const std::uint8_t* a_ctl, const std::uint8_t* a_proj, Fix& a_out,
                        const char*& a_why) {
            // The solve sets flags bit 0 when it has run (`or [ctl+0x3C], 1`); before that +0x74 is the ctor's zero.
            if ((Rd<std::uint32_t>(a_ctl, kAcFlags) & 1u) == 0) { a_why = "the controller has not solved yet (no lead point)"; return false; }
            const float speed = Rd<float>(a_ctl, kAcSpeed);
            if (!(speed > 1.0f) || !(speed < 1.0e8f)) { a_why = "the controller's launch speed is not set yet"; return false; }
            float P[3], O[3], A[3];
            std::memcpy(P, a_ctl + kAcLeadPoint, sizeof(P));
            std::memcpy(O, a_ld + kLdOrigin, sizeof(O));
            std::memcpy(A, a_ctl + kAcAimPoint, sizeof(A));
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(P[i]) || !std::isfinite(O[i]) || !std::isfinite(A[i])) { a_why = "a non-finite aim or origin"; return false; }
            // The lead point and the point the engine is launching at must agree to within a body length and the
            // scatter (the random offset is a few tens of units): a stale lead point is never aimed at.
            {
                const double ex = double(P[0]) - A[0], ey = double(P[1]) - A[1];
                if (ex * ex + ey * ey > 1024.0 * 1024.0) { a_why = "the lead point is far from the engine's aim point (stale solve)"; return false; }
            }
            const double dx = double(P[0]) - O[0], dy = double(P[1]) - O[1], dz = double(P[2]) - O[2];
            const double d  = std::sqrt(dx * dx + dy * dy);
            if (d < 16.0) { a_why = "the target is too close for a ballistic re-aim (< 16 units)"; return false; }
            double g = 0.0;
            if (a_proj && g_gravityScale) {
                const auto  ft = Rd<std::uint32_t>(a_proj, kPjFlagsTypes);
                const float pg = ((ft >> kPjGrenadeBit) & 1u) ? 1.0f : Rd<float>(a_proj, kPjGravity);
                g              = double(pg) * double(*g_gravityScale);
                if (!std::isfinite(g)) g = 0.0;
            }
            double aimZ = P[2], tanA = 0.0;
            if (g > 0.0) {
                const double v2   = double(speed) * speed;
                const double disc = v2 * v2 - g * (2.0 * dz * v2 + g * d * d);
                tanA              = disc > 0.0 ? (v2 - std::sqrt(disc)) / (g * d) : 1.0;   // pi/4 when out of range
                aimZ              = O[2] + tanA * d;
            }
            const double ddz = aimZ - O[2];
            const double len = std::sqrt(d * d + ddz * ddz);
            if (!(len > 0.0)) { a_why = "a zero-length launch direction"; return false; }
            double s = ddz / len;
            s        = s > 1.0 ? 1.0 : (s < -1.0 ? -1.0 : s);
            a_out.angZ     = static_cast<float>(std::atan2(dx, dy));
            a_out.angX     = static_cast<float>(-std::asin(s));
            a_out.aim[0]   = P[0]; a_out.aim[1] = P[1]; a_out.aim[2] = static_cast<float>(aimZ);
            a_out.speed    = speed;
            a_out.grav     = static_cast<float>(g);
            a_out.angleDeg = std::atan(tanA) * kRad2Deg;
            if (!std::isfinite(a_out.angZ) || !std::isfinite(a_out.angX)) { a_why = "a non-finite launch angle"; return false; }
            return true;
        }

        // What Before() hands After() across the engine's Launch.
        struct Pre {
            RE::FormID       fid = 0, target = 0, desired = 0, proj = 0;
            APMF_API::Handle h   = APMF_API::kInvalidHandle;
            std::uint32_t    shotId = 0, budget = 10, winMiss = 0, pend = 0;
            bool             corrected = false, wanted = false;
            const char*      why       = nullptr;
            float            engZ = 0, engX = 0, outZ = 0, outX = 0;
            float            aim[3]{}, origin[3]{}, ctlAim[3]{}, rnd[3]{};
            Fix              fix;
            const char*      name = "";
        };

        bool Before(std::uint8_t* a_ld, Pre& a_pre) {
            auto* shooter = Rd<RE::TESObjectREFR*>(a_ld, kLdShooter);
            if (!shooter) return false;
            auto* actor = shooter->As<RE::Actor>();
            if (!actor || actor->IsPlayerRef()) return false;
            const RE::FormID fid = actor->GetFormID();
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(fid, APMF_API::kIntent_ProjectileAim, p, basis, &h))
                return false;
            const std::int64_t iv     = static_cast<std::int64_t>(p.ival);
            const std::uint32_t budget = iv < 0 ? 0u : (iv > 10 ? 10u : static_cast<std::uint32_t>(iv));
            a_pre.fid = fid; a_pre.h = h; a_pre.budget = budget; a_pre.name = NameOf(actor);
            a_pre.engZ = Rd<float>(a_ld, kLdAngleZ);
            a_pre.engX = Rd<float>(a_ld, kLdAngleX);
            std::memcpy(a_pre.origin, a_ld + kLdOrigin, sizeof(a_pre.origin));
            const auto* cc     = Rd<const std::uint8_t*>(a_ld, kLdCombatCtl);
            const auto* weapon = Rd<const void*>(a_ld, kLdWeapon);
            const auto* proj   = Rd<const std::uint8_t*>(a_ld, kLdProjectile);
            if (proj) a_pre.proj = Rd<RE::FormID>(proj, 0x14);   // TESForm::formID
            if (auto* dt = Rd<RE::TESObjectREFR*>(a_ld, kLdDesiredTarget)) a_pre.desired = dt->GetFormID();
            const char*         why = nullptr;
            const std::uint8_t* ctl = FindAimController(cc, weapon, why);
            if (ctl) {
                std::memcpy(a_pre.ctlAim, ctl + kAcAimPoint, sizeof(a_pre.ctlAim));
                std::memcpy(a_pre.rnd, ctl + kAcRandom, sizeof(a_pre.rnd));
                a_pre.target = FidOfHandle(Rd<std::uint32_t>(ctl, kAcOverride));
            }
            if (!a_pre.target && cc) a_pre.target = FidOfHandle(Rd<std::uint32_t>(cc, kCcTargetHandle));
            if (!a_pre.target) a_pre.target = a_pre.desired;

            // The budget's verdict for THIS shot (leaf lock; nothing called under it).
            bool need = false;
            {
                std::scoped_lock lk(g_amx);
                if (g_actors.size() >= kActorsMax && !g_actors.contains(fid)) return false;
                auto& s      = g_actors[fid];
                s.handle     = h;
                s.budget     = budget;
                s.governed   = true;
                a_pre.winMiss = WindowMisses(s);
                a_pre.pend    = static_cast<std::uint32_t>(s.pending.size());
                a_pre.shotId  = s.nextShot++;
                Sat(s.shots);
                need = budget < kWindow && (a_pre.winMiss + a_pre.pend) >= budget;
            }
            t_shots.fetch_add(1, std::memory_order_relaxed);
            g_everClaimed.store(true, std::memory_order_relaxed);
            a_pre.wanted = need;
            if (!need) return true;

            // Ranked against the Engine source on the CombatAction facet (the ch.18 rule).
            const auto v = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, h, apmf::sources::kEngineSource, fid, "");
            if (!v.enforce) {
                { std::scoped_lock lk(g_amx); if (auto it = g_actors.find(fid); it != g_actors.end()) Sat(it->second.rankStood); }
                t_rank.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(fid, 1))
                    spdlog::info("[ch.29] 0x{} '{}' aim claim h={} would correct a shot but the Engine source outranks it: "
                                 "{}. Logged at most once per 5 s per actor.",
                                 apmf::log::Hex(fid), a_pre.name, h,
                                 apmf::facetattr::WhyStands(apmf::sources::Facet::kCombatAction, v, h));
                return true;
            }
            if (!ctl) {
                { std::scoped_lock lk(g_amx); if (auto it = g_actors.find(fid); it != g_actors.end()) Sat(it->second.uncorrectable); }
                t_uncorrectable.fetch_add(1, std::memory_order_relaxed);
                a_pre.why = why;
                if (LineDue(fid, 2))
                    spdlog::warn("[ch.29] 0x{} '{}' shot #{} could NOT be corrected: {}. The engine's launch stands. "
                                 "Logged at most once per 5 s per actor.",
                                 apmf::log::Hex(fid), a_pre.name, a_pre.shotId, why);
                return true;
            }
            if (!ComputeFix(a_ld, ctl, proj, a_pre.fix, why)) {
                { std::scoped_lock lk(g_amx); if (auto it = g_actors.find(fid); it != g_actors.end()) Sat(it->second.uncorrectable); }
                t_uncorrectable.fetch_add(1, std::memory_order_relaxed);
                a_pre.why = why;
                if (LineDue(fid, 3))
                    spdlog::warn("[ch.29] 0x{} '{}' shot #{} could NOT be corrected: {}. The engine's launch stands. "
                                 "Logged at most once per 5 s per actor.",
                                 apmf::log::Hex(fid), a_pre.name, a_pre.shotId, why);
                return true;
            }
            if (g_iniObserve) {
                { std::scoped_lock lk(g_amx); if (auto it = g_actors.find(fid); it != g_actors.end()) Sat(it->second.wouldCorrect); }
                t_would.fetch_add(1, std::memory_order_relaxed);
                a_pre.why = "observe-only";
                return true;
            }
            Wr<float>(a_ld, kLdAngleZ, a_pre.fix.angZ);
            Wr<float>(a_ld, kLdAngleX, a_pre.fix.angX);
            a_pre.outZ = a_pre.fix.angZ;
            a_pre.outX = a_pre.fix.angX;
            a_pre.corrected = true;
            { std::scoped_lock lk(g_amx); if (auto it = g_actors.find(fid); it != g_actors.end()) Sat(it->second.corrected); }
            t_corrected.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        void After(RE::ProjectileHandle* a_out, const Pre& a_pre) {
            Shot sh;
            sh.id        = a_pre.shotId;
            sh.handle    = a_out ? a_out->native_handle() : 0;
            sh.target    = a_pre.target;
            sh.proj      = a_pre.proj;
            sh.t0        = apmf::clock::MonotonicMs();
            sh.corrected = a_pre.corrected;
            std::memcpy(sh.origin, a_pre.origin, sizeof(sh.origin));
            std::memcpy(sh.aim, a_pre.corrected ? a_pre.fix.aim : a_pre.ctlAim, sizeof(sh.aim));
            bool dropped = false;
            {
                std::scoped_lock lk(g_amx);
                auto it = g_actors.find(a_pre.fid);
                if (it == g_actors.end()) return;
                auto& s = it->second;
                if (s.pending.size() >= kPendingMax) {   // the oldest unresolved shot counts as a miss
                    s.pending.erase(s.pending.begin());
                    PushOutcome(s, true);
                    Sat(s.misses);
                    dropped = true;
                }
                s.pending.push_back(sh);
                g_anyPending.store(true, std::memory_order_release);
            }
            if (dropped) t_misses.fetch_add(1, std::memory_order_relaxed);
            if (!g_iniProbe) return;
            const double dZ = (double(a_pre.outZ) - a_pre.engZ) * kRad2Deg, dX = (double(a_pre.outX) - a_pre.engX) * kRad2Deg;
            spdlog::info("[ch.29] 0x{} '{}' SHOT #{} {} budget={} window(misses={} pending={}) target=0x{} desired=0x{} proj=0x{} "
                         "handle=0x{:X} | engine angles (z {:.4f}, x {:.4f}) -> {} (z {:.4f}, x {:.4f}) delta ({:.2f}, {:.2f}) deg | "
                         "origin ({:.0f} {:.0f} {:.0f}) ctl aim ({:.0f} {:.0f} {:.0f}) rnd ({:.1f} {:.1f} {:.1f}) lead aim ({:.0f} {:.0f} {:.0f}) "
                         "speed {:.0f} g {:.1f} angle {:.2f} deg{}{}",
                         apmf::log::Hex(a_pre.fid), a_pre.name, a_pre.shotId,
                         a_pre.corrected ? "CORRECTED" : (a_pre.wanted ? "WANTED" : "ENGINE"), a_pre.budget, a_pre.winMiss,
                         a_pre.pend, apmf::log::Hex(a_pre.target), apmf::log::Hex(a_pre.desired), apmf::log::Hex(a_pre.proj),
                         sh.handle, a_pre.engZ, a_pre.engX, a_pre.corrected ? "ours" : "unchanged",
                         a_pre.corrected ? a_pre.outZ : a_pre.engZ, a_pre.corrected ? a_pre.outX : a_pre.engX,
                         a_pre.corrected ? dZ : 0.0, a_pre.corrected ? dX : 0.0, a_pre.origin[0], a_pre.origin[1],
                         a_pre.origin[2], a_pre.ctlAim[0], a_pre.ctlAim[1], a_pre.ctlAim[2], a_pre.rnd[0], a_pre.rnd[1],
                         a_pre.rnd[2], a_pre.fix.aim[0], a_pre.fix.aim[1], a_pre.fix.aim[2], a_pre.fix.speed, a_pre.fix.grav,
                         a_pre.fix.angleDeg, a_pre.why ? " why=" : "", a_pre.why ? a_pre.why : "");
        }

        RE::ProjectileHandle* Thunk(RE::ProjectileHandle* a_out, void* a_ld) {
            const auto orig = g_orig.load(std::memory_order_acquire);
            Pre  pre{};
            bool tracked = false;
            if (a_ld) {
                try {
                    tracked = Before(static_cast<std::uint8_t*>(a_ld), pre);
                } catch (...) {
                    t_errors.fetch_add(1, std::memory_order_relaxed);
                    tracked = false;   // the launch leaves exactly as the engine built it
                }
            }
            auto* r = orig(a_out, a_ld);
            if (tracked) {
                try {
                    After(a_out, pre);
                } catch (...) {
                    t_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return r;
        }

        // ---- the hit sink (event dispatch thread; the leaf mutex, FormIDs and atomics only) ----
        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static HitSink* Get() { static HitSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override {
                try {
                    if (!a_e || !g_anyPending.load(std::memory_order_acquire)) return RE::BSEventNotifyControl::kContinue;
                    auto* cause = a_e->cause.get();
                    if (!cause) return RE::BSEventNotifyControl::kContinue;
                    const RE::FormID fid  = cause->GetFormID();
                    auto*            tgt  = a_e->target.get();
                    const RE::FormID tfid = tgt ? tgt->GetFormID() : 0;
                    const RE::FormID pj   = a_e->projectile;
                    Shot          sh;
                    bool          found = false, onTarget = false;
                    std::uint32_t winMiss = 0;
                    const char*   name    = "";
                    {
                        std::scoped_lock lk(g_amx);
                        auto it = g_actors.find(fid);
                        if (it == g_actors.end() || it->second.pending.empty()) return RE::BSEventNotifyControl::kContinue;
                        auto& s = it->second;
                        for (auto p = s.pending.begin(); p != s.pending.end(); ++p) {
                            if (pj != 0 && p->proj != 0 && p->proj != pj) continue;
                            sh = *p;
                            s.pending.erase(p);
                            found = true;
                            break;
                        }
                        if (!found) return RE::BSEventNotifyControl::kContinue;
                        onTarget = sh.target == 0 || sh.target == tfid;
                        PushOutcome(s, !onTarget);
                        Sat(onTarget ? s.hits : s.hitsOther);
                        winMiss = WindowMisses(s);
                        RefreshAnyPending();
                    }
                    (onTarget ? t_hits : t_hitsOther).fetch_add(1, std::memory_order_relaxed);
                    if (g_iniProbe) {
                        name = NameOf(cause);
                        spdlog::info("[ch.29] 0x{} '{}' shot #{} {} after {} ms: hit 0x{} '{}' (shot's target 0x{}); window misses now {}",
                                     apmf::log::Hex(fid), name, sh.id, onTarget ? (sh.corrected ? "HIT (corrected)" : "HIT") : "HIT OTHER",
                                     apmf::clock::MonotonicMs() - sh.t0, apmf::log::Hex(tfid), NameOf(tgt), apmf::log::Hex(sh.target),
                                     winMiss);
                    }
                } catch (...) {
                    t_errors.fetch_add(1, std::memory_order_relaxed);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }  // namespace

    bool Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_whyNot.load(std::memory_order_acquire); }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        const char*   why = nullptr;
        const RtSpec* rt  = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        else if (GetPrivateProfileIntA("ProjectileAim", "bProjectileAim", 1, kIni) == 0) why = "[ProjectileAim] bProjectileAim=0";
        if (!why) {
            rt = apmf::allowance::IsRuntime1_7_104()                ? &k17
               : REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)  ? &kSE
                                                                    : &kAE;
        }
        std::uintptr_t fire = 0, launch = 0, solve = 0, grav = 0, vt = 0, site = 0;
        if (!why) {
            const auto base = REL::Module::get().base();
            fire   = rt->fireId ? REL::ID(rt->fireId).address() : base + rt->fireRva;
            launch = rt->launchId ? REL::ID(rt->launchId).address() : base + rt->launchRva;
            solve  = rt->solveId ? REL::ID(rt->solveId).address() : base + rt->solveRva;
            grav   = rt->gravityId ? REL::ID(rt->gravityId).address() : base + rt->gravityRva;
            vt     = rt->vtableId ? REL::ID(rt->vtableId).address() : base + rt->vtableRva;
            site   = fire + rt->siteOff;
            bool ok = apmf::allowance::SeatVerified(fire, kRowFire);
            ok      = apmf::allowance::SeatVerified(launch, kRowLaunch) && ok;
            ok      = apmf::allowance::SeatVerified(site, kRowSite) && ok;
            ok      = apmf::allowance::SeatVerified(solve, kRowSolve) && ok;
            ok      = apmf::allowance::SeatVerified(grav, kRowGravity) && ok;
            ok      = apmf::allowance::SeatVerified(vt, kRowVtable) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            const auto*  b   = reinterpret_cast<const std::uint8_t*>(site);
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            if (b[0] != 0xE8 || site + 5 + static_cast<std::intptr_t>(rel) != launch)
                why = "Fire's launch call no longer calls Projectile::Launch (another patcher got there first)";
            else if (std::memcmp(b - 8, rt->siteLead, 8) != 0)
                why = "the bytes before Fire's launch call differ from the verified ones (another patcher or a different build)";
            else {
                const auto*  g    = reinterpret_cast<const std::uint8_t*>(solve + rt->gravityOff);
                std::int32_t disp = 0;
                std::memcpy(&disp, g + 5, sizeof(disp));
                if (std::memcmp(g, kGravOpcode, sizeof(kGravOpcode)) != 0 || std::memcmp(g + 9, kGravTail, sizeof(kGravTail)) != 0 ||
                    solve + rt->gravityOff + 9 + static_cast<std::intptr_t>(disp) != grav)
                    why = "the aim solve's gravity-scale read differs from the verified bytes (a different build)";
            }
        }
        if (why) {
            g_whyNot.store(why, std::memory_order_release);
            spdlog::warn("[ch.29] projectile-aim seat NOT installed -- {}. Every kIntent_ProjectileAim claim is refused; "
                         "the engine launches every shot as it always does.",
                         why);
            return;
        }
        g_gravityScale = reinterpret_cast<const float*>(grav);
        g_aimVtable    = vt;
        g_iniObserve   = GetPrivateProfileIntA("ProjectileAim", "bObserveOnly", 0, kIni) != 0;
        g_iniProbe     = GetPrivateProfileIntA("ProjectileAim", "bProbe", 0, kIni) != 0;
        {
            const long ms = static_cast<long>(GetPrivateProfileIntA("ProjectileAim", "iShotWindowMs", 4000, kIni));
            g_windowMs    = ms < 500 ? 500u : (ms > 30000 ? 30000u : static_cast<std::uint64_t>(ms));
        }

        static auto* s_tr = new SKSE::Trampoline("APMF.ProjectileAim");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(32);
        if (mem) s_tr->set_trampoline(mem, 32);
        else s_tr->create(32);

        g_orig.store(reinterpret_cast<Launch_t>(launch), std::memory_order_release);   // before the patch
        const std::uintptr_t prev = s_tr->write_call<5>(site, reinterpret_cast<std::uintptr_t>(&Thunk));
        if (prev != launch)
            spdlog::error("[ch.29] seat patch: the previous call target changed between the check and the patch "
                          "(0x{:X}, expected 0x{:X}); the verified Launch is called.",
                          prev, launch);
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
            g_ourTarget = site + 5 + static_cast<std::intptr_t>(rel);
        }
        g_site = site;
        apmf::sources::RegisterCallSeat(apmf::sources::Facet::kCombatAction, kRowSite, site, prev);

        // The hit sink. The holder's GetSingleton is the row the substrate's sinks already verify.
        if (apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(14108, 14298) }.address(),
                                          "Travel.ScriptEventSourceHolder.GetSingleton (the hit / combat sinks)")) {
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) holder->AddEventSink<RE::TESHitEvent>(HitSink::Get());
            else spdlog::error("[ch.29] ScriptEventSourceHolder unavailable: hits cannot be read, every shot will resolve as a MISS "
                               "at the window. Report this line.");
        } else {
            spdlog::error("[ch.29] the ScriptEventSourceHolder row refused: hits cannot be read, every shot will resolve as a "
                          "MISS at the window.");
        }

        g_installed.store(true, std::memory_order_release);
        g_whyNot.store("installed", std::memory_order_release);
        const auto base = REL::Module::get().base();
        spdlog::info("[ch.29] projectile-aim seat installed on {}: TESObjectWEAP::Fire (RVA 0x{:X}) +0x{:X} -> Projectile::Launch "
                     "(RVA 0x{:X}); aim solve RVA 0x{:X} reads the gravity scale at RVA 0x{:X} (now {:.3f}); aim controller vtable "
                     "RVA 0x{:X}. Window {} ms. Observe-only: {}. Probe: {}. A corrected shot of a claimed actor is launched at the "
                     "engine's own unscattered solution; every other shot is untouched.",
                     rt->name, fire - base, rt->siteOff, launch - base, solve - base, grav - base, *g_gravityScale, vt - base,
                     g_windowMs, g_iniObserve ? "yes ([ProjectileAim] bObserveOnly=1)" : "no",
                     g_iniProbe ? "on ([ProjectileAim] bProbe=1)" : "off");
    }

    bool SiteIntact() {
        if (!Installed()) return false;
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(g_site + 1), sizeof(rel));
        const bool ok = *reinterpret_cast<const std::uint8_t*>(g_site) == 0xE8 &&
                        g_site + 5 + static_cast<std::intptr_t>(rel) == g_ourTarget;
        if (!ok && !g_siteLost.exchange(true))
            spdlog::error("[ch.29] Fire's launch call (RVA 0x{:X}) was RE-PATCHED by another DLL after Harbinger installed its "
                          "seat: ch.29 corrections now run only if that DLL calls on to Harbinger. Logged once.",
                          g_site - REL::Module::get().base());
        return ok;
    }

    const char* RefuseReason(RE::FormID actor) {
        if (!Installed()) return NotInstalledReason();
        if (actor == 0x14) return "the actor is the player (the player's shots are aimed by the player)";
        return nullptr;
    }

    void ResetAll(const char* why) {
        {
            std::scoped_lock lk(g_amx);
            g_actors.clear();
            g_anyPending.store(false, std::memory_order_release);
        }
        spdlog::debug("[ch.29] reset ({}): windows, pending shots and counters dropped", why);
    }

    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_ProjectileAimInfo* out) {
        if (!Installed()) return APMF_API::kProjectileAimState_Unavailable;
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(actor, APMF_API::kIntent_ProjectileAim, p, basis, &h))
            return APMF_API::kProjectileAimState_None;
        const std::int64_t  iv = static_cast<std::int64_t>(p.ival);
        APMF_API::APMF_ProjectileAimInfo info{};
        info.budget          = iv < 0 ? 0u : (iv > 10 ? 10u : static_cast<std::uint32_t>(iv));
        std::uint32_t state  = APMF_API::kProjectileAimState_Claimed;
        {
            std::scoped_lock lk(g_amx);
            if (auto it = g_actors.find(actor); it != g_actors.end()) {
                const auto& s = it->second;
                if (s.governed && s.handle == h) state = APMF_API::kProjectileAimState_Governed;
                info.shots = s.shots; info.corrected = s.corrected; info.hits = s.hits; info.hitsOther = s.hitsOther;
                info.misses = s.misses; info.pending = static_cast<std::uint32_t>(s.pending.size());
                info.windowFill = s.fill; info.windowMisses = WindowMisses(s); info.uncorrectable = s.uncorrectable;
                info.wouldCorrect = s.wouldCorrect; info.rankStood = s.rankStood;
            }
        }
        if (out && out->size >= sizeof(APMF_API::APMF_ProjectileAimInfo)) {
            info.size = sizeof(info); info.state = state; info.ownerHandle = h;
            *out = info;
        }
        return state;
    }

    void Heartbeat() {
        if (!Installed()) return;
        const auto now = apmf::clock::MonotonicMs();
        // Shots whose flight window elapsed resolve as MISSES (every frame, cheap when nothing is in flight).
        if (g_anyPending.load(std::memory_order_acquire)) {
            struct Expired { RE::FormID fid; Shot sh; std::uint32_t winMiss; };
            std::vector<Expired> exp;
            {
                std::scoped_lock lk(g_amx);
                for (auto& [fid, s] : g_actors) {
                    for (auto p = s.pending.begin(); p != s.pending.end();) {
                        if (now - p->t0 >= g_windowMs) {
                            PushOutcome(s, true);
                            Sat(s.misses);
                            exp.push_back({ fid, *p, WindowMisses(s) });
                            p = s.pending.erase(p);
                        } else ++p;
                    }
                }
                RefreshAnyPending();
            }
            for (const auto& e : exp) {
                t_misses.fetch_add(1, std::memory_order_relaxed);
                if (!g_iniProbe) continue;
                // The projectile's resting place (the arrow where it stuck or fell) and the target's place now.
                float pp[3]{}, tp[3]{};
                bool  havePP = false, haveTP = false;
                if (e.sh.handle) {
                    if (const auto ref = RE::TESObjectREFR::LookupByHandle(e.sh.handle)) {
                        const auto pos = ref->GetPosition();
                        pp[0] = pos.x; pp[1] = pos.y; pp[2] = pos.z; havePP = true;
                    }
                }
                if (e.sh.target) {
                    if (auto* t = RE::TESForm::LookupByID<RE::Actor>(e.sh.target)) {
                        const auto pos = t->GetPosition();
                        tp[0] = pos.x; tp[1] = pos.y; tp[2] = pos.z; haveTP = true;
                    }
                }
                double dist = -1.0;
                if (havePP && haveTP)
                    dist = std::sqrt((double(pp[0]) - tp[0]) * (pp[0] - tp[0]) + (double(pp[1]) - tp[1]) * (pp[1] - tp[1]) +
                                     (double(pp[2]) - tp[2]) * (pp[2] - tp[2]));
                const char* name = "";
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(e.fid)) name = NameOf(a);
                spdlog::info("[ch.29] 0x{} '{}' shot #{} MISS{} (no hit within {} ms): aimed at ({:.0f} {:.0f} {:.0f}); projectile now {}; "
                             "target 0x{} now {}; projectile-to-target {:.0f}; window misses now {}",
                             apmf::log::Hex(e.fid), name, e.sh.id, e.sh.corrected ? " (corrected)" : "", g_windowMs, e.sh.aim[0],
                             e.sh.aim[1], e.sh.aim[2],
                             havePP ? fmt::format("({:.0f} {:.0f} {:.0f})", pp[0], pp[1], pp[2]) : std::string("gone"),
                             apmf::log::Hex(e.sh.target), haveTP ? fmt::format("({:.0f} {:.0f} {:.0f})", tp[0], tp[1], tp[2]) : std::string("unknown"),
                             dist, e.winMiss);
            }
        }
        if (!g_everClaimed.load(std::memory_order_relaxed)) return;
        if (g_lastBeatMs != 0 && now - g_lastBeatMs < 60000) return;
        g_lastBeatMs = now;
        std::size_t actors = 0;
        {
            std::scoped_lock lk(g_amx);
            actors = g_actors.size();
        }
        SiteIntact();
        spdlog::info("[ch.29] H actors={} shots={} corrected={} hits={} hitsOther={} misses={} uncorrectable={} wouldCorrect={} "
                     "rankStood={} vtableMismatch={} errors={} site={}",
                     actors, t_shots.load(), t_corrected.load(), t_hits.load(), t_hitsOther.load(), t_misses.load(),
                     t_uncorrectable.load(), t_would.load(), t_rank.load(), t_vtableMismatch.load(), t_errors.load(),
                     g_siteLost.load() ? "RE-PATCHED" : "ours");
    }

}  // namespace apmf::projectileaim
