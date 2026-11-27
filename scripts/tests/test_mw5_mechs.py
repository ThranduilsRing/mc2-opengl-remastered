"""Synthetic ActorX/glTF tests; no game data. Trigger: tools/mw5_mechs/*.py."""
import json
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mw5_mechs import build_glb as B
from tools.mw5_mechs.ue_basis import psa_pos_to_gltf, psa_quat_to_gltf
from tools.mw5_mechs.verify_basis import angle, fk, world_rotation_delta


@pytest.mark.parametrize('recipe', sorted((Path(B.__file__).parent / 'recipes').glob('*.json')))
def test_mw5_forward_matches_movement(recipe):
    from tools.mw5_mechs.build_pack import face_movement
    r = json.loads(recipe.read_text())
    chassis = r.get('vehicles', [r])
    for _ in chassis:
        doc = {'nodes': [{'name': 'hull'}], 'scenes': [{'nodes': [0]}]}
        face_movement(doc)
        # UE +X survives both native and ActorX conversion; MC2 expects glTF +Z.
        root = doc['nodes'][doc['scenes'][0]['nodes'][0]]
        np.testing.assert_allclose(B.trs_mat(root) @ [1, 0, 0, 0], [0, 0, 1, 0], atol=1e-7)
        face_movement(doc)
        assert len(doc['nodes']) == 2 and root['children'] == [0]


def test_built_mw5_mechs_face_movement():
    import os
    from tools.mw5_mechs.sockets import load_glb
    root = os.environ.get('MW5_STANDALONE_INSTALL')
    if not root:
        pytest.skip('requires the generated private packs')
    recipes = [p for p in (Path(B.__file__).parent / 'recipes').glob('*.json') if p.stem != 'vehicles']
    for path in recipes:
        r = json.loads(path.read_text())
        doc, _ = load_glb(Path(root) / 'mods/mw5_mechs/data/tgl' / (r['mc2']['stem'] + '.glb'))
        # Cockpit position is not an axis: Dragon's cockpit sits behind its pelvis.
        # Check the authored UE +X basis at the actual exported scene boundary.
        for scene in doc['scenes']:
            node = doc['nodes'][scene['nodes'][0]]
            assert node['name'] == 'mw5_forward', r['chassis_id']
            np.testing.assert_allclose(B.trs_mat(node) @ [1, 0, 0, 0],
                                       [0, 0, 1, 0], atol=1e-7)


def test_built_packs_do_not_override_stock_units():
    import csv
    import io
    import os
    import re
    from tools.mc2x_import import fst
    root = os.environ.get('MW5_STANDALONE_INSTALL')
    if not root:
        pytest.skip('requires the generated private packs')
    inst = Path(root)
    stock_paths, stock_ids = set(), set()
    for archive in inst.glob('*.fst'):
        for off, comp, real, name, lz, data in fst.fst_entries(archive):
            stock_paths.add(name.lower())
            if name.startswith('data/objects/') and name.endswith('.csv'):
                rows = list(csv.reader(io.StringIO(fst.decompress_entry(off, comp, real, lz, data).decode('latin-1'), newline='')))
                if rows and rows[0][0] == 'BaseChassisInfo':
                    stock_ids.update(r[1] for r in rows[4:8] if len(r)>1 and r[1].isdigit())
    stock_paths.update(p.relative_to(inst).as_posix().lower() for p in (inst/'data/objects').glob('*'))
    identities = set()
    for pack in (inst/'mods/mw5_mechs', inst/'mods/mw5-vehicles'):
        assert not [p for p in pack.rglob('*') if p.is_file() and p.relative_to(pack).as_posix().lower() in stock_paths]
        for fit in (pack/'data/defs/chassis').rglob('chassisdef.fit'):
            text = fit.read_text()
            identity = re.search(r'ChassisId\s*=\s*"([^"]+)"', text)[1]
            assert identity.startswith('mw5:') and identity not in identities
            identities.add(identity)
            stem = re.search(r'Stem\s*=\s*"([^"]+)"', text)[1]
            rows = list(csv.reader((pack/'data/objects'/f'{stem}.csv').open()))
            assert rows[2][1] == rows[22][1] and rows[2][1].startswith('MW5 ') and ' / ' not in rows[22][1]
            assert not stock_ids.intersection((rows[4][1], rows[5][1]))
            assert (pack/'data/tgl'/f'{stem}.ini').is_file()
            assert (pack/'data/art/mechicons'/f'{stem}.png').is_file()
        registry = pack/'data/model_overrides/models.json'
        if registry.exists():
            assert all(r['replaces'].startswith('vehicle:mw5_') for r in json.loads(registry.read_text())['overrides'])
    assert len(identities) == 71


@pytest.mark.parametrize('family,host,donor', [('atlas', 'atlas', 'atlas'), ('archer', 'archer', 'thor')])
def test_logistics_keeps_donor_loadout_and_registers_new_chassis(tmp_path, monkeypatch, family, host, donor):
    import csv
    import io
    from tools.mw5_mechs import build_pack as P
    from tools.mc2x_import import fst
    from tools.bt2018_mechs import render_icons as R
    recipe = json.loads((Path(P.__file__).with_name('recipes') / (family + '.json')).read_text())
    stem = recipe['mc2']['stem']
    rows = [[''] * 15 for _ in range(116)]
    rows[0][0] = 'BaseChassisInfo'
    rows[2][3:5] = ['Tonnage', '100' if donor == 'atlas' else '70']
    rows[12][1], rows[13][1] = '6', '5'
    rows[22][:2] = ['VariantName', 'Donor Prime']
    rows[25][:6] = ['Item0MasterId', '0', '1', '2', '99', 'Laser']
    rows[16][1:9] = ['3', '11', '11', '15', '15', '22', '15', '15']
    rows[17][1:12] = ['9', '13', '13', '16', '16', '19', '15', '15', '4', '4', '6']
    rows[100][:9] = ['ComponentPlacement', 'Head', 'LeftArm', 'RightArm', 'LeftTorso', 'RightTorso', 'CenterTorso', 'LeftLeg', 'RightLeg']
    rows[101][2] = '0'
    rows += [r[:] for r in rows[19:116]]
    rows[119][1] = 'Donor Elite'
    buf = io.StringIO()
    csv.writer(buf).writerows(rows)
    data = buf.getvalue().encode()
    monkeypatch.setattr(fst, 'fst_entries', lambda p: [(0, len(data), len(data), f'data/objects/{donor}.csv', False, data),
                                                       (0, 0, 0, 'data/objects/compbas.csv', False,
                                                        ('Component Table,Type,Name,x,x,x,Weight\n' +
                                                         ','.join(['99', 'EnergyWeapon', 'Laser', '1', '1', '1', '60'] + [''] * 20 + ['1', '1']) + '\n').encode())])
    monkeypatch.setattr(fst, 'decompress_entry', lambda off, comp, real, lz, data: data)
    monkeypatch.setattr(P, 'load_glb', lambda p: ({'extras': {'mw5Attachments': []}}, b''))
    monkeypatch.setattr(P, 'package_nodes', lambda *a: ({'weapon_0': 'j_Spine2'}, {'weapon_0': '0 0 0'}, {}, 12))
    icons = []
    monkeypatch.setattr(R, 'build_icons', lambda *a: icons.append(a))
    out = tmp_path / 'pack'
    tgl = out / 'data/tgl'
    tgl.mkdir(parents=True)
    (tgl / (host + '.ini')).write_text('FITini\nFITend\n')
    (tgl / (stem + '.package.json')).write_text('{}')
    P.logistics(recipe, tmp_path, out)
    target = out / 'data/objects' / (stem + '.csv')
    result = list(csv.reader(io.StringIO(target.read_text())))
    assert result[25] == rows[25]
    assert len(result) == 116
    assert result[22][1] == result[2][1] == 'MW5 ' + family.title()
    assert not (out / 'data/objects' / (donor + '.csv')).exists()
    assert not (out / 'data/objects' / (host + '.csv')).exists()
    assert ' / ' not in (out / 'data/defs/text/en_us' / (stem + '.fit')).read_text()
    assert result[8][1] == stem and result[10][1] == stem + '_bay'
    assert result[2][4] == rows[2][4]
    assert icons[0][1] == stem
    report = json.loads((out / 'content' / (stem + '.json')).read_text())
    assert report['donor'] == donor
    fits = list((out / 'data/defs/chassis').rglob('chassisdef.fit'))
    assert len(fits) == 1
    if host != donor:
        assert 'b Purchasable = TRUE' in fits[0].read_text()
        assert 'st Socket = "weapon_0"' in fits[0].read_text()
        assert 'f Payload = 60.0' in fits[0].read_text()
        assert 'f EmptyMass = 21.0' in fits[0].read_text()
        text = fits[0].read_text()
        assert 'st Source = "mc2-donor:thor"' in text
        assert '[Location.Head]\nl Slots = 6' in text
        assert '[Location.CenterTorso]\nl Slots = 12' in text
        assert '[Location.LeftLeg]\nl Slots = 6' in text
        arm = text.split('[Location.LeftArm]')[1].split('[Location.RightArm]')[0]
        assert 'l HardpointCount = 1' in arm and 'st Socket0 = "weapon_2"' in arm
        assert 'l ArmorPool = 130' in text


def chunk(name, size, records):
    return struct.pack("<20sIII", name.encode(), 0, size, len(records)) + b"".join(records)


@pytest.fixture
def fixture(tmp_path):
    names = ["Root", "Pelvis", "Foot0_Left", "Foot0_Right"]
    parents = [0, 0, 1, 1]
    # Independent expected glTF quaternions: nontrivial root and child rotations.
    s = np.sqrt(.5)
    qs = [[0, 0, s, s], [s, 0, 0, s], [0, s, 0, s], [0, 0, 0, 1]]
    ps = [[1, 2, 3], [0, 1, 0], [1, -1, 0], [-1, -1, 0]]
    actor_q = [[0, -s, 0, s], [s, 0, 0, -s], [0, 0, s, -s], [0, 0, 0, -1]]
    actor_p = [[100, -300, 200], [0, 0, 100], [100, 0, -100], [-100, 0, -100]]
    bones = [struct.pack("<64sIii4f3ff3f", n.encode(), 0, 0, parents[i], *actor_q[i], *actor_p[i], 0, 0, 0, 0)
             for i, n in enumerate(names)]
    psk = tmp_path / "test.psk"
    psk.write_bytes(chunk("ACTRHEAD", 0, []) + chunk("REFSKELT", 120, bones))
    info = struct.pack("<64s64siiiifffiii", b"Stand", b"", 4, 0, 0, 0, 0, 2, 30, 0, 0, 2)
    keys = [struct.pack("<8f", *actor_p[i], *actor_q[i], 1/30) for _ in range(2) for i in range(4)]
    psa = tmp_path / "TBR_Stand_ANI.psa"
    psa.write_bytes(chunk("ANIMHEAD", 0, []) + chunk("BONENAMES", 120, bones) + chunk("ANIMINFO", 168, [info])
                    + chunk("ANIMKEYS", 32, keys))
    nodes = [{"name": "wrapper", "children": [1]}]
    nodes += [{"name": n, "translation": ps[i], "rotation": qs[i],
               "children": [j+1 for j in range(1, 4) if parents[j] == i]} for i, n in enumerate(names)]
    blob, views, accs = bytearray(), [], []

    def add(arr, ctype, typ):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes})
        accs.append({"bufferView": len(views)-1, "componentType": ctype, "type": typ, "count": len(arr)})
        blob.extend(arr.tobytes())
        return len(accs)-1

    attrs = {"POSITION": add(np.arange(18, dtype=np.float32).reshape(6, 3), 5126, "VEC3"),
             "NORMAL": add(np.tile(np.array([0, 1, 0], np.float32), (6, 1)), 5126, "VEC3"),
             "TEXCOORD_0": add(np.zeros((6, 2), np.float32), 5126, "VEC2"),
             "JOINTS_0": add(np.array([[2, 1, 0, 0]]*3 + [[3, 1, 0, 0]]*3, np.uint16), 5123, "VEC4"),
             "WEIGHTS_0": add(np.tile(np.array([.9, .1, 0, 0], np.float32), (6, 1)), 5126, "VEC4")}
    indices = add(np.arange(6, dtype=np.uint32), 5125, "SCALAR")
    matrices = np.array([np.linalg.inv(m).T.reshape(16) for m in fk(nodes)[1:]], np.float32)
    ibm = add(matrices, 5126, "MAT4")
    g = {"nodes": nodes, "skins": [{"joints": [1, 2, 3, 4], "inverseBindMatrices": ibm}],
         "meshes": [{"primitives": [{"attributes": attrs, "indices": indices}]}],
         "buffers": [{"uri": "fixture.bin", "byteLength": len(blob)}], "bufferViews": views, "accessors": accs}
    (tmp_path / "fixture.bin").write_bytes(blob)
    skm = tmp_path / "fixture.gltf"
    skm.write_text(json.dumps(g))
    return skm, psk, psa, qs, ps


def test_madcat_shoulder_pods_survive_import(fixture, tmp_path):
    from tools.mw5_mechs.make_mech import weapon_attachments

    recipe = B.load_recipe(B.DEFAULT_RECIPE.with_name('madcat_alt.json'))
    skm = fixture[0]
    hull = json.loads(skm.read_text())
    hull['nodes'][2]['name'] = 'Torso_Pitch'
    skm.write_text(json.dumps(hull))
    specs = weapon_attachments(recipe, tmp_path)[:2]
    for side, spec in zip(('Left', 'Right'), specs):
        source, bone = spec.rsplit('=', 1)
        assert Path(source).stem == f'Weapon_Mech_TBR_Shoulder_{side}_MH1_Missile15_SKM'
        assert bone == 'Torso_Pitch'
        path = Path(source)
        path.parent.mkdir(parents=True, exist_ok=True)
        weapon = {**hull, 'nodes': [{'name': 'Root'}]}
        weapon['buffers'] = [{'uri': str(skm.with_name('fixture.bin'))}]
        path.write_text(json.dumps(weapon))
    doc = B.build(skm, None, tmp_path / 'pods.glb', 'albedo.png', clips=[],
                  attachments=specs, recipe=recipe)
    pods = doc['extras']['mw5Attachments']
    assert len(pods) == 2 and all(pod['meshes'] for pod in pods)
    for pod in pods:
        assert pod['joint'] == recipe['bone_rename'].get('Torso_Pitch', 'Torso_Pitch')
        for mesh in pod['meshes']:
            primitive = doc['meshes'][mesh]['primitives'][0]
            assert doc['accessors'][primitive['indices']]['count'] == 3
            assert doc['nodes'][doc['skins'][mesh]['joints'][0]]['name'] == pod['joint']


def test_basis_round_trip_and_psa(fixture):
    skm, psk, psa, qs, ps = fixture
    bones = B.read_psk(psk)
    names, keys, rate, nf = B.read_psa(psa)
    assert names == [b["name"] for b in bones]
    assert rate == 30 and nf == 2 and keys.shape == (2, 4, 8)
    for i, b in enumerate(bones):
        q = psa_quat_to_gltf(b["quat"], i == 0)
        assert angle(q, qs[i]) < 1e-5
        assert angle(-q, qs[i]) < 1e-5
        assert np.allclose(psa_pos_to_gltf(b["pos"]), ps[i])
        # Undo the specified basis and ActorX child conjugation independently.
        inverse_q = q[[0, 2, 1, 3]] * [1, -1, 1, 1 if i == 0 else -1]
        assert abs(np.dot(inverse_q, b["quat"])) == pytest.approx(1)
        assert np.allclose(np.array(ps[i])[[0, 2, 1]] * [100, -100, 100], b["pos"])
        assert angle(psa_quat_to_gltf(keys[0, i, 3:7], i == 0), qs[i]) < 1e-5


def test_split_determinism_and_animation(fixture, tmp_path):
    skm, _, _, _, _ = fixture
    a, b = tmp_path / "a.glb", tmp_path / "b.glb"
    doc = B.build(skm, tmp_path, a, "albedo.png")
    B.build(skm, tmp_path, b, "albedo.png")
    assert a.read_bytes() == b.read_bytes()
    assert {m["name"] for m in doc["meshes"]} == {"j_LFoot", "j_RFoot"}
    assert all(len(n["name"]) <= 24 for n in doc["nodes"])
    assert "mw5_fix" not in {n["name"] for n in doc["nodes"]}
    assert len(doc["animations"][0]["channels"]) == 4
    data = a.read_bytes()
    jslen = struct.unpack_from("<I", data, 12)[0]
    buf = data[28+jslen:]
    for skin in doc["skins"]:
        assert len(skin["joints"]) == 1
    for mesh in doc["meshes"]:
        pr = mesh["primitives"][0]
        assert B.read_accessor(doc, buf, pr["indices"]).tolist() == [0, 1, 2]
        assert np.all(B.read_accessor(doc, buf, pr["attributes"]["JOINTS_0"]) == 0)
    for channel in doc["animations"][0]["channels"]:
        sampler = doc["animations"][0]["samplers"][channel["sampler"]]
        q = B.read_accessor(doc, buf, sampler["output"])
        assert np.max(angle(q, doc["nodes"][channel["target"]["node"]]["rotation"])) < 1e-5


@pytest.mark.parametrize("fault", ["ibm", "mixed", "name"])
def test_hard_gates(fixture, tmp_path, fault):
    skm, _, _, _, _ = fixture
    g = json.loads(skm.read_text())
    buf = bytearray((tmp_path / "fixture.bin").read_bytes())
    if fault == "name":
        # Bone name fits, but its generated mesh-node name exceeds 24.
        g["nodes"][3]["name"] = "x" * 24
    else:
        accessor = g["skins"][0]["inverseBindMatrices"] if fault == "ibm" else g["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"]
        off = g["bufferViews"][g["accessors"][accessor]["bufferView"]]["byteOffset"]
        struct.pack_into("<f" if fault == "ibm" else "<H", buf, off + (2*64 if fault == "ibm" else 8), 42 if fault == "ibm" else 3)
    skm.write_text(json.dumps(g))
    (tmp_path / "fixture.bin").write_bytes(buf)
    with pytest.raises(ValueError, match={"ibm": "ibm_drift", "mixed": "1%", "name": "24 chars"}[fault]):
        B.build(skm, tmp_path, tmp_path / "bad.glb", "albedo.png")


def test_truncated_actorx(tmp_path):
    p = tmp_path / "bad.psa"
    p.write_bytes(chunk("ANIMKEYS", 32, [bytes(32)])[:-1])
    with pytest.raises(ValueError, match="truncated"):
        B.read_psa(p)


def test_world_pelvis_distinguishes_root_sign(fixture):
    skm, _, psa, _, _ = fixture
    nodes = json.loads(skm.read_text())["nodes"]
    _, keys, _, _ = B.read_psa(psa)
    pose = {i+1: {"rotation": psa_quat_to_gltf(k[3:7], i == 0),
                  "translation": psa_pos_to_gltf(k[:3])} for i, k in enumerate(keys[0])}
    rest = fk(nodes)
    assert world_rotation_delta(fk(nodes, pose)[2], rest[2]) < 1e-5
    pose[1]["rotation"] = psa_quat_to_gltf(keys[0, 0, 3:7], False)
    assert world_rotation_delta(fk(nodes, pose)[2], rest[2]) > 179.99


def test_bridge_output_bytes_unchanged(fixture, tmp_path):
    # Hardpoints intentionally add trueHeightM and mw5Attachments extras; geometry/clips stay unchanged.
    import hashlib
    skm = fixture[0]
    B.build(skm, tmp_path, tmp_path / "a.glb", "albedo.png")
    assert hashlib.sha256((tmp_path / "a.glb").read_bytes()).hexdigest() == \
        "d1b685dd64ad0cddb484d5807aa15e6831a4b1ee00f309c79bc9ed44d6bb0f25"


def test_native_input_matches_bridge_on_fixture(fixture, tmp_path):
    import base64
    skm, _, psa, qs, _ = fixture
    g = json.loads(skm.read_text())
    blob = bytearray((skm.parent / g["buffers"][0]["uri"]).read_bytes())
    # Native-style doc: no wrapper node, mesh node under the default scene, one LOD, rotation clip, bridge list.
    nodes = [{k: ([c - 1 for c in v] if k == "children" else v) for k, v in n.items()} for n in g["nodes"][1:]]
    nodes.append({"name": "mesh_LOD0", "mesh": 0, "skin": 0})
    while len(blob) % 4:
        blob.append(0)
    times = np.array([0, 1 / 30], np.float32)
    g["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": 8})
    blob.extend(times.tobytes())
    g["accessors"].append({"bufferView": len(g["bufferViews"]) - 1, "componentType": 5126, "type": "SCALAR", "count": 2})
    tin, samplers, channels = len(g["accessors"]) - 1, [], []
    for i, q in enumerate(qs):
        g["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": 32})
        blob.extend(np.array([q, q], np.float32).tobytes())
        g["accessors"].append({"bufferView": len(g["bufferViews"]) - 1, "componentType": 5126, "type": "VEC4", "count": 2})
        samplers.append({"input": tin, "output": len(g["accessors"]) - 1, "interpolation": "LINEAR"})
        channels.append({"sampler": i, "target": {"node": i, "path": "rotation"}})
    g.update(nodes=nodes, skins=[{**g["skins"][0], "joints": [0, 1, 2, 3]}], scene=0, scenes=[{"nodes": [0, 4]}],
             animations=[{"name": "TBR_Stand_ANI", "samplers": samplers, "channels": channels}],
             extras={"materials": [], "loadout": [], "bridge": []},
             buffers=[{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}])
    native = tmp_path / "native.gltf"
    native.write_text(json.dumps(g))
    a = B.build(native, None, tmp_path / "n.glb", "albedo.png")
    b = B.build(skm, tmp_path, tmp_path / "b.glb", "albedo.png")
    assert [n["name"] for n in a["nodes"]] == [n["name"] for n in b["nodes"]]
    assert [m["name"] for m in a["meshes"]] == [m["name"] for m in b["meshes"]]
    assert [x["name"] for x in a["animations"]] == [x["name"] for x in b["animations"]] == ["atlas_moveCoreIdle", "atlas_downedGetup", "atlas_hitReactLgtFwd",
                                                                                              "atlas_hitReactLgtBwd", "atlas_hitReactLgtLeft", "atlas_hitReactLgtRight"]
    assert all(a["accessors"][s["output"]]["count"] == 1
               for clip in a["animations"][1:] for s in clip["samplers"])
    assert [c["target"] for c in a["animations"][0]["channels"]] == [c["target"] for c in b["animations"][0]["channels"]]
    abuf = (tmp_path / "n.glb").read_bytes()
    bbuf = (tmp_path / "b.glb").read_bytes()
    ab, bb = abuf[28 + struct.unpack_from("<I", abuf, 12)[0]:], bbuf[28 + struct.unpack_from("<I", bbuf, 12)[0]:]
    for ca, cb in zip(a["animations"][0]["channels"], b["animations"][0]["channels"]):
        qa = B.read_accessor(a, ab, a["animations"][0]["samplers"][ca["sampler"]]["output"])
        qb = B.read_accessor(b, bb, b["animations"][0]["samplers"][cb["sampler"]]["output"])
        assert np.max(angle(qa, qb[:len(qa)])) < 1e-4
    for ma, mb in zip(a["meshes"], b["meshes"]):
        pa, pb = ma["primitives"][0], mb["primitives"][0]
        assert np.allclose(B.read_accessor(a, ab, pa["attributes"]["POSITION"]), B.read_accessor(b, bb, pb["attributes"]["POSITION"]))


@pytest.mark.parametrize('authored', [True, False])
def test_attached_muzzle_in_rotated_joint_frame(fixture, tmp_path, authored):
    from tools.mw5_mechs.sockets import package_nodes
    skm = fixture[0]
    hull = json.loads(skm.read_text())
    # Add a cockpit so the fallback has a known horizontal forward axis.
    hull['nodes'].append({'name': 'Cockpit', 'translation': [0, 5, 10]})
    skm.write_text(json.dumps(hull))
    weapon = json.loads(skm.read_text())
    weapon['nodes'] = [{'name': 'Root'}, {'name': 'Fire00' if authored else 'tip',
                                        'translation': [2, 3, 30]}]
    weapon['materials'] = [{'name': 'weapon_a'}, {'name': 'weapon_b'}]
    for mesh in weapon['meshes']:
        for primitive in mesh['primitives']:
            primitive['material'] = 1
    wp = tmp_path / 'weapon.gltf'
    wp.write_text(json.dumps(weapon))
    out = tmp_path / 'assembled.glb'
    bone = 'Foot0_Left'
    doc = B.build(skm, tmp_path, out, 'albedo.png', attachments=[f'{wp}={bone}'])
    assert len(doc['materials']) == 3
    assert all(p['material'] == 2 for m in doc['meshes'][2:] for p in m['primitives'])
    row = dict(node='weapon_0', mesh='weapon', bone=bone, type=4, fires=['Fire00'])
    nodes, offsets, types, height = package_nodes(out, [row, dict(node=None)])
    assert nodes['weapon_0'] == 'j_LFoot' and types == {'weapon_0': 4}
    assert height == 15  # original hull bounds; weapons must not alter this
    frame = fk(hull['nodes'])[3]
    point = np.array([float(x) for x in offsets['weapon_0'].split()])
    if authored:
        assert np.allclose(point, [2, 3, 30])
    else:
        # Independent expected tip: transform every source vertex, select by +Z.
        verts = np.arange(18).reshape(6, 3)
        world = (np.c_[verts, np.ones(6)] @ frame.T)[:, :3]
        expected = verts[np.argmax(world[:, 2])]
        assert np.allclose(point, expected, atol=1e-4)
    assert all(len(n['name']) <= 24 for n in doc['nodes'])
    assert all(not any(s in m['name'].lower() for s in ('_explode', '_dmg', 'blip', 'indc', 'uix'))
               for m in doc['meshes'])
    from tools.mw5_mechs.build_pack import emit_ini
    ini = emit_ini('FITini\n[Bounds]\nf UpperLeftX = 1\nFITend', types)
    assert 'l NumWeapon = 1' in ini and 'l WeaponType = 4' in ini
    assert 'weapon_0' in ini and 'f UpperLeftX = 1' in ini
    with pytest.raises(ValueError, match='not unique'):
        package_nodes(out, [dict(row, mesh='missing')])
    with pytest.raises(ValueError, match='joint mismatch'):
        package_nodes(out, [dict(row, bone='Foot0_Right')])
    with pytest.raises(ValueError, match='duplicate'):
        package_nodes(out, [row, row])


def test_mirrored_weapon_attachments(fixture, tmp_path):
    from tools.mw5_mechs.sockets import package_nodes
    skm = fixture[0]
    weapon = json.loads(skm.read_text())
    weapon['nodes'] = [{'name': 'Fire00', 'translation': [2, 3, 30]}]
    wp = tmp_path / 'weapon.gltf'
    wp.write_text(json.dumps(weapon))
    bones = ['Foot0_Left', 'Foot0_Right']
    out = tmp_path / 'mirrored.glb'
    B.build(skm, tmp_path, out, 'albedo.png', attachments=[f'{wp}={b}' for b in bones])
    rows = [dict(node=f'weapon_{i}', mesh='weapon', bone=b, type=4, fires=['Fire00'])
            for i, b in enumerate(bones)]
    nodes, offsets, types, _ = package_nodes(out, rows)
    assert [nodes[r['node']] for r in rows] == ['j_LFoot', 'j_RFoot']
    assert offsets == {r['node']: '2.0000 3.0000 30.0000' for r in rows}
    assert types == {r['node']: 4 for r in rows}
    B.build(skm, tmp_path, out, 'albedo.png', attachments=[f'{wp}={bones[0]}'] * 2)
    with pytest.raises(ValueError, match='not unique'):
        package_nodes(out, rows[:1])


def test_embedded_native_attachment_uses_lod0(fixture, tmp_path):
    import base64
    from tools.mw5_mechs.sockets import load_glb
    skm = fixture[0]
    weapon = json.loads(skm.read_text())
    blob = (tmp_path / weapon['buffers'][0]['uri']).read_bytes()
    weapon['buffers'][0]['uri'] = 'data:application/octet-stream;base64,' + base64.b64encode(blob).decode()
    weapon['extras'] = {'bridge': []}
    weapon['meshes'].append(weapon['meshes'][0])  # retained LOD1 must not add triangles
    weapon['nodes'] += [{'children': [len(weapon['nodes']) + 1]}, {'mesh': 0}, {'mesh': 1}]
    weapon['scene'] = 1
    weapon['scenes'] = [{'nodes': [len(weapon['nodes']) - 1]}, {'nodes': [len(weapon['nodes']) - 3]}]
    wp = tmp_path / 'embedded.gltf'
    wp.write_text(json.dumps(weapon))
    out = tmp_path / 'embedded.glb'
    B.build(skm, tmp_path, out, 'albedo.png', attachments=[f'{wp}=Foot0_Left'])
    doc, buf = load_glb(out)
    attachment = doc['extras']['mw5Attachments'][0]
    assert attachment['joint'] == 'j_LFoot'
    primitives = [p for m in attachment['meshes'] for p in doc['meshes'][m]['primitives']]
    assert sum(len(B.read_accessor(doc, buf, p['indices'])) for p in primitives) == 6
    assert all(np.all(np.isfinite(B.read_accessor(doc, buf, p['attributes']['POSITION']))) for p in primitives)


def test_attachment_requires_resolved_bone(fixture, tmp_path):
    with pytest.raises(ValueError, match='bone not found'):
        B.build(fixture[0], tmp_path, tmp_path / 'bad.glb', 'albedo.png',
                attachments=[f'{fixture[0]}=unresolved'])


def test_recipe_entry_point_and_maps(fixture, tmp_path, monkeypatch):
    from tools.mw5_mechs import build_pack as P
    monkeypatch.setattr(P, 'logistics', lambda *a: None)
    skm = fixture[0]
    recipe = B.load_recipe()
    recipe["mc2"]["stem"] = "fixture_mech"
    recipe["mc2"]["model"].update(source_skm=skm.name, anim_dir=".", textures="textures",
                                     loadout="loadout.json", host_ini="fixture_host")
    recipe["bone_rename"]["Foot0_Left"] = "fixture_foot"
    recipe["clips"] = {"Stand": "fixture_idle"}
    recipe["hold_clips"] = {"fixture_hold": ["Stand", -1]}
    recipe_path = tmp_path / "recipe.json"
    recipe_path.write_text(json.dumps(recipe))
    (tmp_path / "loadout.json").write_text("[]")
    for tier in ("", "128"):
        folder = tmp_path / "textures" / tier
        folder.mkdir(parents=True, exist_ok=True)
        for tag in ("alb", "nrm"):
            (folder / f"fixture_mech_{tag}.ktx2").write_bytes(b"synthetic")
    install = tmp_path / "install"
    (install / "data/tgl").mkdir(parents=True)
    (install / "data/tgl/fixture_host.ini").write_text("FITini\n[Bounds]\nf UpperLeftX = 1\nFITend\n")
    monkeypatch.setattr(sys, "argv", ["build_pack", "--recipe", str(recipe_path), "--install", str(install)])
    P.main()
    from tools.mw5_mechs.sockets import load_glb
    tgl = install / "mods/mw5_mechs/data/tgl"
    doc, buf = load_glb(tgl / "fixture_mech.glb")
    assert "fixture_foot" in [n["name"] for n in doc["nodes"]]
    assert [a["name"] for a in doc["animations"]] == ["fixture_idle", "fixture_hold"]
    assert all(doc["accessors"][s["output"]]["count"] == 1 for s in doc["animations"][1]["samplers"])
    assert 'st AnimHost = "fixture_host"' in (tgl / "fixture_mech.ini").read_text()
    assert not (tgl / "fixture_host.ini").exists()
    pkg = json.loads((tgl / "fixture_mech.package.json").read_text())
    assert pkg["sourceGlb"] == "fixture_mech.glb"
    recipe["clips"]["Missing"] = "fixture_missing"
    recipe_path.write_text(json.dumps(recipe))
    with pytest.raises(ValueError, match="fixture_missing"):
        P.main()


def test_missile_attachment_type(fixture, tmp_path):
    from tools.mw5_mechs.sockets import package_nodes
    from tools.mw5_mechs.build_pack import emit_ini
    weapon = json.loads(fixture[0].read_text())
    weapon['nodes'] = [{'name': 'Fire00', 'translation': [2, 3, 30]}]
    wp, out = tmp_path / 'missile.gltf', tmp_path / 'missile.glb'
    wp.write_text(json.dumps(weapon))
    B.build(fixture[0], tmp_path, out, 'albedo.png', attachments=[f'{wp}=Foot0_Left'])
    _, _, types, _ = package_nodes(out, [dict(node='weapon_0', mesh='missile',
                                             bone='Foot0_Left', type=5, fires=['Fire00'])])
    assert types == {'weapon_0': 5}
    assert 'l WeaponType = 5' in emit_ini('FITini\nFITend', types)


def test_static_weapon_attachment(fixture, tmp_path):
    weapon = json.loads(fixture[0].read_text())
    del weapon['skins']
    attrs = weapon['meshes'][0]['primitives'][0]['attributes']
    del attrs['JOINTS_0'], attrs['WEIGHTS_0']
    wp = tmp_path / 'static.gltf'
    wp.write_text(json.dumps(weapon))
    doc = B.build(fixture[0], tmp_path, tmp_path / 'static.glb', 'albedo.png',
                  attachments=[f'{wp}=Foot0_Left'])
    entry = doc['extras']['mw5Attachments'][0]
    assert entry['joint'] == 'j_LFoot' and len(entry['meshes']) == 1
    mesh = entry['meshes'][0]
    assert doc['accessors'][doc['meshes'][mesh]['primitives'][0]['indices']]['count'] == 6
    assert doc['nodes'][doc['skins'][mesh]['joints'][0]]['name'] == 'j_LFoot'


def test_bridge_requires_animation_directory(fixture, tmp_path):
    with pytest.raises(ValueError, match='anim-dir.*bridge'):
        B.build(fixture[0], None, tmp_path / 'bad.glb', 'albedo.png')


def test_stride_uses_backward_midstance_instead_of_lower_swing_ankle(monkeypatch):
    from tools.mw5_mechs import parity as P
    # Both feet travel backwards at 4 u/s, then swing forwards at 12 u/s.
    # The opposite ankle's height must not decide which part is stance.
    foot = [[-4 * f / 30, f / 30, 0] for f in range(31)]
    foot += [[-4 + 12 * f / 30, 0, 0] for f in range(1, 11)]
    monkeypatch.setattr(P, 'foot_tracks', lambda *a: ([foot, foot], 4 / 3))
    asset = type('Asset', (), {'doc': {'animations': [{'name': 'atlas_moveCoreWalkFwd'}]}})()
    assert P.stride_speeds(asset)['atlas_moveCoreWalkFwd'] == pytest.approx(4)
    asset.doc['animations'][0]['name'] = 'atlas_moveCoreWalkBwd'
    assert P.stride_speeds(asset)['atlas_moveCoreWalkBwd'] == pytest.approx(12)
    monkeypatch.setattr(P, 'foot_tracks', lambda *a: ([[[0, 0, 0]] * 3] * 2, 1))
    with pytest.raises(ValueError, match='no planted-foot sweep'):
        P.stride_speeds(asset)


def test_parity_sample_normalizes_short_arc():
    from tools.mw5_mechs.parity import sample
    q = sample([0, 1], [[0, 0, 0, 1], [0, 0, -1, 0]], .5, True)
    assert np.linalg.norm(q) == pytest.approx(1)
    assert np.allclose(q, [0, 0, -np.sqrt(.5), np.sqrt(.5)])
    assert sample([0, 1], [[0, 0, 0, 1], [0, 0, 0, -1]], .5, True) == [0, 0, 0, 1]
    assert sample([0, 1], [[0, 2, 4], [2, 4, 6]], .5, False) == [1, 3, 5]


def test_distance_curve_preserves_variable_planted_travel(monkeypatch):
    from tools.mw5_mechs import parity as P
    foot = [[x, 0, 0] for x in (0, -.1, -.4, -.9, -1, -.5, 0)]
    monkeypatch.setattr(P, 'foot_tracks', lambda *a: ([foot, foot], 1))
    asset = type('Asset', (), {'doc': {'animations': [{'name': 'atlas_moveCoreWalkFwd'}]}})()
    duration, *distance = map(float, P.distance_curves(asset)['atlas_moveCoreWalkFwd'].split())
    assert duration == 1
    assert distance[:5] == pytest.approx([0, .1, .4, .9, 1])
    assert all(b > a for a, b in zip(distance, distance[1:]))
    assert np.all(np.diff(np.asarray(distance, dtype=np.float32)) > 0)


def test_faced_stride_ignores_larger_lateral_limp_sway(monkeypatch):
    from tools.mw5_mechs import parity as P
    foot = [[10 * z, 0, z] for z in (0, -.25, -.5, -.75, -1, -.5, 0)]
    monkeypatch.setattr(P, 'foot_tracks', lambda *a: ([foot, foot], 1))
    asset = type('Asset', (), {'doc': {'nodes': [{'name': 'mw5_forward'}],
        'animations': [{'name': 'atlas_moveCoreWalkLimpFwdRight'}]}})()
    duration, *distance = map(float, P.distance_curves(asset)['atlas_moveCoreWalkLimpFwdRight'].split())
    assert duration == 1
    assert distance[4] == pytest.approx(1)
    assert P.stride_speeds(asset)['atlas_moveCoreWalkLimpFwdRight'] == pytest.approx(1.5)


def test_shutdown_fallback_holds_own_stand(fixture, tmp_path):
    from tools.mw5_mechs.build_pack import bind_stationary
    doc = B.build(fixture[0], tmp_path, tmp_path / 'stand.glb', 'a.png')
    bind_stationary(doc)
    for name in ('atlas_shutdownPwroff', 'atlas_shutdownIdle', 'atlas_shutdownPwron'):
        clip = next(a for a in doc['animations'] if a['name'] == name)
        assert clip['channels']
        assert all(doc['accessors'][s[k]]['count'] == 1 for s in clip['samplers'] for k in ('input', 'output'))


def test_empty_native_hold_channel_names_source(fixture, tmp_path):
    g = json.loads(fixture[0].read_text())
    g['extras'] = {'bridge': []}
    g.update(scene=0, scenes=[{'nodes': [0, len(g['nodes'])]}])
    g['nodes'].append({'name': 'LOD0', 'mesh': 0})
    for typ in ('SCALAR', 'VEC4'):
        g['accessors'].append(dict(bufferView=0, componentType=5126, type=typ, count=0))
    g['animations'] = [dict(name='TBR_Stand_ANI', samplers=[dict(input=len(g['accessors'])-2,
                           output=len(g['accessors'])-1)],
                           channels=[dict(sampler=0, target=dict(node=1, path='rotation'))])]
    native = tmp_path / 'empty.gltf'
    native.write_text(json.dumps(g))
    recipe = B.load_recipe()
    recipe.update(clips={}, hold_clips={'empty_hold': ['Stand', -1]})
    with pytest.raises(ValueError, match='TBR_Stand_ANI.*Root.*empty'):
        B.build(native, None, tmp_path / 'bad.glb', 'albedo.png', recipe=recipe)


def test_purchase_unlock_table_matches_all_stock_missions():
    import csv
    import hashlib
    import os
    import re
    from tools.mc2x_import.fst import fst_entries, decompress_entry
    from tools.pak_dump import read_packets, decode_packet
    root = os.environ.get('MW5_STANDALONE_INSTALL')
    if not root:
        pytest.skip('requires stock FSTs and generated private packs')
    inst = Path(root)
    text = (Path(__file__).resolve().parents[2] / 'data/defs/purchase/unlocks.fit').read_text()
    table = dict((s, int(n)) for s, n in re.findall(r'st Unit\d+ = "([^"]+)"\s+l Mission\d+ = (\d+)', text))
    clan = set('direwolf executioner gargoyle kitfox maddog madcat puma shadowcat stormcrow summoner timberwolf warhawk clan_elemental'.split())
    def milestone(tons):
        return 1 if tons <= 35 else 3 if tons <= 55 else 6 if tons <= 75 else 9
    expected = {}
    for pack in ('mw5_mechs', 'mw5-vehicles'):
        for p in (inst/'mods'/pack/'data/objects').glob('*.csv'):
            rows = list(csv.reader(p.open(encoding='latin1')))
            tons = float(rows[2][4])
            expected[p.stem] = (18 if tons <= 55 else 19 if tons <= 75 else 22) if p.stem.removeprefix('mw5_') in clan else milestone(tons)
    packets = read_packets(inst/'data/objects/object2.pak')
    entries = fst_entries(inst/'art.fst')
    buildings = next(decompress_entry(o,c,r,l,d).decode('latin1') for o,c,r,n,l,d in entries if n.lower().endswith('/buildings.csv'))
    for row in csv.reader(buildings.splitlines()):
        if len(row) > 4 and row[3] == 'VEHICLE':
            fit = decode_packet(packets[int(row[4])]).decode('latin1')
            tons = float(re.search(r'CurTonnage\s*=\s*([\d.]+)', fit)[1])
            expected[row[0].lower()] = 18 if row[0].lower() == 'swarmlrmc' else milestone(tons)
    csvs = {Path(n).stem: decompress_entry(o,c,r,l,d).decode('latin1')
            for o,c,r,n,l,d in fst_entries(inst/'misc.fst') if n.lower().endswith('.csv')}
    for row in csv.reader(buildings.splitlines()):
        if len(row) > 5 and row[3] == 'MECH' and row[5] == 'HELICOPTER':
            expected[row[0].lower()] = milestone(float(list(csv.reader(csvs[row[0].lower()].splitlines()))[2][4]))
    assert len(expected) == 106
    assert table == expected
    stock = {}
    for o,c,r,n,l,d in fst_entries(inst/'mission.fst'):
        if re.search(r'/purchase\d\d.fit$', n):
            stock[int(n[-6:-4])] = set(re.findall(r'st Mech\d+\s*=\s*"([^"]+)"', decompress_entry(o,c,r,l,d).decode('latin1')))
    fingerprints = '19656e7c86ca 19656e7c86ca 1cac7d5d3373 efbf4ab1c0a6 a6a08811d35a a7e83723af3f a7e83723af3f a7e83723af3f 5cb34d144c62 faaa5c0f6dd2 b3b6dac31e6d b3b6dac31e6d a97d2d5040e6 993d25ada087 993d25ada087 993d25ada087 993d25ada087 b7070d511ac4 2a7c1ace822a 2a7c1ace822a 2a7c1ace822a 9549e1c32ffb 9549e1c32ffb 9549e1c32ffb'.split()
    for mission in range(1, 25):
        assert hashlib.sha256(','.join(sorted(stock[mission])).encode()).hexdigest()[:12] == fingerprints[mission-1]
        unlocked = stock[mission] | {s for s,n in table.items() if s.startswith('mw5_') or n <= mission}
        assert unlocked == stock[mission] | {s for s,n in expected.items() if s.startswith('mw5_') or n <= mission}
        assert 'mw5_direwolf' in unlocked
        assert ('hunter' in unlocked)
