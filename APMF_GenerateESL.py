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

NEXT_OBJECT_ID = 0x808   # first never-used local id

# ── Vanilla refs ──
FREF_PLAYER      = 0x00000014   # PlayerRef -- the PLACEHOLDER location target
FREF_TMPL_TRAVEL = 0x00016FAA   # vanilla PACK template 19 "Travel"

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


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "Data"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_pack()

    out_path = os.path.join(out_dir, "APMF.esl")
    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"wrote {out_path} ({len(data)} bytes)")
    print(f"  PACK  0x{FID_TRAVEL_BASE & 0xFFF:03X}-"
          f"0x{(FID_TRAVEL_BASE + TRAVEL_SLOTS - 1) & 0xFFF:03X}    "
          f"APMF_TravelPackage0..{TRAVEL_SLOTS - 1} (ch.19 travel, Travel 00016FAA, "
          f"PLDT type 0, radius 75)")


if __name__ == '__main__':
    main()
