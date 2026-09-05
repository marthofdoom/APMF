#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/AiCastSeats.h"

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
        bool ReadIniFlag(const char* a_key) {
            return GetPrivateProfileIntA("AiCastSeats", a_key, 0, "Data/SKSE/Plugins/APMF.ini") != 0;
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
        constexpr std::size_t kCalculateScore = 0x0C;
        constexpr std::size_t kCheckStartCast = 0x06;
        constexpr std::size_t kCheckStopCast  = 0x07;
        constexpr std::size_t kGetMagicTarget = 0x0A;

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
        int nScore = 0, nStart = 0, nStop = 0, nTgt = 0;

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

        spdlog::info("[aicastseats] OBSERVE-ONLY seat probe: GROUP A (item score) {} -- CalculateScore "
                     "on {} item vtable(s). GROUP B (caster seats) {} -- CheckStartCast/CheckStopCast/"
                     "GetMagicTarget on {}/{}/{} caster vtable(s). Both flags read ONCE from "
                     "Data/SKSE/Plugins/APMF.ini [AiCastSeats] EnableItemScoreProbe / "
                     "EnableCasterSeatProbe (0/1, default 0=OFF). Chains to the original "
                     "unconditionally; never alters an argument or a return value.",
                     groupA ? "ARMED" : "OFF", nScore, groupB ? "ARMED" : "OFF", nStart, nStop, nTgt);
    }

}
