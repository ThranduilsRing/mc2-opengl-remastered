#!/usr/bin/env python3
"""pch_audit - find the translation units whose code a precompiled header would change, and keep them out of it.

WHAT: compares every object of the gameos, mclib and mc2 targets built WITHOUT the precompiled header (the reference tree) with the same
object built WITH it, and writes cmake/pch/pch_skip.cmake: the sources whose code or data differ, which the build compiles without the PCH.
`--check` makes the same comparison and fails when any object differs, i.e. it proves a tree built with the skip list is byte-equivalent.
WHY: the PCH makes every <cmath>/<cstdlib> overload visible to every TU. A TU that included no STL header called sqrt(double) for a float
argument; with the PCH it calls sqrt(float), so its numbers change. A precompiled header must not change what the engine computes, so such a
TU stays out of it. The comparison ignores what the PCH legitimately moves: linker directives, unwind tables, local label numbers,
anonymous-namespace hashes, and STL instantiations that are emitted in the precompiled object instead of the TU.

  py -3 tools/pch_audit.py --ref build-pch-off --pch build-pch-on --out cmake/pch/pch_skip.cmake
  py -3 tools/pch_audit.py --ref build-pch-off --pch build-pch-on --check
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import objhash  # noqa: E402

TARGETS = ("gameos", "mclib", "mc2")
#: the one STL weak variable that moves into the precompiled object
KNOWN_MOVES = {"_Avx2WmemEnabledWeakValue"}
STD = "@std@@"
#: sections that carry code or data; unwind tables, debug info and linker directives follow from them
CODE_SECTIONS = (".text", ".rdata", ".data", ".CRT", ".tls", ".rtc")
_NORM = re.compile(r"A0x[0-9a-f]{8}|^\$LN[0-9]+$")


#: the build time that cmake/GenerateBuildFingerprint.cmake stamps into string literals, as bytes in .rdata and as part of a literal's
#: mangled symbol name ("?3" is ':')
_STAMP_BYTES = re.compile(rb"20[0-9][0-9]-[01][0-9]-[0-3][0-9]T[0-2][0-9]:[0-5][0-9]:[0-5][0-9]")
_STAMP_NAME = re.compile(r"20[0-9][0-9](?:-|\?9)[01][0-9](?:-|\?9)[0-3][0-9]T[0-2][0-9]\?3[0-5][0-9]\?3[0-5][0-9]")


def norm(name: str) -> str:
    """A symbol name without the parts that legitimately differ between two compiles of the same source."""
    if _STAMP_NAME.search(name) or "MC2FP1" in name or "?$CCcommit?$CC" in name:
        return "build-stamp-literal"
    return _NORM.sub("#", objhash._mask_symbol(name))


def _ignored_leader(lead: str) -> bool:
    return STD in lead or lead.startswith("__xmm@") or "MHIKGOKE" in lead      # STL, SIMD constants, the STL AM/PM table


def signature(path: str):
    """-> (Counter of code/data section signatures, Counter of defined symbol names) of one COFF object."""
    b = open(path, "rb").read()
    parsed = objhash.parse_coff(b)
    fmt, secs, _syms = parsed
    if fmt == "coff-bigobj":
        _nsec, symptr, nsym = struct.unpack_from("<III", b, 44)
        symsz = 20
    else:
        _m, _nsec, _ts, symptr, nsym, _opt, _c = struct.unpack_from("<HHIIIHH", b, 0)
        symsz = 18
    strtab = symptr + nsym * symsz

    def sname(raw):
        if raw[:4] == b"\0\0\0\0":
            off = struct.unpack_from("<I", raw, 4)[0]
            return b[strtab + off:b.index(b"\0", strtab + off)].decode("latin-1")
        return raw.split(b"\0", 1)[0].decode("latin-1")

    names, secno_of, leader = {}, {}, {}
    i = 0
    while i < nsym:
        o = symptr + i * symsz
        if fmt == "coff-bigobj":
            nm_raw, _val, secno, _typ, cls, naux = struct.unpack_from("<8sIiHBB", b, o)
        else:
            nm_raw, _val, secno, _typ, cls, naux = struct.unpack_from("<8sIhHBB", b, o)
        nm = sname(nm_raw)
        if cls == 3 and 0 < secno <= len(secs) and nm == secs[secno - 1][0]:        # a section symbol is named by its content
            nm = "section:" + hashlib.sha256(secs[secno - 1][3]).hexdigest()[:16]
        names[i], secno_of[i] = nm, secno
        if cls == 2 and secno > 0 and secno not in leader:
            leader[secno] = nm
        i += 1 + naux
    sigs = collections.Counter()
    for sidx, (nm, chars, rawsz, data, rel) in enumerate(secs, 1):
        if not nm.startswith(CODE_SECTIONS) or _ignored_leader(leader.get(sidx) or ""):
            continue
        if nm.startswith(".rdata"):
            data = _STAMP_BYTES.sub(lambda m: b"#" * len(m.group(0)), objhash._mask_fingerprint(data))
        rels = tuple(sorted((va, typ, norm(names.get(idx, "?%d" % idx)))
                            for va, idx, typ in (struct.unpack_from("<IIH", rel, k * 10) for k in range(len(rel) // 10))))
        sigs[(nm, chars, rawsz, hashlib.sha256(data).hexdigest(), rels)] += 1
    defined = collections.Counter(norm(names[k]) for k in names
                                  if secno_of[k] > 0 and not names[k].startswith((".debug", "section:", "$LN"))
                                  and not _ignored_leader(names[k]) and norm(names[k]) != "build-stamp-literal")
    return sigs, defined


def differs(ref_obj: str, pch_obj: str) -> bool:
    """True when the two objects differ in code, data or defined symbols beyond what a precompiled header legitimately moves."""
    sa, da = signature(ref_obj)
    sb, db = signature(pch_obj)
    diff_secs = (sa - sb) + (sb - sa)
    diff_syms = {k for k in ((da - db) + (db - da)) if k not in KNOWN_MOVES}
    return bool(diff_secs) or bool(diff_syms)


def object_sources(tree: str) -> dict:
    """{object path relative to the tree: repo-relative source} from compile_commands.json."""
    with open(os.path.join(tree, "compile_commands.json"), encoding="utf-8") as fh:
        db = json.load(fh)
    out = {}
    for e in db:
        obj = os.path.normpath(os.path.join(e["directory"], e["output"]))
        src = e["file"] if os.path.isabs(e["file"]) else os.path.join(e["directory"], e["file"])
        out[os.path.relpath(obj, tree).replace("\\", "/")] = os.path.relpath(os.path.normpath(src), ROOT).replace("\\", "/")
    return out


def target_of(obj_rel: str):
    m = re.search(r"CMakeFiles/(gameos|mclib|mc2)\.dir/", obj_rel)
    return m.group(1) if m else None


def audit(ref_tree: str, pch_tree: str) -> dict:
    """-> {target: sorted repo-relative sources whose object differs}; objects missing from either tree count as differing."""
    srcs = object_sources(ref_tree)
    out = {t: [] for t in TARGETS}
    for obj_rel, src in sorted(srcs.items()):
        t = target_of(obj_rel)
        if not t or obj_rel.endswith("cmake_pch.cxx.obj"):
            continue
        a, b = os.path.join(ref_tree, obj_rel), os.path.join(pch_tree, obj_rel)
        if not (os.path.exists(a) and os.path.exists(b)) or differs(a, b):
            out[t].append(src)
    return {t: sorted(set(v)) for t, v in out.items()}


HEADER = """# GENERATED by tools/pch_audit.py -- do not edit. Sources compiled WITHOUT the precompiled header (cmake/pch/mc2_std_pch.h).
# Each of them produced different code or data with the PCH (the PCH makes the <cmath>/<cstdlib> overloads visible, which changes
# e.g. sqrt(float) from the double to the float overload in a TU that included no STL header). Regenerate with:
#   py -3 tools/pch_audit.py --ref <tree built with -DMC2_PCH=OFF> --pch <tree built with -DMC2_PCH=ON and the skip list emptied> --out cmake/pch/pch_skip.cmake
"""


def existing(path: str) -> dict:
    """The skip list already in `path` ({target: [source]}); empty when the file is missing."""
    out = {t: [] for t in TARGETS}
    if not os.path.exists(path):
        return out
    text = open(path, encoding="utf-8").read()
    for t in TARGETS:
        m = re.search(r'set\(MC2_PCH_SKIP_%s\n((?:[ \t]+"[^"]*"\n)*)\)' % t, text)
        if m:
            out[t] = re.findall(r'"([^"]+)"', m.group(1))
    return out


def render(result: dict) -> str:
    lines = [HEADER]
    for t in TARGETS:
        lines.append(f"set(MC2_PCH_SKIP_{t}")
        lines += [f'    "{p}"' for p in result.get(t, [])]
        lines.append(")\n")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ref", required=True, help="build tree configured with -DMC2_PCH=OFF (relative to the repo root)")
    ap.add_argument("--pch", required=True, help="build tree configured with -DMC2_PCH=ON")
    ap.add_argument("--out", help="write the skip list here (merged with the entries already there; --replace overwrites)")
    ap.add_argument("--replace", action="store_true", help="with --out: drop the entries already in the file")
    ap.add_argument("--check", action="store_true", help="exit 1 if any object differs (the PCH tree already honours the skip list)")
    args = ap.parse_args(argv)
    ref = args.ref if os.path.isabs(args.ref) else os.path.join(ROOT, args.ref)
    pch = args.pch if os.path.isabs(args.pch) else os.path.join(ROOT, args.pch)
    result = audit(ref, pch)
    for t in TARGETS:
        print(f"pch_audit: {t}: {len(result[t])} object(s) differ")
    if args.out:
        out_path = os.path.join(ROOT, args.out)
        if not args.replace:
            old = existing(out_path)
            result = {t: sorted(set(result[t]) | set(old[t])) for t in TARGETS}
        with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render(result))
        print(f"pch_audit: wrote {args.out}")
    return 1 if args.check and any(result.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
