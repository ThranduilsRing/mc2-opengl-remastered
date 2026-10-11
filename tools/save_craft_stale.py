#!/usr/bin/env python3
"""save_craft_stale.py - turn a current in-mission save into one an older build could have written.

WHAT: an older build has different record sizes, so one object class has records of another size. This rewrites the packet file with every
object packet of one chosen size shortened by 4 bytes (zlib packets are unpacked and repacked), offsets and checksum recomputed.
WHY: the loader tells an object's class from the byte size of its packet, so such a file is the reproduction for the stale-save crash in
GameObjectManager::Load, and the test input for the reject-and-log path (`[SAVE v1] event=reject reason=unknown_record_size`).

  py -3 tools/save_craft_stale.py IN.ims OUT.ims            # list the packet sizes after the object-manager header, shrink the most common one
  py -3 tools/save_craft_stale.py IN.ims OUT.ims --size N   # shrink the packets of size N

A current save comes from MC2_BOOT_QUICKSAVE_AT=<render frame> on a mission that is past its intro (mc2_01); it lands in ~/.mechcommander2/savegame/
testgame.ims. Put the crafted file there and start a run with MC2_BOOT_QUICKLOAD_AT=<frame>.
"""
import struct
import sys
import zlib
from collections import Counter

TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
NUL = 7


def read(path):
    data = open(path, 'rb').read()
    first, = struct.unpack_from('<I', data, 0)
    first_off, = struct.unpack_from('<I', data, 4)
    n = first_off // 4 - 2
    table = struct.unpack_from('<%dI' % n, data, 8)
    pk = []
    for i, e in enumerate(table):
        typ, off = e >> TYPE_SHIFT, e & OFFSET_MASK
        end = len(data)
        for e2 in table[i + 1:]:
            end = e2 & OFFSET_MASK
            break
        raw = data[off:end] if typ != NUL else b''
        if typ == 4:   # zlib: [unpacked length][stream]
            raw = zlib.decompress(raw[4:])
        pk.append((typ, raw))
    return first, pk


def write(path, pk, with_checksum):
    n = len(pk)
    head = 8 + 4 * n
    body = bytearray()
    table = []
    for typ, raw in pk:
        if typ == 4:
            raw = struct.pack('<I', len(raw)) + zlib.compress(raw)
        off = head + len(body)
        table.append((typ << TYPE_SHIFT) + off)
        body += raw
    out = bytearray(struct.pack("<II", 0, head))
    out += struct.pack('<%dI' % n, *table)
    out += body
    first = (sum(out[4:]) & 0xFFFFFFFF) if with_checksum else 0xFEEDFACE
    struct.pack_into('<I', out, 0, first)
    open(path, 'wb').write(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    want = int(sys.argv[sys.argv.index('--size') + 1]) if '--size' in sys.argv else None
    first, pk = read(src)
    sizes = [len(r) if t in (0, 4) else None for t, r in pk]
    hdr = [i for i, (t, r) in enumerate(pk) if t in (0, 4) and len(r) == 60]
    print('packets', len(pk), 'first int %08x' % first, 'header candidates', hdr)
    start = hdr[-1] + 1
    counts = Counter(s for s in sizes[start:] if s)
    print('raw sizes after the header:', counts.most_common(12))
    if want is None:
        want = counts.most_common(1)[0][0]
    out = []
    changed = 0
    for i, (t, r) in enumerate(pk):
        if i >= start and t in (0, 4) and len(r) == want:
            r = r[:-4]
            changed += 1
        out.append((t, r))
    with_checksum = first != 0xFEEDFACE
    write(dst, out, with_checksum)
    print('shrunk', changed, 'packets of size', want, '->', want - 4, 'checksum' if with_checksum else 'no checksum')


if __name__ == '__main__':
    main()
