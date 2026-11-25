"""Synthetic material proof. Trigger: tools/mw5_mechs/{bake_albedo,cook_textures,build_pack,build_glb}.py."""
import json
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mw5_mechs import bake_albedo as A, cook_textures as C, build_pack as P, build_glb as B
from scripts.tests.test_mw5_mechs import fixture  # noqa: F401


def test_linear_blend_zones_and_alpha(tmp_path):
    # Synthetic parameters, deliberately unlike any source-game palette.
    params = {"colorSpace": "linear", "parameters": {
        "MetalColorMultiplier": [0, 0, 0], "PaintColorMultiplier": [1, 1, 1],
        "PaintColorPrimary": [1, 0, 0], "PaintColorSecondary": [0, 1, 0], "PaintColorTertiary": [0, 0, 1]},
        "maskChannels": dict(R="PaintColorPrimary", G="PaintColorSecondary", B="PaintColorTertiary", A="opacity")}
    mask = tmp_path / "mask.png"
    Image.fromarray(np.array([[[0, 0, 0, 255], [255, 0, 0, 255], [255, 255, 0, 128],
                               [255, 0, 255, 64], [128, 0, 0, 0]]], np.uint8)).save(mask)
    out = np.asarray(A.bake(mask, 5, params))[2]
    assert out.tolist() == [[0, 0, 0, 255], [255, 0, 0, 255], [0, 255, 0, 128],
                            [0, 0, 255, 64], [188, 0, 0, 0]]
    params["colorSpace"] = "srgb"
    with pytest.raises(ValueError, match="linear"):
        A.bake(mask, 5, params)


def test_rg_normal_flip_and_reconstruction(tmp_path):
    src = tmp_path / "nrm.png"
    Image.fromarray(np.array([[[128, 128, 0, 0], [204, 230, 17, 255], [255, 255, 255, 0]]], np.uint8)).save(src)
    out = np.asarray(C.normal_image(src, 3))[1, :, :3] / 255
    assert np.allclose(out[0], [.5, .5, 1], atol=1/255)
    assert out[1, 0] > .75 and out[1, 1] < .15  # R used, G flipped, alpha ignored
    xyz = out * 2 - 1
    assert np.allclose(np.linalg.norm(xyz, axis=1), 1, atol=.01)
    assert np.all(xyz[:, 2] >= 0)


def test_pack_rest_sole_includes_toes_and_bind_transform(fixture, tmp_path):
    # Give the synthetic mesh a zero sole. Joint translations/rotations are nontrivial.
    skm = fixture[0]
    doc = json.loads(skm.read_text())
    binary = skm.with_suffix('.bin')
    buf = bytearray(binary.read_bytes())
    positions = B.read_accessor(doc, buf, doc['meshes'][0]['primitives'][0]['attributes']['POSITION'])
    positions[:, 1] -= 1
    binary.write_bytes(buf)
    glb = tmp_path / 'rest.glb'
    B.build(skm, tmp_path, glb, 'mw5_madcat_alb.png')
    g, data = P.load_glb(glb)
    assert abs(P.rest_sole_height(g, data)) <= .1
    # A descendant toe below the main foot must participate in the measurement.
    joint = g['skins'][0]['joints'][0]
    g['nodes'][joint]['name'] = 'toe'
    parent = next(n for n in g['nodes'] if joint in n.get('children', []))
    parent['children'] = [len(g['nodes']) if i == joint else i for i in parent['children']]
    g['nodes'].append({'name': 'j_LFoot', 'children': [joint]})
    assert abs(P.rest_sole_height(g, data)) <= .1
    g['nodes'][0]['translation'][1] += .3
    assert P.rest_sole_height(g, data) > .1


@pytest.mark.parametrize("orm", [False, True])
def test_cook_and_ktx_only_pack(tmp_path, monkeypatch, fixture, orm):
    alb, nrm = tmp_path / "alb.png", tmp_path / "nrm.png"
    Image.new("RGBA", (2, 2), (97, 53, 21, 255)).save(alb)
    Image.new("RGB", (2, 2), (128, 128, 0)).save(nrm)
    calls = []

    def encode(ktx, png, out, uastc):
        calls.append((ktx, Path(png).name, Image.open(png).size))
        Path(out).write_bytes(b"synthetic-cooked")
        return True

    monkeypatch.setattr(C, "cook_one_png", encode)
    cooked, scratch = tmp_path / "cooked", tmp_path / "scratch"
    C.cook("ktx.exe", alb, nrm, cooked, scratch)
    assert calls == [("ktx.exe", "mw5_madcat_alb.png", (2048, 2048)),
                     ("ktx.exe", "mw5_madcat_nrm.png", (2048, 2048))]
    if orm:
        for tier in ("", "128"):
            (cooked / tier / "mw5_madcat_orm.ktx2").write_bytes(b"synthetic-cooked")
    (cooked / "mw5_madcat_alb.png").write_bytes(alb.read_bytes())
    inst = tmp_path / "install"
    (inst / "data/tgl").mkdir(parents=True)
    (inst / "data/tgl/madcat.ini").write_text("FITini\n[Gestures]\nl NumGestures = 1\nFITend\n")
    glb = tmp_path / "test.glb"
    B.build(fixture[0], tmp_path, glb, "mw5_madcat_alb.png")
    loadout = tmp_path / "loadout.json"
    loadout.write_text("[]")
    monkeypatch.setattr(sys, "argv", ["build_pack", "--install", str(inst), "--glb", str(glb), "--textures", str(cooked), "--loadout", str(loadout)])
    P.main()
    tgl = inst / "mods/mw5_mechs/data/tgl"
    pkg = json.loads((tgl / "mw5_madcat.package.json").read_text())
    assert pkg["featureBits"] == ["HAS_NORMAL"]
    assert set(pkg["materials"]) == ({"albedo", "normal", "orm"} if orm else {"albedo", "normal"})
    assert pkg["materials"]["normal"]["logicalFormat"] == "linear-normal-xyz"
    for mat in pkg["materials"].values():
        assert (tgl / mat["ktx2"]).read_bytes() == (tgl / "128" / mat["ktx2"]).read_bytes()
    assert not list(tgl.rglob("*.tga"))
    doc, _ = P.load_glb(tgl / "mw5_madcat.glb")
    assert all((tgl / image["uri"]).is_file() for image in doc["images"])
    (cooked / "128/mw5_madcat_nrm.ktx2").unlink()
    with pytest.raises(FileNotFoundError):
        P.main()


def test_cook_uses_recipe_stem(tmp_path, monkeypatch):
    recipe = B.load_recipe()
    recipe['mc2']['stem'] = 'fixture_mech'
    alb, nrm = tmp_path / 'alb.png', tmp_path / 'nrm.png'
    Image.new('RGBA', (2, 2), (97, 53, 21, 255)).save(alb)
    Image.new('RGB', (2, 2), (128, 128, 0)).save(nrm)
    def encode(ktx, png, out, uastc):
        Path(out).write_bytes(b'synthetic-cooked')
        return True
    monkeypatch.setattr(C, 'cook_one_png', encode)
    deploy = tmp_path / 'cooked'
    C.cook('ktx.exe', alb, nrm, deploy, tmp_path / 'scratch', recipe=recipe)
    assert sorted(p.relative_to(deploy).as_posix() for p in deploy.rglob('*.ktx2')) == [
        '128/fixture_mech_alb.ktx2', '128/fixture_mech_nrm.ktx2',
        'fixture_mech_alb.ktx2', 'fixture_mech_nrm.ktx2']


def test_source_material_survives_rigid_split(fixture, tmp_path):
    skm = fixture[0]
    g = json.loads(skm.read_text())
    g["materials"] = [{"name": "dummy_material_0"}, {"name": "dummy_material_1"}]
    pr = g["meshes"][0]["primitives"][0]
    pr["material"] = 0
    g["meshes"][0]["primitives"].append(dict(pr, material=1))
    skm.write_text(json.dumps(g))
    d = B.build(skm, tmp_path, tmp_path / "out.glb", "mw5_madcat_alb.tga")
    assert [m["name"] for m in d["materials"]] == ["mw5_body", "mw5_variant"]
    assert all([p["material"] for p in m["primitives"]] == [0, 1] for m in d["meshes"])


def _synthetic_params():
    return {"scalars": {"PaintWear": 2, "PaintWearTightness": 26, "DirtStrength": 0, "GrimeMultiplier": -1, "DirtRoughness": 1,
                        "PaintRoughness": 1, "PaintRoughnessMultiplier": 2, "ScratchRoughnessMultiplier": .2,
                        "GrimeRoughnessMultiplier": .4, "MetalDetailContrast": -.2, "PaintDetailContrast": .3,
                        "MetalTilingScale": 2, "PaintTilingScale": 1},
            "vectors": {"DirtColor": [0, 0, 0], "GrimeColor": [0, 0, 0]},
            "paint": {"MetalColorMultiplier": [.5, .5, .5], "PaintColorMultiplier": [1, 1, 1], "PaintColorPrimary": [.1, .2, .3],
                      "PaintColorSecondary": [0, 0, 0], "PaintColorTertiary": [0, 0, 0]},
            "metals": {"A": {"roughness": 2.9, "metallic": 1}, "B": {"roughness": .25, "metallic": .5}},
            "metalId": {"000": "A", "010": "B"}}


def test_bake_material_channels(monkeypatch):
    """Paint is rough non-metal; bare metal takes its metalID material's roughness (clamped to 1) and metallic;
    a strong scratch exposes metal through paint; the normal map stays unit length."""
    from tools.mw5_mechs import bake_material as M
    monkeypatch.setattr(M, "SIZE", 4)
    # pixels: paint, bare metal id 000, bare metal id 010, paint with a strong scratch
    mask = np.zeros((4, 4, 4), np.float32); mask[..., 3] = 1; mask[0, 0, 0] = 1; mask[0, 3, 0] = 1
    mid = np.zeros((4, 4, 4), np.float32); mid[0, 2, 1] = 1; mid[0, 2, 3] = 1
    wear = np.zeros((4, 4, 3), np.float32); wear[0, 3, 0] = 1
    nrm = np.full((4, 4, 3), .5, np.float32)
    detail = (np.full((8, 8, 3), .5, np.float32),) * 2
    alb, orm, nm = (np.asarray(i) / 255 for i in M.bake(mask, mid, wear, nrm, _synthetic_params(), detail))
    assert alb.shape == orm.shape == nm.shape == (4, 4, 4)
    rough, metal = orm[0, :, 1], orm[0, :, 2]
    assert metal[0] == 0 and abs(rough[0] - 1) < .01                  # paint
    assert abs(metal[1] - 1) < .01 and abs(rough[1] - 1) < .01        # id 000: roughness 2.9 clamps to 1
    assert abs(metal[2] - .5) < .01 and abs(rough[2] - .25) < .01     # id 010
    assert metal[3] == 1 or abs(metal[3] - 1) < .01                   # scratched paint is bare metal
    assert rough[3] < .3                                              # and shiny (ScratchRoughnessMultiplier)
    assert np.allclose(orm[..., 0], 1) and np.allclose(np.linalg.norm(nm[..., :3] * 2 - 1, axis=-1), 1, atol=.03)


def test_pack_texture_references_resolve():
    """Every texture a built pack's package.json names exists in both size tiers as a 2048 BC7 KTX2."""
    import os
    pack = Path(os.environ.get("MW5_PACK_DIR", "mw5-pack-not-set"))
    if not (pack / "mw5_madcat.package.json").is_file():
        pytest.skip("no built MW5 pack here")
    pkg = json.loads((pack / "mw5_madcat.package.json").read_text())
    assert {"albedo", "normal", "orm"} <= set(pkg["materials"])
    for mat in pkg["materials"].values():
        for tier in ("", "128"):
            head = (pack / tier / mat["ktx2"]).read_bytes()[:28]
            assert head[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
            vk_format, _, width, height = np.frombuffer(head[12:28], "<u4")
            assert (vk_format, width, height) == (145, 2048, 2048)    # BC7 UNORM; the cook is linear-tagged for data maps


def _stm(path, tris):
    """Static mesh with one primitive per entry of tris (a triangle count each), UV0 spread over the atlas."""
    blob, views, accs, prims = bytearray(), [], [], []

    def add(arr, ctype, typ):
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes})
        blob.extend(arr.tobytes())
        accs.append({"bufferView": len(views) - 1, "componentType": ctype, "count": len(arr), "type": typ})
        return len(accs) - 1
    for n in tris:
        pos = np.random.default_rng(n).random((n * 3, 3)).astype(np.float32)
        prims.append({"attributes": {"POSITION": add(pos, 5126, "VEC3"), "NORMAL": add(np.tile(np.float32([0, 1, 0]), (n * 3, 1)), 5126, "VEC3"),
                                     "TEXCOORD_0": add(pos[:, :2].copy(), 5126, "VEC2")},
                      "indices": add(np.arange(n * 3, dtype=np.uint32), 5125, "SCALAR")})
    (path.with_suffix(".bin")).write_bytes(bytes(blob))
    path.write_text(json.dumps({"buffers": [{"uri": path.with_suffix(".bin").name, "byteLength": len(blob)}], "bufferViews": views,
                                "accessors": accs, "meshes": [{"primitives": prims}]}))


def test_cockpit_hull_and_glass_added(tmp_path):
    from tools.mw5_mechs import add_cockpit as K
    from tools.mw5_mechs.sockets import load_glb
    recipe = B.load_recipe()
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": recipe["cockpit"]["bone"]}],
           "meshes": [], "skins": [], "materials": [{"name": "mw5_body"}], "accessors": [], "bufferViews": [], "buffers": [{"byteLength": 0}]}
    js = json.dumps(doc).encode()
    js += b" " * (-len(js) % 4)
    import struct
    (tmp_path / "in.glb").write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js)) + struct.pack("<I4s", len(js), b"JSON") + js + struct.pack("<I4s", 0, b"BIN\0"))
    _stm(tmp_path / "hull.gltf", [3, 8])
    _stm(tmp_path / "glass.gltf", [5])
    K.add_cockpit(tmp_path / "in.glb", tmp_path / "out.glb", tmp_path / "hull.gltf", tmp_path / "glass.gltf", recipe)
    g, buf = load_glb(tmp_path / "out.glb")
    by = {m["name"]: m for m in g["meshes"]}
    assert len(by["cockpit_hull"]["primitives"]) == 1
    assert g["accessors"][by["cockpit_hull"]["primitives"][0]["indices"]]["count"] == 24  # biggest primitive is the nose
    assert g["accessors"][by["cockpit_glass"]["primitives"][0]["indices"]]["count"] == 24  # 3 window + 5 pane triangles
    glass_mat = g["materials"][by["cockpit_glass"]["primitives"][0]["material"]]
    assert glass_mat["name"] == "mw5_glass" and glass_mat["pbrMetallicRoughness"]["roughnessFactor"] < .1
    uv = B.read_accessor(g, buf, by["cockpit_glass"]["primitives"][0]["attributes"]["TEXCOORD_0"])
    x, y, size = recipe["cockpit"]["glass"]["tile"]
    assert ((uv * K.ATLAS >= [x, y]) & (uv * K.ATLAS <= [x + size, y + size])).all()
    assert by["cockpit_hull"]["primitives"][0]["material"] == 0
    assert all(len(s["joints"]) == 1 for s in g["skins"])
    K.add_cockpit(tmp_path / "in.glb", tmp_path / "framed.glb", tmp_path / "hull.gltf", None, recipe)
    framed, data = load_glb(tmp_path / "framed.glb")
    assert len(framed["meshes"]) == len(framed["skins"]) == 1
    prim = framed["meshes"][0]["primitives"][0]
    assert framed["accessors"][prim["indices"]]["count"] == 33  # frame and its three window triangles
    uv = B.read_accessor(framed, data, prim["attributes"]["TEXCOORD_0"])
    assert np.allclose(uv[-9:], K.glass_uv(recipe["cockpit"]["glass"]["tile"]))


def test_glass_stamp_fills_only_the_tile(tmp_path):
    from tools.mw5_mechs import add_cockpit as K
    recipe = B.load_recipe()
    for tag in ("alb", "nrm", "orm"):
        Image.new("RGB", (K.ATLAS, K.ATLAS), (200, 200, 200)).save(tmp_path / f"{tag}.png")
    K.stamp(tmp_path, recipe)
    x, y, size = recipe["cockpit"]["glass"]["tile"]
    for tag in ("alb", "nrm", "orm"):
        a = np.asarray(Image.open(tmp_path / f"{tag}.png").convert("RGB"))
        inside = a[y:y + size, x:x + size]
        assert (inside == inside[0, 0]).all() and not (inside[0, 0] == 200).all()
        assert (a.sum() - inside.sum()) == 200 * 3 * (K.ATLAS ** 2 - size * size)
    orm = np.asarray(Image.open(tmp_path / "orm.png").convert("RGB"))[y, x]
    assert orm[1] < 30 and orm[2] > 150  # glossy and reflective


READY_GLB = Path(__import__("os").environ.get("MW5_PACK_DIR", "mw5-pack-not-set")) / "mw5_madcat.glb"


@pytest.mark.skipif(not READY_GLB.is_file(), reason="owner pack not built here")
def test_ready_pack_has_cockpit_and_glass_uvs_clear_of_other_parts():
    from tools.mw5_mechs import add_cockpit as K
    from tools.mw5_mechs.sockets import load_glb
    g, buf = load_glb(READY_GLB)
    x, y, size = B.load_recipe()["cockpit"]["glass"]["tile"]
    names = {m["name"] for m in g["meshes"]}
    assert {"cockpit_hull", "cockpit_glass"} <= names
    for m in g["meshes"]:
        for pr in m["primitives"]:
            assert "material" in pr
            if m["name"] == "cockpit_glass" or m["name"].startswith("wp"):
                continue  # weapon parts tile their own out-of-range UVs
            uv = B.read_accessor(g, buf, pr["attributes"]["TEXCOORD_0"])[B.read_accessor(g, buf, pr["indices"]).ravel()] * K.ATLAS
            a_, b_, c_ = uv.reshape(-1, 3, 2).transpose(1, 0, 2)
            den = (b_[:, 1] - c_[:, 1]) * (a_[:, 0] - c_[:, 0]) + (c_[:, 0] - b_[:, 0]) * (a_[:, 1] - c_[:, 1])
            for px, py in ((x, y), (x + size, y), (x, y + size), (x + size, y + size), (x + size / 2, y + size / 2)):
                u = ((b_[:, 1] - c_[:, 1]) * (px - c_[:, 0]) + (c_[:, 0] - b_[:, 0]) * (py - c_[:, 1])) / np.where(den == 0, 1, den)
                v = ((c_[:, 1] - a_[:, 1]) * (px - c_[:, 0]) + (a_[:, 0] - c_[:, 0]) * (py - c_[:, 1])) / np.where(den == 0, 1, den)
                assert not ((den != 0) & (u >= 0) & (v >= 0) & (u + v <= 1)).any(), f"{m['name']} UVs cover the glass tile"


def test_extras_streaks_flow_down_and_decal_lands(tmp_path):
    from tools.mw5_mechs import bake_extras as E
    # one vertical wall facing +Z: world up (Y) maps to image up (-row), so downhill in UV is +row
    pos = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], np.float64)
    uv = np.array([[.1, .9], [.9, .9], [.9, .1], [.1, .1]])
    n = np.tile([0, 0, 1.0], (4, 1))
    part = (pos, n, uv, np.array([[0, 1, 2], [0, 2, 3]]))
    p, nr, cov, down = E.rasterize([part], size=256)
    assert cov[128, 128] and (down[128, 128, 1] > .9) and abs(down[128, 128, 0]) < .1
    seeds_ledge = nr.copy(); seeds_ledge[..., 1] = .9    # pretend the wall is a ledge so streaks seed
    s = E.streaks(seeds_ledge, cov, down, 1, steps=40, density=.05)
    assert s.sum() > 0 and s[:128].sum() < s[128:].sum() * 5   # nothing above the seed band flows up
    atlas = tmp_path / "atlas.png"
    a = np.zeros((64, 64, 4), np.uint8); a[8:56, 8:56] = 255
    Image.fromarray(a).save(atlas)
    alb = np.full((256, 256, 4), 20, np.uint8)
    orm = np.full((256, 256, 4), 200, np.uint8)
    d = [{"name": "t", "cell": (0, 0, 64, 64), "axis": (0, 0, -1), "at": (.5, .5, 0), "size": (.5, .5), "depth": .5}]
    alb2, orm2 = E.decals((p, nr, cov, down), alb, orm, atlas, d)
    assert alb2[128, 128, 0] > 200 and alb2[5, 5, 0] == 20


def test_trim_proud_glass_drops_only_the_piece_above_the_hull(tmp_path):
    from tools.mw5_mechs import add_cockpit as K
    from tools.mw5_mechs.sockets import load_glb
    import struct
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0],            # hull, top y=1
                    [0, 0, 0], [1, 0, 0], [0, .5, 0],           # glass piece inside
                    [2, 0, 0], [3, 0, 0], [2, 2, 0]], np.float32)   # glass piece proud
    blob = pos.tobytes() + np.array([0, 1, 2, 0, 1, 2, 3, 4, 5], np.uint32).tobytes()
    doc = {"asset": {"version": "2.0"}, "buffers": [{"byteLength": len(blob)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 108}, {"buffer": 0, "byteOffset": 108, "byteLength": 36}],
           "accessors": [{"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                         {"bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 6, "type": "VEC3"},
                         {"bufferView": 1, "byteOffset": 0, "componentType": 5125, "count": 3, "type": "SCALAR"},
                         {"bufferView": 1, "byteOffset": 12, "componentType": 5125, "count": 6, "type": "SCALAR"}],
           "meshes": [{"name": "cockpit_hull", "primitives": [{"attributes": {"POSITION": 0}, "indices": 2}]},
                      {"name": "cockpit_glass", "primitives": [{"attributes": {"POSITION": 1}, "indices": 3}]}]}
    js = json.dumps(doc).encode()
    js += b" " * (-len(js) % 4)
    (tmp_path / "in.glb").write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON")
                                      + js + struct.pack("<I4s", len(blob), b"BIN\0") + blob)
    assert K.trim_proud_glass(tmp_path / "in.glb", tmp_path / "out.glb") == 1
    g, buf = load_glb(tmp_path / "out.glb")
    assert B.read_accessor(g, buf, g["meshes"][1]["primitives"][0]["indices"]).tolist() == [0, 1, 2]
