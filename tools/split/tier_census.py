#!/usr/bin/env python3
"""What: a per-file census of the 1000-1500 line tier (the file-size ratchet's file set): function count, largest function, non-function units, and a split verdict.
Why: the next split candidate should come from a table, not from re-reading files; the verdict is a heuristic (it cannot see whether the functions form groups), so CHEAP means
"worth a look", not "cut here".
Verdicts: PIECE (already a piece of a split: a sibling <stem>_internal.h exists; PIECE+PHASE when one function is over 55%), PHASE (one function over 55% of the file),
CHEAP (12+ functions, none over 35%), MID (anything else with functions), LEFT (header, shader, or no functions).
Usage: py -3 tools/split/tier_census.py [--min 1000] [--max 1500]   (prints a markdown table; reads the committed tree's working copy)"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
import cpp_units          # noqa: E402
import file_size_ratchet  # noqa: E402


def tracked():
    out = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True).stdout.splitlines()
    pats = file_size_ratchet.BUILTIN_EXCLUDES
    return [r for r in out if file_size_ratchet.wanted(r, file_size_ratchet.DEFAULT_DIRS, pats)]


def row(rel, stems):
    path = os.path.join(REPO, rel)
    n = file_size_ratchet.count_lines(open(path, "rb").read())
    base = os.path.basename(rel)
    d = os.path.dirname(rel)
    piece_of = next((s for s in stems.get(d, ()) if base.startswith(s + "_") or base == s + ".cpp"), None)
    if not rel.endswith((".cpp", ".cc")):
        return n, 0, 0, 0, "LEFT (" + ("header" if rel.endswith((".h", ".hpp", ".inl")) else "shader") + ")"
    _, units = cpp_units.parse(path)
    funcs = [u for u in units if u["kind"] == "func"]
    big = max([u["end"] - u["start"] + 1 for u in funcs] or [0])
    other = sum(1 for u in units if u["kind"] not in ("func", "pp", "tail"))
    pct = 100 * big // max(n, 1)
    if pct >= 55:
        verdict = "PHASE"
    elif len(funcs) >= 12 and pct < 35:
        verdict = "CHEAP"
    elif funcs:
        verdict = "MID"
    else:
        verdict = "LEFT"
    if piece_of:
        verdict = "PIECE+PHASE" if verdict == "PHASE" else "PIECE"
    return n, len(funcs), big, other, verdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=int, default=1000)
    ap.add_argument("--max", type=int, default=1500)
    a = ap.parse_args()
    files = tracked()
    stems = {}
    for r in files:
        if r.endswith("_internal.h"):
            stems.setdefault(os.path.dirname(r), set()).add(os.path.basename(r)[:-len("_internal.h")])
    rows = []
    for r in files:
        n = file_size_ratchet.count_lines(open(os.path.join(REPO, r), "rb").read())
        if a.min <= n <= a.max:
            rows.append((r,) + row(r, stems))
    rows.sort(key=lambda x: (x[0].split("/")[0], -x[1]))
    print("| File | Lines | Funcs | Largest fn | Other units | Verdict |")
    print("|---|---:|---:|---:|---:|---|")
    for r, n, f, big, other, v in rows:
        print("| `%s` | %d | %d | %d (%d%%) | %d | %s |" % (r, n, f, big, 100 * big // max(n, 1), other, v))
    print("\n%d files" % len(rows), file=sys.stderr)


if __name__ == "__main__":
    main()
