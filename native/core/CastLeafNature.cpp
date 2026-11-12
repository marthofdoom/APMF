#include "PCH.h"
#include "core/CastLeafNature.h"
#include "core/Allowance.h"   // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"   // TryGetCastSeatClaimForForm (Q91 own-claim pass)
#include "core/Sources.h"      // ClientForHandle (same-client test)
#include "core/Log.h"

#include <array>
#include <cstring>

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// ch.7 HOSTILE OFFENSE -- which spell a cast leaf is about to cast, and its nature (ABI v40).
//
// THE RULING (marth 2026-11-11, on the G3 flee whose Offense deny ate the follower's own heals: "wouldn't it be
// non offense cast"). kCombatActionCat_Offense means HOSTILE actions. A beneficial cast (heal, buff, ward, on
// self or an ally) is not Offense and passes an Offense deny. kCombatActionCat_Cast still denies every cast.
//
// THE ENGINE PATH (read on the unpacked 1.6.1170 / 1.5.97 executables and the plaintext 1.7.104 one; agentlog
// move-batch.md "CH7 HOSTILE OFFENSE"). The four cast leaves' act() (slot 0x02) pushes the leaf's frame and
// tail-jumps to the leaf's Enter. Every Enter starts by reading the thread's CONTEXT WINDOW, the same read on
// all three builds (`movups xmm0,[thread+0x128]`: a data-stack header pointer at +0x128 and a u32 offset at
// +0x130; context = header->base + offset), and treats it as the magic context its ancestor context node built:
//     context+0x00  MagicCaster*        the actor's hand caster (state +0x30, currentSpell +0x28)
//     context+0x08  MagicItem*          THE SPELL: the shared cast helper passes it to the caster's cast call
//                                       (`mov rdx,[ctx+8]; mov rcx,[ctx]; call`) and then asks it
//                                       `GetCastingType() == kConcentration` (`call [vtbl+0x2A8]; cmp eax,2`);
//                                       PrepareDualCast asks it the same question in its own Enter
//     context+0x10  CombatMagicCaster*
//     context+0x20  TESShout*           (the shout leaf's context) the shout cast helper picks a variation of it
//                                       (`mov rcx,[ctx+0x20]; call`) and hands it to the actor's shout call
//                                       (`mov rdx,[ctx+0x20]`), which casts variations[i].spell (shout+0x68+i*0x18)
// Functions (AE id / SE id / 1.7.104 raw RVA; rows "CastNature.*" in native/VerifiedAddresses.h):
//     Enter.Immediate      49100 0x89FCE0 / 48053 0x8094D0 / 0x8B5180
//     Enter.Concentration  49105 0x89FFF0 / 48058 0x8097D0 / 0x8B5490
//     Enter.DualCast       49110 0x8A02D0 / 48063 0x809AB0 / 0x8B5770
//     Enter.Shout          49764 0x8BBB70 / 48809 0x825290 / 0x8D1150
//     Cast   (helper)      49083 0x89EE30 / 48037 0x8085E0 / 0x8B42D0
//     ShoutCast (helper)   49758 0x8BB660 / 48803 0x824D00 / 0x8D0C40
// None of the six ids is in the fork's MIT id table for 1.7.104, so they are placed by raw RVA there (rows
// "rva:0x..." in the 1.7.104 table, generator-proven: the 1.6.1170 signature hits the claimed RVA alone).
//
// THE READ. PassesOffenseDeny runs in the leaf's act() thunk BEFORE the leaf's own act: the window is not
// touched by the leaf's push, so these are the bytes the leaf reads a few instructions later. Nothing is
// hooked, called or written here: the proof below only licenses READING context+0x08 / +0x20 as what the
// engine says they are. Bounded by the data-stack top (header+0x0C), as [Probe] bRangedSelect's read is.
//
// THE PROOF (Install; principle 11, each runtime its own values). Per leaf, all of:
//   * every function's address is a verified row (allowance::SeatVerified);
//   * the Enter's window read, byte for byte, ending with the context in the register the next check names;
//   * (Immediate / Concentration) `mov rcx,<ctx>; call` at the known offset lands on the verified cast helper,
//     and the helper still holds `mov <reg>,rcx` and the two spell reads of [<reg>+8];
//   * (DualCast) its own `mov rcx,[rbx+8]; mov rax,[rcx]; call [rax+0x2A8]; cmp eax,2`;
//   * (Shout) `mov rcx,<ctx>; call` lands on the verified shout cast helper, which still holds
//     `mov r14,rcx`, `mov rcx,[r14+0x20]; call` and `mov rdx,[r14+0x20]`.
// A leaf that fails any of these is REFUSED BY NAME and keeps the pre-v40 behaviour: every cast of that leaf
// counts as Offense (a deny that is never weaker than declared; the log says so).
//
// THE NATURE RULE (form data only). BENEFICIAL IS AN ALLOW-LIST (tier-3 review F1, 2026-11-11: the first
// rule read "everything not hostile is beneficial" and let a Self script-only spell and an unflagged Stagger /
// Absorb through; INVARIANTS #17b(2) says an undeclared nature counts as hostile).
//   HOSTILE     any effect whose base effect is flagged Hostile or Detrimental; an enemy-control archetype
//               (Calm, Frenzy, Demoralize, Paralysis, TurnUndead: marth 2026-11-09, hostile even when the
//               record leaves the flag unset); a Cloak effect whose associated spell has such an effect.
//   BENEFICIAL  at least one readable effect, and EVERY effect is on the benign archetype list with neither
//               flag: ValueModifier, PeakValueModifier, DualValueModifier, AccumulateMagnitude, Invisibility,
//               Light, CureDisease, CureParalysis, CureAddiction, CurePoison, BoundWeapon, Rally, SlowTime,
//               Etherealize, DetectLife, Guide, EnhanceWeapon, SummonCreature and Reanimate (marth 2026-11-11:
//               summons are not offense), and a Cloak whose associated spell is itself BENEFICIAL by this
//               rule. Passes an Offense-only deny.
//   UNDECLARED  everything else: no readable effect, ANY Script effect on any delivery (the record defers to
//               Papyrus), or any archetype off the list (Stagger, Absorb, Disarm, SpawnHazard, Telekinesis,
//               Banish, SoulTrap, GrabActor, Concussion, SpawnScriptedRef, CommandSummoned, Dispel, ...).
//               COUNTS AS HOSTILE for the deny, logged per form at warn.
// KNOWN RESIDUAL (not closable from record data; DENY-COMPLETENESS-AUDIT row 14 lists it): a value modifier
// with neither flag that harms through its target or its magnitude script reads exactly like a heal-other.
// A shout is judged over ALL its variations that carry a spell: one hostile or undeclared word makes the shout
// hostile, so the answer never depends on which word the engine picks.
// ============================================================================

namespace apmf::castleafnature {

    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // Thread context window + the two context slots (identical on all three builds; see the proof).
        constexpr std::size_t kThreadWindowPtr = 0x128;
        constexpr std::size_t kThreadWindowOff = 0x130;
        constexpr std::size_t kHeaderTop       = 0x0C;
        constexpr std::size_t kCtxSpell        = 0x08;
        constexpr std::size_t kCtxShout        = 0x20;

        struct Window {
            std::uintptr_t      off;
            const std::uint8_t* bytes;
            std::size_t         len;
        };
        struct Fn {
            const char*    row;   // VerifiedAddresses row label
            std::uint64_t  id;    // 0 = raw RVA (1.7.104)
            std::uintptr_t rva;   // raw RVA when the id is 0
        };

        // ---- byte windows, each read off that runtime's executable -------------------------------------------
        // Enter.Immediate / Enter.Concentration: movups xmm0,[rax+0x128]; movq rax,xmm0; movups [rsp+0x20],xmm0;
        // test rax,rax; je +9; mov ebx,[rsp+0x28]; add rbx,[rax]; jmp +2; xor ebx,ebx   -> context in rbx
        constexpr std::uint8_t kWinRbx[] = { 0x0F, 0x10, 0x80, 0x28, 0x01, 0x00, 0x00, 0x66, 0x48, 0x0F, 0x7E,
                                             0xC0, 0x0F, 0x11, 0x44, 0x24, 0x20, 0x48, 0x85, 0xC0, 0x74, 0x09,
                                             0x8B, 0x5C, 0x24, 0x28, 0x48, 0x03, 0x18, 0xEB, 0x02, 0x33, 0xDB };
        constexpr std::uint8_t kCallRbx[] = { 0x48, 0x8B, 0xCB, 0xE8 };   // mov rcx,rbx; call
        constexpr std::uint8_t kCallRbp[] = { 0x48, 0x8B, 0xCD, 0xE8 };   // mov rcx,rbp; call
        // Enter.DualCast -> context in rbx
        constexpr std::uint8_t kWinDualAE[] = { 0x0F, 0x10, 0x80, 0x28, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x44, 0x24,
                                                0x58, 0x45, 0x33, 0xE4, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x48, 0x85,
                                                0xC0, 0x74, 0x09, 0x8B, 0x5C, 0x24, 0x60, 0x48, 0x03, 0x18 };
        constexpr std::uint8_t kWinDualSE[] = { 0x0F, 0x10, 0x80, 0x28, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x44, 0x24,
                                                0x28, 0x33, 0xED, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x48, 0x85, 0xC0,
                                                0x74, 0x09, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x03, 0x18, 0xEB, 0x03,
                                                0x48, 0x8B, 0xDD };
        // mov rcx,[rbx+8]; mov rax,[rcx]; call [rax+0x2A8]; cmp eax,2   (MagicItem::GetCastingType, slot 0x55)
        constexpr std::uint8_t kDualSpell[] = { 0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8B, 0x01, 0xFF,
                                                0x90, 0xA8, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x02 };
        // Enter.Shout -> context in rbp (AE / 1.7.104), rbx (SE)
        constexpr std::uint8_t kWinShoutAE[] = { 0x0F, 0x10, 0x80, 0x28, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x44, 0x24,
                                                 0x58, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x48, 0x85, 0xC0, 0x74, 0x09,
                                                 0x8B, 0x6C, 0x24, 0x60, 0x48, 0x03, 0x28, 0xEB, 0x03 };
        constexpr std::uint8_t kWinShoutSE[] = { 0x0F, 0x10, 0x80, 0x28, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x44,
                                                 0x24, 0x68, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x48, 0x85, 0xC0,
                                                 0x74, 0x07, 0x8B, 0x5C, 0x24, 0x70, 0x48, 0x03, 0x18 };
        // Cast helper: mov <ctx>,rcx; then mov rdx,[ctx+8]; mov rcx,[ctx]; call; and
        // mov rcx,[ctx+8]; mov rax,[rcx]; call [rax+0x2A8]; cmp eax,2.   ctx = r14 (AE / 1.7.104), r15 (SE)
        constexpr std::uint8_t kCastRegAE[]  = { 0x4C, 0x8B, 0xF1 };
        constexpr std::uint8_t kCastRegSE[]  = { 0x4C, 0x8B, 0xF9 };
        constexpr std::uint8_t kCastArgAE[]  = { 0x49, 0x8B, 0x56, 0x08, 0x49, 0x8B, 0x0E, 0xE8 };
        constexpr std::uint8_t kCastArgSE[]  = { 0x49, 0x8B, 0x57, 0x08, 0x49, 0x8B, 0x0F, 0xE8 };
        constexpr std::uint8_t kCastTypeAE[] = { 0x49, 0x8B, 0x4E, 0x08, 0x48, 0x8B, 0x01, 0xFF,
                                                 0x90, 0xA8, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x02 };
        constexpr std::uint8_t kCastTypeSE[] = { 0x49, 0x8B, 0x4F, 0x08, 0x48, 0x8B, 0x01, 0xFF,
                                                 0x90, 0xA8, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x02 };
        // ShoutCast helper: mov r14,rcx; mov rcx,[r14+0x20]; call; mov rdx,[r14+0x20]   (same bytes on all three)
        constexpr std::uint8_t kShoutReg[]  = { 0x4C, 0x8B, 0xF1 };
        constexpr std::uint8_t kShoutPick[] = { 0x49, 0x8B, 0x4E, 0x20, 0xE8 };
        constexpr std::uint8_t kShoutArg[]  = { 0x49, 0x8B, 0x56, 0x20 };

        struct RtSpec {
            const char* name;
            Fn          enterImm, enterConc, enterDual, enterShout, cast, shoutCast;
            Window      immWin, immCall;       // immCall ends with the E8 whose target must be `cast`
            Window      concWin, concCall;
            Window      dualWin, dualSpell;
            Window      shoutWin, shoutCall;   // shoutCall ends with the E8 whose target must be `shoutCast`
            Window      castReg, castArg, castType;
            Window      scReg, scPick, scArg;
        };
        constexpr RtSpec kAE{
            "1.6.1170",
            { "CastNature.Enter.Immediate", 49100, 0 },     { "CastNature.Enter.Concentration", 49105, 0 },
            { "CastNature.Enter.DualCast", 49110, 0 },      { "CastNature.Enter.Shout", 49764, 0 },
            { "CastNature.Cast", 49083, 0 },                { "CastNature.ShoutCast", 49758, 0 },
            { 0x30, kWinRbx, sizeof(kWinRbx) },             { 0x73, kCallRbx, sizeof(kCallRbx) },
            { 0x33, kWinRbx, sizeof(kWinRbx) },             { 0x96, kCallRbx, sizeof(kCallRbx) },
            { 0x49, kWinDualAE, sizeof(kWinDualAE) },       { 0x19F, kDualSpell, sizeof(kDualSpell) },
            { 0xC5, kWinShoutAE, sizeof(kWinShoutAE) },     { 0x219, kCallRbp, sizeof(kCallRbp) },
            { 0x29, kCastRegAE, sizeof(kCastRegAE) },       { 0x14C, kCastArgAE, sizeof(kCastArgAE) },
            { 0x160, kCastTypeAE, sizeof(kCastTypeAE) },
            { 0x20, kShoutReg, sizeof(kShoutReg) },         { 0xDB, kShoutPick, sizeof(kShoutPick) },
            { 0xEA, kShoutArg, sizeof(kShoutArg) } };
        constexpr RtSpec kSE{
            "1.5.97",
            { "CastNature.Enter.Immediate", 48053, 0 },     { "CastNature.Enter.Concentration", 48058, 0 },
            { "CastNature.Enter.DualCast", 48063, 0 },      { "CastNature.Enter.Shout", 48809, 0 },
            { "CastNature.Cast", 48037, 0 },                { "CastNature.ShoutCast", 48803, 0 },
            { 0x30, kWinRbx, sizeof(kWinRbx) },             { 0x73, kCallRbx, sizeof(kCallRbx) },
            { 0x33, kWinRbx, sizeof(kWinRbx) },             { 0x96, kCallRbx, sizeof(kCallRbx) },
            { 0x3C, kWinDualSE, sizeof(kWinDualSE) },       { 0x93, kDualSpell, sizeof(kDualSpell) },
            { 0xD8, kWinShoutSE, sizeof(kWinShoutSE) },     { 0x18D, kCallRbx, sizeof(kCallRbx) },
            { 0x2F, kCastRegSE, sizeof(kCastRegSE) },       { 0x157, kCastArgSE, sizeof(kCastArgSE) },
            { 0x16C, kCastTypeSE, sizeof(kCastTypeSE) },
            { 0x24, kShoutReg, sizeof(kShoutReg) },         { 0xFA, kShoutPick, sizeof(kShoutPick) },
            { 0x109, kShoutArg, sizeof(kShoutArg) } };
        // 1.7.104: raw RVAs (no MIT-table row for these ids); offsets and bytes are the 1.6.1170 ones, re-read
        // on the 1.7.104 executable (identical windows at identical offsets in all six functions).
        constexpr RtSpec k17{
            "1.7.104",
            { "CastNature.Enter.Immediate", 0, 0x8B5180 },  { "CastNature.Enter.Concentration", 0, 0x8B5490 },
            { "CastNature.Enter.DualCast", 0, 0x8B5770 },   { "CastNature.Enter.Shout", 0, 0x8D1150 },
            { "CastNature.Cast", 0, 0x8B42D0 },             { "CastNature.ShoutCast", 0, 0x8D0C40 },
            { 0x30, kWinRbx, sizeof(kWinRbx) },             { 0x73, kCallRbx, sizeof(kCallRbx) },
            { 0x33, kWinRbx, sizeof(kWinRbx) },             { 0x96, kCallRbx, sizeof(kCallRbx) },
            { 0x49, kWinDualAE, sizeof(kWinDualAE) },       { 0x19F, kDualSpell, sizeof(kDualSpell) },
            { 0xC5, kWinShoutAE, sizeof(kWinShoutAE) },     { 0x219, kCallRbp, sizeof(kCallRbp) },
            { 0x29, kCastRegAE, sizeof(kCastRegAE) },       { 0x14C, kCastArgAE, sizeof(kCastArgAE) },
            { 0x160, kCastTypeAE, sizeof(kCastTypeAE) },
            { 0x20, kShoutReg, sizeof(kShoutReg) },         { 0xDB, kShoutPick, sizeof(kShoutPick) },
            { 0xEA, kShoutArg, sizeof(kShoutArg) } };

        // TESShout::variations is read through CommonLib; the engine's own read is shout+0x68+i*0x18 (the spell
        // of variation i) on all three builds (AE 38804 +0x4E, SE 37828's twin 0x634110 +0x4E, 1.7.104 0x6D8DF0 +0x4E).
        static_assert(offsetof(RE::TESShout, variations) == 0x60, "TESShout::variations moved -- re-measure the shout call");
        static_assert(offsetof(RE::TESShout::Variation, spell) == 0x08, "TESShout::Variation::spell moved");
        static_assert(sizeof(RE::TESShout::Variation) == 0x18, "TESShout::Variation size changed");

        // ---- state ---------------------------------------------------------------------------------------------
        // Written once by Install (game thread, before ActionGate hooks a single vtable), read-only after.
        bool g_immOk = false, g_concOk = false, g_dualOk = false, g_shoutOk = false;
        std::atomic<bool> g_installed{ false };

        std::atomic<std::uint64_t> g_passed{ 0 };       // beneficial: passed an Offense-only deny
        std::atomic<std::uint64_t> g_hostile{ 0 };      // hostile: the deny stands
        std::atomic<std::uint64_t> g_undeclared{ 0 };   // undeclared nature: counted as hostile
        std::atomic<std::uint64_t> g_unreadable{ 0 };   // window / pointer unreadable: counted as hostile
        std::atomic<std::uint64_t> g_errors{ 0 };       // a throw inside: counted as hostile
        std::atomic<std::uint64_t> g_lastBeatMs{ 0 };
        std::atomic<std::uint64_t> g_beatSeen{ 0 };     // total verdicts at the last heartbeat

        constexpr std::uint64_t kLineMs      = 10000;   // per {actor, form, verdict} line cadence
        constexpr std::uint64_t kHeartbeatMs = 30000;
        constexpr std::size_t   kLineKeysMax = 4096;    // the rate-limit map is cleared past this (lines repeat, never lost)
        std::mutex                                       g_lineMx;   // leaf lock: no engine call, no log under it
        std::unordered_map<std::uint64_t, std::uint64_t> g_lineLast;

        bool LineDue(RE::FormID a_actor, RE::FormID a_form, std::uint32_t a_verdict) {
            const std::uint64_t key = (static_cast<std::uint64_t>(a_actor) << 32) ^
                                      (static_cast<std::uint64_t>(a_form) << 2) ^ a_verdict;
            const auto       now = apmf::clock::MonotonicMs();
            std::scoped_lock lk(g_lineMx);
            if (g_lineLast.size() > kLineKeysMax) g_lineLast.clear();
            auto& last = g_lineLast[key];
            if (last != 0 && now - last < kLineMs) return false;
            last = now ? now : 1;
            return true;
        }

        bool BytesAt(std::uintptr_t a_fn, const Window& a_w) {
            return std::memcmp(reinterpret_cast<const void*>(a_fn + a_w.off), a_w.bytes, a_w.len) == 0;
        }
        // `a_call` ends with an E8: true when that call lands on a_target.
        bool CallLandsOn(std::uintptr_t a_fn, const Window& a_call, std::uintptr_t a_target) {
            const std::uintptr_t e8 = a_fn + a_call.off + a_call.len - 1;
            std::int32_t         rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(e8 + 1), sizeof(rel));
            return e8 + 5 + static_cast<std::intptr_t>(rel) == a_target;
        }
        std::uintptr_t Resolve(const Fn& a_fn) {
            return a_fn.id ? REL::ID(a_fn.id).address() : REL::Module::get().base() + a_fn.rva;
        }

        // ---- the nature rule (immutable form data; any thread) ---------------------------------------------------
        enum class Nature : std::uint8_t { kHostile, kBeneficial, kUndeclared };

        bool HostileEffect(const RE::EffectSetting* a_be) {
            if (a_be->IsHostile() || a_be->IsDetrimental()) return true;
            using Arch = RE::EffectArchetypes::ArchetypeID;
            switch (a_be->GetArchetype()) {
            case Arch::kCalm:
            case Arch::kFrenzy:
            case Arch::kDemoralize:
            case Arch::kParalysis:
            case Arch::kTurnUndead:
                return true;
            default:
                return false;
            }
        }

        // BENEFICIAL IS AN ALLOW-LIST (review F1, 2026-11-11). An effect declares "beneficial" only when its
        // archetype is one the engine itself gives a non-hostile meaning AND its record carries neither the
        // Hostile nor the Detrimental flag (HostileEffect ran first). Everything else declares nothing:
        //   - a Script effect NEVER declares beneficial, on any delivery: what it does lives in Papyrus, where
        //     a "Self" spell can burn everything around the caster (the Self Script-only exemption this
        //     replaces passed exactly those);
        //   - Stagger, Absorb, Disarm, SpawnHazard, Telekinesis, Banish, SoulTrap, GrabActor, Concussion,
        //     SpawnScriptedRef, CommandSummoned, Lock, Open, Darkness, ValueAndParts,
        //     WerewolfFeed, Werewolf, VampireLord, Disguise and any id this list does not name: undeclared.
        // Q92 (marth 2026-11-12): NightEye is on the list always; Dispel only when the spell is delivered to Self
        // (a_selfDelivered, the spell's own GetDelivery()). A Dispel aimed at anything else stays undeclared.
        // Summon and Reanimate are on the list by ruling (marth 2026-11-11: summons are NOT offense).
        // Archetype ids: RE/E/EffectArchetypes.h (the fork at the registry baseline).
        bool BenignArchetype(RE::EffectArchetypes::ArchetypeID a_arch, bool a_selfDelivered) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            switch (a_arch) {
            case Arch::kValueModifier:         // without Detrimental / Hostile (HostileEffect ran first)
            case Arch::kPeakValueModifier:
            case Arch::kDualValueModifier:
            case Arch::kAccumulateMagnitude:
            case Arch::kInvisibility:
            case Arch::kLight:
            case Arch::kCureDisease:
            case Arch::kCureParalysis:
            case Arch::kCureAddiction:
            case Arch::kCurePoison:
            case Arch::kBoundWeapon:
            case Arch::kRally:
            case Arch::kSlowTime:
            case Arch::kEtherealize:
            case Arch::kDetectLife:
            case Arch::kGuide:
            case Arch::kEnhanceWeapon:
            case Arch::kNightEye:
            case Arch::kSummonCreature:
            case Arch::kReanimate:
                return true;
            case Arch::kDispel:
                return a_selfDelivered;   // Q92: only when delivered to Self
            default:
                return false;
            }
        }

        // The spell is BENEFICIAL only when it has at least one readable effect and EVERY effect declares
        // beneficial; one hostile effect makes it HOSTILE; anything else is UNDECLARED (which denies). A Cloak
        // declares what the spell it applies declares (one level: a cloak inside a cloak's spell, or a cloak
        // with no spell, is undeclared). An effect entry with no base effect is unreadable: undeclared.
        Nature NatureOf(RE::MagicItem* a_mi, bool a_insideCloak = false) {
            if (!a_mi) return Nature::kUndeclared;
            using Arch = RE::EffectArchetypes::ArchetypeID;
            std::size_t seen = 0;
            bool        undeclared = false;
            const bool  selfDelivered = a_mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            for (auto* eff : a_mi->effects) {
                auto* be = eff ? eff->baseEffect : nullptr;
                if (!be) {
                    undeclared = true;
                    continue;
                }
                ++seen;
                if (HostileEffect(be)) return Nature::kHostile;
                const auto arch = be->GetArchetype();
                if (arch == Arch::kCloak) {
                    auto* inner = (!a_insideCloak && be->data.associatedForm)
                                      ? be->data.associatedForm->As<RE::SpellItem>() : nullptr;
                    if (!inner) {
                        undeclared = true;
                        continue;
                    }
                    const Nature n = NatureOf(inner, true);
                    if (n == Nature::kHostile) return Nature::kHostile;
                    if (n == Nature::kUndeclared) undeclared = true;
                } else if (!BenignArchetype(arch, selfDelivered)) {
                    undeclared = true;
                }
            }
            if (seen == 0 || undeclared) return Nature::kUndeclared;
            return Nature::kBeneficial;
        }

        // A MagicItem the engine can cast from a hand or a shout: anything else at the slot is not what the
        // proof says it is (never seen; counted as unreadable, which denies).
        bool LooksLikeMagicItem(const RE::TESForm* a_form) {
            switch (a_form->GetFormType()) {
            case RE::FormType::Spell:
            case RE::FormType::Scroll:
            case RE::FormType::Enchantment:
            case RE::FormType::AlchemyItem:
            case RE::FormType::Ingredient:
                return true;
            default:
                return false;
            }
        }

        // The current context of `a_thread`, when `a_need` bytes of it lie below the data-stack top.
        bool Context(void* a_thread, std::size_t a_need, std::uintptr_t& a_ctx) {
            const auto  thread = reinterpret_cast<std::uintptr_t>(a_thread);
            auto* const header = *reinterpret_cast<std::uintptr_t* const*>(thread + kThreadWindowPtr);
            if (!header) return false;
            const auto base = *header;
            if (!base) return false;
            const auto off = *reinterpret_cast<const std::uint32_t*>(thread + kThreadWindowOff);
            const auto top = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(header) + kHeaderTop);
            if (static_cast<std::uint64_t>(off) + a_need > top) return false;
            a_ctx = base + off;
            return true;
        }

        const char* NameOf(const RE::TESForm* a_form) {
            const char* n = a_form ? a_form->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        // Q91: true when the client that filed the deny claim `a_denyClaim` also holds a live kIntent_Cast claim
        // driving `a_form` on `a_actor`. An unattributed handle (kNoSource) never matches.
        bool OwnClaimHolds(RE::FormID a_actor, RE::FormID a_form, APMF_API::Handle a_denyClaim) {
            if (!a_form) return false;
            apmf::CastSeatClaim cl{};
            if (!apmf::ControlMap::Get().TryGetCastSeatClaimForForm(a_actor, a_form, cl)) return false;
            const auto denyClient = apmf::sources::ClientForHandle(a_denyClaim);
            if (denyClient == apmf::sources::kNoSource) return false;
            return apmf::sources::ClientForHandle(cl.handle) == denyClient;
        }

        bool Decide(LeafKind a_kind, void* a_thread, RE::FormID a_actor, APMF_API::Handle a_claim) {
            RE::TESForm* form   = nullptr;   // the spell or the shout, for the log
            Nature       nature = Nature::kUndeclared;
            bool         read   = false;
            std::uintptr_t ctx  = 0;
            if (a_kind == LeafKind::kSpell) {
                if (Context(a_thread, kCtxSpell + sizeof(void*), ctx)) {
                    auto* mi = *reinterpret_cast<RE::MagicItem* const*>(ctx + kCtxSpell);
                    if (mi && LooksLikeMagicItem(mi)) {
                        form   = mi;
                        nature = NatureOf(mi);
                        read   = true;
                    }
                }
            } else if (a_kind == LeafKind::kShout) {
                if (Context(a_thread, kCtxShout + sizeof(void*), ctx)) {
                    auto* shout = *reinterpret_cast<RE::TESShout* const*>(ctx + kCtxShout);
                    if (shout && shout->GetFormType() == RE::FormType::Shout) {
                        form = shout;
                        read = true;
                        std::size_t spells = 0;
                        nature             = Nature::kBeneficial;
                        for (const auto& v : shout->variations) {
                            if (!v.spell) continue;
                            ++spells;
                            const Nature n = NatureOf(v.spell);
                            if (n == Nature::kHostile) {
                                nature = Nature::kHostile;
                                break;
                            }
                            if (n == Nature::kUndeclared) nature = Nature::kUndeclared;
                        }
                        if (spells == 0) nature = Nature::kUndeclared;
                    }
                }
            }
            const RE::FormID fid = form ? form->GetFormID() : 0;
            if (!read) {
                g_unreadable.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(a_actor, 0, 3))
                    spdlog::warn("[ch.7] hostile-offense: 0x{} {} leaf, the context's {} could not be read (window or "
                                 "pointer) -- counted as HOSTILE, the Offense deny stands (h={}).",
                                 apmf::log::Hex(a_actor), a_kind == LeafKind::kShout ? "shout" : "cast",
                                 a_kind == LeafKind::kShout ? "shout" : "spell", a_claim);
                return false;
            }
            switch (nature) {
            case Nature::kBeneficial:
                g_passed.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(a_actor, fid, 0))
                    spdlog::info("[ch.7] hostile-offense: 0x{} casts '{}' (0x{}), every effect declared beneficial -- NOT Offense, "
                                 "passes the Offense deny (h={}).",
                                 apmf::log::Hex(a_actor), NameOf(form), apmf::log::Hex(fid), a_claim);
                return true;
            case Nature::kUndeclared:
                // Q91 (marth 2026-11-12): a spell the SAME client holds a live cast claim for, on this actor, is
                // declared by that client and passes that client's own Offense-only deny. Not another client's
                // deny (the owners differ), not a full deny (this function only runs for an Offense-only one),
                // not a hostile record (that case never gets here). Lock-free claim read; the client lookup is
                // the shared leaf lock ClaimOutranksTree already takes on this thread.
                if (OwnClaimHolds(a_actor, fid, a_claim)) {
                    g_passed.fetch_add(1, std::memory_order_relaxed);
                    if (LineDue(a_actor, fid, 1))
                        spdlog::info("[ch.7] hostile-offense: 0x{} casts '{}' (0x{}), nature undeclared but passed own "
                                     "Offense-only deny: client {} holds cast claim for spell 0x{} (deny h={}).",
                                     apmf::log::Hex(a_actor), NameOf(form), apmf::log::Hex(fid),
                                     apmf::sources::ClientForHandle(a_claim), apmf::log::Hex(fid), a_claim);
                    return true;
                }
                g_undeclared.fetch_add(1, std::memory_order_relaxed);
                if (LineDue(a_actor, fid, 2))
                    spdlog::warn("[ch.7] hostile-offense: 0x{} casts '{}' (0x{}), nature UNDECLARED by its record (no "
                                 "readable effect, or an effect that does not declare itself beneficial: a script, or an archetype off "
                                 "the benign list) -- counted "
                                 "as HOSTILE, the Offense deny stands (h={}).",
                                 apmf::log::Hex(a_actor), NameOf(form), apmf::log::Hex(fid), a_claim);
                return false;
            case Nature::kHostile:
            default:
                g_hostile.fetch_add(1, std::memory_order_relaxed);
                return false;   // the deny that follows logs itself (ActionGate)
            }
        }
    }  // namespace

    void Install() {
        if (g_installed.exchange(true)) return;
        const char* why = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        else if (GetPrivateProfileIntA("CombatAction", "bHostileOnlyOffense", 1, kIni) == 0)
            why = "[CombatAction] bHostileOnlyOffense=0";
        if (why) {
            spdlog::warn("[ch.7] hostile-offense NOT armed -- {}. Every cast leaf counts as Offense (the pre-v40 "
                         "behaviour): an Offense deny also denies beneficial casts.", why);
            return;
        }
        const RtSpec& rt = apmf::allowance::IsRuntime1_7_104()               ? k17
                         : REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97) ? kSE
                                                                             : kAE;
        struct Addr { std::uintptr_t a; bool ok; };
        const auto verified = [](const Fn& a_fn) {
            const std::uintptr_t a = Resolve(a_fn);
            return Addr{ a, apmf::allowance::SeatVerified(a, a_fn.row) };
        };
        const Addr imm = verified(rt.enterImm), conc = verified(rt.enterConc), dual = verified(rt.enterDual),
                   shout = verified(rt.enterShout), cast = verified(rt.cast), sc = verified(rt.shoutCast);

        const bool castOk = cast.ok && BytesAt(cast.a, rt.castReg) && BytesAt(cast.a, rt.castArg) &&
                            BytesAt(cast.a, rt.castType);
        const bool scOk = sc.ok && BytesAt(sc.a, rt.scReg) && BytesAt(sc.a, rt.scPick) && BytesAt(sc.a, rt.scArg);
        g_immOk  = castOk && imm.ok && BytesAt(imm.a, rt.immWin) && BytesAt(imm.a, rt.immCall) &&
                   CallLandsOn(imm.a, rt.immCall, cast.a);
        g_concOk = castOk && conc.ok && BytesAt(conc.a, rt.concWin) && BytesAt(conc.a, rt.concCall) &&
                   CallLandsOn(conc.a, rt.concCall, cast.a);
        g_dualOk = dual.ok && BytesAt(dual.a, rt.dualWin) && BytesAt(dual.a, rt.dualSpell);
        g_shoutOk = scOk && shout.ok && BytesAt(shout.a, rt.shoutWin) && BytesAt(shout.a, rt.shoutCall) &&
                    CallLandsOn(shout.a, rt.shoutCall, sc.a);

        const auto say = [](bool a_ok) { return a_ok ? "hostile-only" : "REFUSED (every cast is Offense)"; };
        const bool all = g_immOk && g_concOk && g_dualOk && g_shoutOk;
        const auto lvl = all ? spdlog::level::info : spdlog::level::warn;
        spdlog::log(lvl,
                    "[ch.7] hostile-offense (ABI v40) on {}: CastImmediateSpell {}, CastConcentrationSpell {}, "
                    "PrepareDualCast {}, CastShout {}. On a hostile-only leaf kCombatActionCat_Offense denies a cast "
                    "only when its spell is hostile or of undeclared nature; kCombatActionCat_Cast denies every cast.{}",
                    rt.name, say(g_immOk), say(g_concOk), say(g_dualOk), say(g_shoutOk),
                    all ? "" : " A REFUSED leaf failed its address row or its byte proof (another patcher, or a "
                               "different build): the spell it casts is not read there.");
    }

    LeafKind ProvenKind(std::string_view a_leafName) {
        if (a_leafName == "CombatBehaviorCastImmediateSpell") return g_immOk ? LeafKind::kSpell : LeafKind::kNone;
        if (a_leafName == "CombatBehaviorCastConcentrationSpell") return g_concOk ? LeafKind::kSpell : LeafKind::kNone;
        if (a_leafName == "CombatBehaviorPrepareDualCast") return g_dualOk ? LeafKind::kSpell : LeafKind::kNone;
        if (a_leafName == "CombatBehaviorCastShout") return g_shoutOk ? LeafKind::kShout : LeafKind::kNone;
        return LeafKind::kNone;
    }

    bool PassesOffenseDeny(LeafKind a_kind, void* a_thread, RE::FormID a_actor, APMF_API::Handle a_claim) noexcept {
        if (a_kind == LeafKind::kNone || !a_thread) return false;
        try {
            return Decide(a_kind, a_thread, a_actor, a_claim);
        } catch (...) {
            g_errors.fetch_add(1, std::memory_order_relaxed);
            return false;   // nothing unwinds into the tree; a throw keeps the deny
        }
    }

    void Heartbeat() {
        const auto total = g_passed.load(std::memory_order_relaxed) + g_hostile.load(std::memory_order_relaxed) +
                           g_undeclared.load(std::memory_order_relaxed) + g_unreadable.load(std::memory_order_relaxed) +
                           g_errors.load(std::memory_order_relaxed);
        if (total == g_beatSeen.load(std::memory_order_relaxed)) return;   // nothing judged since the last line
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastBeatMs.load(std::memory_order_relaxed) < kHeartbeatMs) return;
        g_lastBeatMs.store(now, std::memory_order_relaxed);   // game-thread-only writer
        g_beatSeen.store(total, std::memory_order_relaxed);
        spdlog::info("[ch.7] hostile-offense H (casts met by an Offense-only deny, session totals): passed-beneficial={} "
                     "denied-hostile={} denied-undeclared={} denied-unreadable={} errors={}",
                     g_passed.load(std::memory_order_relaxed), g_hostile.load(std::memory_order_relaxed),
                     g_undeclared.load(std::memory_order_relaxed), g_unreadable.load(std::memory_order_relaxed),
                     g_errors.load(std::memory_order_relaxed));
    }
}
