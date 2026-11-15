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
