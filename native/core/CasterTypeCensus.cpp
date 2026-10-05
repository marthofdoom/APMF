#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/Allowance.h"
#include "core/ControlMap.h"
#include "core/CasterTypeCensus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Win32 INI read for the one probe flag. Declared by hand, exactly like
// core/AiCastSeats.cpp / core/CastSeats.cpp / core/CastClassify.cpp do -- PCH does
// not pull in <Windows.h>, and this is the single Win32 call this file needs.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See core/CasterTypeCensus.h for the design and the question it answers.
//
// ── ABI EVIDENCE (disassembled callees, 1.6.1170 AND 1.5.97, not CommonLib) ───
//   0x06 CheckStartCast: bool(CombatMagicCaster*, CombatController*), the shape
//        core/CastSeats.cpp's banner already proved on the Restore impl (AE
//        0x81f980); every one of the 15 types overrides it (AE 0x81e7b0 Offensive
//        .. 0x822490 Reanimate; SE 0x7824c0 .. 0x7860f0). Scalar return.
//   0x0B NotifyStartCast: void(CombatMagicCaster*, CombatController*). Base impl
//        AE 0x81e090 / SE 0x781ce0 is `ret 0` (void). Restore's override AE
//        0x81fc60 / SE 0x7838f0 keeps rcx = this, rdx = the controller (`mov rdi,
//        rdx` then forwards it). The callers (AE 49086 0x89f2b0, 49101 0x89feb0,
//        49159 0x8a2369, 49083 0x89efb8; SE 48039 0x808a30) load rdx =
//        CombatController* the same way they do for 0x06. No return value read.
//   Both slots are hooked on the SAME 15 vtables, each a VerifiedAddresses row
//   (tools/verified_addresses/spec.json; Armor's row added by this change) and each
//   RTTI-derivation-checked against RTTI_CombatMagicCaster at install.
//
// ── ARMOR (ENGINE_NOTES §0.28 does not hold for these binaries) ─────────────────
//   VTABLE_CombatMagicCasterArmor (AE id 211222 -> 0x18CD4D0, SE id 265023 ->
//   0x1687268) walks to `.?AVCombatMagicCasterArmor@@` whose hierarchy is
//   {Armor, CombatMagicCaster, CombatObject, NiRefObject} on BOTH runtimes, and the
//   classify table's (ValueModifier, DamageResist, self) row builds it. The 2026-07-22
//   note ("a vtable symbol with no class") came from a different build and a hand
//   list; here the DerivesFrom walk at install is the guard, exactly as for the others.
//
// ── THE CLASSIFY TABLE (decoded from the binaries, identical on both) ─────────────
//   AE 0x20163B0 / SE 0x1DF2B00, 23 rows of 48 bytes {u32 archetype, i32 av, u8 self,
//   u8 hostile, f32 weight, ..., creator}. The classifier (CombatMagicItemData slot 1,
//   AE 0x81D830 / SE 0x7811F0) keys each effect as archetype<<16 | avByte<<8 |
//   hostile<<1 | self, where avByte is the effect's primary AV only for the
//   archetypes whose info flag bit 1 is set (AE table 0x1FD3028 / SE 0x1DB0028,
//   the set kAvArchetypes below; 0xFF otherwise), plus a second lookup on the
//   secondary AV for those archetypes. The spell's item (and so its caster type) is
//   the row of its HIGHEST-SCORING effect (AE 0x81DBA0 keeps the best row with a
//   non-null creator). kRows is that table verbatim; it is used ONLY to label a
//   window with its PREDICTED row, never to decide anything.
// ============================================================================

namespace apmf::castertypecensus {

    namespace {

        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the SE/AE layout split");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point (0x68)");
        static_assert(offsetof(RE::CombatMagicCaster, magicItem) == 0x18,
                      "CombatMagicCaster::magicItem moved -- re-verify against the fork header");

        constexpr std::size_t kCheckStartCast  = 0x06;
        constexpr std::size_t kNotifyStartCast = 0x0B;

        // ---- the 15 caster types, one index each (the order of every per-type array) ----
        constexpr std::size_t kTypes = 15;
        constexpr std::array<const char*, kTypes> kTypeNames{ {
            "Offensive", "Restore", "Ward", "Summon", "Stagger", "Disarm", "Cloak", "Light",
            "Invisibility", "BoundItem", "Armor", "TargetEffect", "Paralyze", "Script", "Reanimate",
        } };
        enum : int {
            kOffensive = 0, kRestore, kWard, kSummon, kStagger, kDisarm, kCloak, kLight,
            kInvisibility, kBoundItem, kArmor, kTargetEffect, kParalyze, kScript, kReanimate,
            kNoCaster = -1,   // a recognised row whose creator is null: the engine mints NO item
        };

        const std::array<REL::VariantID, kTypes>& CasterVtables() {
            static const std::array<REL::VariantID, kTypes> s{ {
                RE::VTABLE_CombatMagicCasterOffensive[0],    RE::VTABLE_CombatMagicCasterRestore[0],
                RE::VTABLE_CombatMagicCasterWard[0],         RE::VTABLE_CombatMagicCasterSummon[0],
                RE::VTABLE_CombatMagicCasterStagger[0],      RE::VTABLE_CombatMagicCasterDisarm[0],
                RE::VTABLE_CombatMagicCasterCloak[0],        RE::VTABLE_CombatMagicCasterLight[0],
                RE::VTABLE_CombatMagicCasterInvisibility[0], RE::VTABLE_CombatMagicCasterBoundItem[0],
                RE::VTABLE_CombatMagicCasterArmor[0],        RE::VTABLE_CombatMagicCasterTargetEffect[0],
                RE::VTABLE_CombatMagicCasterParalyze[0],     RE::VTABLE_CombatMagicCasterScript[0],
                RE::VTABLE_CombatMagicCasterReanimate[0],
            } };
            return s;
        }

        // Resolved vtable address per type, set once at install for every type whose
        // BOTH seats installed (0 = not observed). Read lock-free on the combat thread.
        std::array<std::atomic<std::uintptr_t>, kTypes> g_vt{};

        int TypeOf(std::uintptr_t a_vt) {
            for (std::size_t i = 0; i < kTypes; ++i)
                if (g_vt[i].load(std::memory_order_relaxed) == a_vt) return static_cast<int>(i);
            return -2;   // not one of ours (structurally unreachable, see the thunks)
        }

        // ---- the decoded classify table (see the banner) ----
        struct Row {
            std::int32_t arch;
            std::int32_t av;       // -1 = none (key byte 0xFF)
            std::uint8_t self;
            std::uint8_t hostile;
            int          type;     // kNoCaster for a null creator
        };
        constexpr std::array<Row, 23> kRows{ {
            { 0, 24, 0, 1, kOffensive },      { 0, 25, 0, 1, kNoCaster },      { 0, 26, 0, 1, kNoCaster },
            { 33, -1, 0, 1, kStagger },       { 9, -1, 0, 1, kDisarm },        { 10, -1, 0, 1, kTargetEffect },
            { 42, 1, 0, 1, kTargetEffect },   { 24, 1, 0, 1, kTargetEffect },  { 21, 53, 0, 1, kParalyze },
            { 1, -1, 0, 1, kScript },         { 0, 24, 1, 0, kRestore },       { 0, 25, 1, 0, kRestore },
            { 0, 26, 1, 0, kNoCaster },       { 0, 63, 1, 0, kWard },          { 18, -1, 1, 0, kSummon },
            { 18, -1, 0, 0, kSummon },        { 35, -1, 1, 0, kCloak },        { 12, -1, 1, 0, kLight },
            { 11, 54, 1, 0, kInvisibility },  { 17, -1, 1, 0, kBoundItem },    { 0, 39, 1, 0, kArmor },
            { 22, -1, 0, 0, kReanimate },     { 1, -1, 1, 0, kScript },
        } };
        // Archetypes whose info flag bit 1 is set (the AV is part of the key).
        constexpr std::array<std::int32_t, 17> kAvArchetypes{ { 0, 4, 5, 6, 7, 8, 11, 14, 21, 24, 31, 32, 34, 38,
                                                                39, 42, 45 } };

        bool ArchUsesAv(std::int32_t a_arch) {
            return std::find(kAvArchetypes.begin(), kAvArchetypes.end(), a_arch) != kAvArchetypes.end();
        }

        // -2 = no row at all; kNoCaster = a row with a null creator; else a type index.
        int LookupRow(std::int32_t a_arch, std::int32_t a_av, bool a_self, bool a_hostile) {
            const std::uint8_t keyAv = ArchUsesAv(a_arch) ? static_cast<std::uint8_t>(a_av & 0xFF) : 0xFF;
            for (const auto& r : kRows) {
                if (r.arch == a_arch && static_cast<std::uint8_t>(r.av & 0xFF) == keyAv &&
                    r.self == (a_self ? 1 : 0) && r.hostile == (a_hostile ? 1 : 0))
                    return r.type;
            }
            return -2;
        }

        const char* RowName(int a_row) {
            if (a_row == -2) return "no-row";
            if (a_row == kNoCaster) return "row-without-caster";
            return kTypeNames[static_cast<std::size_t>(a_row)];
        }

        // Label: the rows each effect keys into, natively and with seat 0's self flip
        // (emulating core/CastClassify.cpp: the flag is flipped by the first
        // NON-HOSTILE effect while a driving claim with a target stands, and then
        // stays set for the effects after it). GAME THREAD (form reads only).
        std::string PredictRows(RE::MagicItem* a_spell, bool a_seat0Applies) {
            if (!a_spell) return "spell?";
            const bool nativeSelf = a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            bool       flag       = nativeSelf;
            std::string nat, s0;
            int         i = 0;
            for (auto* eff : a_spell->effects) {
                if (!eff || !eff->baseEffect) continue;
                auto*       mgef = eff->baseEffect;
                const auto  arch = static_cast<std::int32_t>(mgef->GetArchetype());
                const auto  pav  = static_cast<std::int32_t>(mgef->data.primaryAV);
                const auto  sav  = static_cast<std::int32_t>(mgef->data.secondaryAV);
                const bool  host = mgef->IsHostile();
                if (a_seat0Applies && !flag && !host) flag = true;
                auto one = [&](bool a_self) {
                    std::string s = fmt::format("e{}:a{}/av{}{}={}", i, arch, pav, host ? "/H" : "",
                                                RowName(LookupRow(arch, pav, a_self, host)));
                    if (ArchUsesAv(arch) && sav != -1)
                        s += fmt::format("+av{}={}", sav, RowName(LookupRow(arch, sav, a_self, host)));
                    return s;
                };
                if (!nat.empty()) { nat += ' '; s0 += ' '; }
                nat += one(nativeSelf);
                s0 += one(flag);
                ++i;
                if (i >= 4) break;   // a line, not a dump
            }
            if (nat.empty()) return "no-effects";
            return fmt::format("native[{}] seat0[{}]", nat, s0);
        }

        // ---- config + global counters ----
        std::atomic<bool>          g_armed{ false };
        std::atomic<int>           g_openWindows{ 0 };
        std::atomic<std::uint32_t> g_nStart{ 0 }, g_nNotify{ 0 };
        std::array<std::atomic<std::uint64_t>, kTypes> g_callsAll{};   // 0x06 calls, every actor
        std::array<std::atomic<std::uint64_t>, kTypes> g_firesAll{};   // 0x0B fires, every actor
        std::atomic<std::uint64_t> g_foreign{ 0 };                     // thunk entered on an unrecorded vtable

        // ---- one global line budget (the heartbeat bypasses it and reports the drops) ----
        constexpr std::uint64_t kBudgetWindowMs = 10000;
        constexpr int           kBudgetLines    = 60;
        std::atomic<std::uint64_t> g_budgetStart{ 0 };
        std::atomic<int>           g_budgetUsed{ 0 };
        std::atomic<std::uint64_t> g_dropped{ 0 };

        bool LineDue() {
            const auto now   = apmf::clock::MonotonicMs();
            auto       start = g_budgetStart.load(std::memory_order_relaxed);
            if (now - start >= kBudgetWindowMs &&
                g_budgetStart.compare_exchange_strong(start, now, std::memory_order_relaxed))
                g_budgetUsed.store(0, std::memory_order_relaxed);
            if (g_budgetUsed.fetch_add(1, std::memory_order_relaxed) < kBudgetLines) return true;
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // ---- windows ----
        struct Window {
            RE::FormID    actor  = 0;
            char          hand   = 'R';   // L / R / D (dual)
            RE::FormID    spell  = 0;
            RE::FormID    proxy  = 0;
            RE::FormID    driven = 0;
            RE::FormID    target = 0;
            std::uint32_t flags  = 0;
            bool          self   = false;
            std::uint64_t openMs = 0;
            std::uint32_t series = 0;   // series id (per actor), 1-based index below
            std::uint32_t seriesIdx = 0;
            // game thread
            bool          ctrlAtOpen   = false;
            std::uint64_t ctrlSeenMs   = 0;
            std::uint32_t maxHandState = 0;
            std::uint64_t firstChargeMs = 0;   // claimed hand holds driven, state >= 2
            std::uint64_t firstInstantMs = 0;  // kInstant caster holds spell/proxy, state != 0
            std::uint32_t milestones = 0;
            std::string   predicted;
            // combat thread (under g_mx)
            std::array<std::uint32_t, kTypes> builtDriven{}, builtOther{}, firedDriven{}, firedOther{};
            std::uint32_t yesDriven = 0, noDriven = 0;
            std::uint64_t firstBuiltMs = 0, firstYesMs = 0, firstFireMs = 0;
            int           firstFireType = -1, firstBuiltType = -1;
            std::vector<const void*> casters;   // distinct instances seen (bounded)
            // anim (under g_mx)
            std::uint32_t animBegin = 0, animFire = 0;
            std::uint64_t firstAnimFireMs = 0;
        };

        struct SeriesState {
            std::uint32_t id = 0;
            std::uint32_t count = 0;
            std::uint32_t fired = 0;
            std::uint64_t lastCloseMs = 0;
            std::uint64_t lastFireMs = 0;
            std::string   entries;   // compact per-window results
        };

        std::mutex                                           g_mx;      // leaf lock: never held across an engine call or a log write
        std::unordered_map<RE::FormID, std::vector<Window>>  g_windows;
        std::unordered_map<RE::FormID, SeriesState>          g_series;  // game thread only

        // window totals for the heartbeat (game thread)
        std::uint64_t g_opened = 0, g_closed = 0;
        std::array<std::uint64_t, kTypes> g_hbBuiltDriven{}, g_hbFiredDriven{}, g_hbBuiltOther{};
        std::uint64_t g_hbZero = 0;
        std::uint64_t g_lastPollMs = 0, g_lastHeartbeatMs = 0;

        std::uint64_t Rel(std::uint64_t a_open, std::uint64_t a_ms) { return a_ms ? a_ms - a_open : 0; }

        // ---- the combat-thread observation (both seats) ----
        struct Pending {
            bool        log = false;
            std::string line;
        };

        void Observe(int a_type, RE::CombatMagicCaster* a_this, RE::CombatController* a_cc, bool a_fire, bool a_answer) {
            if (!a_this || !a_cc) return;
            auto* item = a_this->magicItem;
            if (!item) return;
            auto  attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>, refcounted for this scope
            auto* actor  = attPtr.get();
            if (!actor) return;
            const RE::FormID fid      = actor->GetFormID();
            const RE::FormID itemForm = item->GetFormID();
            const auto       now      = apmf::clock::MonotonicMs();
            const auto       t        = static_cast<std::size_t>(a_type);

            Pending p;
            {
                std::scoped_lock lk(g_mx);
                auto it = g_windows.find(fid);
                if (it == g_windows.end() || it->second.empty()) return;
                // Attribute to the window whose driven form (or spell) this item is; an item
                // no window names is "the engine casting something else" and is counted on
                // every window of this actor.
                // Only the DRIVEN form counts as the claim's own cast (the claim seats match
                // the same way): with a delivery-flip proxy, the original self spell showing
                // up is the engine healing ITSELF, which is exactly an "other" cast.
                Window* hit = nullptr;
                for (auto& w : it->second)
                    if (itemForm == w.driven) { hit = &w; break; }
                if (hit) {
                    Window& w = *hit;
                    if (a_fire) {
                        ++w.firedDriven[t];
                        if (!w.firstFireMs) {
                            w.firstFireMs   = now;
                            w.firstFireType = a_type;
                            p.log = true;
                            p.line = fmt::format(
                                "[ctcensus] 0x{} FIRED type={} item=0x{} (driven 0x{}) hand={} target=0x{} at +{} ms "
                                "(first build +{} ms) -- the engine released it from the hand (NotifyStartCast).",
                                apmf::log::Hex(fid), kTypeNames[t], apmf::log::Hex(itemForm), apmf::log::Hex(w.driven),
                                w.hand, apmf::log::Hex(w.target), now - w.openMs, Rel(w.openMs, w.firstBuiltMs));
                        }
                    } else {
                        if (a_answer) { ++w.yesDriven; if (!w.firstYesMs) w.firstYesMs = now; }
                        else ++w.noDriven;
                        if (std::find(w.casters.begin(), w.casters.end(), a_this) == w.casters.end() &&
                            w.casters.size() < 64) {
                            w.casters.push_back(a_this);
                            ++w.builtDriven[t];
                            if (!w.firstBuiltMs) {
                                w.firstBuiltMs   = now;
                                w.firstBuiltType = a_type;
                                p.log = true;
                                p.line = fmt::format(
                                    "[ctcensus] 0x{} BUILT type={} item=0x{} (driven 0x{}) hand={} at +{} ms -- "
                                    "CheckStartCast answered {} (the whole chain's answer).",
                                    apmf::log::Hex(fid), kTypeNames[t], apmf::log::Hex(itemForm),
                                    apmf::log::Hex(w.driven), w.hand, now - w.openMs, a_answer ? "YES" : "NO");
                            }
                        }
                    }
                } else {
                    Window& w0 = it->second.front();
                    if (a_fire) {
                        for (auto& w : it->second) ++w.firedOther[t];
                        p.log = true;
                        p.line = fmt::format(
                            "[ctcensus] 0x{} OTHER FIRED type={} item=0x{} while a claim drives 0x{} (+{} ms) -- the "
                            "engine cast something the claim did not name.",
                            apmf::log::Hex(fid), kTypeNames[t], apmf::log::Hex(itemForm), apmf::log::Hex(w0.driven),
                            now - w0.openMs);
                    } else if (std::find(w0.casters.begin(), w0.casters.end(), a_this) == w0.casters.end() &&
                               w0.casters.size() < 64) {
                        w0.casters.push_back(a_this);
                        for (auto& w : it->second) ++w.builtOther[t];
                        p.log = true;
                        p.line = fmt::format(
                            "[ctcensus] 0x{} OTHER BUILT type={} item=0x{} while a claim drives 0x{} (+{} ms), "
                            "CheckStartCast -> {}.",
                            apmf::log::Hex(fid), kTypeNames[t], apmf::log::Hex(itemForm), apmf::log::Hex(w0.driven),
                            now - w0.openMs, a_answer ? "YES" : "NO");
                    }
                }
            }
            if (p.log && LineDue()) spdlog::info("{}", p.line);
        }

        // ---- the thunks ----
        using CheckStartCast_t  = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);
        using NotifyStartCast_t = void (*)(RE::CombatMagicCaster*, RE::CombatController*);
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_startOrig;
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_notifyOrig;

        // A lookup miss is structurally unreachable (the thunk is entered only through a
        // slot Install() wrote, and the map is filled before any combat can run). If it
        // ever happens the vtable was not patched by this file, so its slot holds a real
        // callable (never this thunk): call THAT, never a fabricated answer.
        template <class Fn>
        Fn OrigFor(const std::unordered_map<std::uintptr_t, std::uintptr_t>& a_map, std::uintptr_t a_vt, std::size_t a_slot) {
            const auto it = a_map.find(a_vt);
            if (it != a_map.end()) return reinterpret_cast<Fn>(it->second);
            g_foreign.fetch_add(1, std::memory_order_relaxed);
            return *reinterpret_cast<Fn*>(a_vt + a_slot * sizeof(void*));
        }

        bool CheckStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt     = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto orig   = OrigFor<CheckStartCast_t>(g_startOrig, vt, kCheckStartCast);
            const bool answer = orig(a_this, a_cc);   // THE CHAIN'S OWN ANSWER, returned unchanged below
            const int  type   = TypeOf(vt);
            if (type >= 0) {
                g_callsAll[static_cast<std::size_t>(type)].fetch_add(1, std::memory_order_relaxed);
                if (Active()) Observe(type, a_this, a_cc, false, answer);
            }
            return answer;
        }

        void NotifyStartCastThunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            const auto vt   = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto orig = OrigFor<NotifyStartCast_t>(g_notifyOrig, vt, kNotifyStartCast);
            const int  type = TypeOf(vt);
            if (type >= 0) {
                g_firesAll[static_cast<std::size_t>(type)].fetch_add(1, std::memory_order_relaxed);
                // Observe BEFORE chaining: every field read here is the caster's own and
                // the controller's attacker handle; nothing the original could tear down.
                if (Active()) Observe(type, a_this, a_cc, true, true);
            }
            orig(a_this, a_cc);
        }

        std::atomic<bool> g_installed{ false };

        // ---- game-thread helpers ----
        struct SeenClaim {
            RE::FormID    actor = 0;
            char          hand  = 'R';
            apmf::CastSeatClaim claim{};
            RE::FormID    driven = 0;
            bool          ctrl  = false;
            std::uint32_t handState = 0;   // claimed hand(s): state while holding spell/proxy
            bool          instant = false; // kInstant caster holding spell/proxy, state != 0
        };

        // ACTOR_RUNTIME_DATA starts at Actor+0xE0 (SE) / +0xE8 (AE); +0xC0 inside it is the
        // disassembled +0x1A0 / +0x1A8 of Character::GetMagicCaster on both runtimes.
        static_assert(offsetof(RE::Actor::ACTOR_RUNTIME_DATA, magicCasters) == 0xC0,
                      "magicCasters moved -- re-verify Character::GetMagicCaster's slot read (AE 0x6C20D0 "
                      "+0x1A8, SE 0x6301C0 +0x1A0) before trusting this read");

        std::uint32_t HandHolds(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_src, RE::FormID a_spell,
                                RE::FormID a_proxy) {
            // READ the slot, never `Actor::GetMagicCaster`: that call (Character vtable slot 0x5C,
            // AE 0x6C20D0 / SE 0x6301C0) ALLOCATES and installs a new ActorMagicCaster when the
            // slot is null. `magicCasters[]` is indexed by CastingSource (kLeftHand 0, kRightHand 1,
            // kInstant 3). Null = no caster = holds nothing.
            const auto idx = static_cast<std::size_t>(a_src);
            if (idx >= 4) return 0;
            auto* mc = a_actor->GetActorRuntimeData().magicCasters[idx];
            if (!mc || !mc->currentSpell) return 0;
            const auto f = mc->currentSpell->GetFormID();
            if (f != a_spell && (a_proxy == 0 || f != a_proxy)) return 0;
            return static_cast<std::uint32_t>(mc->state.get());
        }

        const char* StateName(std::uint32_t s) {   // core/CastObserve.cpp's field-observed names
            switch (s) {
            case 0:  return "None";
            case 1:  return "Unk1";
            case 2:  return "Charging";
            case 3:  return "Charged";
            case 4:  return "Casting";
            case 5:  return "Released";
            case 6:  return "Concluding";
            default: return "?";
            }
        }

        std::string TypeCounts(const std::array<std::uint32_t, kTypes>& a) {
            std::string s;
            for (std::size_t i = 0; i < kTypes; ++i)
                if (a[i]) s += fmt::format("{}{}={}", s.empty() ? "" : ",", kTypeNames[i], a[i]);
            return s.empty() ? "none" : s;
        }

        std::string Verdict(const Window& w) {
            std::uint32_t built = 0, fired = 0;
            for (std::size_t i = 0; i < kTypes; ++i) { built += w.builtDriven[i]; fired += w.firedDriven[i]; }
            const bool handAnimated = w.firstChargeMs != 0;
            if (fired) {
                return fmt::format("FIRED via {} ({}; hand max state {}={}; anim SpellFire {} BeginCast {})",
                                   w.firstFireType >= 0 ? kTypeNames[static_cast<std::size_t>(w.firstFireType)] : "?",
                                   handAnimated ? "the hand charged: ANIMATED" : "no hand charge seen",
                                   w.maxHandState, StateName(w.maxHandState), w.animFire, w.animBegin);
            }
            if (built)
                return fmt::format("BUILT {} NOT FIRED (CheckStartCast YES {} / NO {})", TypeCounts(w.builtDriven),
                                   w.yesDriven, w.noDriven);
            if (!w.ctrlAtOpen && !w.ctrlSeenMs) return "NOT BUILT: NO CONTROLLER (no combat AI to build a caster)";
            return fmt::format("NOT BUILT (predicted {})", w.predicted);
        }

    }

    bool Active() {
        return g_armed.load(std::memory_order_relaxed) && g_openWindows.load(std::memory_order_relaxed) > 0;
    }

    void NoteAnimEvent(RE::FormID a_actor, std::string_view a_tag) {
        if (!Active()) return;
        const bool fire  = a_tag.find("SpellFire") != std::string_view::npos;
        const bool begin = a_tag.find("BeginCast") != std::string_view::npos;
        if (!fire && !begin) return;
        const auto now = apmf::clock::MonotonicMs();
        std::scoped_lock lk(g_mx);
        auto it = g_windows.find(a_actor);
        if (it == g_windows.end()) return;
        for (auto& w : it->second) {
            if (fire) { ++w.animFire; if (!w.firstAnimFireMs) w.firstAnimFireMs = now; }
            if (begin) ++w.animBegin;
        }
    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ctcensus] VR runtime -- caster-type census NOT installed (seats verified on 1.6.1170, "
                         "1.5.97 and 1.7.104 only).");
            return;
        }
        // Exact-binary gate (CLAUDE.md rule 11), the same two-version predicate
        // core/CastClassify.cpp and core/AiCastSeats.cpp's Group C use.
        // F2b: 1.7.104 too -- every CombatMagicCaster* slot function is identical there.
        const auto ver      = REL::Module::get().version();
        const bool onAE1170 = ver == REL::Version{ 1, 6, 1170, 0 };
        const bool onSE597  = ver == REL::Version{ 1, 5, 97, 0 };
        if (!onAE1170 && !onSE597 && !allowance::IsRuntime1_7_104()) {
            spdlog::warn("[ctcensus] runtime {} -- the 0x06/0x0B caster seats are disassembly-confirmed on "
                         "1.6.1170, 1.5.97 and 1.7.104 only; census NOT installed.", ver.string("."));
            return;
        }
        if (g_installed.exchange(true)) return;

        if (GetPrivateProfileIntA("Probe", "bCasterTypeCensus", 1, "Data/SKSE/Plugins/APMF.ini") == 0) {
            spdlog::info("[ctcensus] [Probe] bCasterTypeCensus=0 -- caster-type census off (no seat installed).");
            return;
        }

        REL::Relocation<void*> casterTD{ RE::RTTI_CombatMagicCaster };
        const auto&            vts = CasterVtables();
        const int nStart  = allowance::InstallOnVtables(vts, kCheckStartCast, &CheckStartCastThunk, casterTD.get(),
                                                        "ctcensus-0x06", g_startOrig);
        const int nNotify = allowance::InstallOnVtables(vts, kNotifyStartCast, &NotifyStartCastThunk, casterTD.get(),
                                                        "ctcensus-0x0B", g_notifyOrig);
        g_nStart.store(static_cast<std::uint32_t>(nStart));
        g_nNotify.store(static_cast<std::uint32_t>(nNotify));

        std::string missing, names;
        for (std::size_t i = 0; i < kTypes; ++i) {
            REL::Relocation<std::uintptr_t> vt{ vts[i] };
            const bool both = g_startOrig.find(vt.address()) != g_startOrig.end() &&
                              g_notifyOrig.find(vt.address()) != g_notifyOrig.end();
            if (both) g_vt[i].store(vt.address(), std::memory_order_relaxed);
            else missing += fmt::format("{}{}", missing.empty() ? "" : ",", kTypeNames[i]);
            names += fmt::format("{}{}", i ? "," : "", kTypeNames[i]);
        }
        g_armed.store(nStart > 0 || nNotify > 0, std::memory_order_relaxed);

        if (nStart != static_cast<int>(kTypes) || nNotify != static_cast<int>(kTypes)) {
            spdlog::error("[ctcensus] installed 0x06 on {}/{} and 0x0B on {}/{} caster vtables; NOT observed: {}. A "
                          "type missing here reads as ZERO in every census line -- do not read its zero as \"the "
                          "engine never builds it\".",
                          nStart, kTypes, nNotify, kTypes, missing.empty() ? "-" : missing);
        } else {
            spdlog::info("[ctcensus] armed: CheckStartCast (0x06) + NotifyStartCast (0x0B) observed on all {} "
                         "CombatMagicCaster types ({}). PASSIVE: answers chained unchanged. [Probe] "
                         "bCasterTypeCensus=1 (field test; reset before release).",
                         kTypes, names);
        }
    }

    void Poll() {
        if (!g_armed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastPollMs < 200) return;
        g_lastPollMs = now;

        // ---- 1. what the published claims say right now (engine reads, no lock held) ----
        std::vector<SeenClaim> seen;
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            for (auto& handle : pl->highActorHandles) {
                auto a = handle.get();
                auto* actor = a.get();
                if (!actor || actor->IsPlayerRef()) continue;
                const RE::FormID fid = actor->GetFormID();
                apmf::CastSeatClaim cl{}, cr{};
                const bool hasL = apmf::ControlMap::Get().TryGetCastSeatClaimForHand(fid, apmf::CastHand::kLeft, cl);
                const bool hasR = apmf::ControlMap::Get().TryGetCastSeatClaimForHand(fid, apmf::CastHand::kRight, cr);
                if (!hasL && !hasR) continue;
                const bool ctrl = actor->GetActorRuntimeData().combatController != nullptr;
                auto add = [&](char a_hand, const apmf::CastSeatClaim& c) {
                    if (c.flags & APMF_API::kCastFlag_DenyHandOnly) return;   // drives nothing
                    const RE::FormID driven = c.proxy ? c.proxy : c.spell;
                    if (driven == 0) return;
                    for (auto& s : seen)   // one claim occupying both hands (dual) = one window
                        if (s.actor == fid && s.driven == driven && s.claim.target == c.target) { s.hand = 'D'; return; }
                    SeenClaim s;
                    s.actor  = fid;
                    s.hand   = a_hand;
                    s.claim  = c;
                    s.driven = driven;
                    s.ctrl   = ctrl;
                    seen.push_back(s);
                };
                if (hasL) add('L', cl);
                if (hasR) add('R', cr);
                for (auto& s : seen) {
                    if (s.actor != fid) continue;
                    std::uint32_t st = 0;
                    if (s.hand == 'L' || s.hand == 'D')
                        st = (std::max)(st, HandHolds(actor, RE::MagicSystem::CastingSource::kLeftHand, s.claim.spell, s.claim.proxy));
                    if (s.hand == 'R' || s.hand == 'D')
                        st = (std::max)(st, HandHolds(actor, RE::MagicSystem::CastingSource::kRightHand, s.claim.spell, s.claim.proxy));
                    s.handState = st;
                    s.instant   = HandHolds(actor, RE::MagicSystem::CastingSource::kInstant, s.claim.spell, s.claim.proxy) != 0;
                }
            }
        }
        // Predicted rows for windows about to open (form reads, game thread, no lock).
        std::vector<std::string> predicted(seen.size());
        {
            std::scoped_lock lk(g_mx);
            for (std::size_t i = 0; i < seen.size(); ++i) {
                auto it = g_windows.find(seen[i].actor);
                bool exists = false;
                if (it != g_windows.end())
                    for (auto& w : it->second)
                        if (w.driven == seen[i].driven && w.target == seen[i].claim.target) exists = true;
                if (!exists) predicted[i] = "?";   // mark: compute below
            }
        }
        for (std::size_t i = 0; i < seen.size(); ++i) {
            if (predicted[i].empty()) continue;
            auto* spell = RE::TESForm::LookupByID<RE::MagicItem>(seen[i].driven);
            predicted[i] = PredictRows(spell, static_cast<bool>(seen[i].claim.targetHandle));
        }

        // ---- 2. reconcile the windows (under the leaf lock; lines collected, logged after) ----
        std::vector<std::string> lines;         // budgeted
        std::vector<std::string> closeLines;    // budgeted too, but collected separately (ordered after)
        struct Closed { RE::FormID actor; Window w; std::string why; };
        std::vector<Closed> closed;
        {
            std::scoped_lock lk(g_mx);
            // close windows whose claim is gone or changed
            for (auto it = g_windows.begin(); it != g_windows.end();) {
                auto& vec = it->second;
                for (auto wi = vec.begin(); wi != vec.end();) {
                    const SeenClaim* match = nullptr;
                    bool sameActorOtherTarget = false;
                    for (auto& s : seen) {
                        if (s.actor != wi->actor) continue;
                        if (s.driven == wi->driven && s.claim.target == wi->target) { match = &s; break; }
                        sameActorOtherTarget = true;
                    }
                    if (match) {
                        Window& w = *wi;
                        if (match->ctrl && !w.ctrlSeenMs) w.ctrlSeenMs = now;
                        if (match->handState > w.maxHandState) w.maxHandState = match->handState;
                        if (match->handState >= 2 && !w.firstChargeMs) {
                            w.firstChargeMs = now;
                            lines.push_back(fmt::format(
                                "[ctcensus] 0x{} HAND hand={} holds 0x{} state={}({}) at +{} ms -- the claimed hand is "
                                "charging the claimed spell (the animated path).",
                                apmf::log::Hex(w.actor), w.hand, apmf::log::Hex(w.driven), match->handState,
                                StateName(match->handState), now - w.openMs));
                        }
                        if (match->instant && !w.firstInstantMs) {
                            w.firstInstantMs = now;
                            lines.push_back(fmt::format(
                                "[ctcensus] 0x{} INSTANT caster holds 0x{} at +{} ms -- the claimed form is running on "
                                "the kInstant caster (the direct, unanimated road).",
                                apmf::log::Hex(w.actor), apmf::log::Hex(w.driven), now - w.openMs));
                        }
                        // ZERO milestones: 3 / 10 / 30 s with nothing built for the driven form.
                        static constexpr std::array<std::uint64_t, 3> kMs{ { 3000, 10000, 30000 } };
                        for (std::size_t m = 0; m < kMs.size(); ++m) {
                            if ((w.milestones & (1u << m)) || now - w.openMs < kMs[m] || w.firstBuiltMs) continue;
                            w.milestones |= (1u << m);
                            lines.push_back(fmt::format(
                                "[ctcensus] 0x{} ZERO +{} s: claim drives 0x{} (hand={} target=0x{}) and NO caster was "
                                "built for it | controller {} | other built [{}] other fired [{}] | hand max state {}={} "
                                "| instant {} | predicted {}",
                                apmf::log::Hex(w.actor), kMs[m] / 1000, apmf::log::Hex(w.driven), w.hand,
                                apmf::log::Hex(w.target), (w.ctrlAtOpen || w.ctrlSeenMs) ? "yes" : "NONE",
                                TypeCounts(w.builtOther), TypeCounts(w.firedOther), w.maxHandState,
                                StateName(w.maxHandState), w.firstInstantMs ? "yes" : "no", w.predicted));
                        }
                        ++wi;
                        continue;
                    }
                    const char* why = sameActorOtherTarget ? "claim repointed / replaced" : "claim ended or actor unloaded";
                    closed.push_back({ wi->actor, std::move(*wi), why });
                    wi = vec.erase(wi);
                }
                if (vec.empty()) it = g_windows.erase(it);
                else ++it;
            }
            // open windows for new claims
            for (std::size_t i = 0; i < seen.size(); ++i) {
                const auto& s = seen[i];
                auto& vec = g_windows[s.actor];
                bool exists = false;
                for (auto& w : vec)
                    if (w.driven == s.driven && w.target == s.claim.target) exists = true;
                if (exists) continue;
                Window w;
                w.actor  = s.actor;
                w.hand   = s.hand;
                w.spell  = s.claim.spell;
                w.proxy  = s.claim.proxy;
                w.driven = s.driven;
                w.target = s.claim.target;
                w.flags  = s.claim.flags;
                w.self   = s.claim.target == 0 || s.claim.target == s.actor;
                w.openMs = now;
                w.ctrlAtOpen = s.ctrl;
                w.predicted  = predicted[i].empty() ? "?" : predicted[i];
                vec.push_back(std::move(w));
            }
            int open = 0;
            for (auto& [fid, vec] : g_windows) open += static_cast<int>(vec.size());
            g_openWindows.store(open, std::memory_order_relaxed);
        }

        // ---- 3. series bookkeeping + OPEN / CLOSE / SERIES lines (game thread only) ----
        constexpr std::uint64_t kSeriesGapMs = 8000;
        for (auto& c : closed) {
            ++g_closed;
            const Window& w = c.w;
            std::uint32_t built = 0, fired = 0;
            for (std::size_t i = 0; i < kTypes; ++i) {
                built += w.builtDriven[i]; fired += w.firedDriven[i];
                g_hbBuiltDriven[i] += w.builtDriven[i]; g_hbFiredDriven[i] += w.firedDriven[i];
                g_hbBuiltOther[i] += w.builtOther[i];
            }
            if (!built) ++g_hbZero;
            closeLines.push_back(fmt::format(
                "[ctcensus] 0x{} CLOSE #{}.{} driven=0x{} spell=0x{} hand={} target=0x{}({}) {}ms ({}) | VERDICT: {} | "
                "built [{}] fired [{}] | other built [{}] other fired [{}] | first build +{} first YES +{} first fire +{} "
                "first charge +{} first anim fire +{} instant {} | controller {} | conc {} | predicted {}",
                apmf::log::Hex(w.actor), w.series, w.seriesIdx, apmf::log::Hex(w.driven), apmf::log::Hex(w.spell), w.hand,
                apmf::log::Hex(w.target), w.self ? "self" : "other", now - w.openMs, c.why, Verdict(w),
                TypeCounts(w.builtDriven), TypeCounts(w.firedDriven), TypeCounts(w.builtOther), TypeCounts(w.firedOther),
                Rel(w.openMs, w.firstBuiltMs), Rel(w.openMs, w.firstYesMs), Rel(w.openMs, w.firstFireMs),
                Rel(w.openMs, w.firstChargeMs), Rel(w.openMs, w.firstAnimFireMs),
                w.firstInstantMs ? "yes" : "no", (w.ctrlAtOpen || w.ctrlSeenMs) ? "yes" : "NONE",
                (w.flags & APMF_API::kCastFlag_Concentration) ? "yes" : "no", w.predicted));
            auto& sr = g_series[c.actor];
            sr.lastCloseMs = now;
            if (fired) { ++sr.fired; sr.lastFireMs = w.firstFireMs; }
            if (sr.entries.size() < 900)
                sr.entries += fmt::format("{}[{} 0x{}->0x{} {}{}]", sr.entries.empty() ? "" : " ", w.seriesIdx,
                                          apmf::log::Hex(w.driven), apmf::log::Hex(w.target),
                                          fired ? "fired +" : (built ? "built-not-fired" : "ZERO"),
                                          fired ? std::to_string(w.firstFireMs - w.openMs) : std::string{});
        }
        // Stamp series ids on windows opened this poll, and flush quiet series.
        {
            std::vector<std::pair<RE::FormID, std::string>> opens;
            {
                std::scoped_lock lk(g_mx);
                for (auto& [fid, vec] : g_windows) {
                    for (auto& w : vec) {
                        if (w.series) continue;
                        auto& sr = g_series[fid];
                        const bool continues = sr.count > 0 && now - sr.lastCloseMs < kSeriesGapMs;
                        if (!continues) { sr = SeriesState{}; sr.id = static_cast<std::uint32_t>(++g_opened); }
                        ++sr.count;
                        w.series    = sr.id;
                        w.seriesIdx = sr.count;
                        const std::uint64_t gap = (continues && sr.lastFireMs) ? now - sr.lastFireMs : 0;
                        opens.emplace_back(fid, fmt::format(
                            "[ctcensus] 0x{} OPEN #{}.{} driven=0x{} spell=0x{}{} hand={} target=0x{}({}) conc={} "
                            "controller={} {}| predicted {}",
                            apmf::log::Hex(fid), w.series, w.seriesIdx, apmf::log::Hex(w.driven), apmf::log::Hex(w.spell),
                            w.proxy ? " (proxy)" : "", w.hand, apmf::log::Hex(w.target), w.self ? "self" : "other",
                            (w.flags & APMF_API::kCastFlag_Concentration) ? "yes" : "no", w.ctrlAtOpen ? "yes" : "NONE",
                            continues ? fmt::format("| series continues, {} ms after the previous window's fire ",
                                                    gap)
                                      : std::string{},
                            w.predicted));
                    }
                }
            }
            for (auto& o : opens) lines.push_back(std::move(o.second));
        }
        for (auto it = g_series.begin(); it != g_series.end();) {
            auto& sr = it->second;
            bool open = false;
            {
                std::scoped_lock lk(g_mx);
                open = g_windows.find(it->first) != g_windows.end();
            }
            if (!open && sr.count > 0 && now - sr.lastCloseMs >= kSeriesGapMs) {
                if (sr.count >= 2)
                    closeLines.push_back(fmt::format(
                        "[ctcensus] 0x{} SERIES #{}: {} back-to-back window(s), {} fired | {}",
                        apmf::log::Hex(it->first), sr.id, sr.count, sr.fired, sr.entries));
                it = g_series.erase(it);
                continue;
            }
            ++it;
        }

        for (auto& l : lines)
            if (LineDue()) spdlog::info("{}", l);
        for (auto& l : closeLines)
            if (LineDue()) spdlog::info("{}", l);

        // ---- 4. heartbeat (RULE C: prints with zeros, bypasses the budget) ----
        if (now - g_lastHeartbeatMs >= 60000) {
            g_lastHeartbeatMs = now;
            auto all = [](const std::array<std::atomic<std::uint64_t>, kTypes>& a) {
                std::string s;
                for (std::size_t i = 0; i < kTypes; ++i)
                    s += fmt::format("{}{}={}", i ? "," : "", kTypeNames[i], a[i].load(std::memory_order_relaxed));
                return s;
            };
            auto arr = [](const std::array<std::uint64_t, kTypes>& a) {
                std::string s;
                for (std::size_t i = 0; i < kTypes; ++i) s += fmt::format("{}{}={}", i ? "," : "", kTypeNames[i], a[i]);
                return s;
            };
            spdlog::info("[ctcensus] HEARTBEAT seats 0x06={}/15 0x0B={}/15 | windows open={} opened-series={} closed={} "
                         "zero-built={} | claimed: built [{}] fired [{}] other-built [{}] | ENGINE-WIDE (every actor) "
                         "fires [{}] CheckStartCast calls [{}] | lines dropped {} | foreign-vtable entries {}",
                         g_nStart.load(), g_nNotify.load(), g_openWindows.load(std::memory_order_relaxed), g_opened,
                         g_closed, g_hbZero, arr(g_hbBuiltDriven), arr(g_hbFiredDriven), arr(g_hbBuiltOther),
                         all(g_firesAll), all(g_callsAll), g_dropped.load(std::memory_order_relaxed),
                         g_foreign.load(std::memory_order_relaxed));
        }
    }

}
