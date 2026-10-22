"""Content pipeline: IR round-trip, cache key, cook contract, validator, package."""
import base64
import json
import struct
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.content_pipeline import cache, gltf_io, ir, package, texcook, validate  # noqa: E402
from tools.content_pipeline.frontends import gltf_file, stock_ase  # noqa: E402


# --------------------------------------------------------------------------- fixtures

@pytest.fixture(autouse=True)
def private_cache(tmp_path, monkeypatch):
    monkeypatch.setattr(cache, "DEFAULT_DIR", tmp_path / "cook-cache")

def quat(axis, degrees):
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    h = np.radians(degrees) / 2
    return [*(axis * np.sin(h)), np.cos(h)]


def make_gltf(tmp_path, *, bad_normal=False):
    """Hierarchy + transforms + an instanced, mirrored mesh + MASK alpha + COLOR_0."""
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], "<f4")
    nrm = np.array([[0, 0, 1], [0, 0, 1], [0, 1, 0], [1, 0, 0]], "<f4")
    if bad_normal:
        nrm[0] = (0, 0, 2)
    tan = np.array([[1, 0, 0, 1]] * 4, "<f4")
    uv = np.array([[0, 0], [1, 0], [0, 1], [1, 1]], "<f4")
    col = np.array([[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [9, 9, 9, 255]], "u1")
    idx = np.array([0, 1, 2, 0, 2, 3], "<u2")
    blobs, views, accs = b"", [], []
    for arr, typ, comp, extra in ((pos, "VEC3", 5126, {"min": pos.min(0).tolist(), "max": pos.max(0).tolist()}),
                                  (nrm, "VEC3", 5126, {}), (tan, "VEC4", 5126, {}), (uv, "VEC2", 5126, {}),
                                  (col, "VEC4", 5121, {"normalized": True}), (idx, "SCALAR", 5123, {})):
        while len(blobs) % 4:
            blobs += b"\0"
        views.append({"buffer": 0, "byteOffset": len(blobs), "byteLength": arr.nbytes})
        accs.append({"bufferView": len(views) - 1, "componentType": comp, "count": len(arr.reshape(len(arr), -1)) if typ != "SCALAR" else arr.size, "type": typ, **extra})
        blobs += arr.tobytes()
    Image.new("RGBA", (8, 8), (200, 100, 50, 128)).save(tmp_path / "albedo.png")
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": "root", "translation": [10, 0, -5], "rotation": quat([0, 1, 0], 90), "scale": [2, 2, 2],
                      "children": [1, 2]},
                     {"name": "part_a", "mesh": 0, "translation": [0, 3, 0]},
                     {"name": "part_b_mirror", "mesh": 0, "matrix": [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 0, 0, 1]}],
           "meshes": [{"name": "panel", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2,
                                                                       "TEXCOORD_0": 3, "COLOR_0": 4},
                                                        "indices": 5, "material": 0}]}],
           "materials": [{"name": "mesh_metal", "alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": True,
                          "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                          "extras": {"mc2_material_class": "grating"}}],
           "textures": [{"source": 0}], "images": [{"uri": "albedo.png"}],
           "buffers": [{"byteLength": len(blobs), "uri": "data:application/octet-stream;base64," + base64.b64encode(blobs).decode()}],
           "bufferViews": views, "accessors": accs}
    path = tmp_path / "src.gltf"
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path, doc, pos.astype(float), nrm.astype(float), idx.reshape(-1, 3)


def expected_world(doc, pos, nrm):
    """Independent reference: compose glTF matrices by hand."""
    out = []
    root = gltf_io.node_matrix(doc["nodes"][0])
    for child in (1, 2):
        m = root @ gltf_io.node_matrix(doc["nodes"][child])
        p = pos @ m[:3, :3].T + m[:3, 3]
        n = nrm @ np.linalg.inv(m[:3, :3])
        out.append((p, n / np.linalg.norm(n, axis=1, keepdims=True), np.linalg.det(m[:3, :3]) < 0))
    return out


# --------------------------------------------------------------------------- IR round-trip

def test_ir_round_trip_preserves_transforms_normals_alpha(tmp_path):
    src, doc, pos, nrm, tris = make_gltf(tmp_path)
    asset = gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w")
    # Hierarchy and instancing survive in the IR (frame node + root + two children; one shared mesh).
    assert [n.name for n in asset.lod0().nodes] == ["source_frame", "root", "part_a", "part_b_mirror"]
    assert asset.lod0().nodes[2].mesh == asset.lod0().nodes[3].mesh == 0
    m = asset.materials[0]
    assert (m.alpha_mode, m.alpha_cutoff, m.double_sided, m.material_class) == ("MASK", 0.4, True, "grating")
    assert m.metallic == ir.CLASS_DEFAULTS["grating"][0]      # from the class, not the colour
    # JSON round-trip is lossless.
    ir.save(asset, tmp_path / "a.mc2ir.json")
    again = ir.load(tmp_path / "a.mc2ir.json")
    assert ir.fingerprint(again) == ir.fingerprint(asset)
    # Runtime GLB: baked transforms, identity nodes, valid; reading it back gives the same world geometry.
    glb = tmp_path / "rt.glb"
    glb.write_bytes(gltf_io.write_runtime_glb(again.lod0(), again.materials, {0: "a_panel.tga"}))
    assert validate.errors(validate.validate_runtime_glb(glb)) == []
    back, mats, _ = gltf_io.read_gltf(glb, frame="mc2_runtime", texture_roots=None)
    flat = back.flattened()
    assert mats[0].alpha_mode == "MASK" and mats[0].alpha_cutoff == pytest.approx(0.4)
    for (p_exp, n_exp, mirrored), (_, prim) in zip(expected_world(doc, pos, nrm), flat):
        assert np.allclose(prim.positions, p_exp, atol=1e-5)
        assert np.allclose(prim.normals, n_exp, atol=1e-5)
        assert np.array_equal(prim.indices, tris[:, [0, 2, 1]] if mirrored else tris)   # mirror keeps faces front
        assert np.allclose(prim.uv0[:, 1], 1 - np.array([0, 0, 1, 1]), atol=1e-6)        # TG v convention
        assert np.allclose(prim.color0[3], np.array([9, 9, 9, 255]) / 255, atol=1e-6)
        assert np.allclose(np.abs(prim.tangents[:, 3]), 1)


def test_runtime_frame_is_axis0_inverse(tmp_path):
    src, *_ = make_gltf(tmp_path)
    asset = gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w")
    glb = gltf_io.write_runtime_glb(asset.lod0(), asset.materials, {})
    doc, binary = gltf_io.split_glb(glb)
    raw = gltf_io.read_accessor(doc, [binary], doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])
    tg = asset.lod0().flattened()[0][1].positions
    assert np.allclose(raw, tg * [-1, -1, 1], atol=1e-5)


# --------------------------------------------------------------------------- cache key

def test_cache_key_is_content_not_name(tmp_path):
    a = tmp_path / "x.png"
    b = tmp_path / "renamed.png"
    Image.new("RGBA", (4, 4), (1, 2, 3, 4)).save(a)
    b.write_bytes(a.read_bytes())
    k = lambda p, params=None: cache.cache_key("c/1", params or {"role": "orm"}, [ir.sha256_file(p)])  # noqa: E731
    assert k(a) == k(b)
    Image.new("RGBA", (4, 4), (1, 2, 3, 5)).save(b)
    assert k(a) != k(b)
    assert k(a) != k(a, {"role": "normal"})
    assert cache.cache_key("c/2", {"role": "orm"}, [ir.sha256_file(a)]) != k(a)
    with pytest.raises(ValueError):
        cache.cache_key("c/1", {}, ["x.png"])


def test_cache_atomic_publish_and_lru(tmp_path):
    kind = cache.CacheKind("t-", ".bin", max_files=2)
    calls = []
    for key in (1, 2, 3):
        cache.cached(tmp_path, kind, key, lambda: calls.append(1) or b"data")
    assert cache.cached(tmp_path, kind, 3, lambda: b"other") == b"data"
    assert len(calls) == 3
    names = sorted(p.name for p in tmp_path.iterdir())
    assert len(names) == 2 and not any(".tmp" in n for n in names)


# --------------------------------------------------------------------------- cook contract

def test_tiers_are_real_downscales_and_never_enlarge():
    img = np.zeros((128, 64, 4), np.uint8)
    tiers = texcook.base_color_tiers(img)
    assert tiers[32].shape[:2] == (32, 16) and tiers[64].shape[:2] == (64, 32)
    assert tiers[256].shape[:2] == (128, 64) and tiers[1024].shape[:2] == (128, 64)


def test_normal_mips_unit_and_coverage_preserved():
    rng = np.random.default_rng(3)
    v = rng.normal(size=(16, 16, 3))
    v[..., 2] = np.abs(v[..., 2]) + 0.5
    v /= np.linalg.norm(v, axis=2, keepdims=True)
    rgba = np.dstack([np.rint((v * 0.5 + 0.5) * 255), np.full((16, 16), 255)]).astype(np.uint8)
    for lv in texcook.mip_chain(rgba, "normal")[1:]:
        n = lv[..., :3] / 255.0 * 2 - 1
        assert np.allclose(np.linalg.norm(n, axis=2), 1, atol=0.02)
    cut = np.full((64, 64, 4), 255, np.uint8)
    cut[..., 3] = (rng.random((64, 64)) ** 2 * 255).astype(np.uint8)   # sparse foliage-like coverage
    base = np.mean(cut[..., 3] >= 128)
    plain = texcook.mip_chain(cut, "orm")             # no preservation: coverage collapses
    chain = texcook.mip_chain(cut, "base_color", alpha_cutoff=0.5)
    assert abs(np.mean(plain[3][..., 3] >= 128) - base) > 0.2
    for lv in chain[1:4]:
        assert abs(np.mean(lv[..., 3] >= 128) - base) < 0.08


def test_ktx2_container_round_trip():
    img = (np.arange(8 * 8 * 4) % 251).astype(np.uint8).reshape(8, 8, 4)
    data = texcook.cook_ktx2(img, "orm", compress=False, cache_dir=None)
    vk, w, h, levels = texcook.read_ktx2(data)
    assert (vk, w, h, len(levels)) == (texcook.VK_RGBA8_UNORM, 8, 8, 4)
    assert levels[0] == img.tobytes()


# --------------------------------------------------------------------------- validator

def test_validator_rejects_broken_gltf(tmp_path):
    src, doc, *_ = make_gltf(tmp_path, bad_normal=True)
    codes = {c for s, c, _ in validate.validate_gltf(src) if s == "error"}
    assert "ATTR_UNIT" in codes
    doc["nodes"][1]["matrix"] = list(np.eye(4).reshape(-1))
    doc["accessors"][0]["max"] = [9, 9, 9]
    (tmp_path / "b.gltf").write_text(json.dumps(doc), encoding="utf-8")
    codes = {c for s, c, _ in validate.validate_gltf(tmp_path / "b.gltf") if s == "error"}
    assert {"NODE_MATRIX_TRS", "POSITION_BOUNDS"} <= codes
    # The runtime profile refuses unbaked node transforms.
    src2, *_ = make_gltf(tmp_path)
    assert "RUNTIME_NODE" in {c for _, c, _ in validate.validate_runtime_glb(src2)}


# --------------------------------------------------------------------------- IR rules

def test_class_rules_never_infer_metal():
    with pytest.raises(ValueError):
        ir.Asset("x", "building", "staticProp:x", [ir.Material("m", "painted_metal", metallic=0.5, reviewed=True)],
                 {"intact": [ir.Lod(ir.Scene())]}).check()
    with pytest.raises(ValueError):          # authored class without review
        ir.Asset("x", "building", "staticProp:x", [ir.Material("m", "concrete")], {"intact": [ir.Lod(ir.Scene())]}).check()
    glass = ir.Material.for_class("g", "glass", reviewed=True)
    assert (glass.metallic, glass.roughness) == ir.CLASS_DEFAULTS["glass"]


# --------------------------------------------------------------------------- package

def test_package_carries_destroyed_conditions_shadow_and_is_reproducible(tmp_path):
    src, *_ = make_gltf(tmp_path)
    def asset():
        return gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w",
                               destroyed=src, shadows={"intact": src}, conditions={"worn": {"intact": src}})
    out1 = package.build_package([asset()], tmp_path / "p1", mod_id="cp-test", compress=False)
    out2 = package.build_package([asset()], tmp_path / "p2", mod_id="cp-test", compress=False)
    rec = json.loads((out1 / "data/model_overrides/models.json").read_text())["overrides"][0]
    assert rec["destroyed"]["source"] == "panel/pristine_destroyed_lod0.glb"
    assert rec["shadow"]["intact"] == "panel/pristine_intact_shadow.glb"
    assert rec["conditions"]["worn"]["source"] == "panel/worn_intact_lod0.glb"
    assert all(r["class"] == "grating" for r in rec["materials"]["slots"])
    assert rec["materials"]["slots"][0]["alphaMode"] == "MASK"
    for t in (32, 128):
        assert (out1 / f"data/tgl/{t}/a_cp_panel_0.tga").is_file()
    assert package.check_package(out1) == []
    for f in (out1 / "data").rglob("*"):
        if f.is_file():
            assert f.read_bytes() == (out2 / f.relative_to(out1)).read_bytes(), f


def test_stock_detail_mask_keeps_legacy_regions(tmp_path):
    normal, rough = tmp_path / "n.png", tmp_path / "r.png"
    Image.new("RGB", (4, 4), (218, 128, 218)).save(normal)
    Image.new("L", (4, 4), 80).save(rough)
    row = {"slot": 0, "class": "rust", "reviewed": True, "license": "CC0", "provider": "ambientCG",
           "normal": str(normal), "roughnessMap": str(rough), "maskRects": [[0, 0, 0.5, 1]]}
    stock_ase.validate_slot(row)
    alpha = np.ones((4, 4), np.float32)
    alpha[0, 0] = 0                                    # a stock window texel
    row["excludeAlpha"] = True
    n, orm = stock_ase.cook_detail(row, 4, alpha)
    assert tuple(orm[1, 0]) == (255, 80, 0, 255)       # covered: neutral AO, authored rough, no metal
    assert tuple(orm[0, 3]) == (255, 255, 0, 0)        # outside the mask: coverage 0 = legacy
    assert orm[0, 0, 3] == 0                           # window excluded by stock alpha
    assert tuple(n[0, 3, :3]) == (128, 128, 255)
    for bad in ({"metallic": 0.5}, {"provider": "x"}, {"reviewed": False}, {"class": "legacy_prelit"}):
        with pytest.raises(ValueError):
            stock_ase.validate_slot({**row, **bad})
