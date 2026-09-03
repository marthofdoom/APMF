#pragma once

// T2c -- CheckCast allowance (the hard pre-charge cast gate). See CastGate.cpp
// for the design; Docs/ALLOWANCE-TEMPLATE.md §3/§7.
namespace apmf::castgate {
    // Patch VTABLE_ActorMagicCaster[0] slot 0x0A (once). Call at kDataLoaded.
    // VR-refused. Idempotent.
    void Install();
}
