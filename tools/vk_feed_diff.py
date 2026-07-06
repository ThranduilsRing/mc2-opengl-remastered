#!/usr/bin/env python3
"""vk_feed_diff.py — VK-OFFLINE-PROOFS-1 golden-vs-current feed diff.

Compares two MC2_VK_FEED_DUMP outputs (newline-delimited JSON, one object per
island: terrain/prop/mech — see GameOS/gameos/vk_feed_dump.h). Reports:
  - which islands are present in each file
  - per-island scalar field diffs
  - per-island list diffs (patches/instances), keyed by a stable identity so
    reordering doesn't cause false positives

Usage:
    py -3 tools/vk_feed_diff.py golden.jsonl current.jsonl
    py -3 tools/vk_feed_diff.py golden.jsonl current.jsonl --island terrain

Exit code 0 = no differences (or only expected ones with --allow-missing),
1 = differences found, 2 = usage/parse error.
"""
import argparse
import json
import sys

# Identity key per island's list field: (list_field_name, key_fields)
ISLAND_LIST_KEY = {
    "terrain": ("patches", ("blockOriginX", "blockOriginY", "lodStep")),
    "prop":    ("instances", ("typeID", "posX", "posY", "posZ")),
    "mech":    ("instances", ("typeLodRecordIndex", "baseBoneOffset")),
}

SCALAR_SKIP = {"patches", "instances"}


def load(path):
    """Returns {island_name: record} — last record per island wins (matches the
    once-per-process latch semantics, but tolerates a re-run appending twice)."""
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"{path}:{lineno}: JSON parse error: {e}", file=sys.stderr)
                sys.exit(2)
            island = rec.get("island")
            if not island:
                print(f"{path}:{lineno}: record missing 'island' key", file=sys.stderr)
                sys.exit(2)
            out[island] = rec
    return out


def diff_scalars(island, a, b):
    diffs = []
    keys = (set(a.keys()) | set(b.keys())) - SCALAR_SKIP - {"island"}
    for k in sorted(keys):
        va, vb = a.get(k, "<missing>"), b.get(k, "<missing>")
        if va != vb:
            diffs.append(f"  [{island}] {k}: golden={va!r} current={vb!r}")
    return diffs


def diff_lists(island, a, b):
    list_field, key_fields = ISLAND_LIST_KEY.get(island, (None, None))
    if list_field is None:
        return []
    la, lb = a.get(list_field, []), b.get(list_field, [])

    def keyfn(item):
        return tuple(item.get(k) for k in key_fields)

    ma = {keyfn(x): x for x in la}
    mb = {keyfn(x): x for x in lb}
    diffs = []
    only_golden = set(ma) - set(mb)
    only_current = set(mb) - set(ma)
    if only_golden:
        diffs.append(f"  [{island}] {list_field}: {len(only_golden)} record(s) only in golden "
                      f"(missing from current) e.g. {list(only_golden)[:5]}")
    if only_current:
        diffs.append(f"  [{island}] {list_field}: {len(only_current)} record(s) only in current "
                      f"(new/unexpected) e.g. {list(only_current)[:5]}")
    common = set(ma) & set(mb)
    field_diff_count = 0
    for k in sorted(common):
        ia, ib = ma[k], mb[k]
        for f in (set(ia.keys()) | set(ib.keys())):
            if ia.get(f) != ib.get(f):
                field_diff_count += 1
                if field_diff_count <= 10:
                    diffs.append(f"  [{island}] {list_field} key={k} field={f}: "
                                  f"golden={ia.get(f)!r} current={ib.get(f)!r}")
    if field_diff_count > 10:
        diffs.append(f"  [{island}] ... {field_diff_count - 10} more field diffs suppressed")
    diffs.append(f"  [{island}] {list_field}: golden_count={len(la)} current_count={len(lb)} "
                  f"common={len(common)}")
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("current")
    ap.add_argument("--island", action="append", help="restrict to these island(s); default all")
    ap.add_argument("--allow-missing", action="store_true",
                     help="don't fail if an island is present in one file but not the other")
    args = ap.parse_args()

    golden = load(args.golden)
    current = load(args.current)

    islands = args.island or sorted(set(golden.keys()) | set(current.keys()))
    any_diff = False

    for island in islands:
        a = golden.get(island)
        b = current.get(island)
        if a is None or b is None:
            msg = f"[{island}] present in golden={a is not None} current={b is not None}"
            print(msg)
            if not args.allow_missing:
                any_diff = True
            continue
        d = diff_scalars(island, a, b) + diff_lists(island, a, b)
        if d:
            print(f"--- {island} ---")
            for line in d:
                print(line)
            # A pure count-summary line (no golden/current mismatch) doesn't count as a diff.
            real_diffs = [l for l in d if "golden_count=" not in l]
            if real_diffs:
                any_diff = True
        else:
            print(f"[{island}] identical")

    sys.exit(1 if any_diff else 0)


if __name__ == "__main__":
    main()
