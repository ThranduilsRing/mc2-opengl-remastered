"""Cook assembled MW5 vehicles into ground overrides and imported VTOL appearances."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline.cook import cook
from tools.content_pipeline.gltf_io import load_document
from tools.mw5_mechs.build_pack import emit_ini


def write_glb(path, doc, binary):
    text = json.dumps(doc, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 28 + len(text) + len(binary))
                     + struct.pack('<II', len(text), 0x4e4f534a) + text
                     + struct.pack('<II', len(binary), 0x004e4942) + binary)


def air_variant(out, vehicle, base):
    # AssaultCopter has an appearance INI but no stock object2 slot. Offer the
    # attack Igor on the real HeavyCopter role while retaining the base pack.
    variant = out/'variants'/('mw5-'+vehicle['id'].replace('_','-'))
    tgl = variant/'data/tgl'
    tgl.mkdir(parents=True, exist_ok=True)
    source, host = vehicle['replaces'].split(':')[1], base['replaces'].split(':')[1]
    ini = (out/'data/tgl'/f'{source}.ini').read_text(encoding='latin-1')
    ini = ini.replace(f'AnimHost = "{source}"', f'AnimHost = "{host}"')
    (tgl/f'{host}.ini').write_text(ini, encoding='latin-1')
    (variant/'mod.json').write_text(json.dumps(dict(schema='mc2-mod/1', id=variant.name,
        name='MW5 attack Igor', version='0.1.0', dependencies=['mw5-vehicles'], type='assets'), indent=2)+'\n')
    (variant/'COPY.txt').write_text('Copy this folder beside mw5-vehicles in <MC2>/mods. Select mw5-igor-attack.\n'
                                  'The mw5-vehicles dependency retains all other appearances; HeavyCopter uses the attack Igor.\n')


def build(recipe, assemblies, stock, out):
    recipe = json.loads(Path(recipe).read_text())
    assemblies, stock, out = map(Path, (assemblies, stock, out))
    vehicles = [v for v in recipe['vehicles'] if not v.get('variant_of')]
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
        specs.append(dict(frontend='gltf', id='mw5_'+v['id'], replaces=v['replaces'], kind='prop',
                          intact=str(path), frame='gltf', wu_per_unit=10,
                          classes={m['name']: 'painted_metal' for m in doc.get('materials', [])}))
    try:
        cook(dict(mod_id='mw5-vehicles', name='MW5 vehicles, infantry and Elemental', assets=specs), out,
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
            if yaw:
                if re.search(r'\bst AnimationNodeId\s*=', ini, re.I):
                    ini = re.sub(r'(\bst AnimationNodeId\s*=\s*)"[^"]*"',
                                 lambda m: m[1] + '"' + yaw + '"', ini, flags=re.I)
                else:
                    ini = ini.replace('FITend', f'[AnimationNode]\nst AnimationNodeId = "{yaw}"\n\nFITend')
            (tgl/f'{host}.ini').write_text(ini, encoding='latin-1')
            continue
        # VTOLs are stock Mech3DAppearances, so use the existing imported-mech
        # path. Keep the source animation hierarchy and metre/Y-up frame.
        record = next(r for r in models['overrides'] if r['replaces'] == v['replaces'])
        cooked, _, _ = load_document(out/'data/model_overrides'/record['source'])
        names = {m['name']: m for m in cooked.get('materials', [])}
        doc, buffers, _ = load_document(assemblies/v['id']/'assembled.glb')
        for mat in doc.get('materials', []):
            c = names.get(mat['name'], {})
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
        (tgl/f'{host}.ini').write_text(imported, encoding='latin-1')
    air = {v['replaces'] for v in vehicles if v.get('kind') == 'mech'}
    models['overrides'] = [r for r in models['overrides'] if r['replaces'] not in air]
    registry.write_text(json.dumps(models, indent=2)+'\n')
    manifest = out/'mod.json'
    mod = json.loads(manifest.read_text())
    mod['capabilities'] = ['imported_mechs', 'package_chassis']
    manifest.write_text(json.dumps(mod, indent=2)+'\n')
    (out/'COPY.txt').write_text(
        'Personal-use assets extracted from the owner\'s MW5 installations. Do not redistribute.\n'
        'Copy this folder to <MC2 install>/mods/mw5-vehicles and select mw5-vehicles.\n'
        'Optional folders under variants can be copied beside the base mod and selected separately.\n'
        'Ground appearances replace nearest stock roles; VTOLs use the existing imported-mech path.\n'
        'Stock missions, movement, weapons and collision remain the gameplay source.\n'
        'Source layered materials are approximated. Human infantry VAT gait and Elemental skeletal\n'
        'animation are not translated; these appearances retain their source bind pose.\n'
        'Turret yaw and barrel pitch are separate nodes; stock vehicle code controls yaw only.\n'
        'Look acceptance is pending owner review. See the assembly receipts and family sheets.\n')
    shutil.copyfile(assemblies/'assembly-receipt.json', out/'assembly-receipt.json')
    for v in recipe['vehicles']:
        if not v.get('variant_of'):
            continue
        spec = next(s for s in specs if s['id'] == 'mw5_'+v['variant_of']).copy()
        path = assemblies/v['id']/'assembled.glb'
        doc, _, _ = load_document(path)
        spec.update(id='mw5_'+v['id'], intact=str(path),
                    classes={m['name']: 'painted_metal' for m in doc.get('materials', [])})
        variant = out/'variants'/('mw5-'+v['id'].replace('_','-'))
        cook(dict(mod_id=variant.name, name='MW5 '+v['id'], assets=[spec]), variant, compress=False)
        manifest = variant/'mod.json'
        mod = json.loads(manifest.read_text()); mod['dependencies'] = ['mw5-vehicles']
        manifest.write_text(json.dumps(mod, indent=2)+'\n')
        (variant/'COPY.txt').write_text('Copy this folder beside mw5-vehicles in <MC2>/mods and select it\n'
                                     'to use SRM infantry. The base pack remains a dependency. Source VAT gait is not translated.\n')
    for v in vehicles:
        if v.get('host_variant_of'):
            air_variant(out, v, next(b for b in vehicles if b['id'] == v['host_variant_of']))
    print(f'PACK {len(vehicles)} appearances: {out}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('recipe')
    ap.add_argument('assemblies')
    ap.add_argument('stock')
    ap.add_argument('out')
    build(**vars(ap.parse_args()))
