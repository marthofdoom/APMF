#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/Allowance.h"
#include "core/ControlMap.h"
#include "core/EquipGate.h"

#include <intrin.h>
#include <unordered_set>

// Win32 INI read for the one log-gate flag this file adds below. Declared by
// hand, exactly like core/AiCastSeats.cpp / core/CastSeats.cpp / core/Hook.cpp
// do -- PCH does not pull in <Windows.h>, and this is the single Win32 call
// this file needs.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// T2a -- CheckShouldEquip, the combat AI's per-item "should I put this in my
// hands?" permission bit. Docs/ALLOWANCE-TEMPLATE.md §3/§7.
//
// Hooks CombatInventoryItem::CheckShouldEquip (vtable slot 0x0F) on the 30
// concrete spell/staff CombatInventoryItemMagicT<item,caster> instantiations --
// the SAME set MFO's own CombatStyle.cpp equip gate patches, for the SAME
// reason: CombatInventoryItemMagic/...Staff are themselves ABSTRACT (pure
// CreateCaster; no live object ever dispatches through their own vtable), so
// only the concrete per-caster-category instantiations carry a real vtable to
// hook -- the same 14-way caster-category split CastGate/CasterConsent rides,
// times {Magic, Staff}, plus Armor (mage-armor spells are exactly what a
// caster re-arms; this concrete instantiation genuinely derives
// CombatInventoryItemMagic in the pinned headers, unlike the BARE
// VTABLE_CombatMagicCasterArmor symbol the §0.28 lesson excludes elsewhere).
//
// DELIBERATELY NOT the design doc's aspirational 87/"Melee-Ranged-Shield-Torch"
// count (Docs/ALLOWANCE-TEMPLATE.md §3/§4 table). Verified 2026-09-02 against
// the pinned upstream (CharmedBaryon/CommonLibSSE-NG): the CombatInventoryItem
// TYPE enum lists kMelee/kRanged/kShield/kTorch, but NO CONCRETE C++ CLASS for
// any of them exists in the headers -- there is no `CombatInventoryItemMelee.h`
// etc., so no `RE::VTABLE_CombatInventoryItemMelee` symbol exists to hook or
// RTTI-verify. This is the EXACT same finding MFO's own CombatStyle.cpp already
// records ("weapon items (Melee/Ranged) are absent too: their classes are NOT
// in the pinned headers"). Potion/Scroll/Shout DO have concrete header classes,
// but are deliberately excluded on GAMEPLAY grounds MFO's CombatStyle.cpp
// already proved out: a claimed SPELL's FormID never equals a potion's, so
// hooking potion/scroll/shout here would only ever be a false-positive block on
// combat drinking/shouting (the v1.0.32 lesson) -- never a true exclusivity
// check for a spell-select claim. Widen only on new header evidence + a
// deliberate new intent for weapon/shout selection (T1/future work).
//
// This replicates MFO's proven gate (CombatStyle.cpp's own CheckShouldEquip
// hook), but APMF-side and MUTEX-FREE: Allowed() is the lock-free RCU
// ControlMap read, never a mutex (contrast MFO CombatStyle.cpp:276, which
// takes one) -- exactly the threading discipline §5 requires of every T2
// thunk (all run on combat threads).
//
// SECOND FACET (2026-09-03): also honors a ch.15 kIntent_Equipment claim --
// the weapon-order side of MFO CombatStyle.cpp's SAME gate (its g_owned[..]
// .equipOrder path, CombatStyle.cpp:286-317). MFO holds a follower's hands to
// a chosen weapon by RequestEx'ing kIntent_Equipment with param.form = that
// weapon's FormID; while the claim stands, every spell/staff item this hook
// sees (subjectForm) is, by construction, never equal to a weapon FormID, so
// Allowed(kIntent_Equipment, subjectForm) denies ALL of them -- exactly
// CombatStyle.cpp's "every vtable this gate patches is hand-competing magic
// by construction" rule, without per-item-type dispatch.
//
// The one exemption MFO's gate carries (its WantedSpell check, line 305) is
// the off-hand loan: a gambit spell the follower is ALSO allowed to cast
// stays equippable even while the weapon order holds the stance. Ported
// here as: if `subjectForm` IS the actor's own actively-claimed ch.8
// kIntent_SelectSpell form, it is exempt from the ch.15 deny outright (and,
// as before, the ch.8 exclusivity check never denies its own claimed spell).
// This is what lets MFO retire its own g_owned/equipOrder/WantedSpell gate
// entirely in favor of two independent APMF claims (ch.8 + ch.15) whose
// deny decisions this ONE hook now combines.
//
// ch.8b SEAT 0x0F (feat/ai-cast-seats-impl, 2026-09-05) -- this file now hosts
// the FIRST of the five engine cast seats (the other four live in
// core/CastSeats.cpp, which explains the whole set). While a `kIntent_Cast` claim
// stands, the claimed heal's Restore item is answered ELIGIBLE here WITHOUT
// chaining, because the Restore templates' own `CheckShouldEquip` IS the vanilla
// self/foe health gate (`0x81f7c0`, which reads its target straight off the
// CombatController and can therefore never be redirected). That is the one
// non-chaining answer in this codebase; see the SEAT 0x0F block in the thunk and
// Docs/INVARIANTS.md #20. Every other path through this thunk is unchanged and
// still strictly engine-answer-first.
//
// PER-HAND (2026-09-0x, INVARIANTS #18): a `CombatInventoryItem` instance
// carries its OWN `itemSlot.equipSlot` (a real member, static_assert'd offset
// below) -- the AI sets this to the vanilla Left/Right Hand BGSEquipSlot when
// it builds the item for that hand. Comparing it against
// `BGSDefaultObjectManager`'s own Left/Right Hand default objects resolves
// which hand THIS CheckShouldEquip call is about, so the ch.8b cast-execution
// narrowing (`Allowance::AllowedCastForHand`) can be scoped to the claimed
// hand only, leaving the OTHER hand's re-arm decision fully AI-governed.
// ============================================================================

namespace apmf::equipgate {

    namespace {

        // Same layout guard MFO's CombatStyle.cpp/CasterConsent.cpp already
        // carry -- the combat thread may only read a CombatController member
        // BELOW 0x68 (the AE +8 layout bug, ENGINE_NOTES §0.29).
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the "
                      "SE/AE layout split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point "
                      "(0x68) -- its compiled offset is WRONG on AE runtimes");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10,
                      "CombatInventoryItem::item moved -- re-verify against the "
                      "pinned header before shipping");
        static_assert(offsetof(RE::CombatInventoryItem, itemSlot) == 0x20,
                      "CombatInventoryItem::itemSlot moved -- re-verify against the "
                      "pinned header before shipping (per-hand deny reads "
                      "itemSlot.equipSlot, INVARIANTS #18)");
        // ch.8b SEAT 0 REBUILD TRIGGER (RE notebook J7/J9): both plain CommonLib-
        // declared members (clib/include/RE/C/CombatController.h /
        // CombatInventory.h per the RE pass) -- no raw offset invented here, just
        // guarded exactly as #7 asks. `inventory` sits at 0x10, well below the AE
        // +0x68 divergence point, so it is layout-identical on SE and AE.
        static_assert(offsetof(RE::CombatController, inventory) == 0x10,
                      "CombatController::inventory moved -- re-verify against the "
                      "pinned header before shipping (SEAT 0 rebuild trigger)");
        static_assert(offsetof(RE::CombatInventory, dirty) == 0x1C4,
                      "CombatInventory::dirty moved -- re-verify against the pinned "
                      "header before shipping (SEAT 0 rebuild trigger)");

        using CheckShouldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);

        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;

        // ---- ch.8b / t2a PER-CALL LOG (2026-09-05): 0x0F previously had NO
        // per-call log at all -- the field session that produced the RE notebook's
        // J5 finding could not show whether this seat was ever reached for a
        // Restore item. Rate-limited (per actor+subject, matching every other
        // seat's cadence in this codebase) and INI-gated ([EquipGate]
        // EnableEquipGateLog, default 0 -- this hook is called for up to 30
        // vtables x every known spell/staff item, every rescore pass, so it can
        // be noisy; the SEAT 0x0F force/deny decisions below log unconditionally
        // at the same throttle regardless of this flag, since those are rare,
        // load-bearing decisions rather than routine traffic). ----
        std::atomic<bool> g_logEnabled{ false };
        std::mutex                                       g_rlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;

        // feat/equip-deny-complete (2026-09-05), own kill-switch, independent of
        // [CastSeats] EnableSeat0Classify (CastClassify.cpp:209) which that flag
        // keeps governing unchanged: this one only controls whether the NEW
        // deny-complete branch below fires, so a field session can isolate
        // classification from the completeness deny if either needs to be
        // ruled out separately. Default ON.
        std::atomic<bool> g_denyCompleteEnabled{ true };

        bool LogDue(RE::FormID a_actor, RE::FormID a_subject) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) | static_cast<std::uint64_t>(a_subject);
            std::scoped_lock lk(g_rlMx);
            auto& last = g_lastLogMs[key];
            if (now - last < 1500) return false;
            last = now;
            return true;
        }

        // Mirrors core/AiCastSeats.cpp's ResolveTypeName exactly (raw
        // MSVC-decorated name; no demangler exists anywhere in this codebase or
        // CommonLib). Never guesses -- returns nullptr on any missing RTTI link.
        const char* ResolveTypeName(std::uintptr_t a_vtableAddr) {
            if (!a_vtableAddr) return nullptr;
            auto* colPtr = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(a_vtableAddr - sizeof(void*));
            if (!colPtr) return nullptr;
            auto* td = colPtr->typeDescriptor.get();
            if (!td) return nullptr;
            return td->mangled_name();
        }

        // feat/0x0f-callsite-log: friendly name for the caller RVA captured below.
        // Known 0x0F call sites (this session's disassembly): 0x80fcd0 (pre-loop,
        // invoked at 0x813643) and 0x813af2/0x813d38/0x814270/0x8144b2 (selector).
        const char* CallSiteName(std::uintptr_t a_rva) {
            switch (a_rva) {
                case 0x80fcd0: return "pre-loop";
                case 0x813af2:
                case 0x813d38:
                case 0x814270:
                case 0x8144b2: return "selector";
                default:       return "unknown";
            }
        }
        // ch.8b SEAT 0x0F (feat/ai-cast-seats-impl): which of the 30 patched item
        // vtables are the RESTORE templates. Recorded at install from the two NAMED
        // symbols (never inferred) so the seat's non-chaining force is scoped to
        // exactly the vtables whose CheckShouldEquip IS the vanilla self/foe health
        // gate -- see the SEAT 0x0F block in the thunk below.
        std::unordered_set<std::uintptr_t> g_restoreVtables;
        std::atomic<bool> g_installed{ false };

        // Per-hand deny (INVARIANTS #18): the vanilla Left/Right Hand BGSEquipSlot
        // forms, resolved ONCE at install through CommonLib's own version-robust
        // singleton (RE::BGSDefaultObjectManager -- the SAME table the engine
        // itself uses to look these up; no hardcoded FormID). A concrete
        // CombatInventoryItem instance's OWN `itemSlot.equipSlot` (set by the AI
        // when IT builds that item for a specific hand) is compared against these
        // to resolve which hand THIS CheckShouldEquip call is deliberating for.
        std::atomic<RE::BGSEquipSlot*> g_leftHandSlot{ nullptr };
        std::atomic<RE::BGSEquipSlot*> g_rightHandSlot{ nullptr };

        bool EquipGateThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            // feat/0x0f-callsite-log: captured FIRST, before any other call, so it
            // is the genuine caller, not a frame this thunk itself pushed.
            const auto callSiteRva =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base();
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            // Foreign object -> "don't equip" is the benign default here,
            // exactly as MFO's CombatStyle.cpp gate treats it (the base impl
            // itself returns false for a fleeing combatant).
            if (oit == g_orig.end()) return false;
            const auto original = reinterpret_cast<CheckShouldEquip_t>(oit->second);

            // Resolve the deliberating actor, the subject form and the hand BEFORE the
            // engine call. The DENY half below still runs engine-answer-first (nothing
            // resolved here is acted on until after `original`); the ch.8b SEAT 0x0F
            // block is the one branch that must decide WITHOUT chaining, and it can
            // only do so if it has these three facts in hand first. See the seat's own
            // block for why chaining is structurally impossible there.
            RE::FormID           fid          = 0;
            RE::FormID           subjectForm  = 0;
            allowance::Hand      callerHand   = allowance::Hand::kUnknown;
            if (a_cc) {
                auto  attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>
                if (auto* actor = attPtr.get()) fid = actor->GetFormID();

                auto* item  = a_this->item;
                subjectForm = item ? item->GetFormID() : 0;

                // Per-hand deny (INVARIANTS #18): resolve which hand THIS item instance
                // is for from its own itemSlot.equipSlot -- a real struct member (see
                // the static_assert above), not an invented offset. Neither vanilla
                // hand slot (kEitherHandEquip, a null slot, or the singleton table not
                // yet resolved) degrades to kUnknown -- AllowedCastForHand then falls
                // back to the actor-wide floor, never a guess.
                auto*      slot = a_this->itemSlot.equipSlot;
                const auto lh   = g_leftHandSlot.load(std::memory_order_acquire);
                const auto rh   = g_rightHandSlot.load(std::memory_order_acquire);
                callerHand =
                    (slot && slot == lh) ? allowance::Hand::kLeft  :
                    (slot && slot == rh) ? allowance::Hand::kRight :
                                            allowance::Hand::kUnknown;
            }

            // ================================================================
            // ch.8b SEAT 0x0F -- WHICH ITEM ENTERS THE HANDS. THE ONE NON-CHAINING
            // ANSWER IN THIS CODEBASE. (feat/ai-cast-seats-impl; Docs/INVARIANTS.md
            // #20 states the exception and its three conditions.)
            //
            // WHY IT CANNOT CHAIN. The five concrete Restore ITEM templates OVERRIDE
            // CheckShouldEquip with `CombatInventoryItemMagic::CheckShouldEquip
            // (return true) && 0x81f7c0(ctrl, item)`. That static pre-check has NO
            // caster object -- it takes its target DIRECTLY off the CombatController
            // (`spell delivery == kSelf ? ctrl.attacker : ctrl.TARGET`) and runs
            // `ShouldRestore` on it. So the ORIGINAL's answer for a HEALTHY follower
            // fighting a HEALTHY foe is always NO, there is no seat between it and
            // the fields it reads, and `engineSays == false` above would return
            // before anything downstream ran. Chaining here cannot express "heal the
            // ally": the equipment set never admits the heal, the magic context is
            // never built, and seats 0x06/0x0A/0x07/0x0D are NEVER CALLED. This is
            // THE seat that unblocks the whole facet, and it is answered from the
            // claim.
            //
            // WHY THAT IS STILL NOT "MANUFACTURING A DECISION" (#0). APMF does not
            // perform the equip, choose the hand, time it, or animate it: this only
            // says "this heal is ELIGIBLE for your equipment set." The AI's own
            // selection loop still scores it, still applies its slot-mask/range/
            // blackboard/resource gates, still runs its own `Equip magic` leaf with
            // its own animation, and is free not to pick it. And it is the engine's
            // OWN eligibility signal, one level up -- exactly what the engine emits
            // for a vanilla self-heal.
            //
            // SCOPE, three ways, all required simultaneously:
            //   * only the RESTORE item templates (recorded at install; every other
            //     magic/staff template still chains unconditionally, as before);
            //   * only the DRIVEN FORM of a live kIntent_Cast claim on THIS actor
            //     -- proxy-when-one-exists, else spell, so the original kSelf form is
            //     never forced (it would land on the caster, not the ally); and
            //   * only the claim's OWN HAND when this call resolves to one.
            // ================================================================
            // ch.8b SEAT 0 REBUILD TRIGGER (RE notebook J9): ONE lock-free RCU
            // read, resolved here (regardless of which of the 30 vtables `vt` is)
            // and reused by both the SEAT 0x0F block below and the completeness
            // fix. While a live cast claim stands on this actor, re-arm
            // `CombatInventory::dirty` every tick this hook runs for ANY of the
            // actor's items -- the proper, engine-native trigger for the NEXT
            // `CombatInventory::Rebuild` to re-run SEAT 0's classification
            // (core/CastClassify.cpp) for the claim's driven spell, rather than
            // relying solely on MFO's own combat-style-swap side effect (J7's
            // original, still-standing mechanism -- this is additive, never a
            // replacement). `a_cc` is the SAME engine-supplied CombatController*
            // this thunk already reads fid/subjectForm from; no NEW Actor->
            // CombatController resolution is introduced anywhere (there is no
            // CommonLib accessor for that on this pinned rev -- Docs/PROBE-
            // NONALIAS-PACKAGE.md already documents the neighbouring
            // `combatController` field as accessor-less). One bool write,
            // idempotent, no engine call. Scope note: this only fires while THIS
            // hook is already being called for the actor, i.e. the actor must
            // have at least one OTHER existing magic/staff CombatInventoryItem
            // (0x0F's own precondition) -- a follower with literally no other
            // known spell/staff still depends on MFO's pre-existing trigger for
            // the very FIRST rebuild after a claim engages; documented, not
            // silent (see the PR notes).
            apmf::CastSeatClaim seat{};
            bool hasLiveSeat = false;
            if (fid != 0) {
                hasLiveSeat = apmf::ControlMap::Get().TryGetCastSeatClaim(fid, seat) && seat.targetHandle;
                if (hasLiveSeat && a_cc && a_cc->inventory) a_cc->inventory->dirty = true;
            }

            if (fid != 0 && subjectForm != 0 && g_restoreVtables.contains(vt)) {
                RE::FormID driven = 0;
                bool       handOk = false;
                if (hasLiveSeat) {
                    driven = seat.proxy ? seat.proxy : seat.spell;
                    handOk = (callerHand == allowance::Hand::kUnknown) ||
                             (callerHand == ((seat.flags & APMF_API::kCastFlag_LeftHand)
                                                 ? allowance::Hand::kLeft : allowance::Hand::kRight));
                }

                if (handOk && driven != 0 && subjectForm == driven) {
                    if (LogDue(fid, subjectForm)) {
                        // feat/0x0f-callsite-log: slot 0x0B GetCategory via the vtable
                        // directly (no new hook) -- confirms category 1 == Restore.
                        using GetCategory_t = std::uint32_t (*)(RE::MagicItem*);
                        std::int64_t category = -1;
                        if (auto* mi = a_this->item ? skyrim_cast<RE::MagicItem*>(a_this->item) : nullptr) {
                            const auto miVt = *reinterpret_cast<std::uintptr_t*>(mi);
                            const auto fn   = *reinterpret_cast<GetCategory_t*>(miVt + 0x0B * sizeof(void*));
                            category        = fn(mi);
                        }
                        spdlog::info("[t2a seat 0x0F] 0x{} CheckShouldEquip item=0x{} -> YES (non-chaining; "
                                     "claim spell 0x{}{} target 0x{}; callsite RVA=0x{} [{}]; category={}) -- "
                                     "ELIGIBLE for the equipment set; the AI's own scoring/slot/range/resource "
                                     "gates still decide whether it is actually equipped.",
                                     apmf::log::Hex(fid), apmf::log::Hex(subjectForm), apmf::log::Hex(seat.spell),
                                     seat.proxy ? " via delivery-flip proxy" : "", apmf::log::Hex(seat.target),
                                     apmf::log::Hex(callSiteRva), CallSiteName(callSiteRva), category);
                    }
                    return true;   // THE non-chaining answer
                }
                if (handOk && seat.proxy != 0 && subjectForm == seat.spell) {
                    // N4 exactness: while a delivery-flip proxy is being driven, the
                    // ORIGINAL kSelf spell's own Restore item must NOT be allowed to
                    // take the hand instead -- the AI would cast it and silently heal
                    // the CASTER. `AllowedCastForHand` permits spell||proxy, so it
                    // would let this through; deny it explicitly here.
                    if (LogDue(fid, subjectForm))
                        spdlog::info("[t2a seat 0x0F] 0x{} CheckShouldEquip item=0x{} -> NO (N4: the original "
                                     "kSelf spell while its delivery-flip proxy 0x{} is being driven -- would "
                                     "silently heal the caster instead of the claimed ally).",
                                     apmf::log::Hex(fid), apmf::log::Hex(subjectForm), apmf::log::Hex(seat.proxy));
                    return false;
                }

                // ch.8b SEAT 0x0F COMPLETENESS FIX (RE notebook J9, Docs/INVARIANTS.md
                // #18): by this point subjectForm is definitely NOT the live claim's
                // driven form (the match above would have returned already) -- so ANY
                // Restore item whose spell delivery is NOT kSelf must be denied
                // outright, whether or not a claim currently stands. Vanilla data
                // never produces one at all (J3: the classifier's key requires
                // delivery==kSelf for every Restore-typed effect), so this changes
                // NOTHING today; it closes the one path a heal-OTHER item could
                // otherwise reach the hands after its claim releases, on the wrong
                // hand, or from a future/foreign source -- 0x81f7c0 aims a non-self
                // Restore item at `ctrl.TARGET`, i.e. the FOE.
                // `item` is a TESForm*; GetDelivery() lives on MagicItem (the same type
                // CastClassify.cpp reads at +0x10). RTTI-checked cast: a non-MagicItem
                // item (or a null one) simply falls through to the normal path below.
                auto* mi = a_this->item ? skyrim_cast<RE::MagicItem*>(a_this->item) : nullptr;
                if (mi && mi->GetDelivery() != RE::MagicSystem::Delivery::kSelf) {
                    if (LogDue(fid, subjectForm))
                        spdlog::info("[t2a seat 0x0F] 0x{} CheckShouldEquip item=0x{} -> NO (completeness fix: "
                                     "non-self-delivery Restore item, not the live claim's driven form).",
                                     apmf::log::Hex(fid), apmf::log::Hex(subjectForm));
                    return false;
                }
            }

            // ================================================================
            // ch.8b SEAT 0x0F DENY-COMPLETENESS (feat/equip-deny-complete,
            // 2026-09-05 -- CLAUDE.md principle 2, "for every facet a client can
            // claim there must be a COMPLETE deny"). Disassembly (0x813af2)
            // showed 0x0F is only a hard ADMISSION filter: passing it just means
            // an item was not dropped, it still has to WIN the score compare at
            // 0x815050 against every other item that also survived 0x0F, and
            // only the winner occupies the equip slot the behavior thread reads
            // to mint the next CombatBehaviorContextMagic (and, for Restore,
            // the caster the four other seats need). The block above only
            // governs the two Restore vtables; this one closes the gap for the
            // remaining 28 (and the residual Restore case the block above
            // doesn't already return on: a self-delivery Restore item that is
            // neither the driven form nor the N4 original-spell case).
            //
            // Scope, all three required and enforced by the guards already in
            // effect at this point in the function: ONLY this actor (fid != 0,
            // read off THIS call's own CombatController -- never a client actor
            // list, #4); ONLY while `hasLiveSeat` is true, i.e. exactly for the
            // duration ControlMap reports a live kIntent_Cast claim on this fid
            // (TryGetCastSeatClaim above, same read the YES branch uses -- no
            // latched/cached state here, so this lifts the instant the claim
            // releases or its TTL expires, and never fires for an unclaimed
            // actor); ONLY on the 30 hooked spell/staff vtables (this thunk is
            // never reached for anything else -- line ~191 already returned
            // false for a foreign vtable).
            //
            // Deliberately actor-wide, NOT hand-scoped like AllowedCastForHand
            // below: it has not been established that the 0x815050 score
            // compare partitions by hand, so completeness denies a competing
            // item on EITHER hand rather than risk a same-vtable, other-hand
            // survivor still winning the compare.
            //
            // TWO KNOWN HOLES -- documented, not fixed here (see the brief):
            //   (1) weapon/fist item classes have no concrete header class to
            //       hook (same finding as the file banner above) and can still
            //       out-score us;
            //   (2) Actor::StartCombat's direct ActorEquipManager equip path
            //       (0x6b6bb5..0x6b6c02) bypasses this selector entirely.
            // ================================================================
            if (g_denyCompleteEnabled.load(std::memory_order_relaxed) &&
                hasLiveSeat && fid != 0 && subjectForm != 0) {
                const RE::FormID driven = seat.proxy ? seat.proxy : seat.spell;
                if (driven == 0 || subjectForm != driven) {
                    if (LogDue(fid, subjectForm))
                        spdlog::info("[t2a seat 0x0F] 0x{} CheckShouldEquip item=0x{} -> NO (deny-complete: a "
                                     "cast claim spell 0x{}{} stands on this actor -- every other spell/staff "
                                     "item is denied so the claimed form is the sole survivor of the equip-slot "
                                     "score compare).",
                                     apmf::log::Hex(fid), apmf::log::Hex(subjectForm), apmf::log::Hex(seat.spell),
                                     seat.proxy ? " via delivery-flip proxy" : "");
                    return false;
                }
            }

            // ---- From here down: UNCHANGED, engine-answer-first DENY (#17). The
            // engine gets the first word and APMF only ever flips its YES to NO. ----
            const bool engineSays = original(a_this, a_cc);
            if (g_logEnabled.load(std::memory_order_relaxed) && fid != 0 && LogDue(fid, subjectForm)) {
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[t2a] 0x{} CheckShouldEquip item=0x{} class={} hand={} engineSays={}",
                             apmf::log::Hex(fid), apmf::log::Hex(subjectForm), cls ? cls : "<unresolved>",
                             callerHand == allowance::Hand::kLeft    ? "L" :
                             callerHand == allowance::Hand::kRight   ? "R" : "?",
                             engineSays ? "YES" : "NO");
            }
            if (!engineSays) return false;   // the AI already declined -- nothing to own
            if (!a_cc || fid == 0) return engineSays;

            // Off-hand loan exemption (mirrors MFO CombatStyle.cpp's WantedSpell
            // rule): if this item IS the actor's own actively-claimed ch.8 spell,
            // it is equippable no matter what a ch.15 weapon-order claim says --
            // resolved directly off the SAME winning claim the ch.8 exclusivity
            // check below reads, so both branches necessarily agree with it.
            APMF_API::APMF_Param castClaim{};
            const bool isClaimedSpell =
                subjectForm != 0 &&
                apmf::ControlMap::Get().TryGetOwningClaim(fid, APMF_API::kIntent_SelectSpell, castClaim) &&
                castClaim.form == subjectForm;
            if (isClaimedSpell) return engineSays;

            // Same exemption, one level deeper -- the claim's DELIVERY-FLIP PROXY
            // (H1, the 2026-09-05 drive-chain review; the rule outlived the drive
            // that exposed it). The exemption directly above only recognises the
            // ch.8 claim's literal `param.form`; when a ch.8b claim carries a
            // delivery-flip PROXY, that proxy is a form the ch.8 claim never names,
            // so the ch.8 narrow below would deny the very item the AI is being asked
            // to equip for the claimed cast. `CastClaimNamesForHand` is the
            // strict positive test (a kIntent_Cast claim standing on THIS hand
            // naming THIS spell-or-proxy), so this widens nothing else: it admits
            // exactly the spell/proxy pair the ch.8b claim already admits, and
            // still lets the ENGINE have the final word. Applied here for the same
            // reason as in CastGate -- a claim keyed on a FormID must admit the
            // substitute form at EVERY gate that keys on that FormID.
            if (allowance::CastClaimNamesForHand(fid, subjectForm, callerHand))
                return engineSays;

            // ch.8 -- cast-select exclusivity (unchanged: deny any OTHER spell
            // while a SelectSpell claim names one).
            if (!allowance::Allowed(fid, APMF_API::kIntent_SelectSpell, subjectForm))
                return false;

            // ch.15 -- weapon-order claim: deny any spell/staff re-arm while a
            // kIntent_Equipment claim holds this actor's hands (its param.form,
            // a weapon FormID, never matches a spell/staff subjectForm, so a
            // held claim denies every item this hook sees -- the exemption
            // above already let the claimed cast spell through).
            if (!allowance::Allowed(fid, APMF_API::kIntent_Equipment, subjectForm))
                return false;

            // ch.8b -- cast-execution claim: while a kIntent_Cast claim stands the AI
            // may not re-arm THE CLAIMED HAND with any spell/staff OTHER than the
            // claimed cast spell/proxy (the freeze-free equivalent of "the package
            // holds the spell in hand"). AllowedCastForHand permits exactly claim
            // spell + proxy on the claim's own hand, denies every other subjectForm
            // on that hand, and -- per-hand deny, INVARIANTS #18 -- leaves the OTHER
            // hand's re-arm decision untouched (callerHand resolved above); an actor
            // with no cast claim, or a call this hook cannot resolve to a hand, is
            // unaffected / degrades to the actor-wide floor.
            if (!allowance::AllowedCastForHand(fid, subjectForm, callerHand))
                return false;

            return engineSays;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t2a] VR runtime -- CombatInventoryItem vtable indices are SE/AE-only "
                         "verified; CheckShouldEquip allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        g_logEnabled.store(GetPrivateProfileIntA("EquipGate", "EnableEquipGateLog", 0,
                                                  "Data/SKSE/Plugins/APMF.ini") != 0,
                           std::memory_order_relaxed);
        g_denyCompleteEnabled.store(GetPrivateProfileIntA("EquipGate", "EnableEquipDenyComplete", 1,
                                                           "Data/SKSE/Plugins/APMF.ini") != 0,
                                    std::memory_order_relaxed);

        // Per-hand deny (INVARIANTS #18): resolve the vanilla Left/Right Hand
        // BGSEquipSlot forms ONCE, through CommonLib's own version-robust
        // BGSDefaultObjectManager singleton (the same table the engine consults) --
        // never a hardcoded FormID. A null result (a runtime that somehow has no
        // default-object table populated yet) just means every item resolves to
        // Hand::kUnknown below and this gate degrades to its pre-existing
        // actor-wide floor -- never a crash, never a guess.
        if (auto* dobj = RE::BGSDefaultObjectManager::GetSingleton()) {
            g_leftHandSlot.store(dobj->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip),
                                 std::memory_order_release);
            g_rightHandSlot.store(dobj->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip),
                                  std::memory_order_release);
        }

        // Expected RTTI base: CombatInventoryItem (CheckShouldEquip is declared
        // there; every concrete spell/staff instantiation derives it).
        REL::Relocation<void*> expectedTD{ RE::RTTI_CombatInventoryItem };

        const REL::VariantID kVtables[] = {
            // spells in hand
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterArmor_[0],
            // staves in hand (a staff strips the forced weapon exactly like a
            // spell does, and EquipWeapon counts a staff as NEITHER category)
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterOffensive_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterRestore_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterWard_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterSummon_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterStagger_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterDisarm_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterCloak_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterLight_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterTargetEffect_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterParalyze_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterScript_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterReanimate_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterArmor_[0],
        };
        constexpr std::size_t kCheckShouldEquip = 0x0F;

        const int n = allowance::InstallOnVtables(kVtables, kCheckShouldEquip, &EquipGateThunk,
                                                   expectedTD.get(), "t2a", g_orig);

        // ch.8b SEAT 0x0F scope (feat/ai-cast-seats-impl): record WHICH of the vtables
        // just patched are the RESTORE templates, from the two NAMED symbols -- never
        // inferred from a class name at runtime. Only these two may ever take the
        // seat's non-chaining branch; the other 28 keep chaining unconditionally.
        // A symbol InstallOnVtables SKIPPED (failed RTTI derivation) is simply absent
        // from g_orig, so the thunk never runs for it and this set is harmless.
        for (const auto& id : { RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0],
                                RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterRestore_[0] }) {
            REL::Relocation<std::uintptr_t> vt{ id };
            if (g_orig.contains(vt.address())) g_restoreVtables.insert(vt.address());
        }
        spdlog::info("[t2a] CheckShouldEquip allowance hooked on {} spell/staff inventory-item "
                     "vtable(s) -- ch.8 casting-select and ch.15 equipment (weapon-order) "
                     "claims now enforced here too; ch.8b is per-hand-scoped via "
                     "itemSlot.equipSlot (left-hand slot {}, right-hand slot {}). ch.8b SEAT 0x0F "
                     "armed on the Restore templates only; the SEAT 0 rebuild trigger and the 0x0F "
                     "completeness fix (non-self Restore item denied unless it is the live claim's "
                     "driven form) run on every patched vtable. Deny-complete (every OTHER spell/staff "
                     "item denied, actor-wide, while a cast claim stands): {} ([EquipGate] "
                     "EnableEquipDenyComplete). Per-call trace log: {} ([EquipGate] "
                     "EnableEquipGateLog).",
                     n, static_cast<void*>(g_leftHandSlot.load(std::memory_order_relaxed)),
                     static_cast<void*>(g_rightHandSlot.load(std::memory_order_relaxed)),
                     g_denyCompleteEnabled.load(std::memory_order_relaxed) ? "ON (default)" : "OFF",
                     g_logEnabled.load(std::memory_order_relaxed) ? "ON" : "OFF (default)");
    }

}
