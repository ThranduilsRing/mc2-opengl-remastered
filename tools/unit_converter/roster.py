# tools/unit_converter/roster.py
"""CONVERTER-ROSTER-BATCH-1 (Phase 3) — roster enumeration + batch conversion.

The master unit index is `mc2srcdata/.../art/buildings.csv`. Column 4 (0-based
index 3, header "Type") is the unit species; MECH and VEHICLE rows are the stock
combat roster (per LogisticsData::initVariants recon). This module reads that
index, resolves each unit's stock source files, converts each via the existing
extract_mech / extract_vehicle, and produces ONE aggregate report over the whole
roster.

Source-file resolution (filesystem is lower-cased; buildings.csv col1 is mixed):
  MECH     stem.csv  + tgl/stem.ini        (extract_mech)
  VEHICLE  stem.fit  + tgl/stem.ini        (extract_vehicle)

Missing / malformed rows and units whose stock source files are absent are NOT
fatal: they are SKIPPED and surfaced in the report with a reason (some stock
VEHICLE index rows — legion/lrmc/mog/monsoon/storm/trooptransport — are variants
whose base object .fit lives under a different stem, so they have no direct
`<stem>.fit`). Deterministic; no RNG, no engine, reads read-only source.
"""
from __future__ import annotations

import csv as _csv
import os
from dataclasses import dataclass, field
from typing import List, Optional

from . import emit
from .extract import extract_mech, extract_vehicle
from .fitparse import CompBas

# buildings.csv column layout (0-based). Only NAME and TYPE are load-bearing here.
_COL_NAME = 0
_COL_TYPE = 3

# Species we convert. Everything else (BUILDING/TREE/...) is not a combat unit.
CONVERTIBLE_TYPES = ("MECH", "VEHICLE")


@dataclass
class RosterEntry:
    """One index row resolved against the filesystem."""
    name: str            # buildings.csv col1 (display stem, mixed case)
    stem: str            # lower-cased filesystem stem
    kind: str            # "mech" | "vehicle"


@dataclass
class SkipRecord:
    stem: str
    kind: str
    reason: str


@dataclass
class BatchResult:
    """Outcome of a whole-roster batch conversion."""
    defs: list = field(default_factory=list)          # List[UnitDefinition]
    skipped: List[SkipRecord] = field(default_factory=list)
    written_dirs: List[str] = field(default_factory=list)


# --- index enumeration ------------------------------------------------------

def enumerate_roster(buildings_csv_path: str) -> List[RosterEntry]:
    """Parse buildings.csv and return the MECH + VEHICLE roster entries.

    Malformed rows (too few columns, blank name) are skipped silently at the
    enumeration layer — they never had a unit to convert. The header row
    (Type == "Type") is skipped by the same too-few / non-convertible checks.
    Order follows the index file (deterministic).
    """
    entries: List[RosterEntry] = []
    with open(buildings_csv_path, "r", encoding="latin-1", newline="") as fh:
        for row in _csv.reader(fh):
            if len(row) <= _COL_TYPE:
                continue
            name = (row[_COL_NAME] or "").strip()
            utype = (row[_COL_TYPE] or "").strip().upper()
            if not name or utype not in CONVERTIBLE_TYPES:
                continue
            entries.append(RosterEntry(
                name=name, stem=name.lower(),
                kind="mech" if utype == "MECH" else "vehicle",
            ))
    return entries


# --- per-unit source resolution + conversion --------------------------------

def _convert_entry(entry: RosterEntry, srcdata: str, compbas: CompBas):
    """Convert one roster entry, or return a SkipRecord with a reason.

    Returns (UnitDefinition, None) on success or (None, SkipRecord) on any
    missing source / parse failure. Never raises for a bad unit — the batch
    must survive the whole roster.
    """
    tgl_path = os.path.join(srcdata, "tgl", f"{entry.stem}.ini")
    if entry.kind == "mech":
        src_path = os.path.join(srcdata, "objects", f"{entry.stem}.csv")
    else:
        src_path = os.path.join(srcdata, "objects", f"{entry.stem}.fit")

    if not os.path.isfile(src_path):
        return None, SkipRecord(entry.stem, entry.kind,
                                f"missing source file: {os.path.basename(src_path)}")
    if not os.path.isfile(tgl_path):
        return None, SkipRecord(entry.stem, entry.kind,
                                f"missing tgl model ini: {entry.stem}.ini")
    try:
        if entry.kind == "mech":
            ud = extract_mech(src_path, tgl_path, compbas)
        else:
            ud = extract_vehicle(src_path, tgl_path, compbas)
    except Exception as exc:  # malformed source — skip, don't crash the batch
        return None, SkipRecord(entry.stem, entry.kind,
                                f"conversion error: {type(exc).__name__}: {exc}")
    return ud, None


def convert_roster(srcdata: str, buildings_csv_path: str,
                   out_root: Optional[str] = None) -> BatchResult:
    """Convert the entire MECH + VEHICLE roster. Pure w.r.t. source; writes only
    under out_root when given. Deterministic (index order)."""
    compbas = CompBas.load(os.path.join(srcdata, "objects", "compbas.csv"))
    result = BatchResult()
    for entry in enumerate_roster(buildings_csv_path):
        ud, skip = _convert_entry(entry, srcdata, compbas)
        if skip is not None:
            result.skipped.append(skip)
            continue
        result.defs.append(ud)
        if out_root:
            result.written_dirs.append(emit.write_unit(ud, out_root))
    return result


# --- aggregate report -------------------------------------------------------

def build_batch_report(result: BatchResult) -> dict:
    defs = result.defs
    total_waivers = sum(len(d.waivers) for d in defs)
    return {
        "tool": "CONVERTER-ROSTER-BATCH-1",
        "units_converted": len(defs),
        "mechs": sum(1 for d in defs if d.identity.kind == "mech"),
        "vehicles": sum(1 for d in defs if d.identity.kind == "vehicle"),
        "units_skipped": len(result.skipped),
        "total_waivers": total_waivers,
        "units": [d.to_report_dict() for d in sorted(defs, key=lambda d: d.identity.unit_id)],
        "skipped": [
            {"unit_id": f"core:{s.stem}", "stem": s.stem, "kind": s.kind, "reason": s.reason}
            for s in sorted(result.skipped, key=lambda s: s.stem)
        ],
    }


def render_batch_text(report: dict) -> str:
    out = [f"=== {report['tool']} roster conversion report ==="]
    out.append(f"units converted: {report['units_converted']} "
               f"({report['mechs']} mech, {report['vehicles']} vehicle)")
    out.append(f"units skipped:   {report['units_skipped']}")
    out.append(f"total coherence waivers: {report['total_waivers']}")
    out.append("")
    for u in report["units"]:
        out.append(f"[{u['unit_id']}]  {u['name']}  ({u['kind']}, {u['tonnage']}t)  "
                   f"jump={u['jump']} loco={u['locomotion']} dmg={u['damage_schema']}  "
                   f"waivers={len(u['waivers'])}")
    if report["skipped"]:
        out.append("")
        out.append("skipped (source absent / malformed — not converted):")
        for s in report["skipped"]:
            out.append(f"    - {s['stem']} ({s['kind']}): {s['reason']}")
    return "\n".join(out)
