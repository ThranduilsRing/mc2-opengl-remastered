#!/usr/bin/env python3
"""objhash - prove that an edit left a translation unit's object code unchanged.

WHAT: a digest of the COFF sections that define behaviour (code, data, relocations, the symbol
table), taken from a .obj before an edit and compared with the .obj rebuilt after it.
WHY: comment de-bloat (tools/comment_guard.py) and mechanical clang-tidy rewrites
(tools/tidy/run_tidy_safe.py) both claim "no behaviour change"; identical object sections are the proof
that does not depend on reading the diff.

Excluded on purpose: the COFF header timestamp, the .debug* sections (paths, line tables) and the .chks* incremental-link
checksum (a hash of the source view, not of the code), so a
rebuild at another time, or an edit that only shifts line numbers, hashes the same. A source that uses
__LINE__/__DATE__/__TIME__ still changes when lines shift; callers pass `warn_fn` to downgrade that to
a warning.

  py -3 tools/objhash.py hash BUILD_DIR src.cpp ... > before.json
  ... edit, rebuild ...
  py -3 tools/objhash.py compare before.json

--per-symbol (opt-in, never the default) is a strengthening for objects whose static data is laid out in another order: when a .data / .bss section differs
but the symbol set, every non-data section and every static's bytes and relocation targets are identical by NAME, the object is reported LAYOUT-ONLY instead of
changed. `hash --per-symbol` records what that needs; `diff A.obj B.obj` classifies two objects (IDENTICAL, LAYOUT-ONLY, ORDER-ONLY, DIFF).
"""
from __future__ import annotations

import argparse
import re
import datetime
import glob
import hashlib
import json
import os
import struct
import sys

_DBG_PREFIX = ".debug"
#: MSVC's incremental-link checksum section: a hash of the TU's source text/debug view, not of the code. It changes when only
#: tokens such as NULL -> nullptr change while every code, data and relocation section stays identical.
_NONCODE_SECTIONS = (".debug", ".chks")
#: The build fingerprint literal (cmake/GenerateBuildFingerprint.cmake) carries the dirty flag and the build time, so any TU that embeds it
#: differs between a clean and a dirty tree although no code changed. Its payload is masked (same length) before .rdata is hashed.
_FP_LITERAL = re.compile(rb"MC2FP1\|[ -~]{1,600}?\|FP1END")
_FP_FIXED = len(b"MC2FP1||FP1END")


#: __DATE__ ("Oct 10 2026") and __TIME__ ("19:41:07") string literals: a TU that prints them differs on every compile. Masked, same length.
_DATE_LITERAL = re.compile(rb"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-3][0-9] 20[0-9][0-9]\x00")
_TIME_LITERAL = re.compile(rb"[0-2][0-9]:[0-5][0-9]:[0-5][0-9]\x00")


#: The same two literals as COMDAT symbol names: MSVC names a string literal ??_C@_0<len>@<content hash>@<escaped text>@, so a __DATE__ or
#: __TIME__ literal changes a symbol name even when its bytes are masked. The hash and the escaped text of a date/time-shaped literal are masked.
_MONTHS = "(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
_DATE_SYM = re.compile(r"(\?\?_C@_0(?:[0-9A-P]+@|[0-9]))[0-9A-P]{8}(@" + _MONTHS + r"\?5(?:\?5[0-9]|[0-3][0-9])\?520[0-9]{2}(?:\?\$AA)?@)")
_TIME_SYM = re.compile(r"(\?\?_C@_0(?:[0-9A-P]+@|[0-9]))[0-9A-P]{8}(@[0-2][0-9]\?3[0-5][0-9]\?3[0-5][0-9](?:\?\$AA)?@)")


def _mask_symbol(name):
    return _TIME_SYM.sub(r"\1########@#@", _DATE_SYM.sub(r"\1########@#@", name))


def _mask_fingerprint(data):
    data = _FP_LITERAL.sub(lambda m: b"MC2FP1|" + b"#" * (len(m.group(0)) - _FP_FIXED) + b"|FP1END", data)
    data = _DATE_LITERAL.sub(lambda m: b"#" * (len(m.group(0)) - 1) + b"\x00", data)
    return _TIME_LITERAL.sub(lambda m: b"#" * (len(m.group(0)) - 1) + b"\x00", data)
_SCN_BSS = 0x80
_SCN_NRELOC_OVFL = 0x01000000
_SOURCE_EXTS = (".cpp", ".cc", ".cxx", ".c")

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def read_bytes(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return None


def _h(*parts):
    m = hashlib.sha256()
    for p in parts:
        if isinstance(p, str):
            p = p.encode("utf-8", "replace")
        m.update(struct.pack("<I", len(p)))
        m.update(p)
    return m.hexdigest()


def parse_coff(b):
    """-> (fmt, sections[(name, chars, rawsz, data, relocs)], symbols[(name,value,type,cls,secname)]) or None."""
    try:
        if b[:4] == b"\x00\x00\xff\xff" and len(b) >= 56 and struct.unpack_from("<H", b, 4)[0] >= 2:
            nsec, symptr, nsym = struct.unpack_from("<III", b, 44)
            hdr, symsz, opt, fmt = 56, 20, 0, "coff-bigobj"
        else:
            _m, nsec, _ts, symptr, nsym, opt, _c = struct.unpack_from("<HHIIIHH", b, 0)
            hdr, symsz, fmt = 20, 18, "coff"
        strtab = symptr + nsym * symsz
        strsize = struct.unpack_from("<I", b, strtab)[0] if symptr and strtab + 4 <= len(b) else 0

        def sname(raw):
            if raw[:4] == b"\0\0\0\0":
                off = struct.unpack_from("<I", raw, 4)[0]
                end = b.index(b"\0", strtab + off)
                return b[strtab + off:end].decode("latin-1")
            return raw.split(b"\0", 1)[0].decode("latin-1")

        secs = []
        for k in range(nsec):
            o = hdr + opt + 40 * k
            name, _vs, _va, rawsz, rawptr, relptr, _lp, nrel, _nl, chars = struct.unpack_from("<8sIIIIIIHHI", b, o)
            nm = name.rstrip(b"\0").decode("latin-1")
            if nm.startswith("/") and nm[1:].isdigit():
                off = int(nm[1:])
                nm = b[strtab + off:b.index(b"\0", strtab + off)].decode("latin-1")
            data = b"" if (chars & _SCN_BSS or not rawptr) else b[rawptr:rawptr + rawsz]
            if chars & _SCN_NRELOC_OVFL and nrel == 0xFFFF and relptr:
                nrel = struct.unpack_from("<I", b, relptr)[0]
            rel = b[relptr:relptr + 10 * nrel] if relptr and nrel else b""
            secs.append((nm, chars, rawsz, data, rel))
        syms, i = [], 0
        while i < nsym:
            o = symptr + i * symsz
            if fmt == "coff-bigobj":
                nm_raw, val, secno, typ, cls, naux = struct.unpack_from("<8sIiHBB", b, o)
            else:
                nm_raw, val, secno, typ, cls, naux = struct.unpack_from("<8sIhHBB", b, o)
            sec = secs[secno - 1][0] if 0 < secno <= len(secs) else "#%d" % secno
            syms.append((sname(nm_raw), val, typ, cls, sec))
            i += 1 + naux
        return fmt, secs, syms
    except (struct.error, ValueError, IndexError):
        return None


_NUL4 = b"\0\0\0\0"


def symbol_table(raw):
    """-> {symbol table index: (name, section number, storage class, value)}; aux records occupy an index, as in the COFF relocation entries."""
    if raw[:4] == b"\x00\x00\xff\xff" and len(raw) >= 56 and struct.unpack_from("<H", raw, 4)[0] >= 2:
        _n, symptr, nsym = struct.unpack_from("<III", raw, 44)
        symsz, fmt = 20, "<8sIiHBB"
    else:
        _m, _n, _ts, symptr, nsym, _o, _c = struct.unpack_from("<HHIIIHH", raw, 0)
        symsz, fmt = 18, "<8sIhHBB"
    strtab = symptr + nsym * symsz
    out, i = {}, 0
    while i < nsym:
        nm_raw, val, secno, _typ, cls, naux = struct.unpack_from(fmt, raw, symptr + i * symsz)
        if nm_raw[:4] == _NUL4:
            off = struct.unpack_from("<I", nm_raw, 4)[0]
            name = raw[strtab + off:raw.index(b"\0", strtab + off)].decode("latin-1")
        else:
            name = nm_raw.split(b"\0", 1)[0].decode("latin-1")
        out[i] = (name, secno, cls, val)
        i += 1 + naux
    return out


#: Sections whose symbols are laid out by the compiler in an order that is not behaviour (static data of one translation unit).
LAYOUT_SECTIONS = (".data", ".bss")


def _reloc_labels(rel, table, sec_label):
    """Relocations as (offset, target, type) where the target is the symbol NAME (a section symbol: the content label of the section it names), not its table index."""
    out = []
    for addr, idx, typ in struct.iter_unpack("<IIH", rel[:len(rel) - len(rel) % 10]):
        name, secno, cls, _v = table.get(idx, ("#%d" % idx, 0, 0, 0))
        label = ("SECSYM:" + sec_label.get(secno, name)) if cls == 3 and name.startswith(".") else _mask_symbol(name)
        out.append((addr, label, typ))
    return out


def persym_digest(raw):
    """Name-keyed digest of a COFF object, or None when it does not parse.

    WHAT: {symset: hash of the (name, type, class, section) set without offsets, sections: {section name: hash of the multiset of that name's
    sections with relocations read by target name}, data: {".data": {symbol: hash(bytes, relocations by name)}, ".bss": {symbol: ""}}}.
    WHY: the compiler may lay the file-scope statics of one translation unit out in another order after an unrelated edit; every .data / .bss section
    hash and every symbol-table index then moves although no symbol, no byte of any symbol and no relocation target changed. Per-symbol bytes stop at the
    next symbol's offset and drop trailing zeros (alignment padding). A .bss symbol has no bytes in an object file and its size is not recorded:
    .bss is compared by symbol set only."""
    parsed = parse_coff(raw)
    if not parsed:
        return None
    _fmt, secs, syms = parsed
    table = symbol_table(raw)
    sec_label = {k + 1: (nm if nm in LAYOUT_SECTIONS else _h(nm, str(chars), str(rawsz), data))
                 for k, (nm, chars, rawsz, data, _rel) in enumerate(secs)}
    by_sec = {}
    for name, secno, cls, val in table.values():
        if secno > 0 and cls in (2, 3) and secs[secno - 1][0] in LAYOUT_SECTIONS and not name.startswith("."):
            by_sec.setdefault(secno, []).append((val, name))
    sections, data_syms = {}, {nm: {} for nm in LAYOUT_SECTIONS}
    for k, (nm, chars, rawsz, data, rel) in enumerate(secs):
        if nm.startswith(_NONCODE_SECTIONS):
            continue
        labels = _reloc_labels(rel, table, sec_label)
        if nm in LAYOUT_SECTIONS:
            members = sorted(by_sec.get(k + 1, []))
            for i, (v, name) in enumerate(members):
                if nm == ".bss":
                    data_syms[nm][name] = ""
                    continue
                end = next((w for w, _n in members[i + 1:] if w > v), len(data))
                mine = sorted((a - v, lab, t) for a, lab, t in labels if v <= a < end)
                data_syms[nm][name] = _h(name, data[v:end].rstrip(bytes(1)), repr(mine))
            continue
        if nm.startswith(".rdata"):
            data = _mask_fingerprint(data)
        sections.setdefault(nm, []).append(_h(nm, str(chars), str(rawsz), data, repr(sorted(labels))))
    return {"symset": _h(*sorted(_mask_symbol("%s|%d|%d|%s" % (s[0], s[2], s[3], s[4]))
                                  for s in syms if not s[4].startswith(_DBG_PREFIX))),
            "sections": {nm: _h(*sorted(hs)) for nm, hs in sections.items()},
            "data": data_syms}


def persym_diff(a, b):
    """Reasons two persym_digest results differ; an empty list means the symbol set, every non-layout section and every static's bytes are identical."""
    out = []
    if a["symset"] != b["symset"]:
        out.append("symbol set")
    for nm in sorted(set(a["sections"]) | set(b["sections"])):
        if a["sections"].get(nm) != b["sections"].get(nm):
            out.append("section %s" % nm)
    for nm in LAYOUT_SECTIONS:
        sa, sb = a["data"].get(nm, {}), b["data"].get(nm, {})
        for name in sorted(set(sa) | set(sb)):
            if sa.get(name) != sb.get(name):
                out.append("%s symbol %s (%s)" % (nm, name, "only before" if name not in sb else "only after" if name not in sa else "bytes"))
    return out


def classify(plain_a, plain_b, ps_a, ps_b):
    """-> (IDENTICAL | LAYOUT-ONLY | ORDER-ONLY | DIFF, reasons).
    IDENTICAL: the plain digests agree. LAYOUT-ONLY: a .data / .bss section differs but the symbol set and every symbol's bytes agree (name-keyed).
    ORDER-ONLY: only symbol-table / relocation index order differs. Neither of the last two is identical; DIFF carries the first reasons."""
    if plain_a["code_sha256"] == plain_b["code_sha256"]:
        return "IDENTICAL", []
    if not ps_a or not ps_b:
        return "DIFF", ["no per-symbol record"]
    reasons = persym_diff(ps_a, ps_b)
    if reasons:
        return "DIFF", reasons
    differing = set(k for k in set(plain_a["sections"]) | set(plain_b["sections"]) if plain_a["sections"].get(k) != plain_b["sections"].get(k))
    layout = sorted(k for k in differing if k.split(":", 1)[-1] in LAYOUT_SECTIONS)
    if layout:
        return "LAYOUT-ONLY", layout
    return "ORDER-ONLY", sorted(differing)[:6]


def obj_digest(path, per_symbol=False):
    """Behavioural digest of an object file; see the module docstring for what is excluded. `per_symbol` adds the name-keyed record used by the LAYOUT-ONLY fallback."""
    raw = read_bytes(path)
    if raw is None:
        return None
    info = {"raw_sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}
    parsed = parse_coff(raw)
    if not parsed:
        info.update(format="raw", code_sha256=info["raw_sha256"], sections={})
        return info
    if per_symbol:
        info["persym"] = persym_digest(raw)
    fmt, secs, syms = parsed
    sec_hashes = {}
    for k, (nm, chars, rawsz, data, rel) in enumerate(secs):
        if nm.startswith(_NONCODE_SECTIONS):
            continue
        if nm.startswith(".rdata"):
            data = _mask_fingerprint(data)
        sec_hashes["%d:%s" % (k, nm)] = _h(nm, str(chars), str(rawsz), data, rel)
    sym_part = _h(*[_mask_symbol("%s|%d|%d|%d|%s" % s) for s in syms if not s[4].startswith(_DBG_PREFIX)])
    sec_hashes["symbols"] = sym_part
    info.update(format=fmt, sections=sec_hashes,
                code_sha256=_h(*["%s=%s" % kv for kv in sorted(sec_hashes.items())]))
    return info


def find_objs(build_dir, rel):
    bd = glob.escape(build_dir.replace("\\", "/"))
    r = glob.escape(rel)
    hits = glob.glob("%s/CMakeFiles/*/%s.obj" % (bd, r))
    if not hits:
        base = glob.escape(os.path.basename(rel))
        stem = glob.escape(os.path.splitext(os.path.basename(rel))[0])
        for pat in ("%s/**/%s.obj" % (bd, base), "%s/**/%s.obj" % (bd, stem)):
            hits += glob.glob(pat, recursive=True)
    objs = sorted(set(os.path.abspath(h) for h in hits))
    if len(objs) > 1:
        # Two sources can share a basename (mclib/color.cpp, mclib/stuff/color.cpp): keep the object built in the
        # source's own directory, i.e. the path in front of CMakeFiles/ ends with the source's directory.
        want = os.path.dirname(rel).replace("\\", "/").rstrip("/")
        own = [o for o in objs
               if o.replace("\\", "/").split("/CMakeFiles/")[0].endswith("/" + want) or (not want)]
        if own:
            objs = own
    return objs


def stamp(path):
    return os.stat(path).st_mtime_ns


def hash_files(repo, build_dir, rels, tool="objhash", warn_fn=None, per_symbol=False):
    """-> (document, problems[str]). `warn_fn(repo, rel)` returns a warning string or None. `per_symbol` records the name-keyed digest `compare --per-symbol` needs."""
    doc = {"tool": tool, "version": 1,
           "created": datetime.datetime.now().isoformat(timespec="seconds"),
           "repo": repo.replace("\\", "/"), "build_dir": os.path.abspath(build_dir).replace("\\", "/"),
           "files": {}}
    problems = []
    for rel in rels:
        ent = {}
        doc["files"][rel] = ent
        if not rel.lower().endswith(_SOURCE_EXTS):
            ent.update(status="skip", reason="no object of its own (header/shader): rely on --check")
            continue
        src = os.path.join(repo, rel)
        if not os.path.exists(src):
            problems.append("%s: source missing" % rel)
            ent.update(status="error", reason="source missing")
            continue
        objs = find_objs(build_dir, rel)
        if not objs:
            problems.append("%s: no object under %s - build first" % (rel, build_dir))
            ent.update(status="error", reason="no object found")
            continue
        ent["warn"] = warn_fn(repo, rel) if warn_fn else None
        ent["objs"] = []
        ent["status"] = "ok"
        for o in objs:
            if stamp(o) < stamp(src):
                problems.append("%s: %s is older than the source - not a base build" % (rel, o))
                ent["status"] = "error"
                ent["reason"] = "object older than source"
            d = obj_digest(o, per_symbol=True) if per_symbol else obj_digest(o)
            d.update(path=o.replace("\\", "/"), mtime_ns=stamp(o))
            ent["objs"].append(d)
    return doc, problems


def compare_files(before, per_symbol=False):
    """-> (report lines, number of failures) for a document produced by hash_files.
    With `per_symbol` an object whose plain digest differs is classified by name (see `classify`): LAYOUT-ONLY is not a failure, it is reported as its own line."""
    repo = before.get("repo") or ROOT
    out, bad = [], 0
    for rel, ent in sorted(before["files"].items()):
        if ent.get("status") == "skip":
            out.append("SKIP  %s  (%s)" % (rel, ent["reason"]))
            continue
        if ent.get("status") != "ok":
            out.append("FAIL  %s  unusable 'before' record: %s" % (rel, ent.get("reason")))
            bad += 1
            continue
        src = os.path.join(repo, rel)
        warn = ent.get("warn")
        for o in ent["objs"]:
            p = o["path"]
            if not os.path.exists(p):
                out.append("FAIL  %s  object vanished: %s" % (rel, p))
                bad += 1
                continue
            if stamp(p) == o["mtime_ns"]:
                out.append("FAIL  %s  object not rebuilt since --hash-objs: %s" % (rel, p))
                bad += 1
                continue
            if os.path.exists(src) and stamp(p) < stamp(src):
                out.append("FAIL  %s  object older than the edited source - rebuild: %s" % (rel, p))
                bad += 1
                continue
            now = obj_digest(p)
            if now["code_sha256"] == o["code_sha256"]:
                out.append("OK    %s  behaviour sections identical (%s)" % (rel, os.path.basename(p)))
                continue
            diff = sorted(k for k in set(o["sections"]) | set(now["sections"])
                          if o["sections"].get(k) != now["sections"].get(k))
            if per_symbol:
                verdict, why = classify(o, now, o.get("persym"), persym_digest(read_bytes(p)))
                if verdict == "LAYOUT-ONLY":
                    out.append("LAYOUT-ONLY  %s  %s layout reordered; symbol set and every symbol's bytes identical (%s)" % (rel, ", ".join(why), os.path.basename(p)))
                    continue
                if verdict == "ORDER-ONLY":
                    out.append("FAIL  %s  symbol/relocation index order only (per-symbol identical, no .data/.bss reorder): %s" % (rel, ", ".join(why)))
                    bad += 1
                    continue
                out.append("FAIL  %s  per-symbol: %s" % (rel, "; ".join(why[:6]) + (" ..." if len(why) > 6 else "")))
                bad += 1
                continue
            detail = "sections differ: %s%s" % (", ".join(diff[:6]), " ..." if len(diff) > 6 else "")
            if warn:
                out.append("WARN  %s  %s; %s" % (rel, detail, warn))
            else:
                out.append("FAIL  %s  %s" % (rel, detail))
                bad += 1
    return out, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description="object-code identity proof")
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hash", help="digest the objects of the given sources (JSON on stdout)")
    h.add_argument("build_dir")
    h.add_argument("paths", nargs="+")
    h.add_argument("--repo", default=ROOT)
    h.add_argument("--per-symbol", action="store_true", help="also record the name-keyed digest that compare --per-symbol needs")
    c = sub.add_parser("compare", help="compare rebuilt objects with a saved digest")
    c.add_argument("before")
    c.add_argument("--per-symbol", action="store_true",
                   help="report LAYOUT-ONLY (not a failure) when a .data/.bss section differs but the symbol set and every symbol's bytes are identical")
    d = sub.add_parser("diff", help="classify two object files: IDENTICAL, LAYOUT-ONLY, ORDER-ONLY or DIFF (exit 0 for the first two)")
    d.add_argument("a")
    d.add_argument("b")
    args = ap.parse_args(argv)
    if args.cmd == "hash":
        repo = os.path.abspath(args.repo)
        rels = [os.path.relpath(os.path.abspath(p), repo).replace("\\", "/") for p in args.paths]
        doc, problems = hash_files(repo, args.build_dir, rels, per_symbol=args.per_symbol)
        json.dump(doc, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
        for pr in problems:
            print("objhash: " + pr, file=sys.stderr)
        return 2 if problems else 0
    if args.cmd == "diff":
        da, db = obj_digest(args.a, per_symbol=True), obj_digest(args.b, per_symbol=True)
        if da is None or db is None:
            print("objhash: unreadable object", file=sys.stderr)
            return 2
        verdict, why = classify(da, db, da.get("persym"), db.get("persym"))
        print("%s %s" % (verdict, "; ".join(why[:8])))
        return 0 if verdict in ("IDENTICAL", "LAYOUT-ONLY") else 1
    with open(args.before, "r", encoding="utf-8") as fh:
        lines, bad = compare_files(json.load(fh), per_symbol=args.per_symbol)
    print("\n".join(lines))
    layout = sum(1 for ln in lines if ln.startswith("LAYOUT-ONLY"))
    print("objhash: object compare, %d problem(s)%s" % (bad, ", %d layout-only" % layout if layout else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
