#!/usr/bin/env python3
# tools/unit_converter/roster_shadow_compare.py
"""CONVERTER-ROSTER-BATCH-1 (Phase 3) — roster-wide shadow-compare gate.

ONE command that:
  1. batch-converts the FULL MECH+VEHICLE stock roster (roster.convert_roster),
  2. reads one or more tier1 (or multi-mission) debug_state dumps,
  3. diffs EVERY unit present in the dump(s) against its generated def across the
     engine-authoritative facets (jump / locomotion / damage_schema), and
  4. emits the aggregate acceptance verdict: units covered, per-facet match /
     mismatch counts, engine-only, converter-only, waived facets.

This is the acceptance gate that must be roster-wide green (mismatches == 0, with
waivers noted) before MC2_UNIT_DEFINITION_SOURCE could ever flip to `generated`.
ANY converter-vs-engine disagreement is surfaced as a FINDING, never papered over
— catching exactly that disagreement is the whole reason this gate exists.

Facet diff + join logic is REUSED verbatim from shadow_compare.py (single source
of truth). This module only adds roster batch-conversion + multi-dump merge.

Usage:
  py -3 tools/unit_converter/roster_shadow_compare.py \
      --srcdata A:/Games/mc2-opengl-src/mc2srcdata \
      --engine-dump v0.3/debug_state/history_mc2_01.json \
      --engine-dump v0.3/debug_state/history_mc2_24.json \
      [--out generated/units] [--json] [--strict]

--strict returns non-zero on any mismatch (CI gate). Pure / deterministic apart
from reading the given dumps + read-only source.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from unit_converter import roster as _roster  # noqa: E402
from unit_converter import shadow_compare as _sc  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE_SRCDATA = os.path.join(_HERE, "tests", "fixtures", "stock")
BUILDINGS_CSV_REL = os.path.join("art", "buildings.csv")


def merge_engine_dumps(dump_dicts):
    """Union per-unit facets across multiple dumps (multi-mission coverage).

    Later dumps do not overwrite an earlier unit UNLESS facets diverge, in which
    case the divergence is surfaced the same way shadow_compare surfaces an
    intra-dump conflict: under a `#conflict` unit_id so the diff can't hide it.
    """
    merged = {}
    for d in dump_dicts:
        for uid, facets in _sc.load_engine_facets(d).items():
            prev = merged.get(uid)
            if prev is None:
                merged[uid] = facets
            elif (prev.jump != facets.jump
                  or prev.locomotion != facets.locomotion
                  or prev.damage_schema != facets.damage_schema):
                merged[f"{uid}#conflict"] = facets
    return merged


def run(srcdata, buildings_csv, dump_paths, out_root=None):
    """Batch-convert + roster-diff. Returns (ShadowReport, batch_report_dict)."""
    result = _roster.convert_roster(srcdata, buildings_csv, out_root=out_root)
    batch_report = _roster.build_batch_report(result)
    converter = _sc.load_converter_facets(batch_report)

    dumps = []
    for p in dump_paths:
        with open(p, "r", encoding="utf-8") as fh:
            dumps.append(json.load(fh))
    engine = merge_engine_dumps(dumps)

    rep = _sc.compare(engine, converter)
    return rep, batch_report


def render_text(rep, batch_report) -> str:
    out = ["=== CONVERTER-ROSTER-BATCH-1 roster shadow-compare ==="]
    out.append(f"converted roster: {batch_report['units_converted']} units "
               f"({batch_report['mechs']} mech, {batch_report['vehicles']} vehicle); "
               f"skipped {batch_report['units_skipped']}")
    out.append("")
    out.append(_sc.render_text(rep))
    return "\n".join(out)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Roster-wide shadow-compare: batch-convert + diff every "
                    "dumped unit vs its generated def")
    p.add_argument("--srcdata", default=None,
                   help="stock mc2srcdata root (default: bundled test fixtures)")
    p.add_argument("--buildings-csv", default=None,
                   help="master unit index (default: <srcdata>/art/buildings.csv)")
    p.add_argument("--engine-dump", action="append", default=[], dest="engine_dumps",
                   required=True, help="debug_state dump JSON (repeatable for "
                                       "multi-mission coverage)")
    p.add_argument("--out", default=None, help="write generated defs under this dir")
    p.add_argument("--json", action="store_true", help="emit JSON report")
    p.add_argument("--strict", action="store_true",
                   help="exit non-zero on any mismatch (CI gate)")
    args = p.parse_args(argv)

    srcdata = args.srcdata or FIXTURE_SRCDATA
    if not os.path.isdir(srcdata):
        print(f"error: srcdata not found: {srcdata}", file=sys.stderr)
        return 2
    buildings = args.buildings_csv or os.path.join(srcdata, BUILDINGS_CSV_REL)
    if not os.path.isfile(buildings):
        print(f"error: master index not found: {buildings}", file=sys.stderr)
        return 2
    for pth in args.engine_dumps:
        if not os.path.isfile(pth):
            print(f"error: engine dump not found: {pth}", file=sys.stderr)
            return 2

    rep, batch_report = run(srcdata, buildings, args.engine_dumps, out_root=args.out)

    if args.json:
        payload = rep.to_dict()
        payload["roster"] = {
            "units_converted": batch_report["units_converted"],
            "mechs": batch_report["mechs"],
            "vehicles": batch_report["vehicles"],
            "units_skipped": batch_report["units_skipped"],
            "skipped": batch_report["skipped"],
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(render_text(rep, batch_report))

    if args.strict and rep.mismatches:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
