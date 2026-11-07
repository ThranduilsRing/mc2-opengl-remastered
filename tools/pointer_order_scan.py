#!/usr/bin/env python3
"""pointer_order_scan - where the engine orders or hashes by pointer value (the step 4 determinism hazard list).

WHAT: a grep-grade scan of the committed C++ sources (comments and strings blanked) for the three idioms whose result depends on where an
object was allocated: containers keyed by a pointer (`std::map<T*`, `std::set<T*`, `unordered_map<T*`, `std::less<T*>`, `std::hash<T*>`),
comparators that order two pointer parameters by `<` / `>`, and a pointer cast to an integer for hashing or ordering (`reinterpret_cast<
uintptr_t>`, `(size_t)ptr`-style casts inside a hash function). The committed baseline `tests/unit/pointer_order_baseline.json` holds the
per-file counts; `--check` is a ratchet (a file may not gain a site, a new file may not appear; falls are reported so the baseline can be
lowered with the reason in the commit).
WHY: the ownership step moves objects between allocators and containers, which changes addresses; any order or hash that reads an address
then changes the simulation (the SortList total-order ruling is the same hazard found by hand). The list is what a batch reviewer reads.

  py -3 tools/pointer_order_scan.py --report
  py -3 tools/pointer_order_scan.py --write-baseline
  py -3 tools/pointer_order_scan.py --check
  py -3 tools/pointer_order_scan.py --selftest

Heuristics, not a parser; a false positive is fine as long as it is stable. Stdlib only; reads HEAD through git.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "cxx17_ratchet"))
import cxx17_ratchet as cr  # noqa: E402

BASELINE = os.path.join(ROOT, "tests", "unit", "pointer_order_baseline.json")
DEFAULT_DIRS = ["code", "mclib", "gui", "GameOS", "RenderCore", "RenderWorld", "GameAdapters", "GuiRuntime", "editor", "Gates"]

PATTERNS = {
    # a container ordered or hashed by a pointer key
    "pointer_key": re.compile(r"\b(?:std::)?(?:map|set|multimap|multiset|unordered_map|unordered_set|unordered_multimap|unordered_multiset|less|greater|hash)\s*<\s*(?:const\s+)?[A-Za-z_][\w:]*\s*\*"),
    # a comparator over two pointer parameters that orders the pointers themselves: (T* a, T* b) ... a < b
    "pointer_compare": re.compile(r"\(\s*(?:const\s+)?[A-Za-z_][\w:]*\s*\*\s*(?:const\s+)?(\w+)\s*,\s*(?:const\s+)?[A-Za-z_][\w:]*\s*\*\s*(?:const\s+)?(\w+)\s*\)[^;{]*\{[^}]*?\breturn\s+\(?\s*(?:\1\s*[<>]=?\s*\2|\2\s*[<>]=?\s*\1)\s*\)?\s*;", re.S),
    # a pointer turned into an integer (hashing or ordering by address)
    "pointer_to_int": re.compile(r"reinterpret_cast\s*<\s*(?:std::)?(?:u?intptr_t|size_t|uint64_t|unsigned\s+long\s+long)\s*>\s*\(|\(\s*(?:std::)?(?:u?intptr_t|size_t)\s*\)\s*(?:\(?\s*)?(?:this\b|[A-Za-z_]\w*(?:->|\.)?\w*\s*\)?\s*[\^>%&\)])"),
}


#: A pointer cast to an integer counts only when the line then hashes, mixes or orders it; an alignment mask (`& 15`) or a plain store does not.
_INT_USE = re.compile(r"[%^]|>>|<<|[Hh]ash|[Ss]ort|[Cc]ompare|\bless\b")


def scan_text(text: str) -> dict:
    """-> {idiom: [line numbers]} for one file."""
    clean = "\n".join(cr.strip(text)[1])
    rows = clean.split("\n")
    out = {}
    for name, rx in PATTERNS.items():
        lines = []
        for m in rx.finditer(clean):
            ln = clean.count("\n", 0, m.start()) + 1
            if name == "pointer_to_int" and not _INT_USE.search(rows[ln - 1]):
                continue
            lines.append(ln)
        if lines:
            out[name] = lines
    return out


def scan(blobs: dict) -> dict:
    """-> {rel: {idiom: [lines]}} over the files that have a site."""
    res = {}
    for rel, data in sorted(blobs.items()):
        hit = scan_text(data.decode("utf-8", "replace"))
        if hit:
            res[rel] = hit
    return res


def counts(res: dict) -> dict:
    return {rel: {k: len(v) for k, v in hit.items()} for rel, hit in res.items()}


def check(res: dict, base: dict, out=sys.stdout) -> int:
    """Ratchet per file and idiom. -> 0 ok, 1 a count rose or a new file appeared."""
    cur, bc = counts(res), base.get("files", {})
    rose, fell = [], []
    for rel, hit in cur.items():
        for k, n in hit.items():
            b = bc.get(rel, {}).get(k, 0)
            if n > b:
                rose.append(f"{rel} {k}: {b} -> {n} (lines {', '.join(map(str, res[rel][k][:5]))})")
            elif n < b:
                fell.append(f"{rel} {k}: {b} -> {n}")
    for rel, hit in bc.items():
        for k, b in hit.items():
            if cur.get(rel, {}).get(k, 0) == 0 and b:
                fell.append(f"{rel} {k}: {b} -> 0")
    for line in fell:
        print("pointer_order_scan: lowered (rewrite the baseline): " + line, file=out)
    for line in rose:
        print("pointer_order_scan: FAIL " + line, file=out)
    total = sum(sum(h.values()) for h in cur.values())
    print(f"pointer_order_scan: {'FAIL' if rose else 'ok'} ({total} sites in {len(cur)} files, {len(rose)} rose, {len(fell)} fell)", file=out)
    return 1 if rose else 0


def selftest() -> int:
    import io
    src = """
#include <map>
std::map<GameObject*, int> byObject;          // keyed by address
std::unordered_set<const Node*> seen;
struct ByAddr { bool operator()(const Mover* a, const Mover* b) const { return a < b; } };
static int cmpName(const Mover* a, const Mover* b) { return a->id < b->id; }   // orders by a field, not the pointer
size_t h = reinterpret_cast<uintptr_t>(ptr) >> 4;
std::map<int, Foo*> byId;                     // pointer VALUE, not key: fine
char* aligned = (char*)((size_t)raw & ~15);   // alignment, not ordering: not counted
"""
    fails = []
    hit = scan_text(src)
    if hit.get("pointer_key") != [3, 4]:
        fails.append(f"two pointer-keyed containers expected on lines 3 and 4: {hit.get('pointer_key')}")
    if hit.get("pointer_compare") != [5]:
        fails.append(f"only the comparator that orders the pointers themselves: {hit.get('pointer_compare')}")
    if hit.get("pointer_to_int") != [7]:
        fails.append(f"the reinterpret_cast to uintptr_t: {hit.get('pointer_to_int')}")
    res = scan({"code/a.cpp": src.encode()})
    base = {"files": counts(res)}
    if check(res, base, io.StringIO()) != 0:
        fails.append("equal to the baseline passes")
    more = scan({"code/a.cpp": (src + "\nstd::set<Foo*> extra;\n").encode()})
    if check(more, base, io.StringIO()) != 1:
        fails.append("a new site fails")
    if check(scan({"code/b.cpp": b"std::set<Foo*> extra;"}), base, io.StringIO()) != 1:
        fails.append("a new file fails")
    if check(scan({"code/a.cpp": src.replace("std::unordered_set<const Node*> seen;", "").encode()}), base, io.StringIO()) != 0:
        fails.append("fewer sites pass")
    for f in fails:
        print("pointer_order_scan selftest FAIL: " + f)
    print("pointer_order_scan selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--report", action="store_true")
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--selftest", action="store_true")
    ap.add_argument("--dirs", nargs="+", default=None)
    ap.add_argument("--worktree", action="store_true", help="scan the working tree instead of HEAD")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    base = None
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            base = json.load(fh)
    dirs = args.dirs or (base or {}).get("dirs") or DEFAULT_DIRS
    if args.worktree:
        blobs = {}
        for rel in cr.list_files(dirs):
            with open(os.path.join(ROOT, rel), "rb") as fh:
                blobs[rel] = fh.read()
    else:
        blobs = cr.git_blobs(dirs, "head")
    for rel in cr.INVENTORY_EXEMPT:
        blobs.pop(rel, None)
    res = scan(blobs)
    if args.write_baseline:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"about": "tools/pointer_order_scan.py --write-baseline; per file: sites that order or hash by pointer value",
                       "dirs": dirs, "files": counts(res)}, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print(f"pointer_order_scan: wrote {os.path.relpath(BASELINE, ROOT)} ({sum(sum(h.values()) for h in counts(res).values())} sites in {len(res)} files)")
        return 0
    if args.check:
        if base is None:
            print("pointer_order_scan: no baseline; run --write-baseline")
            return 2
        return check(res, base)
    for rel, hit in res.items():
        for k, lines in hit.items():
            print(f"{rel} {k}: lines {', '.join(map(str, lines))}")
    print(f"{sum(sum(len(v) for v in h.values()) for h in res.values())} sites in {len(res)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
