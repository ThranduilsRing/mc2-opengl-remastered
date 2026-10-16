#!/usr/bin/env python3
"""b4_heap_census - the sealed census of the C++23 step 4 batch B4 (raw-buffer heaps): every hand-paired buffer allocation and release in the B4 subsystems.

WHAT: scans the tracked files of each B4 sub-batch (globs in BATCHES) for the six allocation kinds the batch replaces with owned storage: a named heap's
`Malloc` / `Free` (`terrainHeap->Malloc(`, `systemHeap->Free(`, also through `.`), C `malloc` / `calloc` / `realloc`, C `free`, `new T[` and `delete[]`. Comment
text is ignored. One line per site goes to the committed list `.claude/cleanup/cxx23-step4-b4-callsites.txt`; `--check` compares the tree against it.
WHY: the B4 notes' first census was a hand grep that nobody could re-run (cleanup review 2026-10-15). A committed list plus a check makes the census sealed: a
sub-batch's commit shrinks the list by intent (`--write` in the same commit), and a site that appears or vanishes outside a batch fails `--check`.
Not a compiler census: the heap calls are always spelled `<heap>->Malloc(` / `->Free(` (UserHeap has no other entry point), so text finds them all; the
C heap calls and array new/delete cannot be deprecated in a probe build anyway.

  py -3 tests/analyze/b4_heap_census.py            # summary: per batch, per kind, per heap Malloc / Free
  py -3 tests/analyze/b4_heap_census.py --check    # exit 1 when the tree differs from the committed list
  py -3 tests/analyze/b4_heap_census.py --write    # rewrite the committed list (a batch commit does this by intent)
"""
from __future__ import annotations

import argparse
import collections
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LIST = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b4-callsites.txt")

#: sub-batch -> tracked-path globs (cxx23-step4-b4-notes.md section 2). Order is the batch order.
BATCHES = {
    "B4.1-move": ["mclib/move.cpp", "mclib/move.h", "mclib/move_*.cpp", "mclib/move_*.h"],
    "B4.2-terrain": ["mclib/terrain.cpp", "mclib/terrain.h", "mclib/terrain_*.cpp", "mclib/terrain_*.h", "mclib/terrtxm*.cpp", "mclib/terrtxm*.h",
                     "mclib/mapdata.cpp", "mclib/mapdata.h"],
    "B4.3-txm": ["mclib/txmmgr*.cpp", "mclib/txmmgr*.h"],
    "B4.4-sound": ["mclib/soundsys.cpp", "mclib/soundsys.h", "code/gamesound.cpp", "code/gamesound.h", "code/radio.cpp", "code/radio.h"],
    "B4.5-crater": ["mclib/crater.cpp", "mclib/crater.h"],
    "B4.6-collision": ["code/collsn.cpp", "code/collsn.h"],
}

KINDS = [
    ("heap_malloc", re.compile(r"\b(\w+)\s*(?:->|\.)\s*Malloc\s*\(")),
    ("heap_free", re.compile(r"\b(\w+)\s*(?:->|\.)\s*Free\s*\(")),
    ("c_alloc", re.compile(r"(?<![\w.>:])(malloc|calloc|realloc)\s*\(")),
    ("c_free", re.compile(r"(?<![\w.>:])free\s*\(")),
    ("new_array", re.compile(r"\bnew\s+[\w:<>, ]+?\s*\[")),
    ("delete_array", re.compile(r"\bdelete\s*\[\s*\]")),
]


def strip_comments(text: str) -> list[str]:
    """Blank out // and /* */ comments and string literals, keeping line numbers."""
    out, i, n = [], 0, len(text)
    buf = []
    in_block = False
    while i < n:
        c = text[i]
        if in_block:
            if text.startswith("*/", i):
                in_block, i = False, i + 2
                continue
            buf.append("\n" if c == "\n" else " ")
            i += 1
        elif text.startswith("/*", i):
            in_block, i = True, i + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            buf.append(c + " " * max(0, min(j, n) - i - 1) + (c if j < n and text[j] == c else ""))
            i = j + 1 if j < n and text[j] == c else j
        else:
            buf.append(c)
            i += 1
    return "".join(buf).split("\n")


def tracked() -> list[str]:
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "code", "mclib"], capture_output=True, text=True, check=True).stdout
    return [p for p in out.splitlines() if p]


def census() -> list[str]:
    files = tracked()
    rows = []
    for batch, globs in BATCHES.items():
        for path in sorted(p for p in files if any(fnmatch.fnmatchcase(p, g) for g in globs)):
            with open(os.path.join(ROOT, path), encoding="latin-1") as f:
                raw = f.read().replace("\r\n", "\n")
            for lineno, line in enumerate(strip_comments(raw), 1):
                for kind, rx in KINDS:
                    for m in rx.finditer(line):
                        heap = m.group(1) if kind.startswith("heap_") else "-"
                        rows.append(f"{batch}\t{path}:{lineno}\t{kind}\t{heap}\t{raw.split(chr(10))[lineno - 1].strip()[:120]}")
    return rows


def summary(rows: list[str]) -> None:
    per_batch = collections.Counter(r.split("\t")[0] for r in rows)
    files = {r.split("\t")[1].rsplit(":", 1)[0] for r in rows}
    print(f"b4_heap_census: {len(rows)} sites in {len(files)} files")
    for b in BATCHES:
        kinds = collections.Counter(r.split("\t")[2] for r in rows if r.startswith(b + "\t"))
        print(f"  {b:16s} {per_batch[b]:4d}  " + " ".join(f"{k}={kinds[k]}" for k, _ in KINDS if kinds[k]))
    heaps = collections.defaultdict(lambda: [0, 0])
    for r in rows:
        _, _, kind, heap, _ = r.split("\t", 4)
        if kind.startswith("heap_"):
            heaps[heap][0 if kind == "heap_malloc" else 1] += 1
    print("  heap Malloc / Free: " + ", ".join(f"{h} {a}/{b}" for h, (a, b) in sorted(heaps.items(), key=lambda kv: -sum(kv[1]))))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)
    rows = census()
    if args.write:
        with open(LIST, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(rows) + "\n")
    summary(rows)
    if args.check:
        # Line numbers move with any edit above a site, so the check compares the sites without their line numbers.
        def key(r: str) -> str:
            batch, loc, rest = r.split("\t", 2)
            return f"{batch}\t{loc.rsplit(':', 1)[0]}\t{rest}"
        with open(LIST, encoding="utf-8") as f:
            sealed = collections.Counter(key(r) for r in f.read().splitlines() if r)
        now = collections.Counter(key(r) for r in rows)
        gone, new = sealed - now, now - sealed
        for r in sorted(gone.elements()):
            print(f"  GONE {r}")
        for r in sorted(new.elements()):
            print(f"  NEW  {r}")
        if gone or new:
            print(f"b4_heap_census: FAIL ({sum(gone.values())} gone, {sum(new.values())} new): rewrite the list with --write in the batch commit")
            return 1
        print("b4_heap_census: PASS (tree matches the sealed list)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
