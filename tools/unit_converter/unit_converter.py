#!/usr/bin/env python3
# tools/unit_converter/unit_converter.py
"""LEGACY-UNIT-CONVERTER-1 CLI — offline stock-unit -> UnitDefinition converter.

Bounded first scope: convert ONE mech + ONE vehicle end-to-end, emit sidecar
.fit files, and produce a conversion report. Reads read-only stock source;
writes only under --out.

Usage:
  py -3 tools/unit_converter/unit_converter.py \
      --srcdata A:/Games/mc2-opengl-src/mc2srcdata \
      --mech atlas --vehicle armoredcar \
      --out generated/units [--json]

If --srcdata is omitted, falls back to the bundled test fixtures so the tool
runs self-contained in a worktree without the mc2srcdata sibling checkout.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

# allow both `python -m tools.unit_converter.unit_converter` and direct run
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from unit_converter import emit  # noqa: E402
from unit_converter import roster as _roster  # noqa: E402
from unit_converter.extract import extract_mech, extract_vehicle  # noqa: E402
from unit_converter.fitparse import CompBas  # noqa: E402
from unit_converter.model import UnitDefinition  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE_SRCDATA = os.path.join(_HERE, "tests", "fixtures", "stock")
# Default master unit index relative to a real srcdata root.
BUILDINGS_CSV_REL = os.path.join("art", "buildings.csv")


def convert(srcdata: str, mech_stem: str, vehicle_stem: str):
    compbas = CompBas.load(os.path.join(srcdata, "objects", "compbas.csv"))
    defs = []
    if mech_stem:
        defs.append(extract_mech(
            os.path.join(srcdata, "objects", f"{mech_stem}.csv"),
            os.path.join(srcdata, "tgl", f"{mech_stem}.ini"),
            compbas,
        ))
    if vehicle_stem:
        defs.append(extract_vehicle(
            os.path.join(srcdata, "objects", f"{vehicle_stem}.fit"),
            os.path.join(srcdata, "tgl", f"{vehicle_stem}.ini"),
            compbas,
        ))
    return defs


def build_report(defs) -> dict:
    total_waivers = sum(len(d.waivers) for d in defs)
    return {
        "tool": "LEGACY-UNIT-CONVERTER-1",
        "units_converted": len(defs),
        "mechs": sum(1 for d in defs if d.identity.kind == "mech"),
        "vehicles": sum(1 for d in defs if d.identity.kind == "vehicle"),
        "total_waivers": total_waivers,
        "units": [d.to_report_dict() for d in defs],
        "deferred": (
            "legacy-vs-generated semantic diff is UNIT-DEFINITION-SHADOW-COMPARE-1 "
            "(next slice); not performed here"
        ),
    }


def render_report_text(report: dict) -> str:
    out = []
    out.append(f"=== {report['tool']} conversion report ===")
    out.append(f"units converted: {report['units_converted']} "
               f"({report['mechs']} mech, {report['vehicles']} vehicle)")
    out.append(f"total coherence waivers: {report['total_waivers']}")
    out.append("")
    for u in report["units"]:
        out.append(f"[{u['unit_id']}]  {u['name']}  ({u['kind']}, {u['tonnage']}t)")
        out.append(f"    jump={u['jump']} ({u['jump_evidence']})")
        out.append(f"    locomotion={u['locomotion']}  damage={u['damage_schema']}")
        out.append(f"    model={u['model']}  sockets={u['num_sockets']}  "
                   f"hardpoints={u['num_hardpoints']}")
        out.append(f"    loadout={u['num_loadout']} "
                   f"(bound {u['num_loadout_bound']}/{u['num_loadout']})")
        for w in u["waivers"]:
            out.append(f"    ! waiver: {w}")
        out.append("")
    out.append(f"deferred: {report['deferred']}")
    return "\n".join(out)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="MC2 legacy unit -> UnitDefinition converter (offline)")
    p.add_argument("--srcdata", default=None,
                   help="stock mc2srcdata root (default: bundled test fixtures)")
    p.add_argument("--mech", default="atlas", help="mech chassis stem to convert")
    p.add_argument("--vehicle", default="armoredcar", help="vehicle .fit stem to convert")
    p.add_argument("--out", default=None, help="output dir; if omitted, no files written")
    p.add_argument("--json", action="store_true", help="emit JSON report")
    p.add_argument("--batch", action="store_true",
                   help="convert the FULL MECH+VEHICLE roster from the master "
                        "index (buildings.csv) instead of one mech + one vehicle")
    p.add_argument("--buildings-csv", default=None,
                   help="master unit index (default: <srcdata>/art/buildings.csv)")
    args = p.parse_args(argv)

    srcdata = args.srcdata or FIXTURE_SRCDATA
    if not os.path.isdir(srcdata):
        print(f"error: srcdata not found: {srcdata}", file=sys.stderr)
        return 2

    if args.batch:
        buildings = args.buildings_csv or os.path.join(srcdata, BUILDINGS_CSV_REL)
        if not os.path.isfile(buildings):
            print(f"error: master index not found: {buildings}", file=sys.stderr)
            return 2
        result = _roster.convert_roster(srcdata, buildings, out_root=args.out)
        report = _roster.build_batch_report(result)
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print(_roster.render_batch_text(report))
        return 0

    defs = convert(srcdata, args.mech, args.vehicle)

    if args.out:
        for d in defs:
            emit.write_unit(d, args.out)

    report = build_report(defs)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(render_report_text(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
