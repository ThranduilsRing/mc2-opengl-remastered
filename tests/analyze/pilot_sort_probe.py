#!/usr/bin/env python3
"""pilot_sort_probe - does the pilot-list order of ci_total_less move a stock pair of pilot names?

WHAT: reads every pilot FIT of the stock data (General/descIndex, Rank, Skills/Gunnery), looks the display name up in the legacy string catalog
(data/defs/text/en_us/mc2res_legacy.fit), and compares, for every pair of names, the sign of the old order (CompareStringA with LOCALE_USER_DEFAULT and NORM_IGNORECASE, the
function lstrcmpi is) with the sign of the new one (ASCII-folded lexicographic, then the raw bytes: mc2::str::ci_total_less). Pairs equal in rank and
gunnery are reported separately: only those are ordered by name (code/logisticspilotlistbox.cpp).
WHY: the pilot list order is visible and no gate sees it. The conversion replaces a locale word sort by a locale-free total order, so the stock names
have to be checked once for pairs that move.

  py -3 tests/analyze/pilot_sort_probe.py [--warriors DIR] [--strings FIT]

Windows only (ctypes: CompareStringA). Exit 0; the result is a report, not a gate.
"""
from __future__ import annotations

import argparse
import ctypes
import glob
import itertools
import os
import re
import sys

DEFAULT_WARRIORS = "A:/Games/mc2-srcdata/missions/warriors"
DEFAULT_STRINGS = "A:/Games/MC2 OpenGL Remastered/data/defs/text/en_us/mc2res_legacy.fit"
LOCALE_USER_DEFAULT = 0x0400
NORM_IGNORECASE = 1


def fold(b: bytes) -> bytes:
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)


def new_order(a: bytes, b: bytes) -> int:
    """Sign of ci_total_less: folded bytes, then raw bytes."""
    fa, fb = fold(a), fold(b)
    if fa != fb:
        return -1 if fa < fb else 1
    return 0 if a == b else (-1 if a < b else 1)


def old_order(a: bytes, b: bytes) -> int:
    r = ctypes.windll.kernel32.CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, len(a), b, len(b))
    return {1: -1, 2: 0, 3: 1}[r]


def load_strings(fit_path: str, ids):
    """id -> display text from the legacy string catalog (String blocks with `text = "..."` and `legacyId = N`)."""
    wanted = set(ids)
    out = {i: b"" for i in wanted}
    text = ""
    with open(fit_path, encoding="latin-1") as f:
        for ln in f:
            m = re.match(r'\s*text\s*=\s*"(.*)"\s*$', ln)
            if m:
                text = m[1].replace('\\"', '"')
                continue
            m = re.match(r"\s*legacyId\s*=\s*(\d+)", ln)
            if m and int(m[1]) in wanted:
                out[int(m[1])] = text.encode("latin-1")
    return out


def read_pilots(directory: str):
    pilots = []
    for path in sorted(glob.glob(os.path.join(directory, "*.fit"))):
        text = open(path, encoding="latin-1").read()
        desc = re.search(r"(?im)^\s*\w*\s*descIndex\s*=\s*(\d+)", text)
        rank = re.search(r"(?im)^\s*\w*\s*Rank\s*=\s*(\d+)", text)
        gun = re.search(r"(?im)^\s*\w*\s*Gunnery\s*=\s*(\d+)", text)
        if desc:
            pilots.append((os.path.basename(path), int(desc[1]), int(rank[1]) if rank else -1, int(gun[1]) if gun else -1))
    return pilots


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--warriors", default=DEFAULT_WARRIORS)
    ap.add_argument("--strings", default=DEFAULT_STRINGS)
    args = ap.parse_args()
    if sys.platform != "win32":
        print("pilot_sort_probe: Windows only")
        return 2
    pilots = read_pilots(args.warriors)
    names = load_strings(args.strings, {p[1] for p in pilots})
    rows = [(f, names[d], r, g) for f, d, r, g in pilots if names[d]]
    print(f"pilot_sort_probe: {len(pilots)} pilot files, {len(rows)} with a display name, {len({n for _, n, _, _ in rows})} distinct names")
    moved, tie_pairs, tie_moved = [], 0, []
    for (fa, a, ra, ga), (fb, b, rb, gb) in itertools.combinations(rows, 2):
        same_key = (ra, ga) == (rb, gb)
        tie_pairs += same_key
        if old_order(a, b) != new_order(a, b):
            moved.append((a, b, same_key))
            if same_key:
                tie_moved.append((a, b))
    print(f"all pairs: {len(moved)} of {len(rows) * (len(rows) - 1) // 2} change relative order")
    print(f"pairs equal in rank and gunnery (the ones the name decides): {tie_moved.__len__()} of {tie_pairs} change relative order")
    for a, b, same_key in moved[:40]:
        print(f"  {'TIE ' if same_key else '    '}{a.decode('latin-1')!r} vs {b.decode('latin-1')!r}: old {old_order(a, b):+d} new {new_order(a, b):+d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
