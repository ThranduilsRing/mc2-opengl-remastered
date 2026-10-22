"""Seeded world-unit footprint layouts calibrated from the measured corpus."""
from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import math
import random
import sys
from pathlib import Path

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.ascii import render
from tools.mapgen.common import (CELL_WU, FLAT_DEG, LAYERS, SCHEMA, STEEP_DEG,
                                components, distance, distance_field, load_json,
                                mask_of, output_path, packed_cell, slope_grid, spans, write_json)
from tools.mapgen.validate import validate

PROFILE_PATH = Path(__file__).with_name("profile.json")
MAX_CELLS = 3072  # 1024 editor vertices, three movement cells per vertex.


def synthetic_terrain(size, seed):
    """Gentle, mostly flat country with a lake and one impassable escarpment."""
    rng = random.Random(seed ^ 0x54455252)
    rr, cc = np.mgrid[:size, :size]
    height = np.full((size, size), 160.0)
    for _ in range(5):
        r, c = rng.uniform(0, size), rng.uniform(0, size)
        radius = rng.uniform(.08, .18)*size
        height += rng.uniform(20, 80)*np.exp(-((rr-r)**2+(cc-c)**2)/(2*radius**2))
    lake_r, lake_c = size*.24, size*.28
    wet = ((rr-lake_r)/(size*.065))**2 + ((cc-lake_c)/(size*.09))**2 < 1
    if size >= 1024:
        # Irregular shores and broad drainage relief only affect the large-map path.
        angle = np.arctan2((rr-lake_r)/.065,(cc-lake_c)/.09)
        radius = ((rr-lake_r)/(size*.065))**2+((cc-lake_c)/(size*.09))**2
        wet = radius < (1+.18*np.sin(angle*3+seed)+.10*np.sin(angle*7))**2
        height += 90*np.sin(rr/size*math.pi*2+.6)*np.sin(cc/size*math.pi*3)**2
    height[wet] = 120
    height[(cc > size*.91) & (rr < size*.4)] += 450
    return np.round(height, 3), wet


def read_heightfield(path, size, water_level=None):
    """Accept .npy heights in wu, JSON matrix, or an extracted layout JSON."""
    if Path(path).suffix.lower() == ".npy":
        heights = np.load(path, allow_pickle=False)
        water = np.zeros(np.shape(heights), bool)
    else:
        data = load_json(path)
        if isinstance(data, dict) and data.get("schema") == SCHEMA:
            step = data["terrain"]["cell_step"]
            heights = np.repeat(np.repeat(np.asarray(data["terrain"]["elevation_wu"]), step, 0), step, 1)
            water = mask_of(data, "water")
        elif isinstance(data, dict):
            heights = np.asarray(data["elevation_wu"], dtype=float)
            water = np.asarray(data.get("water", np.zeros(heights.shape, bool)), dtype=bool)
        else:
            heights = np.asarray(data, dtype=float)
            water = np.zeros(heights.shape, bool)
    heights = np.asarray(heights, dtype=float)
    if heights.shape != (size, size) or water.shape != heights.shape or not np.isfinite(heights).all():
        raise ValueError(f"heightfield and water must be finite {size}x{size} grids")
    if water_level is not None:
        water = water | (heights <= water_level)
    return np.round(heights, 3), water


def route(start, goal, allowed, costs=None):
    """Deterministic four-neighbour A* with stable tie breaks."""
    start, goal = tuple(map(int, start)), tuple(map(int, goal))
    if not allowed[start] or not allowed[goal]:
        raise ValueError("road endpoint is not dry flat ground")
    frontier = [(abs(start[0]-goal[0])+abs(start[1]-goal[1]), 0.0, start)]
    best, parent = {start: 0.0}, {}
    rows, cols = allowed.shape
    while frontier:
        _, cost, cell = heapq.heappop(frontier)
        if cost != best.get(cell):
            continue
        if cell == goal:
            path = [cell]
            while cell != start:
                cell = parent[cell]
                path.append(cell)
            return path[::-1]
        r, c = cell
        for nr, nc in ((r-1, c), (r, c-1), (r, c+1), (r+1, c)):
            if not (0 <= nr < rows and 0 <= nc < cols and allowed[nr, nc]):
                continue
            new = cost + 1 + (float(costs[nr, nc]) if costs is not None else 0)
            nxt = (nr, nc)
            if new < best.get(nxt, math.inf):
                best[nxt], parent[nxt] = new, cell
                estimate = new + abs(nr-goal[0]) + abs(nc-goal[1])
                heapq.heappush(frontier, (estimate, new, nxt))
    raise ValueError("no flat dry road route between endpoints")


def sample(rng, stats, low, high):
    """Triangular sampling inside the observed p10..p90, with practical parcel caps."""
    if stats.get("n", 0):
        low = max(low, min(high, stats["p10"]))
        high = min(high, max(low, stats["p90"]))
        mode = min(high, max(low, stats["median"]))
    else:
        mode = (low+high)/2
    return int(round(rng.triangular(low, high, mode))) if high > low else int(low)


def nearest(mask, target):
    cells = np.argwhere(mask)
    if not len(cells):
        raise ValueError("terrain has no suitable cell")
    position = np.square(cells-np.asarray(target)).sum(axis=1).argmin()
    return tuple(map(int, cells[position]))


def plan_cells(size=300, seed=42, heightfield=None, water=None, profile=None, style="stock"):
    if not 96 <= size <= MAX_CELLS:
        raise ValueError(f"size must be between 96 and {MAX_CELLS} cells")
    profile = profile or load_json(PROFILE_PATH)
    stats = profile["pooled"] if style == "mixed" else profile["groups"][style]
    rng = random.Random(seed)
    if heightfield is None:
        heights, wet = synthetic_terrain(size, seed)
    else:
        heights = np.round(np.array(heightfield, dtype=float, copy=True), 3)
        wet = np.zeros_like(heights, bool) if water is None else np.array(water, dtype=bool, copy=True)
    if heights.shape != (size, size) or wet.shape != heights.shape or not np.isfinite(heights).all():
        raise ValueError(f"terrain must be a finite {size}x{size} grid")
    slopes = slope_grid(heights)
    flat = (slopes < FLAT_DEG) & ~wet
    land_groups = components(flat)
    if not land_groups or len(land_groups[0]) < 800:
        raise ValueError("insufficient connected dry flat land for roads and bases")
    mainland = np.zeros_like(flat)
    rr, cc = np.array(land_groups[0]).T
    mainland[rr, cc] = True
    del land_groups
    # The road skeleton exists before any parcel, wall, building or tree placement.
    hub = nearest(mainland, (size*.52, size*.5))
    endpoints = []
    for axis, edge, target in ((1, 0, size*.65), (1, size-1, size*.55),
                               (0, 0, size*.65), (0, size-1, size*.3)):
        line = mainland[:, edge] if axis == 1 else mainland[edge, :]
        candidates = np.flatnonzero(line)
        if len(candidates):
            value = int(candidates[np.abs(candidates-target).argmin()])
            endpoints.append((value, edge) if axis == 1 else (edge, value))
    if len(set(endpoints)) < 2:
        raise ValueError("dry flat road network must reach at least two map edges")
    roads = np.zeros_like(flat)
    road_paths = []
    road_radius = 0 if size < 180 else 1

    def paint_road(cell):
        r, c = cell
        area = np.s_[max(0,r-road_radius):min(size,r+road_radius+1),
                     max(0,c-road_radius):min(size,c+road_radius+1)]
        roads[area] |= mainland[area]

    slope_cost = slopes / 25
    macro, site_specs, forest_field = None, None, None
    if size >= 1024:
        from tools.mapgen.macro import plan
        density = stats["metrics"]["enemy_bases_per_100k_cells"]["median"] or 4
        count = max(28,min(220,round(density*size*size/100000*(1024/size)**.75)))
        macro, site_specs, paths, forest_field = plan(mainland,heights,slopes,wet,seed,count)
        for path,width in paths:
            road_paths.append((path,width))
            for cell in path:
                paint_road(cell)
        hub = tuple(site_specs[0]['anchor'])
    for endpoint in endpoints:
        if macro:
            hub = min((tuple(s['anchor']) for s in site_specs[:macro['major_count']]),
                      key=lambda p:distance(p,endpoint))
        path = route(endpoint, hub, mainland, slope_cost)
        road_paths.append((path, 64.0))
        for cell in path:
            paint_road(cell)
    # A secondary dogleg provides settlement frontage away from the main junction.
    for target in (() if macro else ((size*.3, size*.7), (size*.72, size*.72))):
        node = nearest(mainland, target)
        path = route(hub, node, mainland, slope_cost)
        road_paths.append((path, 64.0))
        for cell in path:
            paint_road(cell)
    start = nearest(roads, (size*.79, size*.19))
    layers = {name: np.zeros_like(flat) for name in LAYERS}
    layers["roads"], layers["water"], layers["cliffs"] = roads, wet, slopes >= STEEP_DEG
    occupied, reserved = np.zeros_like(flat), np.zeros_like(flat)
    objects, bases, objectives = [], [], []

    def add(cls, cell, team, base_id=None, role=None, parent=None, rotation=0, **extra):
        r, c = map(int, cell)
        if occupied[r, c] or not mainland[r, c] or (roads[r, c] and cls != "gate"):
            raise ValueError(f"illegal {cls} placement at {(r, c)}")
        obj = {"id": f"object_{len(objects)}", "class": cls, "type_id": None,
               "type_name": f"abstract_{role or cls}", "cell": [r, c], "rotation": rotation,
               "team": team, "damage": 0, "parent_link": parent["id"] if parent else None,
               "parent_cell": parent["cell"] if parent else None,
               "parent_id_raw": packed_cell(parent["cell"]) if parent else -1, **extra}
        if base_id:
            obj["base_id"] = base_id
        if role:
            obj["role"] = role
        objects.append(obj)
        occupied[r, c] = True
        return obj

    density = stats["metrics"]["enemy_bases_per_100k_cells"]["median"] or 4
    target_bases = len(site_specs) if macro else max(3, min(512, round(density*size*size/100000)))
    archetypes = sorted(stats["archetypes"])
    weights = [stats["archetypes"][name]["count"] for name in archetypes]
    road_positions = [tuple(map(int, v)) for v in np.argwhere(roads)]
    # Explicit examples of the three visible site forms; remaining sites follow
    # the measured archetype weights. The validator checks corpus envelopes.
    forms = ["walled_compound", "outpost", "town"]
    for index in range(target_bases):
        spec = site_specs[index] if macro else None
        kind = spec['kind'] if spec else forms[index] if index < len(forms) else rng.choices(archetypes, weights=weights)[0]
        frontage = road_positions
        if spec:
            frontage = [p for p in road_positions if distance(p,spec['anchor'])<max(spec['width'],spec['height'])*(1.8 if index<macro['major_count'] else .9)]
        distribution = stats["archetypes"][kind]
        if not distribution["count"]:
            distribution = profile["pooled"]["archetypes"][kind]
        placed = None
        for attempt in range(500):
            min_side = (18 if size < 180 else 22) if kind == "walled_compound" else 16 if kind == "turret_battery" else 13
            max_side = min(42, size//3, min_side+4 if attempt >= 150 else 42)
            width = sample(rng, distribution["width"], min_side, max_side)
            height = sample(rng, distribution["height"], min_side, max_side)
            if spec:
                width = max(min_side if kind=='walled_compound' else 10,round(spec['width']*rng.uniform(.85,1.1)))
                height = max(min_side if kind=='walled_compound' else 10,round(spec['height']*rng.uniform(.85,1.1)))
                envelope = profile['pooled']['archetypes'][kind]
                width = min(width,int(envelope['width']['max']))
                height = min(height,int(envelope['height']['max']))
            road_r, road_c = rng.choice(frontage)
            side = rng.choice(("N", "S", "W", "E"))
            gap = rng.randint(3, 6)
            # side names identify the parcel edge facing its access road.
            if side == "N":
                r0, c0 = road_r+gap, road_c-width//2
            elif side == "S":
                r0, c0 = road_r-gap-height+1, road_c-width//2
            elif side == "W":
                r0, c0 = road_r-height//2, road_c+gap
            else:
                r0, c0 = road_r-height//2, road_c-gap-width+1
            r1, c1 = r0+height, c0+width
            if r0 < 4 or c0 < 4 or r1 > size-4 or c1 > size-4:
                continue
            rectangle = np.s_[r0:r1, c0:c1]
            if not mainland[rectangle].all() or reserved[max(0,r0-3):r1+3, max(0,c0-3):c1+3].any() or roads[rectangle].any():
                continue
            centre = ((r0+r1)//2, (c0+c1)//2)
            if distance(centre, start) < size*.25:
                continue
            gate = {"N": (r0, road_c), "S": (r1-1, road_c),
                    "W": (road_r, c0), "E": (road_r, c1-1)}[side]
            # The spur crosses one wall cell and reaches the interior centre.
            a, b = (road_r, road_c), centre
            if side in ("N", "S"):
                spur = [(r, road_c) for r in range(min(road_r,b[0]), max(road_r,b[0])+1)]
            else:
                spur = [(road_r, c) for c in range(min(road_c,b[1]), max(road_c,b[1])+1)]
            if any(not mainland[cell] or occupied[cell] for cell in spur):
                continue
            placed = (r0, c0, r1, c1, side, gate, spur)
            break
        if placed is None:
            continue
        r0, c0, r1, c1, side, gate, spur = placed
        base_id = f"base_{len(bases)}"
        team = -1 if kind == "town" else 1
        reserved[r0:r1, c0:c1] = True
        for cell in spur:
            roads[cell] = True
        road_paths.append((spur, CELL_WU))
        walled = kind == "walled_compound"
        if walled:
            layers["cement"][r0+2:r1-2, c0+2:c1-2] = True
            for r in range(r0, r1):
                for c in range(c0, c1):
                    if (r in (r0, r1-1) or c in (c0, c1-1)) and (r, c) != gate:
                        corner = r in (r0, r1-1) and c in (c0, c1-1)
                        rotation = {(r0,c0): 0, (r0,c1-1): 90, (r1-1,c1-1): 180, (r1-1,c0): 270}.get((r,c),
                            90 if c in (c0,c1-1) else 0)
                        add("wall", (r, c), team, base_id, rotation=rotation,
                            segment="corner" if corner else "straight")
        has_runway = kind == "airfield_hq" and max(r1-r0, c1-c0) >= 24
        if has_runway:
            if c1-c0 >= r1-r0:
                strip = np.s_[(r0+r1)//2, c0+2:c1-2]
            else:
                strip = np.s_[r0+2:r1-2, (c0+c1)//2]
            layers["runway"][strip] = True
            roads[strip] = True
            runway_mask = np.zeros_like(flat)
            runway_mask[strip] = True
            runway_cells = [tuple(map(int,v)) for v in np.argwhere(runway_mask)]
            road_paths.append((runway_cells, CELL_WU))
        inset = 4 if walled else 2
        slots = [(r, c) for r in range(r0+inset, r1-inset, 2)
                 for c in range(c0+inset, c1-inset, 2) if not roads[r, c]]
        def choose(target, clearance=1):
            candidates = [cell for cell in slots if not occupied[cell]
                          and not occupied[max(0,cell[0]-clearance):cell[0]+clearance+1,
                                           max(0,cell[1]-clearance):cell[1]+clearance+1].any()]
            return min(candidates, key=lambda cell: (distance(cell, target), cell)) if candidates else None

        begin = len(objects)
        if walled:
            control_cell = choose((gate[0]+(4 if side=="N" else -4 if side=="S" else 0),
                                   gate[1]+(4 if side=="W" else -4 if side=="E" else 3)))
            gate_control = add("gate_control", control_cell, team, base_id)
            add("gate", gate, team, base_id, parent=gate_control,
                rotation=0 if side in ("N", "S") else 90, wall_axis="horizontal" if side in ("N", "S") else "vertical")
        defended = kind in ("walled_compound", "turret_battery") or (kind in ("outpost", "airfield_hq") and rng.random() < .5)
        if spec and spec['population']<12:
            defended = False
        if defended:
            anchor = ((r0+r1)//2-3, (c0+c1)//2-3)
            generator_cell = choose(anchor)
            if generator_cell:
                generator = add("turret_generator", generator_cell, team, base_id)
                control_cell = choose((generator_cell[0]+3, generator_cell[1]))
                control = add("turret_control", control_cell, team, base_id, parent=generator)
                battery_stats = stats["metrics"]["battery_sizes"]
                count = sample(rng, battery_stats, 3, 5)
                targets = [(r0+inset, c0+inset), (r0+inset, c1-inset-1),
                           (r1-inset-1, c1-inset-1), (r1-inset-1, c0+inset),
                           (r0+inset, (c0+c1)//2)]
                for target in targets[:count]:
                    cell = choose(target)
                    if cell is not None:
                        add("turret", cell, team, base_id, parent=control)
        current = len(objects)-begin
        target_count = 0 if kind == "turret_battery" else max(3, sample(rng, distribution["objects"], 4, 28)-current)
        if spec:
            target_count = max(1,spec['population']-current)
        mix = distribution["mix"]
        role_names = ["military", "industrial", "town", "airfield", "hq"]
        role_weights = [mix.get(name, 0) for name in role_names]
        if not sum(role_weights):
            role_weights = [1, 0, 0, 0, 0]
        for number in range(target_count):
            target = (rng.randrange(r0+inset, r1-inset), rng.randrange(c0+inset, c1-inset))
            cell = choose(target)
            if cell is None:
                break
            role = "town" if kind == "town" else rng.choices(role_names, weights=role_weights)[0]
            if spec and spec['region']=='farmland':
                role = rng.choices(('town','industrial'),weights=(8,1))[0]
            elif spec and spec['region']=='military':
                role = rng.choices(('military','hq','industrial'),weights=(8,1,2))[0]
            if kind == "industrial" and number < 3:
                role = "industrial"
            if kind == "airfield_hq" and number == 0:
                role = "airfield" if has_runway else "hq"
            add("building", cell, team, base_id, role=role, rotation=90 if side in ("E", "W") else 0)
        bases.append({"id": base_id, "archetype": kind, "team": team,
                      "bounds": [r0,c0,r1,c1], "entry_cell": list(gate),
                      "road_anchor": [road_r,road_c], "walled": walled})
        if spec:
            bases[-1].update(region=spec['region'],rank=spec['rank'],requested_population=spec['population'],
                             macro_node=index)
        if team == 1:
            target = next((o["cell"] for o in objects[begin:] if o["class"] == "building"), list(centre))
            objectives.append({"id": f"objective_{len(objectives)}", "title": f"Capture or clear {kind} {base_id}",
                               "cell": target, "display": True, "hidden": False, "conditions": []})
    if len(bases) < 2 or not any(b["walled"] for b in bases):
        raise ValueError("terrain cannot fit a compound and at least one other site; use more flat land or a larger size")
    # Decorations are deliberately last; their rejection cannot perturb bases.
    road_distance = distance_field(roads)
    base_distance = distance_field(reserved)
    tree_allowed = mainland & (base_distance >= 4) & (road_distance >= 4) & ~occupied
    region_grid = None
    if macro:
        region_grid = np.asarray(macro['regions'],dtype=np.uint8)
        forest = (region_grid==0) & (forest_field>.38)
        forest_full = np.repeat(np.repeat(forest,8,axis=0),8,axis=1)
        tree_allowed[:forest_full.shape[0],:forest_full.shape[1]] &= forest_full
    tree_allowed[max(0,start[0]-5):start[0]+6, max(0,start[1]-5):start[1]+6] = False
    tree_density = stats["metrics"]["tree_objects_per_100k_cells"]["median"] or 0
    tree_target = min(180000, round(size*size*tree_density/100000))
    tree_cells = np.argwhere(tree_allowed)
    centre_indices = rng.sample(range(len(tree_cells)), min(len(tree_cells), max(8, size*size//5000)))
    cluster_centres = [tuple(map(int, tree_cells[i])) for i in centre_indices]
    if cluster_centres:
        tree_count = 0
        for _ in range(tree_target*5):
            if tree_count >= tree_target:
                break
            centre = rng.choice(cluster_centres)
            radius = 6
            if macro:
                moisture = forest_field[min(centre[0]//8,len(forest_field)-1),min(centre[1]//8,len(forest_field)-1)]
                radius = 4+36*moisture**2
            r, c = round(rng.gauss(centre[0], radius)), round(rng.gauss(centre[1], radius))
            if 0 <= r < size and 0 <= c < size and tree_allowed[r,c] and not occupied[r,c]:
                add("tree", (r,c), -1)
                tree_count += 1
    parts = [{"id": "player", "cell": list(start), "team": 0, "type_name": "abstract_player_lance",
              "player": True, "active": 1, "exists": 1}]
    for base in bases:
        if base["team"] > 0:
            parts.append({"id": "defenders_"+base["id"], "cell": base["road_anchor"], "team": 1,
                          "type_name": "abstract_defenders", "player": False, "active": 1, "exists": 1})
    terrain_type = np.ones_like(heights, dtype=np.uint8)
    terrain_type[layers["cement"]] = 10
    terrain_type[wet] = 0
    return {"schema": SCHEMA, "name": f"generated_{size}_{seed}",
            "source": {"corpus": "generated", "seed": seed, "style": style,
                       "profile_sha256": hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
                       "heightfield_sha256": hashlib.sha256(heights.astype("<f8").tobytes()+wet.tobytes()).hexdigest()},
            "size": {"rows": size, "cols": size, "cell_wu": CELL_WU,
                     "vertices": size//3 if size%3 == 0 else None},
            "objects": objects, "parts": parts, "player_start": list(start), "objectives": objectives,
            "roads_wu": world_roads(road_paths,mainland,size),
            **({"macro":macro} if macro else {}),
            "bases": bases, "terrain": {"cell_step": 1, "elevation_wu": heights.tolist(),
                "slope_deg": np.round(slopes, 3).tolist(), "terrain_type": terrain_type.tolist(),
                "overlay_type": np.where(roads, 0, -1).tolist(), "water_elevation_wu": None},
            "layers": {name: spans(mask) for name, mask in layers.items()}, "warnings": [],
            "generation": {"requested_sites": target_bases, "placed_sites": len(bases),
                           "flat_limit_deg": FLAT_DEG, "road_edge_cells": [list(c) for c in endpoints],
                           "wall_cement_inset_cells": 2, "model_footprints": "unassigned"}}


def simplify_path(path):
    """Keep bends/endpoints of the exact routed centreline."""
    return [p for i,p in enumerate(path) if i in (0,len(path)-1) or
            (p[0]-path[i-1][0],p[1]-path[i-1][1]) != (path[i+1][0]-p[0],path[i+1][1]-p[1])]


def world_roads(paths,allowed,size):
    result = []
    for path,width in paths:
        # A routed cell guarantees 42 2/3 wu of dry surface. Broader roads need
        # dry neighbours as well; narrow an entire route beside an obstacle.
        near_obstacle = any(not allowed[max(0,r-1):r+2,max(0,c-1):c+2].all() for r,c in path)
        if width <= CELL_WU or near_obstacle:
            width = 40.0
        result.append({"points": [[round((c+.5-size/2)*CELL_WU,4),round((size/2-r-.5)*CELL_WU,4)]
                                  for r,c in simplify_path(path)],"width_wu":width})
    return result


def generate(size=300, seed=42, heightfield=None, water=None, profile=None, style="stock"):
    from tools.mapgen.placement import upgrade
    profile = profile or load_json(PROFILE_PATH)
    return upgrade(plan_cells(size,seed,heightfield,water,profile,style),profile)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=300)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--heightfield", type=Path)
    parser.add_argument("--water-level", type=float)
    parser.add_argument("--profile", type=Path, default=PROFILE_PATH)
    parser.add_argument("--style", default="stock", help="stock, mixed, or a corpus install label")
    parser.add_argument("--cells-per-char", type=int, default=3)
    parser.add_argument("--wu-per-pixel",type=float,help="override the measured Quonset scale")
    parser.add_argument("--hillshade",action="store_true",help="subtle pixel-art hillshade")
    parser.add_argument("--out", type=Path, default=Path("tools/mapgen/corpus/generated"))
    args = parser.parse_args(argv)
    try:
        terrain, wet = read_heightfield(args.heightfield, args.size, args.water_level) if args.heightfield else (None, None)
        if args.water_level is not None and args.heightfield is None:
            terrain, wet = synthetic_terrain(args.size, args.seed)
            wet |= terrain <= args.water_level
        profile = load_json(args.profile)
        layout = generate(args.size, args.seed, terrain, wet, profile, args.style)
        errors = validate(layout, profile)
        if errors:
            raise ValueError("; ".join(errors[:8]))
        out = output_path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        suffix = ".json.gz" if args.size>1024 else ".json"
        write_json(out / (layout["name"]+suffix), layout)
        path = out / (layout["name"]+".txt")
        path.write_text(render(layout, args.cells_per_char), encoding="ascii")
        from tools.mapgen.raster import render as raster, crop_base
        raster(layout,out/(layout["name"]+".png"),args.wu_per_pixel,hillshade=args.hillshade)
        raster(layout,out/(layout["name"]+"_base.png"),args.wu_per_pixel,crop_base(layout),args.hillshade)
        print(f"{path}: {len(layout['bases'])} sites, {len(layout['objects'])} objects; plausibility PASS")
    except (ValueError, KeyError) as error:
        parser.exit(2, f"generation failed: {error}\n")


if __name__ == "__main__":
    main()
