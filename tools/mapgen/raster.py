"""North-up pixel-art tacmaps, using model footprints and authored road masks."""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw

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
        # Keep a huge single-link cluster from creating an excessively broad base crop.
        cr, c = int(np.median(rr)), int(np.median(cc))
        bounds = [max(int(rr.min()),cr-38),max(int(cc.min()),c-38),
                  min(int(rr.max())+1,cr+38),min(int(cc.max())+1,c+38)]
    r0,c0,r1,c1 = bounds
    return [max(0,r0-margin),max(0,c0-margin),min(rows,r1+margin),min(cols,c1+margin)]


# The owner scale is a measured stock Quonset long side / two pixels.
QUONSET_KEY = "stock:335"
VIEW_SCALE = 4
TERRAIN_COLOURS = {"grass": (78,96,60), "scrub": (108,111,69), "dirt": (139,118,83),
                   "sand": (183,159,104), "rock": (108,108,103), "snow": (209,218,210),
                   "cement": (121,125,123), "water": (43,83,111)}
TEAM_COLOURS = {-1: (206,189,133), 0: (89,187,225), 1: (218,91,70),
                2: (227,158,69), 3: (171,127,205)}
ROAD_COLOUR = (165,147,103)
GATE_COLOUR = (247,205,89)


def quonset_scale(catalog=None):
    asset = (catalog or table())["types"][QUONSET_KEY]
    return max(asset["width_wu"],asset["depth_wu"])/2


def pixel_rectangle(obj, footprint, wu_per_pixel):
    """Round native dimensions once, then snap display rotation to a quarter turn."""
    width = max(1,round(footprint["width_wu"]/wu_per_pixel))
    height = max(1,round(footprint["depth_wu"]/wu_per_pixel))
    if math.floor((obj.get("rotation",0)%360+45)/90)%2:
        width,height = height,width
    return width,height


def terrain_palette(surface):
    """Reduce install tac colours to eight flat terrain colours."""
    names = ("grass","scrub","dirt","sand","rock","snow")
    choices = np.asarray([TERRAIN_COLOURS[name] for name in names],dtype=int)
    palette = np.tile(TERRAIN_COLOURS["grass"],(256,1)).astype(np.uint8)
    for key,value in surface.get("terrain",{}).items():
        index = int(key)
        if 0 <= index < len(palette):
            colour = np.asarray(value["rgb"])
            palette[index] = choices[np.square(choices-colour).sum(axis=1).argmin()]
    palette[0] = TERRAIN_COLOURS["water"]
    palette[[10,*range(13,21)]] = TERRAIN_COLOURS["cement"]
    return palette


def render(layout, out, wu_per_pixel=None, crop=None, hillshade=False, catalog=None):
    catalog = catalog or table()
    rows,cols = layout["size"]["rows"],layout["size"]["cols"]
    r0,c0,r1,c1 = crop or (0,0,rows,cols)
    if not (0 <= r0 < r1 <= rows and 0 <= c0 < c1 <= cols):
        raise ValueError("crop must be inside the map")
    overridden = wu_per_pixel is not None
    reference = quonset_scale()
    wu_per_pixel = reference if wu_per_pixel is None else wu_per_pixel
    if not math.isfinite(wu_per_pixel) or wu_per_pixel <= 0:
        raise ValueError("wu-per-pixel must be finite and positive")
    width = math.ceil((c1-c0)*CELL_WU/wu_per_pixel)
    height = math.ceil((r1-r0)*CELL_WU/wu_per_pixel)
    if max(width,height)*VIEW_SCALE > 16384 or width*height*VIEW_SCALE**2 > 200_000_000:
        raise ValueError("raster/4x preview too large; use a crop or a coarser scale")
    install = layout["source"].get("asset_install",layout["source"]["corpus"])
    surface = catalog.get("surfaces",{}).get(install,{})
    terrain = layout["terrain"]
    step = terrain["cell_step"]
    # Sample output pixel centres directly: a 3072-cell map needs only ~3115
    # pixels per side, not a full world-unit or intermediate RGB heightfield.
    rr = np.minimum((r0+(np.arange(height)+.5)*wu_per_pixel/CELL_WU).astype(int),r1-1)
    cc = np.minimum((c0+(np.arange(width)+.5)*wu_per_pixel/CELL_WU).astype(int),c1-1)
    tr,tc = rr//step,cc//step
    types = np.asarray(terrain["terrain_type"],dtype=np.uint8)
    base = terrain_palette(surface)[types[tr[:,None],tc[None,:]]]
    if layout.get('macro'):
        macro = layout['macro']
        regional = np.asarray(macro['regions'],dtype=np.uint8)
        # Interpolate region membership, not integer labels (which would invent
        # intermediate land uses). This hides the planning grid at full-map scale.
        scores = [np.asarray(Image.fromarray((regional==i).astype(np.uint8)*255).resize(
                  (cols,rows),Image.Resampling.BILINEAR)) for i in range(5)]
        labels = np.argmax(np.stack(scores),axis=0)
        # Muted land-use colours keep native model silhouettes legible.
        palette = np.array([(78,96,60),(112,115,73),(117,109,82),
                            (94,104,70),(112,112,91)],dtype=np.uint8)
        base = palette[labels[rr[:,None],cc[None,:]]]
    if hillshade:
        h = np.asarray(terrain["elevation_wu"],dtype=float)[tr[:,None],tc[None,:]]
        if min(h.shape)>1:
            dr,dc = np.gradient(h,wu_per_pixel)
            lighting = np.clip((dr-dc)*.2,-.08,.08)
            shade = np.where(lighting < -.025,.94,np.where(lighting > .025,1.06,1.0))
            base = np.clip(base*shade[:,:,None],0,255).astype(np.uint8)
    for layer,colour in (("cement",TERRAIN_COLOURS["cement"]),("water",TERRAIN_COLOURS["water"])):
        mask = mask_of(layout,layer)
        base[mask[rr[:,None],cc[None,:]]] = colour
    ground = Image.fromarray(base)
    del base
    draw = ImageDraw.Draw(ground)
    scale = CELL_WU/wu_per_pixel
    def xy(point):
        x,y = point[:2]
        return ((x/CELL_WU+cols/2-c0)*scale,(rows/2-y/CELL_WU-r0)*scale)
    def snap(point):
        return tuple(math.floor(v) for v in xy(point))
    if layout.get("roads_wu"):
        for road in layout["roads_wu"]:
            points = [snap(p) for p in road["points"]]
            w = min(2,max(1,round(road["width_wu"]/wu_per_pixel)))
            draw.line(points,fill=ROAD_COLOUR,width=w)
    else:
        overlays = np.asarray(terrain.get("overlay_type",[]))
        handles = np.asarray(terrain.get("overlay_handle",[]))
        counts = {}
        for key in surface.get("overlays",{}):
            kind,tile = map(int,key.split(":"))
            counts[kind] = max(counts.get(kind,0),tile+1)
        starts,cursor = {},255
        for kind in sorted(counts):
            starts[kind],cursor = cursor,cursor+counts[kind]
        cache = {}
        for r in range(r0//step,min(len(overlays),math.ceil(r1/step))):
            for c in range(c0//step,min(overlays.shape[1],math.ceil(c1/step))):
                kind = int(overlays[r,c])
                if kind < 0 or kind == 2:
                    continue
                tile = int(handles[r,c])-starts.get(kind,255) if handles.size else 0
                key = f"{kind}:{tile}"
                definition = surface.get("overlays",{}).get(key)
                box = (round((c*step-c0)*scale),round((r*step-r0)*scale),
                       round(((c+1)*step-c0)*scale),round(((r+1)*step-r0)*scale))
                size = (box[2]-box[0],box[3]-box[1])
                if min(size)<=0:
                    continue
                if definition:
                    ck = (key,size)
                    if ck not in cache:
                        mask = Image.new("L",tuple(reversed(definition["shape"])))
                        md = ImageDraw.Draw(mask)
                        for mr,a,b in definition["spans"]:
                            md.line((a,mr,b-1,mr),fill=255)
                        # Area coverage keeps narrow authored roads legible at
                        # one pixel without antialiasing the final flat palette.
                        cache[ck] = mask.resize(size,Image.Resampling.BOX).point(lambda p:255 if p>=64 else 0)
                    ground.paste(ROAD_COLOUR,box,cache[ck])
                else:
                    cx,cy = (box[0]+box[2])//2,(box[1]+box[3])//2
                    draw.line((cx,box[1],cx,box[3]-1),fill=ROAD_COLOUR,width=1)
    draw = ImageDraw.Draw(ground)
    priority = {"tree":0,"prop":1,"wall":3,"fence":3,"turret":4,"gate":5}
    objects = sorted(layout["objects"],key=lambda o:(priority.get(o["class"],2),o["id"]))
    unresolved = 0
    for obj in objects:
        fp = lookup(layout,obj,catalog)
        pos = world_position(layout,obj)
        low,high = fp["bounds_wu"]
        angle = math.radians(obj.get("rotation",0))
        co,si = math.cos(angle),math.sin(angle)
        dx,dy = (low[0]+high[0])/2,(low[1]+high[1])/2
        centre = (pos[0]+dx*co-dy*si,pos[1]+dx*si+dy*co)
        x,y = snap(centre)
        pw,ph = pixel_rectangle(obj,fp,wu_per_pixel)
        if x+max(pw,ph)<0 or y+max(pw,ph)<0 or x-max(pw,ph)>=width or y-max(pw,ph)>=height:
            continue
        cls = obj["class"]
        role = building_role(obj) if cls=="building" else cls
        colour = COLOURS.get(role,(143,135,113))
        team = obj.get("team",-1)
        team_colour = TEAM_COLOURS.get(team,TEAM_COLOURS[1] if team>0 else TEAM_COLOURS[-1])
        if team>=0:
            colour = tuple(round(.75*a+.25*b) for a,b in zip(colour,team_colour))
        if fp["width_wu"]<=0 or fp["depth_wu"]<=0:
            draw.point((x,y),fill=(244,55,218))
            unresolved += 1
        elif cls in ("wall","fence"):
            length = max(fp["width_wu"],fp["depth_wu"])
            ux,uy = (co,si) if fp["width_wu"]>=fp["depth_wu"] else (-si,co)
            # Include original pivots and yaw for diagonal fence chains, then
            # let integer Bresenham lines choose the nearest pixel shape.
            a = snap((centre[0]-ux*length/2,centre[1]-uy*length/2))
            b = snap((centre[0]+ux*length/2,centre[1]+uy*length/2))
            draw.line((a,b),fill=colour,width=1)
        elif cls=="gate":
            draw.point((x,y),fill=GATE_COLOUR)
        elif cls=="turret":
            draw.point((x,y),fill=team_colour)
        elif cls=="tree":
            draw.point((x,y),fill=COLOURS["tree"])
            if max(fp["width_wu"],fp["depth_wu"])>=3*wu_per_pixel:
                draw.point((x-1,y),fill=COLOURS["tree"])
                draw.point((x+1,y),fill=COLOURS["tree"])
        else:
            left,top = x-pw//2,y-ph//2
            draw.rectangle((left,top,left+pw-1,top+ph-1),fill=colour)
    for marker in layout.get("objectives",[]):
        if marker.get("display") and not marker.get("hidden") and marker.get("cell"):
            x,y = snap(world_position(layout,marker))
            draw.point([(x-1,y),(x+1,y),(x,y-1),(x,y+1)],fill=GATE_COLOUR)
    if layout.get("player_start"):
        point = layout.get("player_start_wu")
        if point is None:
            point = world_position(layout,{"cell":layout["player_start"]})
        x,y = snap(point)
        draw.line([(x-1,y+1),(x,y-1),(x+1,y+1)],fill=TEAM_COLOURS[0],width=1)
    path = output_path(out)
    path.parent.mkdir(parents=True,exist_ok=True)
    ground.save(path,optimize=True)
    preview = path.with_stem(path.stem+"_4x")
    enlarged = ground.resize((width*VIEW_SCALE,height*VIEW_SCALE),Image.Resampling.NEAREST)
    enlarged.save(preview,optimize=True)
    metadata = {"mission":layout["name"],"install":install,"wu_per_pixel":wu_per_pixel,
                "crop_cells":[r0,c0,r1,c1],"map_pixels":[width,height],"header_pixels":0,
                "style":"quonset-pixel-1","north_up":True,"scale_overridden":overridden,
                "scale_reference":{"type_key":QUONSET_KEY,"long_side_wu":reference*2,"pixels_long":2},
                "png":path.name,"preview_png":preview.name,"preview_scale":VIEW_SCALE,
                "preview_pixels":[width*VIEW_SCALE,height*VIEW_SCALE],
                "unresolved_visible_footprints":unresolved,"hillshade":hillshade,
                "road_geometry":"world polylines" if layout.get("roads_wu") else "authored alpha masks",
                "terrain_palette":TERRAIN_COLOURS,"object_palette":COLOURS,"team_palette":TEAM_COLOURS,
                "symbols":{"wall":"1px line","gate":"gold pixel","turret":"team pixel",
                           "tree":"green pixel/cluster","player":"cyan chevron","objective":"gold diamond"}}
    write_json(path.with_suffix(".render.json"),metadata,pretty=True)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mission",nargs="?",type=Path)
    parser.add_argument("--out",type=Path)
    parser.add_argument("--wu-per-pixel",type=float)
    parser.add_argument("--crop",nargs=4,type=int,metavar=("R0","C0","R1","C1"))
    parser.add_argument("--base-crop",type=int)
    shading = parser.add_mutually_exclusive_group()
    shading.add_argument("--hillshade",action="store_true",help="subtle three-level height shading")
    shading.add_argument("--no-hillshade",action="store_false",dest="hillshade")
    parser.set_defaults(hillshade=False)
    parser.add_argument("--generated",action="store_true",help="re-render committed generated layouts")
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
                render(layout,path,args.wu_per_pixel,hillshade=args.hillshade)
                if row["file"] in ("stock/mc2_03.json","stock/mc2_17.json"):
                    render(layout,path.with_stem(path.stem+"_base"),args.wu_per_pixel,crop_base(layout),args.hillshade)
                print(path,flush=True)
    if args.generated:
        folder = args.out or Path("docs/mapgen/raster/generated")
        for source in sorted([*folder.glob("generated_*.json"),*folder.glob("generated_*.json.gz")]):
            if source.name.endswith(".render.json"):
                continue
            layout = load_json(source)
            path = folder/(layout["name"]+".png")
            render(layout,path,args.wu_per_pixel,hillshade=args.hillshade)
            render(layout,path.with_stem(path.stem+"_base"),args.wu_per_pixel,crop_base(layout),args.hillshade)
            print(path,flush=True)
    elif args.mission:
        layout = load_json(args.mission)
        crop = crop_base(layout,args.base_crop) if args.base_crop is not None else args.crop
        render(layout,args.out or args.mission.with_suffix(".png"),args.wu_per_pixel,crop,args.hillshade)
    elif not (args.all or args.examples):
        parser.error("provide a mission, --all, --examples or --generated")


if __name__ == "__main__":
    main()
