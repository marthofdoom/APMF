#include "PCH.h"
#include "core/Allowance.h"   // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"   // Categorize(): THE one category map (MAP.md EquipSink 8b)
#include "core/HandBlock.h"

#include <intrin.h>   // _ReturnAddress (core/EquipGate.cpp precedent)
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

// Win32 INI read, declared by hand like every other core/*.cpp that reads APMF.ini
// (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// See HandBlock.h for the rule, the field origin and the seat. Everything below is the
// 0x0F half; the sink half is a step in core/EquipSink.cpp's verdict that calls HeldFor.

namespace apmf::handblock {

    namespace {

        // Layout this file reads (all below the AE +8 CombatController divergence at 0x68,
        // ENGINE_NOTES §0.29). The same asserts core/EquipGate.cpp and core/AiCastSeats.cpp
        // carry for the same members.
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify against the fork header");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10,
                      "CombatInventoryItem::item moved -- AE 48124 reads the form at item+0x10");
        static_assert(offsetof(RE::CombatInventoryItem, itemSlot) == 0x20,
                      "CombatInventoryItem::itemSlot moved -- AE 48124 passes [item+0x20] as the equip slot");

        constexpr std::size_t   kCheckShouldEquip = 0x0F;
        constexpr std::uint64_t kLineMs           = 1500;   // per (actor, item) cadence, the seats' usual one

        using CheckShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);

        // The four weapon-class leaves (+ OneHandedBlock, below) and, per exact runtime, the function slot 0x0F holds
        // in the unpacked executable (read off the vtables, 2026-10-06, this branch's agent
        // log). Melee / Ranged / Shield share the base `sub rsp,0x28; mov rcx,rdx; call
        // IsFleeing; test al,al; sete al; ret` (AE 0x817FC0 -> 33232, SE 0x77DC90 -> 32485,
        // 1.7.104 0x82CEA0). Torch OVERRIDES the slot (AE 0x819760, SE 0x77F350, 1.7.104
        // 0x82E640): the same IsFleeing test, then "is a torch worth holding here" through
        // the actor's process; the same `bool(CombatInventoryItem*, CombatController*)`
        // shape, rcx = this, rdx = controller, the answer in al. Scalar return: the hidden
        // sret class (the GetMagicTarget CTD) cannot apply. The vtables are VerifiedAddresses
        // rows CombatInventoryItem{Melee,Ranged,Shield,Torch} on all three builds; the slot
        // values are those rows' doc column (0x0F) and the 1.7.104 raw rows
        // AiCastSeats.kCheckShouldEquipBaseAE / HandBlock.Torch.CheckShouldEquip / HandBlock.OneHandedBlock.CheckShouldEquip.
        struct ClassSpec {
            const char*    tag;
            REL::VariantID vtable;
            std::uintptr_t equipAE;   // 1.6.1170
            std::uintptr_t equipSE;   // 1.5.97
            std::uintptr_t equip17;   // 1.7.104
        };
        constexpr ClassSpec kClasses[] = {
            { "Melee",  RE::VTABLE_CombatInventoryItemMelee[0],  0x817FC0, 0x77DC90, 0x82CEA0 },
            { "Ranged", RE::VTABLE_CombatInventoryItemRanged[0], 0x817FC0, 0x77DC90, 0x82CEA0 },
            { "Shield", RE::VTABLE_CombatInventoryItemShield[0], 0x817FC0, 0x77DC90, 0x82CEA0 },
            { "Torch",  RE::VTABLE_CombatInventoryItemTorch[0],  0x819760, 0x77F350, 0x82E640 },
            // Review F1 (1cc0bc9): the unarmed "block with a free off-hand" item. CombatInventory
            // builds it over the unarmed weapon global with slot kLeftHandEquip (AE 0x80F2C5..
            // 0x80F2F5), and it OVERRIDES 0x0F: AE 0x819300 (id 45056) / SE 0x77EF60 (43837) /
            // 1.7.104 0x82E1E0, the same `bool(this, controller)` shape (rdx = controller,
            // IsFleeing first). Without it the left hand had no admission gate for that item.
            { "OneHandedBlock", RE::VTABLE_CombatInventoryItemOneHandedBlock[0], 0x819300, 0x77EF60, 0x82E1E0 },
        };
        constexpr std::size_t kNumClasses = std::size(kClasses);

        // Filled once at Install (before any combat can run), read-only after.
        struct Installed {
            std::uintptr_t vt   = 0;
            std::uintptr_t orig = 0;
            const char*    tag  = "";
        };
        std::array<Installed, kNumClasses> g_slots{};
        std::atomic<bool> g_enabled{ true };     // the INI switch (both halves)
        std::atomic<bool> g_seatArmed{ false };  // at least one 0x0F class installed
        std::atomic<bool> g_installOnce{ false };

        std::atomic<std::uint64_t> g_denies{ 0 };
        std::atomic<std::uint64_t> g_foreign{ 0 };

        std::mutex                                       g_rlMx;   // leaf lock: no engine call, no log under it
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLineMs;

        // Review F8: a GLOBAL cap on top of the per-(actor, item) cadence, the equip sink's
        // shape (kCapPerSec lines per second). A line past the cap is counted, and the count
        // is printed with the next line that is admitted (never hidden).
        constexpr std::uint32_t kCapPerSec   = 100;
        std::uint64_t           g_capSecond  = 0;   // under g_rlMx
        std::uint32_t           g_capUsed    = 0;
        std::uint32_t           g_capDropped = 0;

        bool LineDue(RE::FormID a_actor, RE::FormID a_item, std::uint32_t& a_droppedBefore) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) | a_item;
            std::scoped_lock lk(g_rlMx);
            if (g_lastLineMs.size() > 4096) g_lastLineMs.clear();   // bounded; a re-log beats growth
            auto& last = g_lastLineMs[key];
            if (last != 0 && now - last < kLineMs) return false;
            if (now / 1000 != g_capSecond) { g_capSecond = now / 1000; g_capUsed = 0; }
            if (g_capUsed >= kCapPerSec) { ++g_capDropped; return false; }
            ++g_capUsed;
            last            = now;
            a_droppedBefore = g_capDropped;
            g_capDropped    = 0;
            return true;
        }

        // Review F3: what THIS thread's last pass through the thunk decided, so an observer
        // chained OUTSIDE it (core/AiCastSeats.cpp's Ranged probe) can report the engine's
        // own answer and Harbinger's NO separately. Written on every pass that reached the
        // engine; read only by the same thread right after its own call returned.
        struct LastPass {
            const void* item    = nullptr;
            bool        engine  = false;
            bool        refused = false;
        };
        thread_local LastPass t_last{};

        // The return addresses of the 0x0F call sites, per exact runtime (log label only;
        // the same five sites core/AiCastSeats.cpp's Ranged probe labels: pre-loop 44868 /
        // 43637 and the four evaluate passes of 44899 / 43666; 1.7.104 keeps 1.6.1170's
        // offsets, both functions identical there).
        struct RetLabel { std::uintptr_t rva; const char* name; };
        constexpr RetLabel kRetAE[] = { { 0x80FF35, "pre-loop" }, { 0x813AF5, "evaluate" }, { 0x813D3B, "evaluate" },
                                        { 0x814273, "evaluate" }, { 0x8144B5, "evaluate" } };
        constexpr RetLabel kRetSE[] = { { 0x775F04, "pre-loop" }, { 0x779780, "evaluate" }, { 0x77995C, "evaluate" },
                                        { 0x779EA3, "evaluate" }, { 0x77A07C, "evaluate" } };
        constexpr RetLabel kRet17[] = { { 0x824E15, "pre-loop" }, { 0x8289D5, "evaluate" }, { 0x828C1B, "evaluate" },
                                        { 0x829153, "evaluate" }, { 0x829395, "evaluate" } };
        const char* SiteName(std::uintptr_t a_rva) {
            static const bool onAE = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
            static const bool onSE = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97);
            static const bool on17 = allowance::IsRuntime1_7_104();
            if (onAE) for (const auto& r : kRetAE) if (r.rva == a_rva) return r.name;
            if (onSE) for (const auto& r : kRetSE) if (r.rva == a_rva) return r.name;
            if (on17) for (const auto& r : kRet17) if (r.rva == a_rva) return r.name;
            return "unlabelled";
        }

        // The item as the bound object Categorize reads. Only the three governed hand types
        // a weapon-class leaf can carry; anything else (no form, a foreign type) is not this
        // rule's business and keeps the engine's answer.
        const RE::TESBoundObject* BoundOf(RE::TESForm* a_item) {
            if (!a_item) return nullptr;
            if (auto* w = a_item->As<RE::TESObjectWEAP>()) return w;
            if (auto* a = a_item->As<RE::TESObjectARMO>()) return a;
            if (auto* l = a_item->As<RE::TESObjectLIGH>()) return l;
            return nullptr;
        }

        bool CheckShouldEquipThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            // Captured FIRST: the genuine caller, not a frame this thunk pushed.
            const auto retRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base();
            const auto vt     = *reinterpret_cast<std::uintptr_t*>(a_this);

            CheckShouldEquip_t orig = nullptr;
            const char*        tag  = "?";
            for (const auto& s : g_slots) {
                if (s.vt == vt && s.orig) { orig = reinterpret_cast<CheckShouldEquip_t>(s.orig); tag = s.tag; break; }
            }
            if (!orig) {
                // Structurally unreachable: this thunk is written only into the slots
                // recorded above. Never fabricate an answer: call what that vtable's slot
                // really holds -- unless it is this very thunk (a foreign copy of our
                // pointer), where the only non-recursive answer is the benign "don't equip"
                // core/EquipGate.cpp gives a foreign object.
                g_foreign.fetch_add(1, std::memory_order_relaxed);
                const auto live = *reinterpret_cast<std::uintptr_t*>(vt + kCheckShouldEquip * sizeof(void*));
                if (live == reinterpret_cast<std::uintptr_t>(&CheckShouldEquipThunk) || live == 0) return false;
                return reinterpret_cast<CheckShouldEquip_t>(live)(a_this, a_cc);
            }

            const bool engine = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST, always called
            t_last = LastPass{ a_this, engine, false };
            if (!engine) return false;                 // nothing to narrow
            if (!g_enabled.load(std::memory_order_relaxed) || !a_cc) return engine;
            if (apmf::ControlMap::Get().ControlledCount() == 0) return engine;   // nothing claimed anywhere

            auto  actorPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>, held for the reads below
            auto* actor    = actorPtr.get();
            if (!actor) return engine;
            const RE::FormID fid = actor->GetFormID();

            RE::TESForm* const        item  = a_this->item;
            const RE::TESBoundObject* bound = BoundOf(item);
            if (!bound) return engine;
            const RE::FormID itemId = item->GetFormID();

            // The hand(s) this candidate would take, from THE one category map with the
            // item's OWN slot (AE 48124 equips [item+0x20] as the slot). A two-hander or a
            // bow competes for both; a one-hander with an EitherHand / foreign / null slot
            // competes for both (Categorize's conservative rule, which the SINK still applies; at
            // THIS seat that branch is unreachable: CombatInventory AddItem (AE 0x811AC0) splits a
            // multi-parent slot into per-parent items, so a one-hander arrives as a RightHand
            // item plus a LeftHand clone -- APMF-B66 F10); a shield or a torch, the left.
            const std::uint32_t competes =
                apmf::equipsink::Categorize(bound, a_this->itemSlot.equipSlot) &
                (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left);
            if (!competes) return engine;

            // A spells-only floor (ABI v20) holds its hand only against a spell-like item;
            // the weapon-class leaves carry weapons / shields / torches / unarmed, so for
            // them it normally holds nothing on ITS hand (SpellLike is still asked, never
            // assumed); a claim on the other hand still holds a 2H / bow / unarmed candidate.
            HandHold hold{};
            if (!HeldFor(fid, competes, itemId, SpellLike(item), hold)) return engine;

            g_denies.fetch_add(1, std::memory_order_relaxed);
            t_last.refused = true;
            std::uint32_t dropped = 0;
            if (LineDue(fid, itemId, dropped)) {
                char comp[48];
                spdlog::info("[handblock 0x0F] 0x{} '{}' CheckShouldEquip {} item=0x{} '{}' competes={} -> NO (hand {} "
                             "held: R={} L={}) -- engine had said YES; the item stays out of the "
                             "equipment set for the claim's duration. site={} (ret 0x{}){}",
                             apmf::log::Hex(fid), actor->GetName() ? actor->GetName() : "?", tag, apmf::log::Hex(itemId),
                             item->GetName() ? item->GetName() : "?",
                             apmf::equipsink::CategoryNames(competes, comp, sizeof(comp)), HeldName(hold.held),
                             HoldDesc(hold, APMF_API::kEquipCat_Right), HoldDesc(hold, APMF_API::kEquipCat_Left),
                             SiteName(retRva), apmf::log::Hex(retRva),
                             dropped ? fmt::format(" ({} [handblock 0x0F] line(s) dropped by the {}/s cap before this one)",
                                                   dropped, kCapPerSec)
                                     : std::string());
            }
            return false;   // THE one narrowing answer: the engine's YES turned to NO
        }

        bool ReadIniEnabled() {
            return GetPrivateProfileIntA("HandBlock", "bHandClaimBlocksEquip", 1, "Data/SKSE/Plugins/APMF.ini") != 0;
        }

    }   // namespace

    bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

    bool IsThunk(std::uintptr_t a_fn) {
        return a_fn != 0 && a_fn == reinterpret_cast<std::uintptr_t>(&CheckShouldEquipThunk);
    }

    bool EngineAnswerFor(const void* a_item, bool a_chainAnswer, bool& a_refused) {
        a_refused = false;
        if (t_last.item != a_item) return a_chainAnswer;   // this call never reached the block
        a_refused = t_last.refused;
        return t_last.engine;
    }

    const char* HeldName(std::uint32_t a_held) {
        const bool r = (a_held & APMF_API::kEquipCat_Right) != 0;
        const bool l = (a_held & APMF_API::kEquipCat_Left) != 0;
        return r && l ? "R+L" : r ? "R" : l ? "L" : "-";
    }

    bool SpellLike(const RE::TESForm* a_item) {
        if (!a_item) return false;
        if (a_item->Is(RE::FormType::Spell) || a_item->Is(RE::FormType::Scroll)) return true;
        if (const auto* w = a_item->As<RE::TESObjectWEAP>()) return w->IsStaff();   // a staff casts
        return false;
    }

    std::string HoldDesc(const HandHold& a_hold, std::uint32_t a_handBit) {
        if (!(a_hold.held & a_handBit)) return "-";
        const bool right = a_handBit == APMF_API::kEquipCat_Right;
        if (right ? a_hold.spellsOnlyR : a_hold.spellsOnlyL) return "spells-only floor";
        if (right ? a_hold.denyOnlyR : a_hold.denyOnlyL)     return "deny-only floor";
        return "claim (spell 0x" + apmf::log::Hex(right ? a_hold.spellR : a_hold.spellL) + ")";
    }

    bool HeldFor(RE::FormID a_actor, std::uint32_t a_competes, RE::FormID a_item, bool a_spellLike, HandHold& a_out) {
        a_out = HandHold{};
        if (!g_enabled.load(std::memory_order_relaxed) || a_actor == 0) return false;
        auto& cm = apmf::ControlMap::Get();
        if (cm.ControlledCount() == 0) return false;

        struct Side { std::uint32_t bit; apmf::CastHand hand; };
        constexpr Side kSides[] = { { APMF_API::kEquipCat_Right, apmf::CastHand::kRight },
                                    { APMF_API::kEquipCat_Left,  apmf::CastHand::kLeft } };
        for (const auto& s : kSides) {
            if (!(a_competes & s.bit)) continue;
            RE::FormID    spell = 0, proxy = 0;
            std::uint32_t flags = 0;
            // The claim that OCCUPIES this hand (a dual-cast claim occupies both), live
            // only: a lapsed claim is already gone (ControlMap.cpp TryGetCastClaimForHand).
            if (!cm.TryGetCastClaimForHand(a_actor, s.hand, spell, proxy, &flags)) continue;
            if (a_item != 0 && (a_item == spell || a_item == proxy)) continue;   // the claim's own action
            const bool denyOnly = (flags & APMF_API::kCastFlag_DenyHandOnly) != 0;
            // ABI v20: a floor carrying kCastFlag_FloorSpellsOnly reserves the hand against
            // spells only (the bit means nothing without kCastFlag_DenyHandOnly: a driving
            // claim keeps the full block). A non-spell item skips THIS hand only; one that
            // also competes for the other hand (2H, bow, unarmed) is still held there.
            const bool spellsOnly = denyOnly && (flags & APMF_API::kCastFlag_FloorSpellsOnly) != 0;
            if (spellsOnly && !a_spellLike) continue;
            a_out.held |= s.bit;
            if (s.bit == APMF_API::kEquipCat_Right) { a_out.spellR = spell; a_out.denyOnlyR = denyOnly; a_out.spellsOnlyR = spellsOnly; }
            else                                    { a_out.spellL = spell; a_out.denyOnlyL = denyOnly; a_out.spellsOnlyL = spellsOnly; }
        }
        return a_out.held != 0;
    }

    bool AnyHandHeld(RE::FormID a_actor) {
        HandHold h{};
        // a_spellLike = true: a spells-only floor still counts as holding a hand for the
        // pre-gate; the per-item HeldFor call that follows decides whether it holds it
        // against THIS item.
        return HeldFor(a_actor, APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left, 0, true, h);
    }

    void Install() {
        if (g_installOnce.exchange(true)) return;
        g_enabled.store(ReadIniEnabled(), std::memory_order_relaxed);
        if (!g_enabled.load(std::memory_order_relaxed)) {
            spdlog::warn("[handblock] [HandBlock] bHandClaimBlocksEquip=0 -- a hand held by a cast claim does NOT "
                         "refuse weapon / shield / torch equips (neither the 0x0F seat nor the equip-sink step).");
            return;
        }
        if (REL::Module::IsVR()) {
            spdlog::warn("[handblock] VR runtime -- the weapon-class 0x0F seat is verified on 1.6.1170, 1.5.97 and "
                         "1.7.104 only; NOT installed (the equip-sink step still applies wherever that seat runs).");
            return;
        }
        if (!allowance::RuntimeSupported()) {   // G1: the exact build, never a family bucket
            spdlog::error("[handblock] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- the weapon-class "
                          "CheckShouldEquip (0x0F) seat is NOT installed (REFUSED).",
                          REL::Module::get().version().string("."));
            return;
        }
        const bool onAE = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
        const bool on17 = allowance::IsRuntime1_7_104();

        int         n = 0;
        std::string refused;
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            const auto&                     spec = kClasses[i];
            REL::Relocation<std::uintptr_t> vt{ spec.vtable };
            if (vt.address() == 0 ||
                !allowance::SeatVerified(vt.address(), fmt::format("HandBlock.{}.CheckShouldEquip", spec.tag))) {
                refused += fmt::format("{}{}(vtable unverified)", refused.empty() ? "" : ",", spec.tag);
                continue;
            }
            // REVIEW-BACKLOG APMF-B36 F2: a DENY at this slot must know what it wraps. The
            // live slot must hold the disassembled engine function for THIS exact build; a
            // prior hook (another DLL) or a null slot refuses this class, loudly. The
            // equip-sink step still covers the hand on every path, the combat one included.
            REL::Relocation<std::uintptr_t> expected{ REL::Offset(onAE ? spec.equipAE : on17 ? spec.equip17 : spec.equipSE) };
            // 1.7.104 has no Address Library: its three slot functions are raw-RVA rows of that
            // build's table (AiCastSeats.kCheckShouldEquipBaseAE 0x82CEA0, HandBlock.Torch.
            // CheckShouldEquip 0x82E640, HandBlock.OneHandedBlock.CheckShouldEquip 0x82E1E0), so
            // the literal itself is self-checked there. On
            // 1.6.1170 / 1.5.97 the literals are the vtable rows' generated 0x0F doc column.
            if (on17 && !allowance::SeatVerified(expected.address(),
                                                 fmt::format("HandBlock.{}.CheckShouldEquip (1.7.104 slot function)", spec.tag))) {
                refused += fmt::format("{}{}(1.7.104 slot function unverified)", refused.empty() ? "" : ",", spec.tag);
                continue;
            }
            const auto live = *reinterpret_cast<std::uintptr_t*>(vt.address() + kCheckShouldEquip * sizeof(void*));
            if (live == 0 || live != expected.address()) {
                spdlog::error("[handblock] {} vtable 0x{} slot 0x0F holds 0x{}, expected the disassembled engine "
                              "function 0x{} for this build -- REFUSED for {} (never a deny over a slot it cannot "
                              "identify; a prior hook would be another DLL's). The equip-sink step still refuses "
                              "an equip into a held hand.",
                              spec.tag, apmf::log::Hex(vt.address(), 16), apmf::log::Hex(live, 16),
                              apmf::log::Hex(expected.address(), 16), spec.tag);
                refused += fmt::format("{}{}(slot 0x0F not the engine function)", refused.empty() ? "" : ",", spec.tag);
                continue;
            }
            g_slots[i].vt   = vt.address();
            g_slots[i].tag  = spec.tag;
            g_slots[i].orig = vt.write_vfunc(kCheckShouldEquip, &CheckShouldEquipThunk);
            ++n;
        }
        g_seatArmed.store(n > 0, std::memory_order_release);
        if (n == static_cast<int>(kNumClasses)) {
            spdlog::info("[handblock] INSTALLED: CheckShouldEquip (0x0F) on the Melee, Ranged, Shield, Torch and "
                         "OneHandedBlock item vtables (each slot held this build's engine function). A hand held by a live cast claim (a "
                         "deny-only floor included; a spells-only floor, ABI v20, refuses spell-like items only) refuses "
                         "every weapon / shield / torch candidate competing for it; "
                         "the engine answers first, only its YES turns to NO. The equip-sink step rides the ch.17 "
                         "seat (its own [apmf][equip-sink] INSTALLED line says whether that seat is live).");
        } else {
            spdlog::error("[handblock] PARTIAL: {}/{} weapon-class 0x0F seats installed; refused: {}. The equip-sink "
                          "step still refuses an equip into a held hand on every path.",
                          n, kNumClasses, refused.empty() ? "-" : refused);
        }
    }
}
