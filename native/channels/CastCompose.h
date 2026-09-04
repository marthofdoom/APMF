#pragma once
#include "APMF_API.h"

// ============================================================================
// ch.8b -- CAST EXECUTION (design.md §3). The channel itself is log-only and
// self-registers in CastCompose.cpp (like CastingSelect.cpp); the whole effect
// of a kIntent_Cast claim lives in the three gate consults (CastGate 0x0A,
// EquipGate 0x0F, ActionGate T1 cast leaves) plus the ControlMap TTL pass.
//
// This header exposes ONLY the one core-callable helper: the FromPackage
// extraction. core/ControlMap.cpp's ApplyRequest calls it on the WRITER thread
// (form lookups legal there) when a kIntent_Cast claim carries
// kCastFlag_FromPackage. It reads ONLY the cast portion (spell + target) out of
// the package -- APMF never runs, offers, installs, or evaluates the package
// (design.md §1a / §3.7). Lives here (not core) so the cast-domain knowledge
// stays with the cast channel; core depends on this one decl, nothing more.
// ============================================================================

namespace apmf::castcompose {

    // Extract the cast portion of a package handed in via kCastFlag_FromPackage.
    // `pkgForm` is a TESPackage FormID. On success, writes the package's spell input
    // to `outSpell` (and its target form to `outTarget`, 0 if none/alias) and returns
    // true. On failure (unreadable / no spell input / unsupported in this build)
    // returns false WITHOUT touching the package -- the caller then REFUSES the claim
    // (never a package run). NEVER offers/installs/evaluates/runs the package: this is
    // a pure READ of two fields. See CastCompose.cpp for the pinned-CommonLib fallback
    // (design.md §5.1/§6): if the package-data read is not cleanly expressible in the
    // pinned CommonLib, it returns false so the client passes the spell directly.
    bool ExtractFromPackage(RE::FormID pkgForm, RE::FormID& outSpell, RE::FormID& outTarget);

}
