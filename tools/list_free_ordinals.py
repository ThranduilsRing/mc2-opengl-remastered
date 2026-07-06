#!/usr/bin/env python3
"""list_free_ordinals.py — print free (undefined) compbas component ordinals.

compbas.csv is the master equipment/component table; the engine addresses
rows by an 8-bit masterID ordinal (0..255), so mods must pick UNUSED ordinals.
This tool prints the free ones.

    py -3 tools/list_free_ordinals.py --compbas <csv-path | install-dir>

Accepts either a loose compbas.csv path, or an install dir — in that case
data/objects/compbas.csv is auto-extracted from <install>/misc.fst via
tools/mc2x_import/fst.py (asm-faithful LZW decoder).

Output: one line "free ordinals (N): a, b, c-d, ..." (ranges collapsed) plus
"used: M" summary. Exit 0 on success, 2 on setup/parse error.

Home note: this lives as a standalone Python tool (not inside mc2_modlint)
because FST/LZW decode on the C++ side would be disproportionate for a lint
helper; mc2_modlint (unit-profile-seam branch) should reference this tool in
its --help when it lands on this branch.
"""
import argparse
import csv
import io
import os
import sys
import tempfile

MAX_ORDINAL = 255


def load_csv_bytes(compbas_arg):
    if os.path.isdir(compbas_arg):
        fst_path = os.path.join(compbas_arg, "misc.fst")
        if not os.path.isfile(fst_path):
            raise FileNotFoundError(f"no misc.fst in install dir: {compbas_arg}")
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "mc2x_import"))
        import fst
        for off, comp, real, name, is_lz, data in fst.fst_entries(fst_path):
            if name.lower().replace("\\", "/") == "data/objects/compbas.csv":
                return fst.decompress_entry(off, comp, real, is_lz, data)
        raise FileNotFoundError("data/objects/compbas.csv not found in misc.fst")
    with open(compbas_arg, "rb") as f:
        return f.read()


def collapse(nums):
    out, i = [], 0
    nums = sorted(nums)
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else f"{nums[i]}-{nums[j]}")
        i = j + 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--compbas", required=True,
                    help="compbas.csv path OR install dir containing misc.fst")
    args = ap.parse_args()

    try:
        raw = load_csv_bytes(args.compbas)
    except Exception as e:
        print(f"error: {e}")
        return 2

    # An ordinal is FREE if it has no row at all, or its row's Type column
    # (col 1) is the "undefined" placeholder the stock table uses for empty
    # slots. Anything else (including "Removed") counts as taken.
    used = set()
    rows = 0
    for row in csv.reader(io.StringIO(raw.decode("latin-1"))):
        if len(row) < 2:
            continue
        try:
            ordinal = int(row[0].strip())
        except ValueError:
            continue  # header / comment row
        rows += 1
        if 0 <= ordinal <= MAX_ORDINAL and row[1].strip().lower() != "undefined":
            used.add(ordinal)

    if rows == 0:
        print("error: no numeric ordinals parsed — wrong file?")
        return 2

    free = [n for n in range(MAX_ORDINAL + 1) if n not in used]
    print(f"used: {len(used)} (rows parsed: {rows})")
    print(f"free ordinals ({len(free)}): {', '.join(collapse(free))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
