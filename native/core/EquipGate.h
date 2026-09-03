#pragma once

// T2a -- CheckShouldEquip allowance (the per-item combat equip gate). See
// EquipGate.cpp for the design; Docs/ALLOWANCE-TEMPLATE.md §3/§7.
namespace apmf::equipgate {
    // Patch the 30 concrete spell/staff CombatInventoryItem vtables' slot 0x0F
    // (once). Call at kDataLoaded. VR-refused. Idempotent.
    void Install();
}
