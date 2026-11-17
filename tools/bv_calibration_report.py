"""Offline MC2 BV calibration. Catalog and output must remain outside the repo.

Published canon loadouts differ from MC2 Prime/A/B/C variants. Compare against
the median published BV2 per chassis, explicitly a proxy, never an exact match.
The evaluator is built by tests/unit; all BV arithmetic remains in C++.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.mc2x_import.fst import fst_entries, decompress_entry


def number(value):
    try:
        result = float(value)
    except ValueError:
        return 0.0  # authored NA cells
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"invalid stock numeric value {value!r}")
    return result


def stock_tables(install):
    texts = {}
    for archive in sorted(install.glob("*.fst")):
        if archive.name.lower() != "misc.fst":
            continue
        for off, comp, real, name, lz, data in fst_entries(archive):
            key = name.lower().replace("\\", "/")
            if key.startswith("data/objects/") and key.endswith(".csv"):
                texts[Path(key).stem] = decompress_entry(off, comp, real, lz, data).decode("latin-1")
    for path in sorted((install / "data/objects").glob("*.csv")):
        texts[path.stem.lower()] = path.read_text(encoding="latin-1")
    return {name: list(csv.reader(io.StringIO(text))) for name, text in texts.items()}


def variants(tables):
    comps = {int(r[0]): r for r in tables["compbas"] if r and r[0].strip().isdigit()}
    for stem, rows in sorted(tables.items()):
        if not rows or rows[0][0].strip() != "BaseChassisInfo":
            continue
        fields = {r[0].strip(): r for r in rows if r}
        if "BaseInternalStructure" not in fields:
            continue
        structure = sum(number(x) for x in fields["BaseInternalStructure"][1:9])
        chassis = rows[2][1].strip()
        for start, row in enumerate(rows):
            if not row or row[0].strip() != "VariantName":
                continue
            items = []
            for item in rows[start + 1:]:
                if item and item[0].strip() == "VariantName":
                    break
                if item and item[0].startswith("Item") and "MasterId" in item[0]:
                    # Column 5 is the compbas ordinal, NOT the logistics load ID.
                    ordinal = int(item[4])
                    if ordinal != 255:
                        items.append(comps[ordinal])
            armor, heat = number(rows[5][4]), number(rows[3][4])
            speed, jump = number(rows[6][4]), 0
            weapons, defensive = [], 0
            ammo = {}
            for c in items:
                if c[1].lower() == "ammo":
                    ammo[int(c[0])] = ammo.get(int(c[0]), 0) + number(c[4])
            users = {}
            for c in items:
                if "weapon" in c[1].lower() and int(c[22]) != 0:
                    users[int(c[22])] = users.get(int(c[22]), 0) + 1
            for c in items:
                kind = c[1].lower()
                if kind == "bulk":
                    armor += number(c[7]) or 32
                elif kind == "heatsink":
                    heat += number(c[7])
                elif kind == "jumpjet":
                    jump = 1
                elif kind == "engine" and int(c[0]) != 1:
                    ladder = [5, 10, 15, 19, 23, 27, 31, 35]
                    speed = next((b for a, b in zip(ladder, ladder[1:]) if speed <= a), speed)
                elif "weapon" in kind:
                    ammo_id = int(c[22])
                    shots = ammo.get(ammo_id, 0) / users[ammo_id] if ammo_id else None
                    weapons.append((number(c[8]), number(c[5]), shots))
                elif kind in {"ecm", "ams"}:
                    defensive += number(c[8])
            heat = max(0, heat - sum(number(c[5]) for c in items if "weapon" not in c[1].lower()))
            legacy = int(number(rows[9][4]) + sum(number(c[8]) for c in items))
            price = number(rows[10][4]) + sum(number(c[9]) for c in items)
            yield {"chassis": chassis, "stem": stem, "variant": row[1].strip(),
                   "armor": armor, "structure": structure, "speed": speed,
                   "jump": jump, "heat": heat, "defensive": defensive,
                   "weapons": weapons, "legacy_cv": legacy, "cbills": price}


def wire(v):
    values = [v[k] for k in ("armor", "structure", "speed", "jump", "heat", "defensive")]
    values.append(len(v["weapons"]))
    for br, heat, shots in v["weapons"]:
        values.extend([br, heat, "energy" if shots is None else "ammo"])
        if shots is not None:
            values.append(shots)
    return " ".join(map(str, values))


def ranks(values):
    ordered = sorted(values)
    return [(ordered.index(v) + len(ordered) - ordered[::-1].index(v) + 1) / 2 for v in values]


def spearman(pairs):
    if len(pairs) < 2:
        return None
    a, b = (ranks(list(x)) for x in zip(*pairs))
    ma, mb = statistics.mean(a), statistics.mean(b)
    denominator = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / denominator if denominator else None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--install", required=True, type=Path)
    p.add_argument("--catalog", required=True, type=Path)
    p.add_argument("--evaluator", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    for path in (args.catalog, args.out):
        if path.resolve().is_relative_to(REPO):
            p.error("catalog and reports must remain outside the repository")
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))["chassis"]
    reference = {}
    for name, entry in catalog.items():
        values = [v["bv2"]["value"] for v in entry["variants"] if (v.get("bv2") or {}).get("value")]
        if values:
            reference[name.casefold().replace(" ", "")] = statistics.median(values)
    data = list(variants(stock_tables(args.install)))
    if not data:
        raise ValueError("no stock variants found")
    result = subprocess.run([str(args.evaluator.resolve()), str(REPO / "data/defs/rules/battle_value.fit")],
                            input="\n".join(map(wire, data)) + "\n", text=True, capture_output=True, check=True)
    bv = list(map(int, result.stdout.splitlines()))
    if len(bv) != len(data):
        raise ValueError("evaluator row count mismatch")
    aliases = {"madcat": "timberwolf", "ryoken": "stormcrow", "thor": "summoner",
               "vulture": "maddog", "uller": "kitfox"}
    rows = []
    for v, value in zip(data, bv):
        key = v["chassis"].casefold().replace(" ", "")
        proxy = reference.get(aliases.get(key, key))
        rows.append({k: v[k] for k in ("chassis", "stem", "variant", "legacy_cv", "cbills")})
        rows[-1].update(our_bv=value, published_bv2_chassis_median=proxy)
    correlations = {}
    for key in ("published_bv2_chassis_median", "legacy_cv", "cbills"):
        pairs = [(r["our_bv"], r[key]) for r in rows if r[key] is not None]
        correlations[key] = {"n": len(pairs), "spearman": spearman(pairs)}
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "correlation.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summary = {"profile": 1, "variants": len(rows), "chassis": len({r['stem'] for r in rows}),
               "published_comparison": "chassis median proxy; MC2 loadouts are not canon variants",
               "catalog_license": "CC BY-NC-SA 4.0; external calibration input only",
               "correlations": correlations}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
