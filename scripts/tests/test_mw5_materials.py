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


def test_cook_and_ktx_only_pack(tmp_path, monkeypatch, fixture):
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
    assert pkg["featureBits"] == ["HAS_NORMAL"] and set(pkg["materials"]) == {"albedo", "normal"}
    assert pkg["materials"]["normal"]["logicalFormat"] == "linear-normal-xyz"
    for mat in pkg["materials"].values():
        assert (tgl / mat["ktx2"]).read_bytes() == (tgl / "128" / mat["ktx2"]).read_bytes()
    assert not list(tgl.rglob("*.tga"))


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
