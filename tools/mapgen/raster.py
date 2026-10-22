"""North-up pixel-art tacmaps, using model footprints and authored road masks."""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import CELL_WU, building_role, load_json, mask_of, output_path, write_json
from tools.mapgen.footprints import lookup, table

COLOURS = {"military": (144,157,168), "industrial": (189,153,103), "town": (185,177,153),
           "airfield": (158,173,187), "hq": (165,148,192), "turret": (207,120,94),
           "turret_generator": (205,172,77), "turret_control": (115,184,193),
           "gate_control": (102,166,175), "gate": (212,178,91), "wall": (163,158,140),
           "fence": (133,134,114), "tree": (47,86,54), "rock": (120,119,111),
           "bridge": (162,159,140), "prop": (139,133,111)}


def world_position(layout, obj):
    if obj.get("position_wu") is not None:
        return obj["position_wu"][:2]
    r, c = obj["cell"]
    return [(c+.5-layout["size"]["cols"]/2)*CELL_WU,
            (layout["size"]["rows"]/2-r-.5)*CELL_WU]


def polygon(obj, footprint, position, rectangle=False):
    if rectangle or not footprint.get("outline_wu"):
        low, high = footprint["bounds_wu"]
        outline = [(low[0],low[1]), (high[0],low[1]), (high[0],high[1]), (low[0],high[1])]
    else:
        outline = footprint["outline_wu"]
    angle = math.radians(obj.get("rotation", 0))
    co, si = math.cos(angle), math.sin(angle)
    return [(position[0]+x*co-y*si, position[1]+x*si+y*co) for x,y in outline]


def crop_base(layout, index=0, margin=5):
    rows, cols = layout["size"]["rows"], layout["size"]["cols"]
    if layout.get("bases"):
        bounds = layout["bases"][index % len(layout["bases"])]["bounds"]
    else:
        from tools.mapgen.analyze import clusters, valid_objects
        groups = sorted(clusters(valid_objects(layout)), key=len, reverse=True)
        if not groups:
            return [max(0,rows//2-20),max(0,cols//2-20),min(rows,rows//2+20),min(cols,cols//2+20)]
        group = groups[index % len(groups)]
        rr, cc = np.array([o["cell"] for o in group]).T
        # Keep a huge single-link cluster from creating a full-size 1wu crop.
        cr, c = int(np.median(rr)), int(np.median(cc))
        bounds = [max(int(rr.min()),cr-38),max(int(cc.min()),c-38),
                  min(int(rr.max())+1,cr+38),min(int(cc.max())+1,c+38)]
    r0,c0,r1,c1 = bounds
    return [max(0,r0-margin),max(0,c0-margin),min(rows,r1+margin),min(cols,c1+margin)]


def render(layout, out, wu_per_pixel=None, crop=None, hillshade=True, catalog=None):
    catalog = catalog or table()
    rows, cols = layout["size"]["rows"], layout["size"]["cols"]
    r0,c0,r1,c1 = crop or (0,0,rows,cols)
    if not (0 <= r0 < r1 <= rows and 0 <= c0 < c1 <= cols):
        raise ValueError("crop must be inside the map")
    if wu_per_pixel is None:
        wu_per_pixel = 1 if crop else next((s for s in (2,4,8) if max(rows,cols)*CELL_WU/s <= 2304),8)
    if not math.isfinite(wu_per_pixel) or wu_per_pixel <= 0:
        raise ValueError("wu-per-pixel must be finite and positive")
    width = math.ceil((c1-c0)*CELL_WU/wu_per_pixel)
    height = math.ceil((r1-r0)*CELL_WU/wu_per_pixel)
    if max(width,height) > 16384 or width*height > 100_000_000:
        raise ValueError("raster too large; use a crop or a coarser scale")
    install = layout["source"].get("asset_install", layout["source"]["corpus"])
    surface = catalog.get("surfaces", {}).get(install, {})
    palette = np.tile([86,105,67], (256,1)).astype(np.uint8)
    for key, value in surface.get("terrain", {}).items():
        if int(key) < 256:
            palette[int(key)] = value["rgb"]
    palette[0] = (43,86,115)
    terrain = layout["terrain"]
    step = terrain["cell_step"]
    types = np.asarray(terrain["terrain_type"], dtype=int)
    base = palette[np.clip(types,0,255)]
    if hillshade:
        h = np.asarray(terrain["elevation_wu"],dtype=float)
        dr,dc = np.gradient(h, step*CELL_WU)
        shade = np.clip((1-.65*dc+.65*dr)/np.sqrt(1+dr*dr+dc*dc),.55,1.2)
        base = np.clip(base*shade[:,:,None],0,255).astype(np.uint8)
    # Crop before enlarging; memory usage depends on output, not full world size.
    rr = np.minimum(np.arange(r0,r1)//step, len(base)-1)
    cc = np.minimum(np.arange(c0,c1)//step, base.shape[1]-1)
    ground = Image.fromarray(base[rr[:,None],cc[None,:]])
    ground = ground.resize((width,height), Image.Resampling.NEAREST)
    draw = ImageDraw.Draw(ground)
    scale = CELL_WU/wu_per_pixel
    def xy(point):
        x,y = point
        return ((x/CELL_WU+cols/2-c0)*scale, (rows/2-y/CELL_WU-r0)*scale)
    for name, colour in (("cement",(125,128,124)), ("water",(43,86,115))):
        for r,a,b in layout["layers"].get(name,[]):
            if r0 <= r < r1 and a < c1 and b > c0:
                draw.rectangle((max(0,(a-c0)*scale),(r-r0)*scale,
                                min(width,(b-c0)*scale)-1,(r+1-r0)*scale-1),fill=colour)
    if layout.get("roads_wu"):
        for road in layout["roads_wu"]:
            points = [xy(p) for p in road["points"]]
            w = max(1,round(road["width_wu"]/wu_per_pixel))
            draw.line(points,fill=(151,143,116),width=w,joint="curve")
    else:
        # Tile alpha is the actual authored visual road width, not MOVE passability.
        overlays = np.asarray(terrain.get("overlay_type", []))
        handles = np.asarray(terrain.get("overlay_handle", []))
        # The catalog maps handles to overlay-local tile offsets in stored order.
        counts = {}
        for key in surface.get("overlays", {}):
            kind,tile = map(int,key.split(":"))
            counts[kind] = max(counts.get(kind,0),tile+1)
        starts, cursor = {}, 255
        for kind in sorted(counts):
            starts[kind], cursor = cursor, cursor+counts[kind]
        cache = {}
        for r in range(r0//step, min(len(overlays),math.ceil(r1/step))):
            for c in range(c0//step,min(overlays.shape[1],math.ceil(c1/step))):
                kind = int(overlays[r,c])
                if kind < 0 or kind == 2:
                    continue
                tile = int(handles[r,c])-starts.get(kind,255) if handles.size else 0
                key = f"{kind}:{tile}"
                definition = surface.get("overlays", {}).get(key)
                box = (round((c*step-c0)*scale),round((r*step-r0)*scale),
                       round(((c+1)*step-c0)*scale),round(((r+1)*step-r0)*scale))
                if definition:
                    size = (box[2]-box[0],box[3]-box[1])
                    ck = (key,size)
                    if ck not in cache:
                        mask = Image.new("L",tuple(reversed(definition["shape"])))
                        md = ImageDraw.Draw(mask)
                        for mr,a,b in definition["spans"]:
                            md.line((a,mr,b-1,mr),fill=255)
                        cache[ck] = mask.resize(size,Image.Resampling.NEAREST)
                    ground.paste((150,141,114),box,cache[ck])
                else:
                    draw.rectangle(box,fill=(150,141,114))
    draw = ImageDraw.Draw(ground)
    # North-up, deterministic object order; trees below buildings.
    objects = sorted(layout["objects"],key=lambda o:(o["class"] != "tree",o["id"]))
    unresolved = 0
    for obj in objects:
        fp = lookup(layout,obj,catalog)
        pos = world_position(layout,obj)
        points = [xy(p) for p in polygon(obj,fp,pos)]
        if not points:
            continue
        xs,ys = zip(*points)
        if max(xs) < 0 or max(ys) < 0 or min(xs) >= width or min(ys) >= height:
            continue
        cls = obj["class"]
        role = building_role(obj) if cls == "building" else cls
        colour = COLOURS.get(role,(140,135,120))
        team = obj.get("team",-1)
        tint = (95,164,195) if team == 0 else (190,98,85) if team > 0 else colour
        colour = tuple(round(.82*a+.18*b) for a,b in zip(colour,tint))
        edge = tuple(max(0,v-55) for v in colour)
        if fp["width_wu"] == 0 or fp["depth_wu"] == 0:
            x,y = xy(pos)
            draw.line((x-2,y-2,x+2,y+2),fill=(240,60,220))
            draw.line((x-2,y+2,x+2,y-2),fill=(240,60,220))
            unresolved += 1
        elif cls == "tree":
            box = (min(xs),min(ys),max(xs),max(ys))
            draw.ellipse(box,fill=colour,outline=(31,62,39))
            if max(xs)-min(xs)>5:
                draw.ellipse((min(xs)+1,min(ys)+1,(min(xs)+max(xs))/2,(min(ys)+max(ys))/2),fill=(66,108,64))
        else:
            shadow = [(x+max(1,3/wu_per_pixel),y+max(1,3/wu_per_pixel)) for x,y in points]
            draw.polygon(shadow,fill=(48,53,43))
            draw.polygon(points,fill=colour,outline=edge)
            if cls not in ("wall","fence","gate") and max(xs)-min(xs)>7 and max(ys)-min(ys)>7:
                cx,cy = sum(xs)/len(xs),sum(ys)/len(ys)
                roof = [(cx+(x-cx)*.65,cy+(y-cy)*.65) for x,y in points]
                draw.polygon(roof,outline=tuple(min(255,v+25) for v in colour))
            if cls == "turret":
                x,y = xy(pos)
                a = math.radians(obj.get("rotation",0))
                length = max(3,min(fp["width_wu"],fp["depth_wu"])*.6/wu_per_pixel)
                draw.line((x,y,x+math.cos(a)*length,y-math.sin(a)*length),fill=(50,49,45),width=2)
    for marker in layout.get("objectives",[]):
        if marker.get("display") and not marker.get("hidden") and marker.get("cell"):
            r,c = marker["cell"]
            x,y = (c+.5-c0)*scale,(r+.5-r0)*scale
            draw.line([(x,y-7),(x+7,y),(x,y+7),(x-7,y),(x,y-7)],fill=(250,220,108),width=2)
    if layout.get("player_start"):
        r,c = layout["player_start"]
        x,y = (c+.5-c0)*scale,(r+.5-r0)*scale
        draw.polygon([(x,y-9),(x-7,y+7),(x,y+3),(x+7,y+7)],fill=(95,225,250),outline=(22,52,64))
    # A compact legend and scale live outside the map, preserving pixel geometry.
    canvas = Image.new("RGB",(max(width,600),height+68),(23,30,34))
    canvas.paste(ground,(0,68))
    d = ImageDraw.Draw(canvas)
    d.text((12,7),f"{install} / {layout['name']}   N ^   {wu_per_pixel:g} wu/px",fill=(230,232,220))
    d.text((12,24),"Terrain + model bounds | blue: player  red tint: enemy  diamond: objective",fill=(176,186,181))
    x = 12
    for label in ("military","industrial","town","hq","turret","wall","tree"):
        d.rectangle((x,45,x+10,55),fill=COLOURS[label])
        d.text((x+15,44),label,fill=(206,211,202))
        x += len(label)*6+31
    bar = max(1,round(256/wu_per_pixel))
    d.line((12,height+50,12+bar,height+50),fill=(241,230,189),width=2)
    d.text((12,height+33),"256 wu",fill=(241,230,189))
    path = output_path(out)
    path.parent.mkdir(parents=True,exist_ok=True)
    canvas.save(path,optimize=True)
    metadata = {"mission": layout["name"], "install": install, "wu_per_pixel": wu_per_pixel,
                "crop_cells": [r0,c0,r1,c1], "map_pixels": [width,height], "header_pixels": 68,
                "unresolved_visible_footprints": unresolved, "hillshade": hillshade,
                "road_geometry": "world polylines" if layout.get("roads_wu") else "authored alpha masks"}
    write_json(path.with_suffix(".render.json"),metadata,pretty=True)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mission",nargs="?",type=Path)
    parser.add_argument("--out",type=Path)
    parser.add_argument("--wu-per-pixel",type=float)
    parser.add_argument("--crop",nargs=4,type=int,metavar=("R0","C0","R1","C1"))
    parser.add_argument("--base-crop",type=int)
    parser.add_argument("--no-hillshade",action="store_true")
    parser.add_argument("--all",action="store_true",help="all 320 phase-1 corpus missions")
    parser.add_argument("--examples",action="store_true",help="the phase-1 preview selection plus two base crops")
    parser.add_argument("--corpus",type=Path,default=Path("tools/mapgen/corpus"))
    args = parser.parse_args()
    if args.all or args.examples:
        rows = load_json(args.corpus/"index.json")["missions"]
        chosen = {str(p.relative_to(Path("docs/mapgen/examples"))).replace("\\","/")[:-4]
                  for p in Path("docs/mapgen/examples").glob("*/*.txt") if p.parent.name != "generated"}
        for row in rows:
            if args.all or row["file"][:-5] in chosen:
                layout = load_json(args.corpus/row["file"])
                path = (args.out or Path("docs/mapgen/raster"))/row["file"].replace(".json",".png")
                render(layout,path,args.wu_per_pixel,hillshade=not args.no_hillshade)
                if row["file"] in ("stock/mc2_03.json","stock/mc2_17.json"):
                    render(layout,path.with_stem(path.stem+"_base"),1,crop_base(layout))
                print(path,flush=True)
    elif args.mission:
        layout = load_json(args.mission)
        crop = crop_base(layout,args.base_crop) if args.base_crop is not None else args.crop
        render(layout,args.out or args.mission.with_suffix(".png"),args.wu_per_pixel,crop,not args.no_hillshade)
    else:
        parser.error("provide a mission, --all or --examples")


if __name__ == "__main__":
    main()
