#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 8 -- CASTING (SELECTION). ARBITRATION + claim lifecycle ONLY, with
// enforcement one layer down in the T2 allowance hooks (core/CastGate.cpp T2c
// CheckCast, core/EquipGate.cpp T2a CheckShouldEquip -- Docs/ALLOWANCE-
// TEMPLATE.md §3/§7). A claim here means: "while I hold this, the ONLY spell
// this actor's AI may select and charge is `param.form`." APMF makes no engine
// write for it; the client's own AI casts, and APMF narrows the alternatives.
//
// +ACT IS RETIRED (feat/ai-cast-seats-impl, marth 2026-09-05). This channel
// briefly carried an opt-in `ival` bit (`kActFlag_Drive`) that handed the claim
// to core/CastExecutor.cpp, which equipped + animated + fired the cast itself
// and fell back to `CastSpellImmediate` when it could not. That whole drive is
// gone. A cast that must actually HAPPEN is now a ch.8b `kIntent_Cast` claim,
// which the ENGINE SEATS (core/CastSeats.cpp + core/EquipGate.cpp) turn into the
// NPC's own native animated cast at the claimed target -- strictly better on
// every axis that mattered (real animation, real charge/fire/concentration, real
// magicka and interrupt handling, no equip race with the AI, no manufactured
// effect application).
//
// So this channel is back to exactly the shape it had before +ACT: log-only, no
// Tick, no engine write, `ival`/`target`/`pos` accepted and ignored (they stay
// RESERVED in APMF_API.h so a client that still sets them is byte-compatible and
// simply gets gate-only behaviour). MFO's offense gambit, which only ever used
// the gate-only mode, is unaffected.
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

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting facet CLAIMED (spell 0x{}) -- gate-only: the client's own AI "
                         "casts; APMF narrows/denies competitors. For a cast APMF should MAKE happen, use "
                         "kIntent_Cast (ch.8b).", apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting claim RE-POINTED (spell 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.8] 0x{} casting facet released.", apmf::log::Hex(id));
        }
        // No Tick: the narrowing lives entirely in the gate consults (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
