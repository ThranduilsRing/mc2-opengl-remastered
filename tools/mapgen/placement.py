"""Continuous world-unit placement inside phase-1 terrain/road parcels."""
from __future__ import annotations

from collections import defaultdict
import hashlib
import json
import math
import random

import numpy as np

from tools.mapgen.common import CELL_WU, FLAT_DEG, distance_field, mask_of, packed_cell, slope_grid
from tools.mapgen.footprints import table
from tools.mapgen.raster import polygon, world_position


def bounds(points):
    xs,ys = zip(*points)
    return min(xs),min(ys),max(xs),max(ys)


def overlaps(a, b, clearance=0):
    """Separating-axis test for rotated convex polygons; touching is permitted."""
    if any(bounds(a)[i+2]+clearance <= bounds(b)[i] or bounds(b)[i+2]+clearance <= bounds(a)[i] for i in (0,1)):
        return False
    for points in (a,b):
        for p,q in zip(points,points[1:]+points[:1]):
            dx,dy = q[0]-p[0],q[1]-p[1]
            length = math.hypot(dx,dy)
            if length < 1e-8:
                continue
            axis = (-dy/length,dx/length)
            aa = [x*axis[0]+y*axis[1] for x,y in a]
            bb = [x*axis[0]+y*axis[1] for x,y in b]
            if max(aa)+clearance <= min(bb)+1e-6 or max(bb)+clearance <= min(aa)+1e-6:
                return False
    return True


class Spatial:
    """Bucketed broad phase so large forests avoid quadratic collision work."""
    def __init__(self):
        self.buckets = defaultdict(list)

    def keys(self, poly, margin=0):
        x0,y0,x1,y1 = bounds(poly)
        x0,y0,x1,y1 = x0-margin,y0-margin,x1+margin,y1+margin
        return [(x,y) for x in range(math.floor(x0/128),math.floor(x1/128)+1)
                for y in range(math.floor(y0/128),math.floor(y1/128)+1)]

    def candidates(self, poly):
        return {obj["id"]: (obj,p) for key in self.keys(poly,4) for obj,p in self.buckets[key]}.values()

    def add(self,obj,poly):
        for key in self.keys(poly):
            self.buckets[key].append((obj,poly))


def cell_of(layout, position):
    return [math.floor(layout["size"]["rows"]/2-position[1]/CELL_WU),
            math.floor(position[0]/CELL_WU+layout["size"]["cols"]/2)]


def covered_cells(layout, poly):
    x0,y0,x1,y1 = bounds(poly)
    r0,c0 = cell_of(layout,(x0,y1))
    r1,c1 = cell_of(layout,(x1-1e-5,y0+1e-5))
    return r0,c0,r1+1,c1+1


def upgrade(layout, profile):
    """Replace abstract cell objects with real IDs and unscaled model bounds.

    The old layout supplies only terrain, road routes, parcels, requested mixes
    and relationship topology. Every final position is accepted using its whole
    rotated rectangle, not the old point occupancy. Walls are rebuilt at native
    segment length; no model is scaled to fit a cell.
    """
    rng = random.Random(layout["source"]["seed"] ^ 0x50495845)
    catalog = table()
    style = layout["source"]["style"]
    install = "stock" if style in ("stock","mixed") else style
    assets = [v for v in catalog["types"].values() if v["install"] == install and v["method"] in ("tgl","ase")]
    pools = defaultdict(list)
    for asset in sorted(assets,key=lambda a:a["type_id"]):
        if asset["class"] == "building" and any(s in asset["type_name"].lower() for s in ("nav", "marker", "crash", "spotlight")):
            continue
        if asset["class"] == "building" and max(asset["width_wu"],asset["depth_wu"]) > 300:
            continue
        pools[(asset["class"],asset["role"] if asset["class"] == "building" else None)].append(asset)
    def pool(cls, role=None):
        result = pools[(cls,role if cls == "building" else None)]
        if not result:
            raise ValueError(f"no measured {install} model for {cls}/{role}")
        return result
    def model(cls,role=None):
        return rng.choice(pool(cls,role))
    size = layout["size"]["rows"]
    roads = mask_of(layout,"roads")
    parcels = np.zeros_like(roads)
    for base in layout["bases"]:
        r0,c0,r1,c1 = base["bounds"]
        parcels[r0:r1,c0:c1] = True
    tree_road_distance,tree_base_distance = distance_field(roads),distance_field(parcels)
    allowed = ~mask_of(layout,"water") & (slope_grid(layout["terrain"]["elevation_wu"]) < FLAT_DEG)
    old_objects = layout["objects"]
    final, by_id, occupied = [], {}, set()
    spatial = Spatial()
    wall_edges = []

    def make(request, asset, centre, rotation=None):
        obj = {**request, "type_id": asset["type_id"], "type_name": asset["type_name"],
               "appearance": asset["appearance"], "object_class": asset["object_class"],
               "footprint_key": f"{install}:{asset['type_id']}",
               "footprint": {key: asset[key] for key in ("width_wu","depth_wu","height_wu","bounds_wu","outline_wu","method")}}
        obj["rotation"] = rotation if rotation is not None else request.get("rotation",0)
        # Centre placement uses the model's asymmetric pivot, then exports the
        # original pivot position expected by Packet 1.
        low,high = asset["bounds_wu"]
        dx,dy = (low[0]+high[0])/2,(low[1]+high[1])/2
        angle = math.radians(obj["rotation"])
        co,si = math.cos(angle),math.sin(angle)
        x,y = centre[0]-dx*co+dy*si,centre[1]-dx*si-dy*co
        r,c = cell_of(layout,(x,y))
        if not (0 <= r < size and 0 <= c < size):
            return None
        obj["cell"] = [r,c]
        obj["position_wu"] = [round(x,4),round(y,4),layout["terrain"]["elevation_wu"][r][c]]
        return obj

    def accept(obj, parcel=None, seam=False):
        if obj is None:
            return False
        poly = polygon(obj,obj["footprint"],obj["position_wu"],rectangle=True)
        r0,c0,r1,c1 = covered_cells(layout,poly)
        if min(r0,c0)<0 or max(r1,c1)>size or not allowed[r0:r1,c0:c1].all():
            return False
        if parcel:
            a,b,c,d = parcel
            if not (a <= r0 and b <= c0 and r1 <= c and c1 <= d):
                return False
        if tuple(obj["cell"]) in occupied:
            return False
        if not seam and roads[tuple(obj['cell'])]:
            return False
        if obj["class"]=="tree" and (tree_road_distance[tuple(obj["cell"])]<4 or
                                      tree_base_distance[tuple(obj["cell"])]<4):
            return False
        if not seam and roads[r0:r1,c0:c1].any():
            return False
        if not seam and any(overlaps(poly,other,clearance=4) for _,other in spatial.candidates(poly)):
            return False
        role = obj.get("role") if obj["class"] == "building" else obj["class"]
        spacing = profile["groups"]["stock"]["adjacency"].get(f"{role} -> {role}",{})
        if spacing.get("n",0) >= 20 and obj.get("base_id"):
            peers = [o for o in final if o.get("base_id")==obj["base_id"] and
                     (o.get("role") if o["class"]=="building" else o["class"])==role]
            if peers:
                nearest = min(math.dist(obj["position_wu"][:2],o["position_wu"][:2]) for o in peers)/CELL_WU
                if not spacing["min"] <= nearest <= spacing["max"]:
                    return False
        final.append(obj)
        by_id[obj["id"]] = obj
        occupied.add(tuple(obj["cell"]))
        spatial.add(obj,poly)
        return True

    for base in layout["bases"]:
        r0,c0,r1,c1 = base["bounds"]
        requests = [o for o in old_objects if o.get("base_id") == base["id"] and o["class"] != "wall"]
        gate_request = next((o for o in requests if o["class"] == "gate"),None)
        if base["walled"]:
            # Select a matched family where possible; native length, no scale.
            walls = [a for a in pool("wall") if a["depth_wu"] > a["width_wu"]*3]
            wall = next((a for a in walls if a["type_name"] == "L_LightWall"),walls[0])
            gates = pool("gate")
            gate_asset = next((a for a in gates if a["type_name"] == wall["type_name"].replace("Wall","Gate")),gates[0])
            x0,x1 = (c0+.5-size/2)*CELL_WU,(c1-.5-size/2)*CELL_WU
            y0,y1 = (size/2-r1+.5)*CELL_WU,(size/2-r0-.5)*CELL_WU
            gate_centre = world_position(layout,gate_request)
            gate_axis = gate_request["wall_axis"]
            gate_rotation = 90 if gate_axis == "horizontal" else 0
            gate_obj = make(gate_request,gate_asset,gate_centre,gate_rotation)
            if not accept(gate_obj,seam=True):
                raise ValueError("cannot fit native gate on the reserved road crossing")
            for edge_index,(a,b,rotation) in enumerate((((x0,y1),(x1,y1),90),((x1,y1),(x1,y0),0),
                                                        ((x1,y0),(x0,y0),90),((x0,y0),(x0,y1),0))):
                dx,dy = b[0]-a[0],b[1]-a[1]
                length = math.hypot(dx,dy)
                ux,uy = dx/length,dy/length
                gx,gy = gate_centre
                t = (gx-a[0])*ux+(gy-a[1])*uy
                on_edge = abs((gx-a[0])*uy-(gy-a[1])*ux) < 1
                gate_span = gate_asset["depth_wu"]
                runs = [(0,length)]
                if on_edge:
                    runs = [(0,t-gate_span/2),(t+gate_span/2,length)]
                ids = []
                for lo,hi in runs:
                    if hi <= lo:
                        raise ValueError("native gate is wider than its wall edge")
                    native = wall["depth_wu"]
                    # Fit the smallest number of unscaled native pieces. Short
                    # runs share end caps rather than introducing a wall gap.
                    count = max(1,math.ceil((hi-lo)/native))
                    centres = [(lo+hi)/2] if count == 1 else np.linspace(lo+native/2,hi-native/2,count)
                    for number,twall in enumerate(centres):
                        request = {"id": f"{base['id']}_wall_{edge_index}_{len(ids)}", "class": "wall", "team": base["team"],
                                   "base_id": base["id"], "parent_link": None,"parent_cell": None,"parent_id_raw": -1,"damage":0}
                        obj = make(request,wall,(a[0]+ux*twall,a[1]+uy*twall),rotation)
                        if not accept(obj,seam=True):
                            raise ValueError("native wall segment does not fit dry flat parcel")
                        ids.append(obj["id"])
                wall_edges.append({"base_id":base["id"],"start":list(a),"end":list(b),"wall_ids":ids,
                                   "gate_id":gate_obj["id"] if on_edge else None})
        # Parent-first placement means sampled distances refer to final pivots.
        priority = {"turret_generator":0,"gate_control":0,"turret_control":1,"turret":2,"building":3,"gate":4}
        for request in sorted(requests,key=lambda o:(priority.get(o["class"],3),o["id"])):
            cls = request["class"]
            if cls == "gate":
                continue
            role = request.get("role")
            assets = pool(cls,role)
            target = world_position(layout,request)
            parent = by_id.get(request.get("parent_link"))
            link_stats = profile["groups"]["stock"]["links"].get(f"{cls} -> {parent['class']}") if parent else None
            inset = 2 if base["walled"] else 1
            parcel = [r0+inset,c0+inset,r1-inset,c1-inset]
            placed = False
            for attempt in range(600):
                asset = assets[attempt % len(assets)] if attempt >= 300 else rng.choice(assets)
                if parent and link_stats:
                    radius = rng.triangular(link_stats["p10"],min(link_stats["p90"],max(r1-r0,c1-c0)*.65),
                                            min(link_stats["median"],max(r1-r0,c1-c0)*.5))*CELL_WU
                    angle = rng.uniform(0,math.tau)
                    centre = (parent["position_wu"][0]+radius*math.cos(angle),parent["position_wu"][1]+radius*math.sin(angle))
                elif attempt < 80:
                    centre = (target[0]+rng.uniform(-60,60),target[1]+rng.uniform(-60,60))
                else:
                    centre = (rng.uniform(c0+inset,c1-inset)*CELL_WU-size*CELL_WU/2,
                              size*CELL_WU/2-rng.uniform(r0+inset,r1-inset)*CELL_WU)
                obj = make(request,asset,centre)
                if obj and link_stats:
                    separation = math.dist(obj["position_wu"][:2],parent["position_wu"][:2])/CELL_WU
                    if not link_stats["min"] <= separation <= link_stats["max"]:
                        continue
                if accept(obj,parcel):
                    placed = True
                    break
            if not placed and cls != "building":
                raise ValueError(f"cannot fit native {cls} in {base['id']}")
        if gate_request:
            gate = by_id[gate_request["id"]]
            parent = by_id[gate["parent_link"]]
            link = profile["groups"]["stock"]["links"]["gate -> gate_control"]
            if not link["min"] <= math.dist(gate["position_wu"][:2],parent["position_wu"][:2])/CELL_WU <= link["max"]:
                raise ValueError("gate control outside measured distance")
    # Cluster membership survives from the cell plan, positions gain continuous
    # jitter and native canopy rejection. Forests remain point sets in world units.
    for request in old_objects:
        if request["class"] != "tree":
            continue
        target = world_position(layout,request)
        for _ in range(12):
            centre = (target[0]+rng.uniform(-18,18),target[1]+rng.uniform(-18,18))
            obj = make(request,model("tree"),centre,rng.uniform(0,360))
            if accept(obj):
                break
    for obj in final:
        parent = by_id.get(obj.get("parent_link"))
        obj["parent_cell"] = parent["cell"] if parent else None
        obj["parent_id_raw"] = packed_cell(parent["cell"]) if parent else -1
    layout["objects"] = final
    layout["wall_edges_wu"] = wall_edges
    layout["source"]["asset_install"] = install
    layout["source"]["footprints_sha256"] = hashlib.sha256(json.dumps(catalog["types"],sort_keys=True).encode()).hexdigest()
    layout["generation"]["model_footprints"] = "native rotated rectangles; world-unit pivots"
    layout["generation"]["placement_version"] = 2
    layout["generation"]["clearance_wu"] = 4
    layout["generation"]["wall_seam_allowance"] = "connected end caps/corners, at most half the shorter native segment"
    for marker,base in zip(layout["objectives"],[b for b in layout["bases"] if b["team"]>0]):
        target = next((o for o in final if o.get("base_id")==base["id"] and o["class"]=="building"),None)
        if target:
            marker["cell"] = target["cell"]
            marker["position_wu"] = target["position_wu"]
    for part in layout["parts"]:
        x,y = world_position(layout,part)
        r,c = part["cell"]
        part["position_wu"] = [round(x,4),round(y,4),layout["terrain"]["elevation_wu"][r][c]]
    layout["player_start_wu"] = layout["parts"][0]["position_wu"]
    return layout
