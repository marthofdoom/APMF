#include "PCH.h"
#include "core/Log.h"
#include "core/AliasPool.h"

// See AliasPool.h for the design writeup; Docs/SPEC-ALIAS-DRIVE.md is the
// authoritative spec. This file is a direct port of MFO's field-proven
// loot-travel alias mechanism (native/Packages.cpp: ForceRefToNative,
// LootActorAlias/EvictionRef, LootTravelClear/LootTravelEvictIf, the
// ReleaseAll per-slot sweep) -- trimmed to APMF's own 16-slot generic pool
// and wired to the SKSE serialization interface the way core/AvLedger.cpp
// already is.

namespace apmf::aliaspool {

    namespace {

        constexpr const char* kPlugin = "APMF.esl";
        constexpr RE::FormID  kLocalClaimQuest         = 0x800;
        constexpr RE::FormID  kLocalPlaceholderPackage = 0x801;

        RE::TESQuest*   g_claimQuest         = nullptr;
        RE::TESPackage* g_placeholderPackage = nullptr;   // resolved + logged only; never touched at runtime

        // Slot -> occupant FormID (0 == free). Main-thread-only (see AliasPool.h);
        // the ONLY writers are ClaimSlot/ReleaseActor/ReleaseAll/Revert.
        RE::FormID g_slotActor[kNumSlots] = {};

        // Mirrors Packages.cpp's EvictMarkerRef/EvictionRef exactly: a session-
        // minted, force-persisted, non-actor XMarker to displace an evicted actor
        // with -- NEVER the player (forcing the player into a package-carrying
        // alias is the furniture-ejection bug class, MFO INVARIANTS #3/#48).
        RE::ObjectRefHandle g_evictMarker{};

        RE::TESObjectREFR* EvictMarkerRef() {
            auto ptr = g_evictMarker.get();
            auto* m = ptr.get();
            // A handle minted in a PREVIOUS save can resolve to something else
            // entirely once handle tables rebuild on load -- only trust it if it
            // still resolves to our XMarker base (0x3B).
            if (m && m->GetBaseObject() && m->GetBaseObject()->GetFormID() == 0x3B)
                return m;
            return nullptr;
        }

        RE::TESObjectREFR* EvictionRef() {
            if (auto* m = EvictMarkerRef()) return m;
            return RE::PlayerCharacter::GetSingleton();
        }

        RE::BGSRefAlias* AliasByID(std::uint32_t a_aliasID) {
            auto* quest = g_claimQuest;
            if (!quest) return nullptr;
            for (auto* base : quest->aliases) {
                if (!base || base->aliasID != a_aliasID) continue;
                return skyrim_cast<RE::BGSRefAlias*>(base);
            }
            return nullptr;
        }

        // NATIVE FIRST, VM as fallback -- ported verbatim from MFO's
        // Packages.cpp ForceRefToNative. The native is `TESQuest::ForceRefTo
        // (aliasID, refr)`; SKSE64 2.2.6 (this runtime's CURRENT_RELEASE_RUNTIME,
        // 1.6.1170) declares it at GameForms.h:1704, offset 0x003CDEE0, which
        // decodes to versionlib id 25052 on this runtime -- the SAME id MFO's
        // own alias-fill plumbing uses (byte-identical CommonLibSSE-NG baseline,
        // native/CMakeLists.txt). AE ONLY -- the SE (1.5.97) id could not be
        // verified (no SE-era SKSE source on disk, and the SE/AE id windows are
        // not index-aligned); off AE this returns false and the caller falls
        // back to the VM route below.
        bool ForceRefToNative(RE::TESQuest* a_quest, std::uint32_t a_aliasID, RE::TESObjectREFR* a_ref) {
            if (!a_quest) return false;
            if (!REL::Module::IsAE()) return false;   // id unverified off AE

            using func_t = std::uint32_t (*)(RE::TESQuest*, std::uint32_t, RE::TESObjectREFR*);
            static const REL::Relocation<func_t> func{ REL::ID(25052) };
            func(a_quest, a_aliasID, a_ref);
            return true;
        }

        RE::BSScript::Internal::VirtualMachine* VM() {
            return RE::BSScript::Internal::VirtualMachine::GetSingleton();
        }

        // A VM handle for an ALIAS, which is NOT a TESForm -- its VM identity is
        // a fixed type id instead (BGSRefAlias::VMTYPEID = 140). Ported verbatim
        // from MFO's Packages.cpp HandleForAlias (see that file's comment for the
        // SKSE-precedent citation: PapyrusAlias.cpp/PapyrusReferenceAlias.cpp
        // both mint alias handles this exact way).
        bool HandleForAlias(RE::BGSRefAlias* a_alias, RE::VMHandle& a_out) {
            auto* vm = VM();
            if (!vm || !a_alias) return false;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return false;
            const auto h = policy->GetHandleForObject(RE::BGSRefAlias::VMTYPEID, a_alias);
            if (h == policy->EmptyHandle()) return false;   // invalid is NOT zero
            a_out = h;
            return true;
        }

        // Dispatch ReferenceAlias.ForceRefTo / .Clear through the VM -- the
        // fallback when the native is unavailable (SE). Ported verbatim from
        // MFO's Packages.cpp DispatchAlias.
        bool DispatchAlias(const char* a_fn, RE::TESObjectREFR* a_arg, std::uint32_t a_aliasID) {
            auto* vm = VM();
            auto* alias = AliasByID(a_aliasID);
            if (!vm || !alias) return false;

            RE::VMHandle handle{};
            if (!HandleForAlias(alias, handle)) {
                spdlog::error("[aliaspool] no VM handle for alias {} -- {} unreachable", a_aliasID, a_fn);
                return false;
            }

            std::unique_ptr<RE::BSScript::IFunctionArguments> args{
                a_arg ? RE::MakeFunctionArguments(std::move(a_arg))
                      : RE::MakeFunctionArguments() };
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

            return vm->DispatchMethodCall2(handle, "ReferenceAlias", a_fn, args.get(), callback);
        }

        int FindSlotFor(RE::FormID actorID) {
            for (int slot = 0; slot < kNumSlots; ++slot)
                if (g_slotActor[slot] == actorID) return slot;
            return -1;
        }

        int FindFreeSlot() {
            for (int slot = 0; slot < kNumSlots; ++slot)
                if (g_slotActor[slot] == 0) return slot;
            return -1;
        }

        // --- co-save pending set, parsed by Load, consumed (logged only) by
        // ApplyPending. See AliasPool.h: this is a diagnostic mirror, not the
        // reset mechanism. ---
        struct Pending { std::uint32_t slot; RE::FormID actor; };
        std::vector<Pending> g_pending;

    }

    void ResolveForms() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[aliaspool] VR runtime -- ForceRefTo/CreateRefHandleByAliasID reloc ids "
                         "are SE/AE only; the alias-drive pool will never claim a slot.");
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[aliaspool] TESDataHandler unavailable -- APMF.esl forms unresolved");
            return;
        }
        g_claimQuest = dh->LookupForm<RE::TESQuest>(kLocalClaimQuest, kPlugin);
        if (!g_claimQuest) {
            spdlog::error("[aliaspool] MISSING APMF_ClaimQuest (0x{} in {}) -- is APMF.esl "
                         "installed and enabled? The alias-drive path is disabled; ch.9 still "
                         "works for actors ALREADY alias-sourced by some other framework.",
                         apmf::log::Hex(kLocalClaimQuest, 3), kPlugin);
            return;
        }
        spdlog::info("[aliaspool] resolved APMF_ClaimQuest -> 0x{}", apmf::log::Hex(g_claimQuest->GetFormID()));

        g_placeholderPackage = dh->LookupForm<RE::TESPackage>(kLocalPlaceholderPackage, kPlugin);
        if (!g_placeholderPackage)
            spdlog::warn("[aliaspool] MISSING APMF_PlaceholderPackage (0x{} in {}) -- diagnostic "
                         "only, not otherwise used at runtime", apmf::log::Hex(kLocalPlaceholderPackage, 3), kPlugin);
        else
            spdlog::info("[aliaspool] resolved APMF_PlaceholderPackage -> 0x{}",
                         apmf::log::Hex(g_placeholderPackage->GetFormID()));

        int found = 0;
        for (int slot = 0; slot < kNumSlots; ++slot)
            if (AliasByID(static_cast<std::uint32_t>(slot))) ++found;
        if (found != kNumSlots)
            spdlog::error("[aliaspool] APMF_ClaimQuest carries {} of {} expected alias slots -- "
                         "ESP/DLL slot-count mismatch?", found, kNumSlots);
        else
            spdlog::info("[aliaspool] confirmed all {} pool slots present", kNumSlots);
    }

    void EnsureEvictMarker() {
        if (EvictMarkerRef()) return;   // already minted this session
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* base = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x0000003B);   // vanilla XMarker
        if (!base) {
            spdlog::warn("[aliaspool] XMarker 0x3B missing -- eviction falls back to the player");
            return;
        }
        auto ref = player->PlaceObjectAtMe(base, true);   // force-persist: handle must survive cell unload
        if (ref) {
            g_evictMarker = ref->GetHandle();
            spdlog::info("[aliaspool] eviction marker minted 0x{}", apmf::log::Hex(ref->GetFormID()));
        }
    }

    bool ClaimSlot(RE::FormID actorID, RE::Actor* actor) {
        if (!actorID || !actor) return false;
        if (REL::Module::IsVR()) return false;
        auto* quest = g_claimQuest;
        if (!quest) return false;

        if (int already = FindSlotFor(actorID); already >= 0) return true;   // idempotent

        const int slot = FindFreeSlot();
        if (slot < 0) {
            spdlog::error("[aliaspool] 0x{}: no free pool slot (all {} claimed) -- alias-drive "
                         "declined this actor; ch.9's 0x49 hook has nothing to override for him "
                         "unless some OTHER framework already put him on the alias ladder",
                         apmf::log::Hex(actorID), kNumSlots);
            return false;
        }

        bool filled = ForceRefToNative(quest, static_cast<std::uint32_t>(slot), actor);
        if (filled) {
            spdlog::info("[aliaspool] 0x{}: pool slot {} filled NATIVELY (synchronous)",
                         apmf::log::Hex(actorID), slot);
        } else if (DispatchAlias("ForceRefTo", actor, static_cast<std::uint32_t>(slot))) {
            filled = true;
            spdlog::info("[aliaspool] 0x{}: pool slot {} fill DISPATCHED via VM (async)",
                         apmf::log::Hex(actorID), slot);
        }
        if (!filled) {
            spdlog::error("[aliaspool] 0x{}: pool slot {} fill FAILED on both routes",
                         apmf::log::Hex(actorID), slot);
            return false;
        }
        g_slotActor[slot] = actorID;
        return true;
    }

    void ReleaseActor(RE::FormID actorID, RE::Actor* actor) {
        (void)actor;   // the evict target is the session marker, not this actor -- kept for a
                        // future EvaluatePackage-on-evict nudge symmetry with ClaimSlot's caller
        const int slot = FindSlotFor(actorID);
        if (slot < 0) return;   // no-op: never claimed a slot (e.g. already alias-sourced elsewhere)

        auto* quest = g_claimQuest;
        auto* ev = EvictionRef();
        bool released = false;
        if (quest && ev) {
            if (ForceRefToNative(quest, static_cast<std::uint32_t>(slot), ev)) {
                released = true;
            } else if (DispatchAlias("Clear", nullptr, static_cast<std::uint32_t>(slot))) {
                released = true;
            }
        }
        if (released)
            spdlog::info("[aliaspool] 0x{}: pool slot {} released (evicted to marker)",
                         apmf::log::Hex(actorID), slot);
        else
            spdlog::error("[aliaspool] 0x{}: pool slot {} release FAILED on both routes -- "
                         "will self-heal on the next ReleaseAll sweep", apmf::log::Hex(actorID), slot);
        g_slotActor[slot] = 0;   // drop our bookkeeping regardless -- ReleaseAll's alias-occupancy
                                 // scan is the backstop if the engine-side evict above failed
    }

    void ReleaseAll(const char* why) {
        auto* quest = g_claimQuest;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ev = EvictionRef();
        const bool haveMarker = ev && ev != player;   // a real non-actor to displace with

        if (quest) {
            // Scan by ALIAS OCCUPANCY across all 16 slots, not just the in-
            // process table -- a save from an earlier session (or before an
            // actor's release fully landed) can hold a fill this session's
            // table never learned about (mirrors MFO Packages.cpp ReleaseAll's
            // P7 per-slot loot sweep).
            for (int slot = 0; slot < kNumSlots; ++slot) {
                RE::ObjectRefHandle h{};
                quest->CreateRefHandleByAliasID(h, static_cast<std::uint32_t>(slot));
                auto* held = h.get() ? h.get().get() : nullptr;
                if (held && held->As<RE::Actor>() && (held != player || haveMarker)) {
                    bool evicted = false;
                    if (ev && ForceRefToNative(quest, static_cast<std::uint32_t>(slot), ev)) {
                        evicted = true;
                    } else if (DispatchAlias("Clear", nullptr, static_cast<std::uint32_t>(slot))) {
                        evicted = true;
                    }
                    spdlog::info("[aliaspool] {} -- pool slot {} held 0x{}{}; {}",
                                 why, slot, apmf::log::Hex(held->GetFormID()),
                                 held == player ? " (the PLAYER)" : "",
                                 evicted ? "evicted" : "EVICT FAILED (both routes)");
                }
            }
        }
        for (auto& id : g_slotActor) id = 0;
    }

    void Save(SKSE::SerializationInterface* intf) {
        if (!intf->OpenRecord(kRecordType, kRecordVersion)) {
            spdlog::warn("[aliaspool] OpenRecord failed -- live pool slots NOT co-saved this save.");
            return;
        }
        std::uint32_t count = 0;
        for (auto id : g_slotActor) if (id) ++count;
        bool ok = intf->WriteRecordData(&count, sizeof(count));
        for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(kNumSlots); ++slot) {
            if (!g_slotActor[slot]) continue;
            ok = ok && intf->WriteRecordData(&slot, sizeof(slot));
            ok = ok && intf->WriteRecordData(&g_slotActor[slot], sizeof(g_slotActor[slot]));
        }
        if (!ok) spdlog::error("[aliaspool] WriteRecordData failed mid-record -- co-save may be truncated.");
        else     spdlog::info("[aliaspool] co-saved {} live pool slot(s) (diagnostic mirror only).", count);
    }

    void Load(SKSE::SerializationInterface* intf, std::uint32_t version) {
        if (version > kRecordVersion) {
            spdlog::warn("[aliaspool] co-save record v{} newer than known v{} -- skipping.", version, kRecordVersion);
            return;
        }
        std::uint32_t count = 0;
        if (!intf->ReadRecordData(&count, sizeof(count))) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t slot = 0;
            RE::FormID    formID = 0;
            if (!intf->ReadRecordData(&slot, sizeof(slot)))     break;
            if (!intf->ReadRecordData(&formID, sizeof(formID))) break;
            RE::FormID resolved = 0;
            if (intf->ResolveFormID(formID, resolved))
                g_pending.push_back(Pending{ slot, resolved });
        }
        spdlog::info("[aliaspool] read {} co-saved live pool slot(s) (record v{}) -- informational; "
                     "ReleaseAll's post-load sweep is the actual reset.", g_pending.size(), version);
    }

    void ApplyPending() {
        for (const auto& p : g_pending)
            spdlog::info("[aliaspool] post-load: slot {} was live at save time (held 0x{}) -- "
                         "swept by the post-load ReleaseAll reconcile, not restored (eviction, "
                         "never restoration, is correct here).", p.slot, apmf::log::Hex(p.actor));
        g_pending.clear();
    }

    void Revert() {
        for (auto& id : g_slotActor) id = 0;
        g_pending.clear();
    }

}
