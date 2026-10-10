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
"""
from __future__ import annotations

import argparse
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


def obj_digest(path):
    """Behavioural digest of an object file; see the module docstring for what is excluded."""
    raw = read_bytes(path)
    if raw is None:
        return None
    info = {"raw_sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}
    parsed = parse_coff(raw)
    if not parsed:
        info.update(format="raw", code_sha256=info["raw_sha256"], sections={})
        return info
    fmt, secs, syms = parsed
    sec_hashes = {}
    for k, (nm, chars, rawsz, data, rel) in enumerate(secs):
        if nm.startswith(_NONCODE_SECTIONS):
            continue
        sec_hashes["%d:%s" % (k, nm)] = _h(nm, str(chars), str(rawsz), data, rel)
    sym_part = _h(*["%s|%d|%d|%d|%s" % s for s in syms if not s[4].startswith(_DBG_PREFIX)])
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
    return sorted(set(os.path.abspath(h) for h in hits))


def stamp(path):
    return os.stat(path).st_mtime_ns


def hash_files(repo, build_dir, rels, tool="objhash", warn_fn=None):
    """-> (document, problems[str]). `warn_fn(repo, rel)` returns a warning string or None."""
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
            d = obj_digest(o)
            d.update(path=o.replace("\\", "/"), mtime_ns=stamp(o))
            ent["objs"].append(d)
    return doc, problems


def compare_files(before):
    """-> (report lines, number of failures) for a document produced by hash_files."""
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
    c = sub.add_parser("compare", help="compare rebuilt objects with a saved digest")
    c.add_argument("before")
    args = ap.parse_args(argv)
    if args.cmd == "hash":
        repo = os.path.abspath(args.repo)
        rels = [os.path.relpath(os.path.abspath(p), repo).replace("\\", "/") for p in args.paths]
        doc, problems = hash_files(repo, args.build_dir, rels)
        json.dump(doc, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
        for pr in problems:
            print("objhash: " + pr, file=sys.stderr)
        return 2 if problems else 0
    with open(args.before, "r", encoding="utf-8") as fh:
        lines, bad = compare_files(json.load(fh))
    print("\n".join(lines))
    print("objhash: object compare, %d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
