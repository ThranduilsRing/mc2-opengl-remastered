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
    precise = layout.get("generation", {}).get("placement_version") == 2
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
        if not precise and any(by_cell.get(cell, {}).get("class") != "wall" for cell in along):
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
            if not precise and actual != perimeter:
                errors.append(f"{base['id']}: wall and gate ring is incomplete")
            if not precise and any(roads[cell] and by_cell.get(cell, {}).get("class") != "gate" for cell in perimeter):
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
    if precise:
        errors.extend(validate_footprints(layout, profile))
    return errors


def validate_footprints(layout, profile=None):
    """Native geometry, continuous pivots, ring coverage and world link distances."""
    import math
    from tools.mapgen.common import CELL_WU, building_role
    from tools.mapgen.footprints import table
    from tools.mapgen.placement import Spatial, bounds, cell_of, covered_cells, overlaps
    from tools.mapgen.raster import polygon
    errors = []
    native = table()["types"]
    by_id = {o["id"]:o for o in layout["objects"]}
    shape = (layout["size"]["rows"],layout["size"]["cols"])
    allowed = ~mask_of(layout,"water") & (slope_grid(layout["terrain"]["elevation_wu"]) < FLAT_DEG)
    roads = mask_of(layout,"roads")
    spatial, polys = Spatial(), {}
    edges = layout.get("wall_edges_wu",[])
    for obj in by_id.values():
        pos,fp = obj.get("position_wu"),obj.get("footprint")
        if not pos or len(pos)!=3 or not all(math.isfinite(v) for v in pos) or not fp or not math.isfinite(obj.get("rotation",0)):
            errors.append(f"{obj['id']}: missing/invalid native footprint or position")
            continue
        asset = native.get(obj.get("footprint_key"))
        if not asset or asset["type_id"] != obj.get("type_id") or any(fp.get(k)!=asset[k] for k in
                ("width_wu","depth_wu","height_wu","bounds_wu","outline_wu")):
            errors.append(f"{obj['id']}: footprint/type differs from native model")
        if cell_of(layout,pos) != obj["cell"]:
            errors.append(f"{obj['id']}: world position and cell disagree")
            if obj["class"]=="gate":
                errors.append(f"{obj['id']}: gate is not between native wall segments; ring is incomplete")
        poly = polygon(obj,fp,pos,rectangle=True)
        polys[obj["id"]] = poly
        r0,c0,r1,c1 = covered_cells(layout,poly)
        if min(r0,c0)<0 or r1>shape[0] or c1>shape[1] or not allowed[r0:r1,c0:c1].all():
            errors.append(f"{obj['id']}: footprint crosses water, slope or map edge")
        if obj["class"] not in ("gate","wall") and roads[max(0,r0):r1,max(0,c0):c1].any():
            errors.append(f"{obj['id']}: footprint blocks a road")
        for other,op in spatial.candidates(poly):
            if not overlaps(poly,op,clearance=3.99):
                continue
            both_wall = obj["class"]==other["class"]=="wall" and obj.get("base_id")==other.get("base_id")
            wall_gate = {obj["class"],other["class"]} <= {"wall","gate"} and obj.get("base_id")==other.get("base_id")
            if both_wall or wall_gate:
                # Connected wall silhouettes intentionally share end caps. Bound
                # their overlap to half the shorter native piece, including joins.
                a,b = bounds(poly),bounds(op)
                overlap_area = max(0,min(a[2],b[2])-max(a[0],b[0]))*max(0,min(a[3],b[3])-max(a[1],b[1]))
                thickness = max(min(fp["width_wu"],fp["depth_wu"]),min(other["footprint"]["width_wu"],other["footprint"]["depth_wu"]))
                length_cap = min(max(fp["width_wu"],fp["depth_wu"]),
                                 max(other["footprint"]["width_wu"],other["footprint"]["depth_wu"]))/2
                if overlap_area <= thickness*max(thickness,length_cap)+1:
                    continue
            errors.append(f"{obj['id']}: footprint overlaps/violates clearance with {other['id']}")
        spatial.add(obj,poly)
        parent = by_id.get(obj.get("parent_link"))
        if profile and parent and obj["class"] in ("turret","turret_control","gate"):
            stats = profile["groups"]["stock"]["links"].get(f"{obj['class']} -> {parent['class']}")
            d = math.dist(pos[:2],parent["position_wu"][:2])/CELL_WU
            if stats and not stats["min"] <= d <= stats["max"]:
                errors.append(f"{obj['id']}: world parent distance outside measured range")
    for base in layout["bases"]:
        if base["walled"] and sum(e["base_id"]==base["id"] for e in edges)!=4:
            errors.append(f"{base['id']}: native wall ring is incomplete")
        if base["walled"]:
            r0,c0,r1,c1 = base["bounds"]
            x0,x1 = (c0+.5-shape[1]/2)*CELL_WU,(c1-.5-shape[1]/2)*CELL_WU
            y0,y1 = (shape[0]/2-r1+.5)*CELL_WU,(shape[0]/2-r0-.5)*CELL_WU
            expected = {((x0,y1),(x1,y1)),((x1,y1),(x1,y0)),((x1,y0),(x0,y0)),((x0,y0),(x0,y1))}
            actual = {(tuple(e["start"]),tuple(e["end"])) for e in edges if e["base_id"]==base["id"]}
            if expected != actual:
                errors.append(f"{base['id']}: native wall ring differs from parcel")
        members = [o for o in by_id.values() if o.get("base_id")==base["id"]]
        if profile:
            for obj in members:
                role = building_role(obj) if obj["class"]=="building" else obj["class"]
                stats = profile["groups"]["stock"]["adjacency"].get(f"{role} -> {role}",{})
                peers = [o for o in members if o is not obj and
                         (building_role(o) if o["class"]=="building" else o["class"])==role]
                if peers and stats.get("n",0)>=20:
                    nearest = min(math.dist(obj["position_wu"][:2],o["position_wu"][:2]) for o in peers)/CELL_WU
                    if not stats["min"] <= nearest <= stats["max"]:
                        errors.append(f"{obj['id']}: {role} neighbour spacing outside corpus range")
    for edge in edges:
        a,b = edge["start"],edge["end"]
        length = math.dist(a,b)
        if length <= 0:
            errors.append("zero length wall edge")
            continue
        ux,uy = (b[0]-a[0])/length,(b[1]-a[1])/length
        intervals = []
        ids = edge["wall_ids"]+([edge["gate_id"]] if edge.get("gate_id") else [])
        for ident in ids:
            obj,poly = by_id.get(ident),polys.get(ident)
            if not obj or not poly:
                continue
            along = [(x-a[0])*ux+(y-a[1])*uy for x,y in poly]
            across = [(x-a[0])*uy-(y-a[1])*ux for x,y in poly]
            if min(across)>1e-3 or max(across)<-1e-3:
                errors.append(f"{ident}: native wall moved off its edge")
                continue
            intervals.append((min(along),max(along)))
        end = 0
        for lo,hi in sorted(intervals):
            if lo > end+.01:
                errors.append(f"{edge['base_id']}: native wall ring is incomplete (gap {lo-end:.2f} wu)")
            end = max(end,hi)
        if end < length-.01:
            errors.append(f"{edge['base_id']}: native wall ring is incomplete")
    if not layout.get("roads_wu"):
        errors.append("missing width polylines")
    for road in layout.get("roads_wu",[]):
        if not math.isfinite(road["width_wu"]) or road["width_wu"] <= 0 or len(road["points"])<2:
            errors.append("invalid width polyline")
            continue
        for a,b in zip(road["points"],road["points"][1:]):
            diagonal = a[0]!=b[0] and a[1]!=b[1]
            if not all(math.isfinite(v) for v in (*a,*b)) or (diagonal and not layout.get('macro')):
                errors.append("road polyline has a nonfinite or unrouted diagonal segment")
                continue
            if diagonal:
                # Regional A* permits diagonals. Check the swept strip in short
                # sections, rather than its potentially enormous global AABB.
                length = math.dist(a,b)
                half = road['width_wu']/2
                dx,dy = -(b[1]-a[1])*half/length,(b[0]-a[0])*half/length
                samples = np.linspace(a,b,max(2,math.ceil(length/(CELL_WU/2))+1))
                for p,q in zip(samples,samples[1:]):
                    poly = [(p[0]+dx,p[1]+dy),(q[0]+dx,q[1]+dy),
                            (q[0]-dx,q[1]-dy),(p[0]-dx,p[1]-dy)]
                    r0,c0,r1,c1 = covered_cells(layout,poly)
                    r0,c0,r1,c1 = max(0,r0),max(0,c0),min(shape[0],r1),min(shape[1],c1)
                    if r1<=r0 or c1<=c0 or not allowed[r0:r1,c0:c1].all():
                        errors.append('road width crosses water or slopes')
                        break
                for p in samples:
                    r,c = cell_of(layout,p)
                    if not (0<=r<shape[0] and 0<=c<shape[1] and roads[r,c]):
                        errors.append('road polyline has an unrouted diagonal segment')
                        break
                continue
            # Sweep width over the terrain, including the half width beside the
            # centreline. Map edges are intentional road exits, so clip there.
            half = road["width_wu"]/2
            dx,dy = (half,0) if a[0]==b[0] else (0,half)
            poly = [(a[0]+dx,a[1]+dy),(b[0]+dx,b[1]+dy),(b[0]-dx,b[1]-dy),(a[0]-dx,a[1]-dy)]
            r0,c0,r1,c1 = covered_cells(layout,poly)
            r0,c0,r1,c1 = max(0,r0),max(0,c0),min(shape[0],r1),min(shape[1],c1)
            if r1<=r0 or c1<=c0 or not allowed[r0:r1,c0:c1].all():
                errors.append("road width crosses water or slopes")
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
