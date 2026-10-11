#!/usr/bin/env python3
"""canonical_obj - an object digest that is blind to the ORDER of symbols and sections and to the layout of .data / .bss.

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

symbol_table = objhash.symbol_table


BLIND = (".data", ".bss")


def blind_section(nm, data, rel, symbols, table, sec_hash):
    """.bss: the set of its symbols; .data: the set of (symbol, its bytes without trailing zero padding, its relocations by target name). Neither depends on the
    order the symbols were laid out in or on the alignment padding between them."""
    if nm == ".bss":
        return objhash._h(nm, repr(sorted(n for _v, n in symbols)))
    relocs = []
    for addr, idx, typ in struct.iter_unpack("<IIH", rel[:len(rel) - len(rel) % 10]):
        name, secno, cls, _v = table.get(idx, ("#%d" % idx, 0, 0, 0))
        label = ("SECSYM:" + sec_hash.get(secno, name)) if cls == 3 and name.startswith(".") else objhash._mask_symbol(name)
        relocs.append((addr, label, typ))
    chunks = []
    for i, (v, name) in enumerate(symbols):
        end = symbols[i + 1][0] if i + 1 < len(symbols) else len(data)
        mine = sorted((a - v, lab, t) for a, lab, t in relocs if v <= a < end)
        chunks.append((name, data[v:end].rstrip(bytes(1)), mine))
    return objhash._h(nm, repr(sorted(chunks)))


def canonical_digest(path):
    raw = objhash.read_bytes(path)
    parsed = objhash.parse_coff(raw) if raw else None
    if not parsed:
        return objhash.obj_digest(path)
    _fmt, secs, syms = parsed
    table = symbol_table(raw)
    sec_hash = {k + 1: objhash._h(nm, str(chars), str(rawsz), data) for k, (nm, chars, rawsz, data, _rel) in enumerate(secs)}
    for k, (nm, *_rest) in enumerate(secs):
        if nm in BLIND:
            sec_hash[k + 1] = nm                       # a relocation into .data / .bss names the section, not its (layout-dependent) content
    by_sec = {}
    for name, secno, cls, val in table.values():
        if secno > 0 and cls in (2, 3) and secs[secno - 1][0] in BLIND and not name.startswith("."):
            by_sec.setdefault(secno, []).append((val, name))
    items = []
    for k, (nm, chars, rawsz, data, rel) in enumerate(secs):
        if nm.startswith((".debug", ".chks")):
            continue
        if nm in BLIND:
            items.append(blind_section(nm, data, rel, sorted(by_sec.get(k + 1, [])), table, sec_hash))
            continue
        if nm.startswith(".rdata"):
            data = objhash._mask_fingerprint(data)
        relocs = []
        for addr, idx, typ in struct.iter_unpack("<IIH", rel[:len(rel) - len(rel) % 10]):
            name, secno, cls, _v = table.get(idx, ("#%d" % idx, 0, 0, 0))
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
