#include "PCH.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/CastClassify.h"

#include <cstring>
#include <set>
#include <tuple>
#include <unordered_set>

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

    // ---- PLACEMENT SPELLS (feat/apmf-buff-summon-seats, ClickUp 86e3dvkwm). A spell with a
    // SummonCreature (18) or Reanimate (22) effect. Their caster types choose WHERE the spell
    // lands themselves (the summon's own placement; Reanimate's own corpse search, which
    // overrides 0x0A/0x0D on all three builds), so a claim on one names no recipient: target 0 =
    // "the claimant casts it", resolved to the claimant's own handle by ControlMap::ApplyRequest.
    // Seat 0 never forces their self flag (see ClassifyThunk). Any thread: form data reads only.
    bool IsPlacementSpell(const RE::MagicItem* a_spell) {
        if (!a_spell) return false;
        for (const auto* eff : a_spell->effects) {
            if (!eff || !eff->baseEffect) continue;
            const auto arch = eff->baseEffect->GetArchetype();
            if (arch == RE::EffectArchetypes::ArchetypeID::kSummonCreature ||
                arch == RE::EffectArchetypes::ArchetypeID::kReanimate)
                return true;
        }
        return false;
    }

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
        // "RTTI .?AVCombatMagicItemData@@"). The RE pass did not establish an
        // RTTI_CombatMagicItemData Address-Library ID (the pinned 3.7.0
        // Offsets_RTTI.h does carry one, `RTTI_CombatMagicItemData{687623,
        // 395938}` -- CONFIRMED table 2026-09-15, "CastClassify.h SEAT 0" row),
        // so install-time verification here is a STRING match on this name (walked off the
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
        // clause). AE 1.6.1170 disassembly-CERTAIN (J9). SE 1.5.97: CONFIRMED
        // (2026-09-15 pass, "CastClassify.h SEAT 0" slot-1 row) -- the ctor
        // (SE 0x780F5C) stores [this+0x10]=MagicItem, [this+0x18]=CombatController,
        // [this+0x4c]=sete(delivery check) byte-for-byte as on AE, and the slot-1
        // thunk (SE 0x7811F0, id 43931) reads [r13+0x4c] the same way; the
        // vtable resolves via the SE id 265000 (0x1686BF8). Both runtimes open
        // the gate below; any other binary is refused, not guessed.
        constexpr std::uintptr_t kSpellOffset    = 0x10;   // RE::MagicItem* -- the effect's owning spell
        constexpr std::uintptr_t kCtrlOffset     = 0x18;   // RE::CombatController* -- the deliberating actor's controller
        constexpr std::uintptr_t kSelfFlagOffset = 0x4c;   // std::uint8_t -- (spell->GetDelivery()==kSelf) at ctor time

        constexpr std::size_t kClassifySlot = 1;   // CombatMagicItemData vtable slot 1 -- the per-effect visitor

        // ---- UNCLASSED HEAL (marth 2026-09-30: "If we can detect it's a heal and that it's not
        // properly classed, go ahead and proxy it."). The visitor (AE 0x81D830 / SE 0x7811F0) keys
        // each effect into the engine's 23-row table and hands the matched row to KEEP-BEST
        // (AE 45325 0x81DBA0 / SE 43934 0x7815A0: `void(resolver, Effect*, row*, float weight)`,
        // weight 1.0 for the primary AV). A heal whose archetype is not ValueModifier (Mysticism's
        // Restore Health is PeakValueModifier, a34/av24) has NO row, so the engine never mints a
        // Restore item. For a CLAIMED driven form only, the thunk hands that effect to KEEP-BEST
        // with the engine's own Restore-Health row (table AE 382289 / SE 509694, row 10 =
        // {ValueModifier, Health, self, beneficial}). Disassembly (agentlog apmf-unclassed-heal):
        // the Restore creator/ctor (AE 0x824510 -> 0x81F710) and every Restore caster slot read
        // only the effect's PRIMARY AV (+0xC4 -> caster +0x28), never the archetype. The hand still
        // casts the spell exactly as authored.
        constexpr REL::VariantID kKeepBestRow(43934, 45325, 0);
        constexpr REL::VariantID kClassifyTable(509694, 382289, 0);
        constexpr std::size_t    kRowSize          = 48;
        constexpr std::size_t    kRestoreHealthRow = 10;

        using KeepBest_t = void (*)(void*, const RE::Effect*, const void*, float);
        KeepBest_t  g_keepBest   = nullptr;   // set once at Install, before the vtable write
        const void* g_restoreRow = nullptr;

        std::mutex                     g_healLogMx;
        std::unordered_set<RE::FormID> g_healLogged;   // once per spell

        // ---- EXIT LOGS (apmf-restore-serve-exitlogs). Field: Close Greater Wounds never logged "served
        // as Restore" and ServeUnclassedHeal has silent early exits, so the log could not name the
        // rejecting check. Every exit now says so, once per (spell FormID, reason). LOGGING ONLY: no
        // exit, order or value below changed. Runs on the combat thread (ClassifyThunk is the engine's
        // CombatMagicItemData slot-1 visitor); dedup is the same leaf mutex + set pattern as g_healLogged
        // above (nothing is held across any other lock or engine call).
        enum class NoServe : std::uint8_t {
            NoKeepBest = 1, NoSpellOrController, NoEffect, NoBaseEffect, NotHealShaped, AlreadyClassed, NoAttacker,
            NoClaim, ClaimNoTarget
        };

        std::unordered_set<std::uint64_t> g_noServeLogged;   // (spell FormID << 8) | tag, guarded by g_healLogMx
        // Exits 8 and 9 dedup per (spell, actor, reason): an unclaimed NPC reaching NoClaim first must not
        // suppress the follower's own line. Separate set, so its key can never collide with the one above.
        std::set<std::tuple<RE::FormID, RE::FormID, std::uint8_t>> g_noServeActorLogged;   // guarded by g_healLogMx
        constexpr std::uint8_t kUnclaimedTag = 0x80;   // OR'd into the reason: "classified, but no live claim"

        // Loose heal shape for the unclaimed fallback: beneficial, Health, magnitude > 0, ANY archetype.
        // (The archetype test is part of `heal` below; a spell rejected by it is exactly what must stay visible.)
        bool LooseHealShaped(const RE::Effect* a_eff) {
            if (!a_eff || !a_eff->baseEffect) return false;
            const auto* m = a_eff->baseEffect;
            return !m->IsHostile() && !m->IsDetrimental() && m->data.primaryAV == RE::ActorValue::kHealth &&
                   a_eff->effectItem.magnitude > 0.0f;
        }

        // `a_needClaim`: for the exits that fire for EVERY spell the engine classifies, log only when a live
        // claim drives this spell on this actor -- otherwise the log would name every spell in the game.
        // EXCEPTION (`a_looseHeal`): a heal-shaped effect with NO live claim is logged once per spell under
        // a distinct reason tag, so "never classified" and "classified without a claim" can be told apart.
        // When the actor cannot be resolved the claim cannot be checked and the exit is logged.
        // `a_actorKey` != 0 keys the dedup by (spell, actor) instead of per spell.
        template <class Detail>
        void NotServed(RE::MagicItem* a_spell, RE::CombatController* a_cc, NoServe a_why, const char* a_reason,
                       bool a_needClaim, bool a_looseHeal, RE::FormID a_actorKey, Detail&& a_detail) {
            const RE::FormID spellForm = a_spell ? a_spell->GetFormID() : 0;
            bool             unclaimed = false;
            if (a_needClaim && a_spell && a_cc) {
                auto attPtr = a_cc->attackerHandle.get();
                if (auto* actor = attPtr.get()) {
                    apmf::CastSeatClaim seat{};
                    if (!apmf::ControlMap::Get().TryGetCastSeatClaimForForm(actor->GetFormID(), spellForm, seat)) {
                        if (!a_looseHeal) return;
                        unclaimed = true;
                    }
                }
            }
            const auto tag = static_cast<std::uint8_t>(static_cast<std::uint8_t>(a_why) | (unclaimed ? kUnclaimedTag : 0));
            {
                std::scoped_lock lk(g_healLogMx);
                const bool fresh = a_actorKey
                                       ? g_noServeActorLogged.emplace(spellForm, a_actorKey, tag).second
                                       : g_noServeLogged.insert((static_cast<std::uint64_t>(spellForm) << 8) | tag).second;
                if (!fresh) return;
            }
            const char* name = a_spell && a_spell->GetName() ? a_spell->GetName() : "?";
            if (unclaimed)
                spdlog::info("[restore-serve] {} ({:08X}) NOT served: {} (no live claim on this actor at classify time; {})",
                             name, spellForm, a_reason, a_detail());
            else
                spdlog::info("[restore-serve] {} ({:08X}) NOT served: {} ({})", name, spellForm, a_reason, a_detail());
        }

        void ServeUnclassedHeal(void* a_this, void* a_effect, RE::MagicItem* a_spell, RE::CombatController* a_cc,
                                std::uint8_t a_selfFlag) {
            if (!g_keepBest || !g_restoreRow || !a_spell || !a_cc) {
                if (a_spell && a_cc)
                    NotServed(a_spell, a_cc, NoServe::NoKeepBest, "KeepBest/Restore row not enabled at install", true,
                              LooseHealShaped(reinterpret_cast<const RE::Effect*>(a_effect)), 0,
                              [&] { return fmt::format("keepBest={} restoreRow={}", g_keepBest != nullptr, g_restoreRow != nullptr); });
                else
                    NotServed(a_spell, a_cc, NoServe::NoSpellOrController, "spell or combat controller null", false, false, 0,
                              [&] { return fmt::format("spell={} cc={}", a_spell != nullptr, a_cc != nullptr); });
                return;
            }
            const auto* eff = reinterpret_cast<const RE::Effect*>(a_effect);
            if (!eff) {
                NotServed(a_spell, a_cc, NoServe::NoEffect, "effect pointer null", true, false, 0, [&] { return std::string("effect=null"); });
                return;
            }
            if (!eff->baseEffect) {
                NotServed(a_spell, a_cc, NoServe::NoBaseEffect, "effect has no base effect", true, false, 0,
                          [&] { return std::string("baseEffect=null"); });
                return;
            }
            const auto* mgef = eff->baseEffect;
            const auto  arch = mgef->GetArchetype();
            // A heal: same shape as core/RestoreCensus.cpp RestoreShaped(), narrowed to Health.
            const bool heal = !mgef->IsHostile() && !mgef->IsDetrimental() &&
                              mgef->data.primaryAV == RE::ActorValue::kHealth && eff->effectItem.magnitude > 0.0f &&
                              (arch == RE::EffectArchetypes::ArchetypeID::kValueModifier ||
                               arch == RE::EffectArchetypes::ArchetypeID::kPeakValueModifier ||
                               arch == RE::EffectArchetypes::ArchetypeID::kDualValueModifier);
            if (!heal) {
                NotServed(a_spell, a_cc, NoServe::NotHealShaped, "effect is not heal-shaped", true, LooseHealShaped(eff), 0, [&] {
                    return fmt::format("effect {:08X} hostile={} detrimental={} primaryAV={} magnitude={} archetype={}",
                                       mgef->GetFormID(), mgef->IsHostile(), mgef->IsDetrimental(),
                                       static_cast<std::int32_t>(mgef->data.primaryAV), eff->effectItem.magnitude,
                                       static_cast<std::uint32_t>(arch));
                });
                return;
            }
            // Not properly classed: the only Restore-Health key is (ValueModifier, Health, self=1, beneficial).
            if (arch == RE::EffectArchetypes::ArchetypeID::kValueModifier && a_selfFlag == 1) {
                NotServed(a_spell, a_cc, NoServe::AlreadyClassed, "engine already classes it as Restore", true, true, 0, [&] {
                    return fmt::format("effect {:08X} archetype={} selfFlag={} (read after seat-0's own force)",
                                       mgef->GetFormID(), static_cast<std::uint32_t>(arch), a_selfFlag);
                });
                return;
            }
            auto  attPtr = a_cc->attackerHandle.get();
            auto* actor  = attPtr.get();
            if (!actor) {
                NotServed(a_spell, a_cc, NoServe::NoAttacker, "combat controller attacker handle did not resolve", false, false, 0,
                          [&] { return std::string("attackerHandle.get()=null"); });
                return;
            }
            const RE::FormID    fid       = actor->GetFormID();
            const RE::FormID    spellForm = a_spell->GetFormID();
            apmf::CastSeatClaim seat{};
            const bool          haveClaim = apmf::ControlMap::Get().TryGetCastSeatClaimForForm(fid, spellForm, seat);
            if (!haveClaim || !seat.targetHandle) {
                if (!haveClaim) {
                    NotServed(a_spell, a_cc, NoServe::NoClaim, "no live cast claim drives this spell on this actor", false, false, fid,
                              [&] { return fmt::format("actor {:08X} spell {:08X}", fid, spellForm); });
                } else {
                    NotServed(a_spell, a_cc, NoServe::ClaimNoTarget, "claim stands but has no target handle", false, false, fid, [&] {
                        return fmt::format("actor {:08X} claim spell {:08X} proxy {:08X} targetHandle=0", fid, seat.spell,
                                           seat.proxy);
                    });
                }
                return;
            }

            g_keepBest(a_this, eff, g_restoreRow, 1.0f);

            std::scoped_lock lk(g_healLogMx);
            if (g_healLogged.insert(spellForm).second)
                spdlog::info("[ch.8b] heal {:08X} not Restore-classed by the engine -> served as Restore (effect "
                             "{:08X} archetype {} on Health has no Restore row; actor {:08X}, claimed spell {:08X})",
                             spellForm, mgef->GetFormID(), static_cast<std::uint32_t>(arch), fid, seat.spell);
        }

        // ==== ROWLESS ROW SUBSTITUTION (feat/apmf-buff-summon-seats) ====================================
        // A claimed spell NONE of whose effects keys a caster row (Muffle, fortify / resist ValueModifiers,
        // Night Eye, Detect Life, cures, Dispel, Calm / Frenzy / Fear / Courage, restore Stamina, damage
        // Magicka / Stamina) gets no CombatInventoryItem, so no caster, so it can never be cast by the AI.
        // For a CLAIMED driven form only, each of its effects is handed to KEEP-BEST with the engine's OWN
        // Script row of the same hostility: row 22 {Script, self, beneficial} or row 9 {Script, other,
        // hostile} -- the row the engine itself uses for effects it has no special logic for.
        //
        // Why Script (disassembly, all three builds, agentlog apmf-buff-summon-seats): its creator (AE
        // 0x824290 / SE 0x7879B0 / 1.7.104 0x839780) builds CombatInventoryItemMagicT<*, Script>, whose
        // CreateCaster (AE 0x827AE0) allocates a bare 0x20-byte CombatMagicCasterScript (ctor AE 0x837C80:
        // nothing past the CombatMagicCaster base). Its slots read nothing archetype-specific: 0x05 is a
        // range category, 0x06 the shared base start checks (AE 0x81E500 && 0x81E6C0), 0x07 the base
        // `return false`, 0x0A / 0x0D the shared base, 0x0C a blackboard restrict timer of the effect's OWN
        // aiDelayTimer (EffectSetting +0x150). The row scores the effect by its OWN aiScore (+0x14C, row
        // f4 AE 0x823260); KEEP-BEST's best score starts at -1.0 (resolver ctor AE 0x81D5A0), so a 0
        // aiScore is still kept. The hand casts the spell exactly as authored (its own effects, delivery,
        // magnitude): the row only decides which caster object answers the seats.
        // Restore (the heal road's row) is NOT used here: its 0x07 measures the effect's primary AV as a
        // restore percent (a Fortify skill would stop a channel at once) and its 0x0B/0x0C set the 15 s
        // MagicRestoreRestrictionTimer that gates the actor's own native self heals.
        //
        // "Keys a caster row" is the classifier's own lookup (AE 0x81D830): key = archetype<<16 |
        // avByte<<8 | hostile<<1 | self, avByte = the primary AV for the AV-keyed archetypes else 0xFF, then
        // the secondary AV for AV-keyed archetypes. The lookup map (built by AE 0x81DCA0 / SE 0x7816C0 /
        // 1.7.104 0x833190) holds the 23 table rows AND every ValueModifier row again under Absorb (4),
        // DualValueModifier (5), AccumulateMagnitude (32) and PeakValueModifier (34). A row with a null
        // creator (restore Stamina self, damage Magicka / Stamina) builds no item, so it counts as rowless.
        // g_rows is the LIVE table, read and checked against kExpectedRows at install.
        struct TableRow {
            std::int32_t arch;
            std::int32_t av;   // -1 = none
            std::uint8_t self;
            std::uint8_t hostile;
            bool         creator;   // non-null creator = the row builds an item
        };
        constexpr std::size_t kTableRows = 23;
        constexpr std::array<TableRow, kTableRows> kExpectedRows{ {
            { 0, 24, 0, 1, true },  { 0, 25, 0, 1, false }, { 0, 26, 0, 1, false }, { 33, -1, 0, 1, true },
            { 9, -1, 0, 1, true },  { 10, -1, 0, 1, true }, { 42, 1, 0, 1, true },  { 24, 1, 0, 1, true },
            { 21, 53, 0, 1, true }, { 1, -1, 0, 1, true },  { 0, 24, 1, 0, true },  { 0, 25, 1, 0, true },
            { 0, 26, 1, 0, false }, { 0, 63, 1, 0, true },  { 18, -1, 1, 0, true }, { 18, -1, 0, 0, true },
            { 35, -1, 1, 0, true }, { 12, -1, 1, 0, true }, { 11, 54, 1, 0, true }, { 17, -1, 1, 0, true },
            { 0, 39, 1, 0, true },  { 22, -1, 0, 0, true }, { 1, -1, 1, 0, true },
        } };
        constexpr std::size_t kScriptHostileRow    = 9;    // {Script, other, hostile}
        constexpr std::size_t kScriptBeneficialRow = 22;   // {Script, self, beneficial}
        // Archetypes whose info flag bit 1 is set (AE 0x1FD3028 / SE 0x1DB0028 / 1.7.104 0x2076028,
        // identical): the AV is part of the key.
        constexpr std::array<std::int32_t, 17> kAvKeyedArchetypes{ { 0, 4, 5, 6, 7, 8, 11, 14, 21, 24, 31, 32, 34,
                                                                     38, 39, 42, 45 } };
        // The map builder's ValueModifier aliases.
        constexpr std::array<std::int32_t, 4> kValueModAliases{ { 4, 5, 32, 34 } };

        bool        g_rowlessOk       = false;     // set once at Install, before the vtable write
        KeepBest_t  g_keepBestRowless = nullptr;   // KeepBestRow, verified separately from the heal road's
        const void* g_scriptRow[2]{};      // [0] beneficial (row 22), [1] hostile (row 9)

        std::set<std::tuple<RE::FormID, RE::FormID, std::uint8_t>> g_rowlessLogged;   // guarded by g_healLogMx

        bool AvKeyed(std::int32_t a_arch) {
            return std::find(kAvKeyedArchetypes.begin(), kAvKeyedArchetypes.end(), a_arch) != kAvKeyedArchetypes.end();
        }

        bool KeyHitsCasterRow(std::int32_t a_arch, std::uint8_t a_avByte, std::uint8_t a_self, std::uint8_t a_hostile) {
            const bool alias =
                std::find(kValueModAliases.begin(), kValueModAliases.end(), a_arch) != kValueModAliases.end();
            for (const auto& r : kExpectedRows) {   // equal to the live table: Install refuses otherwise
                if (!r.creator || r.self != a_self || r.hostile != a_hostile) continue;
                if (static_cast<std::uint8_t>(r.av & 0xFF) != a_avByte) continue;
                if (r.arch == a_arch || (alias && r.arch == 0)) return true;
            }
            return false;
        }

        // Does this effect, under the resolver's self flag, key a row that builds an item?
        bool EffectKeysCasterRow(const RE::Effect* a_eff, std::uint8_t a_self) {
            const auto* m    = a_eff->baseEffect;
            const auto  arch = static_cast<std::int32_t>(m->GetArchetype());
            const auto  host = static_cast<std::uint8_t>(m->IsHostile() ? 1 : 0);
            if (!AvKeyed(arch)) return KeyHitsCasterRow(arch, 0xFF, a_self, host);
            if (KeyHitsCasterRow(arch, static_cast<std::uint8_t>(static_cast<std::int32_t>(m->data.primaryAV) & 0xFF),
                                 a_self, host))
                return true;
            const auto sav = static_cast<std::int32_t>(m->data.secondaryAV);
            return sav != -1 && KeyHitsCasterRow(arch, static_cast<std::uint8_t>(sav & 0xFF), a_self, host);
        }

        enum class RowlessWhy : std::uint8_t { Served = 1, OtherEffectKeyed, HealShaped };

        // The whole spell: no effect keys a caster row, and none is heal-shaped (the heal road above owns
        // those, with the Restore row). `a_keyed` names the first effect that does key one.
        RowlessWhy SpellRowless(const RE::MagicItem* a_spell, std::uint8_t a_self, RE::FormID& a_keyed) {
            for (const auto* e : a_spell->effects) {
                if (!e || !e->baseEffect) continue;
                if (LooseHealShaped(e)) { a_keyed = e->baseEffect->GetFormID(); return RowlessWhy::HealShaped; }
                if (EffectKeysCasterRow(e, a_self)) { a_keyed = e->baseEffect->GetFormID(); return RowlessWhy::OtherEffectKeyed; }
            }
            return RowlessWhy::Served;
        }

        // COMBAT thread, after the chained visitor. Cheap path first: an effect that keys a caster row
        // (almost every effect the engine classifies) returns before any claim read.
        void ServeRowless(void* a_this, void* a_effect, RE::MagicItem* a_spell, RE::CombatController* a_cc,
                          std::uint8_t a_selfFlag) {
            if (!g_rowlessOk || !g_keepBestRowless || !a_spell || !a_cc || !a_effect) return;
            const auto* eff = reinterpret_cast<const RE::Effect*>(a_effect);
            if (!eff->baseEffect || EffectKeysCasterRow(eff, a_selfFlag)) return;

            auto  attPtr = a_cc->attackerHandle.get();
            auto* actor  = attPtr.get();
            if (!actor) return;
            const RE::FormID    fid       = actor->GetFormID();
            const RE::FormID    spellForm = a_spell->GetFormID();
            apmf::CastSeatClaim seat{};
            if (!apmf::ControlMap::Get().TryGetCastSeatClaimForForm(fid, spellForm, seat) || !seat.targetHandle) return;

            RE::FormID keyed = 0;
            const auto why   = SpellRowless(a_spell, a_selfFlag, keyed);
            const bool hostile = eff->baseEffect->IsHostile();
            if (why == RowlessWhy::Served) g_keepBestRowless(a_this, eff, g_scriptRow[hostile ? 1 : 0], 1.0f);

            {
                std::scoped_lock lk(g_healLogMx);
                if (!g_rowlessLogged.emplace(spellForm, fid, static_cast<std::uint8_t>(why)).second) return;
            }
            const char* name = a_spell->GetName() ? a_spell->GetName() : "?";
            if (why == RowlessWhy::Served)
                spdlog::info("[ch.8b seat 0] {} ({:08X}) has NO caster row (effect {:08X} archetype {} primaryAV {} "
                             "hostile={} selfFlag={}) -> served as Script (row {}); actor {:08X}, claimed spell {:08X}. "
                             "The hand casts the spell as authored; the Script caster answers the seats.",
                             name, spellForm, eff->baseEffect->GetFormID(),
                             static_cast<std::uint32_t>(eff->baseEffect->GetArchetype()),
                             static_cast<std::int32_t>(eff->baseEffect->data.primaryAV), hostile, a_selfFlag,
                             hostile ? kScriptHostileRow : kScriptBeneficialRow, fid, seat.spell);
            else
                spdlog::info("[ch.8b seat 0] {} ({:08X}) effect {:08X} has no caster row but is NOT substituted: {} "
                             "(effect {:08X}); actor {:08X}.",
                             name, spellForm, eff->baseEffect->GetFormID(),
                             why == RowlessWhy::HealShaped ? "the spell is heal-shaped (the heal road owns it)"
                                                           : "another effect of the spell keys its own caster row",
                             keyed, fid);
        }

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
                        // feat/per-hand-cast-claims: matched by DRIVEN FORM (this
                        // resolver's own spell), not "the actor-wide best-basis
                        // claim" -- up to two kIntent_Cast claims can be live at
                        // once now (one per hand). Picking the highest-basis claim
                        // overall here would classify against the WRONG hand's
                        // claim whenever the other hand happens to have the higher
                        // basis. See ControlMap::TryGetCastSeatClaimForForm.
                        apmf::CastSeatClaim seat{};
                        if (apmf::ControlMap::Get().TryGetCastSeatClaimForForm(fid, spellForm, seat) && seat.targetHandle) {
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
                            // PLACEMENT SPELLS ARE NEVER FORCED (86e3dvkwm, spell-level). Reanimate has ONE
                            // row, {Reanimate, OTHER, beneficial} (row 21): forcing self=1 would key no row
                            // at all and the claimed spell would get no item. Summon keys the same creator
                            // from both rows 14 and 15, so leaving it native changes nothing and keeps the
                            // engine's own key exactly. Checked on the whole spell, so no later effect of a
                            // placement spell can flip the flag either.
                            const bool placement = IsPlacementSpell(spellPtr);
                            if (placement) {
                                if (LogDue(fid, spellForm))
                                    spdlog::info(
                                        "[ch.8b seat 0] 0x{} CLASSIFY spell=0x{} '{}' selfFlag {} -- matches the live "
                                        "cast claim's driven form, a SUMMON / REANIMATE spell: NOT forced (it keys "
                                        "its own Summon / Reanimate row natively).",
                                        apmf::log::Hex(fid), apmf::log::Hex(spellForm),
                                        spellPtr->GetName() ? spellPtr->GetName() : "?", before);
                            } else if (!hostile) {
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
                        } else if (LogDue(fid, spellForm)) {
                            // Diagnostic only (never decisional): TryGetCastSeatClaimForForm
                            // just said no LIVE claim on this actor drives THIS spell. Peek
                            // the actor-wide floor (any hand) purely to keep the field log's
                            // existing distinction -- "the claim never even reached this
                            // seat" vs "a claim stands, but for a different spell" -- feat/
                            // per-hand-cast-claims made the FORCE decision above hand-exact,
                            // but this fallback log deliberately stays "any hand" (it is
                            // informational, not a bug: with two hands potentially claimed,
                            // reporting either one's driven form here is equally useful).
                            apmf::CastSeatClaim anySeat{};
                            if (apmf::ControlMap::Get().TryGetCastSeatClaim(fid, anySeat) && anySeat.targetHandle) {
                                const RE::FormID otherDriven = anySeat.proxy ? anySeat.proxy : anySeat.spell;
                                if (otherDriven != 0) {
                                    spdlog::info(
                                        "[ch.8b seat 0] 0x{} CLASSIFY spell=0x{} '{}' selfFlag {} -- a live cast "
                                        "claim stands (driven=0x{}) but names a DIFFERENT spell; not forced.",
                                        apmf::log::Hex(fid), apmf::log::Hex(spellForm),
                                        spellPtr->GetName() ? spellPtr->GetName() : "?", before,
                                        apmf::log::Hex(otherDriven));
                                }
                            }
                        }
                    }
                }
            }

            const auto r = orig(a_this, a_effect);   // THE ENGINE'S OWN CLASSIFICATION LOGIC, SEEING WHATEVER WE SET ABOVE
            ServeUnclassedHeal(a_this, a_effect, spellPtr, ccPtr, *selfFlag);
            ServeRowless(a_this, a_effect, spellPtr, ccPtr, *selfFlag);
            return r;
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
        // Exact-binary gate (CLAUDE.md rule 11). NOT REL::Module::IsAE()/IsSE():
        // in the pinned 3.7.0 Relocation.h:899-912 IsAE() is any 1.6.x and IsSE()
        // is the `default:` arm (1.7.104 lands there -- though it never reaches
        // this gate: with no address library CommonLib terminates at SKSE::Init
        // with the address-library dialog, src/SKSE/API.cpp:78-79). The gate is
        // for the binaries that DO load: the three offsets are disassembly-
        // confirmed on exactly 1.6.1170 and 1.5.97; any other 1.6.x/1.5.x is
        // refused by name, nothing is guessed. core/AiCastSeats.cpp's Group C
        // gate and plugin.cpp's `[runtime]` startup line apply this same
        // two-version predicate.
        // F2b (2026-10-05): 1.7.104 opens too -- the ctor (AE 0x81D5A0 / 1.7.104 0x832A90),
        // the slot-1 visitor (0x81D830 / 0x832D20) and KeepBestRow (45325) are instruction-
        // for-instruction identical with every displacement, and the classify table (382289)
        // holds the same 23 rows (Docs/VERIFIED-ADDRESSES.md "1.7.104 proof").
        const auto ver       = REL::Module::get().version();
        const bool onAE1170  = ver == REL::Version{ 1, 6, 1170, 0 };
        const bool onSE597   = ver == REL::Version{ 1, 5, 97, 0 };
        const bool on17104   = allowance::IsRuntime1_7_104();
        if (!onAE1170 && !onSE597 && !on17104) {
            spdlog::warn("[ch.8b seat 0] runtime {} -- the +0x10/+0x18/+0x4c CombatMagicItemData offsets "
                         "this seat reads are disassembly-CONFIRMED on 1.6.1170, 1.5.97 and 1.7.104 only. "
                         "Refusing to install rather than guess a struct layout carries across runtimes "
                         "unchanged (CLAUDE.md rule 11). Heal-OTHER stays absent on this runtime; the "
                         "direct-force degrade path is unaffected.",
                         ver.string("."));
            return;
        }
        if (g_installed.exchange(true)) return;

        if (!ReadIniFlag("EnableSeat0Classify", 1)) {
            spdlog::info("[ch.8b seat 0] disabled by [CastSeats] EnableSeat0Classify=0 -- a heal-OTHER "
                         "spell will not get a combat-AI item; the direct-force degrade path is unaffected.");
            return;
        }

        REL::Relocation<std::uintptr_t> vt{ kCombatMagicItemDataVtable };
        // Belt-and-braces, UNREACHABLE by construction (Fable tier-3 on c70767c,
        // SEV-4): in the pinned 3.7.0 a VariantID does NOT resolve to null for a
        // missing id -- `VariantID::address()` (Relocation.h:1535) is zero only
        // when `id()` is zero, and `IDDatabase::id2offset` (:1069-1095)
        // `report_and_fail`s past the end and otherwise `lower_bound`s with NO
        // equality check off VR (a missing id silently yields the next id's
        // offset). A missing library FILE terminates the process in SKSE::Init
        // (src/SKSE/API.cpp:78-79) before this runs. The REAL guard against a
        // wrong-id resolve is the RTTI mangled-name compare just below. This zero
        // test only keeps ResolveMangledName from walking near null if CommonLib's
        // runtime enum ever returned something outside its three arms.
        if (vt.address() == 0) {
            spdlog::error("[ch.8b seat 0] CombatMagicItemData VariantID resolved to ZERO on {} -- REFUSED (not "
                          "installed; never a blind vtable read). Not the missing-id case (3.7.0 "
                          "report_and_fails or mis-resolves that one, never nulls it): REL::Module::"
                          "GetRuntime() returned no AE/SE/VR arm -- investigate CommonLib, not the library.",
                          ver.string("."));
            return;
        }
        if (!allowance::SeatVerified(vt.address(), "CastClassify.CombatMagicItemData")) {
            spdlog::error("[ch.8b seat 0] CLASSIFY NOT installed (self-check refused the CombatMagicItemData vtable).");
            return;
        }
        const char* name = ResolveMangledName(vt.address());
        if (!name || std::strcmp(name, kExpectedMangledName) != 0) {
            spdlog::error("[ch.8b seat 0] CombatMagicItemData vtable 0x{} did NOT resolve to the expected "
                          "RTTI name '{}' (got '{}') -- REFUSED (not installed; never a blind vtable write).",
                          apmf::log::Hex(vt.address(), 16), kExpectedMangledName, name ? name : "<unresolved>");
            return;
        }

        // Unclassed-heal serving (see ServeUnclassedHeal). Optional: if either piece fails its
        // check, seat 0 still installs and only this part is refused, loudly.
        {
            REL::Relocation<std::uintptr_t> kb{ kKeepBestRow };
            REL::Relocation<std::uintptr_t> tbl{ kClassifyTable };
            const auto row  = tbl.address() + kRestoreHealthRow * kRowSize;
            const auto next = row + kRowSize;   // row 11: ValueModifier / Magicka, the same Restore creator
            const bool rowOk =
                tbl.address() != 0 && *reinterpret_cast<const std::uint32_t*>(row) == 0 &&
                *reinterpret_cast<const std::int32_t*>(row + 4) == static_cast<std::int32_t>(RE::ActorValue::kHealth) &&
                *reinterpret_cast<const std::uint8_t*>(row + 8) == 1 && *reinterpret_cast<const std::uint8_t*>(row + 9) == 0 &&
                *reinterpret_cast<const float*>(row + 0xC) == 1.0f && *reinterpret_cast<const std::uintptr_t*>(row + 0x18) != 0 &&
                *reinterpret_cast<const std::int32_t*>(next + 4) == static_cast<std::int32_t>(RE::ActorValue::kMagicka) &&
                *reinterpret_cast<const std::uintptr_t*>(next + 0x18) == *reinterpret_cast<const std::uintptr_t*>(row + 0x18);
            if (!allowance::SeatVerified(kb.address(), "CastClassify.KeepBestRow")) {
                spdlog::error("[ch.8b seat 0] unclassed-heal serving NOT enabled (self-check refused KeepBestRow).");
            } else if (!rowOk) {
                spdlog::error("[ch.8b seat 0] unclassed-heal serving NOT enabled: classify table row {} at 0x{} is not "
                              "{{ValueModifier, Health, self, beneficial, weight 1, Restore creator}}.",
                              kRestoreHealthRow, apmf::log::Hex(row, 16));
            } else {
                g_keepBest   = reinterpret_cast<KeepBest_t>(kb.address());
                g_restoreRow = reinterpret_cast<const void*>(row);
                spdlog::info("[ch.8b seat 0] unclassed-heal serving enabled (KeepBestRow 0x{}, Restore-Health row 0x{}).",
                             apmf::log::Hex(kb.address(), 16), apmf::log::Hex(row, 16));
            }
        }

        // Rowless row substitution (see ServeRowless). Optional like the heal road: refused alone, loudly,
        // if KeepBestRow fails the self-check or the LIVE table differs from kExpectedRows in any row
        // (archetype, AV, self, hostile, creator null or not) -- the substitution decision is made from
        // kExpectedRows, so it is only sound while the engine's table is exactly that. Both Script rows
        // must carry the same non-null creator (the one Script item / caster family).
        {
            REL::Relocation<std::uintptr_t> kb{ kKeepBestRow };
            REL::Relocation<std::uintptr_t> tbl{ kClassifyTable };
            std::string                     bad;
            if (tbl.address() == 0) bad = "classify table resolved to 0";
            for (std::size_t i = 0; bad.empty() && i < kTableRows; ++i) {
                const auto  r  = tbl.address() + i * kRowSize;
                const auto& ex = kExpectedRows[i];
                const auto  arch = *reinterpret_cast<const std::int32_t*>(r);
                const auto  av   = *reinterpret_cast<const std::int32_t*>(r + 4);
                const auto  self = *reinterpret_cast<const std::uint8_t*>(r + 8);
                const auto  host = *reinterpret_cast<const std::uint8_t*>(r + 9);
                const bool  cr   = *reinterpret_cast<const std::uintptr_t*>(r + 0x18) != 0;
                if (arch != ex.arch || av != ex.av || self != ex.self || host != ex.hostile || cr != ex.creator)
                    bad = fmt::format("row {} is {{arch {}, av {}, self {}, hostile {}, creator {}}}, expected "
                                      "{{arch {}, av {}, self {}, hostile {}, creator {}}}",
                                      i, arch, av, self, host, cr, ex.arch, ex.av, ex.self, ex.hostile, ex.creator);
            }
            const auto rowB = tbl.address() + kScriptBeneficialRow * kRowSize;
            const auto rowH = tbl.address() + kScriptHostileRow * kRowSize;
            if (bad.empty() &&
                *reinterpret_cast<const std::uintptr_t*>(rowB + 0x18) != *reinterpret_cast<const std::uintptr_t*>(rowH + 0x18))
                bad = "the two Script rows (9, 22) do not share one creator";
            if (!allowance::SeatVerified(kb.address(), "CastClassify.KeepBestRow.Rowless")) {
                spdlog::error("[ch.8b seat 0] rowless row substitution NOT enabled (self-check refused KeepBestRow).");
            } else if (!bad.empty()) {
                spdlog::error("[ch.8b seat 0] rowless row substitution NOT enabled: classify table at 0x{}: {}. A claimed "
                              "spell with no caster row stays without an item (never a guessed row).",
                              apmf::log::Hex(tbl.address(), 16), bad);
            } else {
                g_keepBestRowless = reinterpret_cast<KeepBest_t>(kb.address());
                g_scriptRow[0]    = reinterpret_cast<const void*>(rowB);
                g_scriptRow[1]    = reinterpret_cast<const void*>(rowH);
                g_rowlessOk       = true;
                spdlog::info("[ch.8b seat 0] rowless row substitution enabled (KeepBestRow 0x{}, Script rows 0x{} "
                             "beneficial / 0x{} hostile; all 23 table rows match).",
                             apmf::log::Hex(kb.address(), 16), apmf::log::Hex(rowB, 16), apmf::log::Hex(rowH, 16));
            }
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
