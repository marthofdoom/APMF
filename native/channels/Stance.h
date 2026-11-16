#pragma once

// ============================================================================
// ch.3 STANCE / SNEAK (kIntent_Stance). See channels/Stance.cpp.
//
// The two PASSIVE probes below exist to answer one open question (the 2026-11-15
// dangling-state audit, SEV-4): does a crouch Harbinger started (SneakStart on the
// animation graph only; the engine's own sneak state is not written) survive a
// save and load, after which no claim and no release confirm exists to end it?
// They log; they change nothing. A fix (if the field says yes) is its own change.
// ============================================================================

namespace apmf::stance {

    // kSaveGame: one line per actor that holds a Harbinger-started sneak right now (graph
    // variable, engine state). Game thread.
    void LogAtSave();

    // After a load (posted to the main-thread pump; samples twice, at the first pump and
    // 5 s later, because 3D loads over the first frames): every loaded NPC whose graph
    // says sneaking while the engine's own state says not sneaking -- a crouch nothing
    // owns. Up to 16 lines per sample. Game thread.
    void LogAfterLoad(const char* a_when);

}
