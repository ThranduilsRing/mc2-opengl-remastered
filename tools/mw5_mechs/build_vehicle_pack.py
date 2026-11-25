"""Cook assembled MW5 vehicles into ground overrides and imported VTOL appearances."""
import argparse
import csv
import io
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline.cook import cook
from tools.content_pipeline import ir
from tools.content_pipeline.frontends import gltf_file
from tools.content_pipeline.gltf_io import load_document
from tools.mw5_mechs.build_pack import emit_ini, blocks


def write_glb(path, doc, binary):
    text = json.dumps(doc, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 28 + len(text) + len(binary))
                     + struct.pack('<II', len(text), 0x4e4f534a) + text
                     + struct.pack('<II', len(binary), 0x004e4942) + binary)


def material_aliases(materials):
    # Match the static cooker's authored-input deduplication. Different names
    # can share a cooked slot, including names that have no common prefix.
    seen, aliases = {}, {}
    for material in materials:
        values = {k: ([v.sha256, v.role, v.passthrough] if isinstance(v, ir.TextureRef) else v)
                  for k, v in material.__dict__.items() if k not in ('name', 'review_note')}
        key = json.dumps(values, sort_keys=True)
        aliases[material.name] = seen.setdefault(key, material.name)
    return aliases


def logistics(vehicles, stock, out, assemblies):
    from tools.pak_dump import read_packets, decode_packet
    from tools.mc2x_import import fst
    from tools.bt2018_mechs.compile import emit_chassis_fit
    from tools.bt2018_mechs.render_icons import build_icons
    names = dict(jedgar='J. Edgar', missile_carrier='LRM Carrier', srm_carrier='SRM Carrier',
                 clan_elemental='Elemental', infantry_gunner='Infantry', infantry_srm='SRM Infantry',
                 fancy_warrior='Warrior', igor_attack='Igor', igor_lifter='Igor')
    definitions = {}
    for packet in read_packets(stock/'data/objects/object2.pak'):
        text = (decode_packet(packet) or b'').decode('latin-1')
        appearance = re.search(r'\bAppearanceName\s*=\s*"([^"]+)"', text, re.I)
        if appearance:
            definitions.setdefault(appearance[1].lower(), text.replace('\r', ''))
    tables = {}
    for off, comp, real, name, lz, data in fst.fst_entries(stock/'misc.fst'):
        if name.startswith('data/objects/') and name.endswith('.csv'):
            tables[Path(name).stem] = list(csv.reader(io.StringIO(fst.decompress_entry(off, comp, real, lz, data).decode('latin-1'), newline='')))
    components = {r[0]: r for r in tables['compbas'][1:] if len(r) > 6}
    for index, v in enumerate(vehicles):
        stem, host = 'mw5_'+v['id'], v['replaces'].split(':')[1]
        donor = definitions['heavycopter' if host == 'assaultcopter' else host]
        name = names.get(v['id'], 'Riverjack' if v['id'].startswith('riverjack') else v['id'].replace('_', ' ').title())
        sid = 62000 + index * 2
        rows = [r[:] for r in tables.get(host, tables['lightcopter'])[:116]]
        ground = v.get('kind') != 'mech'
        if ground:
            values = dict(blocks(donor))
            def number(key, default=0):
                match = re.search(r'\b'+key+r'\s*=\s*([\d.]+)', donor, re.I)
                return float(match[1]) if match else default
            rows[2][4] = str(number('CurTonnage', 15))
            for r in rows[25:97]:
                if len(r) > 5: r[1:6] = ['0', '0', '0', '', '']
            inventory = [(block, body) for block, body in values.items() if block.startswith('Item:')]
            for i, (_, body) in enumerate(inventory):
                master = re.search(r'\bMasterID\s*=\s*(\d+)', '\n'.join(body))
                if master and i < 72:
                    cid = master[1]
                    rows[25+i][1:6] = ['1', str(i % 6), str(i // 6), cid, components[cid][1]]
            rows[10][4] = str(int(number('BattleRating', 25)*100))
        rows[2][1], rows[4][1], rows[5][1] = name, str(sid), str(sid+1)
        rows[7][1] = str(sid+1)
        rows[8][1], rows[9][1], rows[10][1] = stem, stem+'walk', stem+'_bay'
        for r in rows:
            if r and r[0] == 'VariantName': r[1] = name
        target = out/'data/objects'/f'{stem}.csv'
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('w', newline='', encoding='latin-1') as f:
            csv.writer(f, lineterminator='\n').writerows(rows)
        tons = float(rows[2][4])
        payload = max(1, sum(float(components[r[4]][6] or 0) for r in rows[25:97] if len(r)>4 and r[4] in components))
        node = v['components'][0]['node']
        hardpoints = [dict(id=f'CenterTorso.{i}', location='CenterTorso', mount=mount, socket=node)
                      for i,r in enumerate(rows[25:97]) if len(r)>4 and r[4] in components
                      for mount in [components[r[4]][1].removesuffix('Weapon')]
                      if mount in ('Energy', 'Ballistic', 'Missile')]
        profile = dict(chassis_id='mw5:'+v['id'], stem=stem, family=v['id'], balance_profile='mc2-donor:'+host,
                       hardpoints=hardpoints, mc2=dict(tonnage=tons, empty_mass=0, payload=payload,
                       layout_rev=1, regions={'CenterTorso': [0,0,6,5]}, ap_ordinals=[]))
        fit = emit_chassis_fit(profile, {})
        chassis = fit[fit.index('[Chassis]'):].replace('b Purchasable = TRUE', 'b Purchasable = TRUE\nb Vehicle = TRUE')
        donor = re.sub(r'(\b(?:Name|AppearanceName|ProfileName)\s*=\s*)"[^"]*"', lambda m: m[1]+'"'+stem+'"', donor)
        donor = re.sub(r'(\bDescIndex\s*=\s*)\d+', lambda m: m[1]+str(sid), donor)
        path = out/'data/defs/chassis/mw5'/v['id']/'chassisdef.fit'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(re.sub(r'(?i)FITend\s*$', '', donor)+chassis, encoding='latin-1')
        strings = out/'data/defs/text/en_us'/f'{stem}.fit'
        strings.parent.mkdir(parents=True, exist_ok=True)
        strings.write_text('\n'.join(f'String {{\n key = "mw5.{v["id"]}.{key}"\n text = "{name}"\n legacyId = {lid}\n source = "mw5-vehicles"\n category = "units"\n}}' for key,lid in [('name',sid),('details',sid+1)]))
        # Vehicle meshes have no BattleMech limb names. Render their complete
        # silhouette as one body region without changing the source assembly.
        doc, buffers, _ = load_document(assemblies/v['id']/'assembled.glb')
        for n in doc['nodes']:
            if 'mesh' in n: n['name'] = n.get('name', '') + ('_left_leg' if ground else '_body')
        with tempfile.TemporaryDirectory(prefix='mw5-icons-') as work:
            icon = Path(work)/'icon.glb'
            write_glb(icon, doc, buffers[0])
            build_icons(icon, stem, out/'data/art')


def build(recipe, assemblies, stock, out):
    recipe = json.loads(Path(recipe).read_text())
    assemblies, stock, out = map(Path, (assemblies, stock, out))
    vehicles = recipe['vehicles']
    specs = []
    static_copies = []
    for v in vehicles:
        path = assemblies/v['id']/'assembled.glb'
        doc, buffers, _ = load_document(path)
        if doc.get('animations'):
            doc.pop('animations')
            with tempfile.NamedTemporaryFile(suffix='.glb', delete=False) as tmp:
                path = Path(tmp.name)
            write_glb(path, doc, buffers[0])
            static_copies.append(path)
        specs.append(dict(frontend='gltf', id='mw5_'+v['id'], replaces='vehicle:mw5_'+v['id'], kind='prop',
                          intact=str(path), frame='gltf', wu_per_unit=10,
                          classes={m['name']: 'painted_metal' for m in doc.get('materials', [])}))
    try:
        cook(dict(mod_id='mw5-vehicles', name='Vehicles, infantry and Elemental', assets=specs), out,
             compress=False)
    finally:
        for path in static_copies:
            path.unlink()
    registry = out/'data/model_overrides/models.json'
    models = json.loads(registry.read_text())
    receipts = {r['id']: r for r in json.loads((assemblies/'assembly-receipt.json').read_text())}
    for v in vehicles:
        stem, host = 'mw5_'+v['id'], v['replaces'].split(':')[1]
        tgl = out/'data/tgl'
        ini = (stock/'data/tgl'/f'{host}.ini').read_text(encoding='latin-1')
        yaw = next((c['node'] for c in v['components'] if c.get('animated')), None)
        if v.get('kind') != 'mech':
            # A mod-owned INI deliberately bypasses render overrides. Retain the
            # stock INI and bind our yaw pivot to its existing animation node.
            record = next(r for r in models['overrides'] if r['replaces'] == 'vehicle:'+stem)
            ground_path = out/'data/model_overrides'/record['source']
            ground, buffers, _ = load_document(ground_path)
            stock_yaw = re.search(r'\bst AnimationNodeId\s*=\s*"([^"]*)"', ini, re.I)
            if yaw and stock_yaw:
                target = stock_yaw[1]
                if target.lower() == 'sc_turret':
                    target = 'Centipede_turret'
                for node in ground.get('nodes', []):
                    if node.get('name') == yaw:
                        node['name'] = target
                write_glb(ground_path, ground, buffers[0])
            # The vehicle renderer requires three complete slots. Reuse the
            # complete source mesh at each distance rather than falling to stock.
            record['lods'] = [dict(lod=i, source=record['source'], distance=i*500) for i in (1, 2)]
            (tgl/f'{stem}.ini').write_text(ini, encoding='latin-1')
            continue
        # VTOLs are stock Mech3DAppearances, so use the existing imported-mech
        # path. Keep the source animation hierarchy and metre/Y-up frame.
        record = next(r for r in models['overrides'] if r['replaces'] == 'vehicle:'+stem)
        cooked, _, _ = load_document(out/'data/model_overrides'/record['source'])
        names = {m['name']: m for m in cooked.get('materials', [])}
        doc, buffers, _ = load_document(assemblies/v['id']/'assembled.glb')
        with tempfile.TemporaryDirectory(prefix='mw5-air-materials-') as work:
            original = gltf_file.to_ir(stem, v['replaces'], assemblies/v['id']/'assembled.glb', kind='prop',
                classes={m['name']: 'painted_metal' for m in doc.get('materials', [])},
                meta={'frame': 'gltf', 'wu_per_unit': 10}, work_dir=Path(work), allow_skin=True)
            aliases = material_aliases(original.materials)
        for mat in doc.get('materials', []):
            c = names[aliases[mat['name']]]
            info = c.get('pbrMetallicRoughness', {}).get('baseColorTexture')
            if info is None:
                continue
            image = cooked['images'][cooked['textures'][info['index']]['source']]
            uri = Path(image['uri']).name
            index = len(doc.setdefault('textures', []))
            doc['textures'].append({'source': len(doc.setdefault('images', []))})
            doc['images'].append({'uri': uri})
            mat.setdefault('pbrMetallicRoughness', {})['baseColorTexture'] = {'index': index}
        # Every movement gesture retains the actual Warrior rotor clip.
        if doc.get('animations'):
            clip = doc['animations'][0]
            doc['animations'] = [dict(clip, name=name) for name in (
                'atlas_moveCoreIdle', 'atlas_moveCoreWalkFwd', 'atlas_moveCoreRunFwd',
                'atlas_moveJumpUpIdle', 'atlas_moveCoreWalkBwd',
                'atlas_moveCoreTurnLeftIdle', 'atlas_moveCoreTurnRightIdle',
                'atlas_moveCoreWalkLimpFwdLeft', 'atlas_moveCoreWalkLimpFwdRight')]
        write_glb(tgl/f'{stem}.glb', doc, buffers[0])
        node = v['components'][0]['node']
        bounds = receipts[v['id']]['bounds_m']
        package = dict(schema='bt2018-mech-package/2', chassisId='mw5:'+v['id'],
                       sourceGlb=stem+'.glb', nodes={k: node for k in ('joint_torso','cockpit','hit_left','hit_right')},
                       socketOffsets={}, trueHeightM=bounds[1][2]-bounds[0][2], restSoleHeightM=0,
                       materials={}, featureBits=[])
        (tgl/f'{stem}.package.json').write_text(json.dumps(package, indent=2)+'\n')
        imported = emit_ini(ini, {}, {'mc2': {'stem': stem, 'model': {'host_ini': host}}})
        imported = imported.replace('MW5 Timber Wolf appearance for the stock Mad Cat', 'MW5 '+v['id']+' appearance')
        (tgl/f'{stem}.ini').write_text(imported, encoding='latin-1')
    air = {'vehicle:mw5_'+v['id'] for v in vehicles if v.get('kind') == 'mech'}
    models['overrides'] = [r for r in models['overrides'] if r['replaces'] not in air]
    registry.write_text(json.dumps(models, indent=2)+'\n')
    manifest = out/'mod.json'
    mod = json.loads(manifest.read_text())
    mod['capabilities'] = ['imported_mechs', 'package_chassis']
    manifest.write_text(json.dumps(mod, indent=2)+'\n')
    (out/'COPY.txt').write_text(
        'Personal-use assets extracted from the owner\'s MW5 installations. Do not redistribute.\n'
        'Copy this folder to <MC2 install>/mods/mw5-vehicles and select mw5-vehicles.\n'
        'Select the base pack to enable all imported vehicles.\n'
        'Every unit is a separate chassis alongside the stock roster.\n'
        'Stock missions, movement, weapons and collision remain the gameplay source.\n'
        'Socket/helper meshes are excluded; carrier paint/wear is baked and weapon glow is not albedo.\n'
        'Native Elemental and armed Riverjack textures are retained. Other unresolved materials use a matte fallback.\n'
        'Stock unit files and appearances are not overridden.\n'
        'Human infantry VAT gait and Elemental skeletal\n'
        'animation are not translated; these appearances retain their source bind pose.\n'
        'Turret yaw and barrel pitch are separate nodes; stock vehicle code controls yaw only.\n'
        'Look acceptance is pending owner review. See the assembly receipts and family sheets.\n')
    shutil.copyfile(assemblies/'assembly-receipt.json', out/'assembly-receipt.json')
    logistics(vehicles, stock, out, assemblies)
    print(f'PACK {len(vehicles)} appearances: {out}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('recipe')
    ap.add_argument('assemblies')
    ap.add_argument('stock')
    ap.add_argument('out')
    build(**vars(ap.parse_args()))
