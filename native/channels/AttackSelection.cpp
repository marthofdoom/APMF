#include "PCH.h"
#include "core/AttackSelect.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 18 -- ATTACK SELECTION (kIntent_AttackSelection, ABI v34, ClickUp 86e3bw775). ARBITRATION + claim
// lifecycle ONLY. The deny is enforced one layer down, at StartAttack's weight call (core/AttackSelect.cpp,
// the #17a amendment of 2026-11-09), which reads the WINNING claim through ControlMap::TryGetOwningClaimBasis
// and its declaration from attackselect's own snapshot. This channel makes no engine write: Engage /
// OnOwnerChanged / Release only log (and Engage re-checks that the seat is still in place). The declaration
// of a released claim is dropped by ClientAPI's Release.
// ============================================================================

namespace {

    class AttackSelectionChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "attack-selection"; }
        int              ChannelNo() const override { return 18; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_AttackSelection; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& /*param*/) override {
            const bool intact = apmf::attackselect::SiteIntact();
            spdlog::info("[ch.18] 0x{} attack selection ENGAGED (the declared classes are denied at the engine's "
                         "weighted draw once SetAttackMix declares them){}.",
                         apmf::log::Hex(id), intact ? "" : " -- WARNING: the seat's call site was re-patched by another DLL");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& /*param*/) override {
            spdlog::info("[ch.18] 0x{} attack selection: a different claim now owns the channel.", apmf::log::Hex(id));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.18] 0x{} attack selection released: every attack keeps the engine's weight.",
                         apmf::log::Hex(id));
        }
        // No Tick: the deny lives entirely in the weight seat (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(AttackSelectionChannel);
