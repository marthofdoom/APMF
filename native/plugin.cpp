#include "PCH.h"
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Input.h"
#include "core/NativeBitProbe.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
#include "core/AvLedger.h"
#include "core/CastGate.h"
#include "core/EquipGate.h"
#include "core/ActionGate.h"
#include "core/PackageGate.h"
#include "core/NonAliasProbe.h"
#include "core/AliasPool.h"

// ============================================================================
// APMF -- AI Package Management Framework. Entry point (thin).
//
//   core/    -- the extensible spine: the 0xAD arbiter hook, the multi-NPC
//               ControlMap, the channel Registry, the Arbiter (test surface +
//               per-frame drain), the input surface, the co-saved AV ledger, and
//               the real C-ABI client API (APMF_API.h / core/ClientAPI).
//   channels/ -- one small self-registering module per directable AI facet.
// See MAP.md for the module map and Docs/ARCHITECTURE.md for the design.
// ============================================================================

namespace {

    constexpr std::uint32_t kSerUniqueID = 'APMF';   // co-save owner tag

    // --- SKSE serialization: co-save the outstanding AV overrides so a
    // save-while-engaged is never stranded across a load (INVARIANTS #15).
    // Also co-saves the alias-drive pool's live-slot snapshot (Docs/
    // SPEC-ALIAS-DRIVE.md §4) -- a diagnostic mirror, NOT the reset
    // mechanism (the alias fills persist NATIVELY in the .ess; the
    // kPreLoadGame/post-load ReleaseAll sweeps below are the real reset). ---
    void OnSave(SKSE::SerializationInterface* intf) {
        apmf::av::Save(intf);
        apmf::aliaspool::Save(intf);
    }
    void OnLoad(SKSE::SerializationInterface* intf) {
        std::uint32_t type = 0, version = 0, length = 0;
        while (intf->GetNextRecordInfo(type, version, length)) {
            if (type == apmf::av::kRecordType)        apmf::av::Load(intf, version);
            else if (type == apmf::aliaspool::kRecordType) apmf::aliaspool::Load(intf, version);
        }
    }
    void OnRevert(SKSE::SerializationInterface*) {
        // New game / pre-load wipe: drop all control WITHOUT restoring (actors are
        // being replaced) and clear the ledger; the co-save's pending set restores
        // the incoming save's overrides on kPostLoadGame. Alias-drive: clear the
        // in-process occupancy table too (no restore -- eviction, never
        // restoration, is correct for a claimed alias slot); the incoming save's
        // own alias state is reconciled by the post-load ReleaseAll sweep below,
        // independent of anything this table remembers.
        apmf::ControlMap::Get().Clear();
        apmf::av::Revert();
        apmf::aliaspool::Revert();
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            apmf::hook::Install();
            apmf::castgate::Install();           // T2c CheckCast allowance (Docs/ALLOWANCE-TEMPLATE.md)
            apmf::equipgate::Install();          // T2a CheckShouldEquip allowance
            apmf::actiongate::Install();         // T1 combat-action allowance (ch.7; VR-refused inside)
            apmf::packagegate::Install();        // T3 package-offer allowance (ch.9; VR-refused inside)
            apmf::nonaliasprobe::Install();      // OBSERVE-ONLY 0xDF hook + 0x49 assist + RTTI dumper
                                                  // (Docs/PROBE-NONALIAS-PACKAGE.md; VR-refused inside)
            apmf::nativebitprobe::Install();     // native-bit toggle probe (throwaway; no VR gate needed)
            apmf::aliaspool::ResolveForms();     // alias-drive: resolve APMF.esl's claim quest + pool
                                                  // (Docs/SPEC-ALIAS-DRIVE.md; VR-warned inside, forms
                                                  // still resolved for diagnostics)
            // T4 (TESActionData::Process) REMOVED (2026-09-03): its call-site patch at
            // valhalla's known site collided with SCAR.dll's own hook on the same AI
            // attack-start path -> execute-AV CTD in live combat, not hotkey-gated (the
            // patch was live from install). NEVER a raw call-site patch again
            // (Docs/INVARIANTS.md #17: write_vfunc ONLY). See Docs/PROBE-ALLOWANCE.md
            // "T4 -- DEFERRED" for the crash record; T1 already covers combat body-
            // commands so this is not a coverage dead end.
            //
            // T1Probe/AliasPkgProbe/ProbeClaimSet (2026-09-03): REMOVED, graduated into
            // the real ch.7 (ActionGate) / ch.9 (PackageGate) channels above -- the
            // throwaway hotkey-driven claim set is superseded by real ControlMap claims,
            // never left installed alongside the real channels on the same vtables
            // (Docs/INVARIANTS.md #17).
            if (!REL::Module::IsVR()) {          // no drain seat on VR -> no test surface
                apmf::input::Register();
                apmf::input::LogHelp();
            } else {
                spdlog::warn("[input] VR runtime -- hooks refused, input test surface NOT armed.");
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            // ch.7/ch.9 need no bespoke ClearOnPreLoad -- ReleaseAll already drops
            // every channel's claims (including these) through the generic
            // ControlMap path; unlike the old probes, these are real channels.
            // ch.9's Release (channels/OfferPackage.cpp) already evicts THIS
            // session's own live alias-pool slots as part of that generic path;
            // the explicit aliaspool::ReleaseAll below is a cheap (O(16)) defensive
            // backstop, not the primary release for this session's own claims.
            apmf::Arbiter::Get().ReleaseAll("kPreLoadGame");
            apmf::aliaspool::ReleaseAll("kPreLoadGame");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            // Alias-drive: mint the eviction marker BEFORE the reconcile sweep so
            // its evictions displace with the marker, not a player fallback --
            // mirrors MFO's own kPostLoadGame/kNewGame ordering (Packages::
            // EnsureEvictMarker before Packages::ReleaseAll). Then sweep by ALIAS
            // OCCUPANCY (not the in-process table, which is empty on a fresh load)
            // so a save written mid-claim -- by an earlier session, a crash, or a
            // save from before this session's own ControlMap existed -- never
            // leaves an actor claimed-with-nothing on a stale pool slot (Docs/
            // SPEC-ALIAS-DRIVE.md §4; the alias fill persists natively in the
            // .ess, so this reconcile is the real reset, independent of the
            // co-save record below).
            apmf::aliaspool::EnsureEvictMarker();
            apmf::aliaspool::ReleaseAll("post-load reconcile");
            if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
                apmf::av::ApplyPending();          // restore any stranded AV overrides
                apmf::aliaspool::ApplyPending();   // log the co-saved live-slot snapshot (diagnostic only)
            }
            break;
        default:
            break;
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    apmf::log::Setup();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== APMF {}.{}.{} loading -- game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID(kSerUniqueID);
        ser->SetSaveCallback(OnSave);
        ser->SetLoadCallback(OnLoad);
        ser->SetRevertCallback(OnRevert);
    }

    spdlog::info("=== APMF loaded (multi-NPC control map + channel registry + C-ABI client API) ===");
    return true;
}
