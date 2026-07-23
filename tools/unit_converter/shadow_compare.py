#!/usr/bin/env python3
# tools/unit_converter/shadow_compare.py
"""UNIT-DEFINITION-SHADOW-COMPARE-1 (Phase 3) — the safety-net gate.

Diff the ENGINE's runtime-derived unit facets (from a debug_state dump) against
the offline converter's generated UnitDefinition, per stock unit. This is the
gate that must be green before MC2_UNIT_DEFINITION_SOURCE could ever flip to
`generated`: it proves the converter faithfully captures what the engine derives
for the facets that are engine-authoritative TODAY.

Scope (bounded to engine-authoritative-for-stock facets):
  COMPARED : capability(jump), locomotion, damage_schema
  WAIVED   : hardpoints, sockets, full-UI  (not yet engine-authoritative for
             stock units; the engine dump does not derive them per-mover, so a
             diff would be meaningless — reported as explicitly waived)

Join key: engine dump row `chassis` (ObjectType appearance stem) -> the converter
unit_id `core:<stem.lower()>`. The engine emits canonical token names for
locomotion/damage_schema (locomotionModelName()/damageSchemaName()), which match
the converter's LOCO_*/DMG_* vocabulary, so facets compare as plain strings.

This tool is COMPARE-ONLY. It never flips a source-of-truth, never mutates data,
and (by default) does not hard-fail — a mismatch is a FINDING to surface, because
a disagreement is exactly what shadow-compare exists to catch. Pure, no RNG.

Usage:
  py -3 tools/unit_converter/shadow_compare.py \
      --engine-dump v0.3/debug_state/history_0001.json \
      --converter-report converter_report.json [--json] [--strict]

Both inputs may also be passed as already-parsed dicts via the library API
(load_engine_facets / load_converter_facets / compare).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Facets that are engine-authoritative for stock units TODAY and therefore
# compared. Keep in lockstep with the engine witness dump fields.
COMPARED_FACETS = ("jump", "locomotion", "damage_schema")
# Facets deliberately NOT compared (see module docstring).
WAIVED_FACETS = ("hardpoints", "sockets", "ui_full")

NAMESPACE = "core"


def chassis_to_unit_id(chassis: str, namespace: str = NAMESPACE) -> str:
    """Map an engine `chassis` stem to the converter's namespaced chassis id.

    Mirrors tools/unit_converter/extract.py::_namespaced (lower-cased stem).
    >>> chassis_to_unit_id("Atlas")
    'core:atlas'
    """
    return f"{namespace}:{chassis.strip().lower()}"


def _variant_slug(variant_name: str, index: int = 0) -> str:
    """Mirror extract.py::_variant_slug: stable, greppable variant suffix.
    >>> _variant_slug("Atlas Prime")
    'atlas_prime'
    >>> _variant_slug("Renard's Atlas")
    'renards_atlas'
    """
    cleaned = (variant_name or "").strip().lower().replace("'", "")
    slug = re.sub(r"[^a-z0-9]+", "_", cleaned).strip("_")
    return slug or f"v{index}"


def variant_join_id(chassis: str, variant: str = "", namespace: str = NAMESPACE) -> str:
    """LOADOUT-THREE-LAYER-1: compose the per-VARIANT join id the converter emits.

    Engine dump row (chassis, variant) -> converter unit_id. When `variant` is
    empty (vehicle / base Mover) the id is the bare chassis id, matching the
    converter's chassis-level unit_id and preserving the pre-Phase-3 join.
    >>> variant_join_id("Atlas", "Renard's Atlas")
    'core:atlas_renards_atlas'
    >>> variant_join_id("ArmoredCar", "")
    'core:armoredcar'
    """
    base = chassis_to_unit_id(chassis, namespace)
    variant = (variant or "").strip()
    if not variant:
        return base
    return f"{base}_{_variant_slug(variant)}"


@dataclass
class UnitFacets:
    """The three engine-authoritative facets for one unit, keyed by unit_id."""
    unit_id: str
    jump: Optional[bool] = None
    locomotion: Optional[str] = None
    damage_schema: Optional[str] = None

    def facet(self, name: str):
        return getattr(self, name)


@dataclass
class FacetDiff:
    unit_id: str
    facet: str
    engine: object
    converter: object
    match: bool


@dataclass
class ShadowReport:
    units_compared: int = 0
    matches: int = 0
    mismatches: List[FacetDiff] = field(default_factory=list)
    # dump units with no converter counterpart (converter scope is bounded)
    engine_only: List[str] = field(default_factory=list)
    # converter units never seen in the dump (not spawned in this mission)
    converter_only: List[str] = field(default_factory=list)
    per_facet_matches: Dict[str, int] = field(default_factory=dict)
    waived: List[str] = field(default_factory=list)

    @property
    def verdict(self) -> str:
        return "PASS" if not self.mismatches else "MISMATCH"

    def to_dict(self) -> dict:
        return {
            "tool": "UNIT-DEFINITION-SHADOW-COMPARE-1",
            "verdict": self.verdict,
            "units_compared": self.units_compared,
            "facet_comparisons_total": self.matches + len(self.mismatches),
            "facet_matches": self.matches,
            "facet_mismatches": len(self.mismatches),
            "per_facet_matches": dict(sorted(self.per_facet_matches.items())),
            "mismatches": [
                {
                    "unit_id": d.unit_id, "facet": d.facet,
                    "engine": d.engine, "converter": d.converter,
                }
                for d in self.mismatches
            ],
            "engine_only_units": sorted(self.engine_only),
            "converter_only_units": sorted(self.converter_only),
            "waived_facets": list(self.waived),
        }


# --- input adapters ---------------------------------------------------------

def load_engine_facets(dump: dict) -> Dict[str, UnitFacets]:
    """Extract per-unit facets from a parsed debug_state dump dict.

    Reads the `unitProfile.rows` section (present only when MC2_UNIT_PROFILE_DATA
    was ON). Rows without a `chassis` are skipped (can't be matched). Multiple
    movers of the same chassis collapse to one entry; a facet CONFLICT between
    two same-chassis movers is a finding surfaced as a distinct unit_id suffix.
    """
    out: Dict[str, UnitFacets] = {}
    section = (dump or {}).get("unitProfile") or {}
    rows = section.get("rows") or []
    for r in rows:
        chassis = (r.get("chassis") or "").strip()
        if not chassis:
            continue
        # LOADOUT-THREE-LAYER-1: join by (chassis, variant) so a spawned variant
        # diffs against the SAME converted variant, not the chassis' first (Prime)
        # variant. `variant` is absent in pre-Phase-3 dumps -> chassis-level join.
        variant = (r.get("variant") or "").strip()
        uid = variant_join_id(chassis, variant)
        facets = UnitFacets(
            unit_id=uid,
            jump=bool(r.get("cap_jump_baseline")),
            locomotion=r.get("locomotion"),
            damage_schema=r.get("damage_schema"),
        )
        prev = out.get(uid)
        if prev is not None and (
            prev.jump != facets.jump
            or prev.locomotion != facets.locomotion
            or prev.damage_schema != facets.damage_schema
        ):
            # same chassis, divergent facets across movers -> keep both under a
            # disambiguated id so the diff surfaces it rather than hiding it.
            out[f"{uid}#conflict"] = facets
        else:
            out[uid] = facets
    return out


def load_converter_facets(report: dict) -> Dict[str, UnitFacets]:
    """Extract per-unit facets from the converter's --json report dict.

    Each `units[]` entry is a UnitDefinition.to_report_dict() (unit_id, jump,
    locomotion, damage_schema, ...).
    """
    out: Dict[str, UnitFacets] = {}
    for u in (report or {}).get("units", []):
        uid = u.get("unit_id")
        if not uid:
            continue
        out[uid] = UnitFacets(
            unit_id=uid,
            jump=bool(u.get("jump")),
            locomotion=u.get("locomotion"),
            damage_schema=u.get("damage_schema"),
        )
    return out


# --- the diff ---------------------------------------------------------------

def compare(engine: Dict[str, UnitFacets],
            converter: Dict[str, UnitFacets]) -> ShadowReport:
    """Per-unit, per-facet diff over COMPARED_FACETS. Pure."""
    rep = ShadowReport(waived=list(WAIVED_FACETS))
    rep.per_facet_matches = {f: 0 for f in COMPARED_FACETS}

    common = sorted(set(engine) & set(converter))
    rep.engine_only = sorted(set(engine) - set(converter))
    rep.converter_only = sorted(set(converter) - set(engine))
    rep.units_compared = len(common)

    for uid in common:
        e, c = engine[uid], converter[uid]
        for facet in COMPARED_FACETS:
            ev, cv = e.facet(facet), c.facet(facet)
            if ev == cv:
                rep.matches += 1
                rep.per_facet_matches[facet] += 1
            else:
                rep.mismatches.append(
                    FacetDiff(unit_id=uid, facet=facet,
                              engine=ev, converter=cv, match=False))
    return rep


# --- rendering / CLI --------------------------------------------------------

def render_text(rep: ShadowReport) -> str:
    d = rep.to_dict()
    out = ["=== UNIT-DEFINITION-SHADOW-COMPARE-1 report ==="]
    out.append(f"verdict: {d['verdict']}")
    out.append(f"units compared: {d['units_compared']}  "
               f"(engine-only {len(d['engine_only_units'])}, "
               f"converter-only {len(d['converter_only_units'])})")
    out.append(f"facet comparisons: {d['facet_matches']}/"
               f"{d['facet_comparisons_total']} matched")
    for f, n in d["per_facet_matches"].items():
        out.append(f"    {f}: {n} matched")
    if d["mismatches"]:
        out.append("MISMATCHES (converter disagrees with engine — FINDING):")
        for m in d["mismatches"]:
            out.append(f"    ! {m['unit_id']} {m['facet']}: "
                       f"engine={m['engine']!r} converter={m['converter']!r}")
    if d["engine_only_units"]:
        out.append("engine-only (spawned but not converted — informational):")
        for u in d["engine_only_units"]:
            out.append(f"    - {u}")
    if d["converter_only_units"]:
        out.append("converter-only (converted but not spawned this mission):")
        for u in d["converter_only_units"]:
            out.append(f"    - {u}")
    out.append(f"waived facets (not engine-authoritative for stock yet): "
               f"{', '.join(d['waived_facets'])}")
    return "\n".join(out)


def _read_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Shadow-compare engine-derived facets vs converter output")
    p.add_argument("--engine-dump", required=True,
                   help="debug_state dump JSON (needs unitProfile.rows)")
    p.add_argument("--converter-report", required=True,
                   help="converter --json report")
    p.add_argument("--json", action="store_true", help="emit JSON report")
    p.add_argument("--strict", action="store_true",
                   help="exit non-zero on any mismatch (default: report-only)")
    args = p.parse_args(argv)

    for path in (args.engine_dump, args.converter_report):
        if not os.path.isfile(path):
            print(f"error: input not found: {path}", file=sys.stderr)
            return 2

    engine = load_engine_facets(_read_json(args.engine_dump))
    converter = load_converter_facets(_read_json(args.converter_report))
    rep = compare(engine, converter)

    if args.json:
        print(json.dumps(rep.to_dict(), indent=2, sort_keys=True))
    else:
        print(render_text(rep))

    if args.strict and rep.mismatches:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
