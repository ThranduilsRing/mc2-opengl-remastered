"""Extract a read-only corpus: py -3 tools/mapgen/extract.py --all."""
from __future__ import annotations

import argparse
import hashlib
import math
import re
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import (CELL_WU, SCHEMA, STEEP_DEG, INSTALL_ROOT,
                                output_path, slope_grid, spans, world_cell, write_json)
from tools.mission_codec import (OBJECT, Install, catalog, fit_sections, object_records,
                                 overlay_catalog, packet, parent_cell)

INSTALLS = {"stock": "mc2-win64-v0.4", "MC2X-CVE-G": "MC2X-CVE-G",
            "MC2X-REAP3": "MC2X-REAP3", "Exodus": "MC2-Exodus",
            "MagicExpansion": "MC2-MagicExpansion",
            "MechCommanderOmnitech": "MechCommanderOmnitech", "MCO-Carver5": "MCO-Carver5"}
DEFAULT_OUT = Path(__file__).resolve().parent / "corpus"
VERTEX = np.dtype([("normal", "<f4", (3,)), ("height", "<f4"),
                   ("texture", "<u4"), ("light", "<u4"), ("type", "<u4"),
                   ("selected", "u1"), ("water", "u1"), ("shadow", "u1"), ("highlight", "u1")])


def extract_mission(install, name, types, catalog_sources, overlays, overlay_source, label):
    stem = f"data/missions/{name}"
    raw, pak_source = install.read(stem + ".pak")
    fit_raw, fit_source = install.read(stem + ".fit")
    fit = fit_sections(fit_raw)
    terrain_raw = packet(raw, 0)
    side = math.isqrt(len(terrain_raw) // 32)
    if side < 2 or side * side * 32 != len(terrain_raw):
        raise ValueError(f"packet 0 is not a square PostcompVertex array: {len(terrain_raw)}")
    vertices = np.frombuffer(terrain_raw, dtype=VERTEX).reshape(side, side)
    heights = vertices["height"].astype(float)
    slopes = slope_grid(heights, 128)
    overlay_handles = vertices["texture"] >> 16
    overlay_ids = np.full((side, side), -1, dtype=int)
    for handle, values in overlays.items():
        overlay_ids[overlay_handles == handle] = values["id"]
    unknown = sorted(int(n) for n in np.unique(overlay_handles)
                     if n not in (0, 0xFFFF) and n not in overlays)
    road = (overlay_ids >= 0) & (overlay_ids != 2)
    cement = (vertices["type"] == 10) | ((vertices["type"] >= 13) & (vertices["type"] <= 20))
    wet = (vertices["water"] & 1) != 0
    objects_raw = packet(raw, 1)
    objects = []
    unresolved = set()
    for i, record in enumerate(object_records(objects_raw)):
        number, x, y, z, rotation, damage, team, parent, pad0, pad1 = record
        info = types.get(number)
        if info is None:
            unresolved.add(number)
            info = {"class": "unknown", "type_name": f"type_{number}"}
        cell = world_cell(x, y, side)
        objects.append({**info, "id": f"object_{i}", "type_id": number, "cell": cell,
                        "position_wu": [round(x, 4), round(y, 4), round(z, 4)],
                        "rotation": round(rotation, 4), "team": team, "damage": damage,
                        "parent_cell": parent_cell(parent),
                        "parent_id_raw": parent, "parent_link": None})
    by_cell = {}
    for obj in objects:
        by_cell.setdefault(tuple(obj["cell"]), []).append(obj)
    missing_parents = 0
    for obj in objects:
        if obj["parent_cell"] is not None:
            candidates = by_cell.get(tuple(obj["parent_cell"]), [])
            expected = {"turret": "turret_control", "turret_control": "turret_generator",
                        "gate": "gate_control"}.get(obj["class"])
            preferred = [v for v in candidates if v["class"] == expected]
            # Ambiguous cells are preserved; no invented link.
            chosen = preferred if preferred else candidates
            if len(chosen) == 1:
                obj["parent_link"] = chosen[0]["id"]
            else:
                missing_parents += 1
    parts, starts, objectives = [], [], []
    for section, fields in fit.items():
        if re.fullmatch(r"part\d+", section) and {"positionx", "positiony"} <= fields.keys():
            cell = world_cell(fields["positionx"], fields["positiony"], side)
            player = bool(fields.get("playerpart", False))
            part = {"id": section, "cell": cell, "team": fields.get("teamid", -1),
                    "type_name": fields.get("csvfile", fields.get("objectprofile", "unit")),
                    "player": player, "active": fields.get("active", 1),
                    "exists": fields.get("exists", 1)}
            parts.append(part)
            if player and part["exists"] and part["active"]:
                starts.append(cell)
        if re.fullmatch(r"team\d+objective\d+", section):
            marker = None
            if {"markerx", "markery"} <= fields.keys():
                marker = world_cell(fields["markerx"], fields["markery"], side)
            conditions = []
            for child, values in fit.items():
                if re.fullmatch(re.escape(section) + r"condition\d+", child):
                    conditions.append(values)
            objectives.append({"id": section, "title": fields.get("title", ""), "cell": marker,
                               "display": bool(fields.get("displaymarker", False)),
                               "hidden": bool(fields.get("hiddentrigger", False)),
                               "conditions": conditions})
    start = [round(sum(c[i] for c in starts)/len(starts)) for i in (0, 1)] if starts else None
    warning = []
    if unknown:
        warning.append(f"unmapped overlay handles: {unknown}")
    if unresolved:
        warning.append(f"unresolved object type IDs: {sorted(unresolved)}")
    if missing_parents:
        warning.append(f"unresolved/ambiguous parent cells: {missing_parents}")
    outside = sum(not (0 <= o["cell"][0] < side*3 and 0 <= o["cell"][1] < side*3) for o in objects)
    if outside:
        warning.append(f"objects outside map bounds (preserved): {outside}")
    return {"schema": SCHEMA, "name": name, "source": {"corpus": label, "pak": pak_source,
            "fit": fit_source, "pak_sha256": hashlib.sha256(raw).hexdigest(),
            "fit_sha256": hashlib.sha256(fit_raw).hexdigest(),
            "terrain_objects_sha256": hashlib.sha256(terrain_raw+objects_raw).hexdigest(),
            **catalog_sources, "overlay_catalog": overlay_source},
            "size": {"rows": side*3, "cols": side*3, "vertices": side, "cell_wu": CELL_WU},
            "objects": objects, "parts": parts, "player_start": start, "objectives": objectives,
            "terrain": {"cell_step": 3, "elevation_wu": np.round(heights, 3).tolist(),
                        "slope_deg": np.round(slopes, 3).tolist(),
                        "terrain_type": vertices["type"].tolist(),
                        "overlay_handle": overlay_handles.tolist(), "overlay_type": overlay_ids.tolist(),
                        "water_elevation_wu": fit.get("water", {}).get("elevation")},
            "layers": {"roads": spans(road, 3), "cement": spans(cement, 3), "water": spans(wet, 3),
                       "cliffs": spans(slopes >= STEEP_DEG, 3), "runway": spans(overlay_ids == 7, 3)},
            "warnings": warning}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="stock and all six mod installs")
    parser.add_argument("--install", type=Path, help="read one custom install")
    parser.add_argument("--label", default="custom")
    parser.add_argument("--root", type=Path, default=INSTALL_ROOT)
    parser.add_argument("--mission", action="append", help="limit to these stems (repeatable)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    stock = Install(args.root / INSTALLS["stock"])
    dependency = Install(args.root / INSTALLS["MechCommanderOmnitech"], fallback=stock)
    installs = [(args.label, args.install)] if args.install else [
        (label, args.root / folder) for label, folder in INSTALLS.items() if args.all or label == "stock"]
    out = output_path(args.out, [p for _, p in installs])
    manifest = {"schema": SCHEMA, "missions": [], "errors": [], "installs": {}}
    for label, root in installs:
        try:
            if not root.is_dir():
                raise FileNotFoundError(root)
            # Carver5 is an expansion over Omnitech, not an independent catalog.
            fallback = dependency if label == "MCO-Carver5" else stock
            install = stock if label == "stock" else Install(root, fallback=fallback)
            types, sources = catalog(install)
            overlays, overlay_source = overlay_catalog(install)
            names = sorted(n for n in install.files if n.startswith("data/missions/") and n.endswith(".pak"))
            missing = [n for n in names if not install.has(n[:-4] + ".fit")]
            manifest["installs"][label] = {"root": str(root), "paks": len(names),
                "without_fit": missing, "archive_collisions": install.collisions,
                **sources, "overlay_catalog": overlay_source}
            for filename in names:
                name = filename[len("data/missions/"):-4]
                if (args.mission and name not in args.mission) or filename in missing:
                    continue
                try:
                    mission = extract_mission(install, name, types, sources, overlays, overlay_source, label)
                    relative = f"{label}/{name}.json"
                    write_json(out / relative, mission)
                    manifest["missions"].append({"file": relative, "name": name, "corpus": label,
                        "size": mission["size"], "objects": len(mission["objects"]),
                        "source": mission["source"], "warnings": mission["warnings"]})
                    print(f"{label}/{name}: {mission['size']['rows']} cells, {len(mission['objects'])} objects", flush=True)
                except (ValueError, OSError, struct.error, zlib.error) as error:
                    manifest["errors"].append({"corpus": label, "mission": name, "error": str(error)})
        except (ValueError, OSError, struct.error) as error:
            manifest["errors"].append({"corpus": label, "error": str(error)})
        write_json(out / "index.json", manifest, pretty=True)
    print(f"Extracted {len(manifest['missions'])}; errors {len(manifest['errors'])}; index {out/'index.json'}")
    return int(bool(manifest["errors"]))


if __name__ == "__main__":
    raise SystemExit(main())
