#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/CastClassify.h"

#include <cstring>

// Win32 INI read for the one kill-switch below. Declared by hand, exactly like
// core/AiCastSeats.cpp / core/CastSeats.cpp / core/Hook.cpp do -- PCH does not
// pull in <Windows.h>, and this is the single Win32 call this file needs.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See core/CastClassify.h for the design/why. This TU is the single
// CombatMagicItemData vfunc-slot-1 thunk.
// ============================================================================

namespace apmf::castclassify {

    namespace {

        // ---- THE ONE VTABLE (RE notebook J7/J9). Not a CommonLib symbol -- see
        // the header banner for why. Triple order is REL::VariantID(seID, aeID,
        // vrOffset), verified against the ctor signature the exact same way
        // core/CombatBehaviorRE.h's own local RE extension documents doing for
        // RTTI_CombatBehaviorTreeNode. VR is refused before this is ever
        // resolved (Install()), so the VR offset is carried for documentation
        // completeness only, never dereferenced on a VR runtime.
        constexpr REL::VariantID kCombatMagicItemDataVtable(265000, 211955, 0x1709ce8);

        // Mangled RTTI name, read directly off the 1.6.1170 disassembly (J7:
        // "RTTI .?AVCombatMagicItemData@@"). No RTTI_CombatMagicItemData
        // Address-Library ID was established during the RE pass, so install-time
        // verification here is a STRING match on this name (walked off the
        // resolved vtable's own CompleteObjectLocator/TypeDescriptor, exactly
        // like core/AiCastSeats.cpp's ResolveTypeName) rather than
        // allowance::DerivesFrom's pointer-identity walk against a known base
        // TypeDescriptor. A mismatch refuses the install outright -- never a
        // blind vtable write.
        constexpr const char* kExpectedMangledName = ".?AVCombatMagicItemData@@";

        // ---- the three raw offsets this thunk reads. NOT static_assert'able --
        // CombatMagicItemData is not a CommonLib-declared type in this pinned
        // rev (no header exists to hold a member for offsetof() to check
        // against); the four guards here (RTTI-name match, per-call vtable
        // identity, an INI kill-switch, AE-only refusal) stand in exactly where
        // core/CastSeats.cpp's own `+0x30` aim-override offset explains they
        // must (Docs/INVARIANTS.md #20's "raw offset in a composed seat"
        // clause). AE 1.6.1170 disassembly-CERTAIN (J9); SE 1.5.97 explicitly
        // NOT VERIFIED -- refused at Install() rather than guessed.
        constexpr std::uintptr_t kSpellOffset    = 0x10;   // RE::MagicItem* -- the effect's owning spell
        constexpr std::uintptr_t kCtrlOffset     = 0x18;   // RE::CombatController* -- the deliberating actor's controller
        constexpr std::uintptr_t kSelfFlagOffset = 0x4c;   // std::uint8_t -- (spell->GetDelivery()==kSelf) at ctor time

        constexpr std::size_t kClassifySlot = 1;   // CombatMagicItemData vtable slot 1 -- the per-effect visitor

        constexpr std::uint64_t kLogThrottleMs = 1500;   // matches every other seat's cadence in this codebase

        std::mutex                                       g_rlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;

        bool LogDue(RE::FormID a_actor, RE::FormID a_subject) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) | static_cast<std::uint64_t>(a_subject);
            std::scoped_lock lk(g_rlMx);
            auto& last = g_lastLogMs[key];
            if (now - last < kLogThrottleMs) return false;
            last = now;
            return true;
        }

        // Mirrors core/AiCastSeats.cpp's ResolveTypeName exactly (same
        // reasoning: raw MSVC-decorated name, no demangler exists anywhere in
        // this codebase or CommonLib, and hand-rolling one would itself be the
        // kind of fragile guess this whole file exists to avoid). Returns
        // nullptr (never guesses) if any link in the RTTI chain is absent.
        const char* ResolveMangledName(std::uintptr_t a_vtableAddr) {
            if (!a_vtableAddr) return nullptr;
            auto* colPtr = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(a_vtableAddr - sizeof(void*));
            if (!colPtr) return nullptr;
            auto* td = colPtr->typeDescriptor.get();
            if (!td) return nullptr;
            return td->mangled_name();
        }

        // (CombatMagicItemData* this, RE::Effect* e) -> continue-visiting flag
        // (always 1 per J2/P3.3b). Scalar return -- categorically safe from the
        // hidden-sret-outslot bug class core/AiCastSeats.cpp's banner records for
        // GetMagicTarget (that bug only bites an aggregate return that does not
        // fit a register; a u32/int return never triggers it).
        using Classify_t = std::uint32_t (*)(void*, void*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;

        std::uint32_t ClassifyThunk(void* a_this, void* a_effect) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) {
                // Structurally unreachable (this thunk is only ever entered
                // through the ONE slot Install() wrote) -- see
                // core/AiCastSeats.cpp's file banner for why a lookup miss
                // recovers a REAL live original instead of fabricating a
                // classification-continue result.
                spdlog::error("[ch.8b seat 0] ClassifyThunk: vtable 0x{} not in the recorded set -- "
                              "recovering the LIVE original instead of fabricating a result.",
                              apmf::log::Hex(vt, 16));
                auto live = *reinterpret_cast<Classify_t*>(vt + kClassifySlot * sizeof(void*));
                return live(a_this, a_effect);
            }
            const auto orig = reinterpret_cast<Classify_t>(oit->second);

            auto*      bytes    = reinterpret_cast<std::byte*>(a_this);
            auto*      spellPtr = *reinterpret_cast<RE::MagicItem**>(bytes + kSpellOffset);
            auto*      ccPtr    = *reinterpret_cast<RE::CombatController**>(bytes + kCtrlOffset);
            auto*      selfFlag = reinterpret_cast<std::uint8_t*>(bytes + kSelfFlagOffset);
            const auto before   = *selfFlag;

            // Only a spell the ctor did NOT already mark self-delivery is ever a
            // candidate here -- a genuine self-heal needs no help (J9: forcing
            // +0x4c changes classification and nothing else, so touching an
            // already-1 flag would be a no-op at best; skipped outright rather
            // than risk any interaction with the ctor's own concentration flag
            // at +0x4d, which this file never reads or writes).
            //
            // DELIBERATELY NOT RESTORED after the call (a difference from the RE
            // notebook's own first-draft Q4 phrasing, "flip it for the duration of
            // the call, restore after"). P3.7 confirms the SAME reasoning applies
            // either way for THIS spell's classification pass -- +0x4c is read
            // ONLY inside this visitor and nowhere downstream -- but a spell with
            // MULTIPLE effects re-enters this thunk once PER EFFECT on the SAME
            // resolver object (same `this`, same spell). Delivery is a property of
            // the SPELL, not of one effect, so once the first effect's call proves
            // this resolver's spell is the claim's driven form, every LATER effect
            // of that same spell should see the identical answer -- restoring to 0
            // between effects would silently un-force every effect but the first.
            // Leaving it set is therefore the MORE correct reading for a
            // multi-effect heal, not merely an equally-safe shortcut; the `before
            // == 0` guard above already makes this idempotent (a spell already
            // forced to 1 by an earlier effect in the same pass is simply left
            // alone on the next).
            if (before == 0 && spellPtr && ccPtr) {
                if (auto attPtr = ccPtr->attackerHandle.get()) {
                    if (auto* actor = attPtr.get()) {
                        const RE::FormID fid       = actor->GetFormID();
                        const RE::FormID spellForm = spellPtr->GetFormID();
                        apmf::CastSeatClaim seat{};
                        if (apmf::ControlMap::Get().TryGetCastSeatClaim(fid, seat) && seat.targetHandle) {
                            const RE::FormID driven = seat.proxy ? seat.proxy : seat.spell;
                            if (driven != 0 && spellForm == driven) {
                                // HOSTILE GUARD (2026-09-06, offense-seat-scope). The rescue
                                // exists SOLELY because the 23-row table has no row for
                                // (Health, self=0, hostile=0) -- a HOSTILE claim (e.g.
                                // Firebolt) already keys into an EXISTING row via its own
                                // hostile=1 bit (archetype<<16 | av<<8 | hostile<<1 |
                                // isSelfDelivery, Docs/STATUS.md) and needs no help. Forcing
                                // isSelfDelivery=1 on it would instead misclassify it into a
                                // (archetype, av, hostile=1, isSelfDelivery=1) row vanilla data
                                // never populates -- a hostile spell is never self-cast -- which
                                // could silently break an offense cast that already works today.
                                // Read the SAME "hostile" component the classifier's own key
                                // uses, via `RE::Effect::IsHostile()` -- a real, ordinary
                                // CommonLib method on the argument's OWN documented type (unlike
                                // this file's three raw CombatMagicItemData offsets, which have
                                // no header to check against); not a guess. A delivery-flip
                                // proxy (Heal Other/Healing Hands) is always non-hostile by
                                // construction (core/CastProxy.h only flips kSelf-delivery
                                // BENEFICIAL spells), so this guard costs that path nothing.
                                // NOTE: `Effect::IsHostile()` forwards to `baseEffect->IsHostile()`
                                // UNCONDITIONALLY (no null check of its own, per its CommonLib
                                // implementation) -- so `baseEffect` is null-checked HERE first,
                                // never assumed, before it is ever called.
                                const auto* eff     = reinterpret_cast<const RE::Effect*>(a_effect);
                                const bool  hostile = eff && eff->baseEffect && eff->IsHostile();
                                if (!hostile) {
                                    *selfFlag = 1;   // SET BEFORE CHAINING -- orig() reads this field itself
                                    if (LogDue(fid, spellForm))
                                        spdlog::info(
                                            "[ch.8b seat 0] 0x{} CLASSIFY spell=0x{} '{}' selfFlag {}->{} "
                                            "(matches the live cast claim's driven form{}) -- this effect now "
                                            "keys into the SAME table row a self-heal uses (Restore/Ward/etc., "
                                            "creator 0x824510-family); nothing else about the row, score or "
                                            "duration changes.",
                                            apmf::log::Hex(fid), apmf::log::Hex(spellForm),
                                            spellPtr->GetName() ? spellPtr->GetName() : "?", before, *selfFlag,
                                            seat.proxy ? " -- via delivery-flip proxy" : "");
                                } else if (LogDue(fid, spellForm)) {
                                    spdlog::info(
                                        "[ch.8b seat 0] 0x{} CLASSIFY spell=0x{} '{}' selfFlag {} -- matches "
                                        "the live cast claim's driven form but the EFFECT is HOSTILE; NOT "
                                        "forced (the classify rescue only widens the non-hostile heal/buff-"
                                        "OTHER row -- a hostile claim already classifies correctly on its own).",
                                        apmf::log::Hex(fid), apmf::log::Hex(spellForm),
                                        spellPtr->GetName() ? spellPtr->GetName() : "?", before);
                                }
                            } else if (driven != 0 && LogDue(fid, spellForm)) {
                                // A live claim stands on this actor but names a
                                // DIFFERENT spell -- logged at the SAME throttle so
                                // "the claim never even reached this seat" is
                                // distinguishable in the field log from "reached
                                // it, didn't match."
                                spdlog::info(
                                    "[ch.8b seat 0] 0x{} CLASSIFY spell=0x{} '{}' selfFlag {} -- a live cast "
                                    "claim stands (driven=0x{}) but names a DIFFERENT spell; not forced.",
                                    apmf::log::Hex(fid), apmf::log::Hex(spellForm),
                                    spellPtr->GetName() ? spellPtr->GetName() : "?", before,
                                    apmf::log::Hex(driven));
                            }
                        }
                    }
                }
            }

            return orig(a_this, a_effect);   // THE ENGINE'S OWN CLASSIFICATION LOGIC, SEEING WHATEVER WE SET ABOVE
        }

        bool ReadIniFlag(const char* a_key, long a_default) {
            return GetPrivateProfileIntA("CastSeats", a_key, a_default, "Data/SKSE/Plugins/APMF.ini") != 0;
        }

        std::atomic<bool> g_installed{ false };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ch.8b seat 0] VR runtime -- the CombatMagicItemData vtable/offsets are AE-only "
                         "verified; SEAT 0 (classify) was NOT installed. A heal-OTHER spell will not get a "
                         "combat-AI item on this runtime; the direct-force degrade path is unaffected.");
            return;
        }
        if (!REL::Module::IsAE()) {
            spdlog::warn("[ch.8b seat 0] non-AE runtime (SE 1.5.97, or unrecognised) -- the +0x10/+0x18/"
                         "+0x4c CombatMagicItemData offsets this seat reads are disassembly-CERTAIN on "
                         "1.6.1170 (AE) ONLY; the RE notebook explicitly could NOT verify them on SE. "
                         "Refusing to install rather than guess a struct layout carries across runtimes "
                         "unchanged (CLAUDE.md: 'gate the SE path off or refuse it explicitly'). Heal-OTHER "
                         "stays absent on this runtime; the direct-force degrade path is unaffected.");
            return;
        }
        if (g_installed.exchange(true)) return;

        if (!ReadIniFlag("EnableSeat0Classify", 1)) {
            spdlog::info("[ch.8b seat 0] disabled by [CastSeats] EnableSeat0Classify=0 -- a heal-OTHER "
                         "spell will not get a combat-AI item; the direct-force degrade path is unaffected.");
            return;
        }

        REL::Relocation<std::uintptr_t> vt{ kCombatMagicItemDataVtable };
        const char* name = ResolveMangledName(vt.address());
        if (!name || std::strcmp(name, kExpectedMangledName) != 0) {
            spdlog::error("[ch.8b seat 0] CombatMagicItemData vtable 0x{} did NOT resolve to the expected "
                          "RTTI name '{}' (got '{}') -- REFUSED (not installed; never a blind vtable write).",
                          apmf::log::Hex(vt.address(), 16), kExpectedMangledName, name ? name : "<unresolved>");
            return;
        }

        g_orig[vt.address()] = vt.write_vfunc(kClassifySlot, &ClassifyThunk);

        spdlog::info("[ch.8b seat 0] CLASSIFY installed on CombatMagicItemData (RTTI-name-verified, vtable "
                     "0x{}), slot {} -- while a kIntent_Cast claim stands, its driven spell's effects "
                     "classify into the SAME engine table row a self-heal uses, so the combat AI's own "
                     "Rebuild can mint a real CombatInventoryItem for a heal/buff-OTHER spell for the "
                     "first time. Seats 0x0F/0x06/0x0A/0x07/0x0D (core/EquipGate.cpp + core/CastSeats.cpp) "
                     "do everything downstream of that, unchanged.",
                     apmf::log::Hex(vt.address(), 16), kClassifySlot);
    }

}
