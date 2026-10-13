#!/usr/bin/env python3
"""estring_callsites - turn the C4996 warnings of a deprecation build into the committed EString call-site list.

WHAT: reads the log of a build in which the old EString API (conversion operator, Data, the comparison operators, Compare, Make{Upper,Lower}, the index
functions and Format) carries `[[deprecated("B1")]]`, de-duplicates the warnings (a header is compiled by many translation units) and writes
`.claude/cleanup/cxx23-step4-b1-callsites.txt`: one `file:line:kind` row per call site, with the count per file and per kind on top.
WHY: grep finds the files that spell `EString`; only the compiler finds every use of the API, including `std::string == const char*`-shaped comparisons and
files that never name the type (editor/BeautySidecarPreview.cpp). Each batch commit removes its rows and the count only goes down.
The deprecation itself is applied to mclib/estring.h by an uncommitted local edit and is NEVER committed.

  py -3 tests/analyze/estring_callsites.py BUILD.log [--out PATH] [--root A:/Games/mc2-main]
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()
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
