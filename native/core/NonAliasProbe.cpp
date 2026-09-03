#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/NonAliasProbe.h"

#include <chrono>

// ============================================================================
// See NonAliasProbe.h for the design/why. Implementation notes:
//
// * `Actor::GetCurrentPackage()` (a plain, non-virtual member function --
//   Docs/PROBE-NONALIAS-PACKAGE.md §2 confirms it exists on `Actor.h` with no
//   `override`/index comment) is used here and by core/PackageGate.cpp's
//   observe-log addition to read "the actor's current package" rather than
//   hand-walking `AIProcess::currentPackage`'s raw struct layout (§2: a plain
//   `ActorPackage currentPackage` data member at `AIProcess+0x18` with NO
//   documented field names in the pinned header set) -- calling the engine's
//   own accessor is the safe choice; guessing an internal struct's byte
//   layout is exactly the kind of blind-offset hack the brief bans.
//
// * RTTI type-name resolution (DumpActor/ResolveTypeName) reads
//   CompleteObjectLocator::typeDescriptor directly (a Ptr RVA at +0x0C onto
//   the object's own most-derived RE::msvc::type_info -- simpler than
//   core/Allowance.cpp's `DerivesFrom`, which additionally walks
//   ClassHierarchyDescriptor->baseClassArray because IT is searching for one
//   specific BASE among possibly several; here the COL's own direct
//   typeDescriptor already names the exact class), then calls
//   `type_info::mangled_name()` -- the raw MSVC-decorated name (e.g.
//   ".?AVCharacter@@"), NOT demangled (no demangler exists in this codebase
//   or CommonLib; hand-rolling one would be exactly the "fragile hack" the
//   brief says to avoid -- the raw decorated string is still legible enough
//   to identify the class by eye). Both this and the `procedureType`/
//   `GetCurrentPackage()` choices above were corrected once against the real
//   CommonLibSSE-NG headers (fetched from the pinned commit,
//   c4ab853d095e81e3390b282d7ba01ab2f24ebf25) after a first CI compile
//   failure named the wrong symbols (`GetPackageType()`,
//   `TypeDescriptor::name`) -- no local vcpkg cache exists to check against
//   up front (Docs/PROBE-NONALIAS-PACKAGE.md's own "Source used" section).
// ============================================================================

namespace apmf::nonaliasprobe {

    namespace {

        constexpr std::uint32_t kToggleKey = 0x45;   // NumLock  -- see .h for why not a Numpad0-9 key
        constexpr std::uint32_t kDumpKey   = 0x46;   // ScrollLock -- one-shot vtable/RTTI dump
        constexpr std::uint64_t kRateLimitMs = 2000; // per-actor observe-log cadence

        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_debugEnabled{ false };

        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;   // 0xDF originals, keyed by vtable addr

        std::mutex                                     g_rlMx;
        std::unordered_map<RE::FormID, std::uint64_t>  g_lastLogMs;   // rate-limit table (debug-only traffic)

        std::uint64_t NowMs() {
            using namespace std::chrono;
            return static_cast<std::uint64_t>(duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count());
        }

        // ---- 0xDF -- PutCreatedPackage observe hook ----
        using PutCreatedPackage_t = void (*)(RE::Actor*, RE::TESPackage*, bool, bool, bool);
        constexpr std::size_t kPutCreatedPackage = 0xDF;   // Actor::PutCreatedPackage -- see .h; 1.6.1170-pinned
                                                            // index, no named CommonLib binding to prefer over it
                                                            // today (Docs/PROBE-NONALIAS-PACKAGE.md §2).

        void PutCreatedPackageThunk(RE::Actor* a_this, RE::TESPackage* a_package,
                                    bool a_temp, bool a_created, bool a_allowFromFurniture) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) {
                // Foreign vtable -- should never happen (only ever installed on
                // VTABLE_Character[0]), but never touch `a_this` if it does.
                return;
            }
            const auto orig = reinterpret_cast<PutCreatedPackage_t>(oit->second);

            if (g_debugEnabled.load(std::memory_order_relaxed) && a_this &&
                RateLimitOK(a_this->GetFormID())) {
                spdlog::info("[nonaliasprobe] 0xDF PutCreatedPackage actor=0x{} pkg=0x{} pkgType={} "
                             "temp={} created={} allowFromFurniture={} -- answers Docs/PROBE-NONALIAS-"
                             "PACKAGE.md §6.2 (does the framework's non-alias package ever flow through here?)",
                             apmf::log::Hex(a_this->GetFormID()),
                             apmf::log::Hex(a_package ? a_package->GetFormID() : 0),
                             // TESPackage has no GetPackageType() method (verified against the real
                             // CommonLibSSE-NG header after a first CI compile failure) -- the type
                             // lives as plain data, RE::TESPackage::procedureType (a
                             // stl::enumeration<PACKAGE_PROCEDURE_TYPE, uint32_t> at +0xD8);
                             // .underlying() reads its raw integral value.
                             a_package ? static_cast<std::int32_t>(a_package->procedureType.underlying()) : -1,
                             a_temp, a_created, a_allowFromFurniture);
            }

            orig(a_this, a_package, a_temp, a_created, a_allowFromFurniture);   // chain unconditionally, void
        }

        // ---- Deliverable 2: runtime vtable/RTTI dumper ----

        RE::Actor* CrosshairActor() {
            // Mirrors core/NativeBitProbe.cpp's CrosshairActor() exactly -- the
            // simplest actor-selection mechanism already proven in this
            // codebase (brief: "whatever's simplest and documented").
            if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                if (auto ref = pick->targetActor.get()) {
                    auto* a = ref->As<RE::Actor>();
                    if (a && !a->IsPlayerRef()) return a;
                }
            }
            return nullptr;
        }

        // Reads the RTTI type name of `vtableAddr`'s own most-derived class.
        // SIMPLER than Allowance.cpp's DerivesFrom walk needs to be: the
        // CompleteObjectLocator carries its OWN typeDescriptor as a direct RVA
        // at +0x0C (RE::RTTI::CompleteObjectLocator::typeDescriptor -- verified
        // against the real CommonLibSSE-NG RTTI.h header after a first CI
        // compile failure) -- no baseClassArray walk needed at all, unlike
        // DerivesFrom which is searching for a specific BASE among possibly
        // several. Returns nullptr (never guesses) if any link is absent.
        const char* ResolveTypeName(std::uintptr_t vtableAddr) {
            if (!vtableAddr) return nullptr;
            auto* colPtr = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(
                vtableAddr - sizeof(void*));
            if (!colPtr) return nullptr;

            auto* td = colPtr->typeDescriptor.get();
            if (!td) return nullptr;
            // RE::msvc::type_info exposes the raw MSVC-decorated name (e.g.
            // ".?AVCharacter@@") via mangled_name() -- NOT demangled (no
            // demangler exists in this codebase or CommonLib; the raw
            // decorated string is still legible enough to identify the class).
            return td->mangled_name();
        }

        void DumpActor(RE::Actor* a_actor) {
            if (!a_actor) {
                spdlog::warn("[nonaliasprobe-dump] REFUSED -- aim the crosshair at an NPC (not the "
                             "player) first.");
                return;
            }

            const auto base  = REL::Module::get().base();
            const auto vtPtr = *reinterpret_cast<std::uintptr_t*>(a_actor);
            const auto vtRva = vtPtr - base;
            const char* typeName = ResolveTypeName(vtPtr);

            spdlog::info("[nonaliasprobe-dump] actor=0x{} name='{}' vtable RVA=0x{} rttiTypeName={}",
                         apmf::log::Hex(a_actor->GetFormID()),
                         a_actor->GetName() ? a_actor->GetName() : "?",
                         apmf::log::Hex(vtRva),
                         typeName ? typeName : "<unresolved -- see NonAliasProbe.cpp ResolveTypeName>");

            // First ~0x60 slots -- module-relative RVAs, decimal slot index
            // (Log.h: NEVER `{:X}` -- the known deck hex-formatting bug; slot
            // index logged via plain decimal `{}`, addresses via Hex()).
            constexpr std::size_t kSlotCount = 0x60;
            for (std::size_t i = 0; i < kSlotCount; ++i) {
                const auto slotAddr = *reinterpret_cast<std::uintptr_t*>(vtPtr + i * sizeof(void*));
                const auto slotRva  = slotAddr - base;
                spdlog::info("[nonaliasprobe-dump]   slot {} RVA=0x{}", i, apmf::log::Hex(slotRva));
            }

            // Known slots called out by index, for an eyeball cross-check
            // against IDA / CommonLib REL offsets / the two entries above.
            const auto slot49Rva = *reinterpret_cast<std::uintptr_t*>(vtPtr + 0x49 * sizeof(void*)) - base;
            const auto slotDFRva = *reinterpret_cast<std::uintptr_t*>(vtPtr + 0xDF * sizeof(void*)) - base;
            spdlog::info("[nonaliasprobe-dump]   KNOWN slot 0x49 (CheckForCurrentAliasPackage) RVA=0x{}",
                         apmf::log::Hex(slot49Rva));
            spdlog::info("[nonaliasprobe-dump]   KNOWN slot 0xDF (PutCreatedPackage) RVA=0x{}",
                         apmf::log::Hex(slotDFRva));
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[nonaliasprobe] VR runtime -- 0xDF index is SE/AE-only verified; "
                         "PutCreatedPackage observe hook NOT installed (NumLock/ScrollLock probe "
                         "keys still armed for the dumper + 0x49 assist, which need no vtable index).");
            return;
        }
        if (g_installed.exchange(true)) return;

        REL::Relocation<void*> expectedTD{ RE::RTTI_Character };
        const REL::VariantID   kVtables[] = { RE::VTABLE_Character[0] };

        const int n = allowance::InstallOnVtables(kVtables, kPutCreatedPackage, &PutCreatedPackageThunk,
                                                   expectedTD.get(), "nonaliasprobe", g_orig);
        spdlog::info("[nonaliasprobe] OBSERVE-ONLY diagnostic armed -- PutCreatedPackage (0x{}) hooked on "
                     "{} vtable(s) (Character[0]); logs then chains unconditionally, never alters args/"
                     "return. NumLock toggles this + the 0x49 assist log (OFF by default); ScrollLock "
                     "dumps the crosshair-aimed actor's vtable/RTTI. See Docs/PROBE-NONALIAS-PACKAGE.md.",
                     apmf::log::Hex(kPutCreatedPackage, 2), n);
    }

    void OnHotkey(std::uint32_t a_code) {
        if (a_code == kToggleKey) {
            const bool was = g_debugEnabled.exchange(!g_debugEnabled.load(std::memory_order_relaxed),
                                                     std::memory_order_relaxed);
            spdlog::info("[nonaliasprobe] observe logging (0x49 assist + 0xDF) {} (NumLock).",
                         was ? "DISARMED" : "ARMED -- walk near the actor of interest (e.g. Cicero, "
                                            "0009BCB0) while its non-alias package is active; watch for "
                                            "[ch.9-observe]/[nonaliasprobe] lines.");
        } else if (a_code == kDumpKey) {
            spdlog::info("[nonaliasprobe-dump] ScrollLock -- dumping crosshair-aimed actor...");
            DumpActor(CrosshairActor());
        }
    }

    bool IsEnabled() { return g_debugEnabled.load(std::memory_order_relaxed); }

    bool RateLimitOK(RE::FormID a_actor) {
        const auto now = NowMs();
        std::scoped_lock lock(g_rlMx);
        auto& last = g_lastLogMs[a_actor];   // 0 on first sight -- logs immediately
        if (now - last < kRateLimitMs) return false;
        last = now;
        return true;
    }

}
