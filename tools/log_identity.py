#!/usr/bin/env python3
"""log_identity - compare the game logs of two smoke runs by line shape.

WHAT: reads the *.log files of two smoke artifact directories, replaces every number and address with a placeholder and reports the line shapes
that exist in only one of them (and, for the structured `[TAG vN]` lines, the set of tags).
WHY: the printf -> std::format conversions (C++23 step 3) must not change what the engine prints. Frame counts and timings differ from run to run,
so a byte diff is useless, but a changed spelling, key order, padding of a string or a lost line shows up as a shape that appears on one side only.

  py -3 tools/log_identity.py tests/smoke/artifacts/<before> tests/smoke/artifacts/<after> [--mission mc2_03]

WHAT IT PROVES, AND WHAT IT DOES NOT: line spellings, key order, the set of lines and the set of tags of the lines that the two runs print. It masks every
number, so a changed sign, width, precision or hex case is invisible here; the format corpus (tests/unit/printf_corpus*.inc) proves the numbers. It compares
sets, not counts, ignores a log file present only after, and most tags never fire on mc2_03/mc2_24. Both proofs are needed.
"""
from __future__ import annotations

import argparse
import collections
import glob
import os
import re
import sys

_NUM = re.compile(r"(?<![A-Za-z_\d])(?:0x[0-9a-fA-F]+|[0-9A-F]{16}\b|-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)", re.I)
_TAG = re.compile(r"^\s*\[([A-Z][A-Z0-9_]*)(?: v\d+)?\]")
#: Lines that differ between two runs of the SAME executable, so they say nothing about a conversion: the build stamp and the per-frame block-set
#: trace. (The frame-2 [TerrainLOD v1] DIAG plane dump used to be here; it printed uninitialised camera matrices and was repaired in 47a9623ed.)
#: The git sha of the exe that wrote the log (the [INSTR v1] enabled: line): it differs between any two builds and says nothing about the log's shape.
_BUILD = re.compile(r"(?<![A-Za-z_])build=[0-9a-f]+")
NOISE = re.compile(r"\[BUILD_FINGERPRINT v1\]|^\s*\[BlockSet\]")


def shape(line: str) -> str:
    """The line with numbers and addresses masked. Blanks touching a number are collapsed: `%3d` pads a 5 and a 12 differently."""
    s = _NUM.sub("#", _BUILD.sub("build=", line.rstrip("\r\n")))
    return re.sub(r" +#", " #", re.sub(r"# +", "# ", s))


def shapes_of(path: str):
    out = collections.Counter()
    tags = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if NOISE.search(ln):
                continue
            s = shape(ln)
            if s.strip():
                out[s] += 1
            m = _TAG.match(ln)
            if m and (not tags or tags[-1] != m.group(1)):
                tags.append(m.group(1))
    return out, tags


def compare(before: str, after: str, mission: str | None = None):
    """-> (only_before, only_after, tag_order_equal) over the .log files present in both directories."""
    only_b, only_a, order_ok = [], [], True
    for b in sorted(glob.glob(os.path.join(before, "*.log"))):
        name = os.path.basename(b)
        if "ring_trace" in name or (mission and not name.startswith(mission)):
            continue
        a = os.path.join(after, name)
        if not os.path.exists(a):
            only_b.append(f"{name}: missing after")
            continue
        sb, tb = shapes_of(b)
        sa, ta = shapes_of(a)
        only_b += [f"{name}: {s}" for s in sb if s not in sa]
        only_a += [f"{name}: {s}" for s in sa if s not in sb]
        order_ok = order_ok and (set(tb) == set(ta))        # worker threads interleave HEARTBEAT/TEXMGR lines differently per run: compare the tag SET
    return only_b, only_a, order_ok


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--mission")
    args = ap.parse_args(argv)
    only_b, only_a, order_ok = compare(args.before, args.after, args.mission)
    for s in only_b[:40]:
        print("only before: " + s[:200])
    for s in only_a[:40]:
        print("only after:  " + s[:200])
    print(f"log_identity: {len(only_b)} shape(s) only before, {len(only_a)} only after, tag set {'equal' if order_ok else 'DIFFERENT'}")
    return 0 if not only_b and not only_a and order_ok else 1


if __name__ == "__main__":
    sys.exit(main())
