#!/usr/bin/env python3
"""objhash_all - the objhash proof over EVERY translation unit of the all-targets tree, with a summary that adds up.

WHAT: `hash` records the per-symbol object digest (tools/objhash.py) of every source named in BUILD_DIR/compile_commands.json; `compare` classifies each
object after a rebuild as NOT-REBUILT (no input changed), IDENTICAL, LAYOUT-ONLY, ORDER-ONLY or DIFF, writes a summary json whose class counts add up to the number
of objects, and lists every object that is not IDENTICAL or NOT-REBUILT with its reasons. `text` compares the executable sections of two objects with digits in
relocation target names normalised (the Tracy zone names carry __LINE__, mangled names carry a local-static scope index).
WHY: a refactor that claims "no code change" needs a proof that can be re-read: one table, classes that partition the objects, the exceptions named. The B2.1 commit
message quoted counts that did not add up and kept no raw output (review M3); this is the replacement.

  py -3 tests/analyze/objhash_all.py hash BUILD_DIR REPO OUT.json
  ... edit, rebuild the same tree ...
  py -3 tests/analyze/objhash_all.py compare OUT.json --summary SUMMARY.json
  py -3 tests/analyze/objhash_all.py text OBJ_BEFORE OBJ_AFTER
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import objhash  # noqa: E402


def sources(build_dir: str, repo: str) -> list[str]:
    """The sources of the tree, as repo-relative paths: every `file` of compile_commands.json under the repo, once."""
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)
    repo_norm = os.path.abspath(repo).replace("\\", "/").rstrip("/") + "/"
    rels = set()
    for e in entries:
        path = os.path.abspath(os.path.join(e.get("directory", build_dir), e["file"])).replace("\\", "/")
        if path.lower().startswith(repo_norm.lower()):
            rels.add(path[len(repo_norm):])
    return sorted(r for r in rels if r.lower().endswith((".c", ".cc", ".cpp", ".cxx")))


def cmd_hash(args) -> int:
    doc, problems = objhash.hash_files(args.repo, args.build_dir, sources(args.build_dir, args.repo), per_symbol=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    n_objs = sum(len(e.get("objs", [])) for e in doc["files"].values())
    print(f"objhash_all: {len(doc['files'])} sources, {n_objs} objects hashed, {len(problems)} problem(s) -> {args.out}")
    for p in problems[:20]:
        print("  " + p)
    return 1 if problems else 0


def cmd_compare(args) -> int:
    with open(args.before, encoding="utf-8") as f:
        before = json.load(f)
    counts: collections.Counter = collections.Counter()
    exceptions = []
    total = 0
    for rel, ent in sorted(before["files"].items()):
        for o in ent.get("objs", []):
            total += 1
            path = o["path"]
            if not os.path.exists(path):
                counts["VANISHED"] += 1
                exceptions.append({"source": rel, "object": path, "class": "VANISHED", "why": []})
                continue
            if os.stat(path).st_mtime_ns == o["mtime_ns"]:
                counts["NOT-REBUILT"] += 1
                continue
            now = objhash.obj_digest(path)
            verdict, why = objhash.classify(o, now, o.get("persym"), objhash.persym_digest(objhash.read_bytes(path)))
            counts[verdict] += 1
            if verdict != "IDENTICAL":
                exceptions.append({"source": rel, "object": path, "class": verdict, "why": why[:12]})
    assert sum(counts.values()) == total
    summary = {"objects": total, "classes": dict(sorted(counts.items())), "not_identical": exceptions}
    with open(args.summary, "w", encoding="utf-8", newline="\n") as f:
        json.dump(summary, f, indent=1)
        f.write("\n")
    print(f"objhash_all: {total} objects = " + " + ".join(f"{n} {c}" for c, n in sorted(counts.items())))
    for e in exceptions:
        print(f"  {e['class']:12s} {e['source']}  {'; '.join(e['why'][:3])}")
    return 0


_DIGITS = re.compile(r"\d+")
#: the local-static scope index of a mangled name: `...@?CA@??update@Terrain@@...` (base-16-like letters, one per enclosing scope)
_SCOPE = re.compile(r"@\?[0-9A-Z]{1,3}@\?\?")
_RELOC_SIZE = {1: 8, 0xA: 2}      # IMAGE_REL_AMD64_ADDR64 is 8 bytes, SECTION 2; every other type the compiler emits here patches 4 bytes


def _resolve(table, by_section, idx, addend):
    """The target of a relocation as a NAME: a section symbol plus an inline addend is the static data symbol that starts at or before the addend."""
    name, secno, cls, _val = table.get(idx, ("#%d" % idx, 0, 0, 0))
    if cls == 3 and name.startswith(".") and secno in by_section:
        best = None
        for value, sym in by_section[secno]:
            if value > addend:
                break
            best = (value, sym)
        if best:
            return "%s+%d" % (best[1], addend - best[0])
        return "%s%d+%d" % (name, secno, addend)
    return name


def text_sections(path: str, normalise: bool):
    """-> list of (primary symbol name, bytes with the relocation fields zeroed, sorted (offset, target name, type)) for the executable sections of an object.
    The inline addend of a relocation into static data depends on where the linker-visible layout put the symbol, so it is resolved to the symbol's NAME and masked."""
    raw = objhash.read_bytes(path)
    _fmt, secs, _syms = objhash.parse_coff(raw)
    table = objhash.symbol_table(raw)
    by_section = collections.defaultdict(list)
    primary = {}
    for _i, (name, secno, cls, val) in table.items():
        if secno > 0 and cls in (2, 3) and not name.startswith("."):
            by_section[secno].append((val, name))
        if secno > 0 and cls == 2 and val == 0:
            primary.setdefault(secno, name)
    for lst in by_section.values():
        lst.sort()
    out = []
    for k, (nm, _chars, _rawsz, data, rel) in enumerate(secs):
        if not nm.startswith(".text"):
            continue
        buf, labels = bytearray(data), []
        for addr, idx, typ in struct.iter_unpack("<IIH", rel[:len(rel) - len(rel) % 10]):
            size = _RELOC_SIZE.get(typ, 4)
            label = _resolve(table, by_section, idx, int.from_bytes(buf[addr:addr + size], "little"))
            labels.append((addr, _DIGITS.sub("#", _SCOPE.sub("@?#@??", label)) if normalise else label, typ))
            buf[addr:addr + size] = bytes(size)
        out.append((primary.get(k + 1, "?"), bytes(buf), tuple(sorted(labels))))
    return out


def cmd_text(args) -> int:
    a, b = text_sections(args.a, args.normalise), text_sections(args.b, args.normalise)
    ca, cb = collections.Counter((e[1], e[2]) for e in a), collections.Counter((e[1], e[2]) for e in b)
    only_a = [e for e in a if (ca - cb)[(e[1], e[2])] > 0]
    only_b = [e for e in b if (cb - ca)[(e[1], e[2])] > 0]
    print(f"objhash_all text: {len(a)} / {len(b)} executable sections; {len(only_a)} only before, {len(only_b)} only after "
          f"(relocation fields masked and resolved to symbol names{', digits and local-static scope indices in those names normalised' if args.normalise else ''})")
    for ea in only_a:
        for eb in only_b:
            if eb[0] != ea[0]:
                continue
            if len(ea[1]) != len(eb[1]):
                print(f"  {ea[0][:70]}: {len(ea[1])} bytes before, {len(eb[1])} after")
                continue
            diffs = [i for i, (x, y) in enumerate(zip(ea[1], eb[1])) if x != y]
            runs = collections.Counter()
            for i in diffs:
                runs[(ea[1][i], eb[1][i])] += 1
            words = sorted({(int.from_bytes(ea[1][i:i + 4], "little"), int.from_bytes(eb[1][i:i + 4], "little")) for i in diffs}) if len(diffs) <= 8 else []
            print(f"  {ea[0][:70]}: {len(diffs)} differing bytes of {len(ea[1])}; relocations {'identical' if ea[2] == eb[2] else 'DIFFER'}"
                  + (f"; 4-byte words before/after {words}" if words else ""))
    return 0 if not only_a and not only_b else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hash")
    h.add_argument("build_dir")
    h.add_argument("repo")
    h.add_argument("out")
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("--summary", required=True)
    t = sub.add_parser("text")
    t.add_argument("a")
    t.add_argument("b")
    t.add_argument("--normalise", action="store_true")
    args = ap.parse_args()
    return {"hash": cmd_hash, "compare": cmd_compare, "text": cmd_text}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
