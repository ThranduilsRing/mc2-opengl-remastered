#!/usr/bin/env python3
"""pick_batch - choose the next N translation units for tools/tidy/run_tidy_safe.py.

WHAT: tracked .cpp files under the given directories that (a) are in compile_commands.json, (b) are not excluded or dirty, and (c)
still contain a site the safe checks rewrite (a `NULL` token or a line-leading `typedef`), smallest file first.
WHY: handing run_tidy_safe a list with already-clean files wastes the batch (they tidy to no change), so the batch is picked from files
that have work left. Prints the repo-relative paths, one per line, ready for `xargs`.

  py -3 tools/tidy/pick_batch.py --dir mclib --count 50
  py -3 tools/tidy/pick_batch.py --dir code --dir GameOS/gameos --count 50
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_safe as rts  # noqa: E402

SITE = re.compile(r"\bNULL\b|^\s*typedef\b", re.M)


def tracked_cpp(dirs):
    out = subprocess.run(["git", "-C", rts.ROOT, "ls-files"] + [d.rstrip("/") + "/**/*.cpp" for d in dirs] +
                         [d.rstrip("/") + "/*.cpp" for d in dirs], capture_output=True, text=True).stdout
    return sorted(set(out.split()))


def pick(dirs, count, tree=rts.DEFAULT_TREE):
    in_db = rts.load_compile_db(os.path.join(rts.ROOT, tree))
    dirty = rts.dirty_files()
    cands = []
    for rel in tracked_cpp(dirs):
        kept, _ = rts.select([os.path.join(rts.ROOT, rel)], in_db, dirty)
        if not kept:
            continue
        path = os.path.join(rts.ROOT, rel)
        with open(path, encoding="utf-8", errors="replace") as fh:
            if SITE.search(fh.read()):
                cands.append((os.path.getsize(path), rel))
    cands.sort()
    return [rel for _sz, rel in cands[:count]]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", action="append", required=True)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--tree", default=rts.DEFAULT_TREE)
    args = ap.parse_args(argv)
    for rel in pick(args.dir, args.count, args.tree):
        print(rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
