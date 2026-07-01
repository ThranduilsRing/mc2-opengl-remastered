#!/usr/bin/env python3
# tools/unit_converter/weapon_shadow_compare.py
"""WEAPON-SHADOW-COMPARE-1 (equipment arc, slice 5) — the equipment safety-net.

Diff the compbas converter's generated EquipmentDefinitions against the ENGINE's
authoritative parse of compbas.csv (MasterComponent::masterList), per stock
component. The equipment mirror of shadow_compare.py / roster_shadow_compare.py:
it proves the offline converter faithfully captures what the engine derives, for
the fields that are engine-authoritative TODAY, before any source-of-truth flip.

Join key: engine dump row `masterId` (compbas row ordinal) <-> the converter
report's `legacy_master_id` (the SAME ordinal, by construction). Both sides drop
the `undefined`/blank sentinel slots, so the live-component row sets align.

Per-component COMPARED fields (engine-authoritative for stock):
  kind          : coarse EquipmentKind family (engine derives from ComponentForm,
                  converter from the CSV Type token -> agree by construction; a
                  disagreement is a converter mis-map FINDING)
  is_weapon     : whether the component carries a WeaponFacet
  category      : WeaponTag (Energy/Ballistic/Missile/Any) — weapons only
  damage/heat/recycle : weapon combat stats — weapons only
  range_bucket  : short/medium/long/"" — weapons only
  combat_value  : MasterComponent CV vs converter BR column
  ammo_master_id: stock ammo master-id column (WEAPON_AMMO_OFFSET provenance)
  crit_slots    : InventorySize (size) vs converter crit_slots

WAIVED (not engine-authoritative / best-effort / presentation):
  fire_executor : a best-effort converter DEFAULT (no engine equivalent)
  ammo_category : a named converter id derived from ammo_master_id (compared via
                  the raw ammo_master_id instead)
  tags / presentation (icon / ui string id / special-fx) : presentation-only
  tonnage / allowed_locations : not emitted by this engine witness (kept minimal)

COMPARE-ONLY: never flips a source-of-truth, never mutates data. By default it
does NOT hard-fail — a mismatch is a FINDING to surface plainly, because a
disagreement on a stock component field (e.g. a mis-parsed column) is exactly
what this gate exists to catch. `--strict` exits non-zero on any mismatch (CI).
Pure, no RNG.

Usage:
  py -3 tools/unit_converter/weapon_shadow_compare.py \
      --engine-dump v0.3/debug_state/history_0001.json \
      --converter-report equipment_report.json [--json] [--strict]

Both inputs may also be passed as parsed dicts via the library API
(load_engine_components / load_converter_components / compare).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Fields compared for EVERY live component (weapon-only fields are compared only
# when both sides agree the component is a weapon; see compare()).
COMMON_FACETS = ("kind", "is_weapon", "combat_value", "crit_slots")
WEAPON_FACETS = ("category", "damage", "heat", "recycle", "range_bucket",
                 "ammo_master_id")
# Fields deliberately NOT compared (see module docstring).
WAIVED_FIELDS = ("fire_executor", "ammo_category", "tags", "presentation",
                 "tonnage", "allowed_locations")

# Float compare tolerance (compbas stats are small integers/one-decimal floats).
_EPS = 1e-4


def _feq(a, b) -> bool:
    """Field-equality that treats numerics with a tolerance and everything else
    by value. None-safe."""
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(a, bool) or isinstance(b, bool):
            return bool(a) == bool(b)
        return math.isclose(float(a), float(b), abs_tol=_EPS)
    return a == b


@dataclass
class ComponentFacets:
    """The engine-authoritative facets for ONE component, keyed by masterId."""
    master_id: int
    kind: Optional[str] = None
    is_weapon: Optional[bool] = None
    category: Optional[str] = None
    damage: Optional[float] = None
    heat: Optional[float] = None
    recycle: Optional[float] = None
    range_bucket: Optional[str] = None
    combat_value: Optional[float] = None
    ammo_master_id: Optional[int] = None
    crit_slots: Optional[int] = None

    def facet(self, name: str):
        return getattr(self, name)


@dataclass
class FieldDiff:
    master_id: int
    field: str
    engine: object
    converter: object


@dataclass
class WeaponShadowReport:
    components_compared: int = 0
    field_matches: int = 0
    mismatches: List[FieldDiff] = field(default_factory=list)
    # dump components with no converter counterpart (converter dropped as waiver)
    engine_only: List[int] = field(default_factory=list)
    # converter components never seen in the dump
    converter_only: List[int] = field(default_factory=list)
    per_field_matches: Dict[str, int] = field(default_factory=dict)
    per_field_mismatches: Dict[str, int] = field(default_factory=dict)
    waived: List[str] = field(default_factory=list)

    @property
    def verdict(self) -> str:
        return "PASS" if not self.mismatches else "MISMATCH"

    def to_dict(self) -> dict:
        return {
            "tool": "WEAPON-SHADOW-COMPARE-1",
            "verdict": self.verdict,
            "components_compared": self.components_compared,
            "field_comparisons_total": self.field_matches + len(self.mismatches),
            "field_matches": self.field_matches,
            "field_mismatches": len(self.mismatches),
            "per_field_matches": dict(sorted(self.per_field_matches.items())),
            "per_field_mismatches": dict(sorted(self.per_field_mismatches.items())),
            "mismatches": [
                {
                    "master_id": d.master_id, "field": d.field,
                    "engine": d.engine, "converter": d.converter,
                }
                for d in self.mismatches
            ],
            "engine_only_components": sorted(self.engine_only),
            "converter_only_components": sorted(self.converter_only),
            "waived_fields": list(self.waived),
        }


# --- input adapters ---------------------------------------------------------

def load_engine_components(dump: dict) -> Dict[int, ComponentFacets]:
    """Extract per-component facets from a parsed debug_state dump dict.

    Reads the `masterComponents.rows` section (present only when
    MC2_UNIT_PROFILE_DATA was ON). Keyed by masterId. Sentinel slots are already
    dropped engine-side."""
    out: Dict[int, ComponentFacets] = {}
    section = (dump or {}).get("masterComponents") or {}
    for r in section.get("rows") or []:
        mid = r.get("masterId")
        if mid is None:
            continue
        out[int(mid)] = ComponentFacets(
            master_id=int(mid),
            kind=r.get("kind"),
            is_weapon=bool(r.get("is_weapon")),
            category=r.get("category") or "",
            damage=r.get("damage"),
            heat=r.get("heat"),
            recycle=r.get("recycle"),
            range_bucket=r.get("range_bucket") or "",
            combat_value=r.get("combat_value"),
            ammo_master_id=r.get("ammo_master_id"),
            crit_slots=r.get("crit_slots"),
        )
    return out


def load_converter_components(report: dict) -> Dict[int, ComponentFacets]:
    """Extract per-component facets from the converter's --json report dict.

    Each `components[]` entry is an EquipmentDefinition.to_report_dict(). Weapon
    fields live under the nested `weapon` block (absent for non-weapons)."""
    out: Dict[int, ComponentFacets] = {}
    for c in (report or {}).get("components", []):
        mid = c.get("legacy_master_id")
        if mid is None or int(mid) < 0:
            continue
        w = c.get("weapon") or {}
        is_weapon = c.get("kind") == "Weapon"
        out[int(mid)] = ComponentFacets(
            master_id=int(mid),
            kind=c.get("kind"),
            is_weapon=is_weapon,
            category=w.get("category") or "",
            damage=w.get("damage"),
            heat=w.get("heat"),
            recycle=w.get("recycle"),
            range_bucket=w.get("range_bucket") or "",
            combat_value=w.get("combat_value"),
            ammo_master_id=w.get("ammo_master_id"),
            crit_slots=w.get("crit_slots"),
        )
    return out


# --- the diff ---------------------------------------------------------------

def compare(engine: Dict[int, ComponentFacets],
            converter: Dict[int, ComponentFacets]) -> WeaponShadowReport:
    """Per-component, per-field diff. Pure.

    Common facets are compared for every joined component. Weapon-only facets are
    compared only when BOTH sides classify the component as a weapon (an
    is_weapon disagreement is itself a mismatch surfaced via COMMON_FACETS, so a
    non-weapon on one side doesn't spuriously flood damage/heat diffs)."""
    rep = WeaponShadowReport(waived=list(WAIVED_FIELDS))
    all_fields = COMMON_FACETS + WEAPON_FACETS
    rep.per_field_matches = {f: 0 for f in all_fields}
    rep.per_field_mismatches = {f: 0 for f in all_fields}

    common = sorted(set(engine) & set(converter))
    rep.engine_only = sorted(set(engine) - set(converter))
    rep.converter_only = sorted(set(converter) - set(engine))
    rep.components_compared = len(common)

    for mid in common:
        e, c = engine[mid], converter[mid]
        fields = list(COMMON_FACETS)
        if e.is_weapon and c.is_weapon:
            fields += list(WEAPON_FACETS)
        for f in fields:
            ev, cv = e.facet(f), c.facet(f)
            if _feq(ev, cv):
                rep.field_matches += 1
                rep.per_field_matches[f] += 1
            else:
                rep.mismatches.append(
                    FieldDiff(master_id=mid, field=f, engine=ev, converter=cv))
                rep.per_field_mismatches[f] += 1
    return rep


# --- rendering / CLI --------------------------------------------------------

def render_text(rep: WeaponShadowReport) -> str:
    d = rep.to_dict()
    out = ["=== WEAPON-SHADOW-COMPARE-1 report ==="]
    out.append(f"verdict: {d['verdict']}")
    out.append(f"components compared: {d['components_compared']}  "
               f"(engine-only {len(d['engine_only_components'])}, "
               f"converter-only {len(d['converter_only_components'])})")
    out.append(f"field comparisons: {d['field_matches']}/"
               f"{d['field_comparisons_total']} matched")
    for f, n in d["per_field_matches"].items():
        mm = d["per_field_mismatches"].get(f, 0)
        tail = f"  ({mm} MISMATCH)" if mm else ""
        out.append(f"    {f:16} {n} matched{tail}")
    if d["mismatches"]:
        out.append("MISMATCHES (converter disagrees with engine — FINDING):")
        for m in d["mismatches"]:
            out.append(f"    ! masterId {m['master_id']} {m['field']}: "
                       f"engine={m['engine']!r} converter={m['converter']!r}")
    if d["engine_only_components"]:
        out.append("engine-only components (loaded but not converted — "
                   "informational): "
                   + ", ".join(str(x) for x in d["engine_only_components"]))
    if d["converter_only_components"]:
        out.append("converter-only components (converted but not in dump): "
                   + ", ".join(str(x) for x in d["converter_only_components"]))
    out.append(f"waived fields (not engine-authoritative / best-effort): "
               f"{', '.join(d['waived_fields'])}")
    return "\n".join(out)


def _read_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Shadow-compare engine MasterComponent parse vs "
                    "equipment converter output")
    p.add_argument("--engine-dump", required=True,
                   help="debug_state dump JSON (needs masterComponents.rows)")
    p.add_argument("--converter-report", required=True,
                   help="equipment_converter --json report")
    p.add_argument("--json", action="store_true", help="emit JSON report")
    p.add_argument("--strict", action="store_true",
                   help="exit non-zero on any mismatch (default: report-only)")
    args = p.parse_args(argv)

    for path in (args.engine_dump, args.converter_report):
        if not os.path.isfile(path):
            print(f"error: input not found: {path}", file=sys.stderr)
            return 2

    engine = load_engine_components(_read_json(args.engine_dump))
    converter = load_converter_components(_read_json(args.converter_report))
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
