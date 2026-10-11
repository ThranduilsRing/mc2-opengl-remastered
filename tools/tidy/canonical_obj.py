#!/usr/bin/env python3
"""canonical_obj - an object digest that is blind to the ORDER of symbols and sections.

WHAT: like tools/objhash.py obj_digest, but relocations are compared by the NAME of their target (a section symbol by the content of the section it names) and the
sections and the symbols as sets. WHY: adding or removing a symbol in a translation unit (a constant that is now an inline variable) can move an unrelated static data
member in the symbol table; every relocation then carries a different symbol INDEX and objhash calls the object changed, although code, data and relocation targets are the
same. This digest ignores that and still changes with any code byte, data byte, relocation target or symbol difference.
  py -3 tools/tidy/canonical_obj.py A.obj B.obj      # exit 0 when the digests are equal, 1 when not"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import objhash  # noqa: E402

NUL4 = b"\0\0\0\0"


def symbol_table(raw):
    """-> {symbol table index: (name, section number, storage class)}; aux records occupy an index, as in the COFF relocation entries."""
    if raw[:4] == b"\x00\x00\xff\xff" and len(raw) >= 56 and struct.unpack_from("<H", raw, 4)[0] >= 2:
        _n, symptr, nsym = struct.unpack_from("<III", raw, 44)
        symsz, fmt = 20, "<8sIiHBB"
    else:
        _m, _n, _ts, symptr, nsym, _o, _c = struct.unpack_from("<HHIIIHH", raw, 0)
        symsz, fmt = 18, "<8sIhHBB"
    strtab = symptr + nsym * symsz
    out, i = {}, 0
    while i < nsym:
        nm_raw, _val, secno, _typ, cls, naux = struct.unpack_from(fmt, raw, symptr + i * symsz)
        if nm_raw[:4] == NUL4:
            off = struct.unpack_from("<I", nm_raw, 4)[0]
            name = raw[strtab + off:raw.index(b"\0", strtab + off)].decode("latin-1")
        else:
            name = nm_raw.split(b"\0", 1)[0].decode("latin-1")
        out[i] = (name, secno, cls)
        i += 1 + naux
    return out


def canonical_digest(path):
    raw = objhash.read_bytes(path)
    parsed = objhash.parse_coff(raw) if raw else None
    if not parsed:
        return objhash.obj_digest(path)
    _fmt, secs, syms = parsed
    table = symbol_table(raw)
    sec_hash = {k + 1: objhash._h(nm, str(chars), str(rawsz), data) for k, (nm, chars, rawsz, data, _rel) in enumerate(secs)}
    items = []
    for nm, chars, rawsz, data, rel in secs:
        if nm.startswith((".debug", ".chks")):
            continue
        if nm.startswith(".rdata"):
            data = objhash._mask_fingerprint(data)
        relocs = []
        for addr, idx, typ in struct.iter_unpack("<IIH", rel[:len(rel) - len(rel) % 10]):
            name, secno, cls = table.get(idx, ("#%d" % idx, 0, 0))
            label = ("SECSYM:" + sec_hash.get(secno, name)) if cls == 3 and name.startswith(".") else objhash._mask_symbol(name)
            relocs.append((addr, label, typ))
        items.append(objhash._h(nm, str(chars), str(rawsz), data, repr(sorted(relocs))))
    # the symbol's offset inside its section is layout, not behaviour (the section contents are compared above): name, type, class and section name only
    sym_items = sorted(objhash._mask_symbol("%s|%d|%d|%s" % (s[0], s[2], s[3], s[4])) for s in syms if not s[4].startswith(".debug"))
    return {"code_sha256": objhash._h(*sorted(items), *sym_items)}


def install():
    """Make every objhash.obj_digest caller in this process (the tidy Proof) use the canonical digest."""
    objhash.obj_digest = canonical_digest


if __name__ == "__main__":
    a, b = (canonical_digest(p)["code_sha256"] for p in sys.argv[1:3])
    print("equal" if a == b else "DIFFERENT")
    sys.exit(0 if a == b else 1)
