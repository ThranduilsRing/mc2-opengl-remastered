#!/usr/bin/env python3
# tools/unit_converter/unit_preview.py
"""UNIT-PREVIEW-DUMP-1 (Phase 3) — headless per-unit modder preview report.

Read the ENGINE's `unitPreview` section from a debug_state dump (emitted only
under MC2_UNIT_PROFILE_DATA=1) and print a clean, per-unit report a modder can
scan to verify a unit WITHOUT launching an interactive mission:

  * id / model (appearance chassis) / variant
  * declared presentation sockets, each flagged present/MISSING on the model
  * ActionAnimTags available vs missing (which intents resolve to a gesture the
    loaded skeleton actually provides, via the pure fallback resolver)
  * resolved UI icon asset
  * validateUnitDefinition() finding count + the first finding

The engine already RESOLVES all of this from already-loaded type data; this tool
is a pure FORMATTER over that dump section (plus, optionally, the converter
--json report for a chassis cross-reference). No RNG, no mutation, no asset load.

Exit code is NONZERO if any previewed unit has validator findings (so CI / a
modder's smoke can gate on "every stock unit previews clean").

Usage:
  py -3 tools/unit_converter/unit_preview.py \
      --engine-dump v0.3/debug_state/history_0001.json [--json] \
      [--converter-report converter_report.json]

Library API: load_previews(dump) -> List[UnitPreview]; format_report(previews).
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class SocketPreview:
    name: str
    present: bool


@dataclass
class UnitPreview:
    object_id: int
    name: str
    chassis: str
    variant: str
    is_mech: bool
    sockets: List[SocketPreview] = field(default_factory=list)
    missing_socket_count: int = 0
    anim_tags_available: List[str] = field(default_factory=list)
    anim_tags_missing: List[str] = field(default_factory=list)
    icon_resolved: str = ""
    finding_count: int = 0
    first_finding_code: int = -1
    first_finding_msg: str = ""

    @property
    def clean(self) -> bool:
        """A unit is clean iff the engine's validator reported zero findings."""
        return self.finding_count == 0

    @property
    def label(self) -> str:
        """Human/modder join label: chassis (+ variant if any)."""
        stem = self.chassis or self.name or f"obj{self.object_id}"
        return f"{stem}:{self.variant}" if self.variant else stem


def load_previews(dump: dict) -> List[UnitPreview]:
    """Parse the `unitPreview.rows` section of a debug_state dump.

    Returns [] when the section is absent (gate OFF) — the formatter then prints
    a single "no preview data" line rather than crashing.
    """
    section = (dump or {}).get("unitPreview") or {}
    rows = section.get("rows") or []
    out: List[UnitPreview] = []
    for r in rows:
        sockets = [
            SocketPreview(name=s.get("name", ""), present=bool(s.get("present", False)))
            for s in (r.get("sockets") or [])
        ]
        out.append(
            UnitPreview(
                object_id=int(r.get("objectId", -1)),
                name=r.get("name", "") or "",
                chassis=r.get("chassis", "") or "",
                variant=r.get("variant", "") or "",
                is_mech=bool(r.get("is_mech", False)),
                sockets=sockets,
                missing_socket_count=int(r.get("missing_socket_count", 0)),
                anim_tags_available=list(r.get("anim_tags_available") or []),
                anim_tags_missing=list(r.get("anim_tags_missing") or []),
                icon_resolved=r.get("icon_resolved", "") or "",
                finding_count=int(r.get("finding_count", 0)),
                first_finding_code=int(r.get("first_finding_code", -1)),
                first_finding_msg=r.get("first_finding_msg", "") or "",
            )
        )
    return out


def load_converter_chassis(report: Optional[dict]) -> Dict[str, dict]:
    """Optional cross-reference: map a converter report's chassis stem -> unit dict.

    The converter --json is a list of unit dicts (or {"units": [...]}). We key on
    a lower-cased chassis/model stem so a preview row can note whether the offline
    converter also knows the chassis. Absent/malformed -> {} (cross-ref skipped).
    """
    if not report:
        return {}
    units = report.get("units") if isinstance(report, dict) else report
    if not isinstance(units, list):
        return {}
    out: Dict[str, dict] = {}
    for u in units:
        if not isinstance(u, dict):
            continue
        stem = (u.get("chassis") or u.get("model") or u.get("unit_id") or "")
        stem = str(stem).split(":")[-1].strip().lower()
        if stem:
            out[stem] = u
    return out


def format_report(
    previews: List[UnitPreview],
    converter_chassis: Optional[Dict[str, dict]] = None,
) -> str:
    """Render a plain-text per-unit modder report."""
    converter_chassis = converter_chassis or {}
    if not previews:
        return "unit-preview: no preview data (gate OFF or no units loaded)\n"
    lines: List[str] = []
    lines.append(f"UNIT PREVIEW REPORT — {len(previews)} unit type(s)")
    lines.append("=" * 60)
    for p in previews:
        status = "CLEAN" if p.clean else f"{p.finding_count} FINDING(S)"
        lines.append("")
        lines.append(f"[{p.label}]  ({status})")
        lines.append(f"  model      : {p.chassis or '<none>'}"
                     + (f"  variant={p.variant}" if p.variant else "")
                     + (f"  mech" if p.is_mech else "  non-mech"))
        if converter_chassis:
            known = p.chassis.strip().lower() in converter_chassis
            lines.append(f"  converter  : {'known' if known else 'NOT in converter report'}")
        lines.append(f"  icon       : {p.icon_resolved or '<legacy/none>'}")
        if p.sockets:
            for s in p.sockets:
                flag = "ok" if s.present else "MISSING"
                lines.append(f"  socket     : {s.name}  [{flag}]")
            if p.missing_socket_count:
                lines.append(f"  sockets    : {p.missing_socket_count} declared socket(s) MISSING on model")
        else:
            lines.append("  sockets    : none declared")
        if p.is_mech:
            avail = ", ".join(p.anim_tags_available) or "<none>"
            lines.append(f"  anim avail : {avail}")
            if p.anim_tags_missing:
                lines.append(f"  anim MISS  : {', '.join(p.anim_tags_missing)}")
        else:
            lines.append("  anim       : n/a (non-mech, no skeleton)")
        if not p.clean:
            lines.append(f"  finding    : code={p.first_finding_code} {p.first_finding_msg}")
    lines.append("")
    lines.append("=" * 60)
    dirty = [p for p in previews if not p.clean]
    if dirty:
        lines.append(f"RESULT: {len(dirty)} unit(s) with findings: "
                     + ", ".join(p.label for p in dirty))
    else:
        lines.append(f"RESULT: all {len(previews)} unit(s) preview CLEAN")
    return "\n".join(lines) + "\n"


def previews_to_json(previews: List[UnitPreview]) -> dict:
    return {
        "version": 1,
        "unit_count": len(previews),
        "dirty_count": sum(1 for p in previews if not p.clean),
        "units": [
            {
                "label": p.label,
                "object_id": p.object_id,
                "chassis": p.chassis,
                "variant": p.variant,
                "is_mech": p.is_mech,
                "icon_resolved": p.icon_resolved,
                "sockets": [{"name": s.name, "present": s.present} for s in p.sockets],
                "missing_socket_count": p.missing_socket_count,
                "anim_tags_available": p.anim_tags_available,
                "anim_tags_missing": p.anim_tags_missing,
                "finding_count": p.finding_count,
                "first_finding_code": p.first_finding_code,
                "first_finding_msg": p.first_finding_msg,
                "clean": p.clean,
            }
            for p in previews
        ],
    }


def _load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Headless per-unit preview report (UNIT-PREVIEW-DUMP-1).")
    ap.add_argument("--engine-dump", required=True,
                    help="debug_state dump JSON (needs unitPreview.rows; MC2_UNIT_PROFILE_DATA=1)")
    ap.add_argument("--converter-report", default=None,
                    help="optional converter --json report for a chassis cross-reference")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON instead of text")
    args = ap.parse_args(argv)

    dump = _load_json(args.engine_dump)
    previews = load_previews(dump)
    conv = load_converter_chassis(_load_json(args.converter_report)) if args.converter_report else {}

    if args.json:
        print(json.dumps(previews_to_json(previews), indent=2, sort_keys=True))
    else:
        sys.stdout.write(format_report(previews, conv))

    # Nonzero exit if ANY unit has validator findings (gate for CI / modder smoke).
    return 1 if any(not p.clean for p in previews) else 0


if __name__ == "__main__":
    raise SystemExit(main())
