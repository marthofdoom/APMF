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
// +ACT (this pass) is OPT-IN (marth 2026-09-05, the offense-safety fix): a
// BARE claim (ival's kActFlag_Drive bit CLEAR -- including ival == 0, the
// pre-+ACT shape) stays EXACTLY the gate-only mode above -- CastExecutor is
// never even called, so MFO's offense gambit (which claims this facet only
// to narrow the AI, e.g. via CastGate/EquipGate, and wants ITS OWN AI to fire)
// is byte-identical to before this feature existed. ONLY a claim that sets
// kActFlag_Drive in `ival` gets the +ACT treatment: APMF equips the resolved
// hand(s), animates the engine's own observed cast sequence, and guarantees
// delivery -- via core/CastExecutor.cpp. This is a deliberate, scoped
// exception to "APMF only arbitrates/denies, never executes" (INVARIANTS #0):
// MFO's own equivalent client-side execution code is being REMOVED for the
// facets that opt in, so for THOSE claims APMF becomes the sole owner of
// record, at the CLIENT's explicit request (never invented). See
// core/CastExecutor.h for the full contract (no ABI change: `ival`, unread by
// this channel before this pass, now carries the hand mode + the opt-in bit).
//
// `Release` (and an OnOwnerChanged that DROPS the opt-in bit) restores any
// driven hand(s) via CastExecutor::Release -- safe to call unconditionally,
// a no-op when nothing was driving. No `Tick` (CastExecutor's multi-frame
// work runs via core/MainThread.h's Post/Pump, not a per-actor Tick).
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
            const bool drive = (param.ival & apmf::castexec::kActFlag_Drive) != 0;
            spdlog::info("[ch.8] 0x{} casting facet CLAIMED (spell 0x{}, ival {}) -- {}.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form), param.ival,
                         drive ? "+ACT: APMF drives the cast (CastExecutor)"
                               : "gate-only: the client's own AI casts; APMF narrows/denies");
            if (drive) apmf::castexec::Engage(id, actor, param);
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            const bool drive = (param.ival & apmf::castexec::kActFlag_Drive) != 0;
            spdlog::info("[ch.8] 0x{} casting claim RE-POINTED (spell 0x{}, ival {}) -- {}.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form), param.ival,
                         drive ? "+ACT" : "gate-only");
            if (drive) apmf::castexec::OnOwnerChanged(id, actor, param);
            else       apmf::castexec::Release(id, actor);   // dropped the opt-in -- restore any driven hand
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            spdlog::info("[ch.8] 0x{} casting facet released.", apmf::log::Hex(id));
            apmf::castexec::Release(id, actor);   // no-op if this claim was never driving
        }
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
