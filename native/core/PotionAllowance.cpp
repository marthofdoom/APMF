#include "PCH.h"
#include "core/PotionAllowance.h"
#include "core/Allowance.h"          // SeatVerified(), DerivesFrom(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/ControlMap.h"         // TryGetOwningClaimBasis: the winning ch.30 claim (one RCU read)
#include "core/FacetAttribution.h"   // Decide / InsideSource / CallerBelow / LineDue / WhyStands
#include "core/Log.h"
#include "core/Sources.h"

#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>   // _ReturnAddress: the drink seat's caller
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Win32, declared by hand (the PCH does not pull in <Windows.h>; core/Sources.cpp's one-line import).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// ch.30 POTION ALLOWANCE -- THE SELECTION AND DRINK SEATS (ABI v37, ClickUp 1dfcuabcqpf).
//
// WHY A SEPARATE FACET. Before ch.30 the only thing that could refuse an NPC's potion was the ch.8 allow-list
// (core/CastGate.cpp, CheckCast on the instant caster), and only while a client held a ch.8 claim on that
// actor. MFO drops drugs (any Hostile / Detrimental effect) from its allow-list, so a follower with a cast
// gambit stopped drinking Skooma; a follower with no claim still drank it. ch.30 is the complete source gate:
// a client that holds NOTHING but this claim refuses the potions its predicate matches.
//
// THE ENGINE PATH (read on the unpacked 1.6.1170 and 1.5.97 executables, agentlog apmf-potion-allowance.md).
//   DrinkPotion. Actor::DrinkPotion is Character vtable slot 0x10F (AE 38746 0x6C2DD0 / SE 37797 0x630E80, the
//   same 0x10F index on both; PlayerCharacter has its own override, AE 40690 / SE 39604, which calls the base
//   function directly, so the player never reaches the Character slot). Body, identical on both builds:
//   `potion == null -> return false`; the instant caster (Actor vfunc 0x5C with 3) casts the potion (AE 34409);
//   RemoveItem (vfunc 0x56) takes one from the inventory; `return true`. bool(Actor*, AlchemyItem*,
//   ExtraDataList*), rcx / rdx / r8, al.
//   Every `call [reg+0x878]` (slot 0x10F) in .text, both builds (a full FF /2 disp32 scan):
//     AE 38001+0x7B7 / SE 36976+0x82F  ActorEquipManager's apply step: after the equip event (AE 38386 with the
//                                      ScriptEventSourceHolder), DrinkPotion; false jumps to the epilogue. Every
//                                      EquipObject of an ALCH ends here: EquipObject (AE 38894 / SE 37938) or
//                                      the list sibling -> the worker (AE 38929 +0x289 / SE 37974 +0x235 -> its
//                                      tail 37963 +0x4D) -> the apply. The apply's other callers re-apply
//                                      persisted state (AE 0x22E860 / SE 0x1E1FB0, the 16073 family) or are an
//                                      orphan (AE 38919).
//     AE 49928+0xE6 / SE 48984+0xBE    the combat behaviour tree's DrinkPotion leaf. AE 49928 IS the leaf's act
//                                      (CombatBehaviorTreeNodeObject<CombatBehaviorDrinkPotion> slot 0x02); on
//                                      SE the act (48997) jumps to 48987, which calls the helper 48984. The
//                                      potion is the context's +0x20, the extra list null. false fails the node
//                                      (AE 0x85E0A0), true notifies the context's item (its vfunc 0x58).
//     AE 49917 / 49920                 two more copies of the leaf body with NO reference at all (.pdata
//                                      only): orphans, never run.
//   Selection. The combat AI only reaches the leaf for a potion CombatInventory SELECTED: the item is one of
//   the 15 CombatInventoryItemMagicT<CombatInventoryItemPotion, CombatMagicCaster*> classes (one per caster
//   archetype), and CombatInventory's evaluate (AE 44899 / SE 43666) and pre-loop (AE 44868 / SE 43637) ask
//   each candidate's slot 0x0F (CheckShouldEquip, bool(item*, CombatController*)) and skip it on NO -- the
//   seat core/HandBlock.cpp and core/EquipGate.cpp already use for weapons and spells. A potion's 0x0F is
//   CombatInventoryItemMagic's `true` AND the caster archetype's own check (AE Restore: 0x831EF0 ->
//   0x8199A0, 0x81F7C0).
//
// THE SEATS. (1) SELECTION: write_vfunc 0x0F on the 15 potion vtables. Engine first; a YES for a potion the
// WINNING ch.30 claim's predicate matches becomes NO, so the AI picks among the potions that remain (the
// composition: its own choice from a narrowed set, never a substituted potion). Ranked on the Cast facet against
// the answer's source beneath the seat (InsideSource: the engine, or a module hooked there first; two of the
// vtables, Invisibility and BoundItem, carry ch.24's 0x0F bracket underneath, which is Harbinger's own and
// reads as Engine). (2) DRINK: write_vfunc 0x10F on the Character vtable. A matching potion is refused by
// RETURNING FALSE WITHOUT CALLING THE ENGINE: nothing is cast and nothing is removed from the inventory, and
// both engine callers treat false as an ordinary failure. Ranked on the Cast facet against the drink's SOURCE,
// named from the thunk's return address:
//   the leaf's site + 6                    -> Engine (the combat AI), road CombatAI
//   the apply's site + 6, apply called from the worker's site + 5
//                                          -> the EquipObject caller (facetattr::CallerBelow on EquipObject's or
//                                             the list sibling's worker call + 5): Papyrus EquipItem (AE 54661 /
//                                             SE 53861) and the console (AE 22351 / SE 21869) -> DirectCalls;
//                                             the player's menus (AE 29383 / 29346 / 38907, SE 28629 / 28593 /
//                                             37951) -> PASS (player agency); any other address -> its module
//                                             (SkyrimSE.exe = Engine, skse64 = DirectCalls, a DLL = that
//                                             plugin), road Equip. The caller is identified by the .pdata row
//                                             that contains it, read once at Install from SkyrimSE.exe's own
//                                             exception directory: each of those functions' EquipObject calls
//                                             sits in the function's primary row (checked on both builds).
//   the apply's site + 6, called from elsewhere
//                                          -> that caller's module (Engine), road Engine
//   anything else                          -> the module of the return address, road Direct
// A source that cannot be named is the degrade rule (allowed and alerted; [UnknownSources] Cast=override).
// Why the Cast facet: the potion's effect is delivered as an instant-caster cast, and the only potion deny
// before ch.30 (ch.8's allow-list at CheckCast) already ranks there, so a user's Cast priorities mean the same
// thing for both.
//
// NEVER MASKED. A refused drink logs (rate-limited) which potion, why it matched, the road and the source; a
// match that stands logs why it stands; observe-only logs what it would have refused. The per-actor counters
// are GetPotionAllowanceState's.
//
// RUNTIMES. Exactly 1.6.1170 and 1.5.97: every id above is a verified row (tools/verified_addresses/spec.json,
// "PotionAllowance.*" and the 15 "CombatInventoryItemMagicT.Potion*" rows, plus the EquipSink rows reused for
// EquipObject's sites and the caller functions). 1.7.104 is REFUSED BY NAME before any id is resolved: the
// fork's MIT id table carries the vtables but not DrinkPotion (38746), the leaf (49928) or the apply (38001),
// and an id miss there is fatal. VR and every other build: refused.
// ============================================================================

namespace apmf::potionallowance {

    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";
        using apmf::sources::Facet;

        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify (ENGINE_NOTES §0.29)");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point (0x68)");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10, "CombatInventoryItem::item moved");

        constexpr std::size_t kSlotShouldEquip = 0x0F;
        constexpr std::size_t kSlotDrink       = 0x10F;   // Character vtable, AE and SE (0x6C2DD0 / 0x630E80)

        using ShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);
        using Drink_t       = bool (*)(RE::Actor*, RE::AlchemyItem*, RE::ExtraDataList*);

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_notInstalledReason{ "not installed yet (kDataLoaded has not run)" };
        bool                     g_iniObserve = false;   // written once at Install, before any thunk runs
        bool                     g_iniProbe   = false;

        // Written once at Install, before the first write_vfunc; read-only afterwards.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_origShouldEquip;   // vtable -> previous 0x0F
        Drink_t        g_origDrink  = nullptr;
        std::uintptr_t g_leafRet    = 0;   // the leaf's call + 6
        std::uintptr_t g_applyRet   = 0;   // the apply's call + 6
        std::uintptr_t g_workerRet  = 0;   // the worker's call of the apply + 5
        std::uintptr_t g_eqObjRet   = 0;   // EquipObject's worker call + 5
        std::uintptr_t g_eqListRet  = 0;   // the list sibling's worker call + 5
        struct Range { std::uintptr_t lo = 0, hi = 0; const char* name = ""; int kind = 0; };   // kind 1 script, 2 console, 3 menu
        std::array<Range, 5> g_callers{};

        // ---- the declarations (any-thread writers under g_wmx; readers: the published snapshot) ----
        struct Decl {
            std::uint32_t deny  = 0;
            std::uint32_t flags = 0;
            std::uint32_t kwCount = 0;
            std::array<RE::FormID, APMF_API::kMaxPotionDenyKeywords> kw{};
        };
        using DeclMap = std::unordered_map<APMF_API::Handle, Decl>;
        std::mutex                                          g_wmx;
        std::unordered_set<APMF_API::Handle>                g_claims;      // NoteClaim'd, not yet forgotten
        std::unordered_map<APMF_API::Handle, std::uint32_t> g_badLogged;   // last logged problem mask per handle
        DeclMap                                             g_declsW;      // writer copy
        std::atomic<std::shared_ptr<const DeclMap>>         g_decls{ std::make_shared<const DeclMap>() };
        std::atomic<bool>                                   g_anyDecl{ false };

        void PublishLocked() {
            g_decls.store(std::make_shared<const DeclMap>(g_declsW), std::memory_order_release);
            g_anyDecl.store(!g_declsW.empty(), std::memory_order_release);
        }

        // ---- per-actor counters (leaf mutex; nothing is called while held) ----
        constexpr std::size_t kActorsMax = 512;
        struct ActorState {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            bool             governed = false;
            std::uint32_t    selectionDenied = 0, drinkDenied = 0, rankStood = 0, wouldDeny = 0;
            RE::FormID       lastPotion = 0;
            std::uint32_t    lastRoad   = APMF_API::kPotionRoad_None;
        };
        std::mutex                                 g_amx;
        std::unordered_map<RE::FormID, ActorState> g_actors;
        std::atomic<bool>                          g_capLogged{ false };

        void Sat(std::uint32_t& c) { if (c != 0xFFFFFFFFu) ++c; }

        enum class Outcome { kDenied, kStood, kWould };
        void Count(RE::FormID fid, APMF_API::Handle h, RE::FormID potion, std::uint32_t road, Outcome o) {
            std::scoped_lock lk(g_amx);
            if (g_actors.size() >= kActorsMax && !g_actors.contains(fid)) {
                if (!g_capLogged.exchange(true))
                    spdlog::warn("[ch.30] the per-actor counter table is full ({} actors): 0x{} is NOT counted (its "
                                 "potions are still decided). Cleared at the next load. Logged once.",
                                 kActorsMax, apmf::log::Hex(fid));
                return;
            }
            auto& s = g_actors[fid];
            if (s.handle != h) s = ActorState{};   // a different claim: its own counters
            s.handle     = h;
            s.governed   = true;
            s.lastPotion = potion;
            s.lastRoad   = road;
            if (o == Outcome::kStood) Sat(s.rankStood);
            else if (o == Outcome::kWould) Sat(s.wouldDeny);
            else if (road == APMF_API::kPotionRoad_Selection) Sat(s.selectionDenied);
            else Sat(s.drinkDenied);
        }

        const char* RoadName(std::uint32_t r) {
            switch (r) {
            case APMF_API::kPotionRoad_Selection: return "Selection";
            case APMF_API::kPotionRoad_CombatAI:  return "CombatAI";
            case APMF_API::kPotionRoad_Equip:     return "Equip";
            case APMF_API::kPotionRoad_Engine:    return "Engine";
            case APMF_API::kPotionRoad_Direct:    return "Direct";
            default:                              return "None";
            }
        }

        const char* NameOf(const RE::TESForm* f) {
            if (!f) return "?";
            const char* n = f->GetName();
            return n && *n ? n : "?";
        }

        bool HasKw(const RE::BGSKeywordForm* kf, const Decl& d) {
            if (!kf || !kf->keywords) return false;
            for (std::uint32_t i = 0; i < kf->numKeywords; ++i) {
                const auto* k = kf->keywords[i];
                if (!k) continue;
                const RE::FormID id = k->GetFormID();
                for (std::uint32_t j = 0; j < d.kwCount; ++j)
                    if (d.kw[j] && d.kw[j] == id) return true;
            }
            return false;
        }

        // THE PREDICATE. Reads the item's own record data only (immutable after load). Returns the rule that
        // matched (for the log), or nullptr.
        const char* Matches(const Decl& d, const RE::AlchemyItem* p) {
            if (!p) return nullptr;
            const auto flags  = p->data.flags;
            const bool poison = flags.all(RE::AlchemyItem::AlchemyFlag::kPoison);
            const bool food   = flags.all(RE::AlchemyItem::AlchemyFlag::kFoodItem) && !poison;   // == IsFood()
            if ((d.deny & APMF_API::kPotionDeny_Food) && food) return "food";
            if ((d.deny & APMF_API::kPotionDeny_Potions) && !food) return poison ? "a poison (every potion denied)" : "a potion (every potion denied)";
            const bool harmful = (d.deny & APMF_API::kPotionDeny_Harmful) != 0;
            const bool kws     = d.kwCount != 0;
            if (kws && HasKw(p, d)) return "a denied keyword on the item";
            if (harmful || kws) {
                for (const auto* e : p->effects) {
                    const auto* m = e ? e->baseEffect : nullptr;
                    if (!m) continue;
                    if (harmful && (m->IsHostile() || m->IsDetrimental())) return "a Hostile / Detrimental effect";
                    if (kws && HasKw(m, d)) return "a denied keyword on an effect";
                }
            }
            return nullptr;
        }

        // The winning claim's declaration for `fid` (copied out), or false.
        bool WinningDecl(RE::FormID fid, APMF_API::Handle& h, Decl& out) {
            if (fid == 0x14 || !g_anyDecl.load(std::memory_order_acquire)) return false;
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(fid, APMF_API::kIntent_PotionAllowance, p, basis, &h))
                return false;
            const auto snap = g_decls.load(std::memory_order_acquire);
            const auto it   = snap->find(h);
            if (it == snap->end()) return false;
            out = it->second;
            return true;
        }

        // What a match does once its road and source are known. Returns true = REFUSE.
        bool Rule(RE::Actor* a_actor, const RE::AlchemyItem* a_potion, APMF_API::Handle h, const Decl& d,
                  const char* why, std::uint32_t road, apmf::sources::SourceId src, const char* srcDetail) {
            const RE::FormID fid = a_actor->GetFormID();
            const RE::FormID pid = a_potion->GetFormID();
            const bool       sel = road == APMF_API::kPotionRoad_Selection;
            const std::uint32_t key = (sel ? 0x100000u : 0x200000u) | (pid & 0xFFFFFu);
            if (g_iniObserve || (d.flags & APMF_API::kPotionAllow_ObserveOnly)) {
                Count(fid, h, pid, road, Outcome::kWould);
                if (apmf::facetattr::LineDue(Facet::kCast, fid, key | 0x400000u))
                    spdlog::info("[ch.30] 0x{} '{}' WOULD refuse {} '{}' 0x{} ({}; road={} source={}) -- observe-only "
                                 "({}). Logged at most once per 5 s per actor and potion.",
                                 apmf::log::Hex(fid), NameOf(a_actor), sel ? "the selection of" : "the drink of",
                                 NameOf(a_potion), apmf::log::Hex(pid), why, RoadName(road),
                                 apmf::facetattr::NameOr(src, srcDetail),
                                 g_iniObserve ? "[PotionAllowance] bObserveOnly=1" : "kPotionAllow_ObserveOnly");
                return false;
            }
            const auto v = apmf::facetattr::Decide(Facet::kCast, h, src, fid, srcDetail);
            if (!v.enforce) {
                Count(fid, h, pid, road, Outcome::kStood);
                if (apmf::facetattr::LineDue(Facet::kCast, fid, key | 0x800000u))
                    spdlog::info("[ch.30] 0x{} '{}' {} '{}' 0x{} STANDS ({}; road={}): {}. Logged at most once per 5 s "
                                 "per actor and potion.",
                                 apmf::log::Hex(fid), NameOf(a_actor), sel ? "selection of" : "drink of",
                                 NameOf(a_potion), apmf::log::Hex(pid), why, RoadName(road),
                                 apmf::facetattr::WhyStands(Facet::kCast, v, h));
                return false;
            }
            Count(fid, h, pid, road, Outcome::kDenied);
            if (apmf::facetattr::LineDue(Facet::kCast, fid, key))
                spdlog::info("[ch.30] 0x{} '{}' {} DENIED: '{}' 0x{} ({}) road={} source={} claim h={}. Logged at most "
                             "once per 5 s per actor and potion.",
                             apmf::log::Hex(fid), NameOf(a_actor), sel ? "SELECTION" : "DRINK", NameOf(a_potion),
                             apmf::log::Hex(pid), why, RoadName(road), apmf::facetattr::NameOr(src, srcDetail), h);
            return true;
        }

        // ---- the drink's source, from the thunk's return address ----
        struct Src {
            std::uint32_t            road   = APMF_API::kPotionRoad_Direct;
            apmf::sources::SourceId  src    = apmf::sources::kNoSource;
            const char*              detail = "";
            bool                     pass   = false;   // the player's own menu use, or APMF itself: never refused
            const char*              passWhy = "";
        };

        Src Classify(std::uintptr_t a_ret) {
            namespace s = apmf::sources;
            Src r;
            if (a_ret == g_leafRet) {
                r.road = APMF_API::kPotionRoad_CombatAI;
                r.src  = s::kEngineSource;
                return r;
            }
            if (a_ret == g_applyRet) {
                const auto c1 = apmf::facetattr::CallerBelow(g_applyRet);
                if (!c1.found || c1.addr != g_workerRet) {
                    // The apply reached without the worker: the engine's persisted-state re-apply.
                    r.road   = APMF_API::kPotionRoad_Engine;
                    r.src    = c1.found ? (c1.self ? s::kNoSource : c1.src) : s::kNoSource;
                    r.detail = "ch.30 drink: the equip apply step's caller (not the equip worker)";
                    return r;
                }
                r.road   = APMF_API::kPotionRoad_Equip;
                auto c2  = apmf::facetattr::CallerBelow(g_eqObjRet);
                if (!c2.found) c2 = apmf::facetattr::CallerBelow(g_eqListRet);
                if (!c2.found) {
                    r.detail = "ch.30 drink: no EquipObject frame found above the equip worker";
                    return r;
                }
                if (c2.self) {
                    r.pass    = true;
                    r.passWhy = "Harbinger's own equip";
                    return r;
                }
                for (const auto& g : g_callers) {
                    if (c2.addr < g.lo || c2.addr >= g.hi) continue;
                    if (g.kind == 3) {
                        r.pass    = true;
                        r.passWhy = "the player's own menu use (player agency)";
                        return r;
                    }
                    r.src    = s::kDirectSource;   // Papyrus EquipItem / the console
                    r.detail = g.name;
                    return r;
                }
                r.src    = c2.src;
                r.detail = "ch.30 drink: the EquipObject caller is in no module";
                return r;
            }
            if (apmf::sources::IsSelf(a_ret)) {
                r.pass    = true;
                r.passWhy = "Harbinger itself";
                return r;
            }
            r.road   = APMF_API::kPotionRoad_Direct;
            r.src    = s::ForAddress(a_ret, s::Kind::kPlugin);
            r.detail = "ch.30 drink: the DrinkPotion caller is in no module";
            return r;
        }

        // ---- SEAT 1: the combat AI's selection (slot 0x0F) ----
        bool ShouldEquipThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_origShouldEquip.find(vt);
            if (oit == g_origShouldEquip.end()) return false;   // a foreign object: the benign "don't equip"
            const bool engineSays = reinterpret_cast<ShouldEquip_t>(oit->second)(a_this, a_cc);
            if (!engineSays || !a_cc || !g_anyDecl.load(std::memory_order_acquire)) return engineSays;
            try {
                auto* potion = a_this->item ? a_this->item->As<RE::AlchemyItem>() : nullptr;
                if (!potion) return engineSays;
                auto  attPtr = a_cc->attackerHandle.get();
                auto* actor  = attPtr.get();
                if (!actor) return engineSays;
                APMF_API::Handle h = APMF_API::kInvalidHandle;
                Decl             d;
                if (!WinningDecl(actor->GetFormID(), h, d)) return engineSays;
                const char* why = Matches(d, potion);
                if (!why) return engineSays;
                const auto inside = apmf::facetattr::InsideSource(oit->second);
                if (Rule(actor, potion, h, d, why, APMF_API::kPotionRoad_Selection, inside,
                         "ch.30 selection: a module hooked inside slot 0x0F (unnamed trampoline)"))
                    return false;
            } catch (...) {
            }
            return engineSays;
        }

        // ---- SEAT 2: every drink (Character slot 0x10F) ----
        bool DrinkThunk(RE::Actor* a_actor, RE::AlchemyItem* a_potion, RE::ExtraDataList* a_extra) {
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());   // FIRST: the genuine caller
            if (a_actor && a_potion) {
                try {
                    APMF_API::Handle h = APMF_API::kInvalidHandle;
                    Decl             d;
                    const bool       claimed = WinningDecl(a_actor->GetFormID(), h, d);
                    const char*      why     = claimed ? Matches(d, a_potion) : nullptr;
                    if (why) {
                        const Src s = Classify(ret);
                        if (s.pass) {
                            if (apmf::facetattr::LineDue(Facet::kCast, a_actor->GetFormID(), 0x300000u | (a_potion->GetFormID() & 0xFFFFFu)))
                                spdlog::info("[ch.30] 0x{} '{}' drink of '{}' 0x{} ({}) PASSES: {}.",
                                             apmf::log::Hex(a_actor->GetFormID()), NameOf(a_actor), NameOf(a_potion),
                                             apmf::log::Hex(a_potion->GetFormID()), why, s.passWhy);
                        } else if (Rule(a_actor, a_potion, h, d, why, s.road, s.src, s.detail)) {
                            return false;   // refused: the engine is NOT called (nothing cast, nothing removed)
                        }
                    } else if (g_iniProbe && a_actor->GetFormID() != 0x14 &&
                               apmf::facetattr::LineDue(Facet::kCast, a_actor->GetFormID(), 0x500000u)) {
                        const Src s = Classify(ret);
                        spdlog::info("[ch.30][probe] 0x{} '{}' drinks '{}' 0x{}: road={} source={}{} -- {}. Logged at most "
                                     "once per 5 s per actor.",
                                     apmf::log::Hex(a_actor->GetFormID()), NameOf(a_actor), NameOf(a_potion),
                                     apmf::log::Hex(a_potion->GetFormID()), RoadName(s.road),
                                     apmf::facetattr::NameOr(s.src, "?"), s.pass ? " (pass)" : "",
                                     claimed ? "claimed, the predicate does not match" : "no ch.30 declaration");
                    }
                } catch (...) {
                }
            }
            return g_origDrink(a_actor, a_potion, a_extra);
        }

        // SkyrimSE.exe's own .pdata row whose BeginAddress is `fn` (the function's primary row). Read from the
        // loaded image's exception directory (PE32+ DataDirectory[3]); rows are sorted by BeginAddress.
        bool PdataRow(std::uintptr_t fn, std::uintptr_t& lo, std::uintptr_t& hi) {
            const std::uintptr_t base = REL::Module::get().base();
            const auto*          img  = reinterpret_cast<const std::uint8_t*>(base);
            std::int32_t lfanew = 0;
            std::memcpy(&lfanew, img + 0x3C, 4);
            const std::uint8_t* nt = img + lfanew;
            std::uint32_t sig = 0;
            std::uint16_t magic = 0;
            std::memcpy(&sig, nt, 4);
            std::memcpy(&magic, nt + 24, 2);
            if (sig != 0x00004550u || magic != 0x20Bu) return false;   // "PE\0\0", PE32+
            std::uint32_t dirRva = 0, dirSize = 0;
            std::memcpy(&dirRva, nt + 24 + 112 + 3 * 8, 4);
            std::memcpy(&dirSize, nt + 24 + 112 + 3 * 8 + 4, 4);
            if (!dirRva || dirSize < 12) return false;
            const std::uint32_t want = static_cast<std::uint32_t>(fn - base);
            std::size_t l = 0, r = dirSize / 12;
            while (l < r) {
                const std::size_t m = (l + r) / 2;
                std::uint32_t b = 0, e = 0;
                std::memcpy(&b, img + dirRva + m * 12, 4);
                std::memcpy(&e, img + dirRva + m * 12 + 4, 4);
                if (b == want) { lo = base + b; hi = base + e; return e > b; }
                if (b < want) l = m + 1; else r = m;
            }
            return false;
        }
    }  // namespace

    bool Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_notInstalledReason.load(std::memory_order_acquire); }

    void Install() {
        static bool s_once = false;
        if (s_once) return;
        s_once = true;
        const char* why = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (apmf::allowance::IsRuntime1_7_104())
            why = "1.7.104: the fork's id table does not carry DrinkPotion (38746), the DrinkPotion leaf (49928) or the "
                  "equip apply step (38001), so the drink seat's sites are not placed there";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170 or 1.5.97";
        else if (GetPrivateProfileIntA("PotionAllowance", "bPotionAllowance", 1, kIni) == 0)
            why = "[PotionAllowance] bPotionAllowance=0";
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.30] potion allowance seats NOT installed -- {}. kIntent_PotionAllowance claims are REFUSED.", why);
            return;
        }
        const bool ae = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);   // else exactly 1.5.97 (gated above)

        // VERIFY EVERYTHING BEFORE WRITING ANYTHING: a selection deny without the drink deny (or the reverse) is
        // not a complete deny, so the seat set is installed whole or not at all.
        bool ok = true;
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        ok = apmf::allowance::SeatVerified(vtChar.address(), "PotionAllowance.Character.DrinkPotion(0x10F)") && ok;
        const std::uintptr_t drinkFn = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37797, 38746) }.address();
        ok = apmf::allowance::SeatVerified(drinkFn, "PotionAllowance.DrinkPotion") && ok;
        const std::uintptr_t leaf   = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(48984, 49928) }.address() + (ae ? 0xE6 : 0xBE);
        const std::uintptr_t apply  = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(36976, 38001) }.address() + (ae ? 0x7B7 : 0x82F);
        const std::uintptr_t worker = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37963, 38929) }.address() + (ae ? 0x289 : 0x4D);
        const std::uintptr_t eqObj  = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37938, 38894) }.address() + (ae ? 0x170 : 0xE5);
        const std::uintptr_t eqList = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37937, 38893) }.address() + 0xBC;
        ok = apmf::allowance::SeatVerified(leaf, "PotionAllowance.Site.Leaf") && ok;
        ok = apmf::allowance::SeatVerified(apply, "PotionAllowance.Site.EquipApply") && ok;
        ok = apmf::allowance::SeatVerified(worker, "PotionAllowance.Site.WorkerApply") && ok;
        ok = apmf::allowance::SeatVerified(eqObj, "EquipSink.Site.EquipObject (ch.30 caller recovery)") && ok;
        ok = apmf::allowance::SeatVerified(eqList, "EquipSink.Site.EquipObjectList (ch.30 caller recovery)") && ok;

        struct CallerSpec { REL::RelocationID id; const char* name; int kind; };
        const CallerSpec kCallers[] = {
            { RELOCATION_ID(53861, 54661), "Script (Papyrus EquipItem)", 1 },
            { RELOCATION_ID(21869, 22351), "Console", 2 },
            { RELOCATION_ID(28629, 29383), "PlayerMenu", 3 },
            { RELOCATION_ID(28593, 29346), "PlayerMenu", 3 },
            { RELOCATION_ID(37951, 38907), "PlayerMenu", 3 },
        };
        static_assert(std::size(kCallers) == std::tuple_size_v<decltype(g_callers)>);
        std::array<Range, 5> callers{};
        for (std::size_t i = 0; i < std::size(kCallers); ++i) {
            const std::uintptr_t fn = REL::Relocation<std::uintptr_t>{ kCallers[i].id }.address();
            if (!apmf::allowance::SeatVerified(fn, fmt::format("EquipSink.Path.{} (ch.30 caller recovery)", kCallers[i].name))) {
                ok = false;
                continue;
            }
            Range rg;
            rg.name = kCallers[i].name;
            rg.kind = kCallers[i].kind;
            if (!PdataRow(fn, rg.lo, rg.hi)) {
                spdlog::error("[ch.30] no .pdata row of SkyrimSE.exe begins at the {} function (RVA 0x{}): the caller "
                              "recovery cannot name it.", kCallers[i].name, apmf::log::Hex(fn - REL::Module::get().base(), 0));
                ok = false;
                continue;
            }
            callers[i] = rg;
        }

        const REL::VariantID kVtables[] = {
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterArmor_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterOffensive_[0],
        };
        REL::Relocation<void*> expectedTD{ RE::RTTI_CombatInventoryItem };
        ok = apmf::allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(expectedTD.get()),
                                           "PotionAllowance.RTTI.CombatInventoryItem") && ok;
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            if (!apmf::allowance::SeatVerified(vt.address(), "PotionAllowance.CombatInventoryItemMagicT.Potion (0x0F)")) {
                ok = false;
                continue;
            }
            if (!apmf::allowance::DerivesFrom(vt.address(), expectedTD.get())) {
                spdlog::error("[ch.30] potion item vtable RVA 0x{} does NOT derive CombatInventoryItem.",
                              apmf::log::Hex(vt.address() - REL::Module::get().base(), 0));
                ok = false;
            }
        }
        if (!ok) {
            g_notInstalledReason.store("the address self-check refused a seat row (listed above)", std::memory_order_release);
            spdlog::error("[ch.30] potion allowance seats NOT installed (a row or check failed, listed above). "
                          "kIntent_PotionAllowance claims are REFUSED.");
            return;
        }

        g_iniObserve = GetPrivateProfileIntA("PotionAllowance", "bObserveOnly", 0, kIni) != 0;
        g_iniProbe   = GetPrivateProfileIntA("PotionAllowance", "bProbe", 0, kIni) != 0;
        g_leafRet    = leaf + 6;
        g_applyRet   = apply + 6;
        g_workerRet  = worker + 5;
        g_eqObjRet   = eqObj + 5;
        g_eqListRet  = eqList + 5;
        g_callers    = callers;

        // Slot 0x10F: the function there now. Another DLL may have hooked it first; that is chained, never refused.
        const std::uintptr_t cur = *reinterpret_cast<const std::uintptr_t*>(vtChar.address() + sizeof(void*) * kSlotDrink);
        if (cur != drinkFn) {
            char mod[128]{};
            spdlog::warn("[ch.30] Character slot 0x10F already holds 0x{} ({}), not DrinkPotion 0x{}: another DLL hooked "
                         "it first. Harbinger chains to it; that DLL's own drinks are named by its module.",
                         apmf::log::Hex(cur, 0), apmf::sources::ModuleNameOf(cur, mod, sizeof(mod)),
                         apmf::log::Hex(drinkFn, 0));
        }

        // Write: the 0x0F seats first (the AI's selection), then the drink seat. Each original is stored BEFORE its
        // slot is written (the map reserved up front, so no rehash while a thunk could read it), so a thunk that runs
        // the instant its slot changes already finds what to call.
        g_origShouldEquip.reserve(2 * std::size(kVtables));
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            g_origShouldEquip[vt.address()] =
                *reinterpret_cast<const std::uintptr_t*>(vt.address() + sizeof(void*) * kSlotShouldEquip);
        }
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            const std::uintptr_t prev = vt.write_vfunc(kSlotShouldEquip, &ShouldEquipThunk);
            g_origShouldEquip[vt.address()] = prev;   // the same value (the key exists: no insert, no rehash)
            apmf::sources::NoteVfunc(Facet::kCast,
                                     fmt::format("PotionAllowance.Item@vt0x{:X}.ShouldEquip", vt.address() - REL::Module::get().base()),
                                     vt.address(), kSlotShouldEquip, SKSE::stl::unrestricted_cast<std::uintptr_t>(&ShouldEquipThunk),
                                     prev);
        }
        g_origDrink = reinterpret_cast<Drink_t>(cur);   // before the write, for the same reason
        g_origDrink = reinterpret_cast<Drink_t>(vtChar.write_vfunc(kSlotDrink, &DrinkThunk));
        apmf::sources::NoteVfunc(Facet::kCast, "PotionAllowance.Character.DrinkPotion", vtChar.address(), kSlotDrink,
                                 SKSE::stl::unrestricted_cast<std::uintptr_t>(&DrinkThunk),
                                 reinterpret_cast<std::uintptr_t>(g_origDrink));
        g_installed.store(true, std::memory_order_release);
        g_notInstalledReason.store("", std::memory_order_release);
        spdlog::info("[ch.30] potion allowance seats installed on {}: slot 0x0F on {} potion inventory-item vtables (the "
                     "combat AI's selection) and Character slot 0x10F (every drink; DrinkPotion 0x{}). Drink sources: leaf "
                     "ret 0x{}, equip apply ret 0x{} (worker 0x{}, EquipObject 0x{} / list 0x{}). observe-only {}, probe {}.",
                     ae ? "1.6.1170" : "1.5.97", std::size(kVtables),
                     apmf::log::Hex(drinkFn - REL::Module::get().base(), 0),
                     apmf::log::Hex(g_leafRet - REL::Module::get().base(), 0),
                     apmf::log::Hex(g_applyRet - REL::Module::get().base(), 0),
                     apmf::log::Hex(g_workerRet - REL::Module::get().base(), 0),
                     apmf::log::Hex(g_eqObjRet - REL::Module::get().base(), 0),
                     apmf::log::Hex(g_eqListRet - REL::Module::get().base(), 0), g_iniObserve, g_iniProbe);
    }

    const char* RefuseReason(RE::FormID actor) {
        if (!Installed()) return NotInstalledReason();
        if (actor == 0x14) return "the actor is the player (the player drinks what the player chooses)";
        return nullptr;
    }

    void NoteClaim(APMF_API::Handle h) {
        if (h == APMF_API::kInvalidHandle) return;
        std::scoped_lock lk(g_wmx);
        g_claims.insert(h);
    }

    void Set(APMF_API::Handle h, const APMF_API::APMF_PotionAllowance* a) {
        APMF_API::APMF_PotionAllowance m{};
        const bool clear = a == nullptr;
        if (!clear) std::memcpy(&m, a, sizeof(m));   // read and copied synchronously
        std::uint32_t bad = 0;                        // problem bits, logged when they change per handle
        std::string   what;
        Decl          d;
        if (!clear) {
            if (m.size < sizeof(APMF_API::APMF_PotionAllowance)) { bad |= 1; what += " size too small (refused, declaration cleared);"; }
            constexpr std::uint32_t kDenyAll = APMF_API::kPotionDeny_Harmful | APMF_API::kPotionDeny_Potions | APMF_API::kPotionDeny_Food;
            if (m.deny & ~kDenyAll) { bad |= 2; what += " unknown deny bits masked off;"; }
            d.deny = m.deny & kDenyAll;
            if (m.flags & ~static_cast<std::uint32_t>(APMF_API::kPotionAllow_ObserveOnly)) { bad |= 4; what += " unknown flag bits masked off;"; }
            d.flags = m.flags & APMF_API::kPotionAllow_ObserveOnly;
            std::uint32_t n = m.keywordCount;
            if (n > APMF_API::kMaxPotionDenyKeywords) { bad |= 8; what += " keywordCount above 8 clamped;"; n = APMF_API::kMaxPotionDenyKeywords; }
            for (std::uint32_t i = 0; i < n; ++i)
                if (m.keywords[i]) d.kw[d.kwCount++] = m.keywords[i];
            if (d.deny == 0 && d.kwCount == 0) { bad |= 16; what += " no rule declared (the claim denies nothing);"; }
        }
        bool logIt = false;
        {
            std::scoped_lock lk(g_wmx);
            if (!g_claims.contains(h)) return;   // a stale / unknown / non-ch.30 handle: silent no-op
            if (clear || (bad & 1)) g_declsW.erase(h);
            else g_declsW[h] = d;
            auto& last = g_badLogged[h];
            if (bad != last) { last = bad; logIt = bad != 0; }
            PublishLocked();
        }
        if (logIt)
            spdlog::warn("[ch.30] SetPotionAllowance h={}:{} applied: deny 0x{} flags 0x{} keywords {}. (Logged when "
                         "the problems change.)", h, what, apmf::log::Hex(d.deny, 0), apmf::log::Hex(d.flags, 0), d.kwCount);
        else if (clear)
            spdlog::info("[ch.30] SetPotionAllowance h={}: declaration cleared (the claim denies nothing).", h);
        else
            spdlog::info("[ch.30] SetPotionAllowance h={}: deny{}{}{} keywords {}{}.", h,
                         (d.deny & APMF_API::kPotionDeny_Harmful) ? " Harmful" : "",
                         (d.deny & APMF_API::kPotionDeny_Potions) ? " Potions" : "",
                         (d.deny & APMF_API::kPotionDeny_Food) ? " Food" : "", d.kwCount,
                         (d.flags & APMF_API::kPotionAllow_ObserveOnly) ? " (observe-only)" : "");
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
        g_capLogged.store(false, std::memory_order_relaxed);
        spdlog::debug("[ch.30] reset ({}): declarations and counters dropped", why);
    }

    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_PotionAllowanceInfo* out) {
        if (!Installed()) return APMF_API::kPotionState_Unavailable;
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(actor, APMF_API::kIntent_PotionAllowance, p, basis, &h))
            return APMF_API::kPotionState_None;
        const auto snap = g_decls.load(std::memory_order_acquire);
        const auto dit  = snap->find(h);
        if (dit == snap->end()) return APMF_API::kPotionState_None;
        APMF_API::APMF_PotionAllowanceInfo info{};
        info.deny         = dit->second.deny;
        info.flags        = dit->second.flags;
        info.keywordCount = dit->second.kwCount;
        std::uint32_t state = APMF_API::kPotionState_Declared;
        {
            std::scoped_lock lk(g_amx);
            if (auto it = g_actors.find(actor); it != g_actors.end() && it->second.handle == h) {
                const auto& s = it->second;
                if (s.governed) state = APMF_API::kPotionState_Governed;
                info.selectionDenied = s.selectionDenied;
                info.drinkDenied     = s.drinkDenied;
                info.rankStood       = s.rankStood;
                info.wouldDeny       = s.wouldDeny;
                info.lastPotion      = s.lastPotion;
                info.lastRoad        = s.lastRoad;
            }
        }
        if (out && out->size >= APMF_API::kPotionAllowanceInfoV37Size) {
            info.size = sizeof(info); info.state = state; info.ownerHandle = h;
            *out = info;
        }
        return state;
    }

}  // namespace apmf::potionallowance
