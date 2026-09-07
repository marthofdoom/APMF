#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/AiCastSeats.h"

#include <array>
#include <cmath>
#include <string_view>

// Win32 INI read for the two group flags below. Declared by hand, exactly
// like Hook.cpp's GetCurrentThreadId() -- PCH does not pull in <Windows.h>,
// and this is the one Win32 call this file needs, not a reason to add it.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See AiCastSeats.h for the design/why. Every thunk below follows the exact
// same shape: recover the original for THIS vtable, call it FIRST, log a
// throttled observe line off the result, then return EXACTLY what the engine
// returned. No argument is ever read before the original runs; no return
// value is ever altered afterward.
//
// FALLBACK SAFETY (marth 2026-09-05 review): a lookup miss on the recorded-
// original map is structurally unreachable via normal engine dispatch -- this
// thunk is entered ONLY through a vtable slot this file itself overwrote with
// this exact function pointer (see Install()'s loop), so `*reinterpret_cast<
// uintptr_t*>(a_this)` can only ever equal one of the addresses that same
// loop inserted into the map. But "cannot happen" is not a safety argument
// for a hook installed on 30+ live vtables -- so a miss, if it ever somehow
// occurred (memory corruption, a future engine subclass, an install-time
// bookkeeping bug), no longer fabricates a plausible-looking return value.
// It recovers a REAL, live function pointer instead (RecoverLiveOriginal
// below): if a vtable was truly never patched by this file, its slot still
// holds a genuine callable (the engine's own function, or another mod's
// already-chained hook) -- reading it is never a guess. Install() also
// verifies (and loudly logs if not) that every vtable in each group's fixed
// list actually got an original recorded, closing the loop the other
// direction: the fixed set this file iterates is exactly the set whose
// originals are guaranteed present.
//
// STANDING RULE (marth 2026-09-05, root-caused from a live deck CTD): A
// CommonLib vfunc declaration is NOT ABI-trustworthy on its own -- verify
// every hooked vfunc against the disassembled callee before writing a thunk
// for it, especially anything CommonLib types `void*`/`unk`, and ESPECIALLY
// anything that might be a hidden-return (sret) out-slot. CommonLib declared
// `CombatMagicCaster::GetMagicTarget` as `void* GetMagicTarget(CombatController*)
// const` (2 args, pointer return) -- WRONG. The real engine ABI (base impl
// shared by all 14 caster vtables) is `Out16* GetMagicTarget(CombatMagicCaster*,
// Out16* out, CombatController*)`: the Microsoft x64 ABI inserts a HIDDEN
// out-pointer for any aggregate return >8 bytes that doesn't fit a register,
// which CommonLib's header never modeled. The original thunk, written to the
// wrong 2-arg shape, passed the map lookup's internal `unordered_map` node
// pointer as if it were the CombatController* argument (every real argument
// shifted one register) -- `orig` then read attacker/target handles out of
// that foreign node's memory, handed back a garbage "target" (in the observed
// crash, literally the engine's own GetMagicTarget function address, misread
// as an Actor*), and a caller several frames later dereferenced it as an
// object and jumped through a vtable slot built from raw instruction bytes.
// Fixed below (see Out16 + the 3-arg GetMagicTarget_t). CalculateScore (0x0C)
// and CheckStartCast/CheckStopCast (0x06/0x07) were re-checked against this
// same risk: all three return a SCALAR (float / bool) that always fits in a
// single register or XMM slot, so the x64 ABI can never insert a hidden
// out-pointer for them regardless of what CommonLib's header says -- the bug
// CLASS that hit GetMagicTarget is categorically impossible for a scalar
// return. CheckStartCast's exact 2-arg/bool-return shape is additionally
// field-proven: MFO's shipped, months-live native/CasterConsent.cpp:439 hooks
// the identical signature on the identical vtable list with no ABI-mismatch
// symptom ever observed. CheckStopCast shares that same vtable list, the
// adjacent slot, and the same single-CombatController*-argument shape with no
// counter-evidence. (This environment has no disassembler/game-binary access
// to hand-verify CalculateScore's argument count byte-for-byte the way
// GetMagicTarget was; the ABI-category argument above is sound regardless,
// but a byte-level disassembly pass remains the gold standard before treating
// any NEW vfunc here as trustworthy -- do it on the deck/IDA side if in doubt.)
// ============================================================================

namespace apmf::aicastseats {

    namespace {

        constexpr std::uint64_t kThrottleMs = 1500;   // per (actor, subject) cadence, all four seats

        // See the file banner. Reads the CURRENT function pointer stored at
        // `a_slot` on the live vtable at `a_vtable` -- i.e. exactly what a real
        // virtual call through that vtable would invoke right now. Never used on
        // a vtable this file itself patched (that slot would just read back this
        // very thunk); only reached on the structurally-unreachable miss path,
        // where by definition this file never wrote to that vtable's slot.
        template <class Fn>
        Fn RecoverLiveOriginal(std::uintptr_t a_vtable, std::size_t a_slot) {
            return *reinterpret_cast<Fn*>(a_vtable + a_slot * sizeof(void*));
        }

        // ---- config: two independently-selectable probe groups, both OFF by
        // default. No established INI/Config infra exists yet in APMF (grepped
        // clean), so this reads directly from the SAME "Data/SKSE/Plugins/
        // APMF.ini" location core/Log.cpp's Setup() already uses for APMF.log --
        // read ONCE at Install(). Deliberately NOT a hotkey/toggle: the standing
        // rule (marth 2026-09-0x) is probes are fully passive, config-gated or
        // always-on rate-limited logging only, never a runtime input switch.
        // Missing file/section/key -> GetPrivateProfileIntA returns the default
        // (0/OFF) without erroring, so this is safe with no ini present at all.
        bool ReadIniFlag(const char* a_key, long a_default = 0) {
            return GetPrivateProfileIntA("AiCastSeats", a_key, a_default, "Data/SKSE/Plugins/APMF.ini") != 0;
        }

        // ---- layout guards (ENGINE_NOTES §0.29 -- the AE +8 CombatController
        // bug). Every member this file reads is BELOW the 0x68 divergence
        // point, so it is layout-identical on SE and AE. ----
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the "
                      "SE/AE layout split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point (0x68)");
        static_assert(offsetof(RE::CombatController, targetHandle) == 0x2C,
                      "CombatController::targetHandle moved -- re-verify against the pinned header");
        static_assert(offsetof(RE::CombatController, targetHandle) < 0x68,
                      "targetHandle is past the AE layout divergence point (0x68)");
        static_assert(offsetof(RE::CombatInventoryItem, item) == 0x10,
                      "CombatInventoryItem::item moved -- re-verify against the pinned header");
        static_assert(offsetof(RE::CombatMagicCaster, magicItem) == 0x18,
                      "CombatMagicCaster::magicItem moved -- re-verify against the pinned header");

        // ---- shared RTTI class-name resolver (mirrors core/NonAliasProbe.cpp's
        // ResolveTypeName exactly -- see that file's comment for why the raw
        // MSVC-decorated name, not a demangled one, is the right call here: no
        // demangler exists in this codebase or CommonLib, and hand-rolling one
        // would itself be the kind of fragile guess this probe exists to avoid).
        // Returns nullptr (never guesses) if any link in the RTTI chain is absent.
        const char* ResolveTypeName(std::uintptr_t vtableAddr) {
            if (!vtableAddr) return nullptr;
            auto* colPtr = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(vtableAddr - sizeof(void*));
            if (!colPtr) return nullptr;
            auto* td = colPtr->typeDescriptor.get();
            if (!td) return nullptr;
            return td->mangled_name();
        }

        // ---- one shared throttle table + mutex for all four seats. Combat-
        // thread traffic only, low contention, leaf lock (never held across a
        // call into the engine or another lock). Key = (actorFormID << 32 |
        // subjectFormID) so a burst that touches several DIFFERENT items/spells
        // for the same actor logs each of them once, while a repeat of the
        // SAME (actor, subject) pair within the window is suppressed -- this is
        // what lets deliverable 1's "real score distribution" come through
        // whole on the first rescore burst instead of being cut to one line.
        std::mutex                                    g_rlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastScoreMs;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastStartMs;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastStopMs;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastTargetMs;
        // TASK 3 (marth 2026-09-05): a SEPARATE throttle table for GetMagicTarget's
        // ENTRY log, deliberately not shared with g_lastTargetMs (the existing
        // exit/result log's table) -- an ENTER line must never be suppressed by the
        // exit log's own dedup window, so the next deck run can tell "never called"
        // (no ENTER line at all) apart from "called, then something died in/after
        // it" (an ENTER line with no matching exit line).
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastTargetEntryMs;

        std::uint64_t RlKey(RE::FormID a_actor, RE::FormID a_subject) {
            return (static_cast<std::uint64_t>(a_actor) << 32) | static_cast<std::uint64_t>(a_subject);
        }

        bool ThrottleOK(std::unordered_map<std::uint64_t, std::uint64_t>& a_table,
                        RE::FormID a_actor, RE::FormID a_subject) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = RlKey(a_actor, a_subject);
            std::scoped_lock lk(g_rlMx);
            auto& last = a_table[key];
            if (now - last < kThrottleMs) return false;
            last = now;
            return true;
        }

        // Vfunc slot indices -- file-scope so both the Install() loops and the
        // thunks' RecoverLiveOriginal fallback reference the exact same constant.
        constexpr std::size_t kCalculateScore   = 0x0C;
        constexpr std::size_t kCheckStartCast   = 0x06;
        constexpr std::size_t kCheckStopCast    = 0x07;
        constexpr std::size_t kGetMagicTarget   = 0x0A;
        constexpr std::size_t kCheckShouldEquip = 0x0F;   // TASK 2 -- same slot core/EquipGate.cpp patches

        // ======================================================================
        // SEAT 1 -- WHICH item. CombatInventoryItem::CalculateScore, vfunc 0x0C.
        // Only ever called during the AI's own periodic item-rescore pass (NOT
        // per frame), so no extra throttling is needed to keep this "periodic" --
        // the engine already paces it. The per-(actor,item) dedup above still
        // caps a pathological rescore loop to one line per item per window.
        // ======================================================================

        using CalculateScore_t = float (*)(RE::CombatInventoryItem*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_scoreOrig;

        float CalculateScoreThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            CalculateScore_t orig;
            if (const auto oit = g_scoreOrig.find(vt); oit != g_scoreOrig.end()) {
                orig = reinterpret_cast<CalculateScore_t>(oit->second);
            } else {
                // See the file banner -- never fabricate a score; recover a real,
                // live original instead of a lookup miss this file itself made
                // structurally unreachable in the first place.
                spdlog::error("[aicastseats] CalculateScore: vtable 0x{} not in the recorded set -- "
                              "recovering the LIVE original instead of fabricating a score.",
                              apmf::log::Hex(vt, 16));
                orig = RecoverLiveOriginal<CalculateScore_t>(vt, kCalculateScore);
            }

            const float score = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST -- this is the only call made

            if (!a_cc) return score;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return score;
            const auto fid = actor->GetFormID();

            auto*      item     = a_this->item;
            const auto itemForm = item ? item->GetFormID() : 0;

            if (ThrottleOK(g_lastScoreMs, fid, itemForm)) {
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[aicastseats] t={} 0x{} '{}' SCORE item=0x{} '{}' class={} score={:.3f}",
                             apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                             actor->GetName() ? actor->GetName() : "?", apmf::log::Hex(itemForm),
                             item && item->GetName() ? item->GetName() : "?", cls ? cls : "<unresolved>",
                             score);
            }
            return score;
        }

        // ======================================================================
        // GROUP C -- TASK 1 (PRIMARY, ships enabled) + TASK 2 (score bias, ships
        // DISABLED). CalculateScore (0x0C) on the WEAPON-CLASS CombatInventoryItem
        // leaves -- Melee, Ranged, Shield, Torch. Opus PASS S, 2026-09-06,
        // disasm-CONFIRMED (do NOT re-derive): these four have NO CommonLib
        // concrete C++ class and therefore NO `RE::VTABLE_*` symbol (the same
        // finding core/EquipGate.cpp's file banner and Docs/DENY-COMPLETENESS-
        // AUDIT.md row 15 already record for why 0x0F can't be hooked for
        // weapons) -- so this group resolves each vtable from a RAW RVA
        // (`REL::Offset`, no `REL::VariantID`; no Address-Library ID exists for
        // any of the four) and is AE-ONLY (1.6.1170; the RVAs are meaningless on
        // any other build).
        //
        // Melee+Ranged share arbitration category 0; Shield+Torch share category
        // 3 (the engine's fixed `[1,2,4,0,3,5,0,6]` order table + slot-bitmask
        // occupancy walks categories score-BLIND -- only WITHIN a category does
        // `itemScore` (`+0x18`) actually get compared, ascending, highest wins).
        // A weapon score can therefore only ever decide melee-vs-ranged or
        // shield-vs-torch; it can never outscore a spell that already claimed
        // the hand (weapons are walked after categories 1/2/4) -- TASK 2 below
        // does not attempt that.
        //
        // INSTALL-TIME VERIFICATION, belt-and-suspenders and STRONGER than an
        // RTTI-name guess: this codebase has never established the real mangled
        // class names for these four leaves (unlike CastClassify.cpp's
        // CombatMagicItemData, which at least has a confirmed name to string-
        // match). So the GATING check here is the one fact this session actually
        // disassembled -- the LIVE function pointer already sitting in vtable
        // slot 0x0C, BEFORE this file writes anything, must equal the
        // disasm-confirmed CalculateScore address for that exact class. A
        // mismatch means the vtable RVA resolved to something other than what
        // marth's table names for THIS build -- refuse that one class, log both
        // addresses, never a blind write. RTTI name is ALSO resolved and logged
        // per class at install, informational only (not gating), so a future
        // pass can confirm/replace this whole check with a real name match.
        //
        // TASK 2 (score bias, [AiCastSeats] EnableScoreSteer, default 0): while a
        // live ch.15 `kIntent_Equipment` claim on this actor names THIS item's
        // exact form (the SAME claim core/EquipGate.cpp's T2a gate already reads
        // via `Allowance::Allowed`), the engine's own returned score is biased
        // UPWARD by a fixed constant -- never fabricated from nothing, the
        // engine always answers first and the base is always its real number.
        // Independent of GROUP C's own probe flag being armed: TASK 2 can only
        // ever fire if TASK 1's hook is actually installed, and Install() logs
        // that dependency explicitly. Stays OFF until TASK 1's probe data
        // (deliverable of this same change) confirms 0x0C actually runs for a
        // follower and for which of these four classes.
        // ======================================================================

        using WeaponScore_t = float (*)(RE::CombatInventoryItem*, RE::CombatController*);

        struct WeaponClassSpec {
            const char*    tag;            // for logging only
            std::uintptr_t vtableRva;      // AE 1.6.1170 RVA, disasm-confirmed
            std::uintptr_t calcScoreRva;   // this class's OWN CalculateScore impl RVA -- the install-time gate
            int            category;       // engine arbitration category (informational)
        };
        constexpr WeaponClassSpec kWeaponClasses[] = {
            { "Melee",  0x18c9028, 0x8183e0, 0 },
            { "Ranged", 0x18c90d8, 0x8188b0, 0 },
            { "Shield", 0x18c9188, 0x818df0, 3 },
            { "Torch",  0x18c92e8, 0x819480, 3 },
        };

        struct WeaponClassInfo { const char* tag; int category; };
        std::unordered_map<std::uintptr_t, WeaponClassInfo> g_weaponClassInfo;
        std::unordered_map<std::uintptr_t, std::uintptr_t>  g_weaponScoreOrig;
        std::atomic<bool> g_scoreSteerEnabled{ false };

        // ======================================================================
        // TASK 1 per-hand resolution (marth 2026-09-06 PASS S brief) -- own copy
        // of the SAME per-hand read core/EquipGate.cpp's T2a gate already does
        // for `callerHand` (itemSlot.equipSlot vs. the vanilla Left/Right Hand
        // BGSEquipSlot default objects). Kept file-local rather than exported
        // from EquipGate.cpp/Allowance.h -- this is a read of a plain struct
        // member on `CombatInventoryItem` (the SAME base class WeaponScoreThunk
        // already operates on), resolved through the SAME engine singleton, so
        // duplicating the two-line lookup is cheaper and lower-risk than adding
        // a cross-TU dependency for it. `allowance::Hand` (Allowance.h) is
        // reused as the result type so both files speak the same vocabulary.
        // ======================================================================
        std::atomic<RE::BGSEquipSlot*> g_leftHandSlot{ nullptr };
        std::atomic<RE::BGSEquipSlot*> g_rightHandSlot{ nullptr };

        // Own copy of core/EquipGate.cpp's ResolveHandSlot -- see that file's copy
        // for the full root-cause writeup (pinned CommonLib rev c4ab853d, AE-only
        // IsObjectInitialized offset bug misreads a live TESForm*'s bytes as a
        // bool). Bypasses GetObject/IsObjectInitialized entirely: reads
        // objects[idx] directly (safe for a low index like kLeftHandEquip(19)/
        // kRightHandEquip(20), well before AE's extra appended entries) and
        // validates with As<T>(); a bad resolution REFUSES (nullptr + loud log)
        // rather than silently degrading to any-hand (principle 7).
        RE::BGSEquipSlot* ResolveHandSlot(RE::BGSDefaultObjectManager* dobj, RE::DEFAULT_OBJECT idx,
                                          const char* which) {
            const auto        i   = static_cast<std::size_t>(idx);
            RE::TESForm* const raw = dobj->objects[i];
            auto* const        slot = raw ? raw->As<RE::BGSEquipSlot>() : nullptr;
            if (!slot) {
                spdlog::error("[aicast] BGSDefaultObjectManager::objects[{}] did not resolve to a "
                              "BGSEquipSlot for the {} hand (raw = {}) -- per-hand resolution REFUSED "
                              "for that hand, not silently degraded to any-hand.",
                              i, which, static_cast<void*>(raw));
            }
            return slot;
        }

        // ======================================================================
        // TASK 2 (marth 2026-09-06 PASS S brief, [EquipGate] EnableDualWieldPreference,
        // default 0) -- shared state. See ShieldEquipGateThunk below (after
        // WeaponScoreThunk) for the full design and the guards.
        // ======================================================================
        std::atomic<bool> g_dualWieldPrefEnabled{ false };

        // Vanilla Skyrim.esm One-Handed dual-wield perks. VERIFIED 2026-09-06
        // directly against the shipped Skyrim.esm's own PERK records -- this
        // environment has no xEdit/CK, so the raw TES4 record structure was
        // parsed by hand (EDID subrecord text -> its owning PERK record header
        // -> that record's own FormID field), exactly the "verify against real
        // game data, never guess" precedent Loadout.cpp's DualCastPerkForSchool
        // set for 0x000153CD..0x000153D1. Findings, byte-for-byte off the ESM:
        //   EDID "DualFlurry30" (rank 1) -- FormID 0x00106256. Its own NNAM
        //     (next-rank) field reads 0x00106257 -- confirmed to chain to:
        //   EDID "DualFlurry50" (rank 2) -- FormID 0x00106257.
        //   EDID "DualSavagery"          -- FormID 0x00106258 (immediately
        //     adjacent in the authoring block; a separate, later perk in the
        //     same One-Handed dual-wield line, not a further Dual Flurry rank).
        // All three are checked independently below (own ANY -> invested) --
        // simpler and equally correct vs. walking BGSPerk::nextPerk, since all
        // three concrete forms are already resolved individually.
        constexpr RE::FormID kDualFlurryRank1FormID = 0x00106256;   // EDID DualFlurry30
        constexpr RE::FormID kDualFlurryRank2FormID = 0x00106257;   // EDID DualFlurry50
        constexpr RE::FormID kDualSavageryFormID    = 0x00106258;   // EDID DualSavagery
        std::atomic<RE::BGSPerk*> g_dualFlurryRank1{ nullptr };
        std::atomic<RE::BGSPerk*> g_dualFlurryRank2{ nullptr };
        std::atomic<RE::BGSPerk*> g_dualSavagery{ nullptr };

        // Genuinely-one-handed + perk-invested is necessary but NOT sufficient
        // -- denying the shield must never leave the off-hand empty (worse than
        // the shield). This table answers "does the ENGINE'S OWN Melee-category
        // scoring pass currently see a SECOND, DISTINCT one-handed weapon for
        // this actor" by piggybacking on WeaponScoreThunk (a hook already
        // field-proven to run), rather than walking inventory -- Actor::
        // GetInventory() allocates and is main-thread-idiomatic everywhere else
        // in this codebase, not something to call from a combat-thread vfunc
        // hook. Keyed on the item's own FormID (not the per-hand instance), so
        // a weapon scored via BOTH a main- and an off-hand CombatInventoryItem
        // (TASK 1's own open question) still counts ONCE -- this answers "is a
        // second DISTINCT weapon available" correctly either way that resolves.
        std::mutex g_oneHandedMx;
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, std::uint64_t>> g_oneHandedSeen;
        constexpr std::uint64_t kOneHandedSightingTtlMs = 6000;   // a few rescore cycles' worth; no LIVE state to expire early

        void RecordOneHandedSighting(RE::FormID a_actor, RE::FormID a_weaponForm) {
            const auto now = apmf::clock::MonotonicMs();
            std::scoped_lock lk(g_oneHandedMx);
            auto& perActor = g_oneHandedSeen[a_actor];
            perActor[a_weaponForm] = now;
            for (auto it = perActor.begin(); it != perActor.end(); )
                it = (now - it->second > kOneHandedSightingTtlMs) ? perActor.erase(it) : std::next(it);
        }

        bool HasSecondOneHandedWeapon(RE::FormID a_actor) {
            const auto now = apmf::clock::MonotonicMs();
            std::scoped_lock lk(g_oneHandedMx);
            const auto it = g_oneHandedSeen.find(a_actor);
            if (it == g_oneHandedSeen.end()) return false;
            int live = 0;
            for (const auto& [form, ms] : it->second)
                if (now - ms <= kOneHandedSightingTtlMs) ++live;
            return live >= 2;
        }

        bool IsOneHandedMeleeWeapon(RE::TESObjectWEAP* a_weap) {
            if (!a_weap) return false;
            switch (a_weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kOneHandDagger:
            case RE::WEAPON_TYPE::kOneHandAxe:
            case RE::WEAPON_TYPE::kOneHandMace:
                return true;
            default:
                return false;
            }
        }

        // HasPerk() ONLY (deliberately not also TESNPC::GetPerkIndex's base-
        // template check, unlike Loadout.cpp's CanDualCast/OwnsExactPerk
        // precedent) -- this runs on the COMBAT thread, where GetPerkIndex has
        // no established precedent in either codebase (both known uses are
        // MainThread::Post/AddTask-side). HasPerk alone is a plain read on the
        // Actor's own already-resolved runtime perk list, no lock, matching
        // every other combat-thread read in this file. The known gap (a perk
        // authored only on a shared base TESNPC template, never runtime-added)
        // fails CLOSED here -- toward keeping the shield, never toward denying
        // it -- so it can never produce an empty off-hand from a missed read.
        bool HasDualWieldInvestment(RE::Actor* a_actor) {
            if (!a_actor) return false;
            auto* r1 = g_dualFlurryRank1.load(std::memory_order_acquire);
            auto* r2 = g_dualFlurryRank2.load(std::memory_order_acquire);
            auto* sv = g_dualSavagery.load(std::memory_order_acquire);
            return (r1 && a_actor->HasPerk(r1)) || (r2 && a_actor->HasPerk(r2)) ||
                   (sv && a_actor->HasPerk(sv));
        }

        // RIGHT-SIZED from real field data (marth 2026-09-06 field session):
        // measured weapon-score magnitudes were Falmer War Axe 194, bow 81, and
        // magic scores ~0.08-29 -- so an additive bias only needs to clear ~200
        // to dominate any same-category rival actually observed. 1000.0f is an
        // additive offset over the engine's OWN answer (finalScore = engineScore
        // + kScoreSteerBias, unchanged shape), right-sized to that ~200 ceiling
        // with headroom, replacing the old 100000.0f placeholder that predated
        // any measurement. Still ships with EnableScoreSteer default OFF in code
        // (see Install()) -- this constant only matters once that INI flag is
        // deliberately turned on.
        constexpr float kScoreSteerBias = 1000.0f;

        // Dedup-on-transition, not a bare time throttle (marth 2026-09-06: "a
        // recent probe printed 37 identical lines of a stable condition through
        // a 1.5s throttle -- a throttle is not a dedup"). Logs on first sighting
        // of a (actor,item) key, on a real score/bias-state change beyond
        // epsilon, or on a long heartbeat floor so a genuinely stable value still
        // proves the probe alive at least once per window -- never silently.
        // `kMinChangeIntervalMs` only guards a pathological same-tick oscillation
        // from flooding; it never suppresses the FIRST report of a change.
        struct WeaponScoreLogState { std::uint64_t lastMs = 0; float lastScore = 0.0f; bool biased = false; };
        // Keyed on (actor,item) PLUS hand -- indexed [kUnknown=0, kLeft=1,
        // kRight=2] -- so the SAME item scored via two DIFFERENT hand
        // instances (TASK 1's own duplicate-cat-0 question) gets its own
        // first-sighting log for EACH hand, rather than the second hand's
        // sighting being suppressed as "no real change" against the first.
        std::unordered_map<std::uint64_t, std::array<WeaponScoreLogState, 3>> g_weaponScoreLogState;
        constexpr float          kScoreEpsilon         = 0.01f;
        constexpr std::uint64_t  kHeartbeatMs          = 15000;
        constexpr std::uint64_t  kMinChangeIntervalMs  = 250;

        bool WeaponScoreLogDue(RE::FormID a_actor, RE::FormID a_subject, allowance::Hand a_hand,
                               float a_score, bool a_biased) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = RlKey(a_actor, a_subject);
            std::scoped_lock lk(g_rlMx);   // shares the file's one throttle mutex -- leaf lock, no engine call under it
            auto& st = g_weaponScoreLogState[key][static_cast<std::size_t>(a_hand)];
            const bool first     = (st.lastMs == 0);
            const bool changed   = !first && (std::fabs(a_score - st.lastScore) > kScoreEpsilon || a_biased != st.biased);
            const bool heartbeat = !first && (now - st.lastMs) >= kHeartbeatMs;
            if (!first && !changed && !heartbeat) return false;                              // stable, floor not due -- SUPPRESS
            if (!first && changed && !heartbeat && (now - st.lastMs) < kMinChangeIntervalMs)
                return false;                                                                // oscillation guard only
            st = { now, a_score, a_biased };
            return true;
        }

        float WeaponScoreThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            const auto vt = *reinterpret_cast<std::uintptr_t*>(a_this);
            WeaponScore_t orig;
            if (const auto oit = g_weaponScoreOrig.find(vt); oit != g_weaponScoreOrig.end()) {
                orig = reinterpret_cast<WeaponScore_t>(oit->second);
            } else {
                // See the file banner -- never fabricate a score; recover a real,
                // live original instead of a lookup miss this file itself made
                // structurally unreachable in the first place.
                spdlog::error("[aicastseats] weapon CalculateScore: vtable 0x{} not in the recorded "
                              "set -- recovering the LIVE original instead of fabricating a score.",
                              apmf::log::Hex(vt, 16));
                orig = RecoverLiveOriginal<WeaponScore_t>(vt, kCalculateScore);
            }

            const float engineScore = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST -- the only call made before any decision

            if (!a_cc) return engineScore;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return engineScore;
            const auto fid = actor->GetFormID();

            auto*      item     = a_this->item;
            const auto itemForm = item ? item->GetFormID() : 0;

            // TASK 1 (marth 2026-09-06 PASS S brief) -- per-hand + slot-bitmask
            // report, the data that confirms or kills the duplicate-cat-0
            // hypothesis. TWO DISTINCT signals, both real, both read-only:
            //   * `hand` -- WHICH hand THIS SCORING CALL is for, resolved from
            //     `itemSlot.equipSlot` (a real CombatInventoryItem member, the
            //     SAME per-call field core/EquipGate.cpp's T2a gate already
            //     reads for `callerHand`) compared against the vanilla Left/
            //     Right Hand BGSEquipSlot default objects. This is the field
            //     that would differ across TWO calls scoring the SAME item
            //     FormID if the engine really does build one CombatInventoryItem
            //     wrapper per hand.
            //   * `slotMask` -- the CONFIRMED-GROUND-TRUTH raw bitmask at
            //     `item+0x28` (PASS S disasm: the occupancy-test value the
            //     engine's own order-table walk compares against a
            //     CombatEquipment set's own mask at +0x130/+0x160). This is a
            //     property of the ITEM FORM itself (constant across hands for
            //     the same weapon), so seeing it differ from a shield's own
            //     mask (or NOT differ) is the second half of the evidence.
            //   Which CombatEquipment SET (r15+0x118 vs +0x148) is being
            //   filled is NOT resolvable from this call site -- CalculateScore
            //   runs during the scoring pass, before the later AddItem/
            //   order-table walk that actually assigns a set; no pointer to
            //   that walk's own `r15` object reaches this vfunc's arguments.
            //   Logged as "set=?" rather than guessed.
            allowance::Hand hand = allowance::Hand::kUnknown;
            {
                auto*      slot = a_this->itemSlot.equipSlot;
                const auto lh   = g_leftHandSlot.load(std::memory_order_acquire);
                const auto rh   = g_rightHandSlot.load(std::memory_order_acquire);
                hand = (slot && slot == lh) ? allowance::Hand::kLeft  :
                       (slot && slot == rh) ? allowance::Hand::kRight :
                                               allowance::Hand::kUnknown;
            }
            std::uint32_t slotMask = 0;
            if (item) {
                slotMask = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(item) + 0x28);
            }

            const auto  cit = g_weaponClassInfo.find(vt);
            const char* tag = cit != g_weaponClassInfo.end() ? cit->second.tag : "?";
            const int   cat = cit != g_weaponClassInfo.end() ? cit->second.category : -1;

            // TASK 2 evidence table (only while the feature's own flag is on --
            // zero extra state/work otherwise): record a DISTINCT sighting of a
            // one-handed melee weapon FormID for this actor, from the SAME
            // Melee-category scoring pass ShieldEquipGateThunk below consults
            // via HasSecondOneHandedWeapon. See that table's own comment
            // (above HasDualWieldInvestment) for why this beats an inventory walk.
            if (g_dualWieldPrefEnabled.load(std::memory_order_relaxed) && cat == 0 && itemForm != 0) {
                if (auto* weap = item ? item->As<RE::TESObjectWEAP>() : nullptr;
                    IsOneHandedMeleeWeapon(weap)) {
                    RecordOneHandedSighting(fid, itemForm);
                }
            }

            // TASK 2 (score bias): bias ONLY when a live ch.15 kIntent_Equipment
            // claim on this actor names THIS EXACT item form -- the same claim/
            // read EquipGate.cpp's T2a gate already uses (TryGetOwningClaim,
            // lock-free RCU, any thread). Never applied with the flag off;
            // never applied to any other item.
            float finalScore = engineScore;
            bool  biased     = false;
            if (g_scoreSteerEnabled.load(std::memory_order_relaxed) && itemForm != 0) {
                APMF_API::APMF_Param claim{};
                if (apmf::ControlMap::Get().TryGetOwningClaim(fid, APMF_API::kIntent_Equipment, claim) &&
                    claim.form == itemForm) {
                    finalScore = engineScore + kScoreSteerBias;
                    biased     = true;
                }
            }

            if (WeaponScoreLogDue(fid, itemForm, hand, finalScore, biased)) {
                const char* cls  = ResolveTypeName(vt);
                const char* hs   = hand == allowance::Hand::kLeft  ? "L" :
                                   hand == allowance::Hand::kRight ? "R" : "?";
                if (biased) {
                    spdlog::info("[aicastseats] t={} 0x{} '{}' WEAPON-SCORE class={} rtti={} cat={} hand={} "
                                 "slotMask=0x{} item=0x{} '{}' engineScore={:.3f} STEERED->{:.3f} (ch.15 claim)",
                                 apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                                 actor->GetName() ? actor->GetName() : "?", tag, cls ? cls : "<unresolved>",
                                 cat, hs, apmf::log::Hex(slotMask), apmf::log::Hex(itemForm),
                                 item && item->GetName() ? item->GetName() : "?", engineScore, finalScore);
                } else {
                    spdlog::info("[aicastseats] t={} 0x{} '{}' WEAPON-SCORE class={} rtti={} cat={} hand={} "
                                 "slotMask=0x{} item=0x{} '{}' engineScore={:.3f}",
                                 apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                                 actor->GetName() ? actor->GetName() : "?", tag, cls ? cls : "<unresolved>",
                                 cat, hs, apmf::log::Hex(slotMask), apmf::log::Hex(itemForm),
                                 item && item->GetName() ? item->GetName() : "?", engineScore);
                }
            }
            return finalScore;
        }

        // ======================================================================
        // TASK 2 -- [EquipGate] EnableDualWieldPreference, default 0 (OFF).
        // CheckShouldEquip (0x0F) on the SAME Shield raw-RVA vtable GROUP C's
        // own CalculateScore (0x0C) check just identity-verified above --
        // deliberately placed HERE (core/AiCastSeats.cpp), not core/EquipGate.cpp,
        // per the brief: EquipGate.cpp's 0x0F hook only reaches the 30 concrete
        // Magic/Staff/Armor CombatInventoryItem instantiations it can name via a
        // real `RE::VTABLE_*` symbol (its own file banner) -- Shield has none,
        // exactly like Melee/Ranged/Torch, so the raw-RVA vtable GROUP C already
        // resolved is the ONLY handle this codebase has on Shield's slot 0x0F.
        //
        // WHY THIS INSTALL IS SAFE WITHOUT A SECOND DISASM-CONFIRMED ADDRESS.
        // GROUP C's 0x0C install-time check (above, in Install()) already reads
        // the LIVE pointer at slot 0x0C on THIS EXACT vtable object and requires
        // it to equal the disasm-confirmed CalculateScore address for Shield --
        // that is what proves `vt.address()` for the Shield entry in
        // kWeaponClasses genuinely IS Shield's own vtable (not a stale RVA or a
        // symbol drift). CombatInventoryItem is an ABSTRACT base whose vtable
        // SHAPE (which slot is which virtual function) is fixed by the C++ ABI
        // once the class hierarchy is fixed -- it is the same reasoning
        // core/EquipGate.cpp already relies on to patch slot 0x0F identically
        // across 30 UNRELATED concrete subclasses using one fixed slot index,
        // with no per-class address to check against either. So once 0x0C is
        // proven to be Shield's real slot (the check above), slot 0x0F on that
        // SAME already-identity-verified vtable is CheckShouldEquip by the same
        // structural guarantee -- this is a WEAKER install-time guard than 0x0C's
        // own (no independent known-good address exists for 0x0F on Shield to
        // cross-check, unlike 0x0C), and that gap is called out here rather than
        // papered over. The live pointer at 0x0F is still only ever CAPTURED
        // (never assumed, never fabricated) and always chained through for
        // every call this hook does not explicitly deny.
        //
        // WHAT IT DOES: engine-answer-first, exactly the allowance-template
        // contract (Docs/ALLOWANCE-TEMPLATE.md §3) core/EquipGate.cpp's own T2a
        // gate follows -- calls `orig` unconditionally, and only ever turns a
        // YES into a NO. Denies THIS shield's admission only when ALL hold:
        //   1. the actor is resolvable and this candidate is genuinely a shield
        //      item (non-zero form);
        //   2. no live ch.15 kIntent_Equipment claim already names THIS EXACT
        //      shield form -- composition, not a fight with MFO's own equip
        //      gambits or a deliberate client choice (CLAUDE.md principle 3);
        //   3. the actor is GENUINELY one-handed right now: GetEquippedObject
        //      (false) (right hand -- a plain already-resolved Actor-local
        //      read, no forms-map lock, no inventory walk) is a one-handed
        //      melee TESObjectWEAP;
        //   4. the actor has invested in the vanilla dual-wield perk line
        //      (HasDualWieldInvestment -- verified real FormIDs, see the
        //      comment above g_dualFlurryRank1);
        //   5. a SECOND, DISTINCT one-handed melee weapon is CONFIRMED
        //      available (HasSecondOneHandedWeapon -- the engine's own
        //      Melee-category scoring pass, not a guess) -- an unconfirmed
        //      off-hand weapon means NO deny, ever: an empty off-hand with no
        //      shield is strictly worse than the shield, and this table only
        //      ever grows evidence, never assumes it.
        // Every other call is untouched -- returns the engine's own answer
        // unmodified, including every call while the flag is off (checked
        // AFTER `orig` runs, never gating the engine call itself).
        // ======================================================================

        using ShieldEquip_t = bool (*)(RE::CombatInventoryItem*, RE::CombatController*);
        std::atomic<std::uintptr_t> g_shieldVtableAddr{ 0 };
        std::uintptr_t              g_shieldEquipOrig = 0;   // set ONCE at install; single-vtable hook, no map needed
        std::mutex                                       g_shieldDenyRlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastShieldDenyMs;

        bool ShieldEquipGateThunk(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc) {
            const auto vt         = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto expectedVt = g_shieldVtableAddr.load(std::memory_order_acquire);
            ShieldEquip_t orig;
            if (vt == expectedVt && g_shieldEquipOrig != 0) {
                orig = reinterpret_cast<ShieldEquip_t>(g_shieldEquipOrig);
            } else {
                // Structurally unreachable -- this thunk is only ever installed
                // on the one Shield vtable slot captured at install. Never
                // fabricate a should-equip result; recover the live original
                // for whatever vtable actually called us.
                spdlog::error("[aicastseats] ShieldEquipGate: vtable 0x{} != installed Shield vtable 0x{} -- "
                              "recovering the LIVE original instead of fabricating a result.",
                              apmf::log::Hex(vt, 16), apmf::log::Hex(expectedVt, 16));
                orig = RecoverLiveOriginal<ShieldEquip_t>(vt, kCheckShouldEquip);
            }

            const bool result = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST -- always called, flag or no flag
            if (!result) return result;                // engine already said no -- nothing to narrow
            if (!g_dualWieldPrefEnabled.load(std::memory_order_relaxed)) return result;
            if (!a_cc) return result;

            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return result;
            const auto fid = actor->GetFormID();

            auto*      shieldItem = a_this->item;
            const auto shieldForm = shieldItem ? shieldItem->GetFormID() : 0;
            if (shieldForm == 0) return result;

            // Guard 2: never override an explicit ch.15 equipment claim naming
            // THIS exact shield -- the same claim/read WeaponScoreThunk's own
            // TASK 2 branch and EquipGate.cpp's T2a gate both already use.
            APMF_API::APMF_Param claim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(fid, APMF_API::kIntent_Equipment, claim) &&
                claim.form == shieldForm) {
                return result;
            }

            // Guard 3: genuinely one-handed right now.
            auto* rightObj  = actor->GetEquippedObject(false);
            auto* rightWeap = rightObj ? rightObj->As<RE::TESObjectWEAP>() : nullptr;
            if (!IsOneHandedMeleeWeapon(rightWeap)) return result;

            // Guards 4 + 5: perk investment, then confirmed off-hand availability
            // -- deliberately checked LAST (cheapest-first is backwards here;
            // correctness-critical guard 5 must be the final word before a deny).
            if (!HasDualWieldInvestment(actor)) return result;
            if (!HasSecondOneHandedWeapon(fid)) return result;

            {
                const auto now = apmf::clock::MonotonicMs();
                const auto key = RlKey(fid, shieldForm);
                std::scoped_lock lk(g_shieldDenyRlMx);
                auto& last = g_lastShieldDenyMs[key];
                if (now - last >= kThrottleMs) {
                    last = now;
                    spdlog::info("[aicastseats] t={} 0x{} '{}' TASK2 DENY shield=0x{} '{}' -- dual-wield "
                                 "perk investment confirmed + a second one-handed weapon is available; "
                                 "freeing the off-hand slot for the AI's own weapon pass.",
                                 now, apmf::log::Hex(fid), actor->GetName() ? actor->GetName() : "?",
                                 apmf::log::Hex(shieldForm),
                                 shieldItem && shieldItem->GetName() ? shieldItem->GetName() : "?");
                }
            }
            return false;   // THE one narrowing answer -- everything else above returns `result` unmodified
        }

        // ======================================================================
        // SEAT 2 -- WHETHER to cast. CombatMagicCaster::CheckStartCast, vfunc 0x06.
        //
        // THIS PROBE IS THE INNER HOOK on this slot when MFO is also present
        // (marth 2026-09-05): the deck log shows APMF installing at 14:19:50,
        // MFO's own CheckStartCast hook (native/CasterConsent.cpp, ADVISORY
        // deny) installing LATER at 14:19:58. write_vfunc chains newest-first,
        // so MFO -- installed second -- sits OUTER (the engine calls MFO's
        // thunk, which calls this probe's thunk as ITS "orig", which calls the
        // real engine implementation as ITS OWN "orig"). This thunk therefore
        // logs the RAW, un-vetoed engine answer, BEFORE MFO's advisory logic
        // gets a chance to flip a YES to NO for its own reasons. Read a
        // CheckStartCast[..] -> YES line here as "the AI's OWN combat brain
        // wanted this," not "the AI actually cast it" -- MFO may still have
        // suppressed it one layer further out. Do not misread the two as the
        // same thing when correlating this log against MFO's.
        // ======================================================================

        using CheckStartCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_startOrig;

        bool CheckStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            CheckStartCast_t orig;
            if (const auto oit = g_startOrig.find(vt); oit != g_startOrig.end()) {
                orig = reinterpret_cast<CheckStartCast_t>(oit->second);
            } else {
                // See the file banner -- never fabricate a start/no-start decision.
                spdlog::error("[aicastseats] CheckStartCast: vtable 0x{} not in the recorded set -- "
                              "recovering the LIVE original instead of fabricating a result.",
                              apmf::log::Hex(vt, 16));
                orig = RecoverLiveOriginal<CheckStartCast_t>(vt, kCheckStartCast);
            }

            const bool result = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST

            if (!a_cc) return result;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return result;
            const auto fid = actor->GetFormID();

            auto*      spell     = a_this->magicItem;
            const auto spellForm = spell ? spell->GetFormID() : 0;

            if (ThrottleOK(g_lastStartMs, fid, spellForm)) {
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[aicastseats] t={} 0x{} '{}' CheckStartCast[{}] spell=0x{} '{}' -> {}",
                             apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                             actor->GetName() ? actor->GetName() : "?", cls ? cls : "<unresolved>",
                             apmf::log::Hex(spellForm), spell && spell->GetName() ? spell->GetName() : "?",
                             result ? "YES" : "NO");
            }
            return result;
        }

        // ======================================================================
        // SEAT 3 -- WHERE it aims. CombatMagicCaster::GetMagicTarget, vfunc 0x0A.
        //
        // CORRECTED ABI (marth 2026-09-05, root-caused from a live deck CTD --
        // see the file banner's STANDING RULE). CommonLib's `void* GetMagicTarget
        // (CombatController*) const` is WRONG: the real engine callee (base impl
        // shared by all 14 caster vtables, confirmed against all three known
        // engine call sites' `lea rdx,[rsp+X]; mov r8,ctrl; call [rax+0x50]`
        // pattern) takes a HIDDEN 16-byte HANDLE+POINTER out-slot as its second
        // argument -- the Microsoft x64 ABI's mandatory convention for any
        // aggregate return that doesn't fit a single register. The engine's own
        // callers allocate `out` on THEIR stack and pass a pointer to it; we
        // never allocate it ourselves, only forward the caller's pointer through
        // to `orig` UNCHANGED and read it back afterward (never write to it).
        // ======================================================================

        // {handle, ptr} -- handle at +0x0 (4 bytes), 4 bytes of alignment padding,
        // ptr at +0x8 (8 bytes) = 16 bytes total. Matches the crash log's own
        // out-slot dump byte-for-byte: [out+0x00]=0 (handle), [out+0x08]=a pointer
        // value (here, garbage from the old 2-arg-shaped call).
        struct Out16 {
            std::uint32_t handle;
            RE::Actor*    ptr;
        };
        static_assert(sizeof(Out16) == 16, "Out16 must be exactly 16 bytes -- re-verify the engine's "
                                            "handle+pointer out-slot layout before trusting this shape");
        static_assert(offsetof(Out16, ptr) == 8, "Out16::ptr must sit at +0x8 -- matches the crash log's "
                                                  "own out-slot dump ([out+0x8] held the garbage pointer)");

        using GetMagicTarget_t = Out16* (*)(RE::CombatMagicCaster*, Out16*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_targetOrig;

        Out16* GetMagicTargetThunk(RE::CombatMagicCaster* a_this, Out16* a_out, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            GetMagicTarget_t orig;
            if (const auto oit = g_targetOrig.find(vt); oit != g_targetOrig.end()) {
                orig = reinterpret_cast<GetMagicTarget_t>(oit->second);
            } else {
                // See the file banner -- never fabricate a target pointer.
                spdlog::error("[aicastseats] GetMagicTarget: vtable 0x{} not in the recorded set -- "
                              "recovering the LIVE original instead of fabricating a target.",
                              apmf::log::Hex(vt, 16));
                orig = RecoverLiveOriginal<GetMagicTarget_t>(vt, kGetMagicTarget);
            }

            // TASK 3 (marth 2026-09-05): ENTRY log, BEFORE calling orig() and on a
            // throttle table of its own (g_lastTargetEntryMs, not the exit log's
            // g_lastTargetMs below) -- the 2026-09-05 deck crash logged CheckStartCast
            // -> YES and the AI's own BeginCastRight, but NO GetMagicTarget line ever
            // fired (entry or exit) before the CTD (the old 2-arg-shaped call was
            // corrupting its arguments before it could reach a log line). This line,
            // on its own dedup window, lets the next run distinguish "GetMagicTarget
            // was never called" (no ENTER line at all) from "it was called, and
            // something died in/after it" (an ENTER line with no matching exit line
            // further down). `a_out` is NOT yet filled at this point (the engine
            // caller's stack slot, uninitialized until `orig` runs) -- never read.
            if (a_cc) {
                if (auto* actorPre = a_cc->attackerHandle.get().get()) {
                    const auto fidPre      = actorPre->GetFormID();
                    auto*      spellPre    = a_this->magicItem;
                    const auto spellFormPre = spellPre ? spellPre->GetFormID() : 0;
                    if (ThrottleOK(g_lastTargetEntryMs, fidPre, spellFormPre)) {
                        const char* clsPre = ResolveTypeName(vt);
                        spdlog::info("[aicastseats] t={} 0x{} '{}' GetMagicTarget[ENTER][{}] spell=0x{} '{}'",
                                     apmf::clock::MonotonicMs(), apmf::log::Hex(fidPre),
                                     actorPre->GetName() ? actorPre->GetName() : "?",
                                     clsPre ? clsPre : "<unresolved>", apmf::log::Hex(spellFormPre),
                                     spellPre && spellPre->GetName() ? spellPre->GetName() : "?");
                    }
                }
            }

            // ENGINE ANSWERS FIRST -- the exact 3 arguments, unchanged, in the exact
            // engine order; `a_out` is the CALLER's out-slot, never one we allocate,
            // and its contents are never altered by us (only read back below, after
            // `orig` has filled it). Return EXACTLY what `orig` returned.
            Out16* result = orig(a_this, a_out, a_cc);

            if (!a_cc) return result;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return result;
            const auto fid = actor->GetFormID();

            auto*      spell     = a_this->magicItem;
            const auto spellForm = spell ? spell->GetFormID() : 0;

            if (ThrottleOK(g_lastTargetMs, fid, spellForm)) {
                // Classify off the FILLED out-struct (a_out, which orig() just wrote
                // into) -- never off `result` itself (a raw pointer identity, not the
                // resolved Actor*). a_out->ptr is a real RE::Actor* per the corrected
                // ABI; compared, never dereferenced beyond the pointer-equality checks
                // every other seat in this codebase already does the same way.
                auto*       attackerActor = actor;   // same object attackerHandle resolved above
                auto*       combatTarget  = a_cc->targetHandle.get().get();
                RE::Actor*  got           = a_out ? a_out->ptr : nullptr;
                const char* which =
                    !got                                    ? "NONE" :
                    (attackerActor && got == attackerActor) ? "SELF/ATTACKER" :
                    (combatTarget  && got == combatTarget)  ? "COMBAT_TARGET(foe)" :
                                                               "OTHER";
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[aicastseats] t={} 0x{} '{}' GetMagicTarget[EXIT][{}] spell=0x{} '{}' -> {} "
                             "(handle=0x{} ptr=0x{})",
                             apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                             actor->GetName() ? actor->GetName() : "?", cls ? cls : "<unresolved>",
                             apmf::log::Hex(spellForm), spell && spell->GetName() ? spell->GetName() : "?",
                             which, apmf::log::Hex(a_out ? a_out->handle : 0),
                             apmf::log::Hex(reinterpret_cast<std::uintptr_t>(got), 16));
            }
            return result;
        }

        // ======================================================================
        // SEAT 4 -- HOW LONG. CombatMagicCaster::CheckStopCast, vfunc 0x07.
        // ======================================================================

        using CheckStopCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_stopOrig;

        bool CheckStopCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            CheckStopCast_t orig;
            if (const auto oit = g_stopOrig.find(vt); oit != g_stopOrig.end()) {
                orig = reinterpret_cast<CheckStopCast_t>(oit->second);
            } else {
                // See the file banner -- never fabricate a stop/continue decision.
                spdlog::error("[aicastseats] CheckStopCast: vtable 0x{} not in the recorded set -- "
                              "recovering the LIVE original instead of fabricating a result.",
                              apmf::log::Hex(vt, 16));
                orig = RecoverLiveOriginal<CheckStopCast_t>(vt, kCheckStopCast);
            }

            const bool result = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST

            if (!a_cc) return result;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return result;
            const auto fid = actor->GetFormID();

            auto*      spell     = a_this->magicItem;
            const auto spellForm = spell ? spell->GetFormID() : 0;

            if (ThrottleOK(g_lastStopMs, fid, spellForm)) {
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[aicastseats] t={} 0x{} '{}' CheckStopCast[{}] spell=0x{} '{}' -> {}",
                             apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                             actor->GetName() ? actor->GetName() : "?", cls ? cls : "<unresolved>",
                             apmf::log::Hex(spellForm), spell && spell->GetName() ? spell->GetName() : "?",
                             result ? "STOP" : "CONTINUE");
            }
            return result;
        }

        std::atomic<bool> g_installed{ false };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[aicastseats] VR runtime -- the CombatInventoryItem/CombatMagicCaster vtable "
                         "indices are SE/AE-only verified; the observe-only seat probe was NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        // TASK 2 (marth 2026-09-05): the 2026-09-05 deck run produced a NEW crash
        // signature that did not occur before this probe was deployed -- the probe
        // is the prime suspect but NOT proven, since MFO's pre-existing Targeting-
        // hook cast-chain crashes share frames 4-8 with it. Split into two
        // INDEPENDENTLY selectable groups, each install-gated by its own INI flag,
        // so a deck run can arm exactly ONE and isolate which seat (if either) is
        // implicated. Both OFF by default -- see ReadIniFlag's comment for why an
        // INI read (not a hotkey) is the right call here.
        const bool groupA = ReadIniFlag("EnableItemScoreProbe");   // Group A: CalculateScore (item vtables)
        const bool groupB = ReadIniFlag("EnableCasterSeatProbe");  // Group B: CheckStartCast/CheckStopCast/
                                                                    // GetMagicTarget (caster vtables)
        // GROUP C (marth 2026-09-06, Opus PASS S brief): the WEAPON-class
        // CalculateScore probe -- UNRELATED to the groupA/groupB crash-isolation
        // history above (different vtables entirely: raw-RVA weapon leaves, not
        // the RTTI-verified magic item/caster symbols). TASK 1 there is PRIMARY
        // and ships ENABLED by default (CalculateScore is a scalar-return vfunc,
        // categorically immune to the GetMagicTarget-class sret ABI bug this file's
        // banner documents -- see kAiCastSeats's SEAT 1 for that same reasoning
        // already applied to the magic-item CalculateScore probe). TASK 2 (score
        // steer) is its own flag and stays OFF until this probe's own field data
        // confirms 0x0C actually runs.
        const bool groupC     = ReadIniFlag("EnableWeaponScoreProbe", 1);   // GROUP C, default ON (TASK 1)
        const bool scoreSteer = ReadIniFlag("EnableScoreSteer", 0);         // TASK 2 (score bias), default OFF
        // TASK 2 (dual-wield preference), marth 2026-09-06 PASS S brief -- its OWN
        // section/key per the brief ([EquipGate] EnableDualWieldPreference, not
        // [AiCastSeats]): the FACET this flag governs (Shield admission, T2a's
        // own allowance-template contract) lives conceptually with EquipGate.cpp's
        // other equip-gate flags even though the hook itself installs here (see
        // ShieldEquipGateThunk's own comment for why). Bypasses ReadIniFlag (which
        // hardcodes the [AiCastSeats] section) for that reason.
        const bool dualWieldPref = GetPrivateProfileIntA("EquipGate", "EnableDualWieldPreference", 0,
                                                          "Data/SKSE/Plugins/APMF.ini") != 0;
        int nScore = 0, nStart = 0, nStop = 0, nTgt = 0, nWeaponScore = 0, nWeaponRefused = 0;
        int nShieldEquip = 0;

        // TASK 1 per-hand resolution (see g_leftHandSlot's own comment) --
        // resolved unconditionally, harmless if GROUP C never installs anything
        // to use it. Same engine singleton core/EquipGate.cpp already reads;
        // never a hardcoded FormID. A null result just means every WEAPON-SCORE
        // line below reports hand=? -- never a crash, never a guess.
        if (auto* dobj = RE::BGSDefaultObjectManager::GetSingleton()) {
            g_leftHandSlot.store(ResolveHandSlot(dobj, RE::DEFAULT_OBJECT::kLeftHandEquip, "left"),
                                 std::memory_order_release);
            g_rightHandSlot.store(ResolveHandSlot(dobj, RE::DEFAULT_OBJECT::kRightHandEquip, "right"),
                                  std::memory_order_release);
        }

        // TASK 2 perk resolution -- ONCE, main-thread-legal here (TESForm::
        // LookupByID takes the engine's own forms-map lock, which is exactly
        // why every combat-thread reader in this codebase resolves its forms
        // at Install() and caches the pointer, never per-call). See the
        // constants' own comment (above g_dualFlurryRank1) for how these three
        // FormIDs were verified. A null result (a load order that somehow
        // lacks Skyrim.esm's own perks) just means HasDualWieldInvestment can
        // never return true -- the feature degrades to fully inert, never a
        // guess or a crash.
        if (dualWieldPref) {
            g_dualFlurryRank1.store(RE::TESForm::LookupByID<RE::BGSPerk>(kDualFlurryRank1FormID),
                                    std::memory_order_release);
            g_dualFlurryRank2.store(RE::TESForm::LookupByID<RE::BGSPerk>(kDualFlurryRank2FormID),
                                    std::memory_order_release);
            g_dualSavagery.store(RE::TESForm::LookupByID<RE::BGSPerk>(kDualSavageryFormID),
                                 std::memory_order_release);
        }

        // ---- GROUP A / SEAT 1: CalculateScore (0x0C) on the 30 concrete spell/
        // staff CombatInventoryItem vtables -- the IDENTICAL list core/EquipGate.cpp
        // already RTTI-verifies and patches at slot 0x0F, verbatim (a different
        // slot on the same symbols never disturbs that existing hook).
        if (groupA) {
            REL::Relocation<void*> itemTD{ RE::RTTI_CombatInventoryItem };
            const REL::VariantID kItemVtables[] = {
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
            nScore = allowance::InstallOnVtables(kItemVtables, kCalculateScore, &CalculateScoreThunk,
                                                  itemTD.get(), "aicastseats-score", g_scoreOrig);
            // TASK 1 completeness check: every vtable InstallOnVtables actually wrote to
            // gets its original recorded in g_scoreOrig in that SAME statement (see
            // Allowance.h), so nScore == |kItemVtables| here is what guarantees a
            // runtime lookup miss below can only be the structurally-unreachable case
            // the file banner describes. A mismatch means some symbol failed its RTTI
            // derivation (already WARN-logged by InstallOnVtables) -- that type is
            // simply never intercepted, not a partially-recorded original.
            if (nScore != static_cast<int>(std::size(kItemVtables))) {
                spdlog::error("[aicastseats] GROUP A: CalculateScore installed on {}/{} item vtable(s) -- "
                              "{} symbol(s) failed RTTI derivation (see the WARN above) and are simply NOT "
                              "intercepted (untouched, call their real original directly).",
                              nScore, static_cast<int>(std::size(kItemVtables)),
                              static_cast<int>(std::size(kItemVtables)) - nScore);
            }
        }   // groupA

        // ---- GROUP B / SEATS 2-4: CheckStartCast (0x06) / CheckStopCast (0x07) /
        // GetMagicTarget (0x0A) on the 14 concrete CombatMagicCaster vtables --
        // the IDENTICAL list MFO's native/CasterConsent.cpp already patches at
        // slot 0x06 (CombatMagicCasterArmor deliberately excluded, ENGINE_NOTES
        // §0.28: a vtable symbol with no real class behind it).
        if (groupB) {
            REL::Relocation<void*> casterTD{ RE::RTTI_CombatMagicCaster };
            const REL::VariantID kCasterVtables[] = {
                RE::VTABLE_CombatMagicCasterOffensive[0],    RE::VTABLE_CombatMagicCasterRestore[0],
                RE::VTABLE_CombatMagicCasterWard[0],         RE::VTABLE_CombatMagicCasterSummon[0],
                RE::VTABLE_CombatMagicCasterStagger[0],      RE::VTABLE_CombatMagicCasterDisarm[0],
                RE::VTABLE_CombatMagicCasterCloak[0],        RE::VTABLE_CombatMagicCasterLight[0],
                RE::VTABLE_CombatMagicCasterInvisibility[0], RE::VTABLE_CombatMagicCasterBoundItem[0],
                RE::VTABLE_CombatMagicCasterTargetEffect[0], RE::VTABLE_CombatMagicCasterParalyze[0],
                RE::VTABLE_CombatMagicCasterScript[0],       RE::VTABLE_CombatMagicCasterReanimate[0],
            };
            constexpr int kExpectedCaster = static_cast<int>(std::size(kCasterVtables));

            nStart = allowance::InstallOnVtables(kCasterVtables, kCheckStartCast, &CheckStartCastThunk,
                                                  casterTD.get(), "aicastseats-start", g_startOrig);
            nStop  = allowance::InstallOnVtables(kCasterVtables, kCheckStopCast, &CheckStopCastThunk,
                                                  casterTD.get(), "aicastseats-stop", g_stopOrig);
            nTgt   = allowance::InstallOnVtables(kCasterVtables, kGetMagicTarget, &GetMagicTargetThunk,
                                                  casterTD.get(), "aicastseats-target", g_targetOrig);
            // TASK 1 completeness check -- same reasoning as Group A above; all three
            // slots share the identical RTTI check on the identical vtable list, so
            // nStart == nStop == nTgt == kExpectedCaster is the expected steady state.
            if (nStart != kExpectedCaster || nStop != kExpectedCaster || nTgt != kExpectedCaster) {
                spdlog::error("[aicastseats] GROUP B: CheckStartCast/CheckStopCast/GetMagicTarget installed "
                              "on {}/{}/{} of {} caster vtable(s) -- an inconsistent count across three "
                              "identical-list installs means a bug in InstallOnVtables itself, not a normal "
                              "RTTI skip (which would affect all three identically); investigate before "
                              "trusting this group's data.",
                              nStart, nStop, nTgt, kExpectedCaster);
            }
        }   // groupB

        // ---- GROUP C: CalculateScore (0x0C) on the Melee/Ranged/Shield/Torch
        // weapon-class leaves -- raw RVA, AE-only, no CommonLib symbol. See the
        // GROUP C block comment above WeaponScoreThunk for the full design.
        if (groupC) {
            if (!REL::Module::IsAE()) {
                spdlog::warn("[aicastseats] GROUP C (weapon-class item score) requires AE (1.6.1170) -- "
                             "the Melee/Ranged/Shield/Torch vtable RVAs are AE-only disassembly-confirmed "
                             "and meaningless on any other runtime. NOT installed on this build.");
            } else {
                for (const auto& spec : kWeaponClasses) {
                    REL::Relocation<std::uintptr_t> vt{ REL::Offset(spec.vtableRva) };
                    REL::Relocation<std::uintptr_t> expectedFn{ REL::Offset(spec.calcScoreRva) };
                    // Read the LIVE slot 0x0C pointer BEFORE writing anything (the
                    // same read RecoverLiveOriginal performs elsewhere in this file) --
                    // the install-time gate, never a blind vtable write.
                    const auto curFn = reinterpret_cast<std::uintptr_t>(
                        RecoverLiveOriginal<WeaponScore_t>(vt.address(), kCalculateScore));
                    if (curFn != expectedFn.address()) {
                        spdlog::error("[aicastseats] GROUP C: {} vtable 0x{} slot 0x0C holds 0x{}, expected "
                                      "the disasm-confirmed CalculateScore address 0x{} -- REFUSED (not "
                                      "installed; never a blind vtable write; the vtable-RVA table may be "
                                      "stale for this build).",
                                      spec.tag, apmf::log::Hex(vt.address(), 16), apmf::log::Hex(curFn, 16),
                                      apmf::log::Hex(expectedFn.address(), 16));
                        ++nWeaponRefused;
                        continue;
                    }
                    const char* rtti = ResolveTypeName(vt.address());
                    g_weaponClassInfo[vt.address()] = { spec.tag, spec.category };
                    g_weaponScoreOrig[vt.address()] = vt.write_vfunc(kCalculateScore, &WeaponScoreThunk);
                    spdlog::info("[aicastseats] GROUP C: {} CalculateScore hooked (vtable 0x{}, category {}, "
                                 "RTTI name '{}' -- informational only, not verified against a known base).",
                                 spec.tag, apmf::log::Hex(vt.address(), 16), spec.category,
                                 rtti ? rtti : "<unresolved>");
                    ++nWeaponScore;

                    // TASK 2 -- Shield ONLY, and only after the SAME 0x0C identity
                    // check above already passed for this exact vtable object (the
                    // continue on refusal a few lines up means we never reach here
                    // for a Shield entry whose vtable identity is unconfirmed). See
                    // ShieldEquipGateThunk's own comment for the full design and the
                    // weaker-guard callout.
                    if (dualWieldPref && std::string_view(spec.tag) == "Shield") {
                        const auto curEquipFn = reinterpret_cast<std::uintptr_t>(
                            RecoverLiveOriginal<ShieldEquip_t>(vt.address(), kCheckShouldEquip));
                        if (curEquipFn == 0) {
                            spdlog::error("[aicastseats] TASK2: Shield vtable 0x{} slot 0x0F holds a null "
                                          "pointer -- REFUSED (dual-wield preference NOT installed; never a "
                                          "blind vtable write).",
                                          apmf::log::Hex(vt.address(), 16));
                        } else {
                            g_shieldEquipOrig = curEquipFn;
                            g_shieldVtableAddr.store(vt.address(), std::memory_order_release);
                            vt.write_vfunc(kCheckShouldEquip, &ShieldEquipGateThunk);
                            spdlog::info("[aicastseats] TASK2: dual-wield preference ARMED -- Shield "
                                         "CheckShouldEquip (slot 0x0F) hooked on the same identity-verified "
                                         "vtable 0x{} GROUP C's own CalculateScore check just confirmed is "
                                         "genuinely Shield's (no independent disasm-confirmed address exists "
                                         "for 0x0F itself -- see ShieldEquipGateThunk's comment for why that "
                                         "is still a safe install).",
                                         apmf::log::Hex(vt.address(), 16));
                            ++nShieldEquip;
                        }
                    }
                }
            }
        }   // groupC

        g_scoreSteerEnabled.store(scoreSteer && nWeaponScore > 0, std::memory_order_relaxed);
        g_dualWieldPrefEnabled.store(dualWieldPref && nShieldEquip > 0, std::memory_order_relaxed);

        spdlog::info("[aicastseats] OBSERVE-ONLY seat probe: GROUP A (item score) {} -- CalculateScore "
                     "on {} item vtable(s). GROUP B (caster seats) {} -- CheckStartCast/CheckStopCast/"
                     "GetMagicTarget on {}/{}/{} caster vtable(s). GROUP C (weapon-class item score) {} -- "
                     "CalculateScore on {}/{} weapon vtable(s) ({} refused the install-time function-"
                     "pointer check). Score-steer (TASK 2a, biases a claimed form's own score) is {} "
                     "([AiCastSeats] EnableScoreSteer, default 0) -- {}. Dual-wield preference (TASK 2b, "
                     "denies Shield admission for a perked, confirmed-second-weapon one-handed follower) is "
                     "{} ([EquipGate] EnableDualWieldPreference, default 0) -- {}. All flags read ONCE from "
                     "Data/SKSE/Plugins/APMF.ini: [AiCastSeats] EnableItemScoreProbe / EnableCasterSeatProbe "
                     "(default 0=OFF), EnableWeaponScoreProbe (default 1=ON), EnableScoreSteer (default 0); "
                     "[EquipGate] EnableDualWieldPreference (default 0). Every probe chains to the original "
                     "unconditionally when its own steer/deny flag is OFF; never alters an argument.",
                     groupA ? "ARMED" : "OFF", nScore, groupB ? "ARMED" : "OFF", nStart, nStop, nTgt,
                     groupC ? "ARMED" : "OFF", nWeaponScore, static_cast<int>(std::size(kWeaponClasses)),
                     nWeaponRefused,
                     g_scoreSteerEnabled.load(std::memory_order_relaxed) ? "ARMED" : "OFF",
                     !scoreSteer                ? "flag is off" :
                     nWeaponScore == 0          ? "flag is on but GROUP C installed 0 vtables, so it cannot fire" :
                                                   "GROUP C is live -- steer will bias a claimed form's own weapon score",
                     g_dualWieldPrefEnabled.load(std::memory_order_relaxed) ? "ARMED" : "OFF",
                     !dualWieldPref ? "flag is off" :
                     nShieldEquip == 0 ? "flag is on but the Shield CheckShouldEquip install was refused, so it cannot fire" :
                                         "Shield admission-deny is live");
    }

}
