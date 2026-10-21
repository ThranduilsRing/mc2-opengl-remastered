"""Validate generated cell plans; authored missions are measured, not repaired."""
from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import (FLAT_DEG, SCHEMA, components, distance, distance_field,
                                load_json, mask_of, packed_cell, slope_grid)


def validate(layout, profile=None):
    """Return actionable errors, including measured corpus envelopes.

    This is a point-cell plausibility check, not engine collision/pathing proof.
    """
    errors = []
    if layout.get("schema") != SCHEMA:
        return ["unsupported layout schema"]
    rows, cols = layout["size"]["rows"], layout["size"]["cols"]
    shape = (rows, cols)
    terrain = layout["terrain"]
    if terrain.get("cell_step") != 1:
        return ["validation requires a generated cell-resolution heightfield"]
    heights = np.asarray(terrain["elevation_wu"], dtype=float)
    if heights.shape != shape or not np.isfinite(heights).all():
        return ["heightfield shape or finite-value check failed"]
    slopes = slope_grid(heights)
    for name, runs in layout["layers"].items():
        if any(len(run) != 3 or not (0 <= run[0] < rows and 0 <= run[1] < run[2] <= cols) for run in runs):
            return [f"invalid {name} layer spans"]
    wet, roads, cement = (mask_of(layout, name) for name in ("water", "roads", "cement"))
    allowed = ~wet & (slopes < FLAT_DEG)
    if np.any(roads & ~allowed):
        errors.append("roads cross water or ground above the flatness limit")
    if np.any(cement & ~allowed):
        errors.append("cement covers water or ground above the flatness limit")
    if np.any(mask_of(layout, "runway") & ~roads):
        errors.append("runway is disconnected from road surface")
    road_groups = components(roads)
    if len(road_groups) != 1:
        errors.append(f"roads have {len(road_groups)} components; expected one")
    edges = sum(bool(side.any()) for side in (roads[0], roads[-1], roads[:,0], roads[:,-1]))
    if edges < 2:
        errors.append("roads do not reach two map edges")
    objects = layout["objects"]
    by_id = {o["id"]: o for o in objects}
    if len(by_id) != len(objects):
        errors.append("object IDs are not unique")
    by_cell, batteries, by_base = {}, Counter(), defaultdict(list)
    for obj in objects:
        cell = tuple(obj["cell"])
        if len(cell) != 2 or not (0 <= cell[0] < rows and 0 <= cell[1] < cols):
            errors.append(f"{obj['id']}: outside map")
            continue
        if cell in by_cell:
            errors.append(f"{obj['id']}: shares a cell with {by_cell[cell]['id']}")
        by_cell[cell] = obj
        if not allowed[cell]:
            errors.append(f"{obj['id']}: on water or ground above the flatness limit")
        if roads[cell] and obj["class"] not in ("gate", "bridge"):
            errors.append(f"{obj['id']}: blocks a road")
        if obj.get("base_id"):
            by_base[obj["base_id"]].append(obj)
        expected = {"turret": "turret_control", "turret_control": "turret_generator", "gate": "gate_control"}.get(obj["class"])
        parent = by_id.get(obj.get("parent_link"))
        if expected and (not parent or parent["class"] != expected):
            errors.append(f"{obj['id']}: missing {expected} parent")
        if parent:
            if parent["team"] != obj["team"]:
                errors.append(f"{obj['id']}: parent belongs to another team")
            if obj.get("parent_cell") != parent["cell"] or obj.get("parent_id_raw") != packed_cell(parent["cell"]):
                errors.append(f"{obj['id']}: parent link and packed cell disagree")
            if obj["class"] == "turret":
                batteries[parent["id"]] += 1
            if profile and expected:
                # Batteries intentionally use conservative stock chain distances,
                # even when a mod's site mix is selected.
                stats = profile["groups"]["stock"]["links"].get(f"{obj['class']} -> {expected}")
                if stats and stats["n"] and distance(cell, parent["cell"]) > stats["max"]:
                    errors.append(f"{obj['id']}: {expected} outside measured range")
        elif obj.get("parent_cell") is not None or obj.get("parent_id_raw", -1) != -1:
            errors.append(f"{obj['id']}: dangling packed parent cell")
    for obj in objects:
        if obj["class"] == "turret_control" and not batteries[obj["id"]]:
            errors.append(f"{obj['id']}: control has no turret battery")
        if obj["class"] != "gate":
            continue
        r, c = obj["cell"]
        if not (0 <= r < rows and 0 <= c < cols):
            continue
        along = ((r,c-1),(r,c+1)) if obj.get("wall_axis") == "horizontal" else ((r-1,c),(r+1,c))
        across = ((r-1,c),(r+1,c)) if obj.get("wall_axis") == "horizontal" else ((r,c-1),(r,c+1))
        if any(by_cell.get(cell, {}).get("class") != "wall" for cell in along):
            errors.append(f"{obj['id']}: gate is not between two wall segments")
        if any(not (0 <= rr < rows and 0 <= cc < cols and roads[rr,cc]) for rr, cc in across) or not roads[r,c]:
            errors.append(f"{obj['id']}: gate is not on a road crossing")
    compound_floor = np.zeros(shape, bool)
    base_mask = np.zeros(shape, bool)
    bases = layout.get("bases", [])
    start = layout.get("player_start")
    if start is None or not (0 <= start[0] < rows and 0 <= start[1] < cols and roads[tuple(start)]):
        errors.append("player start is missing or off the road network")
    for base in bases:
        r0,c0,r1,c1 = base["bounds"]
        if not (0 <= r0 < r1 <= rows and 0 <= c0 < c1 <= cols):
            errors.append(f"{base['id']}: parcel outside map")
            continue
        if base_mask[r0:r1,c0:c1].any():
            errors.append(f"{base['id']}: overlaps another parcel")
        base_mask[r0:r1,c0:c1] = True
        if not allowed[r0:r1,c0:c1].all():
            errors.append(f"{base['id']}: parcel crosses water or slopes")
        if not roads[tuple(base["entry_cell"])] or not roads[tuple(base["road_anchor"])]:
            errors.append(f"{base['id']}: access road is missing")
        if base["team"] > 0 and start and distance(((r0+r1)//2,(c0+c1)//2), start) < min(rows,cols)*.25:
            errors.append(f"{base['id']}: enemy site is too close to player start")
        members = by_base[base["id"]]
        for obj in members:
            r,c = obj["cell"]
            if not (r0 <= r < r1 and c0 <= c < c1):
                errors.append(f"{obj['id']}: outside its parcel")
        if base["walled"]:
            floor = np.s_[r0+2:r1-2,c0+2:c1-2]
            compound_floor[floor] = True
            if not cement[floor].all():
                errors.append(f"{base['id']}: cement floor has holes")
            perimeter = {(r,c) for r in range(r0,r1) for c in range(c0,c1)
                         if r in (r0,r1-1) or c in (c0,c1-1)}
            actual = {tuple(o["cell"]) for o in members if o["class"] in ("wall", "gate")}
            if actual != perimeter:
                errors.append(f"{base['id']}: wall and gate ring is incomplete")
            if any(roads[cell] and by_cell.get(cell, {}).get("class") != "gate" for cell in perimeter):
                errors.append(f"{base['id']}: road crosses wall outside a gate")
        if profile:
            envelope = profile["pooled"]["archetypes"][base["archetype"]]
            structures = [o for o in members if o["class"] not in ("wall", "tree")]
            for field, value in (("width", c1-c0), ("height", r1-r0), ("objects", len(structures))):
                measured = envelope[field]
                if measured["n"] and not measured["min"] <= value <= measured["max"]:
                    errors.append(f"{base['id']}: {field}={value} outside corpus range")
    if np.any(cement != compound_floor):
        errors.append("cement extends outside compound floors or has missing cells")
    if not any(b["walled"] for b in bases) or len(bases) < 2:
        errors.append("missing compound or second settlement")
    road_distance, base_distance = distance_field(roads), distance_field(base_mask)
    for obj in objects:
        if obj["class"] == "tree":
            cell = tuple(obj["cell"])
            if not (0 <= cell[0] < rows and 0 <= cell[1] < cols):
                continue
            if road_distance[cell] < 4 or base_distance[cell] < 4:
                errors.append(f"{obj['id']}: tree too close to a road or base")
    for part in layout.get("parts", []):
        r,c = part["cell"]
        if not (0 <= r < rows and 0 <= c < cols and allowed[r,c]):
            errors.append(f"{part['id']}: unit start on water, slope or outside map")
    if profile:
        stock = profile["groups"]["stock"]
        for size in batteries.values():
            envelope = stock["metrics"]["battery_sizes"]
            if not envelope["min"] <= size <= envelope["max"]:
                errors.append(f"battery size {size} outside stock range")
        if min(rows,cols) >= stock["map_size"]["min"]:
            metrics = profile["pooled"]["metrics"]
            density = sum(b["team"] > 0 for b in bases)*100000/(rows*cols)
            coverage = float(roads.mean()*100)
            for field, value in (("enemy_bases_per_100k_cells", density), ("roads_coverage_percent", coverage)):
                envelope = metrics[field]
                if not envelope["min"] <= value <= envelope["max"]:
                    errors.append(f"{field}={value:.3f} outside corpus range")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mission", type=Path)
    parser.add_argument("--profile", type=Path, default=Path(__file__).with_name("profile.json"))
    args = parser.parse_args(argv)
    errors = validate(load_json(args.mission), load_json(args.profile))
    for error in errors:
        print(error)
    print(f"{'FAIL' if errors else 'PASS'}: {len(errors)} plausibility errors")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
