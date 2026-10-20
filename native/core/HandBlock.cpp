#include "PCH.h"
#include "core/Sources.h"   // Harbinger 1a: seat census
#include "core/Allowance.h"   // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"   // Categorize(): THE one category map (MAP.md EquipSink 8b)
#include "core/HandBlock.h"
#include "core/FacetAttribution.h"   // D2 (2026-10-12): the refusal is ranked on the Equip facet

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
        std::atomic<std::uint64_t> g_declDenies{ 0 };   // ch.17 selection-time refusals (2026-10-13)
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

        // ch.17 AT SELECTION TIME (2026-10-13, ClickUp 86e3pyhxv; the Sept enforced-run defect,
        // Docs/STATUS.md: 114 bow + 18 shield refusals at the SINK because CombatInventory had
        // already admitted the item here -- the AI kept a plan it could never carry out). For an
        // actor with a ch.17 claim, answer NO for a candidate its declaration refuses, using THE
        // shared verdict (equipsink::DeclaredSetVerdict) with the candidate's own slot (the slot
        // AE 48124 / SE 46955 hands to EquipObject, so the sink would compute the same mask).
        // Engine answered first (the caller passes its YES); a NO only narrows. Ranked on the Equip
        // facet against the answer's source beneath this seat (InsideSource: the engine, or a hook
        // DLL inside 0x0F; a CLIENT's hook ranks as the engine, so the ch.17 own-client binding of
        // the sink never arises here). Observe-only (INI opt-in or the claim's bit): logged, the
        // engine's YES stands. Lock-free RCU read only (TryGetEquipSet), combat thread.
        // REVIEW F2 (2026-10-13, field criterion): a HAND whose every candidate the ch.17 declaration refuses
        // at selection (an owned hand whose declared item is not carried, or a client with the default
        // {All, 0} scope declaring armor only) leaves that hand with nothing to pick, silently. No new seat:
        // the closest observable is this seat's own answers per (actor, hand). For a claimed actor every
        // YES this seat returns for a candidate competing for a hand stamps that hand; every ch.17 NO
        // counts. When a hand has had ch.17 NOs and NO YES at all for kEmptyHandMs (several CombatInventory
        // evaluations), one line names the actor, the hand and the declared item(s) -- once per (actor,
        // hand, claim handle). Log only: no answer changes. Leaf lock; the line is logged outside it.
        constexpr std::uint64_t kEmptyHandMs = 5000;
        struct HandWatch {
            std::uint64_t firstNoMs = 0;   // first ch.17 NO since the last YES (0 = none)
            std::uint32_t nos       = 0;
            bool          logged    = false;
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
        };
        std::mutex                                  g_hwMx;
        std::unordered_map<std::uint64_t, HandWatch> g_handWatch;   // (actor << 1 | left) -> watch
        void NoteHandAnswerImpl(RE::FormID a_fid, std::uint32_t a_hands, bool a_yes, const apmf::EquipSetView& a_set);
        // Never throws: it runs inside the engine's CheckShouldEquip frame (a map insert may allocate).
        void NoteHandAnswer(RE::FormID a_fid, std::uint32_t a_hands, bool a_yes, const apmf::EquipSetView& a_set) {
            try {
                NoteHandAnswerImpl(a_fid, a_hands, a_yes, a_set);
            } catch (...) {
            }
        }
        void NoteHandAnswerImpl(RE::FormID a_fid, std::uint32_t a_hands, bool a_yes, const apmf::EquipSetView& a_set) {
            const auto now = apmf::clock::MonotonicMs();
            struct Due { bool left; std::uint32_t nos; std::uint64_t ms; };
            Due due[2]{};
            int nDue = 0;
            {
                std::scoped_lock lk(g_hwMx);
                if (g_handWatch.size() > 4096) g_handWatch.clear();   // bounded; a re-log beats growth
                for (const bool left : { false, true }) {
                    const std::uint32_t bit = left ? APMF_API::kEquipCat_Left : APMF_API::kEquipCat_Right;
                    if (!(a_hands & bit)) continue;
                    auto& w = g_handWatch[(static_cast<std::uint64_t>(a_fid) << 1) | (left ? 1u : 0u)];
                    if (w.handle != a_set.handle) w = HandWatch{ 0, 0, false, a_set.handle };   // a new claim: a fresh watch
                    if (a_yes) { w.firstNoMs = 0; w.nos = 0; continue; }
                    if (w.firstNoMs == 0) w.firstNoMs = now;
                    ++w.nos;
                    if (!w.logged && w.nos >= 2 && now - w.firstNoMs >= kEmptyHandMs) {
                        w.logged = true;
                        due[nDue++] = Due{ left, w.nos, now - w.firstNoMs };
                    }
                }
            }
            for (int i = 0; i < nDue; ++i) {
                try {
                    // The declared items that could fill this hand (FormIDs only: no lookup on the combat thread).
                    std::string decl;
                    for (std::uint32_t k = 0; k < a_set.count && k < APMF_API::kMaxEquipSet; ++k) {
                        const auto sl = a_set.slots[k];
                        const bool fits = sl == APMF_API::kEquipSlot_Default ||
                                          (due[i].left ? sl == APMF_API::kEquipSlot_Left : sl == APMF_API::kEquipSlot_Right);
                        if (!fits) continue;
                        decl += (decl.empty() ? "" : ", ");
                        decl += "0x" + apmf::log::Hex(a_set.forms[k]) +
                                (sl == APMF_API::kEquipSlot_Default ? " (default)" : due[i].left ? " (left)" : " (right)");
                    }
                    spdlog::warn("[equipauth 0x0F] 0x{} {} hand: EVERY candidate the combat AI offered for this hand was "
                                 "refused by the ch.17 declaration ({} refusals, no candidate admitted, for {} ms). The hand "
                                 "has nothing it may pick. Declared items for this hand: {}. Scope owned=0x{} denied=0x{}, "
                                 "set={} item(s). A declared item the actor does not carry, or a hand owned but left "
                                 "undeclared, reads this way. Logged once per actor, hand and claim.",
                                 apmf::log::Hex(a_fid), due[i].left ? "LEFT" : "RIGHT", due[i].nos, due[i].ms,
                                 decl.empty() ? std::string("none") : decl, apmf::log::Hex(a_set.owned, 2),
                                 apmf::log::Hex(a_set.denied, 2), a_set.count);
                } catch (...) {
                }
            }
        }

        bool DeclaredSetAnswer(RE::FormID a_fid, RE::Actor* a_actor, RE::TESForm* a_item,
                               RE::FormID a_itemId, std::uint32_t a_cats, CheckShouldEquip_t a_orig, const char* a_tag,
                               std::uintptr_t a_retRva, bool a_engine) {
            if (!a_engine || a_cats == 0) return a_engine;
            apmf::EquipSetView set{};
            if (!apmf::ControlMap::Get().TryGetEquipSet(a_fid, set)) return a_engine;   // no ch.17 claim
            const std::uint32_t hands = a_cats & (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left);
            // ABI v25: a form the set declares ONLY by instance (uid). A CombatInventoryItem carries
            // the form alone (item at +0x10; no extra data list, fork CombatInventoryItem.h) and the
            // equip it drives names NO instance (AE 48124/48126, SE 46955/46957, 1.7.104
            // 0x88CFF0/0x88D360 all pass a null a_extraData), which the sink refuses for such a
            // form. So the candidate is in-set only while a hand it competes for ALREADY holds the
            // form (`held`: keeping it equips nothing new; the declared instance is put on, and a
            // wrong copy replaced, by the enforce pass, which names it). GetEquippedObject is the
            // plain actor-local read AiCastSeats' shield gate makes from this same slot.
            apmf::equipsink::EquipInstance inst{};
            if (apmf::equipsink::InstanceOnly(set, a_itemId) && a_actor) {
                if ((hands & APMF_API::kEquipCat_Right) && a_actor->GetEquippedObject(/*left*/ false) == a_item) inst.held = true;
                if ((hands & APMF_API::kEquipCat_Left) && a_actor->GetEquippedObject(/*left*/ true) == a_item) inst.held = true;
            }
            const auto dv = apmf::equipsink::DeclaredSetVerdict(set, a_itemId, a_cats, inst);
            if (!dv.refuse) {
                if (hands) NoteHandAnswer(a_fid, hands, /*yes*/ true, set);   // review F2: this hand has a candidate
                return a_engine;
            }
            char cats[48];
            if (dv.observe) {
                try {
                    std::uint32_t dropped = 0;
                    if (LineDue(a_fid, a_itemId ^ 0x17000000u, dropped))
                        spdlog::info("[equipauth 0x0F] 0x{} CheckShouldEquip {} item=0x{} '{}' cat={} owned={} black={}: "
                                     "the ch.17 declaration would refuse it -- OBSERVE-ONLY (INI bEquipObserveOnly=1 or "
                                     "the claim's kEquipAuth_ObserveOnly), so the engine's YES stands. site={}",
                                     apmf::log::Hex(a_fid), a_tag, apmf::log::Hex(a_itemId),
                                     a_item->GetName() ? a_item->GetName() : "?",
                                     apmf::equipsink::CategoryNames(a_cats, cats, sizeof(cats)), dv.ownedCat ? 1 : 0,
                                     dv.blacked ? 1 : 0, SiteName(a_retRva));
                } catch (...) {
                }
                if (hands) NoteHandAnswer(a_fid, hands, /*yes*/ true, set);   // observe-only: the YES stands
                return a_engine;
            }
            try {
                const auto v = apmf::facetattr::Decide(apmf::sources::FacetForEquipCats(a_cats), set.handle,
                                                       apmf::facetattr::InsideSource(reinterpret_cast<std::uintptr_t>(a_orig)),
                                                       a_fid, "CheckShouldEquip (0x0F) ch.17 declared set: the answer came "
                                                              "through a hook inside the seat that is in no module");
                if (!v.enforce) {
                    try {
                        if (apmf::facetattr::LineDue(apmf::sources::FacetForEquipCats(a_cats), a_fid, 0x0F17u))
                            spdlog::info("[equipauth 0x0F] 0x{} CheckShouldEquip item=0x{}: the ch.17 declaration would "
                                         "refuse it -- {}, so the engine's own YES STANDS.",
                                         apmf::log::Hex(a_fid), apmf::log::Hex(a_itemId),
                                         apmf::facetattr::WhyStands(apmf::sources::FacetForEquipCats(a_cats), v, set.handle));
                    } catch (...) {
                    }
                    if (hands) NoteHandAnswer(a_fid, hands, /*yes*/ true, set);   // the source outranks: YES stands
                    return a_engine;
                }
            } catch (...) {
                // A registry throw keeps the claim's answer (the hand block's rule above).
            }
            g_declDenies.fetch_add(1, std::memory_order_relaxed);
            if (hands) NoteHandAnswer(a_fid, hands, /*yes*/ false, set);   // review F2 (never throws)
            t_last.refused = true;
            try {
                std::uint32_t dropped = 0;
                if (LineDue(a_fid, a_itemId ^ 0x17000000u, dropped))
                    spdlog::info("[equipauth 0x0F] 0x{} '{}' CheckShouldEquip {} item=0x{} '{}' cat={} owned={} black={} "
                                 "-> NO (the ch.17 declaration refuses it: {}) -- engine had said YES; the item is not "
                                 "selected, so the AI never plans around an equip the sink would refuse. site={} (ret 0x{}){}",
                                 apmf::log::Hex(a_fid), a_actor->GetName() ? a_actor->GetName() : "?", a_tag,
                                 apmf::log::Hex(a_itemId), a_item->GetName() ? a_item->GetName() : "?",
                                 apmf::equipsink::CategoryNames(a_cats, cats, sizeof(cats)), dv.ownedCat ? 1 : 0,
                                 dv.blacked ? 1 : 0,
                                 dv.blacked      ? "a denied category"
                                 : dv.byInstance ? "declared by instance (uid) and not held in this hand: the combat "
                                                   "AI's equip names no instance, Harbinger's equip pass puts the "
                                                   "declared one on"
                                                 : "off the declared set",
                                 SiteName(a_retRva), apmf::log::Hex(a_retRva),
                                 dropped ? fmt::format(" ({} 0x0F line(s) dropped by the {}/s cap before this one)",
                                                       dropped, kCapPerSec)
                                         : std::string());
            } catch (...) {
            }
            return false;
        }

        // ---- ch.15 WEAPON ADMISSION (feat/apmf-weapon-admission, 2026-10-20; HandBlock.h has the contract) ----
        // WHY THIS SEAT AND NOT A CONTEXT NODE (traced on the unpacked 1.6.1170 and 1.5.97 images, agentlog
        // apmf-weapon-admission.md): the combat tree's "Action Equipment Selector" (AE builder 47688 / SE 46461)
        // runs, per equipment slot, "Melee Sequence" = { "Equip weapon" (the CombatBehaviorEquipObject leaf, whose
        // Enter AE 48124 / SE 46955 calls ActorEquipManager::EquipObject when the slot's item is not worn),
        // "Melee Link" } (Ranged the same with EquipRangedWeapon, 48126 / 46957). The "Melee Context" node
        // (CreateContextNode2<ContextMelee>) is inside the Link and wraps ONLY the attacks, so the swap happens
        // before it and a deny there would cost attacks, not the swap. The item that slot carries is the one
        // CombatInventory's evaluate picked, and that pick asks THIS slot (0x0F) for every candidate.
        std::atomic<bool> g_ch15On{ true };        // [HandBlock] bEquipmentWeaponAdmission
        std::atomic<bool> g_ch15Observe{ false };  // [HandBlock] bEquipmentWeaponObserveOnly
        std::atomic<std::uint64_t> g_ch15Denies{ 0 };

        enum : std::uint8_t { kShapeNone = 0, kShapeOneHand = 1, kShapeBothHands = 2, kShapeLeft = 3 };

        // The hand shape of a claimed form: no lookup, a form-type read and the WEAP type tests only.
        std::uint8_t ShapeOf(RE::TESForm* a_form) {
            if (!a_form) return kShapeNone;
            if (auto* w = a_form->As<RE::TESObjectWEAP>()) {
                if (w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow()) return kShapeBothHands;
                return kShapeOneHand;   // one-handed, a staff included (Categorize's own split)
            }
            if (auto* a = a_form->As<RE::TESObjectARMO>())
                return (apmf::equipsink::Categorize(a, nullptr) & APMF_API::kEquipCat_Shield) ? kShapeLeft : kShapeNone;
            if (a_form->As<RE::TESObjectLIGH>()) return kShapeLeft;
            return kShapeNone;
        }

        // Claimed-form shapes resolved on the game thread (NoteEquipmentClaimForm). Leaf lock, find only on
        // the combat thread (no allocation there).
        std::mutex                                     g_shapeMx;
        std::unordered_map<RE::FormID, std::uint8_t>   g_shapes;

        bool CachedShape(RE::FormID a_form, std::uint8_t& a_out) {
            std::scoped_lock lk(g_shapeMx);
            const auto it = g_shapes.find(a_form);
            if (it == g_shapes.end()) return false;
            a_out = it->second;
            return true;
        }

        // The hand(s) a ch.15 claim on `a_weapon` holds for `a_actor`, by the contract in HandBlock.h.
        // `a_how` names where the answer came from (log text). 0 = no hand (the rule does nothing).
        std::uint32_t ClaimedHands(RE::Actor* a_actor, RE::FormID a_weapon, const char*& a_how) {
            a_how = "-";
            RE::TESForm* const r = a_actor->GetEquippedObject(/*left*/ false);
            RE::TESForm* const l = a_actor->GetEquippedObject(/*left*/ true);
            std::uint32_t worn = 0;
            if (r && r->GetFormID() == a_weapon) worn |= APMF_API::kEquipCat_Right;
            if (l && l->GetFormID() == a_weapon) worn |= APMF_API::kEquipCat_Left;
            std::uint8_t shape = kShapeNone;
            if (worn) {
                shape = ShapeOf((worn & APMF_API::kEquipCat_Right) ? r : l);
            } else if (!CachedShape(a_weapon, shape)) {
                a_how = "unresolved (not worn, shape not cached)";
                return 0;
            }
            switch (shape) {
            case kShapeBothHands:
                a_how = worn ? "two-handed, worn" : "two-handed, not worn";
                return APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left;
            case kShapeLeft:
                a_how = worn ? "shield/torch, worn" : "shield/torch, not worn";
                return APMF_API::kEquipCat_Left;
            case kShapeOneHand:
                if (worn) { a_how = "one-handed, the hand it is worn in"; return worn; }
                a_how = "one-handed, not worn: the right hand";
                return APMF_API::kEquipCat_Right;
            default:
                a_how = "names no hand";
                return 0;
            }
        }

        // Over-suppression watch (the field criterion): a CLAIMED hand that has had ch.15 refusals and NO
        // admitted candidate (the claimed weapon itself) for kEmptyHandMs. One line per (actor, hand, claim).
        // Log only. Never throws.
        struct Ch15Watch {
            std::uint64_t    firstNoMs = 0;
            std::uint32_t    nos       = 0;
            bool             logged    = false;
            APMF_API::Handle handle    = APMF_API::kInvalidHandle;
        };
        std::mutex                                     g_c15Mx;
        std::unordered_map<std::uint64_t, Ch15Watch>   g_c15Watch;   // (actor << 1 | left) -> watch

        void NoteCh15Answer(RE::FormID a_fid, std::uint32_t a_hands, bool a_yes, APMF_API::Handle a_handle,
                            RE::FormID a_weapon) {
            try {
                const auto now = apmf::clock::MonotonicMs();
                struct Due { bool left; std::uint32_t nos; std::uint64_t ms; };
                Due due[2]{};
                int nDue = 0;
                {
                    std::scoped_lock lk(g_c15Mx);
                    if (g_c15Watch.size() > 4096) g_c15Watch.clear();   // bounded; a re-log beats growth
                    for (const bool left : { false, true }) {
                        const std::uint32_t bit = left ? APMF_API::kEquipCat_Left : APMF_API::kEquipCat_Right;
                        if (!(a_hands & bit)) continue;
                        auto& w = g_c15Watch[(static_cast<std::uint64_t>(a_fid) << 1) | (left ? 1u : 0u)];
                        if (w.handle != a_handle) w = Ch15Watch{ 0, 0, false, a_handle };
                        if (a_yes) { w.firstNoMs = 0; w.nos = 0; continue; }
                        if (w.firstNoMs == 0) w.firstNoMs = now;
                        ++w.nos;
                        if (!w.logged && w.nos >= 2 && now - w.firstNoMs >= kEmptyHandMs) {
                            w.logged = true;
                            due[nDue++] = Due{ left, w.nos, now - w.firstNoMs };
                        }
                    }
                }
                for (int i = 0; i < nDue; ++i)
                    spdlog::warn("[ch.15 0x0F] 0x{} {} hand: {} candidate(s) refused for the ch.15 claim on weapon 0x{} "
                                 "and the claimed weapon itself was NOT offered for {} ms (h={}). The hand has nothing "
                                 "the AI may pick: the actor does not carry the weapon, or the engine refused it. "
                                 "Logged once per actor, hand and claim.",
                                 apmf::log::Hex(a_fid), due[i].left ? "LEFT" : "RIGHT", due[i].nos,
                                 apmf::log::Hex(a_weapon), due[i].ms, a_handle);
            } catch (...) {
            }
        }

        // ch.15's step. Runs only when the engine said YES and the hand-claim block did not refuse. Returns
        // true when the candidate is REFUSED (the caller answers NO); false keeps the engine's YES for the
        // next step (ch.17's declared set). Lock-free RCU claim read; the claimed form's shape from the worn
        // object or the game-thread cache. Ranked on the Equip facet like every other equip deny (D2).
        bool Ch15Refuses(RE::FormID a_fid, RE::Actor* a_actor, RE::TESForm* a_item, RE::FormID a_itemId,
                         std::uint32_t a_cats, CheckShouldEquip_t a_orig, const char* a_tag, std::uintptr_t a_retRva) {
            if (!g_ch15On.load(std::memory_order_relaxed)) return false;
            const std::uint32_t competes = a_cats & (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left);
            if (!competes) return false;
            APMF_API::APMF_Param p15{};
            float                b15 = 0.0f;
            APMF_API::Handle     h15 = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_fid, APMF_API::kIntent_Equipment, p15, b15, &h15))
                return false;                                   // no ch.15 claim on this actor
            if (p15.form == 0) return false;                    // the no-param probe facet: not this rule
            const RE::FormID weapon = p15.form;

            const char*         how     = "-";
            const std::uint32_t claimed = ClaimedHands(a_actor, weapon, how);
            char                comp[48];
            std::uint32_t       dropped = 0;
            const auto          key     = a_itemId ^ 0x15000000u;

            if (claimed == 0) {
                try {
                    if (LineDue(a_fid, weapon ^ 0x15150000u, dropped))
                        spdlog::info("[ch.15 0x0F] 0x{} ch.15 claim on 0x{} holds no hand ({}): the weapon rule does "
                                     "nothing for this actor. h={}",
                                     apmf::log::Hex(a_fid), apmf::log::Hex(weapon), how, h15);
                } catch (...) {
                }
                return false;
            }
            char hands[48];
            if (a_itemId == weapon || !(competes & claimed)) {
                const bool isClaimed = a_itemId == weapon;
                if (isClaimed) NoteCh15Answer(a_fid, competes & claimed, /*yes*/ true, h15, weapon);
                try {
                    if (LineDue(a_fid, key, dropped))
                        spdlog::info("[ch.15 0x0F] 0x{} {} item=0x{} '{}' competes={} claimed=0x{} hands={} ({}) -> {}. "
                                     "site={}",
                                     apmf::log::Hex(a_fid), a_tag, apmf::log::Hex(a_itemId),
                                     a_item->GetName() ? a_item->GetName() : "?",
                                     apmf::equipsink::CategoryNames(competes, comp, sizeof(comp)), apmf::log::Hex(weapon),
                                     apmf::equipsink::CategoryNames(claimed, hands, sizeof(hands)), how,
                                     isClaimed ? "ADMIT (the claimed weapon)"
                                               : "UNTOUCHED (competes only for a hand the claim does not hold)",
                                     SiteName(a_retRva));
                } catch (...) {
                }
                return false;
            }

            // A competitor for a claimed hand.
            if (g_ch15Observe.load(std::memory_order_relaxed)) {
                try {
                    if (LineDue(a_fid, key, dropped))
                        spdlog::info("[ch.15 0x0F] 0x{} {} item=0x{} '{}' competes={} claimed=0x{} hands={} ({}) -> "
                                     "WOULD REFUSE -- OBSERVE-ONLY ([HandBlock] bEquipmentWeaponObserveOnly=1), the "
                                     "engine's YES stands. site={}",
                                     apmf::log::Hex(a_fid), a_tag, apmf::log::Hex(a_itemId),
                                     a_item->GetName() ? a_item->GetName() : "?",
                                     apmf::equipsink::CategoryNames(competes, comp, sizeof(comp)), apmf::log::Hex(weapon),
                                     apmf::equipsink::CategoryNames(claimed, hands, sizeof(hands)), how, SiteName(a_retRva));
                } catch (...) {
                }
                return false;
            }
            try {
                const auto v = apmf::facetattr::Decide(apmf::sources::FacetForEquipCats(a_cats), h15,
                                                       apmf::facetattr::InsideSource(reinterpret_cast<std::uintptr_t>(a_orig)),
                                                       a_fid, "CheckShouldEquip (0x0F) ch.15 weapon admission: the answer "
                                                              "came through a hook inside the seat that is in no module");
                if (!v.enforce) {
                    try {
                        if (apmf::facetattr::LineDue(apmf::sources::FacetForEquipCats(a_cats), a_fid, 0x0F15u))
                            spdlog::info("[ch.15 0x0F] 0x{} CheckShouldEquip item=0x{}: the ch.15 weapon rule would "
                                         "refuse it -- {}, so the engine's own YES STANDS.",
                                         apmf::log::Hex(a_fid), apmf::log::Hex(a_itemId),
                                         apmf::facetattr::WhyStands(apmf::sources::FacetForEquipCats(a_cats), v, h15));
                    } catch (...) {
                    }
                    return false;
                }
            } catch (...) {
                // A registry throw keeps the claim's answer (the hand block's rule).
            }
            g_ch15Denies.fetch_add(1, std::memory_order_relaxed);
            NoteCh15Answer(a_fid, competes & claimed, /*yes*/ false, h15, weapon);   // never throws
            try {
                if (LineDue(a_fid, key, dropped))
                    spdlog::info("[ch.15 0x0F] 0x{} '{}' CheckShouldEquip {} item=0x{} '{}' competes={} claimed=0x{} "
                                 "hands={} ({}) -> NO (another weapon for a hand the ch.15 claim holds) -- engine had "
                                 "said YES; the AI keeps the claimed weapon. h={} site={} (ret 0x{}){}",
                                 apmf::log::Hex(a_fid), a_actor->GetName() ? a_actor->GetName() : "?", a_tag,
                                 apmf::log::Hex(a_itemId), a_item->GetName() ? a_item->GetName() : "?",
                                 apmf::equipsink::CategoryNames(competes, comp, sizeof(comp)), apmf::log::Hex(weapon),
                                 apmf::equipsink::CategoryNames(claimed, hands, sizeof(hands)), how, h15,
                                 SiteName(a_retRva), apmf::log::Hex(a_retRva),
                                 dropped ? fmt::format(" ({} 0x0F line(s) dropped by the {}/s cap before this one)",
                                                       dropped, kCapPerSec)
                                         : std::string());
            } catch (...) {
            }
            return true;
        }

        // The steps after the hand-claim block, in order: ch.15's weapon admission, then ch.17's declared set.
        bool AfterHandBlock(RE::FormID a_fid, RE::Actor* a_actor, RE::TESForm* a_item, RE::FormID a_itemId,
                            std::uint32_t a_cats, CheckShouldEquip_t a_orig, const char* a_tag, std::uintptr_t a_retRva,
                            bool a_engine) {
            if (a_engine && Ch15Refuses(a_fid, a_actor, a_item, a_itemId, a_cats, a_orig, a_tag, a_retRva)) {
                t_last.refused = true;
                return false;
            }
            return DeclaredSetAnswer(a_fid, a_actor, a_item, a_itemId, a_cats, a_orig, a_tag, a_retRva, a_engine);
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
            if (!a_cc) return engine;
            if (apmf::ControlMap::Get().ControlledCount() == 0) return engine;   // nothing claimed anywhere
            // Two narrowing steps share this seat: the hand-claim block (HeldFor already answers
            // "nothing held" while [HandBlock] bHandClaimBlocksEquip=0) and, since 2026-10-13, the
            // ch.17 declared-set refusal at SELECTION time (below). Either may turn YES to NO.

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
            const std::uint32_t allCats  = apmf::equipsink::Categorize(bound, a_this->itemSlot.equipSlot);
            const std::uint32_t competes = allCats & (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left);

            // A spells-only floor (ABI v20) holds its hand only against a spell-like item;
            // the weapon-class leaves carry weapons / shields / torches / unarmed, so for
            // them it normally holds nothing on ITS hand (SpellLike is still asked, never
            // assumed); a claim on the other hand still holds a 2H / bow / unarmed candidate.
            HandHold hold{};
            if (!competes || !HeldFor(fid, competes, itemId, SpellLike(item), hold))
                return AfterHandBlock(fid, actor, item, itemId, allCats, orig, tag, retRva, engine);

            // D2 (marth 2026-10-12, "equip denies rank by priority like every other facet"): the NO
            // replaces the answer of the chain beneath this seat (the engine, or a module hooked inside
            // 0x0F on this vtable: InsideSource). It stands only while the cast claim holding the hand
            // outranks that source on the EQUIP facet; otherwise the engine's YES stands. A registry
            // throw keeps the claim's answer (the default-priority answer, core/EquipGate.cpp's rule).
            try {
                const APMF_API::Handle h = apmf::facetattr::CastClaimHandle(fid);
                const auto v = apmf::facetattr::Decide(apmf::sources::FacetForEquipCats(allCats), h,
                                                       apmf::facetattr::InsideSource(reinterpret_cast<std::uintptr_t>(orig)),
                                                       fid, "CheckShouldEquip (0x0F) hand block: the answer came through "
                                                            "a hook inside the seat that is in no module");
                if (!v.enforce) {
                    try {
                        if (apmf::facetattr::LineDue(apmf::sources::FacetForEquipCats(allCats), fid, 0x0F0Bu))
                            spdlog::info("[handblock 0x0F] 0x{} CheckShouldEquip item=0x{}: the hand-claim block would "
                                         "refuse it -- {}, so the engine's own YES STANDS.",
                                         apmf::log::Hex(fid), apmf::log::Hex(itemId),
                                         apmf::facetattr::WhyStands(apmf::sources::FacetForEquipCats(allCats), v, h));
                    } catch (...) {
                    }
                    // The hand block stood down; the ch.17 declaration may still refuse the candidate.
                    return AfterHandBlock(fid, actor, item, itemId, allCats, orig, tag, retRva, engine);
                }
            } catch (...) {
            }

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

    void NoteEquipmentClaimForm(RE::FormID a_form, RE::TESForm* a_resolved) {
        if (a_form == 0) return;
        try {
            const std::uint8_t shape = ShapeOf(a_resolved);
            {
                std::scoped_lock lk(g_shapeMx);
                if (g_shapes.size() > 1024) g_shapes.clear();   // bounded; the next Engage re-fills
                g_shapes[a_form] = shape;
            }
            spdlog::info("[ch.15] weapon admission: claimed form 0x{} '{}' -> {} (the hand(s) the 0x0F rule holds when it "
                         "is not worn; worn, the hand it is worn in decides a one-hander).",
                         apmf::log::Hex(a_form), a_resolved && a_resolved->GetName() ? a_resolved->GetName() : "?",
                         shape == kShapeBothHands ? "both hands (two-handed / bow / crossbow)"
                         : shape == kShapeLeft    ? "the left hand (shield / torch)"
                         : shape == kShapeOneHand ? "one-handed (the right hand when not worn)"
                                                  : (a_resolved ? "names no hand: the weapon rule does nothing"
                                                                : "does not resolve: the weapon rule does nothing"));
        } catch (...) {
        }
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
        // The seat also carries ch.17's SELECTION-time refusal (2026-10-13), so it installs when
        // EITHER half wants it. equipsink::Install runs later (plugin.cpp), so its INI switch is read
        // here directly, with the sink's own default (1). A ch.17 claim is refused while the sink
        // seat is down, so this half reads no claim then even if the seat is installed.
        const bool ch17 = GetPrivateProfileIntA("EquipAuthority", "bEquipAuthority", 1, "Data/SKSE/Plugins/APMF.ini") != 0;
        // ch.15 weapon admission (2026-10-20): its own kill switch and observe-only switch, same section.
        const bool ch15 =
            GetPrivateProfileIntA("HandBlock", "bEquipmentWeaponAdmission", 1, "Data/SKSE/Plugins/APMF.ini") != 0;
        g_ch15On.store(ch15, std::memory_order_relaxed);
        g_ch15Observe.store(
            GetPrivateProfileIntA("HandBlock", "bEquipmentWeaponObserveOnly", 0, "Data/SKSE/Plugins/APMF.ini") != 0,
            std::memory_order_relaxed);
        if (!ch15)
            spdlog::warn("[handblock] [HandBlock] bEquipmentWeaponAdmission=0 -- a ch.15 kIntent_Equipment claim does "
                         "NOT refuse another weapon for its hand at the 0x0F seat.");
        if (!g_enabled.load(std::memory_order_relaxed)) {
            spdlog::warn("[handblock] [HandBlock] bHandClaimBlocksEquip=0 -- a hand held by a cast claim does NOT "
                         "refuse weapon / shield / torch equips (neither the 0x0F seat nor the equip-sink step).{}",
                         (ch17 || ch15) ? " The 0x0F seat still installs for ch.17's selection-time refusal and/or "
                                          "ch.15's weapon admission." : "");
            if (!ch17 && !ch15) return;
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
            apmf::sources::NoteVfunc(apmf::sources::Facet::kEquipWeapon, fmt::format("HandBlock.{}.CheckShouldEquip", spec.tag),
                                     vt.address(), kCheckShouldEquip, SKSE::stl::unrestricted_cast<std::uintptr_t>(&CheckShouldEquipThunk),
                                     g_slots[i].orig);
            ++n;
        }
        g_seatArmed.store(n > 0, std::memory_order_release);
        if (n == static_cast<int>(kNumClasses)) {
            spdlog::info("[handblock] INSTALLED: CheckShouldEquip (0x0F) on the Melee, Ranged, Shield, Torch and "
                         "OneHandedBlock item vtables (each slot held this build's engine function). Hand-claim block "
                         "{}: a hand held by a live cast claim (a "
                         "deny-only floor included; a spells-only floor, ABI v20, refuses spell-like items only) refuses "
                         "every weapon / shield / torch candidate competing for it. ch.17 selection-time refusal {}: "
                         "a candidate the actor's declared equip set refuses is not selected. ch.15 weapon admission "
                         "{}: another weapon / shield / torch for a hand a kIntent_Equipment claim's weapon holds is "
                         "not selected. "
                         "The engine answers first, only its YES turns to NO. The equip-sink step rides the ch.17 "
                         "seat (its own [apmf][equip-sink] INSTALLED line says whether that seat is live).",
                         g_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF (bHandClaimBlocksEquip=0)",
                         ch17 ? "ON" : "OFF (bEquipAuthority=0)",
                         !ch15 ? "OFF (bEquipmentWeaponAdmission=0)"
                         : g_ch15Observe.load(std::memory_order_relaxed) ? "OBSERVE-ONLY (bEquipmentWeaponObserveOnly=1)"
                                                                          : "ON");
        } else {
            spdlog::error("[handblock] PARTIAL: {}/{} weapon-class 0x0F seats installed; refused: {}. The equip-sink "
                          "step still refuses an equip into a held hand on every path.",
                          n, kNumClasses, refused.empty() ? "-" : refused);
        }
    }
}
