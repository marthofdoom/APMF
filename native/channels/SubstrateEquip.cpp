#include "PCH.h"
#include "channels/SubstrateEquip.h"

#include "APMF_API.h"
#include "core/Allowance.h"   // RuntimeSupported: the exact-build gate
#include "core/Clock.h"
#include "core/EquipGate.h"
#include "core/EquipSink.h"
#include "core/HandBlock.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// THE SUBSTRATE'S SPELL EQUIP -- how the combat AI puts a spell in hand, and the two nodes the
// allow-list opens for it (feat/apmf-substrate-equip, 2026-10-19; MFO S3 review F1, SEV-2).
//
// WHY: a substrate holder with a ch.8b cast claim had nothing that puts the spell in its hand. The floors
// are deny-only, 0x0F (core/EquipGate.cpp) is a gate, and the substrate's allow-list force-failed the
// equip leaf. marth: MFO does not equip ("we are porting everything to harbinger"), and Harbinger does
// not fire an EquipSpell either (gate the source, not the output): the engine's own AI equips.
//
// THE PATH, disassembled (AE = 1.6.1170 / SE = 1.5.97; ids from the Address Library, bodies read from
// our unpacked executables). It runs in every ordinary combat cast: it is the ONLY road from the tree's
// root to a CombatBehaviorContextMagic and the cast leaves (the "Magic" tree is linked from nowhere else).
//   CombatBehaviorTreeCombat (AE 47531) = Parallel "Combat Parallel" { Movement Link, Action Link }
//   CombatBehaviorTreeAction (AE 47687 0x8651E0 / SE 46460 0x7CDDC0):
//     Repeat "Action Repeat"
//      > AcquireResource<2> "Acquire Action Resource" (resource "Action", 100)      <- node 1 (allowed here)
//      > SpawnParallel "Use Equipment Parallel" (4 equipment-slot threads)
//      > CreateContextNode1<EquipContext> "Equipment Slot Context" (EquipContext(idx): its item is the
//        CombatInventory's SELECTED set entry (+0x148, CombatEquipment) whose slot mask has bit idx,
//        AE 48117 -> 44839)
//      > link "ActionEquipmentSelector"
//   CombatBehaviorTreeActionEquipmentSelector (AE 47688 0x865500 / SE 46461 0x7CE0E0):
//     Repeat > DynamicConditional (NOT EquipContext::Refresh, AE 48118) > Repeat
//      > Conditional "Check Equip" (EquipContext::CheckEquip, AE 48119 -> 44859)
//      > ValueChildSelector "Action Equipment Selector" on EquipContext::GetItemType (AE 48121)
//         Magic Sequence (type 4) = [ "Equip magic" = CombatBehaviorEquipSpell, "Magic Link" ]  <- node 2
//         Scroll / Staff / Shield / Melee = [ EquipObject, link ]; Ranged = [ EquipRangedWeapon, link ];
//         Shout = [ EquipShout, link ]; Potion = [ "Equip potion" (function node), UsePotion link ];
//         Empty = [ "Equip empty" (function node), "Equip pause" = Pause ]
//   CombatBehaviorTreeMagic (AE 49112 0x8A0720): ContextMagic > Casting Type Selector > ... > the cast leaves.
// EquipSpell's Enter (AE 48127 0x878260 / SE 46958 0x7E0BB0, act AE 0x867940 / SE 0x7D05F0 pushes 4):
//   item = the CURRENT context's item (thread+0x128 window, +8); if the actor's object in item->equipSlot
//   (+0x20; AE 39190 / SE 38230) is not item->item (+0x10), ActorEquipManager's internal EquipSpell
//   (AE 38896 / SE 37940; hand index from the slot: LeftHand 0, RightHand 1, Voice 2 -- AE 11419) equips
//   it; then the CombatInventory bookkeeping (AE 44860 / SE 43629), context+0x10 = 1, and Actor vfunc 0xA6
//   DrawWeaponMagicHands(true). Its update (AE 48128) succeeds once weaponState (+0xCC bits 5..7) >= drawn.
// So the leaf picks NOTHING: it equips exactly the item the context carries, into that item's own hand,
// and the item is whatever survived CombatInventory's selection -- which core/EquipGate.cpp's 0x0F seat
// (spell / staff items) and core/HandBlock.cpp's 0x0F seat (weapon / shield / torch items) gate per hand.
// Under a substrate that is: the claimed hand admits only the claim's driven form (proxy, else spell);
// the floored hand (a full kCastFlag_DenyHandOnly floor) admits no spell / staff and no weapon / shield /
// torch. That is the steering: the engine chooses, from a set that holds only the claimed spell.
//
// WHY BOTH NODES, AND NO OTHER: the allow-list classed AcquireResource<2> kParamDeny (core/BehaviorTreeCatalog.h),
// and the disassembly above puts its "Action" instance at the head of the Action branch, so the allow-list cut
// that whole branch at node 1, before the equip context, the equip leaf AND the Magic link: no substrate cast
// could ever be reached, spell in hand or not. (The 10-19 dry-run lines `LEAF DENIED
// CombatBehaviorAcquireResource<2>` are consistent with it but are not the proof: that line does not name the
// instance, and the "Movement" one is denied too -- REVIEW-BACKLOG APMF-B141.) Node 1 has a second instance, "Acquire
// Movement Resource" ("Movement", AE 49436 / SE 48431): it stays denied (the body is the package's). The
// two instances differ only in their resource name, a BSFixedString at node+0x28 -- written by the
// node's only two constructions per runtime (AE 0x865385 "Action" / 0x8ADEF9 "Movement"; SE 0x7CDF65 /
// 0x816B95: `lea rcx,[rbx+0x28]` + the BSFixedString copy, then `mov dword [rbx+0x30], 0x64`). Every other
// NodeObject on the opened path is a structural node (passes already) or a leaf that stays denied:
// EquipObject / EquipRangedWeapon / EquipShout (so no weapon, shield, torch, staff, scroll or shout is
// ever put in a hand: those sequences fail before their links), Pause, DrinkPotion. The non-NodeObject
// nodes on it (context creation, conditionals, value nodes, links and the EquipContext function node
// "Equip empty" / "Equip potion" = AE 48120 / SE 46952) do CombatInventory bookkeeping only: AE 44860
// updates the equipped-item list (+0x178) and calls the item's Equip / Unequip slots (0x11 / 0x12), which
// are empty functions on every CombatInventoryItem class except the five *Restore Unequips (a blackboard
// timer) -- no actor equip (REVIEW-BACKLOG APMF-B99).
//
// Threads: see the header. The node read (node+0x28) is the node's own member, alive while its act()
// runs; BSFixedString::c_str reads the pool entry the node holds a reference to (no lock).
// ============================================================================

namespace apmf::substrateequip {
    namespace {
        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";
        // AcquireResource<2>'s first parameter, the resource name (see the file header for the four
        // construction sites that prove it on both runtimes).
        constexpr std::size_t kResourceNameOff = 0x28;
        static_assert(sizeof(RE::BSFixedString) == 8, "the node's resource name is one BSFixedString pointer");

        std::atomic<bool> g_enabled{ false };
        bool              g_read = false;

        struct Rec {
            // shared with the combat thread
            std::atomic<RE::FormID>    id{ 0 };
            std::atomic<std::uint32_t> equipAllowed{ 0 }, actionAllowed{ 0 };
            std::atomic<std::uint64_t> firstEquipMs{ 0 }, firstActionMs{ 0 };
            // game thread only
            std::uint64_t beganMs = 0, reachedMs = 0;
            bool          loggedEquip = false, loggedAction = false, sawClaim = false;
            RE::FormID    spell = 0, proxy = 0;
            std::uint32_t flags = 0;
        };
        std::array<Rec, kMaxSlots> g_recs{};

        int IniInt(const char* a_sec, const char* a_key, int a_def) {
            char b[32]{};
            REX::W32::GetPrivateProfileStringA(a_sec, a_key, "", b, sizeof(b), kIni);
            if (!b[0]) return a_def;
            return std::atoi(b);
        }

        void ReadOnce() {
            if (g_read) return;
            g_read = true;
            // Exactly 1.6.1170, 1.5.97 or 1.7.104. 1.7.104 (2026-10-20): EquipSpell's Enter (0x88D510, AE 0x878260; the
            // DrawWeaponMagicHands call at +0x125), its act() (0x87CB80, AE 0x867940) and both AcquireResource<2>
            // constructions (0x87A5C5 "Action" in 0x87A420, AE 0x865385; 0x8C3469 "Movement" in 0x8C2900, AE 0x8ADEF9:
            // `lea rcx,[rbx+0x28]`) are instruction-identical to 1.6.1170, so node+0x28 and the path above hold there.
            const bool runtimeOk = apmf::allowance::RuntimeSupported();
            const bool ini = IniInt("CombatSubstrate", "bSubstrateEquip", 1) != 0;
            g_enabled.store(runtimeOk && ini, std::memory_order_release);
            if (!runtimeOk)
                spdlog::warn("[substrate] the spell-equip path (AcquireResource<2> \"Action\" + EquipSpell) is verified on "
                             "exactly 1.6.1170, 1.5.97 and 1.7.104 only: it stays DENIED on this runtime.");
            else if (!ini)
                spdlog::warn("[substrate] [CombatSubstrate] bSubstrateEquip=0: the spell-equip path stays DENIED, so a "
                             "substrate cast is never reached (the Action branch is cut at \"Acquire Action Resource\").");
        }

        // ASCII case-insensitive compare (BSFixedString's pool keeps the first spelling it interned).
        bool EqualsNoCase(const char* a_s, const char* a_lit) {
            if (!a_s) return false;
            for (; *a_s && *a_lit; ++a_s, ++a_lit) {
                const char x = (*a_s >= 'A' && *a_s <= 'Z') ? static_cast<char>(*a_s - 'A' + 'a') : *a_s;
                const char y = (*a_lit >= 'A' && *a_lit <= 'Z') ? static_cast<char>(*a_lit - 'A' + 'a') : *a_lit;
                if (x != y) return false;
            }
            return *a_s == 0 && *a_lit == 0;
        }

        bool Valid(int a_slot) { return a_slot >= 0 && a_slot < kMaxSlots; }

        const char* HandText(std::uint32_t a_flags) {
            if (a_flags & APMF_API::kCastFlag_DualCast) return "L+R";
            return (a_flags & APMF_API::kCastFlag_LeftHand) ? "L" : "R";
        }

        bool Holds(RE::Actor* a_actor, bool a_left, RE::FormID a_spell, RE::FormID a_proxy) {
            const auto* f = a_actor->GetEquippedObject(a_left);
            const RE::FormID id = f ? f->GetFormID() : 0;
            return id != 0 && (id == a_spell || id == a_proxy);
        }

        // Every hand the claim names holds its spell (or its delivery-flip proxy).
        bool Reached(RE::Actor* a_actor, const Rec& a_r) {
            if (!a_actor) return false;
            if (a_r.flags & APMF_API::kCastFlag_DualCast)
                return Holds(a_actor, true, a_r.spell, a_r.proxy) && Holds(a_actor, false, a_r.spell, a_r.proxy);
            return Holds(a_actor, (a_r.flags & APMF_API::kCastFlag_LeftHand) != 0, a_r.spell, a_r.proxy);
        }

        std::string InHands(RE::Actor* a_actor) {
            if (!a_actor) return "actor gone";
            const auto* r = a_actor->GetEquippedObject(false);
            const auto* l = a_actor->GetEquippedObject(true);
            return fmt::format("R=0x{} L=0x{}", apmf::log::Hex(r ? r->GetFormID() : 0),
                               apmf::log::Hex(l ? l->GetFormID() : 0));
        }
    }

    bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

    Node Classify(std::string_view a_leafName) {
        ReadOnce();
        if (!Enabled()) return Node::kNone;
        if (a_leafName == "CombatBehaviorEquipSpell") return Node::kEquipSpell;
        if (a_leafName == "CombatBehaviorAcquireResource<2>") return Node::kActionResource;
        return Node::kNone;
    }

    const char* NodeName(Node a_kind) {
        switch (a_kind) {
        case Node::kEquipSpell:     return "CombatBehaviorEquipSpell";
        case Node::kActionResource: return "CombatBehaviorAcquireResource<2> (\"Action\")";
        default:                    return "none";
        }
    }

    bool Allows(Node a_kind, const void* a_node) {
        if (!Enabled() || !a_node) return false;
        switch (a_kind) {
        case Node::kEquipSpell:
            return true;
        case Node::kActionResource: {
            const auto* name = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(a_node) +
                                                                          kResourceNameOff);
            return EqualsNoCase(name->c_str(), "Action");   // the string pool is case-insensitive
        }
        default:
            return false;
        }
    }

    void NoteAllowed(int a_slot, RE::FormID a_actor, Node a_kind) {
        if (!Valid(a_slot) || a_actor == 0) return;
        Rec& r = g_recs[a_slot];
        if (r.id.load(std::memory_order_acquire) != a_actor) return;
        auto& count = a_kind == Node::kEquipSpell ? r.equipAllowed : r.actionAllowed;
        auto& first = a_kind == Node::kEquipSpell ? r.firstEquipMs : r.firstActionMs;
        count.fetch_add(1, std::memory_order_relaxed);
        if (first.load(std::memory_order_relaxed) == 0) {
            std::uint64_t zero = 0;
            first.compare_exchange_strong(zero, apmf::clock::MonotonicMs(), std::memory_order_relaxed);
        }
    }

    void Begin(int a_slot, RE::FormID a_actor, std::uint64_t a_now) {
        if (!Valid(a_slot)) return;
        Rec& r = g_recs[a_slot];
        r.id.store(0, std::memory_order_release);
        r.equipAllowed.store(0, std::memory_order_relaxed);
        r.actionAllowed.store(0, std::memory_order_relaxed);
        r.firstEquipMs.store(0, std::memory_order_relaxed);
        r.firstActionMs.store(0, std::memory_order_relaxed);
        r.beganMs     = a_now;
        r.reachedMs   = 0;
        r.loggedEquip = r.loggedAction = r.sawClaim = false;
        r.spell = r.proxy = 0;
        r.flags = 0;
        r.id.store(a_actor, std::memory_order_release);
    }

    void Serve(int a_slot, RE::Actor* a_actor, RE::FormID a_spell, RE::FormID a_proxy, std::uint32_t a_flags,
               std::uint64_t a_now) {
        if (!Valid(a_slot) || !a_actor) return;
        Rec& r = g_recs[a_slot];
        if (r.id.load(std::memory_order_acquire) != a_actor->GetFormID()) return;
        r.sawClaim = true;
        r.spell    = a_spell;
        r.proxy    = a_proxy;
        r.flags    = a_flags;
        const RE::FormID driven = a_proxy ? a_proxy : a_spell;
        try {
            if (!r.loggedAction && r.actionAllowed.load(std::memory_order_relaxed) != 0) {
                r.loggedAction = true;
                spdlog::info("[substrate] 0x{} EQUIP allowed: {} for claimed spell 0x{} hand {} (+{} ms; the Action branch "
                             "now runs: equip context, equip selector, Magic link)",
                             apmf::log::Hex(a_actor->GetFormID()), NodeName(Node::kActionResource), apmf::log::Hex(driven),
                             HandText(a_flags), r.firstActionMs.load(std::memory_order_relaxed) - r.beganMs);
            }
            if (!r.loggedEquip && r.equipAllowed.load(std::memory_order_relaxed) != 0) {
                r.loggedEquip = true;
                spdlog::info("[substrate] 0x{} EQUIP allowed: {} for claimed spell 0x{} hand {} (+{} ms; the engine equips "
                             "the equip context's item, which only the claimed form can be on this hand)",
                             apmf::log::Hex(a_actor->GetFormID()), NodeName(Node::kEquipSpell), apmf::log::Hex(driven),
                             HandText(a_flags), r.firstEquipMs.load(std::memory_order_relaxed) - r.beganMs);
            }
        } catch (...) {
        }
        if (r.reachedMs == 0 && Reached(a_actor, r)) r.reachedMs = a_now;
    }

    void Closing(int a_slot, RE::FormID a_actor, RE::Actor* a_actorPtr, const char* a_how, std::uint64_t a_now) {
        if (!Valid(a_slot)) return;
        Rec& r = g_recs[a_slot];
        if (r.id.load(std::memory_order_acquire) != a_actor) return;
        if (r.sawClaim) {
            if (r.reachedMs == 0 && Reached(a_actorPtr, r)) r.reachedMs = a_now;
            const RE::FormID driven = r.proxy ? r.proxy : r.spell;
            const auto       eq     = r.equipAllowed.load(std::memory_order_relaxed);
            const auto       ac     = r.actionAllowed.load(std::memory_order_relaxed);
            try {
                if (r.reachedMs != 0) {
                    spdlog::info("[substrate] 0x{} EQUIP window end ({}): claimed spell 0x{} hand {} REACHED the hand at "
                                 "+{} ms (Action resource let through {}, EquipSpell let through {})",
                                 apmf::log::Hex(a_actor), a_how ? a_how : "?", apmf::log::Hex(driven), HandText(r.flags),
                                 r.reachedMs - r.beganMs, ac, eq);
                } else {
                    spdlog::warn("[substrate] 0x{} EQUIP window end ({}): claimed spell 0x{} hand {} did NOT reach the hand "
                                 "in {} ms (in hand now: {}; Action resource let through {}, EquipSpell let through {}{})",
                                 apmf::log::Hex(a_actor), a_how ? a_how : "?", apmf::log::Hex(driven), HandText(r.flags),
                                 a_now - r.beganMs, InHands(a_actorPtr), ac, eq,
                                 !Enabled()  ? "; the equip path is OFF"
                                 : ac == 0   ? "; the Action branch never ran for this holder"
                                 : eq == 0   ? "; the equip leaf never ran (the selection held no spell for the claimed hand)"
                                             : "");
                }
            } catch (...) {
            }
        }
        r.id.store(0, std::memory_order_release);
    }

    void Reset() {
        for (auto& r : g_recs) r.id.store(0, std::memory_order_release);
    }

    // =========================================================================================================
    // THE HANDS (review F1 / F2 of a0112b24; marth: "Harbinger sheathes/stows first", "Harbinger restores it").
    //
    // WHY: EquipSpell's Enter always calls DrawWeaponMagicHands(true) (AE 0x878385 / SE 0x7E0CF3), which draws
    // whatever BOTH hands hold, and its EquipSpell displaces a one-hander in the claimed hand (a two-hander or a
    // bow from both). So a client substrate takes every weapon, shield and torch off both hands before its entry
    // and puts exactly those copies back when it ends.
    //
    // THE PATH: Harbinger's established equip road, the one channels/EquipAuthority.cpp (ch.17, #17a condition 4)
    // uses: ActorEquipManager::UnequipObject / EquipObject on the main thread, queued, not forced, sounds on, never
    // a 3D reset; every equip inside equipsink::ApmfEquipScope (the sink logs it as APMF's own, `tls=1`; it is
    // NOT a bypass: the sink's verdict still applies). The copy is named by its extra data list, the engine's own
    // pointer from GetInventory, found by its ExtraUniqueID uid (equipsink::ExtraUid, the ABI v25 instance
    // identity) -- the uid is what is recorded, never the pointer (a list does not outlive an unequip safely). A
    // worn copy WITHOUT an ExtraUniqueID is recorded by form and restored by form (logged "no uid": any copy).
    //
    // THE WINDOW: Stow right after TakeSlot (game thread, Begin), BEFORE the floors publish; Enter waits (at most
    // kStowWaitMs) until no stowed item is in a hand, then calls StartCombat (its re-arm equip runs inside it).
    // While the substrate stands, the hand floors hold both hands, so the equip sink's hand-claim step (1a)
    // refuses every equip into them -- an MFO ch.17 re-equip of its declared weapon, StartCombat's re-arm, a
    // script -- and core/HandBlock.cpp's 0x0F seat keeps the AI from selecting one: the stow stands without a
    // re-assert.
    //
    // THE RESTORE (CloseSub / DropSub -> RestoreWhenFree, driven by PollHands every frame): each item goes back as
    // soon as NO live cast claim or floor holds a hand it competes for (handblock::HeldFor, the same test the
    // sink's step 1a applies -- an equip issued earlier would be refused by it). The floors release through
    // ControlMap's queue, so the floored hand frees on the next drain; the claimed hand frees when the client's
    // cast claim ends (a real-fight DROP keeps that claim by design: the other hand comes back at once, the
    // claimed hand when the claim ends). A restored item is verified worn within kRestoreVerifyMs, else
    // "restore FAILED" names the cause class (the sink refused it: a ch.17 declared set that leaves it out, a
    // ch.26 force, another hand claim). MFO's ch.17 declaration names the follower's own loadout, so the restore
    // is in-set (step 4); its own enforce pass re-equipping the same copy after the floors drop is a no-op
    // (EquipObject returns early for a worn list).
    //
    // RECORD LIFETIME: one record per actor (game-thread map). Dead -> DROPPED with a log line (the copies stay in
    // the inventory; a corpse needs nothing in hand). Unloaded -> kept, restored on the first frame he is loaded
    // again (one log line while waiting). Load / revert (ResetAll) -> DROPPED with a log line naming the copies:
    // the record describes the world the load replaced, and the copies are in the saved inventory; carrying it
    // across a load would need a co-save record (not built: a co-save format change needs its own ruling).
    // =========================================================================================================
    namespace {
        constexpr std::uint64_t kStowWaitMs      = 1500;   // the queued unequips apply on the actor's next AI update
        constexpr std::uint64_t kRestoreVerifyMs = 3000;   // the same hold ch.17 gives an issued equip (kPendingMaxMs)

        enum class ItemSt : std::uint8_t { kStowed, kIssued, kDone, kFailed };
        struct HandItem {
            RE::FormID    form      = 0;
            std::uint16_t uid       = 0;       // ExtraUniqueID of the worn copy (0 = none: restored by form)
            bool          left      = false;   // the hand it was in
            bool          bothHands = false;   // a two-hander / bow / crossbow: one item, both hands
            ItemSt        st        = ItemSt::kStowed;
            std::uint64_t issuedMs  = 0;
            std::string   why;
        };
        struct Stowed {
            std::vector<HandItem> items;
            bool                  restoring = false;
            std::uint64_t         stowMs = 0, closeMs = 0;
            std::string           how;
            bool                  loggedHeld = false, loggedUnloaded = false;
        };
        std::unordered_map<RE::FormID, Stowed> g_stowed;   // GAME THREAD ONLY
        std::string                            g_stowWhy;  // StowSettled's kFailed text (game thread)

        bool HandType(RE::FormType a_t) {
            return a_t == RE::FormType::Weapon || a_t == RE::FormType::Armor || a_t == RE::FormType::Light;
        }
        bool BothHands(const RE::TESForm* a_f) {
            const auto* w = a_f ? a_f->As<RE::TESObjectWEAP>() : nullptr;
            return w && (w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow());
        }
        const RE::BGSEquipSlot* HandSlot(bool a_left) {
            return RE::TESForm::LookupByID<RE::BGSEquipSlot>(a_left ? apmf::equipsink::kLeftHandEquipSlot
                                                                    : apmf::equipsink::kRightHandEquipSlot);
        }
        // Is this list the copy worn in that hand? Right (and a both-hands item): ExtraWorn. Left: a weapon is
        // ExtraWornLeft; a shield / torch is worn (ExtraWorn, or ExtraWornLeft).
        bool ListWornIn(const RE::ExtraDataList* a_xl, const RE::TESForm* a_obj, bool a_left, bool a_both) {
            if (!a_xl) return false;
            const bool r = a_xl->HasType(RE::ExtraDataType::kWorn), l = a_xl->HasType(RE::ExtraDataType::kWornLeft);
            if (!a_left || a_both) return r;
            return a_obj && a_obj->GetFormType() == RE::FormType::Weapon ? l : (l || r);
        }
        // The worn copy's list of `a_obj` in that hand, from a fresh snapshot (engine pointers; used at once).
        RE::ExtraDataList* WornList(RE::Actor* a_actor, RE::TESBoundObject* a_obj, bool a_left, bool a_both) {
            auto inv = a_actor->GetInventory([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [obj, data] : inv) {
                if (obj != a_obj || !data.second || !data.second->extraLists) continue;
                for (auto* xl : *data.second->extraLists)
                    if (ListWornIn(xl, a_obj, a_left, a_both)) return xl;
            }
            return nullptr;
        }
        // The copy carrying `a_uid` (0: nullptr = any copy). `a_carried`: any copy of the form is carried.
        RE::ExtraDataList* ListByUid(RE::Actor* a_actor, RE::TESBoundObject* a_obj, std::uint16_t a_uid, bool& a_carried) {
            a_carried = false;
            auto inv  = a_actor->GetInventory([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [obj, data] : inv) {
                if (obj != a_obj || data.first <= 0) continue;
                a_carried = true;
                if (a_uid == 0 || !data.second || !data.second->extraLists) return nullptr;
                for (auto* xl : *data.second->extraLists)
                    if (apmf::equipsink::ExtraUid(xl) == a_uid) return xl;
            }
            return nullptr;
        }
        bool InHand(RE::Actor* a_actor, const HandItem& a_h) {
            const auto* f = a_actor->GetEquippedObject(a_h.left && !a_h.bothHands);
            return f && f->GetFormID() == a_h.form;
        }
        bool Worn(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const HandItem& a_h) {
            if (a_h.uid == 0) return InHand(a_actor, a_h);
            bool carried = false;
            const auto* xl = ListByUid(a_actor, a_obj, a_h.uid, carried);
            return xl && ListWornIn(xl, a_obj, a_h.left, a_h.bothHands);
        }
        std::string ItemText(const HandItem& a_h) {
            return a_h.uid ? fmt::format("0x{}/uid 0x{}", apmf::log::Hex(a_h.form), apmf::log::Hex(a_h.uid, 4))
                           : fmt::format("0x{}/no uid", apmf::log::Hex(a_h.form));
        }
        std::string HandsText(const Stowed& a_r) {
            std::string l = "none", r = "none";
            for (const auto& h : a_r.items) {
                if (h.bothHands) r = ItemText(h) + " (both hands)";
                else (h.left ? l : r) = ItemText(h);
            }
            return fmt::format("L={} R={}", l, r);
        }
    }

    const char* OpenRefusal() {
        if (!Enabled()) return nullptr;   // the spell equip is off: nothing equips, nothing is drawn
        if (!apmf::equipgate::DenyCompleteActive())
            return "the spell equip needs the floored hand's spell refusal and it would not stand ([EquipGate] "
                   "EnableEquipDenyComplete=0, or the 0x0F gate is not installed on all 30 spell/staff items or its hand "
                   "slots did not resolve): the floored hand could take any spell (review F3 of a0112b24)";
        if (!apmf::handblock::Enabled())
            return "the spell equip stows the hands, and the hand-claim block that keeps them stowed is off ([HandBlock] "
                   "bHandClaimBlocksEquip=0): a stowed weapon could be re-equipped and drawn mid-window";
        if (!apmf::equipsink::Installed())
            return "the spell equip stows the hands, and the equip sink that keeps them stowed is not installed";
        return nullptr;
    }

    void Stow(RE::FormID a_actor, RE::Actor* a_ptr) {
        if (!Enabled() || !a_ptr || a_actor == 0) return;
        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) {
            spdlog::error("[substrate] 0x{} HANDS stow FAILED: ActorEquipManager singleton is null.", apmf::log::Hex(a_actor));
            return;
        }
        const auto now = apmf::clock::MonotonicMs();
        Stowed&    r   = g_stowed[a_actor];   // a record still waiting for its restore is kept (its items are off)
        r.restoring = false;
        r.stowMs    = now;
        r.how.clear();
        r.loggedHeld = r.loggedUnloaded = false;
        std::erase_if(r.items, [](const HandItem& h) { return h.st == ItemSt::kDone || h.st == ItemSt::kFailed; });
        for (auto& h : r.items) h.st = ItemSt::kStowed;   // an issued-but-unverified restore is re-checked below
        const RE::TESForm* rightObj = a_ptr->GetEquippedObject(false);
        for (const bool left : { false, true }) {
            RE::TESForm* f = a_ptr->GetEquippedObject(left);
            if (!f || !HandType(f->GetFormType())) continue;          // empty, or a spell: not drawn as a weapon
            const bool both = BothHands(f);
            if (left && both && f == rightObj) continue;              // one two-hander, read with the right hand
            auto* obj = f->As<RE::TESBoundObject>();
            if (!obj) continue;
            auto*         xl  = WornList(a_ptr, obj, left, both);
            const auto    uid = apmf::equipsink::ExtraUid(xl);
            const bool known = std::any_of(r.items.begin(), r.items.end(), [&](const HandItem& h) {
                return h.form == obj->GetFormID() && h.uid == uid && h.left == left;
            });
            mgr->UnequipObject(a_ptr, obj, xl, 1, HandSlot(left), /*queue*/ true, /*force*/ false, /*sounds*/ true,
                               /*applyNow*/ false);
            if (!known) {
                HandItem h{};
                h.form      = obj->GetFormID();
                h.uid       = uid;
                h.left      = left;
                h.bothHands = both;
                r.items.push_back(std::move(h));
            }
        }
        if (r.items.empty()) {   // the (empty) record stays: StowSettled's hands test keys on it
            spdlog::info("[substrate] 0x{} HANDS stowed: L=none R=none (no weapon, shield or torch in a hand)",
                         apmf::log::Hex(a_actor));
            return;
        }
        spdlog::info("[substrate] 0x{} HANDS stowed: {} (queued UnequipObject, main thread; the hand floors keep them off "
                     "until the restore)",
                     apmf::log::Hex(a_actor), HandsText(r));
    }

    StowState StowSettled(RE::FormID a_actor, RE::Actor* a_ptr, std::uint64_t a_now, const char*& a_why) {
        a_why = nullptr;
        const auto it = g_stowed.find(a_actor);
        if (it == g_stowed.end() || !a_ptr) return StowState::kClear;
        // GENERIC (review of 1f99de2b, SEV-4): no weapon, shield or torch in EITHER hand, whatever put it there --
        // not only the copies this record stowed (a client could equip another one before the floors published).
        std::string still;
        for (const bool left : { false, true }) {
            const auto* f = a_ptr->GetEquippedObject(left);
            if (f && HandType(f->GetFormType()))
                still += fmt::format("{}{}=0x{}", still.empty() ? "" : ", ", left ? "L" : "R", apmf::log::Hex(f->GetFormID()));
        }
        if (still.empty()) return StowState::kClear;
        if (a_now - it->second.stowMs < kStowWaitMs) return StowState::kWaiting;
        g_stowWhy = fmt::format("the hands were not stowed within {} ms (still in a hand: {}): the entry would draw them",
                                kStowWaitMs, still);
        a_why = g_stowWhy.c_str();
        return StowState::kFailed;
    }

    void RestoreWhenFree(RE::FormID a_actor, const char* a_how, std::uint64_t a_now) {
        const auto it = g_stowed.find(a_actor);
        if (it == g_stowed.end()) return;
        if (it->second.items.empty()) {   // nothing was stowed: nothing to put back
            g_stowed.erase(it);
            return;
        }
        it->second.restoring = true;
        it->second.closeMs   = a_now;
        it->second.how       = a_how ? a_how : "?";
        it->second.loggedHeld = it->second.loggedUnloaded = false;
    }

    void PollHands(std::uint64_t a_now) {
        if (g_stowed.empty()) return;
        auto* mgr = RE::ActorEquipManager::GetSingleton();
        for (auto it = g_stowed.begin(); it != g_stowed.end();) {
            Stowed&          r  = it->second;
            const RE::FormID id = it->first;
            if (!r.restoring) { ++it; continue; }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!actor || actor->IsDead()) {
                spdlog::warn("[substrate] 0x{} HANDS restore DROPPED ({}): {}; the stowed copies stay in the inventory: {}",
                             apmf::log::Hex(id), r.how, actor ? "the actor is dead" : "the actor no longer resolves",
                             HandsText(r));
                it = g_stowed.erase(it);
                continue;
            }
            if (!actor->Is3DLoaded()) {
                if (!r.loggedUnloaded) {
                    r.loggedUnloaded = true;
                    spdlog::info("[substrate] 0x{} HANDS restore waits ({}): the actor is not loaded; restored when he is: {}",
                                 apmf::log::Hex(id), r.how, HandsText(r));
                }
                ++it;
                continue;
            }
            bool pending = false;
            for (auto& h : r.items) {
                if (h.st == ItemSt::kDone || h.st == ItemSt::kFailed) continue;
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(h.form);
                if (!obj) {
                    h.st  = ItemSt::kFailed;
                    h.why = "the form no longer resolves";
                    continue;
                }
                if (h.st == ItemSt::kIssued) {
                    if (Worn(actor, obj, h)) { h.st = ItemSt::kDone; continue; }
                    if (a_now - h.issuedMs >= kRestoreVerifyMs) {
                        h.st  = ItemSt::kFailed;
                        h.why = fmt::format("not worn {} ms after the equip was issued: the equip sink refused it (a ch.17 "
                                            "declared set that leaves it out, a ch.26 force or another hand claim; see the "
                                            "[apmf][equip-obs] / [handblock] lines)", kRestoreVerifyMs);
                        continue;
                    }
                    pending = true;
                    continue;
                }
                const bool              weap   = obj->GetFormType() == RE::FormType::Weapon;
                const RE::BGSEquipSlot* slot   = (weap && !h.bothHands) ? HandSlot(h.left) : nullptr;
                const std::uint32_t     compet = apmf::equipsink::Categorize(obj, slot);
                apmf::handblock::HandHold hold{};
                if (apmf::handblock::HeldFor(id, compet, h.form, apmf::handblock::SpellLike(obj), hold)) {
                    if (!r.loggedHeld) {   // the sink would refuse it now: wait for the claim / floor to end
                        r.loggedHeld = true;
                        spdlog::info("[substrate] 0x{} HANDS restore waits ({}): {} competes for a hand still held "
                                     "(R: {}, L: {}); it goes back when that claim ends.",
                                     apmf::log::Hex(id), r.how, ItemText(h),
                                     apmf::handblock::HoldDesc(hold, APMF_API::kEquipCat_Right),
                                     apmf::handblock::HoldDesc(hold, APMF_API::kEquipCat_Left));
                    }
                    pending = true;
                    continue;
                }
                if (Worn(actor, obj, h)) {   // already back (a client or the engine put it on)
                    h.st  = ItemSt::kDone;
                    h.why = "already back in the hand";
                    continue;
                }
                bool carried = false;
                auto* xl = ListByUid(actor, obj, h.uid, carried);
                if (!carried) {
                    h.st  = ItemSt::kFailed;
                    h.why = "no copy of the form is carried any more";
                    continue;
                }
                if (h.uid != 0 && !xl) {
                    h.st  = ItemSt::kFailed;
                    h.why = "the stowed copy (its uid) is no longer carried; no other copy substituted";
                    continue;
                }
                if (!mgr) {
                    h.st  = ItemSt::kFailed;
                    h.why = "ActorEquipManager singleton is null";
                    continue;
                }
                {
                    apmf::equipsink::ApmfEquipScope scope;   // the sink logs it as APMF's own (not a bypass)
                    mgr->EquipObject(actor, obj, xl, 1, slot, /*queue*/ true, /*force*/ false, /*sounds*/ true,
                                     /*applyNow*/ false);
                }
                h.st       = ItemSt::kIssued;
                h.issuedMs = a_now;
                pending    = true;
            }
            if (pending) { ++it; continue; }
            std::string failed;
            for (const auto& h : r.items)
                if (h.st == ItemSt::kFailed) failed += fmt::format("{}{}: {}", failed.empty() ? "" : "; ", ItemText(h), h.why);
            if (failed.empty())
                spdlog::info("[substrate] 0x{} HANDS restored ({}, +{} ms after the close): {}", apmf::log::Hex(id), r.how,
                             a_now - r.closeMs, HandsText(r));
            else
                spdlog::warn("[substrate] 0x{} HANDS restore FAILED ({}): {} (stowed: {})", apmf::log::Hex(id), r.how,
                             failed, HandsText(r));
            it = g_stowed.erase(it);
        }
    }

    void ResetHands(const char* a_why) {
        for (const auto& [id, r] : g_stowed)
            if (!r.items.empty())
                spdlog::warn("[substrate] 0x{} HANDS restore DROPPED ({}): the world was replaced, so the record no longer "
                             "describes it; the stowed copies stay in the saved inventory: {}",
                             apmf::log::Hex(id), a_why ? a_why : "reset", HandsText(r));
        g_stowed.clear();
    }
}
