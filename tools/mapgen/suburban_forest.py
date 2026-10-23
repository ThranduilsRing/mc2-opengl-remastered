"""Temperate parcels, corridor settlements and forests in the shared layout schema.

Recipes borrow landscape organization, never reference-game assets or dimensions.
All geometry is in native MC2 cells; one cell is 128/3 wu (about 8.5 m).
"""
from __future__ import annotations

import copy
import hashlib
import json
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from tools.mapgen.common import (CELL_WU, distance_field, mask_of, output_path,
                                 slope_grid, spans)

FIELD_COLOURS = ((133, 146, 83), (157, 150, 96), (102, 132, 72),
                 (126, 115, 76), (157, 165, 109), (104, 144, 96))
MATERIALS = Path(__file__).with_name('materials')
VEGETATION_BUDGET = 60000
FEATURE_COLOURS = (("forest_edge", (65, 106, 62)), ("forest_core", (35, 76, 47)),
                   ("clearings", (132, 157, 104)), ("field_borders", (45, 87, 48)),
                   ("settlements", (150, 141, 119)), ("river_banks", (153, 143, 101)),
                   ("ditch", (53, 70, 43)), ("verge", (105, 140, 65)),
                   ("shoulder", (162, 151, 120)), ("roads", (102, 103, 98)),
                   ("cement", (145, 147, 140)), ("water", (48, 99, 135)))


def plan(size, seed, profile):
    """Road-first native sites, then a river reservation and bounded land uses."""
    from tools.mapgen.generate import nearest, plan_cells, route

    if size % 60 or not 180 <= size <= 720:
        raise ValueError("suburban_forest requires 180..720 cells divisible by 60")
    rng = random.Random(seed ^ 0x53554255)
    recipe_profile = copy.deepcopy(profile)
    stats = recipe_profile['groups']['stock']
    for name, entry in stats['archetypes'].items():
        entry['count'] = {'town': 8, 'industrial': 2, 'outpost': 1,
                          'walled_compound': 1}.get(name, 0)
    layout = plan_cells(size, seed, np.full((size, size), 1000.), profile=recipe_profile)
    layout['name'] = f'suburban_forest_{size}_{seed}'
    layout['source'].update(biome='suburban_forest', profile_sha256=hashlib.sha256(
        json.dumps(profile, sort_keys=True).encode()).hexdigest())
    roads = mask_of(layout, 'roads')
    rd = distance_field(roads)
    settlements = np.zeros_like(roads)
    for base in layout['bases']:
        r0, c0, r1, c1 = base['bounds']
        settlements[r0:r1, c0:c1] = True
        # Neutral villages and town-edge industry replace extra military sites.
        if base['archetype'] in ('town', 'industrial'):
            base['team'] = -1
            for obj in layout['objects']:
                if obj.get('base_id') == base['id']:
                    obj['team'] = -1
        base['region'] = 'village' if base['archetype'] == 'town' else base['archetype']
    neutral = {b['id'] for b in layout['bases'] if b['team'] == -1}
    layout['parts'] = [p for p in layout['parts'] if p['id'].removeprefix('defenders_') not in neutral]
    layout['objectives'] = [o for o in layout['objectives']
                            if o['title'].split()[-1] not in neutral]
    # Frontage follows the access street on either side, with gardens behind.
    for base in layout['bases']:
        if base['archetype'] != 'town':
            continue
        r0, c0, r1, c1 = base['bounds']
        gr, gc = base['entry_cell']
        horizontal = gr not in (r0, r1-1)
        slots = ([(gr+d, c) for c in range(c0+3, c1-3, 4) for d in (-4, 4)] if horizontal else
                 [(r, gc+d) for r in range(r0+3, r1-3, 4) for d in (-4, 4)])
        slots = [p for p in slots if r0+2 <= p[0] < r1-2 and c0+2 <= p[1] < c1-2 and not roads[p]]
        members = [o for o in layout['objects'] if o.get('base_id') == base['id'] and o['class'] == 'building']
        for obj, cell in zip(members, slots):
            obj.update(cell=list(cell), rotation=0 if horizontal else 90, frontage=True)

    rr, cc = np.mgrid[:size, :size]
    sd = distance_field(settlements)
    # A continuous reservation bends around sites; roads cross on dry causeways.
    centre = size*(rng.uniform(.20, .30)+.045*np.sin(rr/size*9+rng.random()*6))
    river_allowed = sd >= 7
    start = nearest(river_allowed & (rr == 0), (0, centre[0, 0]))
    end = nearest(river_allowed & (rr == size-1), (size-1, centre[-1, 0]))
    river_path = route(start, end, river_allowed, np.abs(cc-centre)*.16)
    centreline = np.zeros_like(roads)
    for p in river_path:
        centreline[p] = True
    river = distance_field(centreline) <= max(3, round(size*.009))
    crossing = river & (rd <= 6)
    wet = river & (rd > 6) & (sd >= 4)
    # Author at native vertex resolution so packet water and placement agree.
    wet = np.repeat(np.repeat(wet[::3, ::3], 3, 0), 3, 1)
    bank = (distance_field(wet) <= 3) & ~wet & (rd > 3) & ~settlements

    # Smoothly varying forest membership, with an explicit clearing network.
    field = np.zeros((size, size), dtype=float)
    for _ in range(12):
        r, c = rng.uniform(0, size), rng.uniform(0, size)
        ar, ac = rng.uniform(.07, .19)*size, rng.uniform(.08, .22)*size
        field += rng.uniform(.5, 1.2)*np.exp(-((rr-r)/ar)**2-((cc-c)/ac)**2)
    field += .055*np.sin(rr*.19+cc*.11)+.035*np.cos(cc*.33-rr*.17)
    eligible = (rd >= 4) & (sd >= 4) & ~wet & ~bank
    forest = (field >= np.quantile(field[eligible], .62)) & eligible
    clearing = np.zeros_like(roads)
    for group in range(6):
        candidates = np.argwhere(forest)
        if not len(candidates):
            break
        r, c = candidates[rng.randrange(len(candidates))]
        clearing |= ((rr-r)/(size*.023))**2+((cc-c)/(size*.035))**2 < 1
    clearing &= forest
    forest &= ~clearing
    edge = forest & (distance_field(~forest) <= 3)
    core = forest & ~edge

    # Recursive bounded rectangles keep field direction, tint and borders stable.
    ids = np.zeros((size, size), np.int32)
    borders = np.zeros_like(roads)
    parcels = []
    usable = ~forest & ~clearing & ~wet & ~bank & ~settlements & (rd > 3)
    pending = [(0, 0, size, size)]
    while pending:
        r0, c0, r1, c1 = pending.pop()
        h, w = r1-r0, c1-c0
        if max(h, w) > 41:
            vertical = w > h if abs(w-h) > 12 else rng.random() < .5
            length = w if vertical else h
            split = rng.randint(max(12, length//3), min(length-12, length*2//3))
            if vertical:
                pending.extend(((r0, c0, r1, c0+split), (r0, c0+split, r1, c1)))
            else:
                pending.extend(((r0, c0, r0+split, c1), (r0+split, c0, r1, c1)))
            continue
        selected = usable[r0:r1, c0:c1]
        if selected.sum() < 30:
            continue
        ident = len(parcels)+1
        tint = rng.randrange(len(FIELD_COLOURS))
        ids[r0:r1, c0:c1][selected] = ident
        rim = np.zeros((h, w), bool)
        rim[[0, -1], :] = True
        rim[:, [0, -1]] = True
        borders[r0:r1, c0:c1] |= rim & selected
        parcels.append({'id': ident, 'bounds': [r0, c0, r1, c1], 'recipe': 'field_hedgerow',
                        'tint': tint, 'uv_direction_deg': rng.choice((0, 90)),
                        'material': 'farm_furrows' if tint in (1, 3) else ('grass', 'crop', 'meadow')[tint % 3],
                        'tile_metres': 32 if tint in (1, 3) else 24,
                        'terrain_type': (9, 3, 4, 2, 9, 8)[tint]})
    zones = np.zeros_like(roads)
    for obj in layout['objectives']:
        r, c = obj['cell']
        zones[max(0,r-6):r+7, max(0,c-6):c+7] = True
    r, c = layout['player_start']
    zones[max(0,r-6):r+7, max(0,c-6):c+7] = True
    exclusion = (rd < 4) | (sd < 4) | (distance_field(wet) < 4) | zones | clearing
    tree_lines = (rd == 6) & (sd >= 5) & ~wet & ~bank & ~clearing
    layers = {'water': wet, 'river_banks': bank, 'crossings': crossing & ~wet,
              'settlements': settlements, 'forest_core': core, 'forest_edge': edge,
              'clearings': clearing, 'field_borders': borders,
              'tree_lines': tree_lines,
              'shoulder': (rd == 1) & ~wet & ~settlements,
              'verge': (rd == 2) & ~wet & ~settlements,
              'ditch': (rd == 3) & ~wet & ~settlements,
              'objective_zones': zones, 'vegetation_exclusion': exclusion}
    layout['layers'].update({k: spans(v) for k, v in layers.items()})
    heights = np.full((size, size), 1000.)
    heights[bank] = 993
    heights[wet] = 976
    heights[layers['ditch']] = 997
    types = np.full((size, size), 9, np.uint8)
    for parcel in parcels:
        types[ids == parcel['id']] = parcel['terrain_type']
    types[core | edge | borders] = 8
    types[bank | layers['shoulder']] = 4
    types[roads | mask_of(layout, 'cement')] = 10
    types[wet] = 0
    layout['terrain'].update(elevation_wu=heights.tolist(),
        slope_deg=np.round(slope_grid(heights), 3).tolist(), terrain_type=types.tolist(),
        water_elevation_wu=984.)
    layout['source']['heightfield_sha256'] = hashlib.sha256(
        heights.astype('<f8').tobytes()+wet.tobytes()).hexdigest()
    layout['regions'] = {'recipe_version': 2, 'biome': 'suburban_forest',
        'parcel_ids': ids.tolist(), 'parcels': parcels,
        'river_centreline': [list(p) for p in river_path],
        'crossing_recipe': 'dry causeway; bridge geometry reserved for native baker',
        'road_recipe': {'surface': 'stock paved', 'shoulder_cells': 1,
                        'verge_cells': 1, 'ditch_cells': 1, 'vegetation_clearance_cells': 4}}
    # Decoration RNG is independent of site generation and never touches sim RNG.
    layout['objects'] = [o for o in layout['objects'] if o['class'] != 'tree']
    candidates = (core | edge | borders | tree_lines) & ~exclusion
    vegetation = []
    for r in range(1, size-1):
        for c in range(1, size-1):
            if not candidates[r, c] or rng.random() > (.98 if core[r,c] else .50 if edge[r,c] else .98):
                continue
            vegetation.append({'id': f'vegetation_{r}_{c}', 'class': 'tree', 'cell': [r, c],
                'team': -1, 'rotation': 0, 'damage': 0, 'parent_link': None,
                'parent_cell': None, 'parent_id_raw': -1,
                'vegetation_recipe': 'hedgerow' if borders[r,c] else 'tree_line' if tree_lines[r,c] else
                                     'forest_core' if core[r,c] else 'forest_edge'})
    # Preserve continuous border/road runs first; distribute any core thinning
    # across the whole map instead of truncating one side of a large forest.
    lines = [o for o in vegetation if o['vegetation_recipe'] in ('hedgerow', 'tree_line')]
    woods = [o for o in vegetation if o['vegetation_recipe'] not in ('hedgerow', 'tree_line')]
    if len(lines) > VEGETATION_BUDGET:
        lines = rng.sample(lines, VEGETATION_BUDGET)
    woods = rng.sample(woods, min(len(woods), VEGETATION_BUDGET-len(lines)))
    layout['objects'].extend(lines+woods)
    layout['regions']['vegetation_budget'] = VEGETATION_BUDGET
    return layout


def colours(layout):
    """One semantic preview used by the parcel mask and raster/tacmap views."""
    side = layout['size']['rows']
    rgb = np.full((side, side, 3), (114, 137, 88), np.uint8)
    ids = np.asarray(layout['regions']['parcel_ids'])
    for parcel in layout['regions']['parcels']:
        rgb[ids == parcel['id']] = FIELD_COLOURS[parcel['tint']]
    for layer, colour in FEATURE_COLOURS:
        rgb[mask_of(layout, layer)] = colour
    return rgb


def feature_mask(layout, out):
    """Show parcel identity, actual native objects, crossings and objective zones."""
    side = layout['size']['rows']
    image = Image.fromarray(colours(layout)).resize((side*3, side*3), Image.Resampling.NEAREST)
    draw = ImageDraw.Draw(image)
    for obj in layout['objects']:
        r, c = obj['cell']
        if obj['class'] != 'tree':
            draw.rectangle((c*3-1, r*3-1, c*3+2, r*3+2), fill=(220, 211, 186))
    for obj in layout['objectives']:
        r, c = obj['cell']
        draw.ellipse((c*3-12, r*3-12, c*3+12, r*3+12), outline=(255, 104, 62), width=2)
    draw.rectangle((0, 0, side*3, 24), fill=(25, 33, 29))
    draw.text((6, 6), f"{layout['name']} | fields + hedges | forest core/edge + clearings | road/verge/ditch | river/banks | orange=objective", fill='white')
    path = output_path(out)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def cook_colormap(layout, out, lighting):
    """Keep editable albedo plus an offline-lit stock loader sidecar.

    Raw-TGA runtime burnin needs a camera before mission camera initialization.
    Generated missions therefore ship their illumination like stock burnin maps.
    Object shadows remain owned by the runtime shadow renderer.
    """
    source_side = layout['size']['rows']
    side = min(4096, 2**int(np.ceil(np.log2(source_side*8))))
    rgb = np.asarray(Image.fromarray(colours(layout)).resize((side, side), Image.Resampling.NEAREST)).astype(np.float32)
    index = np.minimum((np.arange(side)*source_side/side).astype(int), source_side-1)
    ids = np.asarray(layout['regions']['parcel_ids'])[index[:,None], index[None,:]]
    metres = np.arange(side, dtype=np.float32)*source_side*CELL_WU/(side*5.01)
    yy, xx = metres[:,None], metres[None,:]
    # Fine grass/soil texture and broad irregular growth stay subordinate to
    # the exact parcel mask. No blur across parcel or corridor boundaries.
    grain = .95+.035*np.sin(xx*2.7+yy*1.9)+.025*np.sin(xx*.39-yy*.63)
    rgb *= grain[:,:,None]
    pitch, yaw = np.radians([lighting['lightdirpitch'], lighting['lightdiryaw']+45])
    sun = np.array([np.cos(pitch)*np.cos(yaw), np.cos(pitch)*np.sin(yaw), np.sin(pitch)], np.float32)
    detail_sun = np.full((side,side), sun[2], np.float32)
    diffuse = np.asarray(Image.open(MATERIALS/'farm_furrows_diff.jpg'), dtype=np.float32)
    normals = np.asarray(Image.open(MATERIALS/'farm_furrows_nor_gl.jpg'), dtype=np.float32)/127.5-1
    normals /= np.maximum(np.linalg.norm(normals, axis=-1, keepdims=True), .001)
    ao = np.asarray(Image.open(MATERIALS/'farm_furrows_arm.jpg'), dtype=np.float32)[:,:,0]/255
    for parcel in layout['regions']['parcels']:
        r0,c0,r1,c1 = parcel['bounds']
        r0,c0 = int(np.ceil(r0*side/source_side)),int(np.ceil(c0*side/source_side))
        r1,c1 = int(np.ceil(r1*side/source_side)),int(np.ceil(c1*side/source_side))
        selected = ids[r0:r1,c0:c1] == parcel['id']
        horizontal = parcel['uv_direction_deg'] == 0
        u,v = np.broadcast_arrays(xx[:,c0:c1],yy[r0:r1,:])
        if not horizontal:
            u,v = v,-u
        if parcel['material'] == 'farm_furrows':
            ti = (np.floor(v/parcel['tile_metres']*512).astype(int)+parcel['id']*37)%512
            tj = (np.floor(u/parcel['tile_metres']*512).astype(int)+parcel['id']*61)%512
            colour = diffuse[ti,tj]*1.35
            # GL normals rotate with the parcel's UV frame. ARM blue is
            # metallic, never height; only its AO channel enters this cook.
            n = normals[ti,tj]
            nx,ny = (n[:,:,0],-n[:,:,1]) if horizontal else (-n[:,:,1],-n[:,:,0])
            light = np.maximum(nx*sun[0]+ny*sun[1]+n[:,:,2]*sun[2],0)
            light *= .85+.15*ao[ti,tj]
            detail_sun[r0:r1,c0:c1][selected] = light[selected]
        else:
            rows = .93+.07*np.cos(v/(3.5 if parcel['material']=='crop' else 8)*np.pi*2)
            variation = .96+.04*np.sin(u*.081+np.sin(v*.047)*2)
            colour = rgb[r0:r1,c0:c1]*(rows*variation)[:,:,None]
        rgb[r0:r1,c0:c1][selected] = colour[selected]
    rgb = np.clip(rgb,0,255).astype(np.uint8)
    path = output_path(out)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb).save(path, format='TGA', compression=None)
    dr, dc = np.gradient(np.asarray(layout['terrain']['elevation_wu']), CELL_WU)
    normal = np.stack((-dc, dr, np.ones_like(dr)), axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    intensity = np.maximum(normal @ sun, 0)[index[:,None], index[None,:]]
    intensity = np.clip(intensity+detail_sun-sun[2], 0, 1)
    direct = np.array([lighting['light'+channel] for channel in ('red','green','blue')])
    ambient = np.array([lighting['ambient'+channel] for channel in ('red','green','blue')])
    illumination = np.clip(ambient+intensity[:,:,None]*direct, 0, 255)/255
    baked = (rgb*illumination).astype(np.uint8)
    Image.fromarray(baked).save(path.with_suffix('.burnin.tga'), format='TGA', compression=None)
    return path
