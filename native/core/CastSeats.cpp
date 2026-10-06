#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/Allowance.h"
#include "core/ControlMap.h"
#include "core/CastSeats.h"
#include "core/RestoreCensus.h"
#include "core/Sightline.h"   // ABI v18 kCastFlag_OwnLineOfSight

// Win32 INI read for the one kill-switch below. Declared by hand, exactly like
// core/AiCastSeats.cpp and core/Hook.cpp do -- PCH does not pull in <Windows.h>,
// and this is the single Win32 call this file needs.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See core/CastSeats.h for the design, the five-seat table and the scope/safety
// argument. This TU is the four caster-vtable thunks.
//
// ── ABI EVIDENCE (verified against the DISASSEMBLED callee, 1.6.1170, not against
// CommonLib's declaration -- the standing rule core/AiCastSeats.cpp's banner
// records after a CommonLib `void*`-shaped `GetMagicTarget` declaration caused a
// live deck CTD) ────────────────────────────────────────────────────────────
//
//   0x06 CheckStartCast     Restore impl 0x81f980
//        `mov rsi,rdx / mov r14,rcx / mov rbx,[rdx+0x18]` -> rcx = this,
//        rdx = CombatController* (its +0x18 blackboard). Returns AL.
//        => bool(CombatMagicCaster*, CombatController*). 2 args, SCALAR return:
//           the Microsoft x64 ABI can never insert a hidden sret slot for it.
//
//   0x07 CheckStopCast      Restore impl 0x81faf0
//        `mov rbp,rdx / mov rsi,rcx / subss xmm6,[rcx+0x20] / mov rax,[rcx+0x10]`
//        -> rcx = this (its +0x20 concentrationCastTimeStamp, +0x10 inventoryItem),
//        rdx = CombatController*. Returns AL.
//        => bool(CombatMagicCaster*, CombatController*). Same shape as 0x06.
//
//   0x0A GetMagicTarget     base impl 0x81e020 (SHARED by 13 of 14 caster vtables)
//        `mov rcx,[rcx+0x18] ... mov rbx,r8 / mov rdi,rdx ... mov [rdi],eax /
//         mov rax,rdi / mov [rdi+8],rcx`
//        -> rcx = this (+0x18 magicItem), rdx = a HIDDEN 16-byte out-slot,
//        r8 = CombatController* (+0xc8 handleCount, +0xd0 cachedAttacker,
//        +0x28 attackerHandle, +0xd8 cachedTarget, +0x2c targetHandle).
//        Writes {u32 handle @+0, Actor* ptr @+8} into the caller's slot and
//        RETURNS that same pointer in rax.
//        => Out16*(CombatMagicCaster*, Out16* out, CombatController*). THREE args.
//        CommonLib declares TWO (`void* GetMagicTarget(CombatController*) const`)
//        and is WRONG; writing a thunk to that shape shifts every argument one
//        register. This is the exact bug that CTD'd the passive probe.
//
//   0x0D SetupAimController base impl 0x81e0b0 = a bare `ret` (all 14 casters
//        share it except Reanimate). No rax written => `void` return, no sret.
//        The caller (the magic-context ctor 0x89eae0) passes the freshly built
//        CombatProjectileAimController in rdx.
//        => void(CombatMagicCaster*, CombatProjectileAimController*).
//
//   Restore caster vtable slot dump (AE 211142) confirming the indices:
//        05 0x81f7b0  06 0x81f980  07 0x81faf0  08 0x81e000  09 0x81e010
//        0A 0x81e020  0B 0x81fc60  0C 0x81fcd0  0D 0x81e0b0
//
// ── THE AIM OVERRIDE FIELD (+0x30) ──────────────────────────────────────────
// `CombatAimController`'s ctor 0x7f51c0 lays out `+0x28 = CombatController*` and
// `+0x30 = 0` (a u32), and its load-game path reads +0x30 back as a serialized
// FormID->ActorHandle. Every aim reader found in 0x7f5000-0x7f9000 -- including
// vfunc7 (the tolerance test, 0x7f6da0: `mov eax,[rcx+0x30]; test eax,eax; jne
// <use it>` else fall back to `ctrl(+0x28)->cachedTarget(+0xd8)/targetHandle`) --
// honours it first. So +0x30 is "aim at THIS actor instead of the combat target".
//
// It is also the ONE field in this file reached by a RAW OFFSET: the pinned
// CommonLib has no `CombatProjectileAimController` class at all (forward
// declaration only), so there is no member to reach through and no build-time
// static_assert to lean on (INVARIANTS #7). It is therefore guarded THREE ways:
//   (1) its own INI kill-switch, `[CastSeats] EnableAimSeat`;
//   (2) an install-time RTTI derivation check on the aim vtable symbol; and
//   (3) a per-call RUNTIME IDENTITY CHECK -- the object's vtable pointer must
//       equal `VTABLE_CombatProjectileAimController`'s resolved address EXACTLY
//       before a single byte is written. A pointer that is anything else (a
//       melee/track/disable aim controller, another mod's subclass, garbage) is
//       left completely untouched.
// This is stronger evidence than a header member would be -- it is the shipped
// binary's own layout, re-checked against the shipped binary's own vtable at
// every call -- but it is still the one place here that would need re-verifying
// on a new runtime, which is why it has a switch and the others do not.
// ============================================================================

namespace apmf::castseats {

    namespace {

        // ---- layout guards. Everything read on the combat thread sits BELOW the
        // AE +0x68 CombatController divergence point (ENGINE_NOTES §0.29).
        // CONFIRMED table (2026-09-15) "MFO layout facts" CombatController row:
        // 0x28/0x2C/0x38 read unshifted on 1.6.1170, 1.5.97 and 1.7.104. ----
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the SE/AE layout "
                      "split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point (0x68)");
        static_assert(offsetof(RE::CombatMagicCaster, magicItem) == 0x18,
                      "CombatMagicCaster::magicItem moved -- re-verify against the pinned header");
        static_assert(offsetof(RE::CombatMagicCasterRestore, primaryAV) == 0x28,
                      "CombatMagicCasterRestore::primaryAV moved -- seat 0x07 reads it to decide "
                      "which actor value the claim's stop-percent is measured against");

        // Vfunc slot indices -- file-scope so Install() and the thunks share one constant.
        constexpr std::size_t kCheckStartCast     = 0x06;
        constexpr std::size_t kCheckStopCast      = 0x07;
        constexpr std::size_t kGetMagicTarget     = 0x0A;
        constexpr std::size_t kSetupAimController = 0x0D;

        // The aim-target override, +0x30 into CombatAimController. See the file banner
        // for the full evidence and the three guards around this one raw offset.
        // RUNTIMES: no version gate, by evidence -- CONFIRMED table (2026-09-15)
        // "MFO layout facts" CombatController row (c): the CombatAimController ctor
        // stores [+0x28]=CombatController* and zeroes [+0x30] on 1.6.1170, 1.5.97
        // AND 1.7.104, and the projectile aim vfunc7 reads [rcx+0x30] first on all
        // three. The VTABLE_CombatProjectileAimController / RTTI_CombatAimController
        // symbols below resolve on SE (vtable id 264187 -> 0x167E2A8; RTTI id
        // 687513) and DerivesFrom holds there ("New for APMF" row 1).
        constexpr std::uintptr_t kAimTargetOverride = 0x30;

        constexpr std::uint64_t kLogThrottleMs = 1500;   // per (actor, subject), matching the probes' cadence

        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_aimSeatArmed{ false };
        // Resolved once at install; 0 == "never write +0x30" (the identity check can
        // then never pass, so the seat self-disables rather than guessing).
        std::atomic<std::uintptr_t> g_aimVtable{ 0 };
        // Resolved once at install (2026-09-06, offense-seat-scope): the ONE
        // caster vtable address whose concrete layout past the shared
        // CombatMagicCaster base is `CombatMagicCasterRestore` -- seat 0x07 casts
        // to that type to read `primaryAV` (static_assert'd offset above), and
        // must never do so for the Offensive caster now also installed below
        // (a DIFFERENT concrete layout at the same base offset). 0 == "never take
        // the Restore-only branch" (harmless: the seat still bounds the channel by
        // claim TTL and target-death, see CheckStopCastThunk).
        std::atomic<std::uintptr_t> g_restoreVtable{ 0 };

        // ---- THE SEATED CASTER TYPES (feat/apmf-buff-summon-seats widened the list from {Restore,
        // Offensive}). Each is a VerifiedAddresses vtable row on 1.6.1170 / 1.5.97 / 1.7.104 and is
        // RTTI-derivation-checked at install (allowance::InstallOnVtables). Slot functions on the
        // three builds (disassembled, agentlog apmf-buff-summon-seats, Docs/VERIFIED-ADDRESSES.md):
        // 0x0A is the SHARED base GetMagicTarget (AE 0x81E020 / SE 0x781CB0 / 1.7.104 0x833510) and
        // 0x0D the shared bare `ret` (0x81E0B0 / 0x781D00 / 0x8335A0) on every type below; 0x06 and
        // 0x07 are per-type, every one `bool(CombatMagicCaster*, CombatController*)` (rdx = the
        // controller, AL returned) on all three. NOT seated: Stagger, Disarm, TargetEffect,
        // Paralyze (hostile, no claim needs them) and Reanimate (its own 0x06 corpse search, 0x0A
        // and 0x0D are what aim it -- a claim's answer would replace the corpse).
        struct SeatType {
            REL::VariantID id;
            const char*    name;
        };
        constexpr std::size_t kSeatTypes = 10;
        const std::array<SeatType, kSeatTypes>& SeatTypes() {
            static const std::array<SeatType, kSeatTypes> s{ {
                { RE::VTABLE_CombatMagicCasterRestore[0], "Restore" },
                { RE::VTABLE_CombatMagicCasterOffensive[0], "Offensive" },
                { RE::VTABLE_CombatMagicCasterWard[0], "Ward" },
                { RE::VTABLE_CombatMagicCasterSummon[0], "Summon" },
                { RE::VTABLE_CombatMagicCasterCloak[0], "Cloak" },
                { RE::VTABLE_CombatMagicCasterLight[0], "Light" },
                { RE::VTABLE_CombatMagicCasterInvisibility[0], "Invisibility" },
                { RE::VTABLE_CombatMagicCasterBoundItem[0], "BoundItem" },
                { RE::VTABLE_CombatMagicCasterArmor[0], "Armor" },
                { RE::VTABLE_CombatMagicCasterScript[0], "Script" },
            } };
            return s;
        }
        // Resolved address per type (set once at install), and PASSIVE per-type counters of the
        // calls a claim ANSWERED, per seat: [type][0x06, 0x07, 0x0A, 0x0D]. Principle 5: the first
        // claim answer on each (type, seat) logs one FIRST line, and SeatCountsLine() feeds the
        // [ctcensus] HEARTBEAT, so a field run shows which of these seats ever execute.
        std::array<std::atomic<std::uintptr_t>, kSeatTypes>                       g_typeVt{};
        std::array<std::array<std::atomic<std::uint32_t>, 4>, kSeatTypes>         g_claimCalls{};
        std::array<std::array<std::atomic<bool>, 4>, kSeatTypes>                  g_firstLogged{};
        std::array<std::atomic<std::uint32_t>, kSeatTypes>                        g_nativePlacement{};

        int TypeIndex(std::uintptr_t a_vt) {
            for (std::size_t i = 0; i < kSeatTypes; ++i)
                if (g_typeVt[i].load(std::memory_order_relaxed) == a_vt) return static_cast<int>(i);
            return -1;
        }

        int SeatIndex(std::uint32_t a_seat) {
            switch (a_seat) {
            case 0x06: return 0;
            case 0x07: return 1;
            case 0x0A: return 2;
            default:   return 3;   // 0x0D
            }
        }

        // ---- one shared throttle table for all four seats (leaf lock, never held
        // across an engine call). Key = (actorFormID << 32 | subjectFormID). ----
        std::mutex                                       g_rlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;

        bool LogDue(RE::FormID a_actor, RE::FormID a_subject, std::uint32_t a_seat) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) |
                             (static_cast<std::uint64_t>(a_subject) ^ (static_cast<std::uint64_t>(a_seat) << 28));
            std::scoped_lock lk(g_rlMx);
            auto& last = g_lastLogMs[key];
            if (now - last < kLogThrottleMs) return false;
            last = now;
            return true;
        }

        // PHASE 0b RESTORE CENSUS (core/RestoreCensus.h). PASSIVE: counts one Restore-caster
        // seat call into an open census window. Restore vtable ONLY (never the Offensive
        // caster these thunks also serve), and a single relaxed pre-gate when no claim
        // window is open, so the common path costs two atomic loads. Changes no answer.
        void CensusNote(std::uintptr_t a_vt, RE::CombatMagicCaster* a_this, RE::CombatController* a_cc,
                        std::uint32_t a_seat, bool a_claimAnswered, bool a_answer) {
            if (a_vt != g_restoreVtable.load(std::memory_order_relaxed) || !apmf::restorecensus::Active()) return;
            if (!a_this || !a_cc) return;
            auto  attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>, refcounted for this scope
            auto* actor  = attPtr.get();
            if (!actor) return;
            apmf::restorecensus::NoteRestoreSeat(actor->GetFormID(), a_this,
                                                 a_this->magicItem ? a_this->magicItem->GetFormID() : 0, a_seat,
                                                 a_claimAnswered, a_answer);
        }

        // ====================================================================
        // THE ONE CLAIM TEST every seat shares.
        //
        // Answers "is THIS Restore caster, on THIS CombatController, the cast a live
        // kIntent_Cast claim named?" -- and nothing else. Two independent gates:
        //   * the deliberating actor (`cc->attackerHandle`) must hold a winning,
        //     unexpired kIntent_Cast claim; and
        //   * `this->magicItem` must be the claim's DRIVEN form.
        //
        // DRIVEN FORM = the claim's `proxy` when one exists, ELSE its `spell`. Not
        // "spell OR proxy": when a delivery-flip proxy was minted, the ORIGINAL kSelf
        // spell must NOT be seat-forced, because the engine's Self branch would land
        // it on the caster no matter what seat 0x0A says. Forcing only the driven form
        // is what makes the ally heal correct rather than a silent self-heal.
        //
        // Also requires a RESOLVABLE target handle: with no target there is nothing to
        // redirect to, and the honest answer is to chain and let the AI be the AI.
        // ====================================================================
        struct SeatMatch {
            RE::FormID              actor  = 0;
            RE::FormID              driven = 0;
            apmf::CastSeatClaim     claim{};
        };

        bool ClaimNamesThisCast(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc, SeatMatch& out) {
            if (!a_this || !a_cc) return false;
            auto* item = a_this->magicItem;
            if (!item) return false;

            auto  attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>, refcounted for this scope
            auto* actor  = attPtr.get();
            if (!actor) return false;

            const RE::FormID fid      = actor->GetFormID();
            const RE::FormID itemForm = item->GetFormID();
            // feat/per-hand-cast-claims: matched by DRIVEN FORM, not "the actor-wide
            // best-basis claim" -- up to two kIntent_Cast claims can be live at once
            // now (one per hand, or a kCastFlag_DualCast claim occupying both), each
            // driving a DIFFERENT CombatMagicCaster instance (one per hand's own
            // magic context). This caster's OWN `magicItem` already tells us exactly
            // which claim (if either) is ours; picking "the highest-basis claim
            // overall" here would answer for the WRONG hand whenever the other hand's
            // claim happens to have the higher basis (the exact bug this pass exists
            // to close). See ControlMap::TryGetCastSeatClaimForForm.
            apmf::CastSeatClaim claim{};
            if (!apmf::ControlMap::Get().TryGetCastSeatClaimForForm(fid, itemForm, claim)) return false;   // no live claim drives THIS item -> chain

            const RE::FormID driven = claim.proxy ? claim.proxy : claim.spell;
            if (driven == 0) return false;                       // degenerate claim -- names nothing
            if (itemForm != driven) return false;                // defensive re-check (should be unreachable: the form query above already matched)
            if (!claim.targetHandle) return false;               // no resolvable target -- nothing to answer

            out.actor  = fid;
            out.driven = driven;
            out.claim  = claim;
            return true;
        }

        // PASSIVE (principle 5): count one claim-answered seat call on this caster type and log the
        // FIRST one per (type, seat) since install. Relaxed atomics only; the one log line is outside
        // any lock. Changes no answer.
        void NoteClaimAnswer(std::uintptr_t a_vt, std::uint32_t a_seat, const SeatMatch& m, const char* a_what) {
            const int t = TypeIndex(a_vt);
            if (t < 0) return;
            const int k = SeatIndex(a_seat);
            g_claimCalls[t][k].fetch_add(1, std::memory_order_relaxed);
            if (g_firstLogged[t][k].exchange(true, std::memory_order_relaxed)) return;
            spdlog::info("[ch.8b seats] FIRST claim answer on the {} caster, seat 0x{:02X}: actor 0x{} driven 0x{} "
                         "target 0x{} -> {}. (Proves this seat executes on this type; counts in the [ctcensus] "
                         "HEARTBEAT.)",
                         SeatTypes()[t].name, a_seat, apmf::log::Hex(m.actor), apmf::log::Hex(m.driven),
                         apmf::log::Hex(m.claim.target), a_what);
        }

        // NATIVE PLACEMENT (feat/apmf-buff-summon-seats, 86e3dvkwm): a claim whose target is the
        // claimant itself on a spell that is NOT kSelf-delivered. ControlMap only resolves that for a
        // SUMMON / REANIMATE spell (castclassify::IsPlacementSpell): their caster type places them,
        // so WHERE stays the engine's (0x0A chains, 0x0D leaves the engine default), while WHETHER and
        // HOW LONG (0x06 / 0x07) answer from the claim. Never aims a ray or a projectile at its own
        // caster (the reason APMF-B39 keeps every other non-kSelf target-0 claim unresolved).
        // GetDelivery is the spell's own virtual (slot 0x57), the same call the base GetMagicTarget
        // makes on this thread.
        bool NativePlacement(RE::CombatMagicCaster* a_this, const SeatMatch& m) {
            return m.claim.target == m.actor && a_this->magicItem &&
                   a_this->magicItem->GetDelivery() != RE::MagicSystem::Delivery::kSelf;
        }

        // ABI v18 kCastFlag_OwnLineOfSight: does this claim ask Harbinger to judge its line of
        // sight, and is there a line to judge? A self target (the claimant's own FormID, APMF-B39)
        // and a deny-only hand claim have none. The verdict is read lock-free from
        // core/Sightline.h's table (FormIDs only, no form lookup), which also marks the pair
        // asked-for so the main thread keeps measuring it while the seats poll.
        bool OwnLosApplies(const SeatMatch& m) {
            return (m.claim.flags & APMF_API::kCastFlag_OwnLineOfSight) != 0 &&
                   (m.claim.flags & APMF_API::kCastFlag_DenyHandOnly) == 0 && m.claim.target != 0 &&
                   m.claim.target != m.actor;
        }

        // ====================================================================
        // SEAT 0x06 -- WHETHER. CombatMagicCasterRestore::CheckStartCast.
        // ====================================================================
        using CheckStartCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_startOrig;

        bool CheckStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_startOrig.find(vt);
            if (oit == g_startOrig.end()) return false;   // foreign vtable -- benign "don't start", touch nothing
            const auto orig = reinterpret_cast<CheckStartCast_t>(oit->second);

            SeatMatch m{};
            if (!ClaimNamesThisCast(a_this, a_cc, m)) {
                const bool native = orig(a_this, a_cc);
                CensusNote(vt, a_this, a_cc, kCheckStartCast, false, native);
                return native;
            }
            // ABI v18 kCastFlag_OwnLineOfSight: the claim asked Harbinger to judge WHETHER the line
            // to its target is open. YES only on a fresh own-ray VISIBLE; anything else (OCCLUDED,
            // UNKNOWN = not measured yet or stale, UNAVAILABLE = the ray could not be cast) is NO
            // for this poll -- the claim stands and the next poll asks again. Never visible by
            // fallback (principle 7).
            if (OwnLosApplies(m)) {
                const auto r = apmf::sightline::Read(m.actor, m.claim.target, true);
                const auto v = apmf::sightline::FreshVerdict(r);
                if (v != APMF_API::kLos_Visible) {
                    CensusNote(vt, a_this, a_cc, kCheckStartCast, true, false);
                    NoteClaimAnswer(vt, kCheckStartCast, m, "NO (own line of sight)");
                    apmf::sightline::NoteSeatHold();
                    if (LogDue(m.actor, m.claim.target, kCheckStartCast))
                        spdlog::info("[ch.8b seat 0x06] 0x{} CheckStartCast -> NO (own line of sight to 0x{} is {}, {}) "
                                     "-- the claim stands; the next poll asks again.",
                                     apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.target),
                                     apmf::sightline::VerdictName(v),
                                     v == APMF_API::kLos_Unknown     ? "not measured yet, or older than 1 s"
                                     : v == APMF_API::kLos_Unsupported ? "the line-of-sight service is not armed"
                                     : r.why != 0                    ? apmf::sightline::WhyName(r.why)
                                                                     : "all 3 rays blocked");
                    return false;
                }
                apmf::sightline::NoteSeatPass();
            }
            CensusNote(vt, a_this, a_cc, kCheckStartCast, true, true);
            NoteClaimAnswer(vt, kCheckStartCast, m, "YES");

            // ANSWER FROM THE CLAIM (the claim is the authority on WHETHER).
            //
            // Chaining here is a real alternative -- with seat 0x0A redirected, the
            // native Restore::CheckStartCast would evaluate the ALLY (its steps 3/4/5
            // all obtain their target through vfunc 0x0A) and say YES whenever
            // ally% < lerp(0, 0.5, attacker.combatStyle.defensiveMult). That is
            // vanilla-EXACT but gambit-INEXACT: a client healing at a 60% threshold
            // gets NO from a native gate whose own ceiling is 50%, and the 15s
            // "MagicRestoreRestrictionTimer" blackboard window would additionally
            // veto re-heals. A claim that says "cast this now" must not be silently
            // overruled by the vanilla thresholds it was written to replace.
            //
            // Nothing dangerous is bypassed by answering YES here:
            //   * MAGICKA is still enforced -- `MagicCaster::CheckCast` runs inside
            //     `MagicCaster::CastSpell`, downstream of this seat (and APMF's own
            //     core/CastGate.cpp rides that same slot).
            //   * HAND READINESS is still enforced -- the cast leaf's own 0x89f3c0
            //     requires `caster->currentSpell == null`, `IsCastingSourceReady`
            //     (the `bMLh_Ready`/`bMRh_Ready` graph bools) and the not-dual check
            //     BEFORE it ever calls this seat.
            //   * The spell must still be EQUIPPED and a magic context built -- seat
            //     0x0F upstream, and that is a real engine equip with its animation.
            // What IS bypassed: the vanilla health threshold, the effect-already-
            // active test and the restrict-timer window. All three are exactly the
            // vanilla policy the claim replaces.
            if (LogDue(m.actor, m.driven, kCheckStartCast))
                spdlog::info("[ch.8b seat 0x06] 0x{} CheckStartCast -> YES (claim spell 0x{}{} target 0x{}) "
                             "-- the claim is the authority; magicka/hand-ready/equip stay engine-enforced.",
                             apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.spell),
                             m.claim.proxy ? " via delivery-flip proxy" : "", apmf::log::Hex(m.claim.target));
            return true;
        }

        // ====================================================================
        // SEAT 0x0A -- WHERE. CombatMagicCaster::GetMagicTarget (base impl, shared).
        //
        // {u32 handle @+0, 4 bytes pad, Actor* ptr @+8} = 16 bytes; the hidden sret
        // out-slot the Microsoft x64 ABI inserts for this aggregate return. ALL 12
        // engine consumers branch on `handle` FIRST and resolve it refcounted through
        // the handle table; `ptr` is read ONLY when `handle == 0`. Handing back a
        // HANDLE is therefore both the version-safe and the LIFETIME-safe form: a
        // despawned/deleted ally simply resolves to null, and every consumer already
        // has a null path (ShouldRestore -> false, effect-active -> false,
        // CheckTargetValid -> false, the fire leaf passes null to CastSpell). No
        // consumer caches the struct past its own call.
        // ====================================================================
        struct Out16 {
            std::uint32_t handle;
            RE::Actor*    ptr;
        };
        // RUNTIMES: no version gate, by evidence -- CONFIRMED table (2026-09-15)
        // "GetMagicTarget sret" row: the hidden out-slot shape is identical on
        // 1.6.1170 (0x81e020), 1.5.97 (0x781CB0, helper 0x782100 fills {u32 @0,
        // ptr @8}) and 1.7.104; base impl shared by 14 of 15 caster vtables on each.
        static_assert(sizeof(Out16) == 16, "Out16 must be exactly 16 bytes -- it IS the engine's hidden "
                                            "sret out-slot; re-verify 0x81e020's tail before trusting this");
        static_assert(offsetof(Out16, ptr) == 8, "Out16::ptr must sit at +0x8 (0x81e020 writes [rdi]=eax "
                                                  "then [rdi+8]=rcx)");

        using GetMagicTarget_t = Out16* (*)(RE::CombatMagicCaster*, Out16*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_targetOrig;

        Out16* GetMagicTargetThunk(RE::CombatMagicCaster* a_this, Out16* a_out, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_targetOrig.find(vt);
            if (oit == g_targetOrig.end()) {
                // Foreign vtable. There is no benign INVENTED answer for an sret return
                // (fabricating one is exactly the class of bug that CTD'd the probe), so
                // recover the LIVE original out of the vtable slot and call it -- a real
                // callable, never a guess. Structurally unreachable: this thunk is only
                // ever entered through a slot this file itself wrote.
                spdlog::error("[ch.8b seat 0x0A] vtable 0x{} not in the recorded set -- recovering the "
                              "LIVE original rather than fabricating a target.", apmf::log::Hex(vt, 16));
                auto live = *reinterpret_cast<GetMagicTarget_t*>(vt + kGetMagicTarget * sizeof(void*));
                return live(a_this, a_out, a_cc);
            }
            const auto orig = reinterpret_cast<GetMagicTarget_t>(oit->second);

            // `a_out` is the ENGINE CALLER's own stack slot -- never one we allocate,
            // and never read before `orig` has filled it. On the chain path it is
            // forwarded through completely unchanged.
            SeatMatch m{};
            if (!a_out || !ClaimNamesThisCast(a_this, a_cc, m)) {
                CensusNote(vt, a_this, a_cc, kGetMagicTarget, false, false);
                return orig(a_this, a_out, a_cc);
            }
            CensusNote(vt, a_this, a_cc, kGetMagicTarget, true, false);

            if (NativePlacement(a_this, m)) {
                const int t = TypeIndex(vt);
                if (t >= 0) g_nativePlacement[t].fetch_add(1, std::memory_order_relaxed);
                NoteClaimAnswer(vt, kGetMagicTarget, m, "native placement (summon / reanimate self claim)");
                return orig(a_this, a_out, a_cc);
            }
            NoteClaimAnswer(vt, kGetMagicTarget, m, "claimed target handle");

            a_out->handle = m.claim.targetHandle.native_handle();
            a_out->ptr    = nullptr;   // handle has precedence at every consumer; ptr is only read when handle==0

            if (LogDue(m.actor, m.driven, kGetMagicTarget))
                spdlog::info("[ch.8b seat 0x0A] 0x{} GetMagicTarget -> claimed target 0x{} (handle 0x{}), "
                             "spell 0x{}{}, {} caster. Seated types only -- Stagger/Disarm/TargetEffect/Paralyze/"
                             "Reanimate share this base impl (or their own) and are never redirected.",
                             apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.target),
                             apmf::log::Hex(a_out->handle), apmf::log::Hex(m.driven),
                             m.claim.proxy ? " (delivery-flip proxy)" : "",
                             TypeIndex(vt) >= 0 ? SeatTypes()[TypeIndex(vt)].name : "?");
            return a_out;
        }

        // ====================================================================
        // SEAT 0x07 -- HOW LONG. CombatMagicCasterRestore::CheckStopCast.
        // Returns TRUE = "stop the channel now".
        // ====================================================================
        using CheckStopCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_stopOrig;

        bool CheckStopCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_stopOrig.find(vt);
            if (oit == g_stopOrig.end()) return true;   // foreign vtable -- benign "stop", touch nothing
            const auto orig = reinterpret_cast<CheckStopCast_t>(oit->second);

            SeatMatch m{};
            if (!ClaimNamesThisCast(a_this, a_cc, m)) return orig(a_this, a_cc);
            NoteClaimAnswer(vt, kCheckStopCast, m, "answered from the claim");

            // ANSWER FROM THE CLAIM. Left native, this seat stops the channel when the
            // target reaches thr+0.25 of the ATTACKER's `defensiveMult`-derived
            // threshold -- so a claim healing from a 60% trigger would get roughly one
            // 0.25s poll of channel and then an InterruptCast, every attempt ("the heal
            // flickers"). The claim owns the duration; the engine still owns everything
            // else about the channel (magicka drain, the aim/LOS re-checks at 0.25s, and
            // the InterruptCast(false) + NotifyStopCast teardown when we say stop).
            //
            // FOUR stop conditions, in the order they can be evaluated cheapest-first:
            //   1. the claim is gone (released, superseded, or TTL-expired) -- handled
            //      ABOVE by ClaimNamesThisCast returning false, which chains to the
            //      native seat; the native seat then evaluates the now-native 0x0A
            //      (the FOE) and stops on its own. Nothing to do here.
            //   2. the hard cap: the claim's own TTL deadline.
            //   3. the target is unresolvable or dead.
            //   4. the target reached the claim's stop percent (CastFlags bits 8-15),
            //      or -- when the client named none -- FULL restoration of the AV this
            //      Restore caster is about. "Full" is the only defensible built-in: it
            //      is the point at which there is provably nothing left to restore.
            const auto nowMs = apmf::clock::MonotonicMs();
            const char* why  = nullptr;

            if (m.claim.expiresMs != 0 && nowMs >= m.claim.expiresMs) {
                why = "claim TTL (hard cap)";
            } else {
                auto  tgtPtr = m.claim.targetHandle.get();   // NiPointer<Actor>
                auto* target = tgtPtr.get();
                if (!target) {
                    why = "target no longer resolves";
                } else if (target->IsDead()) {
                    why = "target is dead";
                } else if (OwnLosApplies(m) && [&m] {
                               // ABI v18: two CONSECUTIVE OCCLUDED measurements (the table's occRun:
                               // readings at most 3 s apart) and the newest one fresh. A single
                               // borderline reading does not cut a beam; UNKNOWN / UNAVAILABLE never
                               // stop a running channel.
                               const auto r = apmf::sightline::Read(m.actor, m.claim.target, true);
                               return r.verdict == APMF_API::kLos_Occluded && r.occRun >= 2 &&
                                      r.ageMs <= APMF_API::kLosFreshMs;
                           }()) {
                    why = "own line of sight OCCLUDED on two consecutive measurements";
                    apmf::sightline::NoteSeatStop();
                } else if (vt == g_restoreVtable.load(std::memory_order_relaxed)) {
                    // RESTORE ONLY (2026-09-06): `primaryAV` is a real member on
                    // `CombatMagicCasterRestore`'s OWN concrete layout past the shared
                    // CombatMagicCaster base -- the vtable-identity check above (not
                    // just the claim match already established by this point) is what
                    // makes this cast safe now that seat 0x07 is ALSO installed on the
                    // Offensive caster, whose concrete layout past that same base
                    // offset is different. An Offensive claim simply has no "restored
                    // AV percent" completion signal to read -- it is bounded above by
                    // claim TTL and target-death only, never by an unbounded read here.
                    if (auto* avo = target->AsActorValueOwner()) {
                        const auto  av   = static_cast<RE::CombatMagicCasterRestore*>(a_this)->primaryAV;
                        const float perm = avo->GetPermanentActorValue(av);
                        if (perm > 0.0f) {
                            const float pct     = avo->GetActorValue(av) / perm;
                            const auto  stopPct = APMF_API::ReadStopPct(m.claim.flags);
                            const float limit   = (stopPct == 0) ? 1.0f : static_cast<float>(stopPct) / 100.0f;
                            if (pct >= limit) why = "target reached the claim's stop percent";
                        }
                    }
                }
            }

            if (why) {
                if (LogDue(m.actor, m.driven, kCheckStopCast))
                    spdlog::info("[ch.8b seat 0x07] 0x{} CheckStopCast -> STOP ({}). Engine teardown is its "
                                 "own: InterruptCast(false) + NotifyStopCast.", apmf::log::Hex(m.actor), why);
                return true;
            }
            return false;   // keep channelling -- the claim still wants this cast
        }

        // ====================================================================
        // SEAT 0x0D -- AIM. CombatMagicCaster::SetupAimController (base = a bare ret).
        //
        // Needed for TWO distinct reasons:
        //   * a kAimed heal (Heal Other) launches a PROJECTILE along the aim, not at
        //     `desiredTarget` -- without this override the healing bolt flies at the
        //     FOE while seat 0x0A only fixes who the effect would have applied to; and
        //   * a kTargetActor CONCENTRATION heal (Healing Hands) is re-checked against
        //     the aim controller's tolerance/LOS at start (0x89f660) and every 0.25s
        //     of channel -- against the FOE unless this is set, so the beam would cut
        //     out whenever the follower faced the ally it is healing.
        //
        // ALWAYS WRITE (the RE notebook's own N2 mitigation). Four aim-controller
        // copy helpers propagate +0x30 field-for-field between controllers, and it was
        // not settled whether a fresh controller can inherit a previous one's value.
        // Writing on EVERY Restore SetupAimController -- the claim's target when a
        // claim stands, and the engine's OWN ctor default (0 = "aim at the combat
        // target") when it does not -- makes that question moot: this seat can never
        // leave a stale override behind, and the no-claim write can only ever restore
        // a value the engine itself wrote one instant earlier. The no-claim branch is
        // additionally skipped when the field is already 0, so the common case is a
        // pure read.
        // ====================================================================
        using SetupAimController_t = void (*)(RE::CombatMagicCaster*, void*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_aimOrig;

        void SetupAimControllerThunk(RE::CombatMagicCaster* a_this, void* a_aim) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_aimOrig.find(vt);
            if (oit == g_aimOrig.end()) return;   // foreign vtable -- the base impl is a bare ret anyway
            const auto orig = reinterpret_cast<SetupAimController_t>(oit->second);

            orig(a_this, a_aim);   // ENGINE FIRST (the Restore base is a bare `ret`; Reanimate's is not,
                                   // and a future engine/mod override must still run before we touch it)

            if (!a_aim || !g_aimSeatArmed.load(std::memory_order_relaxed)) return;

            // GUARD 3 of 3 for the one raw offset in this file (see the file banner):
            // the object's vtable pointer must be EXACTLY
            // VTABLE_CombatProjectileAimController. Anything else -- a melee/track/
            // disable aim controller, another mod's subclass, a corrupted pointer -- is
            // left untouched. Never a blind write.
            const auto expected = g_aimVtable.load(std::memory_order_relaxed);
            if (expected == 0 || *reinterpret_cast<std::uintptr_t*>(a_aim) != expected) return;

            auto* slot = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_aim) +
                                                          kAimTargetOverride);

            SeatMatch m{};
            // The aim controller carries its own CombatController at +0x28 (ctor
            // 0x7f51c0), but this seat is handed no controller argument, so the claim
            // test is done off the caster's OWN inventory item route instead: the magic
            // context ctor builds the aim controller for the SAME item this caster was
            // created from, and ClaimNamesThisCast only needs a CombatController to
            // resolve the deliberating actor. Read it from the aim controller's own
            // +0x28 -- the field the ctor disassembly shows being written from rdx, and
            // the same one every aim reader falls back through. Guarded by the vtable
            // identity check above, so this is never a blind read either.
            auto* cc = *reinterpret_cast<RE::CombatController**>(
                reinterpret_cast<std::uintptr_t>(a_aim) + 0x28);

            const bool claimed = ClaimNamesThisCast(a_this, cc, m);
            if (claimed) NoteClaimAnswer(vt, kSetupAimController, m, NativePlacement(a_this, m) ? "engine default (native placement)" : "aim override");
            if (claimed && !NativePlacement(a_this, m)) {
                const std::uint32_t h = m.claim.targetHandle.native_handle();
                if (*slot != h) {
                    *slot = h;
                    if (LogDue(m.actor, m.driven, kSetupAimController))
                        spdlog::info("[ch.8b seat 0x0D] 0x{} aim override -> claimed target 0x{} "
                                     "(handle 0x{}). A kAimed heal now flies at the ally, and a "
                                     "concentration heal's tolerance/LOS re-checks track it.",
                                     apmf::log::Hex(m.actor), apmf::log::Hex(m.claim.target),
                                     apmf::log::Hex(h));
                }
            } else if (*slot != 0) {
                // No claim on this cast (or a native-placement claim, which keeps the engine's own
                // aim): restore the engine's own ctor default, so this
                // seat can never leave one of ITS OWN overrides on a later controller
                // (the N2 staleness question). Never writes anything the engine would
                // not itself have written.
                *slot = 0;
            }
        }

        bool ReadIniFlag(const char* a_key, long a_default) {
            return GetPrivateProfileIntA("CastSeats", a_key, a_default, "Data/SKSE/Plugins/APMF.ini") != 0;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ch.8b seats] VR runtime -- the CombatMagicCaster vtable indices are SE/AE-only "
                         "verified; the engine cast seats were NOT installed. A kIntent_Cast claim on VR "
                         "stays a pure arbitration+deny claim (the AI casts its own choice).");
            return;
        }
        if (!allowance::RuntimeSupported()) {   // G1: exact build, never the 1.6 path by bucket
            spdlog::error("[ch.8b seats] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- the CombatMagicCaster "
                          "slot indices and the aim +0x30 override are verified on those three only; the engine "
                          "cast seats were NOT installed (REFUSED). A kIntent_Cast claim stays a pure "
                          "arbitration+deny claim (the AI casts its own choice).",
                          REL::Module::get().version().string("."));
            return;
        }
        if (g_installed.exchange(true)) return;

        // SCOPE (2026-09-06, offense-seat-scope): the Restore AND Offensive caster
        // vtables, NOTHING else. `GetMagicTarget`'s implementation is the BASE,
        // shared by 13 of the 14 caster vtables -- installing on the OTHER 12
        // (Stagger, Disarm, Reanimate, ...) would let a claim aim THEIR effects at
        // the ally. Offensive is added here because a claimed HOSTILE spell (e.g.
        // Firebolt) classifies into the Offensive caster, never Restore, so without
        // a seat there a claim on it is inert -- exactly the field-diagnosed bug
        // this pass fixes. Safe ONLY because every thunk below independently
        // re-tests the DRIVEN FORM (`this->magicItem == the claim's proxy-or-spell`)
        // for the deliberating actor on EVERY call (see ClaimNamesThisCast above) --
        // an Offensive caster that is not the claim's is untouched; it chains.
        // RTTI-verified (`DerivesFrom`) exactly like every other seat in this
        // codebase; a symbol that does not derive CombatMagicCaster is skipped,
        // never hooked blind (the CombatMagicCasterArmor lesson, #17).
        //
        // WIDENED 2026-10-05 (feat/apmf-buff-summon-seats, batch A): + Ward, Summon, Cloak, Light,
        // Invisibility, BoundItem, Armor and Script (SeatTypes() above). A claimed self buff keys its
        // own row (Oakflesh -> Armor, Candlelight -> Light, a ward -> Ward, a cloak -> Cloak, a bound
        // weapon -> BoundItem, Invisibility), a buff at ANOTHER actor keys the same row through seat 0's
        // self flip on its delivery-flip proxy, a summon keys Summon, and a rowless spell keys Script
        // through seat 0's row substitution. On those casters the shared base 0x0A aims a non-kSelf
        // form at the COMBAT TARGET (the foe) and 0x06 is the type's own native "should I" -- so a
        // claim on any of them was inert or mis-aimed without these seats. The rule stays "no vtable
        // a live claim does not need": still never Stagger, Disarm, TargetEffect, Paralyze, Reanimate.
        // The same two gates make each one safe (RTTI derivation at install; the DRIVEN-FORM match
        // for the deliberating actor on every call).
        REL::Relocation<void*> casterTD{ RE::RTTI_CombatMagicCaster };
        std::array<REL::VariantID, kSeatTypes> kEngineSeatVtables{};
        for (std::size_t i = 0; i < kSeatTypes; ++i) {
            kEngineSeatVtables[i] = SeatTypes()[i].id;
            REL::Relocation<std::uintptr_t> tv{ SeatTypes()[i].id };
            g_typeVt[i].store(tv.address(), std::memory_order_relaxed);   // label + counters only
        }

        // Resolved once here (not just installed on) so seat 0x07 can identity-check
        // it per call before ever reading CombatMagicCasterRestore::primaryAV -- that
        // member does not exist at the same layout on the Offensive caster now also
        // installed below. See g_restoreVtable's declaration and CheckStopCastThunk.
        REL::Relocation<std::uintptr_t> restoreVt{ RE::VTABLE_CombatMagicCasterRestore[0] };
        g_restoreVtable.store(restoreVt.address(), std::memory_order_relaxed);

        const int nStart = allowance::InstallOnVtables(kEngineSeatVtables, kCheckStartCast, &CheckStartCastThunk,
                                                        casterTD.get(), "ch.8b-seat06", g_startOrig);
        const int nStop  = allowance::InstallOnVtables(kEngineSeatVtables, kCheckStopCast, &CheckStopCastThunk,
                                                        casterTD.get(), "ch.8b-seat07", g_stopOrig);
        const int nTgt   = allowance::InstallOnVtables(kEngineSeatVtables, kGetMagicTarget, &GetMagicTargetThunk,
                                                        casterTD.get(), "ch.8b-seat0A", g_targetOrig);

        // Seat 0x0D: armed by default, killable from the INI (see the header). It is
        // the only seat that writes a raw offset, so it also resolves + RTTI-verifies
        // the aim vtable HERE; a failed derivation leaves g_aimVtable at 0, which makes
        // the per-call identity check unpassable and the seat inert -- never a blind write.
        int nAim = 0;
        if (ReadIniFlag("EnableAimSeat", 1)) {
            REL::Relocation<std::uintptr_t> aimVt{ RE::VTABLE_CombatProjectileAimController[0] };
            REL::Relocation<void*>          aimTD{ RE::RTTI_CombatAimController };
            if (allowance::SeatVerified(aimVt.address(), "CastSeats.CombatProjectileAimController") &&
                allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(aimTD.get()), "CastSeats.RTTI.CombatAimController") &&
                allowance::DerivesFrom(aimVt.address(), aimTD.get())) {
                g_aimVtable.store(aimVt.address(), std::memory_order_relaxed);
                nAim = allowance::InstallOnVtables(kEngineSeatVtables, kSetupAimController, &SetupAimControllerThunk,
                                                    casterTD.get(), "ch.8b-seat0D", g_aimOrig);
                g_aimSeatArmed.store(nAim > 0, std::memory_order_relaxed);
            } else {
                spdlog::warn("[ch.8b seat 0x0D] VTABLE_CombatProjectileAimController does not derive "
                             "CombatAimController on this runtime -- the aim seat is NOT installed (a "
                             "kAimed heal will fly at the AI's own aim target). Documented gap, never a "
                             "blind +0x30 write.");
            }
        } else {
            spdlog::info("[ch.8b seat 0x0D] disabled by [CastSeats] EnableAimSeat=0 -- kAimed heals will "
                         "fly at the AI's own aim target and a concentration heal's LOS/tolerance "
                         "re-checks will track the combat target.");
        }

        spdlog::info("[ch.8b seats] engine cast seats installed on {} caster vtables (Restore, Offensive, Ward, "
                     "Summon, Cloak, Light, Invisibility, BoundItem, Armor, Script; never Stagger/Disarm/"
                     "TargetEffect/Paralyze/Reanimate): 0x06 CheckStartCast {}/{}, 0x07 CheckStopCast {}/{}, "
                     "0x0A GetMagicTarget {}/{}, 0x0D SetupAimController {}/{}. While a kIntent_Cast claim "
                     "stands, the NPC's OWN AI casts the claimed spell at the claimed target -- APMF makes "
                     "no equip, anim or cast write of any kind.",
                     kSeatTypes, nStart, kSeatTypes, nStop, kSeatTypes, nTgt, kSeatTypes, nAim, kSeatTypes);
    }

    std::string SeatCountsLine() {
        std::string s;
        for (std::size_t t = 0; t < kSeatTypes; ++t) {
            const auto a = g_claimCalls[t][0].load(std::memory_order_relaxed);
            const auto b = g_claimCalls[t][1].load(std::memory_order_relaxed);
            const auto c = g_claimCalls[t][2].load(std::memory_order_relaxed);
            const auto d = g_claimCalls[t][3].load(std::memory_order_relaxed);
            const auto n = g_nativePlacement[t].load(std::memory_order_relaxed);
            s += fmt::format("{}{}={}/{}/{}/{}", t ? "," : "", SeatTypes()[t].name, a, b, c, d);
            if (n) s += fmt::format("(native-placement {})", n);
        }
        return s;
    }

}
