#pragma once

// T3 -- package-offer allowance (ch.9, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven AliasPkgProbe (Docs/PROBE-ALLOWANCE.md
// "Probe 2" -- PROVEN for Phases 1-2, engage/release). See PackageGate.cpp
// for the design; Docs/ALLOWANCE-TEMPLATE.md §3/§7.
namespace apmf::packagegate {

    // Patch VTABLE_Character[0] slot 0x49 (CheckForCurrentAliasPackage) once.
    // Call at kDataLoaded. VR-refused (the vtable index + EvaluatePackage
    // reloc are SE/AE only). Idempotent.
    void Install();

    // Nudge the actor's package selection to re-evaluate NOW: Address-Library
    // Actor::EvaluatePackage(actor, true, false) -- resetAI stays false,
    // never a full AI reset (mirrors the field-proven AliasPkgProbe.cpp
    // mechanism exactly). Exposed so channels/OfferPackage.cpp's
    // Engage/OnOwnerChanged/Release can make a package-offer claim take
    // effect within one eval instead of waiting for the engine's own natural
    // poll. No-op on a null actor -- callers may pass the Channel lifecycle's
    // `actor` argument straight through even when it is null.
    void EvaluatePackage(RE::Actor* a_actor);

}
