#include "PCH.h"
#include "core/Log.h"
#include "core/PackageData.h"

// ============================================================================
// PORTED VERBATIM IN SUBSTANCE from MFO's field-run accessor
// (marth-follower-overhaul `native/Packages.cpp:25-81` for the layout proof,
// `:422-484` FindInput, `:655-676` ReadLocation, `:680-691`
// SetAPMFLootTravelTarget). The derivation below is REPRODUCED rather than
// summarized, because the whole memory-safety argument lives in it and a
// summary is not a proof. Symbols re-verified against the pinned CommonLib
// (3.7.0 @ c4ab853d) on 2026-09-22 -- see the static_asserts.
//
// ── LAYOUT: the package-data classes CommonLib does NOT define ──
//
// A templated package's inputs are `IPackageData*` entries in
// `TESCustomPackageData::data` (a `BGSPackageDataList`). The concrete classes
// behind those pointers are `BGSPackageDataPointerTemplate<...>` specializations,
// and the pinned CommonLib defines almost none of them -- only their RTTI ids.
// So the offset of the pointer they carry had to be RECOVERED rather than read.
// Getting it wrong is a silent memory stomp, so the derivation is written down.
//
// Recovered (by MFO) from the game binary's MSVC RTTI ClassHierarchyDescriptor +
// BaseClassArray. The classes DO NOT all have the same shape:
//
//   BGSPackageDataTargetSelector   COL attributes=0 (SINGLE inheritance)
//     object: [IPackageData vptr @00][data @08][pointer @10]        sizeof 0x18
//
//   BGSPackageDataRef              COL attributes=1 (MULTIPLE inheritance)
//     object: [prim vptr @00][IPackageData vptr @08][data @10][pointer @18]
//                                                                  sizeof 0x20
//
// SO THE MEMBER OFFSETS DIFFER -- 0x10 vs 0x18 -- AND YET ONE CONSTANT IS
// CORRECT FOR BOTH. That is not luck and it is the subtle part: the array we
// read these out of is typed `IPackageData** data`, so what is stored is a
// pointer to the IPackageData SUBOBJECT, already base-adjusted by the compiler
// that stored it. (It has to be: every use is a virtual call through
// IPackageData's vtable -- GetTypeName() below is such a call, and it returns the
// right string.) For the multiple-inheritance case that pointer is object+0x08,
// exactly the 0x08 by which the members are pushed down. THE ADJUSTMENT CANCELS
// THE EXTRA BASE, because in both layouts `data` immediately follows the
// IPackageData vptr and `pointer` immediately follows `data`:
//
//     TargetSelector:  IPackageData* = obj+0x00, pointer = obj+0x10
//     Ref:             IPackageData* = obj+0x08, pointer = obj+0x18
//                                   => pointer = IPackageData* + 0x10 in BOTH
//
// CROSS-CHECKED against the two layouts CommonLib DOES assert at the pin, which
// agree exactly -- and which this file re-asserts below so a CommonLib bump that
// moved either one fails the BUILD instead of the game:
//   RE/B/BGSPackageDataBool.h      offsetof(data)==0x08, sizeof==0x10
//                                  (the single-inheritance shape)
//   RE/B/BGSPackageDataLocation.h  sizeof==0x20, parent
//                                  IPackageDataAIWorldLocationHandle asserted at
//                                  0x10 -- the multiple-inheritance shape,
//                                  pointer @0x18, i.e. IPackageData* + 0x10 again.
//
// PER-RUNTIME NOTE (CLAUDE.md principle 11). Nothing in this file resolves an
// Address-Library id or a vtable index, so there is no per-runtime literal to
// place: it is pure struct arithmetic over layouts the pinned CommonLib asserts
// identically for 1.6.1170 and 1.5.97. The 1.6/1.5 split this project cares
// about begins at the CALL sites (ch.9's 0x49 seat and EvaluatePackage), which
// already carry their own per-runtime ids.
// ============================================================================

namespace {

    // sizeof/offsetof cross-checks. If a CommonLib bump moves either shape, the
    // derivation above stops holding and the build must stop with it -- not the
    // game, at a write.
    static_assert(sizeof(RE::IPackageData) == 0x8,
                  "IPackageData is vptr-only; the +0x10 pointer model assumes it");
    static_assert(sizeof(RE::BGSPackageDataLocation) == 0x20,
                  "BGSPackageDataLocation is the MULTIPLE-inheritance shape "
                  "(pointer at obj+0x18 == IPackageData* + 0x10)");
    static_assert(sizeof(RE::BGSPackageDataBool) == 0x10 && offsetof(RE::BGSPackageDataBool, data) == 0x08,
                  "BGSPackageDataBool is the SINGLE-inheritance shape "
                  "(data at obj+0x08, so a pointer member would sit at obj+0x10)");
    static_assert(sizeof(RE::PackageLocation) == 0x18 && offsetof(RE::PackageLocation, locType) == 0x08
                      && offsetof(RE::PackageLocation, rad) == 0x0C
                      && offsetof(RE::PackageLocation, data) == 0x10,
                  "PackageLocation layout -- locType/rad/data are what SetTravelTarget writes");
    static_assert(offsetof(RE::TESCustomPackageData, data) == 0x08
                      && offsetof(RE::TESCustomPackageData, nameMap) == 0x28
                      && offsetof(RE::TESCustomPackageData, templateParent) == 0x30,
                  "TESCustomPackageData layout -- FindInput walks data/nameMap/templateParent");

    // THE recovered constant, derived above. Correct for BOTH the single- and
    // multiple-inheritance package-data shapes.
    constexpr std::size_t kPointerOffFromIPackageData = 0x10;

    // The TYPE name `IPackageData::GetTypeName()` returns for a Location carrier.
    // This is what the ESL's ANAM subrecord carries for the Location value slot
    // (APMF_GenerateESL.py `pack_input("Location", 'PLDT', ...)`).
    //
    // THIS GUARD IS A MEMORY-SAFETY GUARD, not a nicety. The offset above is only
    // meaningful for a BGSPackageDataPointerTemplate; a BGSPackageDataBool is 0x10
    // bytes TOTAL, so reading +0x10 off one would be a read one past the end of the
    // object. An input that does not name itself a Location carrier must never be
    // read through, let alone written.
    constexpr std::string_view kTypeLocation = "Location"sv;

    // The TEMPLATE's own BNAM-declared HUMAN PARAMETER NAME for the Travel
    // template's destination input -- a COMPLETELY DIFFERENT string from the ANAM
    // TYPE above, and the one FindInput searches for (name -> uid -> slot).
    //
    // FIELD-PROVEN, NOT GUESSED. MFO shipped "Location" here for three versions
    // and it MISSED both name maps every time, so the runtime-handle write never
    // ran at all. The deck session of 2026-09-03 (Tuxborn, Cicero) dumped the
    // vanilla Travel template's (00016FAA) real parameter names verbatim --
    // "Place to Travel" (uid 0), "Ride Horse if possible?" (uid 2), "Prefer
    // Preferred Path?" (uid 4), an exhaustive 3-entry miss -- and handed over the
    // correct string. Do not "tidy" it back to "Location".
    constexpr std::string_view kInputLocation = "Place to Travel"sv;

    // The statically-known UNAM uid of the Location input on the vanilla Travel
    // template. LAST RESORT ONLY -- used when BOTH name maps miss the name. Safe
    // even if wrong, because ReadLocation independently validates the resolved
    // slot's reported TYPE NAME before anything is written through it: a bad uid
    // resolves to a slot that declines loudly rather than a silent stomp.
    //
    // A DELIBERATE DIVERGENCE FROM THE MFO ORIGINAL, named here rather than left to
    // be discovered. MFO's `FindInput` carries static fallback uids for the UseMagic
    // template's SPELL (3) and Target (4) inputs and NONE for a Location, so on a
    // both-maps miss MFO's Location path returns null and declines outright. This
    // port adds uid 0 for the Travel template's "Place to Travel", dumped from
    // Skyrim.esm and confirmed by the same 2026-09-03 deck name-map dump that produced
    // the parameter name. It is strictly additive -- the type-name guard still stands
    // in front of every write -- but it means the two implementations can behave
    // DIFFERENTLY on a both-maps miss, which is worth knowing when comparing their
    // logs. (Fable tier-3 on 2d6108f, SEV-5 #3; Docs/REVIEW-BACKLOG.md APMF-B14.)
    constexpr std::int8_t kUidLocationTravel = 0;

    RE::TESCustomPackageData* CustomData(RE::TESPackage* a_pkg) {
        if (!a_pkg || !a_pkg->data) return nullptr;
        return skyrim_cast<RE::TESCustomPackageData*>(a_pkg->data);
    }

    // One diagnostic dump of what a name map ACTUALLY contains -- emitted only on
    // the both-maps-missed path, once per session, so the next deck log settles the
    // runtime shape instead of leaving us to infer it (MFO's own fix for exactly
    // this bug class).
    void DumpNameMap(const char* a_which, RE::BGSPackageDataNameMap* a_maps) {
        if (!a_maps) {
            spdlog::error("[pkgdata]   {} name map: NULL", a_which);
            return;
        }
        std::string names;
        for (const auto& nm : a_maps->nameMap) {
            names += std::format("{}'{}'={}", names.empty() ? "" : ", ",
                                 nm.name.empty() ? "<empty>" : nm.name.c_str(),
                                 static_cast<int>(nm.uid));
        }
        spdlog::error("[pkgdata]   {} name map ({} entr{}): {}", a_which,
                      a_maps->nameMap.size(), a_maps->nameMap.size() == 1 ? "y" : "ies",
                      names.empty() ? "<none>" : names);
    }

    // Resolve a template input NAME to the live IPackageData* that fills it. Two
    // indirections, both of which are the engine's own:
    //
    //   nameMap : BNAM name  -> UNAM uid   (the TEMPLATE's declaration)
    //   uids[i] : slot i     -> UNAM uid   (which input THIS instance fills)
    //
    // so name -> uid -> slot -> data[slot].
    //
    // Instance map FIRST, then the template's ON A MISS -- not only when the
    // instance's map POINTER is null. That distinction is the MFO v1.0.27-31 bug:
    // an instance's nameMap is non-null at runtime yet declares only input TYPES,
    // while the human parameter NAMES live on the vanilla template, so a
    // null-pointer-only fallback missed every time and the write never ran.
    RE::IPackageData* FindInput(RE::TESPackage* a_pkg, std::string_view a_name) {
        auto* cpd = CustomData(a_pkg);
        if (!cpd) {
            spdlog::error("[pkgdata] package 0x{} is not a templated (custom-data) package -- "
                          "refusing to look for input '{}'",
                          apmf::log::Hex(a_pkg ? a_pkg->GetFormID() : 0), a_name);
            return nullptr;
        }

        auto lookup = [&](RE::BGSPackageDataNameMap* a_maps) -> std::int8_t {
            if (!a_maps) return -1;
            for (const auto& nm : a_maps->nameMap) {
                if (!nm.name.empty() && a_name == std::string_view(nm.name.c_str())) return nm.uid;
            }
            return -1;
        };

        auto* instMaps = cpd->nameMap.get();
        RE::BGSPackageDataNameMap* tplMaps = nullptr;
        if (cpd->templateParent) {
            if (auto* tpl = CustomData(cpd->templateParent)) tplMaps = tpl->nameMap.get();
        }

        std::int8_t uid = lookup(instMaps);
        if (uid < 0) uid = lookup(tplMaps);

        if (uid < 0) {
            // BOTH missed. Say exactly what each map held -- ONCE -- so the next deck
            // session confirms the runtime shape, then fall back to the statically
            // known template uid. ReadLocation's type-name guard keeps a wrong uid
            // from ever being written through.
            static std::atomic<bool> s_dumped{ false };
            if (!s_dumped.exchange(true)) {
                spdlog::error("[pkgdata] input '{}' missing from BOTH name maps on package 0x{} -- "
                              "falling back to the static template uid ({}). Maps as found:",
                              a_name, apmf::log::Hex(a_pkg->GetFormID()),
                              static_cast<int>(kUidLocationTravel));
                DumpNameMap("instance", instMaps);
                DumpNameMap("template", tplMaps);
            }
            if (a_name == kInputLocation) {
                uid = kUidLocationTravel;
            } else {
                spdlog::error("[pkgdata] template input '{}' is not declared on package 0x{} and has "
                              "no static fallback uid", a_name, apmf::log::Hex(a_pkg->GetFormID()));
                return nullptr;
            }
        }

        // Find which of OUR value slots carries that uid. BY UID, never by index:
        // the value list is positional on disk, but UNAM[i] labels which template
        // input each slot fills, so the uid is the stable key.
        if (!cpd->data.data || !cpd->data.uids) return nullptr;
        for (std::uint16_t i = 0; i < cpd->data.dataSize; ++i) {
            if (cpd->data.uids[i] != uid) continue;
            return cpd->data.data[i];
        }
        spdlog::error("[pkgdata] input '{}' (uid {}) is declared but package 0x{} supplies no value "
                      "for it", a_name, static_cast<int>(uid), apmf::log::Hex(a_pkg->GetFormID()));
        return nullptr;
    }

    // Recover the PackageLocation* a Location input carries, guarded by its own
    // reported type name.
    RE::PackageLocation* ReadLocation(RE::IPackageData* a_pd) {
        if (!a_pd) return nullptr;

        const auto&            tn   = a_pd->GetTypeName();
        const std::string_view name = tn.empty() ? std::string_view{} : std::string_view(tn.c_str());
        if (name != kTypeLocation) {
            spdlog::error("[pkgdata] Location input reports type '{}' -- not a PackageLocation carrier; "
                          "refusing to read through it",
                          name.empty() ? "<unnamed>"sv : name);
            return nullptr;
        }

        constexpr std::size_t off = kPointerOffFromIPackageData;
        auto* pl = *reinterpret_cast<RE::PackageLocation* const*>(
            reinterpret_cast<const unsigned char*>(a_pd) + off);
        if (!pl) {
            spdlog::error("[pkgdata] Location input '{}' has a null PackageLocation at +0x{}",
                          name, apmf::log::Hex(off, 1));
            return nullptr;
        }
        return pl;
    }

}

namespace apmf::packagedata {

    bool SetTravelTarget(RE::TESPackage* a_pkg, RE::TESObjectREFR* a_ref, float a_radius) {
        if (!a_pkg || !a_ref) return false;

        auto* locPd = FindInput(a_pkg, kInputLocation);
        if (!locPd) return false;
        auto* loc = ReadLocation(locPd);
        if (!loc) return false;

        const int authored = static_cast<int>(loc->locType.get());

        // Both guards passed -- write. Nothing above this line mutated anything, so
        // a decline is never a half-mutated package.
        loc->locType        = RE::PackageLocation::Type::kNearReference;
        loc->data.refHandle = a_ref->CreateRefHandle();
        loc->rad            = static_cast<std::uint32_t>(a_radius);

        spdlog::info("[pkgdata] package 0x{} Location -> ref 0x{} radius {} (authored locType {} -> "
                     "kNearReference).",
                     apmf::log::Hex(a_pkg->GetFormID()), apmf::log::Hex(a_ref->GetFormID()),
                     static_cast<std::uint32_t>(a_radius), authored);
        return true;
    }

}
