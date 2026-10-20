#include "PCH.h"
#include "channels/SubstrateAggressor.h"

#include "core/Allowance.h"   // SeatVerified, the exact-runtime gates
#include "core/Log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// ============================================================================
// THE SUBSTRATE AGGRESSOR CLEAR (substrate round R5, fix F-3; SUBSTRATE-R2-RESEARCH.md section 1; INVARIANTS #0 (j)
// condition 2 (g)).
//
// THE BIT (PROVEN on 1.6.1170 and 1.5.97): AIProcess+0x136 bit 2, touched only through two leaf accessors (a scan of
// every [reg+0x136] access found no other read or write of bit 2 in AIProcess code):
//   get AE 39144 0x6D6C90 / SE 38184 0x6442E0  [Substrate.AIProcess.GetAggressor]
//   set AE 39145 0x6D6CB0 / SE 38185 0x644300  [Substrate.AIProcess.SetAggressor]  (dl = the new value)
// Writers: StartCombat (1 on the attacker of a non-hostile target; the JOIN copies the group's first member's
// bit), StopCombat (0, the call this file makes), AI message 0x2A (1; posted only by the Papyrus native
// Actor.StartCombat), and the process update (0 for an actor with no controller). Readers: GetShouldHelp (a moral
// helper does not help an aggressor), KillImpl (INFERRED: murder versus a justified kill), StartCombat (an attacker
// of an aggressor is not the aggressor), the attack reaction, and the group join. NONE of them raises an alarm, a
// flee or a fight by itself; while it is set on a held holder they read a wrong fact (the follower "attacked
// someone"), which matters at the real-fight transition. So: ONCE, right after the entry, it is set to false through
// the engine's own setter, on the game thread. NEVER again (review F2 of e6f5607c): every later writer is a real
// event -- a script's Actor.StartCombat (AI message 0x2A) on a held holder is a real start whose REAL bit lands only
// later, and a join copies a real group's bit -- and no reader of the bit can tell those apart from a substrate-own
// write, so a re-clear on any read of 1 would erase a legitimate bit. The probe's close / +1 s / +5 s reads and the
// OBSERVED line's aggressor-bit= still show a bit set again while held. The close's StopCombat clears it anyway
// (AE 0x6B7403).
//
// THREADING: game thread only. The setter is the engine's non-atomic read-modify-write of the byte +0x136; the
// engine calls it from StartCombat / StopCombat on whatever thread runs them, so a concurrent engine write of
// another bit of that byte is the same exposure the engine's own StopCombat from the main thread has.
//
// SCOPE: exactly 1.6.1170, 1.5.97 and 1.7.104. On 1.7.104 AE ids 39144 / 39145 have no MIT id-table row (a
// RELOCATION_ID miss is fatal there), so the two accessors are raw-RVA rows (Substrate.AIProcess.*.1_7_104:
// 0x6E9840 / 0x6E9860, byte-identical to the AE bodies) and their bytes are re-read here before use.
// [CombatSubstrate] bSubstrateAggressorClear=0 switches it off (logged per OPEN).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
    // 1.7.104 raw rows (review F3 of e6f5607c) and the exact bodies re-read before use (identical on AE and 1.7.104)
    constexpr std::uintptr_t k17GetAggressor = 0x6E9840;   // AE 0x6D6C90 (id 39144)
    constexpr std::uintptr_t k17SetAggressor = 0x6E9860;   // AE 0x6D6CB0 (id 39145)
    constexpr std::array<std::uint8_t, 13> kGetBody{ 0x0F, 0xB6, 0x81, 0x36, 0x01, 0x00, 0x00, 0xC0, 0xE8, 0x02, 0x24, 0x01, 0xC3 };
    constexpr std::array<std::uint8_t, 39> kSetBody{ 0x44, 0x0F, 0xB6, 0x81, 0x36, 0x01, 0x00, 0x00, 0x41, 0x0F, 0xB6, 0xC0, 0x41,
                                                     0x80, 0xC8, 0x04, 0x24, 0xFB, 0x45, 0x0F, 0xB6, 0xC0, 0x84, 0xD2, 0x0F, 0xB6,
                                                     0xC0, 0x44, 0x0F, 0x44, 0xC0, 0x44, 0x88, 0x81, 0x36, 0x01, 0x00, 0x00, 0xC3 };

    bool Body17(std::uintptr_t a_addr, const std::uint8_t* a_want, std::size_t a_n, const char* a_seat) {
        if (std::memcmp(reinterpret_cast<const void*>(a_addr), a_want, a_n) == 0) return true;
        spdlog::error("[selfcheck] seat {} REFUSED on 1.7.104: the body at RVA 0x{:X} is not the verified accessor", a_seat,
                      a_addr - REL::Module::get().base());
        return false;
    }

    using Get_t = bool (*)(const RE::AIProcess*);
    using Set_t = void (*)(RE::AIProcess*, bool);
    Get_t g_get = nullptr;
    Set_t g_set = nullptr;

    bool        g_on  = false;
    const char* g_off = "not installed";

    struct Rec {
        int atEntry = -1;   // the bit right after StartCombat returned (before the clear)
        int afterClear = -1;
    };
    std::unordered_map<RE::FormID, Rec> g_recs;   // GAME THREAD

}

namespace apmf::substrateaggressor {

    const char* Install() {
        char b[8]{};
        REX::W32::GetPrivateProfileStringA("CombatSubstrate", "bSubstrateAggressorClear", "", b, sizeof(b), kIni);
        if (b[0] && std::atoi(b) == 0) {
            g_on  = false;
            g_off = "[CombatSubstrate] bSubstrateAggressorClear=0 in APMF.ini";
            return nullptr;
        }
        if (!apmf::allowance::RuntimeSupported())
            return "the aggressor clear's rows are verified on exactly 1.6.1170, 1.5.97 and 1.7.104 only";
        std::uintptr_t get = 0, set = 0;
        bool           ok  = true;
        if (apmf::allowance::IsRuntime1_7_104()) {
            get = REL::Offset(k17GetAggressor).address();
            set = REL::Offset(k17SetAggressor).address();
            ok  = apmf::allowance::SeatVerified(get, "Substrate.AIProcess.GetAggressor.1_7_104") &&
                 Body17(get, kGetBody.data(), kGetBody.size(), "Substrate.AIProcess.GetAggressor.1_7_104");
            ok = apmf::allowance::SeatVerified(set, "Substrate.AIProcess.SetAggressor.1_7_104") &&
                 Body17(set, kSetBody.data(), kSetBody.size(), "Substrate.AIProcess.SetAggressor.1_7_104") && ok;
        } else {
            get = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38184, 39144) }.address();
            set = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(38185, 39145) }.address();
            ok  = apmf::allowance::SeatVerified(get, "Substrate.AIProcess.GetAggressor");
            ok  = apmf::allowance::SeatVerified(set, "Substrate.AIProcess.SetAggressor") && ok;
        }
        if (!ok)
            return "the address self-check refused a row the aggressor clear calls (listed above): without it a held "
                   "holder would carry the aggressor bit StartCombat set";
        g_get = reinterpret_cast<Get_t>(get);
        g_set = reinterpret_cast<Set_t>(set);
        g_on  = true;
        g_off = "";
        return nullptr;
    }

    bool        On() { return g_on; }
    const char* OffReason() { return g_off; }

    void AfterEntry(RE::FormID a_holder, RE::AIProcess* a_proc) {
        Rec& r = g_recs[a_holder];
        r      = Rec{};
        if (!g_on) {
            spdlog::warn("[substrate] 0x{} known hole: the aggressor clear is OFF ({}): the holder keeps the aggressor "
                         "bit StartCombat set (AIProcess+0x136 bit 2 = {}).",
                         Hex(a_holder), g_off,
                         a_proc ? ((a_proc->lowProcessFlags.underlying() & 0x04) ? "1" : "0") : "no process");
            return;
        }
        if (!a_proc) {
            spdlog::warn("[substrate] 0x{} aggressor clear: no AI process after the entry; nothing written.", Hex(a_holder));
            return;
        }
        r.atEntry = g_get(a_proc) ? 1 : 0;
        g_set(a_proc, false);   // the engine's own setter, as StopCombat calls it
        r.afterClear = g_get(a_proc) ? 1 : 0;
        spdlog::info("[substrate] 0x{} aggressor bit (AIProcess+0x136 bit 2) after the entry {} -> {} (the engine's setter, "
                     "AE 39145 / SE 38185, set false: a held holder attacked no one){}",
                     Hex(a_holder), r.atEntry, r.afterClear,
                     r.afterClear ? " -- STILL SET after the setter (a concurrent StartCombat set it again?)" : "");
    }

    std::string Closing(RE::FormID a_holder) {
        const auto it = g_recs.find(a_holder);
        if (it == g_recs.end()) return {};
        const Rec r = it->second;
        g_recs.erase(it);
        if (!g_on) return fmt::format("aggressor clear OFF ({})", g_off);
        if (r.atEntry < 0) return "aggressor clear: nothing written (no process at the entry)";
        return fmt::format("aggressor bit at the entry {}, after the one clear {} (never re-cleared: a later 1 is a real "
                           "event's)",
                           r.atEntry, r.afterClear);
    }

    void Reset() { g_recs.clear(); }

}
