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
    wp = tmp_path / 'weapon.gltf'
    wp.write_text(json.dumps(weapon))
    out = tmp_path / 'assembled.glb'
    bone = 'Foot0_Left'
    doc = B.build(skm, tmp_path, out, 'albedo.png', attachments=[f'{wp}={bone}'])
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


def test_attachment_requires_resolved_bone(fixture, tmp_path):
    with pytest.raises(ValueError, match='bone not found'):
        B.build(fixture[0], tmp_path, tmp_path / 'bad.glb', 'albedo.png',
                attachments=[f'{fixture[0]}=unresolved'])
