#!/usr/bin/env python3
"""APMF.esl generator -- emits Harbinger's own plugin from pure Python.

Never hand-edit the ESL; change this file and regenerate:

    python3 APMF_GenerateESL.py Data

DOCTRINE (Linux-Native-Tools, and the reason this file exists):
  Never trust format docs -- dump a working record and mirror it. Every byte
  layout below is forked from MFO's SHIPPED, in-game-proven MFO_GenerateESP.py
  (`build_travel()` / `make_apmf_loot_travel_package()`), which in turn was
  verified against records parsed out of Skyrim.esm. The engine drops malformed
  records SILENTLY; there is no error to catch.

WHY APMF SHIPS A PLUGIN AT ALL (2026-09-22, ch.19 kIntent_Travel).
  ch.9 (`kIntent_OfferPackage`) hands the engine a TESPackage the CLIENT ships.
  ch.19 moves the actor FOR the client, so the package has to be APMF's. Move-to-a-
  place is a staple feature, not a niche one -- the destination may be any object
  reference OR a whole cell -- and ONE shipped, generated, ESL-flagged record set (no
  load-order slot, no overrides, ~2.5 KB, one master) buys every client a working
  travel facet. That is what earns the plugin its place.
  Three options were weighed before writing this file:

    (A) REUSE A VANILLA PACKAGE -- REJECTED, two independent fatal defects.
        A package's Location input lives on the RECORD, not on the actor: the
        runtime write (`core/PackageData.cpp` SetTravelTarget) re-points it for
        EVERY actor in the game running that record, so borrowing a vanilla one
        hijacks whatever quest owns it. And one record carries one destination,
        so a framework that must serve N concurrent engagements needs N records.
        Measured: Skyrim.esm holds 5961 PACK records; 1988 are instances of the
        vanilla Travel template 00016FAA, of which 1494 already use a
        NearReference location bound to a specific vanilla NPC/quest. There is
        no unowned, per-actor-writable vanilla travel package.

    (B) FABRICATE A PACKAGE AT RUNTIME (the MFO "proxy form" pattern,
        IFormFactory::GetConcreteFormFactoryByType<T>()->Create()) -- DEFERRED,
        unproven. That pattern is production-proven for SpellItem ONLY, and it
        works there because a spell's payload is shallow-copyable. A package's
        inputs live behind TESPackage::data (TESPackageData*), so a shallow copy
        SHARES the source's inputs -- defect (A) again -- and a deep copy means
        hand-constructing IPackageData wrapper objects and the UNAM/ANAM name
        map, neither of which the pinned CommonLib exposes. A dynamic 0xFF form
        also does not survive a save/load while AIProcess::currentPackage may
        still point at it. Strictly worse than a shipped record.

    (C) SHIP ONE -- TAKEN. It is the port of a field-proven MFO mechanism.

FormID band is a FROZEN generator<->DLL contract with `native/channels/Travel.cpp`.
One master (Skyrim.esm) => own-file master index 0x01. ESL-legal range 0x800-0xFFF.
Ids are only ever ADDED; a retired id is never recycled.
"""

import os
import struct
import sys

# ── FormID band -- FROZEN. Never renumber. ──
OWN = 0x01000000

# ch.19 (kIntent_Travel) packages: ONE per concurrent travel leg, mirroring MFO's
# per-slot loot-travel records. Each is byte-identical to the others except for its
# FormID and EDID; the DLL overwrites the Location input's live runtime handle AND
# its radius per slot before offering it through ch.9.
#
# THE FROZEN CONTRACT WITH THE DLL IS THE FormID BAND, NOT THE EDIDs.
# native/channels/Travel.cpp resolves these by local id (0x800 + slot) through
# TESDataHandler::LookupForm, so the editor IDs are documentation only and were
# renamed freely when the intent was renamed.
FID_TRAVEL_BASE = OWN | 0x800   # 0x800 .. 0x807 (kTravelSlots == 8)
TRAVEL_SLOTS = 8

# Combat substrate (S2a, 2026-10-13; MFO Docs/COMBAT-SUBSTRATE-DESIGN.md section 3.3). marth
# 2026-10-12: "If it can't be the player, 1 is fine. Use a clone of nazeem." ONE anchor actor,
# the target of every substrate's combat controller, parked in its own interior holding cell.
# native/channels/CombatSubstrate.cpp resolves these by LOCAL id through
# TESDataHandler::LookupForm(id, "APMF.esl") -- the FormID band is the contract, the EDIDs are
# documentation.
FID_ANCHOR_NPC   = OWN | 0x808   # NPC_  APMF_SubstrateAnchor      (the Nazeem clone)
FID_ANCHOR_CELL  = OWN | 0x809   # CELL  APMF_SubstrateHoldingCell (interior, never visited)
FID_ANCHOR_REF   = OWN | 0x80A   # ACHR  APMF_SubstrateAnchorREF   (persistent, the anchor)
FID_ANCHOR_HOME  = OWN | 0x80B   # REFR  APMF_SubstrateAnchorHome  (persistent XMarker, where it goes home)

NEXT_OBJECT_ID = 0x80C   # first never-used local id

# ── Vanilla refs ──
FREF_PLAYER      = 0x00000014   # PlayerRef -- the PLACEHOLDER location target
FREF_TMPL_TRAVEL = 0x00016FAA   # vanilla PACK template 19 "Travel"
FSTAT_XMARKER    = 0x0000003B   # XMarker (STAT) -- the anchor's home marker base
FNPC_NAZEEM      = 0x00013BBF   # Nazeem (NPC_) -- the anchor is cloned from it (documentation)

# ── binary helpers (forked from MFO_GenerateESP.py -- byte-for-byte valid) ──
FORM_VERSION = 44


def subrec(t, d):
    return t.encode('ascii') + struct.pack('<H', len(d)) + d


def record(t, fid, fl, d):
    return (t.encode('ascii') + struct.pack('<I', len(d)) + struct.pack('<I', fl)
            + struct.pack('<I', fid) + struct.pack('<I', 0)
            + struct.pack('<H', FORM_VERSION) + struct.pack('<H', 0) + d)


def group(label, data):
    return (b'GRUP' + struct.pack('<I', 24 + len(data)) + label.encode('ascii')
            + struct.pack('<iII', 0, 0, 0) + data)


def subgroup(label_bytes, gtype, data):
    """A nested GRUP (cell block / sub-block / cell children). `label_bytes` is the raw
    4-byte label: a block number (int32) for types 2/3, the parent CELL's FormID for
    types 6/8/9. Header layout mirrored from Skyrim.esm's WIDeadBodyCleanupCell
    (001037E7): 'GRUP', size, label, type, stamp, unknown -- stamp/unknown 0 here, as
    this generator writes every other group."""
    return (b'GRUP' + struct.pack('<I', 24 + len(data)) + label_bytes
            + struct.pack('<iII', gtype, 0, 0) + data)


def zstr(s):
    # ASCII ONLY -- multibyte UTF-8 renders as mojibake in game.
    return s.encode('ascii') + b'\x00'


def make_tes4(next_id):
    """TES4 record flags 0x200 = ESL. ESL is a LIE unless every local id is
    inside 0x800-0xFFF -- this generator's whole band is, by construction."""
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', next_id)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("marth")) \
        + subrec('SNAM', zstr("Harbinger (APMF) -- framework-owned AI packages"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0, 0x00000200, body)


def pack_input(kind, payload_type, payload):
    """One templated-package data input: ANAM names the TYPE, then its value."""
    return subrec('ANAM', zstr(kind)) + subrec(payload_type, payload)


def build_travel(fid, edid, radius):
    """One PACK instance riding vanilla Travel (00016FAA) with a RUNTIME-HANDLE
    Location: walk to whatever ref the DLL points the Location input at, then
    stop. Byte shape mirrored from MFO's shipped, field-run
    MFO_APMFLootTravelPackage<slot> (MFO_GenerateESP.py build_travel(
    runtime_target=True)), which is itself mirrored from the vanilla
    alias-delivered exemplar VC01FalionAtSummoningCircle (0010FF16).

    The Travel template (00016FAA) declares 3 inputs -- uid 0 "Place to Travel"
    (PLDT), uid 2 "Ride Horse if possible?" and uid 4 "Prefer Preferred Path?"
    (both CNAM Bools). Those three parameter names are NOT guesses: MFO's
    FindInput dumped the template's real BNAM name map on the deck 2026-09-03
    (Tuxborn, Cicero test). The settable UNAM run is 0/2/4 and XNAM is 3.
    """
    body = subrec('EDID', zstr(edid))

    # PKDT general flags 0x00002000 = PREFERRED-SPEED ENABLE, and nothing else.
    #
    # DELIBERATELY NOT kIgnoreCombat (0x00100000), and it matters more than ever now
    # that the contract is "combat interrupts and cancels the movement" (marth,
    # 2026-09-22): if a fight starts en route the engine's own combat AI takes the
    # actor, and the DLL's own monitor sees `IsInCombat()` and ends the leg. Setting
    # the bit would fight both halves of that. A future kTravel_IgnoreCombatEnRoute
    # flag would select a SECOND record set, never mutate this one.
    #
    # preferredSpeed is byte 6 of PKDT (0=Walk 1=Jog 2=Run 3=FastWalk) and is
    # INERT unless 0x2000 is set -- proven by scanning all 5,961 Skyrim.esm PACK
    # records (without 0x2000, byte6 is the inert default 2 in 4,386/4,502; with
    # it, the value spreads across all four speeds). Byte 6 = 2 (Run): an actor
    # sent at an enemy at a stroll would be ambiguous data.
    # Tail '1200028054000000' = packType 18, byte6=2, interruptFlags 0x0054.
    body += subrec('PKDT', struct.pack('<I', 0x00002000) + bytes.fromhex('1200028054000000'))

    # PSDT: any time, any day -- the 3,855-of-5,961 vanilla default.
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))

    # NO CTDA: the package is unconditional. It runs exactly when APMF's 0x49
    # hook hands it to a claimed actor and stops when the claim is released.
    #
    # NO QNAM either. QNAM (owner quest) is mandatory for an ALIAS-valued input
    # (626/626 vanilla instances) and forbidden here: the Location is PLDT type
    # 0 (a live ref handle), so it names no alias and belongs to no quest. APMF
    # delivers this package through ch.9's 0x49 redirect, with NO alias fill at
    # all -- which is precisely why the alias-valued form is unusable for us.
    body += subrec('PKCU', struct.pack('<III', 3, FREF_TMPL_TRAVEL, 3))

    # input uid 0: Location -> PLDT type 0 ("Near Reference",
    # RE::PackageLocation::Type::kNearReference) authored with a PLACEHOLDER
    # non-null ref. 0 of 4,048 vanilla type-0 PLDTs ship a null target, so the
    # placeholder is PlayerRef -- always loaded, harmless, and always overwritten
    # by core/PackageData.cpp SetTravelTarget before the package is ever offered.
    body += pack_input("Location", 'PLDT', struct.pack('<IiI', 0, FREF_PLAYER, radius))
    # inputs uid 2 and uid 4: the two Bools, both false -- as every vanilla
    # WERoad travel instance ships them.
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    # settable-slot run 0/2/4, then XNAM 3 -- verbatim from the exemplar.
    body += subrec('UNAM', struct.pack('<B', 0))
    body += subrec('UNAM', struct.pack('<B', 2))
    body += subrec('UNAM', struct.pack('<B', 4))
    body += subrec('XNAM', struct.pack('<B', 3))
    # Empty on-begin/end/change blocks, as vanilla ships them.
    for blk in ('POBA', 'POEA', 'POCA'):
        body += subrec(blk, b'')
        body += subrec('INAM', struct.pack('<I', 0))
        body += subrec('PDTO', struct.pack('<II', 0, 0))
    return record('PACK', fid, 0, body)


def make_pack():
    """The ch.19 travel packages. The authored radius is 75 -- the middle of marth's
    "50-100u from it" band and the same number native/channels/Travel.cpp uses as its
    default arrival radius, so the engine's own package stop and the DLL's arrival
    test agree by construction.

    It is only a PLACEHOLDER either way: the DLL writes this leg's real radius (and
    the real destination handle) into the record before the package is ever offered,
    and refuses the leg outright if that write is declined. Authoring the same number
    means a reader never has to reconcile two."""
    out = b''
    for i in range(TRAVEL_SLOTS):
        out += build_travel(FID_TRAVEL_BASE + i, f"APMF_TravelPackage{i}", radius=75)
    return group('PACK', out)


# ── Combat substrate anchor (S2a) ─────────────────────────────────────────────
#
# THE NPC_ (0x808): Nazeem (Skyrim.esm 00013BBF, a 453-byte zlib record, read from
# Tuxborn's Skyrim.esm on 2026-10-13) authored FIELD BY FIELD. Every kept subrecord is
# Nazeem's bytes, in Nazeem's order; the changes are listed per field.
#   STRIPPED (design 3.3): VMAD (his WIDeadBodyCleanupScript), the six SNAM factions
#   (his crime faction included), COCT/CNTO (inventory), the five PKID (AI packages),
#   DOFT (outfit), CRIF (crime faction). He carries no SPLO, INAM or PNAM, so there
#   is nothing more to strip.
#   FULL: Nazeem's is a localized string id (Skyrim.esm is localized; this plugin is
#   not), so it is replaced by a plain name.
#   ACBS: Unique (0x20) CLEARED -- the engine must never confuse it with Nazeem;
#   Essential (0x02) and IsGhost (1 << 29) SET (the anchor can never die or be hit:
#   authored on the base, so no frame exists where it is hittable); level 1. Every other
#   ACBS byte is Nazeem's.
#   AIDT: aggression 0 (his: 0, unaggressive) and assistance 0 (his: 1, "helps
#   allies" -> helps nobody, so it never joins anyone's fight). Every other AIDT byte
#   is Nazeem's.
# Harbinger additionally makes it invisible (alpha 0), turns its AI off and drops its
# collision at runtime, and never places it with PlaceAtMe (it is persistent: nothing
# is created or deleted, the .ess only carries its position).
ANCHOR_NAZEEM_KEEP = [
    # (subrecord, Nazeem's bytes as hex) -- in Nazeem's order.
    ('OBND', '000000000000000000000000'),
    ('ACBS', None),   # built below from Nazeem's 300020100000000004000000000064002300000000000000
    ('VTCK', 'd93a0100'),   # MaleCondescending (his)
    ('RNAM', '48370100'),   # NordRace
    ('AIDT', None),   # built below from Nazeem's 0002320300010000000000000000000000000000
    ('CNAM', '6b320100'),   # his class
    ('FULL', None),   # plain string (see above)
    ('DATA', ''),
    ('DNAM', '1b0f1614180f110f0f121312140f140f0f130000000000000000000000000000000000004b003c003c0000000000000000001e00'),
    ('NAM5', 'ff00'),
    ('NAM6', '0000803f'),
    ('NAM7', '00004842'),
    ('NAM8', '01000000'),
    ('QNAM', 'd6d4543e9d9c1c3e8988083e'),
    ('NAM9', '000080bf14ae473f0ad723bfb81e05bf00000000000000000000000000000000000000000000000000000000000000005c8f423fe851383e3e0a57bfcdcc4cbf3e0ad7be0000000000000000'),
    ('NAMA', '0c000000000000000c00000003000000'),
    ('TINI', '0100'),
    ('TINC', '35272200'),
    ('TINV', '64000000'),
    ('TIAS', '4f00'),
]

ACBS_UNIQUE    = 0x00000020
ACBS_ESSENTIAL = 0x00000002
ACBS_GHOST     = 0x20000000


def anchor_acbs():
    b = bytearray.fromhex('300020100000000004000000000064002300000000000000')   # Nazeem's
    flags = struct.unpack_from('<I', b, 0)[0]
    flags = (flags & ~ACBS_UNIQUE) | ACBS_ESSENTIAL | ACBS_GHOST
    struct.pack_into('<I', b, 0, flags)
    struct.pack_into('<H', b, 8, 1)   # level 1 (Nazeem: 4)
    return bytes(b)


def anchor_aidt():
    b = bytearray.fromhex('0002320300010000000000000000000000000000')   # Nazeem's
    b[0] = 0   # aggression: unaggressive (Nazeem's own value)
    b[5] = 0   # assistance: helps nobody (Nazeem: 1, helps allies)
    return bytes(b)


def build_anchor_npc():
    body = subrec('EDID', zstr("APMF_SubstrateAnchor"))
    for sig, hx in ANCHOR_NAZEEM_KEEP:
        if sig == 'ACBS':
            body += subrec('ACBS', anchor_acbs())
        elif sig == 'AIDT':
            body += subrec('AIDT', anchor_aidt())
        elif sig == 'FULL':
            body += subrec('FULL', zstr("Harbinger Anchor"))
        else:
            body += subrec(sig, bytes.fromhex(hx))
    return record('NPC_', FID_ANCHOR_NPC, 0, body)


# THE CELL (0x809): an interior holding cell, mirrored from Skyrim.esm's own holding cell
# WIDeadBodyCleanupCell (001037E7): DATA 0x0001 (interior), its XCLL lighting verbatim,
# LTMP none, XCLW 0. Its XLCN (a vanilla location) and XCIM (an image space) are not
# copied: a cell nobody visits needs neither.
CELL_XCLL_WIDEADBODY = ('ffffff0000000000000000000000000000000000000000000000000000000000000000000000803f18ff'
                        '0a00e5fff300ff0aff00fff3ff007979730084848a00000000000000803f000000000000803f0000000000'
                        '0000009f000000')


def build_anchor_cell():
    body = subrec('EDID', zstr("APMF_SubstrateHoldingCell"))
    body += subrec('DATA', struct.pack('<H', 0x0001))   # interior
    body += subrec('XCLL', bytes.fromhex(CELL_XCLL_WIDEADBODY))
    body += subrec('LTMP', struct.pack('<I', 0))
    body += subrec('XCLW', struct.pack('<f', 0.0))
    return record('CELL', FID_ANCHOR_CELL, 0, body)


REC_PERSISTENT = 0x00000400   # the vanilla holding cell's ACHR / REFR both carry it


def build_anchor_ref():
    """ACHR (0x80A), persistent, mirrored from MQ201HiddenHoldingContainerREF (00039F1E, the
    vanilla holding cell's own persistent ACHR): EDID, NAME (the base), DATA (position +
    rotation, 6 floats). At the cell origin; Harbinger moves it, never this record."""
    body = subrec('EDID', zstr("APMF_SubstrateAnchorREF"))
    body += subrec('NAME', struct.pack('<I', FID_ANCHOR_NPC))
    body += subrec('DATA', struct.pack('<6f', 0.0, 0.0, 0.0, 0.0, 0.0, 0.0))
    return record('ACHR', FID_ANCHOR_REF, REC_PERSISTENT, body)


def build_anchor_home():
    """REFR (0x80B), persistent XMarker, mirrored from WIDeadBodyCleanupCellMarker (001037F2):
    EDID, NAME 0x3B, DATA. The anchor's home: Harbinger sends the anchor back with
    MoveTo(this marker), so it never needs the engine's private MoveTo_Impl."""
    body = subrec('EDID', zstr("APMF_SubstrateAnchorHome"))
    body += subrec('NAME', struct.pack('<I', FSTAT_XMARKER))
    body += subrec('DATA', struct.pack('<6f', 0.0, 0.0, 0.0, 0.0, 0.0, 0.0))
    return record('REFR', FID_ANCHOR_HOME, REC_PERSISTENT, body)


def make_npc():
    return group('NPC_', build_anchor_npc())


def make_cell():
    """Top CELL group -> interior block -> sub-block -> CELL + its children group ->
    the persistent-children group (ACHR, REFR). Block / sub-block are the last and the
    second-to-last decimal digit of the cell's object id (0x809 = 2057 -> block 7,
    sub-block 5), the numbering Skyrim.esm uses (001037E7 = 1062887 -> 7 / 8)."""
    obj = FID_ANCHOR_CELL & 0xFFF
    block, sub = obj % 10, (obj // 10) % 10
    fid_label = struct.pack('<I', FID_ANCHOR_CELL)
    persistent = subgroup(fid_label, 8, build_anchor_ref() + build_anchor_home())
    children = subgroup(fid_label, 6, persistent)
    sub_block = subgroup(struct.pack('<i', sub), 3, build_anchor_cell() + children)
    block_grp = subgroup(struct.pack('<i', block), 2, sub_block)
    return group('CELL', block_grp)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "Data"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_npc()    # canonical group order: NPC_ before PACK, CELL after both
    data += make_pack()
    data += make_cell()

    out_path = os.path.join(out_dir, "APMF.esl")
    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"wrote {out_path} ({len(data)} bytes)")
    print(f"  PACK  0x{FID_TRAVEL_BASE & 0xFFF:03X}-"
          f"0x{(FID_TRAVEL_BASE + TRAVEL_SLOTS - 1) & 0xFFF:03X}    "
          f"APMF_TravelPackage0..{TRAVEL_SLOTS - 1} (ch.19 travel, Travel 00016FAA, "
          f"PLDT type 0, radius 75)")
    print(f"  NPC_  0x{FID_ANCHOR_NPC & 0xFFF:03X}        APMF_SubstrateAnchor (Nazeem 00013BBF clone: no factions, "
          f"packages, outfit, inventory, script; not Unique; essential + ghost; level 1)")
    print(f"  CELL  0x{FID_ANCHOR_CELL & 0xFFF:03X}        APMF_SubstrateHoldingCell (interior)")
    print(f"  ACHR  0x{FID_ANCHOR_REF & 0xFFF:03X}        APMF_SubstrateAnchorREF (persistent, in the holding cell)")
    print(f"  REFR  0x{FID_ANCHOR_HOME & 0xFFF:03X}        APMF_SubstrateAnchorHome (persistent XMarker, the anchor's home)")


if __name__ == '__main__':
    main()
