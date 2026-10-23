"""Emit stock missions through the ordinary PacketFile load path.

py -3 -m tools.mapgen.mission_writer layout.json --out G:/mapgen/missions
Accept stock assets, square 180..720 maps in multiples of 60 cells, and either
flat dry terrain or the low-relief suburban_forest recipe. Never rescale input.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import re
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from tools.mapgen.common import (CELL_WU, load_json, mask_of, output_path, packed_cell,
                                 world_cell, write_json)
from tools.mission_codec import Install, catalog, encode_objects, fit_sections, packet, overlay_catalog
from tools.mapgen.extract import VERTEX, extract_mission
from tools.mapgen.move_writer import build_move, validate_move
from tools.mapgen.placement import Spatial, covered_cells, overlaps, interlocking_crowns
from tools.mapgen.raster import polygon, render
from tools.pak_append import PacketRecord, make_zlib_payload, write_pak

STOCK = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')


def blocks(raw):
    text = raw.decode('latin1').replace('\r', '')
    return {name.lower(): f'[{name}]\n{body}' for name, body in
            re.findall(r'^\[([^]]+)\]\n(.*?)(?=^\[|\Z)', text, re.M | re.S)}


def set_field(block, name, value, kind=None):
    pattern = rf'(?im)^(\w+(?:\[\d+\])?\s+{re.escape(name)}\s*=\s*).*?$'
    if re.search(pattern, block):
        return re.sub(pattern, lambda m: m[1]+str(value), block)
    if kind is None:
        raise ValueError(f'missing template field {name}')
    return block.rstrip()+f'\n{kind} {name} = {value}\n'


def snapped(layout):
    result = copy.deepcopy(layout)
    side = result['size']['rows']
    # Terrain object arrays require whole 20-vertex blocks (three cells/vertex).
    if side != result['size']['cols'] or side % 60 or not 180 <= side <= 720:
        raise ValueError('supported mission sizes: square 180..720 cells, divisible by 60')
    if result['size']['vertices'] != side // 3:
        raise ValueError('size.vertices must equal the cell dimension / 3')
    if result['source'].get('asset_install') != 'stock':
        raise ValueError('mission writer requires stock native footprints')
    height = np.asarray(result['terrain']['elevation_wu'])
    biome = result['source'].get('biome') == 'suburban_forest'
    if height.shape != (side, side) or not np.isfinite(height).all():
        raise ValueError('terrain dimensions or finite-value check failed')
    if not biome and (np.ptp(height) > .001 or result['layers']['water']):
        raise ValueError('phase 2 writer requires flat dry input terrain')
    if biome and (np.ptp(height) > 32 or result['terrain']['water_elevation_wu'] is None):
        raise ValueError('suburban_forest requires low relief and an explicit water level')
    by_id, by_cell, spatial = {}, {}, Spatial()
    for obj in result['objects']:
        # Stock object loading accepts world pivots. Keep sub-cell placement;
        # quantize to 1/256 wu then serialize to the actual engine float32.
        obj['position_wu'] = [float(np.float32(round(v*256)/256))
                              for v in (*obj['position_wu'][:2], height[tuple(obj['cell'])])]
        obj['rotation'] = float(np.float32(obj['rotation']))
        obj['cell'] = world_cell(*obj['position_wu'][:2], side//3)
        cell = tuple(obj['cell'])
        if min(cell) < 0 or max(cell) >= side or cell in by_cell:
            raise ValueError(f"object outside map or ambiguous cell: {obj['id']}")
        if obj['id'] in by_id:
            raise ValueError('duplicate object ID')
        poly = polygon(obj, obj['footprint'], obj['position_wu'], rectangle=True)
        for other, opoly in spatial.candidates(poly):
            # Native wall segments intentionally share end caps/corners.
            if obj['class'] in ('wall', 'gate') and other['class'] in ('wall', 'gate'):
                continue
            if overlaps(poly, opoly) and not interlocking_crowns(obj, other):
                raise ValueError(f"snapped collision: {obj['id']} / {other['id']}")
        spatial.add(obj, poly)
        by_id[obj['id']] = obj
        by_cell[cell] = obj
    for obj in result['objects']:
        parent_id = obj.get('parent_link')
        parent = by_id.get(parent_id)
        expected = {'turret': 'turret_control', 'turret_control': 'turret_generator',
                    'gate': 'gate_control'}.get(obj['class'])
        if parent_id and parent is None or expected and (not parent or parent['class'] != expected):
            raise ValueError(f"invalid parent chain: {obj['id']}")
        if parent and obj['team'] != parent['team']:
            raise ValueError('parent team mismatch')
        obj['parent_cell'] = parent['cell'] if parent else None
        obj['parent_id_raw'] = packed_cell(parent['cell']) if parent else -1
    return result


def collision_grid(layout):
    """Conservative visual model rectangles; gates reserve a traversable opening.

    This is intentionally NOT claimed to be the stock destruction-aware collision
    template baker. Buildings/trees/walls stay blocked after destruction.
    """
    side = layout['size']['rows']
    blocked = mask_of(layout, 'water') | mask_of(layout, 'cliffs')
    for obj in layout['objects']:
        if obj['class'] == 'gate':
            continue
        poly = polygon(obj, obj['footprint'], obj['position_wu'], rectangle=True)
        r0, c0, r1, c1 = covered_cells(layout, poly)
        # Stamp cells intersecting the native rectangle, not its entire AABB.
        for r in range(max(0, r0), min(side, r1)):
            for c in range(max(0, c0), min(side, c1)):
                x, y = (c-side/2)*CELL_WU, (side/2-r)*CELL_WU
                square = [(x, y), (x+CELL_WU, y), (x+CELL_WU, y-CELL_WU), (x, y-CELL_WU)]
                if overlaps(poly, square):
                    blocked[r, c] = True
    blocked[[0, -1], :] = True
    blocked[:, [0, -1]] = True
    return blocked


def place_units(layout, blocked):
    occupied = set()
    units = []
    for part in layout['parts']:
        # A three-mech player lance, two defenders per enemy site.
        for offset in range(3 if part['player'] else 2):
            r, c = part['cell']
            candidates = sorted(((rr, cc) for rr in range(max(2,r-14), min(len(blocked)-2,r+15))
                                 for cc in range(max(2,c-14), min(len(blocked)-2,c+15))),
                                key=lambda p: ((p[0]-r)**2+(p[1]-c-offset*3)**2, p))
            cell = next((p for p in candidates if not blocked[p[0]-1:p[0]+2,p[1]-1:p[1]+2].any()
                         and all(abs(p[0]-a)+abs(p[1]-b) >= 3 for a,b in occupied)), None)
            if cell is None:
                raise ValueError('no collision-free unit spawn near requested part')
            occupied.add(cell)
            rr, cc = cell
            pos = [(cc+.5-len(blocked)/2)*CELL_WU, (len(blocked)/2-rr-.5)*CELL_WU,
                   layout['terrain']['elevation_wu'][rr][cc]]
            units.append({**part, 'id': f'part{len(units)+1}', 'cell': list(cell),
                          'position_wu': pos, 'type_name': 'Bushwacker' if part['player'] else 'UrbanMech'})
    layout['parts'] = units
    layout['player_start'] = units[0]['cell']
    layout['player_start_wu'] = units[0]['position_wu']
    # Verify each spawn and a dry cell adjacent to each target is reachable.
    start = tuple(units[0]['cell'])
    from tools.mapgen.move_writer import distances
    reachable = distances(np.where(blocked, -1, 0), start, 0)
    if any(tuple(p['cell']) not in reachable for p in units):
        raise ValueError('unit spawn disconnected from player')
    for marker in layout['objectives']:
        target = next((o for o in layout['objects'] if o['cell'] == marker['cell']), None)
        if target is None:
            raise ValueError('objective target is missing')
        r0, c0, r1, c1 = covered_cells(layout, polygon(target, target['footprint'],
                                                      target['position_wu'], rectangle=True))
        approaches = ((r,c) for r in range(max(1,r0-3), min(len(blocked)-1,r1+3))
                      for c in range(max(1,c0-3), min(len(blocked)-1,c1+3))
                      if not (r0 <= r < r1 and c0 <= c < c1))
        if not any(cell in reachable for cell in approaches):
            raise ValueError(f"objective has no reachable attack approach: {marker['id']}")
    return units


def terrain_packet(layout, stock_pak):
    side = layout['size']['vertices']
    source = np.frombuffer(packet(stock_pak, 0), VERTEX)
    # Reuse an actual stock grass texture record, with no overlay/water.
    choices = source[(source['type'] == 2) & ((source['texture'] >> 16) == 65535)
                     & ((source['water'] & 1) == 0)]
    if not len(choices):
        raise ValueError('stock grass texture sample missing')
    vertices = np.empty((side, side), VERTEX)
    vertices[:] = choices[0]
    vertices['normal'] = (0, 0, 1)
    vertices['height'] = layout['terrain']['elevation_wu'][0][0]
    for field in ('water', 'selected', 'shadow', 'highlight'):
        vertices[field] = 0
    if layout['source'].get('biome') == 'suburban_forest':
        height = np.asarray(layout['terrain']['elevation_wu'])[::3, ::3]
        vertices['height'] = height
        dr, dc = np.gradient(height, 128.)
        normal = np.stack((-dc, dr, np.ones_like(height)), axis=-1)
        vertices['normal'] = normal / np.linalg.norm(normal, axis=-1, keepdims=True)
        vertices['type'] = np.asarray(layout['terrain']['terrain_type'])[::3, ::3]
        vertices['water'] = mask_of(layout, 'water')[::3, ::3].astype(np.uint8)
        # MapData::calcTransitions resolves the low handle from the four native
        # terrain types on ordinary load. No opaque stock blend handle is reused.
        vertices['texture'] = 0xffff0000
    # Legacy flat layouts retain their actual known-valid stock grass handle.
    return vertices.tobytes()


def tacmap_packet(layout):
    side = layout['size']['rows']
    if layout['source'].get('biome') == 'suburban_forest':
        from tools.mapgen.suburban_forest import colours
        image = Image.fromarray(colours(layout)).resize((128, 128), Image.Resampling.BOX)
    else:
        image = Image.new('RGB', (128, 128), (85, 105, 58))
    draw = ImageDraw.Draw(image)
    for obj in layout['objects']:
        r, c = obj['cell']
        x, y = round(c/side*127), round(r/side*127)
        colour = (25, 65, 25) if obj['class'] == 'tree' else (160, 55, 40) if obj['team'] == 1 else (150, 150, 140)
        draw.rectangle((x-1, y-1, x+1, y+1), fill=colour)
    import io
    stream = io.BytesIO()
    image.save(stream, format='TGA')
    return stream.getvalue()


def mission_fit(layout, template):
    original = blocks(template)
    keep = ('cameras', 'water', 'terrain', 'artillery', 'missionsettings', 'weather', 'thesky', 'music')
    selected = {k: original[k] for k in keep}
    x, y, _ = layout['player_start_wu']
    for k, v in (('PositionX', x), ('PositionY', y), ('StartRotation', 0), ('NewScale', .6)):
        selected['cameras'] = set_field(selected['cameras'], k, v)
    half = layout['size']['vertices']*64
    for k, v in (('TerrainMinX', -half), ('TerrainMinY', half)):
        selected['terrain'] = set_field(selected['terrain'], k, v)
    selected['water'] = set_field(selected['water'], 'Elevation',
        layout['terrain'].get('water_elevation_wu') or -10000)
    if layout['source'].get('biome') == 'suburban_forest':
        selected['water'] = set_field(selected['water'], 'ShallowDepth', 4)
        selected['water'] = set_field(selected['water'], 'Ampliture', 1)
    for k, v in (('MissionName', f'"{layout["name"]}"'), ('MissionNameUseResourceString', 'FALSE'),
                 ('BlurbUseResourceString', 'FALSE'), ('Blurb2UseResourceString', 'FALSE')):
        selected['missionsettings'] = set_field(selected['missionsettings'], k, v)
    sections = ['FITini\n', *selected.values()]
    if layout['source'].get('biome') == 'suburban_forest':
        sections.append(f'[ColorMap]\nst ColorMapName = "{layout["name"]}"\n')
    n = len(layout['parts'])
    sections += [f'[Warriors]\nul NumWarriors = {n}\n', f'[Parts]\nul NumParts = {n}\n', '[Teams]\n']
    for commander in (0, 1):
        ids = [i for i,p in enumerate(layout['parts'], 1) if p['team'] == commander]
        for group in range((len(ids)+11)//12):
            mates = ids[group*12:group*12+12]
            sections.append(f'[Commander{commander}Group:{group}]\nl[12] Mates = '+
                            ','.join(map(str, mates+[0]*(12-len(mates))))+',\n')
    for i, unit in enumerate(layout['parts'], 1):
        player = unit['player']
        profile, brain = ('pmwhacksaw', 'PBrain') if player else ('PMWRegular', 'SBGuard')
        sections.append(f'[Warrior{i}]\nst Profile = "{profile}"\nst Brain = "{brain}"\nl NumCells = 0\nl NumStaticVars = 0\n')
        part = re.sub(r'^\[Part\d+\]', f'[Part{i}]', original['part2' if player else 'part6'])
        values = {'Pilot': i, 'PositionX': unit['position_wu'][0], 'PositionY': unit['position_wu'][1],
                  'SquadNum': i, 'AlternativeStartIndex': 0}
        for k,v in values.items():
            part = set_field(part, k, v)
        sections.append(part)
    targets = [o for o in layout['objects'] if o['team'] == 1 and o['class'] == 'building']
    target = targets[0]
    # Native structure lookup is by exact world position in objective.cpp.
    tx, ty, _ = target['position_wu']
    base = original['team0objective1']
    for k,v in {'Title':'"Destroy the enemy base"', 'Description':'"Destroy the marked base structure."',
                'TitleUseResourceString':'FALSE', 'DescriptionUseResourceString':'FALSE',
                'ActivateOnFlag':'FALSE', 'NumActions':0, 'MarkerX':tx, 'MarkerY':ty}.items():
        base = set_field(base, k, v)
    base = base.replace('[Team0Objective1]', '[Team0Objective0]')
    condition = original['team0objective1condition0'].replace('[Team0Objective1Condition0]', '[Team0Objective0Condition0]')
    for k,v in {'PositionX':tx, 'PositionY':ty, 'CellX':target['cell'][0], 'CellY':target['cell'][1]}.items():
        condition = set_field(condition,k,v)
    sections += ['[Team0Objectives]\nul NumObjectives = 1\n', base, condition]
    sections += [f'[Team{t}Objectives]\nul NumObjectives = 0\n' for t in range(1,8)]
    sections += [f'[Player{t}]\nl DefaultTeam = {1 if t == 1 else 0}\n' for t in range(8)]
    # Objective and defeat evaluation live in Mission::updateTermination. A
    # minimal script keeps the normal ABL load path free of missing-file errors.
    sections += [f'[Script]\nst ScenarioScript = "{layout["name"]}"\n', 'FITend\n']
    layout['objectives'] = [{'id':'team0objective0', 'title':'Destroy the enemy base',
                             'cell': target['cell'], 'position_wu':target['position_wu'], 'display':True}]
    return '\n'.join(sections).encode('latin1')


def emit(layout, source, out, name=None):
    out = output_path(out, [source.root])
    out.mkdir(parents=True, exist_ok=True)
    layout = snapped(layout)
    from tools.mapgen.validate import validate
    errors = validate(layout, load_json(Path(__file__).with_name('profile.json')))
    if errors:
        raise ValueError('snapped layout validation: '+ '; '.join(errors[:5]))
    if name:
        layout['name'] = name
    if not re.fullmatch(r'[a-zA-Z0-9_]{1,60}', layout['name']):
        raise ValueError('invalid mission stem')
    blocked = collision_grid(layout)
    place_units(layout, blocked)
    template_pak, pak_source = source.read('data/missions/mc2_01.pak')
    template_fit, fit_source = source.read('data/missions/mc2_01.fit')
    for dependency in ('pmwhacksaw.fit', 'pmwregular.fit', 'pbrain.abl', 'sbguard.abl'):
        if not source.has('data/missions/warriors/'+dependency):
            raise ValueError(f'missing stock pilot/brain: {dependency}')
    if layout.get('batteries'):
        from tools.mapgen.fields_of_fire import bake, Assets
        fire_assets = Assets(source)
        cells = bake(layout, fire_assets).cells.copy()
    else:
        cells = np.where(blocked, 2, 0x402).astype('<u4')
    cells[mask_of(layout, 'roads')] |= 0x02000000
    cells[mask_of(layout, 'water')] |= 0x08000000
    cells[[0,-1], :] |= 0x200
    cells[:, [0,-1]] |= 0x200
    movement = build_move(cells)
    move_summary = validate_move(movement, layout['size']['rows'])
    records = [(o['type_id'], *o['position_wu'], o['rotation'], o.get('damage',0),
                o['team'], o['parent_id_raw'], 0, 0) for o in layout['objects']]
    packets = [terrain_packet(layout, template_pak), encode_objects(records),
               struct.pack('<i',0), tacmap_packet(layout), *movement]
    if layout.get('batteries'):
        from tools.mapgen.fields_of_fire import MoveLOS, evaluate
        from tools.mapgen.extract import VERTEX
        vertices = np.frombuffer(packets[0], VERTEX)['height'].reshape(layout['size']['vertices'], -1)
        layout['fields_of_fire'] = evaluate(layout, MoveLOS(vertices, cells, fire_assets.tree_limit,
            layout['terrain'].get('water_elevation_wu')), fire_assets)
        if layout['fields_of_fire']['status'] in ('FAIL', 'INCOMPLETE'):
            raise ValueError('serialized fields of fire: '+ '; '.join(
                layout['fields_of_fire']['errors']+layout['fields_of_fire']['incomplete']))
    stem = out/layout['name']
    fit = mission_fit(layout, template_fit)
    write_pak([PacketRecord(i,4,make_zlib_payload(p)) for i,p in enumerate(packets)], stem.with_suffix('.pak'))
    stem.with_suffix('.fit').write_bytes(fit)
    script = f'module {layout["name"]} : integer;\ncode\n    return(0);\nendmodule.\n'
    stem.with_suffix('.abl').write_text(script, encoding='ascii')
    if layout.get('batteries'):
        # Pilot skill is type data, absent from the 40-byte mission record.
        # Emit an explicit companion catalog for this generated mission install.
        object2 = source.read('data/objects/object2.pak')[0]
        count = struct.unpack_from('<I', object2, 4)[0]//4-2
        skills = {o['type_id']: o['pilot_skill'] for o in layout['objects'] if 'pilot_skill' in o}
        catalog_packets = []
        for i in range(count):
            data = packet(object2, i)
            if i in skills:
                data, n = re.subn(rb'(l\s+PilotSkill\s*=\s*)\d+',
                                 lambda m: m[1]+str(skills[i]).encode(), data, flags=re.I)
                if n != 1:
                    raise ValueError(f'cannot set pilot skill for type {i}')
            catalog_packets.append(PacketRecord(i, 4, make_zlib_payload(data)))
        write_pak(catalog_packets, out/'object2.pak')
    colormap = None
    if layout['source'].get('biome') == 'suburban_forest':
        from tools.mapgen.suburban_forest import cook_colormap
        colormap = cook_colormap(layout, out/'textures'/(layout['name']+'.tga'),
                                 fit_sections(fit)['cameras'])
    # Read every packet using the repo's strict decoder, not just our encoder.
    raw = stem.with_suffix('.pak').read_bytes()
    for i,p in enumerate(packets):
        if packet(raw,i) != p:
            raise ValueError(f'packet roundtrip failed: {i}')
    parsed_fit = fit_sections(fit)
    if parsed_fit['parts']['numparts'] != len(layout['parts']):
        raise ValueError('FIT parts count mismatch')
    types, provenance = catalog(source)
    overlays, overlay_source = overlay_catalog(source)
    # The ordinary extractor reads the loose output while resolving stock assets.
    probe_root = out/'roundtrip'
    mission_folder = probe_root/'data/missions'
    mission_folder.mkdir(parents=True,exist_ok=True)
    import shutil
    for suffix in ('.pak','.fit'):
        shutil.copyfile(stem.with_suffix(suffix), mission_folder/(layout['name']+suffix))
    decoded = extract_mission(Install(probe_root, fallback=source), layout['name'], types,
                              provenance, overlays, overlay_source, 'generated')
    if decoded['warnings'] or len(decoded['objects']) != len(records):
        raise ValueError(f"extraction roundtrip failed: {decoded['warnings']}")
    for o, read in zip(layout['objects'], decoded['objects']):
        if o['cell'] != read['cell'] or o['parent_id_raw'] != read['parent_id_raw']:
            raise ValueError('object cell/parent roundtrip mismatch')
    report = {'mission':layout['name'], 'cells':layout['size']['rows'], 'objects':len(records),
              'units':len(layout['parts']), 'move':move_summary, 'packet_count':len(packets),
              'source_pak':pak_source, 'source_fit':fit_source,
              'source_pak_sha256':hashlib.sha256(template_pak).hexdigest(),
              'pak_sha256':hashlib.sha256(raw).hexdigest(), 'fit_sha256':hashlib.sha256(fit).hexdigest(),
              'abl_sha256':hashlib.sha256(stem.with_suffix('.abl').read_bytes()).hexdigest(),
              'validation':'all packets + FIT + extraction + parent links + spawn/objective connectivity passed',
              'limitations':([
                             'dry causeways reserve river crossings; no native bridge geometry',
                             'ground and hover share a conservative water-blocked graph'] if colormap else [
                             'flat grass terrain; planned roads/cement are not texture-baked']) + [
                             'conservative model rectangles; no destruction-aware special areas',
                             'gates use passable apertures; native collision-template baker pending']}
    if layout.get('batteries'):
        report['fields_of_fire'] = layout['fields_of_fire']
        report['turret_catalog'] = {'file': 'object2.pak', 'install_path': 'data/objects/object2.pak',
            'sha256': hashlib.sha256((out/'object2.pak').read_bytes()).hexdigest(),
            'pilot_skills': skills,
            'scope': 'type-wide companion catalog; use only in the generated mission install'}
    if colormap:
        report['colormap'] = {'path': 'data/textures/'+colormap.name,
            'sha256': hashlib.sha256(colormap.read_bytes()).hexdigest(),
            'lighting': 'editable albedo plus offline terrain illumination; runtime object shadows',
            'burnin_path': 'data/textures/'+colormap.with_suffix('.burnin.tga').name,
            'burnin_sha256': hashlib.sha256(colormap.with_suffix('.burnin.tga').read_bytes()).hexdigest()}
    write_json(stem.with_suffix('.mission.json'),report,pretty=True)
    write_json(stem.with_suffix('.layout.json'),layout)
    render(layout,stem.with_suffix('.plan.png'))
    if colormap:
        from tools.mapgen.suburban_forest import feature_mask
        feature_mask(layout, stem.with_suffix('.features.png'))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout',type=Path)
    parser.add_argument('--source',type=Path,default=STOCK)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--name')
    args = parser.parse_args()
    print(emit(load_json(args.layout),Install(args.source),args.out,args.name))


if __name__ == '__main__':
    main()
