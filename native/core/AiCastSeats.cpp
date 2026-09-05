#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/AiCastSeats.h"

// ============================================================================
// See AiCastSeats.h for the design/why. Every thunk below follows the exact
// same shape: recover the original for THIS vtable (a foreign vtable -- which
// cannot happen in practice, since the thunk is only ever reached through a
// slot this file itself installed -- gets a benign, never-read-members
// default), call it FIRST, log a throttled observe line off the result, then
// return EXACTLY what the engine returned. No argument is ever read before
// the original runs; no return value is ever altered afterward.
// ============================================================================

namespace apmf::aicastseats {

    namespace {

        constexpr std::uint64_t kThrottleMs = 1500;   // per (actor, subject) cadence, all four seats

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
            const auto oit = g_scoreOrig.find(vt);
            if (oit == g_scoreOrig.end()) return 0.0f;   // unreachable in practice -- foreign vtable
            const auto orig = reinterpret_cast<CalculateScore_t>(oit->second);

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
        // SEAT 2 -- WHETHER to cast. CombatMagicCaster::CheckStartCast, vfunc 0x06.
        // ======================================================================

        using CheckStartCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_startOrig;

        bool CheckStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_startOrig.find(vt);
            if (oit == g_startOrig.end()) return false;   // unreachable in practice -- foreign vtable
            const auto orig = reinterpret_cast<CheckStartCast_t>(oit->second);

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
        // Returns an opaque `void*` in the pinned header (CommonLib itself never
        // resolved the real return type here) -- this probe NEVER dereferences
        // it. It only compares the raw pointer VALUE the engine returned against
        // two INDEPENDENTLY, safely resolved Actor* pointers (the same
        // attackerHandle/targetHandle accessors every other seat in this
        // codebase already uses) to classify the aim as SELF/ATTACKER,
        // COMBAT_TARGET (the foe), or OTHER/NONE -- exactly deliverable 3's
        // question: is a beneficial (Restore-classified) cast aimed at the foe
        // rather than an ally?
        // ======================================================================

        using GetMagicTarget_t = void* (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_targetOrig;

        void* GetMagicTargetThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_targetOrig.find(vt);
            if (oit == g_targetOrig.end()) return nullptr;   // unreachable in practice -- foreign vtable
            const auto orig = reinterpret_cast<GetMagicTarget_t>(oit->second);

            void* target = orig(a_this, a_cc);   // ENGINE ANSWERS FIRST -- never altered below

            if (!a_cc) return target;
            auto* actor = a_cc->attackerHandle.get().get();
            if (!actor) return target;
            const auto fid = actor->GetFormID();

            auto*      spell     = a_this->magicItem;
            const auto spellForm = spell ? spell->GetFormID() : 0;

            if (ThrottleOK(g_lastTargetMs, fid, spellForm)) {
                auto*       attackerActor = actor;   // same object attackerHandle resolved above
                auto*       combatTarget  = a_cc->targetHandle.get().get();
                const char* which =
                    !target                                                    ? "NONE" :
                    (attackerActor && target == static_cast<void*>(attackerActor)) ? "SELF/ATTACKER" :
                    (combatTarget  && target == static_cast<void*>(combatTarget))  ? "COMBAT_TARGET(foe)" :
                                                                                      "OTHER";
                const char* cls = ResolveTypeName(vt);
                spdlog::info("[aicastseats] t={} 0x{} '{}' GetMagicTarget[{}] spell=0x{} '{}' -> {} "
                             "(raw=0x{})",
                             apmf::clock::MonotonicMs(), apmf::log::Hex(fid),
                             actor->GetName() ? actor->GetName() : "?", cls ? cls : "<unresolved>",
                             apmf::log::Hex(spellForm), spell && spell->GetName() ? spell->GetName() : "?",
                             which, apmf::log::Hex(reinterpret_cast<std::uintptr_t>(target), 16));
            }
            return target;
        }

        // ======================================================================
        // SEAT 4 -- HOW LONG. CombatMagicCaster::CheckStopCast, vfunc 0x07.
        // ======================================================================

        using CheckStopCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_stopOrig;

        bool CheckStopCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_stopOrig.find(vt);
            if (oit == g_stopOrig.end()) return true;   // unreachable in practice -- foreign vtable
            const auto orig = reinterpret_cast<CheckStopCast_t>(oit->second);

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

        // ---- SEAT 1: CalculateScore (0x0C) on the 30 concrete spell/staff
        // CombatInventoryItem vtables -- the IDENTICAL list core/EquipGate.cpp
        // already RTTI-verifies and patches at slot 0x0F, verbatim (a different
        // slot on the same symbols never disturbs that existing hook).
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
        constexpr std::size_t kCalculateScore = 0x0C;
        const int nScore = allowance::InstallOnVtables(kItemVtables, kCalculateScore, &CalculateScoreThunk,
                                                        itemTD.get(), "aicastseats-score", g_scoreOrig);

        // ---- SEATS 2-4: CheckStartCast (0x06) / CheckStopCast (0x07) /
        // GetMagicTarget (0x0A) on the 14 concrete CombatMagicCaster vtables --
        // the IDENTICAL list MFO's native/CasterConsent.cpp already patches at
        // slot 0x06 (CombatMagicCasterArmor deliberately excluded, ENGINE_NOTES
        // §0.28: a vtable symbol with no real class behind it).
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
        constexpr std::size_t kCheckStartCast  = 0x06;
        constexpr std::size_t kCheckStopCast   = 0x07;
        constexpr std::size_t kGetMagicTarget  = 0x0A;

        const int nStart = allowance::InstallOnVtables(kCasterVtables, kCheckStartCast, &CheckStartCastThunk,
                                                        casterTD.get(), "aicastseats-start", g_startOrig);
        const int nStop  = allowance::InstallOnVtables(kCasterVtables, kCheckStopCast, &CheckStopCastThunk,
                                                        casterTD.get(), "aicastseats-stop", g_stopOrig);
        const int nTgt   = allowance::InstallOnVtables(kCasterVtables, kGetMagicTarget, &GetMagicTargetThunk,
                                                        casterTD.get(), "aicastseats-target", g_targetOrig);

        spdlog::info("[aicastseats] OBSERVE-ONLY seat probe armed -- CalculateScore on {} item "
                     "vtable(s); CheckStartCast/CheckStopCast/GetMagicTarget on {}/{}/{} caster "
                     "vtable(s). Chains to the original unconditionally; never alters an argument or "
                     "a return value.",
                     nScore, nStart, nStop, nTgt);
    }

}
