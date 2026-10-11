#!/usr/bin/env python3
"""printf_sites - convert sprintf calls whose destination is not a visible char array, one site at a time, and let the compiler judge.

WHAT: for each listed file, tries every `sprintf(dest, ...)` the batch converter left alone ("destination is not a visible char array") as
`mc2::fmt::to_buf(dest, ...)` -- bottom to top so line numbers stay valid -- builds the file's object after each one, keeps the conversion when
it compiles (dest really is a char array: `animName[i]`, `obj.name`, a member) and puts the original text back when it does not (dest is a pointer).
Prints the kept and the refused sites; the refused ones are the B3b hand list (bound from the allocation, or a ledger entry with the reason).
WHY: the textual array detection only sees `char name[N]` declarations; the compiler knows the real type of every destination expression.

  py -3 tools/tidy/printf_sites.py --tree build-alltargets FILE...
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import printf_buffers as pb  # noqa: E402
import run_tidy_safe as rts  # noqa: E402
import tidy_lock  # noqa: E402

ROOT = rts.ROOT


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="surrogateescape", newline="") as fh:
        return fh.read()


def write(path, text):
    with open(os.path.join(ROOT, path), "w", encoding="utf-8", errors="surrogateescape", newline="") as fh:
        fh.write(text)


def sites_of(text):
    """Original line numbers of the sprintf calls the batch converter refused for want of a visible array."""
    return sorted({line for line, why in pb.convert(text)[2] if "visible char array" in why}, reverse=True)


def try_file(rel: str, tree: str, jobs: int, builder=None):
    """-> (kept lines, refused lines). The file is left with the kept conversions applied."""
    builder = builder or rts.ninja_objects
    kept, refused = [], []
    for line in sites_of(read(rel)):
        before = read(rel)
        after = pb.convert(before, assume_array=True, only_lines={line})[0]
        if after == before:
            refused.append(line)
            continue
        write(rel, after)
        rc, _failed = builder(tree, [rel], jobs)
        if rc:
            write(rel, before)
            refused.append(line)
        else:
            kept.append(line)
    return sorted(kept), sorted(refused)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--tree", default="build-alltargets")
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args(argv)
    tree = os.path.join(ROOT, args.tree)
    out = {}
    with tidy_lock.TidyLock(tree):
        for rel in args.files:
            kept, refused = try_file(rel, tree, args.jobs)
            out[rel] = {"kept": kept, "refused": refused}
            print(f"{rel}: kept {len(kept)}, refused {len(refused)} {refused}", flush=True)
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
