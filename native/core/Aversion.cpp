#include "PCH.h"
#include "core/Aversion.h"
#include "core/Allowance.h"          // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/FacetAttribution.h"   // the Engine source on the Target facet, ranked against the claim
#include "core/Log.h"
#include "core/Sources.h"

#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <shared_mutex>

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, int a_default, const char* a_fileName);

// ============================================================================
// ch.32 AVERSION (kIntent_Aversion, ABI v41) -- a claimant is LESS LIKELY TO BE TARGETED.
//
// THE CARD (MFO class plan H2; marth). The claimant declares a weight in (0, 1). When a foe's target
// selection lands on it, another valid target is picked instead with that probability. The intent never
// invents a target that is not already among the foe's group targets.
//
// IS THERE A VANILLA LEVER? No (asked first, as marth required; read on the three executables, and MFO
// Docs/research/2026-11/class-m0-re.md section 2 + Docs/TAUNT-DESIGN.md 1.3 before that). The standard
// selector's score is: a recent-hit memory (+1000 / +100, 2 s), stickiness (+100 for the current target), the
// same weapon-class state as the attacker (+100), proximity (up to +1000), an attacker-count term whose global
// is 0, reach / combat-area penalties, -500 for a dead or essential-down candidate and -500 for a fleeing one. No actor value, no perk entry
// point and no keyword feeds it. The only vanilla input is DETECTION (admission), which is ch.14 / MFO G2's road
// and hides the claimant from everything, which is not this card.
//
// WHERE THE ENGINE DECIDES (all three executables, 2026-11-11).
//   CombatTargetSelectorStandard::SelectTarget (vtable slot 0x06; AE id 47191 0x84CFE0 / SE id 45918 0x7B5E50 /
//   1.7.104 0x862040) returns its cached pick while it is fresh and otherwise calls the SCORER, its ONE caller on
//   every build (`lea rdx,[rsp+0x50]; mov rcx,rdi; call` at +0x68):
//       ActorHandle* FindBest(selector* rcx, ActorHandle* out rdx)
//       AE id 47195 0x84D280 / SE id 45922 0x7B5FF0 / 1.7.104 0x8622E0
//   FindBest walks combatGroup->targets. An entry that is detected, not hidden and not lost (or the current
//   target while known) is ADMITTED: its handle is resolved, it passes the validity gate and is scored; the
//   best score is written to *out. Everything else goes to a FALLBACK list that is consulted only when nothing
//   was scored (`cmp dword [out],0; jne`). FindBest writes its own stack, *out and reference counts, nothing
//   else: it can be run again in the same call.
//   The validity gate is a call on 1.6.1170 / 1.7.104 and inline on 1.5.97 (principle 11, each its own path):
//     1.6.1170 / 1.7.104   bool IsValidTarget(selector* rcx, Actor* attacker rdx, Actor* candidate r8, zone r9)
//                          AE id 47196 0x84D940 / 1.7.104 0x8629A0, ONE caller: FindBest+0x2AC, then
//                          `test al,al; je <skip this candidate>`. A null candidate returns false.
//     1.5.97               no such function. The scored path resolves the candidate's handle first:
//                          `lea rdx,[rsp+0x58]; lea rcx,[rsp+0x38]; call` at FindBest+0x2C9 to
//                          bool LookupByHandle(const ActorHandle* rcx, NiPointer<Actor>* out rdx)
//                          (SE id 16828 0x2130F0), then `mov rbx,[rsp+0x58]; test rbx,rbx; je <skip>`.
//   The fallback list resolves each of its handles through the same lookup function, at its own call site:
//     AE FindBest+0x52E -> id 17201 0x261E40 / SE FindBest+0x5EF -> id 16828 0x2130F0 / 1.7.104 +0x52E -> 0x2676C0.
//
// THE SEATS (gate the source: the claimant's ADMISSION for one scoring pass; the engine's own scorer ranks
// everyone else and its own answer is what is returned). Three write_call<5> seats per runtime:
//   C0  the selector's call to FindBest. The original runs first, whole. Only when its pick IS an aversion
//       claimant and that claimant's draw says "averted" does the thunk run FindBest AGAIN with the claimant
//       excluded for that pass (thread-local), and answers with the second pick when there is one.
//   X   the exclusion, inside FindBest's scored path. 1.6.1170 / 1.7.104: the IsValidTarget call answers false
//       for the excluded candidate (the engine's own "not a valid target" skip). 1.5.97: the scored path's
//       handle lookup is asked for the null handle instead (the engine's own "unresolved" skip).
//   F   the fallback list's handle lookup. Observe only: it records that the pass fell back, i.e. nothing was
//       scored. A fallback pick is a lost or undetected target, never "another valid target": the first
//       answer stands (the claimant is never refused when it is the only scored candidate).
// Outside a second pass X and F call straight through. Nothing is written to the group, the selector or any
// actor; the selector caches whatever FindBest answered, as it always does.
//
// THE FITNESS TEST (review H2-1 of 0a052e1; default taken, MFO open question 101). In the scorer a candidate
// the attacker cannot reach, cannot path to, that is dead / essential-down, or that is fleeing is PENALISED
// (-2000 / -2500 / -500 / -500), not skipped, so the second pass's best can be exactly such a target and the
// foe would idle, leave its healer for a body on the floor, or run after somebody who is running away. The second pass's answer therefore stands only when it passes the
// SCORER'S OWN four tests, asked by calling the same engine functions with the same arguments FindBest uses
// (Harbinger still computes no score and ranks nobody):
//     reach   bool InCombatArea(CombatController* rcx, const Loc* rdx, float radius xmm2)   false = -2000
//             AE id 33261 0x55B310 / SE id 32512 0x4FFA60 / 1.7.104 0x563180
//             radius = [[controller+0x10]+0x1B8] (every build); asked only when radius < 1024 (the scorer's
//             own RIP constant, read from its `comiss`) or the candidate is in a different space
//     path    bool CrossSpaceReach(Actor* attacker rcx, const Loc* rdx)                      false = -2500
//             AE id 47307 0x853F10 / SE id 46043 0x7BCCD0 / 1.7.104 0x868F80; different space only
//     dead    the candidate's own vfunc at +0x4C8 with edx = 0 (Actor slot 0x99, every build)  true = -500
//             Character's body (AE 0x674ED0, read): life state dead, dying, essential down or recycle
//     fleeing bool IsFleeing(Actor* rcx)                                                      true = -500
//             AE id 37577 0x67EFE0 / SE id 36575 0x5ECB70 / 1.7.104 0x691AA0. Read: with a live, active
//             controller its CombatState::isFleeing, else the process's MiddleHighProcessData::isFleeing.
//             (The H2 round called this test "downed" by inference; the body says fleeing. Essential-down is
//             covered by the dead test above. A bleeding-out, non-essential-down actor passes both.)
//     Loc     Loc* RefLocation(TESObjectREFR* rcx, Loc* out rdx) (a BGSWorldLocation): position at +0x00, worldspace-or-cell at
//             +0x10; `different space` = the attacker's and the candidate's +0x10 differ
//             AE id 19784 0x2EA5E0 / SE id 19357 0x2964A0 / 1.7.104 0x2F0B70
// Each function is a VerifiedAddresses row AND the proven E8 target of its penalty site inside FindBest, and
// the radius load, the 1024 compare and the dead vcall are byte windows re-read at Install (FitSpec below).
// An answer that fails any test, or that cannot be tested, is not answered: the engine's FIRST answer (the
// claimant) stands and the pass is logged and counted as kept (`kept-unfit`). Nothing else is tried.
//
// THE DRAW (principle 9: sized from the engine's cadence, not from a guess). The selector re-scores every
// fCombatSelectTargetUpdateTime (1 s) and 3 s after a switch. A draw per re-score would make the share of time
// a function of that cadence ((1-w)/(1+2w) with the vanilla values), so the draw is made per {foe, claimant}
// per kWindowMs WINDOW instead: a hash of the pair, a session seed and the window index, compared with the
// weight. In any window the claimant is averted for that foe with probability `weight`, whatever the re-score
// cadence is, and two foes draw independently. The window phase is per pair, so a pack does not switch as one.
//
// WHAT WINS OVER IT. A ch.20 pin on the foe replaces the selector's answer after this seat (a declared target
// outranks a weight). The Engine source is RANKED on the Target facet (core/FacetAttribution): the averted
// answer stands only when the claim's priority is strictly above Engine's (APMF_Sources.ini; the defaults say
// yes). The Fixed selector (a scripted or package target) is not seated at all.
//
// DENY-COMPLETENESS (principle 2). The facet is "this actor's standing as a candidate in another actor's
// standard target selection". Covered: every scoring pass of CombatTargetSelectorStandard, on all three
// builds. NOT covered, each listed in DENY-COMPLETENESS-AUDIT row 32: the Fixed selector; a ch.20 pin of any
// client on the foe; another DLL's write of the foe's target after the selector (a later UpdateCombat hook);
// a script StartCombat / the first target of a new fight until the next re-score; the cached pick (up to 3 s);
// the claimant as the ONLY scored candidate; a next best target that fails the scorer's own reach / path /
// dead / fleeing test (the claimant is kept); area effects and attacks aimed at somebody else.
//
// THREADING. FindBest runs inside Actor::UpdateCombat, a BSJobs worker job, several actors in parallel (MFO
// ENGINE_NOTES 0.47). The thunks read: the claimant table under a shared_mutex leaf (written on the game
// thread at Engage / Release), the ControlMap's RCU snapshot, the source registry (lock-free), a thread_local
// pass record, and atomics. They resolve no form and dereference only what the engine handed them (the
// selector's controller, as FindBest itself just did, and on 1.6.1170 / 1.7.104 the candidate the engine
// passes to its own validity gate). The fitness test resolves two reference HANDLES, the attacker's and the
// second pass's answer, exactly the two kinds FindBest resolved a moment earlier in this same call, holds
// them by NiPointer for the test, and hands them to the functions FindBest handed them to.
// Nothing unwinds into the engine: a throw keeps the engine's first answer.
// ============================================================================

namespace apmf::aversion {

    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // ---- the engine functions ------------------------------------------------------------------------------
        using FindBest_t = std::uint32_t* (*)(void* a_selector, std::uint32_t* a_out);
        using IsValid_t  = bool (*)(void* a_selector, RE::Actor* a_attacker, RE::Actor* a_candidate, void* a_zone);
        using Lookup_t   = bool (*)(const std::uint32_t* a_handle, void** a_outSmartPtr);
        // The fitness test's functions (the scorer's own penalty tests; header comment "THE FITNESS TEST").
        struct Loc {                 // what RefLocation writes: 0x18 bytes on every build
            float pos[3];            // 00
            std::uint32_t pad0C;     // 0C
            void* space;             // 10  worldspace, else the cell
        };
        static_assert(sizeof(Loc) == 0x18 && offsetof(Loc, space) == 0x10);
        using RefLoc_t = Loc* (*)(RE::TESObjectREFR* a_ref, Loc* a_out);
        using InArea_t = bool (*)(RE::CombatController* a_controller, const Loc* a_loc, float a_radius);   // radius in xmm2
        using Cross_t  = bool (*)(RE::Actor* a_attacker, const Loc* a_loc);
        using Fleeing_t = bool (*)(RE::Actor* a_actor);
        using IsDead_t = bool (*)(RE::Actor* a_actor, bool a_notEssential);
        constexpr std::size_t    kIsDeadVtblOff     = 0x4C8;   // `call [rax+0x4C8]` on every build (FitSpec::dead)
        constexpr std::uintptr_t kControllerInv     = 0x10;    // `mov rdx,[rcx+0x10]` (FitSpec::radius)
        constexpr std::uintptr_t kInventoryRadius   = 0x1B8;   // `movss xmm,[rdx+0x1B8]`
        static_assert(offsetof(RE::CombatController, inventory) == kControllerInv, "CombatController::inventory moved");

        // The selector's controller and the controller's attacker handle, as FindBest reads them
        // (`mov rax,[rcx+0x10]` at +0x60 and `mov eax,[rax+0x28]` on every build).
        constexpr std::uintptr_t kSelectorController = 0x10;
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28, "CombatController::attackerHandle moved");
        static_assert(sizeof(RE::ActorHandle) == sizeof(std::uint32_t));

        // ---- per-runtime proof (principle 11) ------------------------------------------------------------------
        struct Fn {
            const char*    row;   // native/VerifiedAddresses.h
            std::uint64_t  id;    // 0 = placed by raw RVA (1.7.104: no MIT-table row)
            std::uintptr_t rva;
        };
        struct Site {
            std::size_t         lead;      // offset of the lead bytes inside the function
            const std::uint8_t* leadBytes;
            std::size_t         leadLen;   // the E8 sits at lead + leadLen
            const std::uint8_t* tailBytes; // the bytes right after the 5-byte call
            std::size_t         tailLen;
        };
        struct Win {
            std::size_t         off;   // inside FindBest
            const std::uint8_t* bytes;
            std::size_t         len;
        };
        // The scorer's penalty sites (review H2-1): each E8's target is the function the fitness test calls.
        struct FitSpec {
            Fn   refLoc, inArea, cross, fleeing;
            Site refLocSite, inAreaSite, crossSite, fleeingSite;
            Win  radius;   // mov rcx,[selector+0x10]; mov rdx,[rcx+0x10]; movss xmmN,[rdx+0x1B8]
            Win  limit;    // comiss xmmN,[rip+disp32] (4 opcode bytes; the disp32 follows, then `jb +5`)
            Win  dead;     // mov rax,[rbx]; xor edx,edx; mov rcx,rbx; call [rax+0x4C8]; test al,al; je +8
        };
        struct RtSpec {
            const char* name;
            Fn          slot6, findBest, isValid /* id 0 and rva 0 = inlined on this build */, lookup;
            Site        c0, x, f;
            std::size_t         gateOff;   // FindBest: `cmp dword [out],0; jne` right before the fallback list
            const std::uint8_t* gateBytes;
            bool        xIsLookup;         // true: X is the scored path's handle lookup (1.5.97)
            FitSpec     fit;
        };

        constexpr std::uint8_t kC0Lead[]   = { 0x48, 0x8D, 0x54, 0x24, 0x50, 0x48, 0x8B, 0xCF };            // lea rdx,[rsp+50]; mov rcx,rdi
        constexpr std::uint8_t kC0Tail[]   = { 0x8B, 0x44, 0x24, 0x50, 0x3B, 0x47, 0x18 };                  // mov eax,[rsp+50]; cmp eax,[rdi+18]
        constexpr std::uint8_t kXLeadAE[]  = { 0x4C, 0x8B, 0xC3, 0x49, 0x8B, 0xD6, 0x48, 0x8B, 0x8D, 0xC0, 0x04, 0x00, 0x00 };   // mov r8,rbx; mov rdx,r14; mov rcx,[rbp+4C0]
        constexpr std::uint8_t kXTailAE[]  = { 0x84, 0xC0, 0x0F, 0x84 };                                    // test al,al; je
        constexpr std::uint8_t kXLeadSE[]  = { 0x48, 0x8D, 0x54, 0x24, 0x58, 0x48, 0x8D, 0x4C, 0x24, 0x38 };   // lea rdx,[rsp+58]; lea rcx,[rsp+38]
        constexpr std::uint8_t kXTailSE[]  = { 0x90, 0x48, 0x8B, 0x5C, 0x24, 0x58, 0x48, 0x85, 0xDB, 0x0F, 0x84 };   // nop; mov rbx,[rsp+58]; test rbx,rbx; je
        constexpr std::uint8_t kFLeadAE[]  = { 0x48, 0x8D, 0x54, 0x24, 0x70, 0x48, 0x8D, 0x4C, 0x24, 0x40 };   // lea rdx,[rsp+70]; lea rcx,[rsp+40]
        constexpr std::uint8_t kFTailAE[]  = { 0x90, 0x48, 0x8B, 0x5C, 0x24, 0x70, 0x48, 0x85, 0xDB, 0x74 };   // nop; mov rbx,[rsp+70]; test rbx,rbx; je
        constexpr std::uint8_t kFLeadSE[]  = { 0x48, 0x8D, 0x54, 0x24, 0x78, 0x48, 0x8D, 0x4C, 0x24, 0x40 };   // lea rdx,[rsp+78]; lea rcx,[rsp+40]
        constexpr std::uint8_t kFTailSE[]  = { 0x90, 0x48, 0x8B, 0x5C, 0x24, 0x78, 0x48, 0x85, 0xDB, 0x74 };
        constexpr std::uint8_t kGateAE[]   = { 0x83, 0x3E, 0x00, 0x0F, 0x85 };                              // cmp dword [rsi],0; jne
        constexpr std::uint8_t kGateSE[]   = { 0x83, 0x3B, 0x00, 0x0F, 0x85 };                              // cmp dword [rbx],0; jne
        constexpr std::size_t  kGateLen    = 5;
        // The fitness test's windows (bytes dumped from the three executables 2026-11-11; 1.6.1170 and 1.7.104
        // are identical apart from rel32 / RIP displacements, 1.5.97 keeps the attacker in rsi and the radius
        // in xmm10 and has its own offsets).
        constexpr std::uint8_t kLocLead[]     = { 0x48, 0x8D, 0x55, 0x90, 0x48, 0x8B, 0xCB };                // lea rdx,[rbp-70]; mov rcx,rbx
        constexpr std::uint8_t kLocTailAE[]   = { 0x48, 0x8B, 0x4D, 0xA0, 0x48, 0x3B, 0x4D, 0xE0 };          // mov rcx,[rbp-60]; cmp rcx,[rbp-20]
        constexpr std::uint8_t kLocTailSE[]   = { 0x48, 0x8B, 0x45, 0x20, 0x48, 0x39, 0x45, 0xA0 };          // mov rax,[rbp+20]; cmp [rbp-60],rax
        constexpr std::uint8_t kAreaLeadAE[]  = { 0x41, 0x0F, 0x28, 0xD1, 0x48, 0x8D, 0x55, 0x90, 0x48, 0x8B, 0x85, 0xC0, 0x04, 0x00, 0x00,
                                                  0x48, 0x8B, 0x48, 0x10 };   // movaps xmm2,xmm9; lea rdx,[rbp-70]; mov rax,[rbp+4C0]; mov rcx,[rax+10]
        constexpr std::uint8_t kAreaLeadSE[]  = { 0x41, 0x0F, 0x28, 0xD2, 0x48, 0x8D, 0x55, 0x90, 0x48, 0x8B, 0x85, 0x00, 0x05, 0x00, 0x00,
                                                  0x48, 0x8B, 0x48, 0x10 };   // movaps xmm2,xmm10; lea rdx,[rbp-70]; mov rax,[rbp+500]; mov rcx,[rax+10]
        constexpr std::uint8_t kPenTail[]     = { 0x84, 0xC0, 0x75, 0x08 };                                  // test al,al; jne +8
        constexpr std::uint8_t kCrossLeadAE[] = { 0x48, 0x8D, 0x55, 0x90, 0x49, 0x8B, 0xCE };                // lea rdx,[rbp-70]; mov rcx,r14
        constexpr std::uint8_t kCrossLeadSE[] = { 0x48, 0x8D, 0x55, 0x90, 0x48, 0x8B, 0xCE };                // lea rdx,[rbp-70]; mov rcx,rsi
        constexpr std::uint8_t kFleeLead[]    = { 0x48, 0x8B, 0xCB };                                        // mov rcx,rbx
        constexpr std::uint8_t kFleeTailAE[]  = { 0x48, 0x8B, 0xB5, 0xC8, 0x04, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x08 };   // mov rsi,[rbp+4C8]; test al,al; je +8
        constexpr std::uint8_t kFleeTailSE[]  = { 0x41, 0x83, 0xCE, 0xFF, 0x84, 0xC0, 0x74, 0x08 };          // or r14d,-1; test al,al; je +8
        constexpr std::uint8_t kRadiusAE[]    = { 0x48, 0x8B, 0x4B, 0x10, 0x48, 0x8B, 0x51, 0x10, 0xF3, 0x44, 0x0F, 0x10, 0x8A, 0xB8, 0x01, 0x00, 0x00 };
        constexpr std::uint8_t kRadiusSE[]    = { 0x49, 0x8B, 0x4E, 0x10, 0x48, 0x8B, 0x51, 0x10, 0xF3, 0x44, 0x0F, 0x10, 0x92, 0xB8, 0x01, 0x00, 0x00 };
        constexpr std::uint8_t kLimitAE[]     = { 0x44, 0x0F, 0x2F, 0x0D };                                  // comiss xmm9,[rip+disp32]
        constexpr std::uint8_t kLimitSE[]     = { 0x44, 0x0F, 0x2F, 0x15 };                                  // comiss xmm10,[rip+disp32]
        constexpr std::uint8_t kLimitTail[]   = { 0x72, 0x05 };                                              // jb +5 (radius < limit: ask the area)
        constexpr std::uint8_t kDeadCall[]    = { 0x48, 0x8B, 0x03, 0x33, 0xD2, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0xC8, 0x04, 0x00, 0x00,
                                                  0x84, 0xC0, 0x74, 0x08 };   // mov rax,[rbx]; xor edx,edx; mov rcx,rbx; call [rax+4C8]; test al,al; je +8
        constexpr float        kAreaLimitWant = 1024.0f;   // the value behind the scorer's RIP constant on the three builds

#define APMF_AV_FIT_AE(LOC, AREA, CROSS, FLEE)                                                                              \
        { LOC, AREA, CROSS, FLEE,                                                                                           \
          { 0x357, kLocLead, sizeof(kLocLead), kLocTailAE, sizeof(kLocTailAE) },                                            \
          { 0x3DC, kAreaLeadAE, sizeof(kAreaLeadAE), kPenTail, sizeof(kPenTail) },                                          \
          { 0x405, kCrossLeadAE, sizeof(kCrossLeadAE), kPenTail, sizeof(kPenTail) },                                        \
          { 0x437, kFleeLead, sizeof(kFleeLead), kFleeTailAE, sizeof(kFleeTailAE) },                                        \
          { 0x12A, kRadiusAE, sizeof(kRadiusAE) }, { 0x3CD, kLimitAE, sizeof(kLimitAE) }, { 0x41D, kDeadCall, sizeof(kDeadCall) } }

        constexpr RtSpec kAE{
            "1.6.1170",
            { "Aversion.SelectTarget.Standard", 47191, 0 }, { "Aversion.FindBest", 47195, 0 },
            { "Aversion.IsValidTarget", 47196, 0 },         { "Aversion.LookupActorByHandle", 17201, 0 },
            { 0x60, kC0Lead, sizeof(kC0Lead), kC0Tail, sizeof(kC0Tail) },
            { 0x29F, kXLeadAE, sizeof(kXLeadAE), kXTailAE, sizeof(kXTailAE) },
            { 0x524, kFLeadAE, sizeof(kFLeadAE), kFTailAE, sizeof(kFTailAE) },
            0x4C4, kGateAE, false,
            APMF_AV_FIT_AE((Fn{ "Aversion.RefLocation", 19784, 0 }), (Fn{ "Aversion.InCombatArea", 33261, 0 }),
                           (Fn{ "Aversion.CrossSpaceReach", 47307, 0 }), (Fn{ "Aversion.IsFleeing", 37577, 0 })) };
        constexpr RtSpec kSE{
            "1.5.97",
            { "Aversion.SelectTarget.Standard", 45918, 0 }, { "Aversion.FindBest", 45922, 0 },
            { nullptr, 0, 0 },                              { "Aversion.LookupActorByHandle", 16828, 0 },
            { 0x60, kC0Lead, sizeof(kC0Lead), kC0Tail, sizeof(kC0Tail) },
            { 0x2BF, kXLeadSE, sizeof(kXLeadSE), kXTailSE, sizeof(kXTailSE) },
            { 0x5E5, kFLeadSE, sizeof(kFLeadSE), kFTailSE, sizeof(kFTailSE) },
            0x53B, kGateSE, true,
            { { "Aversion.RefLocation", 19357, 0 }, { "Aversion.InCombatArea", 32512, 0 },
              { "Aversion.CrossSpaceReach", 46043, 0 }, { "Aversion.IsFleeing", 36575, 0 },
              { 0x3C2, kLocLead, sizeof(kLocLead), kLocTailSE, sizeof(kLocTailSE) },
              { 0x452, kAreaLeadSE, sizeof(kAreaLeadSE), kPenTail, sizeof(kPenTail) },
              { 0x47B, kCrossLeadSE, sizeof(kCrossLeadSE), kPenTail, sizeof(kPenTail) },
              { 0x4AD, kFleeLead, sizeof(kFleeLead), kFleeTailSE, sizeof(kFleeTailSE) },
              { 0x15C, kRadiusSE, sizeof(kRadiusSE) }, { 0x443, kLimitSE, sizeof(kLimitSE) }, { 0x493, kDeadCall, sizeof(kDeadCall) } } };
        // 1.7.104: raw RVAs (none of the four AE ids has an MIT-table row); offsets and bytes are the 1.6.1170
        // ones, re-read on the 1.7.104 executable (same windows at the same offsets).
        constexpr RtSpec k17{
            "1.7.104",
            { "Aversion.SelectTarget.Standard", 0, 0x862040 }, { "Aversion.FindBest", 0, 0x8622E0 },
            { "Aversion.IsValidTarget", 0, 0x8629A0 },         { "Aversion.LookupActorByHandle", 0, 0x2676C0 },
            { 0x60, kC0Lead, sizeof(kC0Lead), kC0Tail, sizeof(kC0Tail) },
            { 0x29F, kXLeadAE, sizeof(kXLeadAE), kXTailAE, sizeof(kXTailAE) },
            { 0x524, kFLeadAE, sizeof(kFLeadAE), kFTailAE, sizeof(kFTailAE) },
            0x4C4, kGateAE, false,
            APMF_AV_FIT_AE((Fn{ "Aversion.RefLocation", 0, 0x2F0B70 }), (Fn{ "Aversion.InCombatArea", 0, 0x563180 }),
                           (Fn{ "Aversion.CrossSpaceReach", 0, 0x868F80 }), (Fn{ "Aversion.IsFleeing", 0, 0x691AA0 })) };
#undef APMF_AV_FIT_AE

        // ---- state ---------------------------------------------------------------------------------------------
        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_whyNot{ "not installed yet (before kDataLoaded)" };
        // Written once by Install before any site is patched, read-only after.
        FindBest_t g_findBest = nullptr;
        IsValid_t  g_isValid  = nullptr;
        Lookup_t   g_lookup   = nullptr;
        RefLoc_t   g_refLoc   = nullptr;   // the fitness test's four functions and the scorer's own area limit
        InArea_t   g_inArea   = nullptr;
        Cross_t    g_cross    = nullptr;
        Fleeing_t   g_fleeing   = nullptr;
        const float* g_areaLimit = nullptr;
        std::uint64_t g_seed  = 0;

        // The claimants: reference handle -> FormID. Written on the game thread, read by the seats.
        std::shared_mutex                              g_mx;   // leaf: no engine call, no log under it
        std::unordered_map<std::uint32_t, RE::FormID>  g_byHandle;
        std::unordered_map<RE::FormID, std::uint32_t>  g_byId;
        std::atomic<std::size_t>                       g_count{ 0 };

        std::atomic<std::uint64_t> g_landed{ 0 };       // a scoring pass picked a claimant
        std::atomic<std::uint64_t> g_averted{ 0 };      // ... and another valid target was answered instead
        std::atomic<std::uint64_t> g_keptDraw{ 0 };     // ... and the draw kept it (1 - weight)
        std::atomic<std::uint64_t> g_keptOnly{ 0 };     // ... averted by the draw, but no other valid target
        std::atomic<std::uint64_t> g_keptUnfit{ 0 };    // ... the next best target failed the scorer's own reach / path / dead / fleeing test
        std::atomic<std::uint64_t> g_keptRank{ 0 };     // ... the claim does not outrank Engine on the Target facet
        std::atomic<std::uint64_t> g_badWeight{ 0 };    // ... the winning claim's weight is not in (0, 1)
        std::atomic<std::uint64_t> g_errors{ 0 };
        std::atomic<std::uint64_t> g_lastBeatMs{ 0 };
        std::atomic<std::uint64_t> g_beatSeen{ 0 };

        constexpr std::uint64_t kWindowMs    = 6000;    // the draw's window: two of the engine's 3 s switch intervals
        constexpr std::uint64_t kLineMs      = 10000;   // per {foe, claimant, outcome} line cadence
        constexpr std::uint64_t kHeartbeatMs = 30000;
        constexpr std::size_t   kLineKeysMax = 4096;
        constexpr std::uint32_t kMaxExcluded = 4;       // claimants excluded in one selection (a party's back line)

        std::mutex                                       g_lineMx;   // leaf
        std::unordered_map<std::uint64_t, std::uint64_t> g_lineLast;

        bool LineDue(std::uint32_t a_foe, std::uint32_t a_claimant, std::uint32_t a_outcome) {
            const std::uint64_t key = (static_cast<std::uint64_t>(a_foe) << 32) ^
                                      (static_cast<std::uint64_t>(a_claimant) << 3) ^ a_outcome;
            const auto       now = apmf::clock::MonotonicMs();
            std::scoped_lock lk(g_lineMx);
            if (g_lineLast.size() > kLineKeysMax) g_lineLast.clear();
            auto& last = g_lineLast[key];
            if (last != 0 && now - last < kLineMs) return false;
            last = now ? now : 1;
            return true;
        }

        // ---- the second pass's thread-local record -------------------------------------------------------------
        struct Pass {
            bool          active   = false;
            bool          fallback = false;   // the pass consulted the fallback list: nothing was scored
            std::uint32_t n        = 0;
            std::uint32_t handles[kMaxExcluded]{};
            RE::FormID    ids[kMaxExcluded]{};
        };
        thread_local Pass t_pass;

        bool ExcludedHandle(std::uint32_t a_h) {
            for (std::uint32_t i = 0; i < t_pass.n; ++i)
                if (t_pass.handles[i] == a_h) return true;
            return false;
        }
        bool ExcludedId(RE::FormID a_id) {
            for (std::uint32_t i = 0; i < t_pass.n; ++i)
                if (t_pass.ids[i] == a_id) return true;
            return false;
        }
        struct PassScope {
            PassScope() { t_pass = Pass{}; t_pass.active = true; }
            ~PassScope() { t_pass = Pass{}; }
        };

        // ---- the claim behind a picked handle ------------------------------------------------------------------
        struct Claim {
            RE::FormID       id     = 0;
            float            weight = 0.0f;
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
        };
        // 0 = not a claimant; 1 = a claimant with a usable weight; 2 = a claimant whose weight is out of range.
        int ClaimOf(std::uint32_t a_handle, Claim& a_out) {
            {
                std::shared_lock lk(g_mx);
                const auto it = g_byHandle.find(a_handle);
                if (it == g_byHandle.end()) return 0;
                a_out.id = it->second;
            }
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_out.id, APMF_API::kIntent_Aversion, p, basis, &a_out.handle))
                return 0;   // released since the table was written (the Release call is one drain behind)
            a_out.weight = p.fval;
            return (std::isfinite(p.fval) && p.fval > 0.0f && p.fval < 1.0f) ? 1 : 2;
        }

        std::uint64_t Mix(std::uint64_t a_x) {   // splitmix64 finalizer
            a_x += 0x9E3779B97F4A7C15ull;
            a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
            a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
            return a_x ^ (a_x >> 31);
        }
        // The draw: true for `weight` of the windows of this {foe, claimant} pair.
        bool Averted(std::uint32_t a_foe, std::uint32_t a_claimant, float a_weight) {
            const std::uint64_t pair   = Mix(((static_cast<std::uint64_t>(a_foe) << 32) | a_claimant) ^ g_seed);
            const std::uint64_t window = (apmf::clock::MonotonicMs() + pair % kWindowMs) / kWindowMs;
            const double        u      = static_cast<double>(Mix(pair ^ (window * 0xD6E8FEB86659FD93ull)) >> 11) *
                                         (1.0 / 9007199254740992.0);   // [0, 1)
            return u < static_cast<double>(a_weight);
        }

        RE::FormID FoeId(void* a_selector) {   // for a log line only
            auto* cc = *reinterpret_cast<RE::CombatController* const*>(
                reinterpret_cast<std::uintptr_t>(a_selector) + kSelectorController);
            if (!cc) return 0;
            const auto p = cc->attackerHandle.get();
            return p ? p->GetFormID() : 0;
        }

        // THE FITNESS TEST (header comment). nullptr = the second pass's answer passes the scorer's own four tests;
        // otherwise which one it failed. Calls only the functions FindBest calls, with FindBest's arguments.
        const char* UnfitReason(RE::CombatController* a_cc, std::uint32_t a_alt, RE::FormID& a_altId) {
            if (!a_cc) return "the selector has no controller (not testable)";
            RE::ActorHandle ah{};
            std::memcpy(&ah, &a_alt, sizeof(a_alt));   // the 4-byte handle value the scorer answered
            const RE::NiPointer<RE::Actor> cand     = ah.get();
            const RE::NiPointer<RE::Actor> attacker = a_cc->attackerHandle.get();
            if (!cand) return "its handle no longer resolves (not testable)";
            a_altId = cand->GetFormID();
            if (!attacker) return "the foe's own handle no longer resolves (not testable)";
            const auto inv = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(a_cc) + kControllerInv);
            if (!inv) return "the foe's controller has no inventory record (not testable)";
            const float radius = *reinterpret_cast<const float*>(inv + kInventoryRadius);
            Loc la{}, lc{};
            g_refLoc(attacker.get(), &la);
            g_refLoc(cand.get(), &lc);
            const bool otherSpace = la.space != lc.space;
            // `comiss radius,limit; jb <ask>`: asked when radius < limit (an unordered compare asks too), or in another space.
            if (!(radius >= *g_areaLimit) || otherSpace) {
                if (!g_inArea(a_cc, &lc, radius)) return "it is outside the foe's combat area (the scorer's reach test)";
                if (otherSpace && !g_cross(attacker.get(), &lc))
                    return "it is in another cell or worldspace the foe cannot path to (the scorer's path test)";
            }
            const auto isDead = *reinterpret_cast<const IsDead_t*>(*reinterpret_cast<const std::uintptr_t*>(cand.get()) + kIsDeadVtblOff);
            if (isDead(cand.get(), false)) return "it is dead or essential-down (the scorer's dead test)";
            if (g_fleeing(cand.get())) return "it is fleeing (the scorer's fleeing test)";
            return nullptr;
        }

        // C0's body after the engine's own answer `a_first` (non-zero). Returns the handle to answer with.
        std::uint32_t Shape(void* a_selector, std::uint32_t a_first) {
            Claim c;
            const int kind = ClaimOf(a_first, c);
            if (kind == 0) return a_first;
            g_landed.fetch_add(1, std::memory_order_relaxed);

            auto* cc = *reinterpret_cast<RE::CombatController* const*>(
                reinterpret_cast<std::uintptr_t>(a_selector) + kSelectorController);
            const std::uint32_t foe = cc ? cc->attackerHandle.native_handle() : 0;
            if (kind == 2) {
                g_badWeight.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(foe, a_first, 4))
                    spdlog::warn("[ch.32] aversion: 0x{} holds a claim (h={}) whose weight {} is not inside (0, 1) -- the "
                                 "engine's pick stands. Repoint it with a weight in (0, 1).",
                                 apmf::log::Hex(c.id), c.handle, c.weight);
                return a_first;
            }
            if (!Averted(foe, a_first, c.weight)) {
                g_keptDraw.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(foe, a_first, 0))
                    spdlog::info("[ch.32] aversion: foe 0x{} selected 0x{} (weight {:.2f}, h={}) -- KEPT by the draw for this "
                                 "{} s window.",
                                 apmf::log::Hex(FoeId(a_selector)), apmf::log::Hex(c.id), c.weight, c.handle, kWindowMs / 1000);
                return a_first;
            }

            // The second pass: the same scorer, the averted claimant(s) excluded. The engine ranks the rest.
            std::uint32_t pick = 0;
            {
                PassScope scope;
                t_pass.handles[0] = a_first;
                t_pass.ids[0]     = c.id;
                t_pass.n          = 1;
                for (std::uint32_t i = 0; i < kMaxExcluded; ++i) {
                    std::uint32_t alt = 0;
                    t_pass.fallback   = false;
                    g_findBest(a_selector, &alt);
                    if (alt == 0 || t_pass.fallback || ExcludedHandle(alt)) break;   // nobody else was scored
                    Claim c2;
                    if (t_pass.n < kMaxExcluded && ClaimOf(alt, c2) == 1 && Averted(foe, alt, c2.weight)) {
                        t_pass.handles[t_pass.n] = alt;   // another claimant, averted too in this window
                        t_pass.ids[t_pass.n]     = c2.id;
                        ++t_pass.n;
                        continue;
                    }
                    pick = alt;
                    break;
                }
            }
            if (pick == 0) {
                g_keptOnly.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(foe, a_first, 1))
                    spdlog::info("[ch.32] aversion: foe 0x{} selected 0x{} (weight {:.2f}, h={}), averted by the draw, but the "
                                 "engine scored NO OTHER valid target -- the pick stands (never the only candidate, never a "
                                 "lost one).",
                                 apmf::log::Hex(FoeId(a_selector)), apmf::log::Hex(c.id), c.weight, c.handle);
                return a_first;
            }
            // Review H2-1: the scorer PENALISES an unreachable, unpathable, dead or fleeing candidate, it does not skip
            // it, so the second pass's best can be one. It is answered only when it passes the scorer's own tests.
            RE::FormID altId = 0;
            if (const char* unfit = UnfitReason(cc, pick, altId)) {
                g_keptUnfit.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(foe, a_first, 5))
                    spdlog::info("[ch.32] aversion: foe 0x{} selected 0x{} (weight {:.2f}, h={}), averted by the draw, but the "
                                 "engine's next best target 0x{} is not answered: {} -- the pick STANDS (KEPT, the claimant is "
                                 "the only target the foe can act on).",
                                 apmf::log::Hex(FoeId(a_selector)), apmf::log::Hex(c.id), c.weight, c.handle,
                                 apmf::log::Hex(altId), unfit);
                return a_first;
            }
            // The engine's pick is the Engine source on the Target facet: ranked, like every enforcing seat.
            const auto v = apmf::facetattr::Decide(apmf::sources::Facet::kTarget, c.handle, apmf::sources::kEngineSource,
                                                   c.id, "");
            if (!v.enforce) {
                g_keptRank.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(foe, a_first, 2))
                    spdlog::info("[ch.32] aversion: foe 0x{} selected 0x{} (h={}); {} priority {} does not outrank Engine "
                                 "(priority {}) on the Target facet, so the engine's pick STANDS (APMF_Sources.ini).",
                                 apmf::log::Hex(FoeId(a_selector)), apmf::log::Hex(c.id), c.handle,
                                 apmf::facetattr::NameOr(v.client, "an internal claim"), v.claimPrio, v.srcPrio);
                return a_first;
            }
            g_averted.fetch_add(1, std::memory_order_relaxed);
            apmf::sources::NoteServedClaim(c.handle, apmf::sources::Facet::kTarget);
            if (LineDue(foe, a_first, 3)) {
                const RE::FormID other = altId;   // resolved by the fitness test
                spdlog::info("[ch.32] aversion: foe 0x{} selected 0x{} (weight {:.2f}, h={}) -- AVERTED for this {} s window: "
                             "the engine's next best valid target 0x{} (inside the foe's reach and path, alive and not fleeing by the "
                             "scorer's own tests) is answered instead.",
                             apmf::log::Hex(FoeId(a_selector)), apmf::log::Hex(c.id), c.weight, c.handle, kWindowMs / 1000,
                             apmf::log::Hex(other));
            }
            return pick;
        }

        // ---- the thunks (the engine's combat update, a BSJobs worker) -------------------------------------------
        std::uint32_t* FindBestThunk(void* a_selector, std::uint32_t* a_out) {
            std::uint32_t* const ret = g_findBest(a_selector, a_out);
            if (g_count.load(std::memory_order_relaxed) == 0 || !a_selector || !a_out) return ret;
            const std::uint32_t first = *a_out;
            if (first == 0) return ret;   // the engine holds no target: nothing to shape, nothing invented
            try {
                *a_out = Shape(a_selector, first);
            } catch (...) {
                t_pass = Pass{};
                *a_out = first;   // nothing unwinds into the combat update; the engine's own answer stands
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }
            return ret;
        }

        // X on 1.6.1170 / 1.7.104: the engine's validity gate answers "not valid" for an excluded candidate.
        bool IsValidThunk(void* a_selector, RE::Actor* a_attacker, RE::Actor* a_candidate, void* a_zone) {
            if (t_pass.active && a_candidate && ExcludedId(a_candidate->GetFormID())) return false;
            return g_isValid(a_selector, a_attacker, a_candidate, a_zone);
        }
        // X on 1.5.97: the scored path's lookup is asked for the null handle for an excluded candidate, so the
        // engine's own function leaves the pointer null and the caller takes its own "unresolved" skip.
        bool ScoredLookupThunk(const std::uint32_t* a_handle, void** a_out) {
            if (t_pass.active && a_handle && ExcludedHandle(*a_handle)) {
                const std::uint32_t none = 0;
                return g_lookup(&none, a_out);
            }
            return g_lookup(a_handle, a_out);
        }
        // F: the fallback list is being resolved, so the scored path picked nobody in this pass.
        bool FallbackLookupThunk(const std::uint32_t* a_handle, void** a_out) {
            if (t_pass.active) t_pass.fallback = true;
            return g_lookup(a_handle, a_out);
        }

        // ---- install helpers -----------------------------------------------------------------------------------
        std::uintptr_t Resolve(const Fn& a_fn) {
            return a_fn.id ? REL::ID(a_fn.id).address() : REL::Module::get().base() + a_fn.rva;
        }
        // The lead bytes, an E8 that lands on a_target, the tail bytes. Returns the E8's address or 0.
        std::uintptr_t ProveSite(std::uintptr_t a_fn, const Site& a_s, std::uintptr_t a_target) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(a_fn + a_s.lead);
            if (std::memcmp(b, a_s.leadBytes, a_s.leadLen) != 0) return 0;
            const std::uintptr_t e8 = a_fn + a_s.lead + a_s.leadLen;
            if (*reinterpret_cast<const std::uint8_t*>(e8) != 0xE8) return 0;
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(e8 + 1), sizeof(rel));
            if (e8 + 5 + static_cast<std::intptr_t>(rel) != a_target) return 0;
            if (std::memcmp(reinterpret_cast<const void*>(e8 + 5), a_s.tailBytes, a_s.tailLen) != 0) return 0;
            return e8;
        }
    }  // namespace

    bool        Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_whyNot.load(std::memory_order_acquire); }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        const char*   why = nullptr;
        const RtSpec* rt  = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        else if (GetPrivateProfileIntA("Aversion", "bAversion", 1, kIni) == 0) why = "[Aversion] bAversion=0";
        if (!why) {
            rt = apmf::allowance::IsRuntime1_7_104()               ? &k17
               : REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97) ? &kSE
                                                                   : &kAE;
        }
        std::uintptr_t slot6 = 0, findBest = 0, isValid = 0, lookup = 0, c0 = 0, x = 0, f = 0;
        std::uintptr_t refLoc = 0, inArea = 0, cross = 0, fleeing = 0;
        const float*   areaLimit = nullptr;
        if (!why) {
            slot6    = Resolve(rt->slot6);
            findBest = Resolve(rt->findBest);
            lookup   = Resolve(rt->lookup);
            bool ok  = apmf::allowance::SeatVerified(slot6, rt->slot6.row);
            ok       = apmf::allowance::SeatVerified(findBest, rt->findBest.row) && ok;
            ok       = apmf::allowance::SeatVerified(lookup, rt->lookup.row) && ok;
            if (!rt->xIsLookup) {
                isValid = Resolve(rt->isValid);
                ok      = apmf::allowance::SeatVerified(isValid, rt->isValid.row) && ok;
            }
            // The fitness test's four functions (review H2-1): rows too, and below the scorer's own E8 targets.
            refLoc = Resolve(rt->fit.refLoc);
            inArea = Resolve(rt->fit.inArea);
            cross  = Resolve(rt->fit.cross);
            fleeing = Resolve(rt->fit.fleeing);
            ok     = apmf::allowance::SeatVerified(refLoc, rt->fit.refLoc.row) && ok;
            ok     = apmf::allowance::SeatVerified(inArea, rt->fit.inArea.row) && ok;
            ok     = apmf::allowance::SeatVerified(cross, rt->fit.cross.row) && ok;
            ok     = apmf::allowance::SeatVerified(fleeing, rt->fit.fleeing.row) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            // The bytes NOW (another DLL may have patched a site since the self-check).
            c0 = ProveSite(slot6, rt->c0, findBest);
            x  = ProveSite(findBest, rt->x, rt->xIsLookup ? lookup : isValid);
            f  = ProveSite(findBest, rt->f, lookup);
            if (!c0) why = "the standard selector's call to its scorer is not the verified one (another patcher, or a different build)";
            else if (!x) why = rt->xIsLookup ? "the scorer's scored-path handle lookup is not the verified one (another patcher, or a different build)"
                                             : "the scorer's validity-gate call is not the verified one (another patcher, or a different build)";
            else if (!f) why = "the scorer's fallback-list handle lookup is not the verified one (another patcher, or a different build)";
            else if (std::memcmp(reinterpret_cast<const void*>(findBest + rt->gateOff), rt->gateBytes, kGateLen) != 0)
                why = "the scorer's `no scored pick` test before its fallback list is not the verified one (a different build)";
        }
        if (!why) {
            // The fitness test (review H2-1): the scorer's penalty sites must call exactly the four row-verified
            // functions, load the radius from [[controller+0x10]+0x1B8], compare it with a RIP constant that reads
            // 1024 and ask the candidate's vfunc at +0x4C8 with edx = 0. Nothing is patched here: read-only proof.
            const auto win = [&](const Win& a_w) {
                return std::memcmp(reinterpret_cast<const void*>(findBest + a_w.off), a_w.bytes, a_w.len) == 0;
            };
            const FitSpec& ft = rt->fit;
            if (!ProveSite(findBest, ft.refLocSite, refLoc) || !ProveSite(findBest, ft.inAreaSite, inArea) ||
                !ProveSite(findBest, ft.crossSite, cross) || !ProveSite(findBest, ft.fleeingSite, fleeing))
                why = "a penalty site of the scorer (location / reach / path / fleeing) does not call the verified function (another patcher, or a different build)";
            else if (!win(ft.radius) || !win(ft.dead))
                why = "the scorer's radius load or its dead test is not the verified one (a different build)";
            else if (!win(ft.limit) ||
                     std::memcmp(reinterpret_cast<const void*>(findBest + ft.limit.off + ft.limit.len + 4), kLimitTail, sizeof(kLimitTail)) != 0)
                why = "the scorer's reach-limit compare is not the verified one (a different build)";
            else {
                std::int32_t disp = 0;
                std::memcpy(&disp, reinterpret_cast<const void*>(findBest + ft.limit.off + ft.limit.len), sizeof(disp));
                areaLimit = reinterpret_cast<const float*>(findBest + ft.limit.off + ft.limit.len + 4 + static_cast<std::intptr_t>(disp));
                if (*areaLimit != kAreaLimitWant)
                    why = "the scorer's reach limit does not read 1024 (a different build, or another patcher's data write)";
            }
        }
        if (why) {
            g_whyNot.store(why, std::memory_order_release);
            spdlog::warn("[ch.32] aversion seats NOT installed -- {}. Every kIntent_Aversion claim is refused; the engine "
                         "selects targets as it always does.",
                         why);
            return;
        }
        g_findBest = reinterpret_cast<FindBest_t>(findBest);
        g_isValid  = reinterpret_cast<IsValid_t>(isValid);
        g_lookup   = reinterpret_cast<Lookup_t>(lookup);
        g_refLoc   = reinterpret_cast<RefLoc_t>(refLoc);
        g_inArea   = reinterpret_cast<InArea_t>(inArea);
        g_cross    = reinterpret_cast<Cross_t>(cross);
        g_fleeing   = reinterpret_cast<Fleeing_t>(fleeing);
        g_areaLimit = areaLimit;
        {
            std::random_device rd;
            g_seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ apmf::clock::MonotonicMs();
        }

        static auto* s_tr = new SKSE::Trampoline("APMF.Aversion");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(64);
        if (mem) s_tr->set_trampoline(mem, 64);
        else s_tr->create(64);

        // The originals are the VERIFIED targets (stored above, before any patch). X and F go in first: until C0 is
        // patched no pass is ever active, so they call straight through.
        const std::uintptr_t prevX = rt->xIsLookup
                                         ? s_tr->write_call<5>(x, reinterpret_cast<std::uintptr_t>(&ScoredLookupThunk))
                                         : s_tr->write_call<5>(x, reinterpret_cast<std::uintptr_t>(&IsValidThunk));
        const std::uintptr_t prevF  = s_tr->write_call<5>(f, reinterpret_cast<std::uintptr_t>(&FallbackLookupThunk));
        const std::uintptr_t prevC0 = s_tr->write_call<5>(c0, reinterpret_cast<std::uintptr_t>(&FindBestThunk));
        if (prevX != (rt->xIsLookup ? lookup : isValid) || prevF != lookup || prevC0 != findBest)
            spdlog::error("[ch.32] seat patch: a previous call target changed between the check and the patch (X 0x{:X}, F "
                          "0x{:X}, C0 0x{:X}); the verified functions are called.",
                          prevX, prevF, prevC0);
        using apmf::sources::Facet;
        apmf::sources::RegisterCallSeat(Facet::kTarget, "Aversion.Site.Exclude", x, prevX);
        apmf::sources::RegisterCallSeat(Facet::kTarget, "Aversion.Site.Fallback", f, prevF);
        apmf::sources::RegisterCallSeat(Facet::kTarget, "Aversion.Site.FindBest", c0, prevC0);

        g_installed.store(true, std::memory_order_release);
        g_whyNot.store("installed", std::memory_order_release);
        const auto base = REL::Module::get().base();
        spdlog::info("[ch.32] aversion seats installed on {} (kIntent_Aversion, ABI v41): the standard selector's scorer call "
                     "(RVA 0x{:X}), the scorer's {} (RVA 0x{:X}) and its fallback lookup (RVA 0x{:X}). When a foe's selection "
                     "lands on a claimant whose draw says averted ({} s windows, probability = the claim's weight), the engine's "
                     "own scorer picks again without it; that second answer stands only when it passes the scorer's own reach, "
                     "path, dead and fleeing tests (its four penalty functions, proven at their call sites); with no other "
                     "valid target the pick stands. Nothing is written to the group or the selector.",
                     rt->name, c0 - base, rt->xIsLookup ? "scored-path handle lookup" : "validity gate", x - base, f - base,
                     kWindowMs / 1000);
    }

    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param) {
        if (!Installed()) return NotInstalledReason();
        if (a_actor == 0x14) return "the actor is the player (aversion serves NPCs only)";
        if (!a_param) return "no param (kIntent_Aversion needs RequestEx with param.fval = the weight, inside (0, 1))";
        if (!std::isfinite(a_param->fval) || a_param->fval <= 0.0f || a_param->fval >= 1.0f)
            return "param.fval (the weight) is not inside (0, 1)";
        return nullptr;
    }

    void SetClaimant(RE::FormID a_id, RE::Actor* a_actor, const APMF_API::APMF_Param& a_param, const char* a_what) {
        std::uint32_t h = 0;
        if (a_actor) h = a_actor->GetHandle().native_handle();
        {
            std::unique_lock lk(g_mx);
            if (const auto it = g_byId.find(a_id); it != g_byId.end()) {
                g_byHandle.erase(it->second);
                g_byId.erase(it);
            }
            if (h != 0) {
                g_byHandle[h] = a_id;
                g_byId[a_id]  = h;
            }
            g_count.store(g_byHandle.size(), std::memory_order_relaxed);
        }
        if (h == 0)
            spdlog::warn("[ch.32] 0x{} aversion {} but INERT -- the actor has no reference handle (not loaded). The claim "
                         "stays live and averts nothing until it is engaged again.",
                         apmf::log::Hex(a_id), a_what);
        else
            spdlog::info("[ch.32] 0x{} aversion {}: weight {:.2f}. When a foe's standard target selection lands on this "
                         "actor, another valid target is picked instead in that share of {} s windows.",
                         apmf::log::Hex(a_id), a_what, a_param.fval, kWindowMs / 1000);
    }

    void ClearClaimant(RE::FormID a_id) {
        std::unique_lock lk(g_mx);
        if (const auto it = g_byId.find(a_id); it != g_byId.end()) {
            g_byHandle.erase(it->second);
            g_byId.erase(it);
        }
        g_count.store(g_byHandle.size(), std::memory_order_relaxed);
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        {
            std::unique_lock lk(g_mx);
            n = g_byId.size();
            g_byHandle.clear();
            g_byId.clear();
            g_count.store(0, std::memory_order_relaxed);
        }
        if (n != 0) spdlog::info("[ch.32] {} -- dropped {} aversion entr{}.", a_why, n, n == 1 ? "y" : "ies");
    }

    void Heartbeat() {
        const auto landed = g_landed.load(std::memory_order_relaxed);
        if (landed == g_beatSeen.load(std::memory_order_relaxed)) return;   // no selection landed on a claimant since
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastBeatMs.load(std::memory_order_relaxed) < kHeartbeatMs) return;
        g_lastBeatMs.store(now, std::memory_order_relaxed);   // game-thread-only writer
        g_beatSeen.store(landed, std::memory_order_relaxed);
        spdlog::info("[ch.32] aversion H (scoring passes that picked a claimant, session totals): landed={} averted={} "
                     "kept-by-draw={} kept-no-other-target={} kept-unfit={} kept-engine-outranks={} bad-weight={} errors={} claimants={}",
                     landed, g_averted.load(std::memory_order_relaxed), g_keptDraw.load(std::memory_order_relaxed),
                     g_keptOnly.load(std::memory_order_relaxed), g_keptUnfit.load(std::memory_order_relaxed),
                     g_keptRank.load(std::memory_order_relaxed),
                     g_badWeight.load(std::memory_order_relaxed), g_errors.load(std::memory_order_relaxed),
                     g_count.load(std::memory_order_relaxed));
    }
}
