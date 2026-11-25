#!/usr/bin/env python3
"""Build the personal-use MW5 Mad Cat appearance pack (a mod) from the owner's own extracted files.

Same shape as tools/bt2018_mechs/build_pack.py: the GLB, BC7 maps and sidecar are COPIED into <install>/mods/<mod-id>/;
nothing in the install's base data is modified. Each imported chassis owns a separate CSV,
appearance and package definition. Never commit the generated pack.

  py -3 tools/mw5_mechs/build_pack.py --recipe <owner recipe.json> --install <lane-install>
Mount with MC2_MOD_DEPS=mw5_mechs.
"""
import argparse
import csv
import io
import json
import shutil
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sockets import load_glb, package_nodes  # noqa: E402
from build_glb import DEFAULT_RECIPE, build, load_recipe, read_accessor, trs_mat  # noqa: E402

KEEP = ("Gestures", "Bounds", "SmokeNode0", "JumpJetNode0", "FootNode0", "FootNode1", "FootPrint")

# New chassis inherit the nearest-tonnage MC2 variant, including its lab grid.
TONNAGE = dict(champion=60, crusader=65, direwolf=100, executioner=95,
               gargoyle=80, longbow=85, puma=35, warhawk=85)
DONORS = dict(archer='thor', assassin='shayu', awesome='zeus', blackjack='shadowcat',
              centurion='enfield', champion='vulture', cicada='shayu', commando='anubis',
              crab='hunchback', crusader='catapult', direwolf='atlas', dragon='vulture',
              executioner='bloodasp', firestarter='wolfhound', gargoyle='zeus',
              javelin='uller', jenner='cougar', locust='werewolf', longbow='cyclops',
              marauder='madcat', orion='laohu', panther='hollander', puma='cougar',
              quickdraw='vulture', rifleman='vulture', shadowhawk='ryoken', victor='zeus',
              vindicator='shadowcat', vulcan='shayu', warhammer='thor', warhawk='zeus')


def logistics(recipe, inst, out, loadout=None):
    """Add a pack chassis and stock-style icons without changing engine code."""
    from tools.mc2x_import import fst
    from tools.bt2018_mechs import render_icons
    from tools.bt2018_mechs.compile import emit_chassis_fit
    loadout = recipe.get('loadout', recipe.get('logistics_loadout')) if loadout is None else loadout
    host, stem = recipe['mc2']['model']['host_ini'], recipe['mc2']['stem']
    stock = {}
    components = {}
    weights = {}
    component_rows = {}
    for off, comp, real, name, lz, data in fst.fst_entries(inst / 'misc.fst'):
        if name.startswith('data/objects/') and name.endswith('.csv'):
            rows = list(csv.reader(io.StringIO(fst.decompress_entry(off, comp, real, lz, data).decode('latin-1'), newline='')))
            if name.endswith('/compbas.csv'):
                component_rows = {r[0]: r for r in rows[1:] if len(r) > 28}
                components = {r[0]: r[1].removesuffix('Weapon') for r in rows[1:] if len(r) > 1 and r[1].endswith('Weapon')}
                weights = {r[0]: float(r[6]) for r in rows[1:] if len(r) > 6 and r[6].strip().replace('.', '', 1).isdigit()}
            if rows and rows[0][0] == 'BaseChassisInfo' and len(rows) > 22 and rows[2][3] == 'Tonnage':
                stock[Path(name).stem] = rows
    for name in stock:
        loose = inst / 'data/objects' / (name + '.csv')
        if loose.is_file():
            stock[name] = list(csv.reader(io.StringIO(loose.read_text(encoding='latin-1'))))
    family = recipe['family']
    tons = TONNAGE.get(host)
    if tons is None:
        table = json.loads((Path(__file__).parents[1] / 'bt2018_mechs/data/roster.json').read_text())
        tons = table.get(family, [float(stock[host][2][4]) if host in stock else 0])[0]
    donor = host if host in stock else DONORS[host]
    rows = [r[:] for r in stock[donor]]
    rows = rows[:116]  # One donor loadout, with the imported chassis' own identity.
    name = {'madcat': 'Timber Wolf', 'uller': 'Kit Fox', 'ryoken': 'Stormcrow',
                     'thor': 'Summoner', 'vulture': 'Mad Dog', 'puma': 'Adder',
                     'jaegermech': 'JagerMech', 'direwolf': 'Dire Wolf',
                     'maddog': 'Mad Dog', 'timberwolf': 'Timber Wolf',
                     'shadowhawk': 'Shadow Hawk', 'shadowcat': 'Shadow Cat',
                     'blackknight': 'Black Knight', 'urbanmech': 'UrbanMech',
                     'bushwacker': 'Bushwacker'}.get(host, family.title())
    rows[2][1], rows[8][1], rows[9][1], rows[10][1] = name, stem, stem + 'walk', stem + '_bay'
    # Stable pack string range, separate from the BT2018 range.
    index = sorted(p.stem for p in Path(__file__).with_name('recipes').glob('*.json') if p.stem != 'madcat_alt').index('timberwolf' if family == 'madcat' else family)
    sid = 61000 + index * 2
    rows[4][1], rows[5][1] = str(sid), str(sid + 1)
    rows[7][1] = str(sid + 1)
    for r in rows:
        if r and r[0] == 'VariantName':
            r[1] = name
    target = out / 'data/objects' / (stem + '.csv')
    target.parent.mkdir(parents=True, exist_ok=True)
    if host != stem:
        (target.parent / (host + '.csv')).unlink(missing_ok=True)
        (out / 'data/tgl' / (host + '.ini')).unlink(missing_ok=True)
    text = io.StringIO(newline='')
    csv.writer(text, lineterminator='\n').writerows(rows)
    target.write_text(text.getvalue(), encoding='latin-1')
    tgl = out / 'data/tgl'
    pkg_path = tgl / (stem + '.package.json')
    pkg = json.loads(pkg_path.read_text())
    nodes, offsets, _, _ = package_nodes(tgl / (stem + '.glb'), loadout, recipe)
    pkg['nodes'], pkg['socketOffsets'] = nodes, offsets
    doc, _ = load_glb(tgl / (stem + '.glb'))
    attachments = doc.get('extras', {}).get('mw5Attachments', [])
    sources = {}
    for row in loadout:
        if not row.get('node'):
            continue
        if row.get('copy_of'):
            sources[row['node']] = sources[row['copy_of']] + ' (alias)'
            continue
        source = next((a for a in attachments if a['source'] == row.get('mesh')), None)
        sources[row['node']] = 'MW5 socket' if source and any(s in source['sockets'] for s in row.get('fires', [])) else 'bone heuristic'
    pkg['firePointSources'] = sources
    pkg['clips'] = {'showcase': recipe['clips']['Stand']}
    pkg_path.write_text(json.dumps(pkg, indent=2) + '\n')
    # Keep the donor's rectangular lab and loadout; physical muzzle sockets remain
    # attached to their real arm/torso bones in the appearance sidecar.
    sockets = [k for k in pkg['nodes'] if k.startswith('weapon_')]
    if not sockets:
        raise ValueError(f'{stem}: no weapon sockets')
    # MC2 deliberately ignores component mass when fitting legacy variants.
    # Preserve that donor's complete loadout under the package payload rule.
    payload = max(.8 * float(rows[2][4]) - 7,
                  sum(weights.get(r[4], 0) for r in rows[25:96] if len(r) > 4))
    hardpoints = []
    for mount, types in [('Energy', (4, 5)), ('Ballistic', (2,)), ('Missile', (1,))]:
        compatible = [r['node'] for r in loadout if r.get('node') and r['type'] in types]
        required = max(sum(components.get(r[4]) == mount for r in rows[start+6:start+78] if len(r) > 4)
                       for start in range(19, len(rows)-96, 97))
        for i in range(max(required, len(compatible))):
            socket = (compatible or sockets)[i % len(compatible or sockets)]
            hardpoints.append(dict(id=f'CenterTorso.{mount}.{i}', location='CenterTorso', mount=mount, socket=socket))
    profile = dict(chassis_id=recipe['chassis_id'], stem=stem, family=family,
                   balance_profile='mc2-donor:' + donor, hardpoints=hardpoints,
                   mc2=dict(tonnage=float(rows[2][4]), empty_mass=.2*float(rows[2][4])+7, payload=payload, layout_rev=1,
                            regions={'CenterTorso': [0, 0, int(rows[12][1]), int(rows[13][1])]}, ap_ordinals=[]))
    fit = out / 'data/defs/chassis/mw5' / family / 'chassisdef.fit'
    fit.parent.mkdir(parents=True, exist_ok=True)
    # The classic rectangle is a packing surface, not the physical critical
    # table. Retain the donor's eight locations for the record-sheet lab.
    locations = ['Head', 'LeftArm', 'RightArm', 'LeftTorso', 'RightTorso', 'CenterTorso', 'LeftLeg', 'RightLeg']
    placement = next(i for i, r in enumerate(rows) if r and r[0] == 'ComponentPlacement')
    layout = ['[Layout]', f'st ChassisId = "{recipe["chassis_id"]}"', 'l Version = 1',
              f'st Source = "mc2-donor:{donor}"', f'f Payload = {payload}',
              f'l ArmorPool = {sum(int(v) for v in rows[17][1:12] if v.strip())}', '']
    for col, loc in enumerate(locations, 1):
        indices = list(dict.fromkeys(int(r[col]) for r in rows[placement+1:placement+13]
                                    if len(r) > col and r[col].strip()))
        items = [component_rows.get(rows[25+i][4]) for i in indices if 25+i < placement]
        items = [r for r in items if r and float(r[27] or 0) > 0 and float(r[28] or 0) > 0]
        mounts = [components[r[0]] for r in items if r[0] in components]
        # Legacy MC2 donor designs can exceed the nominal critical-size
        # budget (e.g. Thor's AC20 plus ammo); keep that authored loadout.
        slots = max(6 if loc in ('Head', 'LeftLeg', 'RightLeg') else 12,
                    sum(int(float(r[3])) for r in items))
        rear = {'LeftTorso': 9, 'RightTorso': 10, 'CenterTorso': 11}.get(loc)
        structure = int(rows[16][col])
        layout += [f'[Location.{loc}]', f'l Slots = {slots}', f'l Structure = {structure}',
                   f'l ArmorMax = {2*structure if loc != "Head" else 9}',
                   f'l ArmorRearMax = {2*structure if rear else 0}', f'l HardpointCount = {len(mounts)}']
        for i, mount in enumerate(mounts):
            compatible = [h['socket'] for h in hardpoints if h['mount'] == mount]
            layout += [f'st Hardpoint{i} = "{mount}"', f'st Socket{i} = "{compatible[i % len(compatible)]}"']
        layout.append('')
    fit.write_text(emit_chassis_fit(profile, recipe).replace('FITend', '\n'.join(layout) + '\nFITend'))
    strings = out / 'data/defs/text/en_us' / (stem + '.fit')
    strings.parent.mkdir(parents=True, exist_ok=True)
    strings.write_text('\n'.join('String {\n key = "mw5.' + family + '.' + key + '"\n text = "' + value + '"\n legacyId = ' + str(lid) + '\n source = "mw5_mechs"\n category = "units"\n}'
                                 for key, value, lid in [('name', name, sid), ('details', name, sid+1)]))
    report = out / 'content' / (stem + '.json')
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(dict(chassis=recipe['chassis_id'], donor=donor, tonnage=tons,
                                     donor_tonnage=float(rows[2][4]), stats='stock' if host in stock else 'nearest-tonnage donor',
                                     variants=[name],
                                     fire_points=sources, layout='MC2 donor grid'), indent=2) + '\n')
    render_icons.build_icons(tgl / (stem + '.glb'), stem, out / 'data/art')


def rest_sole_height(doc, buf):
    """Lowest articulated foot vertex in metres, including the skin's bind transform."""
    parents = {c: i for i, n in enumerate(doc['nodes']) for c in n.get('children', [])}
    def world(i):
        return (world(parents[i]) if i in parents else np.eye(4)) @ trs_mat(doc['nodes'][i])
    def foot(i):
        return doc['nodes'][i]['name'] in ('j_LFoot', 'j_RFoot') or (i in parents and foot(parents[i]))
    lows = []
    for node in doc['nodes']:
        if 'mesh' not in node or 'skin' not in node:
            continue
        skin = doc['skins'][node['skin']]
        joint = skin['joints'][0]  # MW5 rigid split: one joint per mesh
        if not foot(joint):
            continue
        bind = read_accessor(doc, buf, skin['inverseBindMatrices'])[0].reshape(4, 4).T
        transform = world(joint) @ bind
        for pr in doc['meshes'][node['mesh']]['primitives']:
            vertices = read_accessor(doc, buf, pr['attributes']['POSITION'])
            lows.append(float((np.c_[vertices, np.ones(len(vertices))] @ transform.T)[:, 1].min()))
    if not lows:
        raise ValueError('no articulated foot geometry')
    return min(lows)


def blocks(text):
    cur, body, out = None, [], []
    for raw in text.splitlines():
        s = raw.strip()
        if s.startswith("[") and s.endswith("]"):
            if cur is not None:
                out.append((cur, body))
            cur, body = s[1:-1], []
        elif cur is not None and s and not s.startswith("//") and s.lower() != "fitend":
            body.append(s)
    if cur is not None:
        out.append((cur, body))
    return out


def emit_ini(host_text, weapon_nodes, recipe=None):
    recipe = load_recipe() if recipe is None else recipe
    stem = recipe["mc2"]["stem"]
    host = recipe["mc2"]["model"]["host_ini"]
    out = ["FITini", "", "// MW5 Timber Wolf appearance for the stock Mad Cat -- generated by tools/mw5_mechs/build_pack.py.",
           "", "[Import]", f'st Source = "{stem}"', f'st AnimHost = "{host}"', "",
           "[TGLData]", f'st FileName0 = "{stem}"', "f Distance0 = 0.0", "",
           "[Nodes]", "l NumSmoke = 1", "l NumJumpJet = 1", f"l NumWeapon = {len(weapon_nodes)}", "l NumFeet = 2", ""]
    for i, (n, t) in enumerate(weapon_nodes.items()):
        out += [f"[WeaponNode{i}]", f'st WeaponNodeName = "{n}"', f"l WeaponType = {t}", ""]
    for name, body in blocks(host_text):
        if name.startswith(KEEP):
            out += [f"[{name}]"] + body + [""]
    return "\n".join(out + ["FITend", ""])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--recipe", type=Path, default=DEFAULT_RECIPE)
    ap.add_argument("--glb", help="optional prebuilt GLB; otherwise build recipe source_skm")
    ap.add_argument("--textures", help="directory cooked by cook_textures.py")
    ap.add_argument("--loadout", help="resolved names-only hardpoint table JSON")
    ap.add_argument("--mod-id")
    a = ap.parse_args()
    recipe = load_recipe(a.recipe)
    model = recipe["mc2"]["model"]
    stem, host_name = recipe["mc2"]["stem"], model["host_ini"]
    def source(name):
        return a.recipe.resolve().parent / model[name]
    textures = Path(a.textures) if a.textures else source("textures")
    loadout = Path(a.loadout) if a.loadout else source("loadout")
    a.mod_id = a.mod_id or recipe["mod"]["id"]
    inst = Path(a.install)
    host = inst / "data" / "tgl" / (dict(jaegermech='jaegermech3').get(host_name, host_name) + '.ini')
    if not host.is_file():
        sys.exit(f"stock {host_name}.ini not found as a loose file: {host}")
    out = inst / "mods" / a.mod_id
    tgl = out / "data" / "tgl"
    tgl.mkdir(parents=True, exist_ok=True)
    glb = tgl / f"{stem}.glb"
    if a.glb:
        if Path(a.glb).resolve() != glb.resolve():
            shutil.copyfile(a.glb, glb)
    else:
        attachments = [f"{a.recipe.resolve().parent / row['source']}={row['bone']}" for row in model["attachments"]]
        build(source("source_skm"), source("anim_dir"), glb, f"{stem}_alb.png",
              attachments=attachments, recipe=recipe)
    doc, buf = load_glb(glb)
    clips = {clip["name"] for clip in doc.get("animations", []) if clip["channels"]}
    expected = set(recipe["clips"].values()) | set(recipe["hold_clips"]) if not a.glb else set()
    if not clips or expected - clips:
        raise ValueError(f"missing animation clips: {sorted(expected - clips)}")
    nodes, offsets, weapon_nodes, height = package_nodes(glb, json.loads(loadout.read_text()), recipe)
    (tgl / f"{stem}.ini").write_bytes(emit_ini(host.read_text(encoding="latin-1"), weapon_nodes, recipe).encode("latin-1"))
    tags = ["alb", "nrm"]
    if (textures / f"{stem}_orm.ktx2").is_file():
        tags.append("orm")
    for tier in ("", "128"):
        (tgl / tier).mkdir(exist_ok=True)
        for tag in tags:
            name = f"{stem}_{tag}.ktx2"
            src, dst = textures / tier / name, tgl / tier / name
            if src.resolve() != dst.resolve():
                shutil.copyfile(src, dst)
    materials = {kind: {"tga": f"{stem}_{tag}.tga", "ktx2": f"{stem}_{tag}.ktx2",
                        "logicalFormat": fmt, "container": "bc7"}
                 for kind, tag, fmt in (("albedo", "alb", "srgb-rgb"), ("normal", "nrm", "linear-normal-xyz"))}
    materials["normal"].update(source=model["normal_source"], skinIndependent=True)
    if "orm" in tags:
        materials["orm"] = {"tga": f"{stem}_orm.tga", "ktx2": f"{stem}_orm.ktx2",
                            "logicalFormat": "linear-ao-roughness-metallic", "container": "bc7"}
    # Keep glTF image URIs resolvable as well as the runtime KTX2 sidecars.
    for image in doc.get("images", []):
        uri = image.get("uri", "")
        if uri and not uri.startswith("data:"):
            src = textures / uri
            if src.is_file():
                shutil.copyfile(src, tgl / uri)
    pkg = {"schema": "bt2018-mech-package/2", "chassisId": recipe["chassis_id"], "sourceGlb": f"{stem}.glb", "nodes": nodes,
           "socketOffsets": offsets, "trueHeightM": height, "restSoleHeightM": rest_sole_height(doc, buf),
           "materials": materials, "featureBits": ["HAS_NORMAL"],
           "grounding": {clip: role for clip, role in recipe.get("grounding", {}).items() if clip in clips}}
    (tgl / f"{stem}.package.json").write_text(json.dumps(pkg, indent=2) + "\n")
    (out / "mod.json").write_text(json.dumps({
        "schema": "mc2-mod/1", "id": a.mod_id, "name": recipe["mod"]["name"], "version": "0.1.0",
        "author": "generated by tools/mw5_mechs/build_pack.py", "type": "assets", "dependencies": [],
        "capabilities": ["imported_mechs", "package_chassis"],
        "description": recipe["mod"]["description"]}, indent=2) + "\n")
    logistics(recipe, inst, out, json.loads(loadout.read_text()))
    print("wrote", out)


if __name__ == "__main__":
    main()
