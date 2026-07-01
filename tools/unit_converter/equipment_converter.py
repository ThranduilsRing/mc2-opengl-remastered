#!/usr/bin/env python3
# tools/unit_converter/equipment_converter.py
"""LEGACY-COMPBAS-CONVERTER-1 (equipment arc, slice 4) CLI — offline stock
compbas.csv -> EquipmentDefinition sidecar converter.

Reads read-only stock compbas.csv; writes only under --out. Mirrors the
unit_converter CLI. STOCK-ONLY: base mc2srcdata/objects/compbas.csv, never
MCO/MC2X/BT2018/mods.

Usage:
  py -3 tools/unit_converter/equipment_converter.py \
      --srcdata A:/Games/mc2-opengl-src/mc2srcdata \
      --out generated/equipment [--json]

If --srcdata is omitted, falls back to the bundled test fixtures (byte-identical
to stock compbas.csv) so the tool runs self-contained in a worktree.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from unit_converter import equipment_emit as emit          # noqa: E402
from unit_converter import equipment_extract as extract     # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE_SRCDATA = os.path.join(_HERE, "tests", "fixtures", "stock")
COMPBAS_REL = os.path.join("objects", "compbas.csv")


def convert(srcdata: str):
    rows = extract.load_compbas_rows(os.path.join(srcdata, COMPBAS_REL))
    return extract.extract_all(rows)


def build_report(defs, waivers) -> dict:
    per_kind = Counter(d.kind for d in defs)
    # legacyMasterId -> id alias map (sorted by ordinal for determinism).
    alias_map = {str(d.legacy_master_id): d.equip_id
                 for d in sorted(defs, key=lambda d: d.legacy_master_id)}
    fire_execs = Counter(d.weapon.fire_executor for d in defs if d.weapon is not None)
    return {
        "tool": "LEGACY-COMPBAS-CONVERTER-1",
        "stock_source": "mc2srcdata/objects/compbas.csv (STOCK-ONLY)",
        "components_converted": len(defs),
        "per_kind_counts": dict(sorted(per_kind.items())),
        "fire_executor_counts": dict(sorted(fire_execs.items())),
        "waivers": list(waivers),
        "num_waivers": len(waivers),
        "legacy_master_id_to_id": alias_map,
        "components": [d.to_report_dict() for d in defs],
        "deferred": (
            "legacy-vs-generated facet diff is WEAPON-SHADOW-COMPARE-1 (next "
            "slice), not performed here. It will diff per WeaponFacet: category, "
            "combat (damage/heat/recycle/range_bucket/combat_value), ammo "
            "(ammo_category vs stock ammoMasterId+WEAPON_AMMO_OFFSET), fit "
            "(crit_slots/tonnage/allowed_locations), plus kind and "
            "legacyMasterId->id alias stability."
        ),
    }


def render_report_text(report: dict) -> str:
    out = []
    out.append(f"=== {report['tool']} conversion report ===")
    out.append(f"stock source: {report['stock_source']}")
    out.append(f"components converted: {report['components_converted']}")
    out.append("per-kind:")
    for k, n in report["per_kind_counts"].items():
        out.append(f"    {k:12} {n}")
    out.append("fireExecutor (weapons):")
    for k, n in report["fire_executor_counts"].items():
        out.append(f"    {k:24} {n}")
    out.append(f"waivers: {report['num_waivers']}")
    for w in report["waivers"][:12]:
        out.append(f"    ! {w}")
    if report["num_waivers"] > 12:
        out.append(f"    ... (+{report['num_waivers'] - 12} more)")
    out.append(f"deferred: {report['deferred']}")
    return "\n".join(out)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="MC2 legacy compbas -> EquipmentDefinition converter (offline)")
    p.add_argument("--srcdata", default=None,
                   help="stock mc2srcdata root (default: bundled test fixtures)")
    p.add_argument("--out", default=None,
                   help="output dir; if omitted, no files written")
    p.add_argument("--json", action="store_true", help="emit JSON report")
    args = p.parse_args(argv)

    srcdata = args.srcdata or FIXTURE_SRCDATA
    compbas = os.path.join(srcdata, COMPBAS_REL)
    if not os.path.isfile(compbas):
        print(f"error: compbas.csv not found: {compbas}", file=sys.stderr)
        return 2

    defs, waivers = convert(srcdata)

    if args.out:
        for d in defs:
            emit.write_equipment(d, args.out)

    report = build_report(defs, waivers)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(render_report_text(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
