#pragma once

// ============================================================================
// ch.8b -- THE REANIMATE CORPSE SEAT (ABI v42, kCastFlag_CorpseTarget, MFO card G7).
//
// A Reanimate spell is aimed by its OWN caster type, never by the shared seats in
// core/CastSeats.cpp: CombatMagicCasterReanimate keeps the corpse it will raise in
// `reanimateTarget` (+0x28), and two of its vfuncs choose that corpse themselves:
//
//   0x06 CheckStartCast  (1.6.1170 0x822490 / 1.5.97 0x7860F0 / 1.7.104 0x837980)
//        keeps the cached corpse while it still resolves, is dead, is not disabled
//        or deleted, and passes the engine's own validity test (1.6.1170 0x81E290:
//        the effect's conditions with the corpse as subject, then the archetype's
//        own add test). Otherwise it runs the corpse SEARCH and writes the result to
//        +0x28 and to aimController(+0x20)+0x30. Returns "a corpse was found".
//   0x0D SetupAimController (0x822680 / 0x7862F0 / 0x837B70)
//        ALWAYS runs the search again, then writes +0x28, +0x20 = the aim
//        controller, aim+0x30 = the corpse and aim+0x3C |= 0x200.
//   0x0A GetMagicTarget   (0x8222C0 / 0x785F60 / 0x8377B0) returns {+0x28, null}.
//
// The search (0x822890 / 0x7864E0 / 0x837D80) takes the HIGHEST LEVEL valid corpse in
// range of the caster. So a claim with target 0 (ABI 19) casts at the engine's pick.
//
// With kCastFlag_CorpseTarget the claim's target IS the corpse, and these two seats
// make the engine's own machinery use it:
//   0x06: the claim's corpse is written to +0x28 BEFORE the engine runs, so the
//         engine's own keep test judges it. Kept -> the engine's YES, and aim+0x30 is
//         set as the search branch would have set it. Replaced (the engine's test
//         refused it and its search ran) -> the engine's pick is removed again (+0x28
//         and aim+0x30 back to 0, what the engine writes when it finds none) and the
//         answer is NO. The engine's pick never casts while the claim stands.
//   0x0D: engine first, then +0x28 and aim+0x30 are rewritten to the claim's corpse
//         if it still resolves, is dead and is not disabled or deleted. If it does
//         not, both are zeroed (no corpse) instead of leaving the engine's pick.
// No function is called by address and no call site is patched: two vtable slots on
// the verified CombatMagicCasterReanimate vtable. Slot contents and every offset above
// were read from the 1.6.1170, 1.5.97 and 1.7.104 executables (identical offsets on
// all three; Docs/VERIFIED-ADDRESSES.md, Docs/CHANNEL-MAP.md row 8b).
//
// THREADING: combat thread. One lock-free RCU ControlMap read per call (through
// castseats::MatchClaim), no follower list, no form lookup, CombatController members
// below +0x68 only.
//
// CONFIG: Data/SKSE/Plugins/APMF.ini [CastSeats] EnableReanimateSeat=0|1 (default 1).
// Off (or refused) -> a kCastFlag_CorpseTarget request is REFUSED at the call.
// ============================================================================

namespace apmf::reanimateseat {

    // Install both seats. kDataLoaded, after castseats::Install. VR and any build that is not
    // exactly 1.6.1170 / 1.5.97 / 1.7.104 are refused; both seats install or neither does.
    void Install();

    // True once both seats are installed. Any thread.
    bool Installed();

    // Why Installed() is false, for the refusal line. Any thread.
    const char* NotInstalledReason();

}
