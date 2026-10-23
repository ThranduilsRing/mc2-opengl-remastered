"""Measure authored layout rules and write a reproducible corpus report/profile."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import (CELL_WU, FLAT_DEG, STEEP_DEG, building_role, components,
                                distance, distance_field, load_json, mask_of, output_path,
                                terrain_at, write_json)

STRUCTURAL = {"building", "turret", "turret_control", "turret_generator", "gate", "gate_control"}
ARCHETYPES = ("walled_compound", "outpost", "town", "industrial", "airfield_hq", "turret_battery")
CLUSTER_RADIUS = 12.0


def summary(values):
    values = [v for v in values if v is not None and math.isfinite(v)]
    if not values:
        return {"n": 0, "min": None, "p10": None, "median": None, "p90": None, "max": None}
    return {"n": len(values), **{name: round(float(value), 3) for name, value in zip(
        ("min", "p10", "median", "p90", "max"), np.percentile(values, [0, 10, 50, 90, 100]))}}


def valid_objects(layout):
    rows, cols = layout["size"]["rows"], layout["size"]["cols"]
    return [o for o in layout["objects"] if 0 <= o["cell"][0] < rows and 0 <= o["cell"][1] < cols]


def structural(objects):
    return [o for o in objects if o["class"] in STRUCTURAL
            and not any(s in o["type_name"].lower() for s in ("navmarker", "extractionmarker", "nav1", "nav2", "streetlight"))]


def clusters(objects, radius=CLUSTER_RADIUS):
    """Single-link same-team clusters, bucketed to avoid quadratic global scans."""
    objects = structural(objects)
    buckets = defaultdict(list)
    for i, obj in enumerate(objects):
        r, c = obj["cell"]
        buckets[(obj["team"], int(r//radius), int(c//radius))].append(i)
    visited, result = set(), []
    for i in range(len(objects)):
        if i in visited:
            continue
        visited.add(i)
        stack, group = [i], []
        while stack:
            j = stack.pop()
            obj = objects[j]
            group.append(obj)
            r, c = obj["cell"]
            br, bc = int(r//radius), int(c//radius)
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    for k in buckets.get((obj["team"], br+dr, bc+dc), []):
                        if k not in visited and distance(obj["cell"], objects[k]["cell"]) <= radius:
                            visited.add(k)
                            stack.append(k)
        if len(group) >= 3 or any(o["class"] == "turret" for o in group):
            result.append(group)
    return result


def archetype(group, walls):
    classes = Counter(o["class"] for o in group)
    roles = Counter(building_role(o) for o in group if o["class"] == "building")
    buildings = sum(roles.values())
    if len(walls) >= 6:
        return "walled_compound"
    if classes["turret"] >= 2 and buildings <= 2:
        return "turret_battery"
    if roles["airfield"] or roles["hq"] >= max(1, buildings*.25):
        return "airfield_hq"
    if roles["industrial"] >= max(2, buildings*.35):
        return "industrial"
    if roles["town"] >= max(2, buildings*.4):
        return "town"
    return "outpost"


def road_shape(layout):
    # Work at source terrain stride; thick cell expansion would inflate junctions.
    step = layout["terrain"]["cell_step"]
    mask = mask_of(layout, "roads")[::step, ::step]
    groups = components(mask, diagonal=True)
    degrees = np.zeros_like(mask, dtype=np.uint8)
    degrees[1:] += mask[:-1]
    degrees[:-1] += mask[1:]
    degrees[:, 1:] += mask[:, :-1]
    degrees[:, :-1] += mask[:, 1:]
    edge_groups = sum(any(r in (0, mask.shape[0]-1) or c in (0, mask.shape[1]-1) for r, c in group) for group in groups)
    return {"components": len(groups), "largest_fraction": round(len(groups[0])/int(mask.sum()), 4) if groups else 0,
            "edge_components": edge_groups, "endpoints": int(((degrees == 1) & mask).sum()),
            "junction_samples": int(((degrees >= 3) & mask).sum()),
            "coverage_percent": round(float(mask.mean()*100), 3)}


def measure(layout):
    objects = valid_objects(layout)
    walls = [o for o in objects if o["class"] == "wall"]
    structs = structural(objects)
    masks = {key: mask_of(layout, key) for key in ("roads", "cement", "water", "cliffs")}
    # Calculate feature proximity on the coarse source grid, with explicit units.
    step = layout["terrain"]["cell_step"]
    distances = {key: distance_field(value[::step, ::step])*step if value.any() else None
                 for key, value in masks.items()}

    def near(name, cell):
        field = distances[name]
        return float(field[cell[0]//step, cell[1]//step]) if field is not None else None

    groups = clusters(objects)
    records = []
    wall_positions = np.array([o["cell"] for o in walls], dtype=float).reshape(-1, 2)
    for group in groups:
        cells = np.array([o["cell"] for o in group], dtype=float)
        # Associate walls within 8 cells of any structure of the same team.
        if len(walls):
            ds = np.sqrt(((wall_positions[:, None, :] - cells[None, :, :])**2).sum(axis=2)).min(axis=1)
            attached = [o for o, d in zip(walls, ds) if d <= 8 and o["team"] == group[0]["team"]]
        else:
            attached = []
        all_cells = np.array([o["cell"] for o in group+attached])
        low, high = all_cells.min(axis=0), all_cells.max(axis=0)
        centre = cells.mean(axis=0).tolist()
        mix = Counter(o["class"] if o["class"] != "building" else building_role(o) for o in group)
        nn = []
        for i, obj in enumerate(group):
            if len(group) > 1:
                nn.append(min(distance(obj["cell"], other["cell"]) for j, other in enumerate(group) if i != j))
        records.append({"archetype": archetype(group, attached), "team": group[0]["team"],
            "width": int(high[1]-low[1]+1), "height": int(high[0]-low[0]+1), "objects": len(group),
            "walls": len(attached), "turrets": sum(o["class"] == "turret" for o in group),
            "generators": sum(o["class"] == "turret_generator" for o in group), "mix": dict(mix),
            "centre": centre, "road_distance": min((near("roads", o["cell"]) for o in group), default=None)
                if distances["roads"] is not None else None,
            "slope_deg": float(np.mean([terrain_at(layout, "slope_deg", o["cell"]) for o in group])),
            "nearest_neighbour": nn,
            "player_distance": distance(centre, layout["player_start"]) if layout.get("player_start") else None})
    slopes = [terrain_at(layout, "slope_deg", o["cell"]) for o in structs]
    # Same-class nearest neighbours describe local building grouping directly.
    adjacency = defaultdict(list)
    for group in groups:
        for obj in group:
            peers = [o for o in group if o is not obj]
            if not peers:
                continue
            other = min(peers, key=lambda o: distance(obj["cell"], o["cell"]))
            a = building_role(obj) if obj["class"] == "building" else obj["class"]
            b = building_role(other) if other["class"] == "building" else other["class"]
            adjacency[f"{a} -> {b}"].append(distance(obj["cell"], other["cell"]))
    by_id = {o["id"]: o for o in objects}
    links = defaultdict(list)
    battery = Counter()
    for obj in objects:
        parent = by_id.get(obj.get("parent_link"))
        if parent:
            links[f"{obj['class']} -> {parent['class']}"].append(distance(obj["cell"], parent["cell"]))
            if obj["class"] == "turret" and parent["class"] == "turret_control":
                battery[parent["id"]] += 1
    rotations = [abs(o["rotation"] % 90) for o in structs if o["class"] == "building"]
    aligned = sum(min(v, 90-v) <= 5 for v in rotations)
    wall_distances, wall_angles = [], []
    for obj in structs:
        if obj["class"] == "building" and walls:
            wall = min(walls, key=lambda other: distance(obj["cell"], other["cell"]))
            separation = distance(obj["cell"], wall["cell"])
            wall_distances.append(separation)
            if separation <= 12:
                delta = abs(obj["rotation"]-wall["rotation"]) % 180
                wall_angles.append(min(delta, 180-delta))
    # Axis-aligned road segments only: compare building rotation to local tangent.
    road_mask = masks["roads"][::step, ::step]
    road_cells = np.argwhere(road_mask)
    relative_angles = []
    for obj in structs:
        if obj["class"] != "building" or not len(road_cells):
            continue
        location = np.array(obj["cell"])/step
        rc = road_cells[np.square(road_cells-location).sum(axis=1).argmin()]
        r, c = map(int, rc)
        if distance(location, rc)*step > 12:
            continue
        vertical = r > 0 and r+1 < len(road_mask) and road_mask[r-1, c] and road_mask[r+1, c]
        horizontal = c > 0 and c+1 < road_mask.shape[1] and road_mask[r, c-1] and road_mask[r, c+1]
        if bool(vertical) != bool(horizontal):
            rotation = obj["rotation"] % 180
            tangent = 90 if vertical else 0
            delta = abs(rotation-tangent) % 180
            relative_angles.append(min(delta, 180-delta))
    return {"name": layout["name"], "corpus": layout["source"]["corpus"],
            "size": layout["size"]["rows"], "objects": len(objects),
            "classes": dict(Counter(o["class"] for o in objects)), "bases": records,
            "roads": road_shape(layout), "links": dict(links), "battery_sizes": list(battery.values()),
            "adjacency": dict(adjacency), "building_slopes": slopes,
            "building_elevation": [terrain_at(layout, "elevation_wu", o["cell"]) for o in structs],
            "building_water_distance": [near("water", o["cell"]) for o in structs],
            "building_cliff_distance": [near("cliffs", o["cell"]) for o in structs],
            "building_road_distance": [near("roads", o["cell"]) for o in structs],
            "building_wall_distance": wall_distances, "wall_relative_angles": wall_angles,
            "wall_cement_distance": [near("cement", o["cell"]) for o in walls],
            "on_water": sum(bool(masks["water"][tuple(o["cell"])]) for o in structs),
            "steep": sum(s >= STEEP_DEG for s in slopes), "structures": len(structs),
            "cardinal_rotation": [aligned, len(rotations)], "road_relative_angles": relative_angles,
            "warnings": layout.get("warnings", [])}


def aggregate(measurements):
    bases = [b for m in measurements for b in m["bases"]]
    result = {"missions": len(measurements), "base_count": len(bases),
              "enemy_bases": sum(b["team"] > 0 for b in bases),
              "enemy_walled": sum(b["team"] > 0 and b["archetype"] == "walled_compound" for b in bases),
              "bases_near_roads": sum(b["road_distance"] is not None and b["road_distance"] <= 3 for b in bases),
              "archetypes": {}, "metrics": {}, "links": {}, "adjacency": {}}
    for name in ARCHETYPES:
        rows = [b for b in bases if b["archetype"] == name]
        mix = Counter()
        for row in rows:
            mix.update(row["mix"])
        result["archetypes"][name] = {"count": len(rows), "mix": dict(mix),
            **{field: summary([row[field] for row in rows]) for field in
               ("width", "height", "objects", "turrets", "generators", "road_distance", "slope_deg", "player_distance")}}
    for field in ("battery_sizes", "building_slopes", "building_elevation", "building_water_distance",
                  "building_cliff_distance", "building_road_distance", "building_wall_distance",
                  "wall_cement_distance", "road_relative_angles", "wall_relative_angles"):
        result["metrics"][field] = summary([v for m in measurements for v in m[field]])
    for field in ("components", "largest_fraction", "edge_components", "coverage_percent", "endpoints", "junction_samples"):
        result["metrics"][f"roads_{field}"] = summary([m["roads"][field] for m in measurements])
    result["metrics"]["enemy_player_distance_fraction"] = summary([
        b["player_distance"]/m["size"] for m in measurements for b in m["bases"]
        if b["team"] > 0 and b["player_distance"] is not None])
    result["metrics"]["enemy_bases_per_100k_cells"] = summary([
        sum(b["team"] > 0 for b in m["bases"])*100000/m["size"]**2 for m in measurements])
    result["metrics"]["tree_objects_per_100k_cells"] = summary([
        m["classes"].get("tree", 0)*100000/m["size"]**2 for m in measurements])
    for kind in ("links", "adjacency"):
        values = defaultdict(list)
        for m in measurements:
            for name, distances in m[kind].items():
                values[name].extend(distances)
        result[kind] = {name: summary(distances) for name, distances in sorted(values.items())}
    result["placement"] = {field: sum(m[field] for m in measurements) for field in ("on_water", "steep", "structures")}
    result["rotation"] = [sum(m["cardinal_rotation"][i] for m in measurements) for i in (0, 1)]
    result["map_size"] = summary([m["size"] for m in measurements])
    return result


def fmt(stats):
    if not stats["n"]:
        return "n/a"
    return f"{stats['median']:g} [{stats['p10']:g}..{stats['p90']:g}]"


def report(profile, manifest):
    groups = profile["groups"]
    lines = ["# Authored MC2 layout corpus", "", "Generated by `py -3 -B tools/mapgen/analyze.py`.", "",
        f"Extracted **{len(manifest['missions'])}** mission/FIT pairs from seven installs; "
        f"**{len(manifest['errors'])}** extraction errors. Stock calibration uses exactly mc2_01..mc2_24.", "",
        "## Method and limits", "",
        "Distances are movement cells (multiply by 42.6667 for world units). Tables show median [p10..p90], "
        "not design mandates. Structural clusters use same-team single-link distance <=12 cells (512 wu), "
        "retaining >=3 objects or any turret. Walls within 8 cells of a structure join its bounds; >=6 wall "
        "pieces defines a walled compound. This can merge adjacent sites and split sparse bases. "
        "Archetype labels are explicit heuristics: walls first, then turret batteries, airfield/HQ names, "
        "industrial mix >=35%, town mix >=40%, otherwise outpost. Building roles are name-based and inspectable "
        "in common.py. Towns can be neutral. Props, trees, rocks and fences do not become structural clusters.", "",
        "Feature distances use an eight-neighbour (Chebyshev) transform on the original 3-cell vertex grid. "
        "Objects are point centres: no inferred model footprints. Slope uses finite differences, steep >=35 deg, "
        "flat <10 deg. Water uses bit 0. Paved areas use terrain types 10,13..20; roads use decoded overlays "
        "except rough. Vertex footprints and road junction counts are approximate. No claim of full MOVE "
        "passability, scripted objectives or vehicle traversal is made.", "",
        "All paired maps were extracted, including tutorial, multiplayer and local test maps. Comparisons below "
        "use custom mod stems absent from the stock install; mods without custom stems use their 24 inherited "
        "campaign maps. Thus inherited recataloging does not masquerade as new mission design. The pooled "
        "generator profile uses stock campaign + custom maps, deduplicated by decoded terrain/object packet "
        "SHA-256. Mod-specific tables may still share maps. Source provenance is in the local index and a "
        "compact committed extraction_manifest.json; full mission JSON is regenerable, ignored local data.", "",
        "## Coverage and differences", "",
        "| Install | Extracted | Comparison scope | Used | Map side cells | Enemy clusters |",
        "|---|---:|---|---:|---|---:|"]
    for label, group in groups.items():
        total = sum(m["corpus"] == label for m in manifest["missions"])
        lines.append(f"| {label} | {total} | {profile['scopes'][label]} | {group['missions']} | {fmt(group['map_size'])} | {group['enemy_bases']} |")
    lines += ["", "## Base archetypes", "", "All-team clusters; point-centre bounds include attached walls.", "",
              "| Dataset / archetype | Clusters | Width cells | Height cells | Structures | Turrets | Nearest road cells |",
              "|---|---:|---|---|---|---|---|"]
    for label, group in groups.items():
        for name, stats in group["archetypes"].items():
            if stats["count"]:
                lines.append(f"| {label} / {name} | {stats['count']} | {fmt(stats['width'])} | {fmt(stats['height'])} | {fmt(stats['objects'])} | {fmt(stats['turrets'])} | {fmt(stats['road_distance'])} |")
    stock = groups["stock"]
    lines += ["", f"Under this clustering rule, **{stock['enemy_bases']-stock['enemy_walled']}/{stock['enemy_bases']} "
              "stock enemy sites are open**, and "
              f"**{stock['bases_near_roads']}/{stock['base_count']} all-team sites** have a structure within "
              "three cells of a road. These counts differ from the earlier 173-cluster memory survey because "
              "the inclusion and wall/post association rules here are explicit and reproducible."]
    lines += ["", "## Stock building mixes and adjacency", "", "| Archetype | Structural role counts |", "|---|---|"]
    for name, stats in stock["archetypes"].items():
        lines.append(f"| {name} | " + ", ".join(f"{k}={v}" for k, v in sorted(stats["mix"].items())) + " |")
    lines += ["", "Directed nearest-neighbour pairs inside clusters; counts count source structures, not independent bases.", "",
              "| Pair | Samples | Distance cells |", "|---|---:|---|"]
    for name, stats in sorted(stock["adjacency"].items(), key=lambda v: -v[1]["n"])[:18]:
        lines.append(f"| {name} | {stats['n']} | {fmt(stats)} |")
    lines += ["", "| Resolved parent chain | Links | Distance cells |", "|---|---:|---|"]
    for name, stats in stock["links"].items():
        lines.append(f"| {name} | {stats['n']} | {fmt(stats)} |")
    lines += ["", "## Road networks, terrain and teams", "",
              "Road components use 8-neighbour adjacency on source samples; 4-neighbour degrees provide endpoint "
              "and junction proxies. Authored roads can be fragmented by terrain, bridges, plazas and missing "
              "overlay samples. Generated roads intentionally impose a single traversable component.", "",
              "| Dataset | Road coverage % | Road components | Largest road fraction | Edge components | Turrets / linked control |",
              "|---|---|---|---|---|---|"]
    for label, group in groups.items():
        m = group["metrics"]
        lines.append(f"| {label} | {fmt(m['roads_coverage_percent'])} | {fmt(m['roads_components'])} | {fmt(m['roads_largest_fraction'])} | {fmt(m['roads_edge_components'])} | {fmt(m['battery_sizes'])} |")
    lines += ["", "| Dataset | Structure slope deg | Elevation wu | Water distance cells | Cliff distance cells | Enemy/start distance / side |",
              "|---|---|---|---|---|---|"]
    for label, group in groups.items():
        m = group["metrics"]
        lines.append(f"| {label} | {fmt(m['building_slopes'])} | {fmt(m['building_elevation'])} | {fmt(m['building_water_distance'])} | {fmt(m['building_cliff_distance'])} | {fmt(m['enemy_player_distance_fraction'])} |")
    lines += ["", "| Dataset | Structures | Wet centres | Steep centres | Cardinal building rotations (within 5 deg) |",
              "|---|---:|---:|---:|---|"]
    for label, group in groups.items():
        p, rot = group["placement"], group["rotation"]
        lines.append(f"| {label} | {p['structures']} | {p['on_water']} | {p['steep']} | {rot[0]}/{rot[1]} |")
    lines += ["", f"Stock wall-to-cement distance: **{fmt(stock['metrics']['wall_cement_distance'])} cells**. "
              "This coarse vertex measurement must not be substituted for a precise cement-edge offset.", "",
              f"Stock rotation relative to the tangent of a nearby straight road (<=12 cells): "
              f"**{fmt(stock['metrics']['road_relative_angles'])} deg**. Object pivots differ by model; "
              "0 and 90 deg both describe grid-aligned siting, not a measured front-door direction.", "",
              f"Stock building-to-nearest-wall distance: **{fmt(stock['metrics']['building_wall_distance'])} cells**. "
              f"For buildings within 12 cells of a wall, relative rotation is "
              f"**{fmt(stock['metrics']['wall_relative_angles'])} deg**. These are model rotations, not "
              "verified door/facing orientations.", "",
              "## Generator calibration", "", "`tools/mapgen/profile.json` stores measured distributions, role mixes and "
              "link ranges, with the extraction-index SHA-256. The generator samples from these ranges, "
              "constraining extreme chained clusters to usable parcel dimensions. It prefers open sites in "
              "the measured proportions, puts roads first, and strengthens traversability/parent-chain "
              "invariants beyond what coarse authored data can prove. Synthetic terrain and rectangular "
              "parcels are implementation choices, not measured stock rules.", "",
              "## Data quality", ""]
    warnings = [(row["corpus"], row["name"], row["warnings"]) for row in manifest["missions"] if row["warnings"]]
    lines.append(f"{len(warnings)} missions have nonfatal source warnings. Unknown catalog IDs remain `unknown` "
                 "and are excluded from structural statistics; unresolved parents remain packed cells. "
                 "No missing relationship is synthesized in extracted data.")
    for label in sorted({r[0] for r in warnings}):
        counter = Counter(w.split(":")[0] for l, _, warnings_ in warnings if l == label for w in warnings_)
        lines.append(f"- {label}: " + ", ".join(f"{key} in {n} maps" for key, n in counter.items()))
    for error in manifest["errors"]:
        lines.append(f"- Extraction error: {error}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path(__file__).resolve().parent / "corpus")
    parser.add_argument("--out", type=Path, default=Path("docs/mapgen/corpus.md"))
    parser.add_argument("--profile", type=Path, default=Path(__file__).resolve().parent / "profile.json")
    args = parser.parse_args(argv)
    index_path = args.corpus / "index.json"
    manifest = load_json(index_path)
    baseline_names = {m["name"] for m in manifest["missions"] if m["corpus"] == "stock"}
    by_label = defaultdict(list)
    for row in manifest["missions"]:
        by_label[row["corpus"]].append(row)
    groups, scopes, pooled, seen = {}, {}, [], set()
    for label, rows in by_label.items():
        campaign = [m for m in rows if re.fullmatch(r"mc2_\d\d", m["name"])]
        custom = [m for m in rows if m["name"] not in baseline_names]
        selected = campaign if label == "stock" or not custom else custom
        scopes[label] = "24 campaign maps" if label == "stock" else "custom stems" if custom else "inherited campaign"
        measurements = []
        for row in selected:
            data = load_json(args.corpus / row["file"])
            measurement = measure(data)
            measurements.append(measurement)
            fingerprint = data["source"]["terrain_objects_sha256"]
            if (label == "stock" or custom) and fingerprint not in seen:
                pooled.append(measurement)
                seen.add(fingerprint)
        groups[label] = aggregate(measurements)
        print(f"{label}: {len(measurements)} maps, {groups[label]['base_count']} clusters", flush=True)
    canonical_index = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    profile = {"schema": "mc2-mapgen-profile-1", "index_sha256": hashlib.sha256(canonical_index).hexdigest(),
               "cluster_radius_cells": CLUSTER_RADIUS, "groups": groups, "scopes": scopes,
               "pooled": aggregate(pooled)}
    write_json(args.profile, profile, pretty=True)
    path = output_path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(report(profile, manifest), encoding="utf-8")
    # Source provenance survives commits without copying hundreds of MB of mission data.
    write_json(path.parent / "extraction_manifest.json", manifest, pretty=True)
    write_json(args.corpus / "measurements.json", {"pooled": pooled}, pretty=False)
    print(f"wrote {path} and {args.profile}")


if __name__ == "__main__":
    main()
