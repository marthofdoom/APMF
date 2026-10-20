#include "PCH.h"
#include "channels/SubstrateEquip.h"

#include "APMF_API.h"
#include "core/Clock.h"
#include "core/Log.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>

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
// WHY BOTH NODES, AND NO OTHER: the 10-19 dry runs logged `LEAF DENIED CombatBehaviorAcquireResource<2>`
// for every holder (_evidence/2026-10-19-lorerim-town-substrate/APMF.log, 25 of 25): the allow-list cut the
// whole Action branch at node 1, before the equip context, the equip leaf AND the Magic link, so no
// substrate cast could ever be reached, spell in hand or not. Node 1 has a second instance, "Acquire
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
            const bool runtimeOk =
                REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) || REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97);
            const bool ini = IniInt("CombatSubstrate", "bSubstrateEquip", 1) != 0;
            g_enabled.store(runtimeOk && ini, std::memory_order_release);
            if (!runtimeOk)
                spdlog::warn("[substrate] the spell-equip path (AcquireResource<2> \"Action\" + EquipSpell) is verified on "
                             "exactly 1.6.1170 and 1.5.97 only: it stays DENIED on this runtime.");
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
}
