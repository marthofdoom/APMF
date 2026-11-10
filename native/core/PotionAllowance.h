#pragma once
#include "APMF_API.h"

#include <cstdint>

// ============================================================================
// core/PotionAllowance -- ch.30 POTION ALLOWANCE (kIntent_PotionAllowance, ABI v37, ClickUp 1dfcuabcqpf,
// 2026-11-10). A deny predicate over the potions an NPC may DRINK, declared without any cast claim. See
// PotionAllowance.cpp's header for the engine path, the evidence and every verified site.
//
// THE SEATS.
//   SELECTION  write_vfunc slot 0x0F (CheckShouldEquip) on the 15 CombatInventoryItemMagicT<Potion, *> vtables.
//              The engine answers first; a YES for a potion the winning claim's predicate matches becomes NO.
//   DRINK      write_vfunc slot 0x10F (DrinkPotion) on the Character vtable. A matching potion is refused by
//              returning false WITHOUT calling the engine (nothing cast, nothing removed). The source is named
//              from the return address: the DrinkPotion leaf (Engine), the equip apply step (the EquipObject
//              caller below it, or Engine), anything else (the module that called).
//
// THREADING. Both thunks run on whatever thread the engine runs them on (the combat AI's update, the main
// thread for an equip). They read the ControlMap RCU snapshot, the published declaration snapshot (an atomic
// shared_ptr), the item's own record data (immutable after load), and take one LEAF mutex for the per-actor
// counters (nothing called under it). Set / Forget / GetState / RefuseReason / ResetAll: any thread.
// Install: the MAIN thread at kDataLoaded.
// ============================================================================

namespace apmf::potionallowance {

    // kDataLoaded, MAIN thread. [PotionAllowance] bPotionAllowance (default 1). Exactly 1.6.1170 / 1.5.97, VR and
    // 1.7.104 refused by name, every SeatVerified row checked before anything is written. Refusal = logged,
    // nothing written, every ch.30 claim refused.
    void Install();
    bool Installed();
    const char* NotInstalledReason();

    // ControlMap::EnqueueRequest: why a kIntent_PotionAllowance request must be refused (nullptr = accept).
    const char* RefuseReason(RE::FormID actor);

    // ClientAPI: a kIntent_PotionAllowance Request/RequestEx returned `h`. Only such a handle may declare.
    void NoteClaim(APMF_API::Handle h);
    // APMF_API_v37::SetPotionAllowance. Copies `a` synchronously; nullptr clears. Any thread.
    void Set(APMF_API::Handle h, const APMF_API::APMF_PotionAllowance* a);
    void Forget(APMF_API::Handle h);    // a client Release (a no-op for a non-ch.30 handle)
    void ResetAll(const char* why);     // revert / kPreLoadGame: declarations and counters
    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_PotionAllowanceInfo* out);

}
