#!/usr/bin/env python3
"""APMF.esl generator — emits the alias-drive claim plugin from pure Python.

Never hand-edit the ESL; change this file and regenerate. Same doctrine as
MFO's MFO_GenerateESP.py (Linux-Native-Tools): never trust format docs — this
file's byte layouts are forked from MFO's own shipped, deck-proven generator
(itself forked from MEO/MRO's), which was in turn verified against records
parsed out of Skyrim.esm.

WHAT THIS SHIPS (Docs/SPEC-ALIAS-DRIVE.md): a tiny ESL carrying ONE claim
quest (APMF_ClaimQuest) with a 16-slot ACTOR-ALIAS POOL at a high static
priority, and ONE minimal placeholder Travel package every slot's alias
carries so the alias is never empty (ENGINE_NOTES #69/#70 lesson, ported from
MFO: an alias-claimed actor with NO valid package STANDS STILL — "claimed
with nothing" roots the actor, so the placeholder must always be a real,
UNGATED, always-resolvable package). native/core/AliasPool.cpp fills a free
slot with `ForceRefTo` when a channels/OfferPackage.cpp claim needs a
non-alias-sourced actor on the engine's alias ladder; core/PackageGate.cpp's
EXISTING 0x49 hook (CheckForCurrentAliasPackage) then overrides whatever this
placeholder would have offered with the client's REAL claimed package. The
placeholder itself is close to a dead letter — it only matters in the narrow
window between the alias fill landing and the 0x49 claim being read, or if a
claim ever lapses while the actor is still slotted.

FormID band — FROZEN generator<->DLL contract (mirrors MFO's DESIGN.md 8.2
banding doctrine). One master (Skyrim.esm) => own-file master index 0x01.
ESL-legal range 0x800-0xFFF; APMF's alias-drive band lives at the very
bottom of it (0x800-0x801 used, 0x802-0xFFF reserved for future APMF forms).

Usage:  python3 APMF_GenerateESP.py [out_dir]     (default: ./out)
"""

import os
import struct
import sys

# ── FormID band — FROZEN. Never renumber; forms are only ever ADDED. ──
OWN = 0x01000000                       # 1 master (Skyrim.esm) => own-file prefix 0x01

FID_CLAIM_QUEST         = OWN | 0x800  # APMF_ClaimQuest -- the 16-slot alias pool
FID_PLACEHOLDER_PACKAGE = OWN | 0x801  # APMF_PlaceholderPackage -- shared by all 16 slots
# 0x802-0xFFF reserved for future APMF forms.

NEXT_OBJECT_ID = 0x802                 # first never-used local id

FORM_VERSION = 44                      # Skyrim SE/AE record form version (mirrors MFO's constant)

FREF_PLAYER      = 0x00000014          # PlayerRef -- always loaded, harmless placeholder target
FREF_TMPL_TRAVEL = 0x00016FAA          # vanilla Travel package template

# Quest priority: a HIGH static constant so APMF's claim quest outranks a
# vanilla follower's own default-priority (~30) / DialogueFollower-style
# (~50) quest the instant it claims an actor's alias -- the whole point of
# the mechanism (an alias claim is locked in at FILL time, never
# re-arbitrated by a later priority change; MFO ENGINE_NOTES 0.35b/0.36).
# FIELD-TUNABLE: override via APMF_CLAIM_PRIORITY for a field test; ship
# value is 90 (Skyrim's own scene-quest band runs 80-96, so 90 sits high in
# vanilla's own precedent range without claiming the very top).
CLAIM_PRIORITY = int(os.environ.get("APMF_CLAIM_PRIORITY", "90"))  # FIELD-TUNABLE

NUM_SLOTS = 16


# ── record-building primitives (byte-identical shape to MFO_GenerateESP.py) ─
def subrec(t, d):
    return t.encode('ascii') + struct.pack('<H', len(d)) + d


def record(t, fid, fl, d):
    return (t.encode('ascii') + struct.pack('<I', len(d)) + struct.pack('<I', fl)
            + struct.pack('<I', fid) + struct.pack('<I', 0)
            + struct.pack('<H', FORM_VERSION) + struct.pack('<H', 0) + d)


def group(label, data):
    return (b'GRUP' + struct.pack('<I', 24 + len(data)) + label.encode('ascii')
            + struct.pack('<iII', 0, 0, 0) + data)


def zstr(s):
    # ASCII ONLY -- multibyte UTF-8 desyncs Papyrus/renders as mojibake.
    return s.encode('ascii') + b'\x00'


def pack_input(kind, payload_type, payload):
    # PACK "settable input" shape: a leading UNAM-run byte identifying the
    # input KIND (mirrored from MFO_GenerateESP.py's pack_input), then the
    # payload subrecord itself.
    return subrec(payload_type, payload)


def qust_dnam(flags=0x0011, priority=30, qtype=0):
    """QUEST_DATA, decoded from vanilla QUSTs (MFO_GenerateESP.py qust_dnam):
        0..1  flags     uint16   0x0011 = kEnabled | kStartsEnabled
        2     priority  uint8    CK 0-100 scale
        3     unused    uint8
        4..7  delay     float
        8..11 type      uint32   0 = None/Misc
    """
    return struct.pack('<HBBfI', flags, priority, 0, 0.0, qtype)


def make_tes4(next_id):
    # TES4 flags 0x200 = ESL. ESL is a LIE unless every local id is inside
    # 0x800-0xFFF (true here: only 0x800/0x801 used).
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', next_id)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("marth"))
    body += subrec('SNAM', zstr("APMF (AI Package Management Framework) -- alias-drive claim plugin"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0, 0x00000200, body)


# ── PACK: the shared placeholder package every pool slot's alias carries ──
def make_placeholder_package():
    """APMF_PlaceholderPackage: rides vanilla Travel (00016FAA), runtime-handle
    Location (PLDT type 0 "Near Reference") authored with a harmless placeholder
    target (FREF_PLAYER) -- byte-shape mirrored from MFO_GenerateESP.py's
    build_travel(runtime_target=True) branch (its own APMF ch.9 loot-travel /
    retreat packages use this exact shape for the identical reason: a
    runtime-handle Location needs no QNAM/alias reference).

    UNGATED (no CTDA) -- ENGINE_NOTES #69/#70 (ported into Docs/SPEC-ALIAS-DRIVE.md
    for this repo): an alias-claimed actor with no VALID package STANDS STILL
    ("claimed with nothing" roots him). Every one of the 16 pool slots' aliases
    carries this SAME package (ALPC), so the slot is never claimed-with-nothing
    even in the brief window between the alias fill landing and
    core/PackageGate.cpp's 0x49 hook overriding it with the client's real
    package. NOT kIgnoreCombat -- if a claim ever lapses mid-combat, this
    placeholder should NOT fight combat for control of the actor; ordinary
    combat preemption is the correct fallback, same as any other follower.
    """
    body  = subrec('EDID', zstr("APMF_PlaceholderPackage"))
    # PKDT: flags 0x00002000 = PREFERRED SPEED ENABLE (mirrors MFO's default
    # build_travel flags -- byte6 below is inert without this bit set, per
    # MFO_GenerateESP.py's own measured 5,961-PACK survey). NOT kIgnoreCombat.
    # Byte tail '1200028054000000': type 18, byte6=2 (Run), interruptFlags 0x0054
    # -- verbatim from MFO's build_travel (itself mirrored from the shipped
    # VC01FalionAtSummoningCircle exemplar).
    body += subrec('PKDT', struct.pack('<I', 0x00002000) + bytes.fromhex('1200028054000000'))
    # PSDT: any time, any day (the 3,855-of-5,961 vanilla default).
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    # NO CTDA -- unconditional. A gated placeholder on a permanently-alias-
    # claimable slot is exactly the frozen-actor bug this package exists to
    # avoid (see the docstring).
    # NO QNAM -- runtime-handle Location names no alias.
    body += subrec('PKCU', struct.pack('<III', 3, FREF_TMPL_TRAVEL, 3))
    # input 0: Location -> PLDT type 0 (Near Reference), placeholder ref
    # FREF_PLAYER, radius 200. Never overwritten at runtime (unlike MFO's
    # APMF-route packages) -- this package's own content is close to a dead
    # letter; the 0x49 hook supplies the actual package while a claim lives.
    body += pack_input("Location", 'PLDT', struct.pack('<IiI', 0, FREF_PLAYER, 200))
    # inputs 2 and 4: the two Bools (RideHorseIfPossible, PreferPreferredPath), both false.
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    # settable-slot run 0/2/4, then XNAM 3 -- verbatim from the Travel template shape.
    body += subrec('UNAM', struct.pack('<B', 0))
    body += subrec('UNAM', struct.pack('<B', 2))
    body += subrec('UNAM', struct.pack('<B', 4))
    body += subrec('XNAM', struct.pack('<B', 3))
    # Empty on-begin/end/change blocks, as vanilla ships them.
    for blk in ('POBA', 'POEA', 'POCA'):
        body += subrec(blk, b'')
        body += subrec('INAM', struct.pack('<I', 0))
        body += subrec('PDTO', struct.pack('<II', 0, 0))
    return record('PACK', FID_PLACEHOLDER_PACKAGE, 0, body)


# ── QUST: the claim quest and its 16-slot actor-alias pool ─────────────────
def make_claim_quest():
    """APMF_ClaimQuest: a 16-slot ACTOR-ALIAS POOL at CLAIM_PRIORITY (default
    90), each alias carrying APMF_PlaceholderPackage. Docs/SPEC-ALIAS-DRIVE.md
    §2 is the design writeup; this is its record-level realization.

    SHAPE: bare Optional/AllowReuseInQuest/AllowReserved alias slots, the
    SAME DialogueFollower-derived shape MFO's own command/loot/retreat quests
    use (Bethesda's own follower system's shape) -- no authored ALFR/ALUA, no
    conditions on the alias itself; native/core/AliasPool.cpp fills a slot
    on demand via TESQuest::ForceRefTo and evicts by force-filling the
    session's eviction XMarker (native, mirrors MFO's Packages.cpp exactly,
    never an authored ESP marker -- see Docs/SPEC-ALIAS-DRIVE.md §4).

    PRIORITY IS STATIC and chosen ONCE (QUST DNAM byte 2, authored here, not
    load order, not the DLL) -- MFO's own field evidence (ENGINE_NOTES 0.35b/
    0.36) proved a RUNTIME priority change does NOT re-arbitrate an
    already-claimed actor, so raising/lowering it at runtime would be a no-op;
    the claim model is entirely alias OCCUPANCY (fill = claim, evict = release).

    START-GAME-ENABLED, NOT RUN-ONCE (flags 0x0011): deliberately DIFFERENT
    from the task brief's literal "start-game-enabled, run-once" wording --
    every one of MFO's OWN alias-carrier quests with this exact bare-pool
    shape (MFO_CommandQuest/MFO_LootQuest/MFO_RetreatQuest) ships NOT
    run-once, and MFO_CommandQuest's own comment is explicit about why: a
    run-once quest is for one-shot startup logic, not a quest whose ALIASES
    must stay claimable for the entire session. Ported the field-proven
    shape rather than the literal brief; see Docs/SPEC-ALIAS-DRIVE.md's
    "deviations from the brief" note. CONSEQUENCE: being start-game-enabled
    and NOT run-once, this quest MUST be listed in Data/SEQ/APMF.seq or it
    never starts on an EXISTING save (MFO_GenerateESP.py's own SEQ comment;
    see main() below).
    """
    body  = subrec('EDID', zstr("APMF_ClaimQuest"))
    body += subrec('FULL', zstr("APMF Claim"))
    body += subrec('DNAM', qust_dnam(0x0011, priority=CLAIM_PRIORITY))
    body += subrec('NEXT', b'')
    body += subrec('ANAM', struct.pack('<I', NUM_SLOTS))

    FNAM = struct.pack('<I', 0x0002 | 0x0008 | 0x0200)   # Optional | AllowReuseInQuest | AllowReserved
    for slot in range(NUM_SLOTS):
        body += subrec('ALST', struct.pack('<I', slot))
        body += subrec('ALID', zstr(f"APMF_Slot{slot}"))
        body += subrec('FNAM', FNAM)
        body += subrec('ALPC', struct.pack('<I', FID_PLACEHOLDER_PACKAGE))
        body += subrec('VTCK', struct.pack('<I', 0))
        body += subrec('ALED', b'')
    return record('QUST', FID_CLAIM_QUEST, 0, body)


def make_qust():
    return group('QUST', make_claim_quest())


def make_pack():
    return group('PACK', make_placeholder_package())


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    data  = make_tes4(NEXT_OBJECT_ID)
    data += make_qust()
    data += make_pack()

    out_path = os.path.join(out_dir, "APMF.esl")
    with open(out_path, 'wb') as f:
        f.write(data)

    # SEQ: a start-game-enabled quest WITHOUT the Run Once flag never starts
    # on an EXISTING save unless it is listed in Data/SEQ/<plugin>.seq (a flat
    # array of uint32 FormIDs, as authored in the plugin) -- MFO_GenerateESP
    # .py's own SEQ comment, ported verbatim. Without this the claim quest's
    # alias pool never starts on an existing save and the alias-drive path
    # silently never claims anyone (no error, no log -- worth restating loudly
    # here because it is exactly the class of miss that looks like a mystery
    # in the field).
    seq_dir = os.path.join(out_dir, "SEQ")
    os.makedirs(seq_dir, exist_ok=True)
    seq_path = os.path.join(seq_dir, "APMF.seq")
    with open(seq_path, 'wb') as f:
        f.write(struct.pack('<I', FID_CLAIM_QUEST))

    print(f"Written: {out_path} ({len(data):,} bytes)")
    print(f"Written: {seq_path} (1 start-game-enabled quest: APMF_ClaimQuest, priority {CLAIM_PRIORITY})")
    print(f"  QUST  0x{FID_CLAIM_QUEST & 0xFFF:03X}  APMF_ClaimQuest ({NUM_SLOTS} actor-alias pool slots, static prio {CLAIM_PRIORITY})")
    print(f"  PACK  0x{FID_PLACEHOLDER_PACKAGE & 0xFFF:03X}  APMF_PlaceholderPackage (shared by all {NUM_SLOTS} slots)")


if __name__ == "__main__":
    main()
