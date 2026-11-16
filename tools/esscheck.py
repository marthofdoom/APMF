#!/usr/bin/env python3
"""Offline acceptance check for INVARIANTS #19 (Harbinger cast proxies must not be in a save).
usage: esscheck.py <save.ess> <FFxxxxxx> [<FFxxxxxx> ...]
The ids are the pool forms APMF.log printed for that save:
  [castproxy] save sweep (kSaveGame ...): ... minted pool form(s) [0xFF00081F, ...]
For each id: every change form whose (decompressed) data holds the 3-byte RefID of that created form
(type 2 = 0x80xxxx | low 22 bits, big-endian) is listed with its owner form id, change-form type and
the offsets. A clean save prints 'CLEAN'. A 3-byte match can be a coincidence in a large record:
judge by the offset repeating across saves / the owner being the caster or the target.
Also scans the global-data region after the change forms (Papyrus etc.) and reports hit counts."""
import struct, sys, zlib, lz4.block
f = open(sys.argv[1], 'rb').read()
ids = [int(x, 16) for x in sys.argv[2:]]
assert f[:13] == b'TESV_SAVEGAME'
p = 13; hs = struct.unpack_from('<I', f, p)[0]; p += 4; h0 = p
p += 8
def ws():
    global p
    n = struct.unpack_from('<H', f, p)[0]; p += 2 + n
ws(); p += 4; ws(); ws(); ws(); p += 2 + 8 + 8
w, hh = struct.unpack_from('<II', f, p); p += 8
comp = struct.unpack_from('<H', f, p)[0]
p = h0 + hs + w * hh * 4
if comp == 2:
    ul, cl = struct.unpack_from('<II', f, p); p += 8
    d = lz4.block.decompress(f[p:p + cl], uncompressed_size=ul)
elif comp == 1:
    ul, cl = struct.unpack_from('<II', f, p); p += 8
    d = zlib.decompress(f[p:p + cl])
else:
    d = f[p:]
q = 0; q += 1; pis = struct.unpack_from('<I', d, q)[0]; q += 4
q += pis
flt = struct.unpack_from('<IIIIIIIIII', d, q)
fidOff, u3Off, g1Off, g2Off, cfOff, g3Off, g1c, g2c, g3c, cfc = flt
B = 13 + 4 + hs + w * hh * 4
fidOff -= B; cfOff -= B; g3Off -= B
fidc = struct.unpack_from('<I', d, fidOff)[0]; fids = struct.unpack_from('<%dI' % fidc, d, fidOff + 4)
def resolve(r):
    t = r >> 22; v = r & 0x3FFFFF
    if t == 0: return fids[v - 1] if 0 < v <= len(fids) else 0
    if t == 1: return v
    if t == 2: return 0xFF000000 | v
    return 0
pat = {i: bytes([0x80 | ((i >> 16) & 0x3F), (i >> 8) & 0xFF, i & 0xFF]) for i in ids}
q = cfOff; bad = 0; own = set()
for n in range(cfc):
    r = (d[q] << 16) | (d[q + 1] << 8) | d[q + 2]; q += 3
    flags = struct.unpack_from('<I', d, q)[0]; q += 4
    t = d[q]; q += 2
    ls = t >> 6; ty = t & 0x3F
    fmt = {0: '<BB', 1: '<HH', 2: '<II'}[ls]
    l1, l2 = struct.unpack_from(fmt, d, q); q += struct.calcsize(fmt)
    data = d[q:q + l1]; q += l1
    owner = resolve(r); own.add(owner)
    if l2:
        try: data = zlib.decompress(data)
        except Exception: pass
    for i, pt in pat.items():
        offs = []; k = data.find(pt)
        while k >= 0: offs.append(k); k = data.find(pt, k + 1)
        if offs:
            bad += 1
            print('HIT  proxy %08X in change form of %08X (type %d, flags %08X, %d bytes) at %s' % (i, owner, ty, flags, len(data), [hex(o) for o in offs[:12]]))
for i in ids:
    if i in own: print('NOTE the save has a change form OF %08X itself' % i)
tail = d[g3Off:]
for i, pt in pat.items():
    c = tail.count(pt)
    if c: print('NOTE %d raw match(es) of %08X in the global-data region after the change forms (Papyrus / misc; 3-byte matches there are usually coincidence)' % (c, i))
print('CLEAN' if bad == 0 else '%d change form(s) reference a proxy' % bad)
