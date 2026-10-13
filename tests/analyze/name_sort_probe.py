#!/usr/bin/env python3
"""name_sort_probe - do the Mechlopedia and mech-list sorts move a stock entry when the old collation is replaced by the ASCII-folded order?

WHAT: for the two list orders that call EString::Compare, takes every stock entry the screen sorts and compares the old order (CompareStringA with
LOCALE_USER_DEFAULT and NORM_IGNORECASE, which is what lstrcmpi, EString::Compare's collation, is) with the new one (ASCII-folded lexicographic: mc2::str::icompare):
  personality  Mechlopedia::PersonalityScreen::begin (code/mechlopedia.cpp): the strings IDS_PERSONALITY_0.. (45300.., PERSONALITY_COUNT of them) upper-cased, each
               inserted before the first list item that is greater. Reports the final list for both comparators and every pair whose relative order differs.
  chassis      MechListBox::AddItem (code/mechlistbox.cpp): among mechs of one weight the chassis display names (DescIndex of every data/objects/*.csv) order the
               list. Reports every pair of names whose relative order differs.
WHY: the order is visible and no gate sees it. Replacing a locale word sort (which ignores '-' and ''' in a word) by a locale-free byte order can move an entry.

  py -3 tests/analyze/name_sort_probe.py [--strings FIT] [--objects DIR]

Windows only (CompareStringA). Exit 0; the result is a report, not a gate.
"""
from __future__ import annotations

import argparse
import csv
import glob
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pilot_sort_probe as base  # noqa: E402

DEFAULT_OBJECTS = "A:/Games/MC2 OpenGL Remastered/data/objects"
PERSONALITY_FIRST, PERSONALITY_COUNT = 45300, 11


def old_cmp(a: bytes, b: bytes) -> int:
    return base.old_order(a, b)


def new_cmp(a: bytes, b: bytes) -> int:
    """Sign of mc2::str::icompare: the folded bytes, then the length (the raw-byte tiebreak of ci_total_less is not part of icompare)."""
    fa, fb = base.fold(a), base.fold(b)
    return 0 if fa == fb else (-1 if fa < fb else 1)


def insertion_order(names, cmp):
    """The list Mechlopedia::PersonalityScreen::begin builds: each name goes before the first item that is greater than it (cmp < 0), else at the end."""
    out = []
    for n in names:
        for i, existing in enumerate(out):
            if cmp(n, existing) < 0:
                out.insert(i, n)
                break
        else:
            out.append(n)
    return out


def pair_report(label, names):
    names = sorted(set(names))
    moved = [(a, b) for a, b in itertools.combinations(names, 2) if old_cmp(a, b) != new_cmp(a, b)]
    print(f"{label}: {len(names)} distinct names, {len(moved)} of {len(names) * (len(names) - 1) // 2} pairs change relative order")
    for a, b in moved[:40]:
        print(f"  {a.decode('latin-1')!r} vs {b.decode('latin-1')!r}: old {old_cmp(a, b):+d} new {new_cmp(a, b):+d}")
    return len(moved)


def chassis_ids(objects: str):
    ids = []
    for path in sorted(glob.glob(os.path.join(objects, "*.csv"))):
        with open(path, newline="", encoding="latin-1") as f:
            rows = list(csv.reader(f))
        # CSVFile::readLong(5, 2) is 1-based: row 5, column 2 is the DescIndex cell.
        if len(rows) >= 5 and len(rows[4]) >= 2 and rows[4][0].strip().lower() == "descindex":
            try:
                ids.append(int(rows[4][1]))
            except ValueError:
                pass
    return ids


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--strings", default=base.DEFAULT_STRINGS)
    ap.add_argument("--objects", default=DEFAULT_OBJECTS)
    args = ap.parse_args()
    if sys.platform != "win32":
        print("name_sort_probe: Windows only")
        return 2
    ids = list(range(PERSONALITY_FIRST, PERSONALITY_FIRST + PERSONALITY_COUNT)) + chassis_ids(args.objects)
    text = base.load_strings(args.strings, set(ids))
    personality = [text[i].upper() for i in range(PERSONALITY_FIRST, PERSONALITY_FIRST + PERSONALITY_COUNT) if text[i]]
    old_list, new_list = insertion_order(personality, old_cmp), insertion_order(personality, new_cmp)
    print(f"personality list, old: {[n.decode('latin-1') for n in old_list]}")
    print(f"personality list, new: {[n.decode('latin-1') for n in new_list]}")
    print("personality list: " + ("IDENTICAL" if old_list == new_list else "DIFFERENT"))
    pair_report("personality", personality)
    chassis = [text[i] for i in chassis_ids(args.objects) if text.get(i)]
    pair_report("chassis", chassis)
    return 0


if __name__ == "__main__":
    sys.exit(main())
