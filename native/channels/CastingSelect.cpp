#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/CastExecutor.h"

// ============================================================================
// Channel 8 -- CASTING (SELECTION), now with an +ACT MODE (feat/cast-act,
// marth 2026-09-05). Historically ARBITRATION + claim lifecycle ONLY, with
// enforcement one layer down in the T2 allowance hooks (core/CastGate.cpp T2c
// CheckCast, core/EquipGate.cpp T2a CheckShouldEquip -- Docs/ALLOWANCE-
// TEMPLATE.md §3/§7, Phase 2). That shape is UNCHANGED and still applies when
// a client only wants exclusivity (selects + grants its own AI consent, casts
// itself).
//
// +ACT (this pass): the SAME claim now ALSO drives the cast end-to-end --
// equip the resolved hand(s), animate the engine's own observed cast sequence,
// and guarantee delivery -- via core/CastExecutor.cpp. This is a deliberate,
// scoped exception to "APMF only arbitrates/denies, never executes" (INVARIANTS
// #0): MFO's own equivalent client-side execution code is being REMOVED, so for
// THIS ONE facet APMF becomes the sole owner of record, at the CLIENT's
// explicit request (RequestEx's `ival` names a hand -- APMF never decides to
// cast on its own, never invents a spell/target). See CastExecutor.h for the
// full contract (no ABI change: `ival`, unread by this channel before this
// pass, now carries the hand mode).
//
// `Release` restores the driven hand(s) (interrupt, teardown anims, deselect,
// release CastExecutor's internal ch.8b protection claim) -- see
// CastExecutor::Release. No `Tick` (CastExecutor's multi-frame work runs via
// core/MainThread.h's Post/Pump, not a per-actor Tick).
// ============================================================================

namespace {

    class CastingSelectChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "casting-select"; }
        int              ChannelNo() const override { return 8; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_SelectSpell; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4B, "Numpad4 : CLAIM the casting facet (arbitration only)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting facet CLAIMED (spell 0x{}, hand mode {}) -- +ACT: APMF "
                         "drives the cast (CastExecutor).", apmf::log::Hex(id),
                         apmf::log::Hex(param.form), param.ival);
            apmf::castexec::Engage(id, actor, param);
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting claim RE-POINTED (spell 0x{}, hand mode {}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form), param.ival);
            apmf::castexec::OnOwnerChanged(id, actor, param);
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            spdlog::info("[ch.8] 0x{} casting facet released.", apmf::log::Hex(id));
            apmf::castexec::Release(id, actor);
        }
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
