#include "PCH.h"
#include "core/CombatStyle.h"
#include "core/Allowance.h"          // SeatVerified(), RuntimeSupported(), IsRuntime1_7_104()
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/FacetAttribution.h"   // the Engine source on the CombatAction facet, ranked against the claim
#include "core/Log.h"
#include "core/Sources.h"

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, int a_default, const char* a_fileName);

// ============================================================================
// ch.33 COMBAT STYLE (kIntent_CombatStyle, ABI v45) -- the actor fights by the combat style the client NAMED.
//
// THE ENGINE PATH (read in the 1.6.1170, 1.5.97 and 1.7.104 executables; Docs/research/melee-moves-re.md).
//   Actor::GetCombatStyle (AE id 38536 0x6B5580 / SE 37586 0x623E60 / 1.7.104 0x6C8000), the same body on all three:
//       style = ExtraDataList::GetCombatStyle(&this->extraList)        ; the call at +0xD, `add rcx,0x70` before it
//       if (!style) style = currentProcess ? currentPackage ? package->combatStyle : 0 : 0
//       if (!style) style = actorBase->GetCombatStyle()                 ; vfunc +0x2A0
//   It has 27 callers on each build: the style accessor family the combat AI reads its multipliers, chances and
//   movement fields through. ExtraDataList::GetCombatStyle (AE 12017 / SE 11878 / 1.7.104 0x171550) is the engine's
//   OWN per-reference override slot (ExtraCombatStyle) and has exactly ONE caller per build: that call.
//   CombatController::UpdateCombatStyle (AE 33239 0x559BD0 / SE 32492 0x4FE5B0 / 1.7.104 0x561A40):
//       old = this->combatStyle (+0x38); this->combatStyle = Actor::GetCombatStyle(attacker)   ; the call at +0x70
//       (SE +0x8A); if (old && old != new) this->inventory(+0x10)->dirty(+0x1C4) = 1
//   controller+0x38 is therefore a CACHE, refreshed only there (the controller's construction, a package change,
//   Actor::SetCombatStyle's tail jump). Its readers are the combat inventory's score multipliers (which weapon,
//   spell, shout or staff the AI equips); every other style read goes through Actor::GetCombatStyle.
//
// THE TWO SEATS (INVARIANTS #17a: write_call<5> at a proven call site; the row + lead/tail bytes + E8 target are
// re-read at Install, and both install or neither does).
//   SEAT B, Actor::GetCombatStyle +0xD (the call to ExtraDataList::GetCombatStyle). For an actor whose live claim
//       is ACTOR-WIDE the thunk answers the claim's style in the place of the engine's own override slot, so
//       every reader of the actor's style gets it. Otherwise the original.
//   SEAT A, CombatController::UpdateCombatStyle +0x70 / +0x8A (the call to Actor::GetCombatStyle). For an actor
//       with ANY live claim the thunk answers the claim's style into the controller's cache. This is the whole
//       effect of a kCombatStyle_EquipChoiceOnly claim: the equipment choice follows the claimed style and nothing
//       else changes.
//   ENGAGE / OWNER CHANGE / RELEASE: Harbinger calls the engine's own UpdateCombatStyle on the actor's live
//       controller (the call Actor::SetCombatStyle's tail makes, AE 38537 +0x31), so the cache picks the claim up,
//       or drops it, at once and the engine marks its own inventory dirty flag. Harbinger writes NOTHING itself:
//       not the controller, not an ExtraCombatStyle, not the base record.
//
// THREADS. The seats run wherever the engine reads a style (the combat update's job workers, the game thread).
// They read g_slots only: a fixed table of atomics {FormID, style pointer, flags}, no lock, no form lookup, no
// allocation; one relaxed load while no claim stands. The table is written on the GAME THREAD only (the channel's
// lifecycle and Heartbeat), a slot's FormID is published last and withdrawn first, and a reader re-reads the
// FormID after the pointer. A TESCombatStyle is a form: it is never freed while the game runs.
//
// RANK. A claim's style is answered only while its client outranks the Engine source on the CombatAction facet
// (the facet ch.7 and ch.31 rank on). Decided on the game thread at engage and again every kRedecideMs, never per
// read: the seats are too hot for a ranking and a counted hit per read would drown the attribution view.
//
// DENY-COMPLETENESS: Docs/DENY-COMPLETENESS-AUDIT.md row 33 names the open paths (a style read that does not go
// through Actor::GetCombatStyle, values the engine derived from the old style before the claim, another DLL's
// write to controller+0x38).
// ============================================================================
namespace apmf::combatstyle {

    namespace {
        using apmf::log::Hex;
        using APMF_API::Handle;

        constexpr const char*   kIni         = "Data/SKSE/Plugins/APMF.ini";
        constexpr std::uint32_t kMaxSlots    = APMF_API::kCombatStyleMaxClaims;
        constexpr std::uint32_t kLive        = 1u << 31;   // slot flag (internal): ranked above Engine, answered
        constexpr std::uint64_t kRedecideMs  = 5000;
        constexpr std::uint64_t kBeatMs      = 30000;

        using ExtraGet_t = RE::TESCombatStyle* (*)(RE::ExtraDataList*);
        using ActorGet_t = RE::TESCombatStyle* (*)(RE::Actor*);
        using Update_t   = void (*)(RE::CombatController*);

        ExtraGet_t g_extraGet = nullptr;   // the verified originals (written once in Install, before any patch)
        ActorGet_t g_actorGet = nullptr;
        Update_t   g_update   = nullptr;

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_whyNot{ "not installed yet (before kDataLoaded)" };

        // ---- what the seats read --------------------------------------------------------------------------------
        struct Slot {
            std::atomic<RE::FormID>          fid{ 0 };
            std::atomic<RE::TESCombatStyle*> style{ nullptr };
            std::atomic<std::uint32_t>       flags{ 0 };   // CombatStyleFlags | kLive
        };
        std::array<Slot, kMaxSlots> g_slots{};
        std::atomic<std::uint32_t>  g_n{ 0 };    // slots in use
        std::atomic<std::uint32_t>  g_hi{ 0 };   // one past the highest slot ever used since the last reset

        std::atomic<std::uint64_t> t_wide{ 0 }, t_cache{ 0 }, t_refresh{ 0 }, t_inert{ 0 };
        std::atomic<bool>          g_firstB{ false }, g_firstA{ false };

        // The claim's style for `a_fid`, or nullptr. `a_wideOnly`: seat B (an EquipChoiceOnly claim is not answered).
        RE::TESCombatStyle* Claimed(RE::FormID a_fid, bool a_wideOnly) {
            const std::uint32_t hi = g_hi.load(std::memory_order_acquire);
            for (std::uint32_t i = 0; i < hi; ++i) {
                Slot& s = g_slots[i];
                if (s.fid.load(std::memory_order_acquire) != a_fid) continue;
                const std::uint32_t       fl = s.flags.load(std::memory_order_acquire);
                RE::TESCombatStyle* const st = s.style.load(std::memory_order_acquire);
                if (s.fid.load(std::memory_order_acquire) != a_fid) return nullptr;   // withdrawn while it was read
                if (!(fl & kLive) || !st) return nullptr;
                if (a_wideOnly && (fl & APMF_API::kCombatStyle_EquipChoiceOnly)) return nullptr;
                return st;
            }
            return nullptr;
        }

        // SEAT B. rcx = &actor->extraList (the engine's `add rcx,0x70`, the same on all three builds).
        RE::TESCombatStyle* ExtraGetThunk(RE::ExtraDataList* a_list) {
            if (g_n.load(std::memory_order_relaxed) != 0 && a_list) {
                static_assert(offsetof(RE::TESObjectREFR, extraList) == 0x70, "seat B derives the actor from its extra list");
                const auto* ref = reinterpret_cast<const RE::TESObjectREFR*>(reinterpret_cast<const std::byte*>(a_list) - 0x70);
                if (RE::TESCombatStyle* st = Claimed(ref->GetFormID(), true)) {
                    t_wide.fetch_add(1, std::memory_order_relaxed);
                    if (!g_firstB.exchange(true, std::memory_order_relaxed))
                        spdlog::info("[ch.33] FIRST answer of seat B (Actor::GetCombatStyle): actor 0x{} reads the claimed combat "
                                     "style 0x{}. The seat runs.", Hex(ref->GetFormID()), Hex(st->GetFormID()));
                    return st;
                }
            }
            return g_extraGet(a_list);
        }

        // SEAT A. rcx = the controller's attacker (the engine passes whatever its handle resolved to, null included:
        // that is handed to the original untouched).
        RE::TESCombatStyle* UpdateGetThunk(RE::Actor* a_actor) {
            if (g_n.load(std::memory_order_relaxed) != 0 && a_actor) {
                if (RE::TESCombatStyle* st = Claimed(a_actor->GetFormID(), false)) {
                    t_cache.fetch_add(1, std::memory_order_relaxed);
                    if (!g_firstA.exchange(true, std::memory_order_relaxed))
                        spdlog::info("[ch.33] FIRST answer of seat A (CombatController::UpdateCombatStyle): actor 0x{}'s controller "
                                     "caches the claimed combat style 0x{}. The seat runs.", Hex(a_actor->GetFormID()),
                                     Hex(st->GetFormID()));
                    return st;
                }
            }
            return g_actorGet(a_actor);
        }

        // ---- game-thread bookkeeping ----------------------------------------------------------------------------
        struct Book {
            RE::FormID    id         = 0;
            RE::FormID    styleId    = 0;
            std::uint32_t flags      = 0;       // CombatStyleFlags
            std::uint32_t slot       = 0;
            bool          decided    = false;   // ranked at least once since the last SetClaim
            bool          live       = false;
            bool          everLive   = false;   // the controller's cache may hold a style of this claim
            bool          refresh    = false;   // the controller's cache must be refreshed by the engine
            std::uint64_t decidedMs  = 0;
        };
        std::mutex              g_bmx;       // LEAF: the two vectors only (no engine call, no log under it)
        std::vector<Book>       g_book;
        std::vector<RE::FormID> g_released;  // actors whose claim ended: one engine refresh each
        std::atomic<std::uint32_t> g_work{ 0 };   // g_book.size() + g_released.size(), for Heartbeat's fast path

        void SyncWork() { g_work.store(static_cast<std::uint32_t>(g_book.size() + g_released.size()), std::memory_order_release); }   // g_bmx held

        // GAME THREAD. The engine's own refresh of the controller's cached style (reads the seats).
        void EngineRefresh(RE::FormID a_id, const char* a_why) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* cc    = actor ? actor->GetActorRuntimeData().combatController : nullptr;
            if (!cc) return;   // no controller: the next one's construction reads the seats itself
            auto* const before = cc->combatStyle;
            g_update(cc);
            t_refresh.fetch_add(1, std::memory_order_relaxed);
            auto* const after = cc->combatStyle;
            spdlog::info("[ch.33] 0x{} '{}': the engine refreshed his controller's cached combat style ({}): 0x{} -> 0x{}.",
                         Hex(a_id), actor->GetName(), a_why, Hex(before ? before->GetFormID() : 0),
                         Hex(after ? after->GetFormID() : 0));
        }

        // ---- install helpers ------------------------------------------------------------------------------------
        struct Fn {
            const char*    row;
            std::uint64_t  id;    // 0 = `rva` is used (1.7.104: no Address Library)
            std::uintptr_t rva;
        };
        struct RtSpec {
            const char* name;
            Fn          actorGet, extraGet, update;
            std::size_t siteA;   // inside update: the E8 to actorGet
        };
        constexpr std::size_t  kSiteB = 0x0D;   // inside actorGet: the E8 to extraGet (all three builds)
        // Seat B: the whole prologue up to the call, then `test rax,rax; jne +0x31`.
        constexpr std::uint8_t kBLead[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1, 0x70 };
        constexpr std::uint8_t kBTail[] = { 0x48, 0x85, 0xC0, 0x75, 0x31 };
        // Seat A: `mov [rsp+0x30],rcx` before the call, then `mov [rdi+0x38],rax; test rsi,rsi`.
        constexpr std::uint8_t kALead[] = { 0x48, 0x89, 0x4C, 0x24, 0x30 };
        constexpr std::uint8_t kATail[] = { 0x48, 0x89, 0x47, 0x38, 0x48, 0x85, 0xF6 };

        constexpr RtSpec kAE{ "1.6.1170",
                              { "CombatStyle.Actor.GetCombatStyle", 38536, 0 },
                              { "CombatStyle.ExtraDataList.GetCombatStyle", 12017, 0 },
                              { "CombatStyle.CombatController.UpdateCombatStyle", 33239, 0 },
                              0x70 };
        constexpr RtSpec kSE{ "1.5.97",
                              { "CombatStyle.Actor.GetCombatStyle", 37586, 0 },
                              { "CombatStyle.ExtraDataList.GetCombatStyle", 11878, 0 },
                              { "CombatStyle.CombatController.UpdateCombatStyle", 32492, 0 },
                              0x8A };
        constexpr RtSpec k17{ "1.7.104",
                              { "CombatStyle.Actor.GetCombatStyle", 0, 0x6C8000 },
                              { "CombatStyle.ExtraDataList.GetCombatStyle", 0, 0x171550 },
                              { "CombatStyle.CombatController.UpdateCombatStyle", 0, 0x561A40 },
                              0x70 };

        std::uintptr_t Resolve(const Fn& a_fn) {
            return a_fn.id ? REL::ID(a_fn.id).address() : REL::Module::get().base() + a_fn.rva;
        }
        // Lead bytes ending at the E8, an E8 that lands on a_target, the tail bytes. Returns the E8's address or 0.
        template <std::size_t L, std::size_t T>
        std::uintptr_t ProveSite(std::uintptr_t a_fn, std::size_t a_e8Off, const std::uint8_t (&a_lead)[L],
                                 const std::uint8_t (&a_tail)[T], std::uintptr_t a_target) {
            const std::uintptr_t e8 = a_fn + a_e8Off;
            if (std::memcmp(reinterpret_cast<const void*>(e8 - L), a_lead, L) != 0) return 0;
            if (*reinterpret_cast<const std::uint8_t*>(e8) != 0xE8) return 0;
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(e8 + 1), sizeof(rel));
            if (e8 + 5 + static_cast<std::intptr_t>(rel) != a_target) return 0;
            if (std::memcmp(reinterpret_cast<const void*>(e8 + 5), a_tail, T) != 0) return 0;
            return e8;
        }
    }  // namespace

    bool        Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_whyNot.load(std::memory_order_acquire); }

    void Install() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        static_assert(offsetof(RE::CombatController, combatStyle) == 0x38, "the cached style the engine's refresh writes");
        const char*   why = nullptr;
        const RtSpec* rt  = nullptr;
        if (REL::Module::IsVR()) why = "VR runtime";
        else if (!apmf::allowance::RuntimeSupported()) why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        else if (GetPrivateProfileIntA("CombatStyle", "bCombatStyle", 1, kIni) == 0) why = "[CombatStyle] bCombatStyle=0";
        std::uintptr_t actorGet = 0, extraGet = 0, update = 0, siteA = 0, siteB = 0;
        if (!why) {
            rt = apmf::allowance::IsRuntime1_7_104()               ? &k17
               : REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97) ? &kSE
                                                                   : &kAE;
            actorGet = Resolve(rt->actorGet);
            extraGet = Resolve(rt->extraGet);
            update   = Resolve(rt->update);
            bool ok  = apmf::allowance::SeatVerified(actorGet, rt->actorGet.row);
            ok       = apmf::allowance::SeatVerified(extraGet, rt->extraGet.row) && ok;
            ok       = apmf::allowance::SeatVerified(update, rt->update.row) && ok;
            if (!ok) why = "the address self-check refused a row (listed above)";
        }
        if (!why) {
            // The bytes NOW (another DLL may have patched a site since the self-check).
            siteB = ProveSite(actorGet, kSiteB, kBLead, kBTail, extraGet);
            siteA = ProveSite(update, rt->siteA, kALead, kATail, actorGet);
            if (!siteB) why = "Actor::GetCombatStyle's call to the extra-data style read is not the verified one (another patcher, or a different build)";
            else if (!siteA) why = "CombatController::UpdateCombatStyle's call to Actor::GetCombatStyle is not the verified one (another patcher, or a different build)";
        }
        if (why) {
            g_whyNot.store(why, std::memory_order_release);
            spdlog::warn("[ch.33] combat style seats NOT installed -- {}. Every kIntent_CombatStyle claim is refused; every actor "
                         "fights by the style the engine derives.", why);
            return;
        }
        g_extraGet = reinterpret_cast<ExtraGet_t>(extraGet);
        g_actorGet = reinterpret_cast<ActorGet_t>(actorGet);
        g_update   = reinterpret_cast<Update_t>(update);

        static auto* s_tr = new SKSE::Trampoline("APMF.CombatStyle");
        void* mem = nullptr;
        if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(32);
        if (mem) s_tr->set_trampoline(mem, 32);
        else s_tr->create(32);

        // The originals are the VERIFIED targets (stored above, before any patch). With no slot both thunks call
        // straight through, so the order of the two patches does not matter.
        const std::uintptr_t prevB = s_tr->write_call<5>(siteB, reinterpret_cast<std::uintptr_t>(&ExtraGetThunk));
        const std::uintptr_t prevA = s_tr->write_call<5>(siteA, reinterpret_cast<std::uintptr_t>(&UpdateGetThunk));
        if (prevB != extraGet || prevA != actorGet)
            spdlog::error("[ch.33] seat patch: a previous call target changed between the check and the patch (B 0x{:X}, A "
                          "0x{:X}); the verified functions are called.", prevB, prevA);
        using apmf::sources::Facet;
        apmf::sources::RegisterCallSeat(Facet::kCombatAction, "CombatStyle.Site.ActorGet", siteB, prevB);
        apmf::sources::RegisterCallSeat(Facet::kCombatAction, "CombatStyle.Site.ControllerCache", siteA, prevA);

        g_installed.store(true, std::memory_order_release);
        g_whyNot.store("installed", std::memory_order_release);
        const auto base = REL::Module::get().base();
        spdlog::info("[ch.33] combat style seats installed on {} (kIntent_CombatStyle, ABI v45): seat B at Actor::GetCombatStyle's "
                     "extra-data read (RVA 0x{:X}), seat A at CombatController::UpdateCombatStyle's style read (RVA 0x{:X}). A "
                     "claimed actor reads the client's combat style; an equip-choice-only claim reaches the controller's cache "
                     "alone. The engine's own UpdateCombatStyle (RVA 0x{:X}) refreshes that cache at engage and release. "
                     "Nothing is written to a controller, a reference or a base record. Up to {} claims.",
                     rt->name, siteB - base, siteA - base, update - base, kMaxSlots);
    }

    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param) {
        if (!Installed()) return NotInstalledReason();
        if (a_actor == 0x14) return "the actor is the player (a combat style claim serves NPCs only)";
        if (!a_param) return "no param (kIntent_CombatStyle needs RequestEx with param.form = a combat style)";
        if (a_param->form == 0) return "param.form is 0 (kIntent_CombatStyle needs the combat style's FormID)";
        if ((static_cast<std::uint32_t>(a_param->ival) & ~APMF_API::kCombatStyle_EquipChoiceOnly) != 0)
            return "param.ival carries a CombatStyleFlags bit this ABI does not define";
        return nullptr;
    }

    void SetClaim(RE::FormID a_id, RE::Actor* a_actor, const APMF_API::APMF_Param& a_param, const char* a_what) {
        if (!Installed()) return;
        auto* style = a_actor ? RE::TESForm::LookupByID<RE::TESCombatStyle>(a_param.form) : nullptr;
        if (!style) {
            // Never a mask: a claim Harbinger cannot serve is said to be inert, loudly, and nothing of it stands.
            ClearClaim(a_id);
            t_inert.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("[ch.33] 0x{} combat style claim is INERT: {}. Nothing is answered for this actor; fix the client's "
                          "request.", Hex(a_id),
                          a_actor ? std::format("form 0x{} is not a combat style (TESCombatStyle)", Hex(a_param.form))
                                  : std::string{ "the actor did not resolve" });
            return;
        }
        const std::uint32_t flags = static_cast<std::uint32_t>(a_param.ival) & APMF_API::kCombatStyle_EquipChoiceOnly;
        bool full = false;
        {
            std::scoped_lock lock(g_bmx);
            Book* b = nullptr;
            for (auto& e : g_book)
                if (e.id == a_id) b = &e;
            if (!b) {
                std::uint32_t slot = kMaxSlots;
                for (std::uint32_t i = 0; i < kMaxSlots; ++i)
                    if (g_slots[i].fid.load(std::memory_order_relaxed) == 0) {
                        bool taken = false;
                        for (const auto& e : g_book) taken = taken || e.slot == i;
                        if (!taken) {
                            slot = i;
                            break;
                        }
                    }
                if (slot == kMaxSlots) {
                    full = true;
                } else {
                    g_book.push_back(Book{});
                    b       = &g_book.back();
                    b->id   = a_id;
                    b->slot = slot;
                    if (slot + 1 > g_hi.load(std::memory_order_relaxed)) g_hi.store(slot + 1, std::memory_order_release);
                    g_n.fetch_add(1, std::memory_order_release);
                }
            }
            if (b) {
                Slot& s = g_slots[b->slot];
                s.fid.store(0, std::memory_order_release);       // withdrawn while it is rewritten
                s.style.store(style, std::memory_order_release);
                s.flags.store(flags, std::memory_order_release); // NOT live: Heartbeat ranks it first (after the publish)
                s.fid.store(a_id, std::memory_order_release);
                b->styleId = a_param.form;
                b->flags   = flags;
                b->decided = false;
                b->live    = false;
                b->refresh = false;
            }
            SyncWork();
        }
        if (full) {
            t_inert.fetch_add(1, std::memory_order_relaxed);
            spdlog::error("[ch.33] 0x{} combat style claim is INERT: {} claims already stand (kCombatStyleMaxClaims). Nothing is "
                          "answered for this actor.", Hex(a_id), kMaxSlots);
            return;
        }
        spdlog::info("[ch.33] 0x{} '{}' combat style {}: style 0x{} '{}', scope {}. It is answered once the claim is ranked above "
                     "the Engine source (next frame).", Hex(a_id), a_actor->GetName(), a_what, Hex(a_param.form),
                     style->GetFormEditorID() ? style->GetFormEditorID() : "",
                     (flags & APMF_API::kCombatStyle_EquipChoiceOnly) ? "EQUIP CHOICE ONLY (the controller's cache)"
                                                                      : "ACTOR-WIDE (every read of his style)");
    }

    void ClearClaim(RE::FormID a_id) {
        std::scoped_lock lock(g_bmx);
        for (auto it = g_book.begin(); it != g_book.end(); ++it) {
            if (it->id != a_id) continue;
            Slot& s = g_slots[it->slot];
            s.fid.store(0, std::memory_order_release);
            s.flags.store(0, std::memory_order_release);
            s.style.store(nullptr, std::memory_order_release);
            g_n.fetch_sub(1, std::memory_order_release);
            if (it->everLive) g_released.push_back(a_id);   // the cache may hold the claim's style: the engine re-derives it
            g_book.erase(it);
            break;
        }
        SyncWork();
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        {
            std::scoped_lock lock(g_bmx);
            n = g_book.size();
            for (auto& s : g_slots) {
                s.fid.store(0, std::memory_order_release);
                s.flags.store(0, std::memory_order_release);
                s.style.store(nullptr, std::memory_order_release);
            }
            g_n.store(0, std::memory_order_release);
            g_hi.store(0, std::memory_order_release);
            g_book.clear();
            g_released.clear();
            SyncWork();
        }
        if (n != 0) spdlog::info("[ch.33] {} -- forgot {} combat style claim slot(s).", a_why, n);
    }

    void Heartbeat() {
        if (!Installed()) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_work.load(std::memory_order_acquire) != 0) {
            std::vector<Book>       book;
            std::vector<RE::FormID> released;
            {
                std::scoped_lock lock(g_bmx);
                book = g_book;
                released.swap(g_released);
                SyncWork();
            }
            for (const RE::FormID id : released) {
                // A newer claim on the same actor since the release: its own engage refreshes.
                bool again = false;
                for (const auto& b : book) again = again || b.id == id;
                if (!again) EngineRefresh(id, "claim released");
            }
            for (const auto& b : book) {
                if (b.decided && now - b.decidedMs < kRedecideMs) continue;
                APMF_API::APMF_Param p{};
                float                basis = 0.0f;
                Handle               h     = APMF_API::kInvalidHandle;
                if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(b.id, APMF_API::kIntent_CombatStyle, p, basis, &h))
                    continue;   // not published yet, or released (the Release call is one drain behind)
                const auto v    = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, h, apmf::sources::kEngineSource,
                                                          b.id, "");
                bool       changed = false, first = false;
                {
                    std::scoped_lock lock(g_bmx);
                    for (auto& e : g_book) {
                        if (e.id != b.id || e.styleId != b.styleId || e.flags != b.flags) continue;   // rewritten meanwhile
                        first       = !e.decided;
                        changed     = first || e.live != v.enforce;
                        e.decided   = true;
                        e.decidedMs = now;
                        e.live      = v.enforce;
                        e.everLive  = e.everLive || v.enforce;
                        Slot& s = g_slots[e.slot];
                        s.flags.store(e.flags | (v.enforce ? kLive : 0u), std::memory_order_release);
                    }
                }
                if (!changed) continue;
                if (v.enforce) {
                    spdlog::info("[ch.33] 0x{} combat style 0x{} is LIVE (h={}, client priority {} above Engine {}).", Hex(b.id),
                                 Hex(b.styleId), h, v.claimPrio, v.srcPrio);
                    EngineRefresh(b.id, "claim live");
                } else {
                    spdlog::warn("[ch.33] 0x{} combat style 0x{} is NOT answered (h={}): the client's priority {} is not above "
                                 "Engine's {} on the CombatAction facet (Harbinger menu). The engine's own style stands.",
                                 Hex(b.id), Hex(b.styleId), h, v.claimPrio, v.srcPrio);
                    if (!first) EngineRefresh(b.id, "claim outranked");
                }
            }
        }
        // RULE C: the totals, about every 30 s while any seat answered since the last line.
        static std::uint64_t s_lastBeat = 0, s_lastSum = 0;
        if (now - s_lastBeat >= kBeatMs) {
            const std::uint64_t sum = t_wide.load(std::memory_order_relaxed) + t_cache.load(std::memory_order_relaxed) +
                                      t_refresh.load(std::memory_order_relaxed) + t_inert.load(std::memory_order_relaxed);
            if (sum != s_lastSum) {
                s_lastSum = sum;
                spdlog::info("[ch.33] H combat style: claims standing={} | seat B answered (actor-wide reads)={} | seat A answered "
                             "(controller cache)={} | engine refreshes asked={} | inert claims={}",
                             g_n.load(std::memory_order_relaxed), t_wide.load(), t_cache.load(), t_refresh.load(), t_inert.load());
            }
            s_lastBeat = now;
        }
    }
}
