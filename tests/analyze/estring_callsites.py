#!/usr/bin/env python3
"""estring_callsites - turn the C4996 warnings of a deprecation build into the committed EString call-site list.

WHAT: reads the log of a build in which the old EString API (conversion operator, Data, the comparison operators, Compare, Make{Upper,Lower}, the index
functions and Format) carries `[[deprecated("B1")]]`, de-duplicates the warnings (a header is compiled by many translation units) and writes
`.claude/cleanup/cxx23-step4-b1-callsites.txt`: one `file:line:kind` row per call site, with the count per file and per kind on top.
WHY: grep finds the files that spell `EString`; only the compiler finds every use of the API, including `std::string == const char*`-shaped comparisons and
files that never name the type (editor/BeautySidecarPreview.cpp). Each batch commit removes its rows and the count only goes down.
The deprecation itself is applied to mclib/estring.h by an uncommitted local edit and is NEVER committed.

  py -3 tests/analyze/estring_callsites.py BUILD.log [--out PATH] [--root A:/Games/mc2-main]
  py -3 tests/analyze/estring_callsites.py --drop code/objective.cpp code/x.cpp:42 ...   # remove converted rows from the committed list
  py -3 tests/analyze/estring_callsites.py --check                                       # every remaining row still names its kind on its line
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_OUT = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b1-callsites.txt")
_DIAG = re.compile(r"^(?P<file>[A-Za-z]:[^()]*?)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*(?:warning|error)\s+C4996:\s*'(?P<kind>[^']*)'")
#: Files whose own use of the API is the implementation, not a call site.
SELF = {"mclib/estring.h", "mclib/estring.cpp"}
FILES_JSON = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b1-files.json")


def parse(log_path: str, root: str):
    root = root.replace("\\", "/").rstrip("/") + "/"
    seen = set()
    rows = []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = _DIAG.match(ln.strip())
            if not m or "EString" not in m["kind"]:
                continue
            path = m["file"].replace("\\", "/")
            if path.lower().startswith(root.lower()):
                path = path[len(root):]
            if path.lower() in SELF:
                continue
            key = (path, int(m["line"]), m["col"] or "0", m["kind"])
            if key not in seen:
                seen.add(key)
                rows.append(key)
    rows.sort()
    return rows


def batch_of() -> dict:
    """file -> batch from cxx23-step4-b1-files.json (the files that name the type); other files are includers or gui."""
    try:
        with open(FILES_JSON, encoding="utf-8") as f:
            return json.load(f)["files"]
    except OSError:
        return {}


def render(rows) -> str:
    per_file = collections.Counter(r[0] for r in rows)
    batches = batch_of()
    per_batch = collections.Counter()
    unlisted = collections.Counter()
    for path, n in per_file.items():
        if path in batches:
            per_batch[batches[path]] += n
        else:
            unlisted[path] = n
    per_kind = collections.Counter(r[3] for r in rows)
    out = ["# EString call sites from a [[deprecated]] build of the old API (tests/analyze/estring_callsites.py); a batch commit removes its rows.",
           f"# total {len(rows)} sites in {len(per_file)} files",
           "# per kind:"]
    out += [f"#   {n:5d}  {k}" for k, n in per_kind.most_common()]
    out += ["# per batch (files that name the type, cxx23-step4-b1-files.json):"]
    out += [f"#   {n:5d}  {b}" for b, n in sorted(per_batch.items())]
    out += [f"# sites in files that do NOT name the type or are gui widgets (includers the compiler forces, or B6): {sum(unlisted.values())} in {len(unlisted)} files"]
    out += [f"#   {n:5d}  {p}" for p, n in sorted(unlisted.items(), key=lambda kv: (-kv[1], kv[0]))]
    out += ["# per file:"]
    out += [f"#   {n:5d}  {p}" for p, n in sorted(per_file.items(), key=lambda kv: (-kv[1], kv[0]))]
    out += ["# file:line:kind"]
    out += [f"{p}:{ln}:{k.replace('EString::', '')}" for p, ln, _col, k in rows]
    return "\n".join(out) + "\n"


def drop(path: str, targets: list[str]) -> int:
    """Rewrite the committed list without the rows of converted files (or file:line rows)."""
    whole = {t for t in targets if ":" not in t}
    lines = {tuple(t.rsplit(":", 1)) for t in targets if ":" in t}
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            file, line, kind = ln.rstrip().split(":", 2)
            if file in whole or (file, line) in lines:
                continue
            rows.append((file, int(line), "0", "EString::" + kind))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(rows))
    print(f"estring_callsites: {len(rows)} sites in {len({r[0] for r in rows})} files remain -> {path}")
    return 0


def kind_token(kind: str) -> str | None:
    """The text a row's kind leaves on its source line; None for the implicit conversion, which leaves none."""
    if kind == "operator ECHARPTR":
        return None
    if kind.startswith("operator "):
        return kind[len("operator "):]
    return "." + kind + "(" if kind == "Data" else kind + "("


def check(path: str) -> int:
    """Fail when the list is out of step with the tree: a row whose file is gone, whose line no longer holds its kind, or a header total that
    does not match the rows. A converted file must leave the list through --drop; a file edited without that shows up here."""
    total = files = None
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            m = re.match(r"# total (\d+) sites in (\d+) files", ln)
            if m:
                total, files = int(m[1]), int(m[2])
            if ln.startswith("#") or not ln.strip():
                continue
            file, line, kind = ln.rstrip().split(":", 2)
            rows.append((file, int(line), kind))
    problems = []
    if (total, files) != (len(rows), len({r[0] for r in rows})):
        problems.append(f"header says {total} sites in {files} files, the rows are {len(rows)} in {len({r[0] for r in rows})}")
    cache: dict[str, list[str] | None] = {}
    for file, line, kind in rows:
        if file not in cache:
            try:
                with open(os.path.join(ROOT, file), encoding="utf-8", errors="replace") as src:
                    cache[file] = src.read().splitlines()
            except OSError:
                cache[file] = None
        text = cache[file]
        if text is None:
            problems.append(f"{file}:{line}:{kind}: the file is gone")
        elif not 1 <= line <= len(text):
            problems.append(f"{file}:{line}:{kind}: the file has {len(text)} lines")
        else:
            token = kind_token(kind)
            if token is not None and token not in text[line - 1]:
                problems.append(f"{file}:{line}:{kind}: the line no longer contains '{token}'")
    for p in problems:
        print(f"estring_callsites: {p}")
    print(f"estring_callsites: {'FAIL' if problems else 'OK'} - {len(rows)} sites, {len(problems)} problem(s)")
    return 1 if problems else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="the deprecation build's log (omit with --drop)")
    ap.add_argument("--drop", nargs="+", metavar="FILE[:LINE]", help="remove these rows from the committed list: a batch that converted them, proven by the build")
    ap.add_argument("--check", action="store_true", help="verify the committed list against the tree (exit 1 on a stale or converted row)")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()
    if args.check:
        return check(args.out)
    if args.drop:
        return drop(args.out, args.drop)
    if not args.log:
        ap.error("a build log is required unless --drop is given")
    rows = parse(args.log, args.root)
    if not rows:
        print("estring_callsites: no EString C4996 rows in the log (was the deprecation applied?)", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(rows))
    print(f"estring_callsites: {len(rows)} sites in {len({r[0] for r in rows})} files -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
