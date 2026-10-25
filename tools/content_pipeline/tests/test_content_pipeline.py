"""Content pipeline: IR round-trip, cache key, cook contract, validator, package."""
import base64
import json
import re
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
    rt, _ = gltf_io.split_glb(glb.read_bytes())
    assert len({n["name"] for n in rt["nodes"]}) == len(rt["nodes"])   # importer keys shapes by node name
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


def test_animated_material_splits_share_pivot_and_parent(tmp_path):
    src, doc, *_ = make_gltf(tmp_path)
    doc["nodes"] = [{"name": "Coil", "mesh": 0, "translation": [0, 6.9, 0],
                     "extras": {"mc2_anim_node": "Coil"}}]
    doc["meshes"][0]["primitives"] *= 2
    src.write_text(json.dumps(doc))
    asset = gltf_file.to_ir("generator", "staticProp:generator", src,
                            meta={"frame": "gltf", "wu_per_unit": 4}, work_dir=tmp_path / "w")
    scene = asset.lod0()
    for node, world in zip(scene.nodes, scene.world_matrices()):
        if node.extras.get("mc2_anim_node"):
            node.extras["mc2_pivot_tg_wu"] = world[:3, 3].tolist()
    data = gltf_io.write_runtime_glb(scene, asset.materials, {0: "albedo.png"})
    runtime, _ = gltf_io.split_glb(data)
    coil, split = runtime["nodes"][:2]
    assert coil["name"] == "Coil" and split["name"] == "Coil_1"
    assert coil["children"] == [1]
    wrapper = runtime["nodes"][runtime["scenes"][0]["nodes"][0]]
    assert wrapper["name"] == "MC2Scene" and wrapper["children"] == [0]
    assert coil["translation"] == pytest.approx([0, -27.6, 0])
    assert split["translation"] == [0, 0, 0]
    path = tmp_path / "runtime.glb"
    path.write_bytes(data)
    assert validate.errors(validate.validate_runtime_glb(path)) == []
    back, _, _ = gltf_io.read_gltf(path, frame="mc2_runtime")
    for (_, expected), (_, actual) in zip(scene.flattened(), back.flattened()):
        assert actual.positions == pytest.approx(expected.positions)
    # A split with another transform would break the shared rotation origin.
    _, binary = gltf_io.split_glb(data)
    for corrupt_node in (1, 2):  # moved material split, then non-identity scene wrapper
        corrupt = json.loads(json.dumps(runtime))
        corrupt["nodes"][corrupt_node]["translation"] = [1, 0, 0]
        body = json.dumps(corrupt).encode()
        body += b" " * (-len(body) % 4)
        path.write_bytes(struct.pack("<III", gltf_io.GLB_MAGIC, 2, 28 + len(body) + len(binary))
                         + struct.pack("<II", len(body), gltf_io.CHUNK_JSON) + body
                         + struct.pack("<II", len(binary), gltf_io.CHUNK_BIN) + binary)
        assert any("RUNTIME_NODE" in e for e in validate.errors(validate.validate_runtime_glb(path)))


@pytest.mark.parametrize("intermediary", [False, True])
@pytest.mark.parametrize("parent_mesh", [None, 0])
def test_nested_animated_chain_matches_stock_over_clip(tmp_path, intermediary, parent_mesh):
    from tools.anim_parity.audit import document_model, pose_matrices, rotation
    fixture = json.loads((Path(__file__).parents[2] / "anim_parity/nested_chain.json").read_text())
    parent, child = fixture["parent"], fixture["child"]
    points = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 1]], float)
    prim = ir.Primitive(points, np.array([[0, 1, 2]]), 0)
    scene = ir.Scene(meshes=[ir.Mesh("probe", [prim, prim])])
    pm, cm = np.eye(4), np.eye(4)
    pm[:3, 3], cm[:3, 3] = parent["pivot"], child["offset"]
    scene.nodes.append(ir.Node("Yaw", matrix=pm, mesh=parent_mesh, extras={"mc2_anim_node": "Yaw"}))
    if intermediary:
        scene.nodes.append(ir.Node("Static", parent=0))
    scene.nodes.append(ir.Node("Pitch", parent=len(scene.nodes)-1, matrix=cm, mesh=0,
                               extras={"mc2_anim_node": "Pitch"}))
    detail = np.eye(4)
    detail[:3, 3] = [1, 2, 3]
    scene.nodes.append(ir.Node("Detail", parent=len(scene.nodes)-1, matrix=detail, mesh=0))
    raw = gltf_io.write_runtime_glb(scene, [ir.Material("probe")], {})
    doc, binary = gltf_io.split_glb(raw)
    root_index = doc["scenes"][0]["nodes"][0]
    assert doc["nodes"][root_index] == {"name": "MC2Scene", "children": [0]}
    path = tmp_path / "nested.glb"
    path.write_bytes(raw)
    assert not validate.errors(validate.validate_runtime_glb(path))
    cooked = document_model(doc, [binary], gltf_io.TG_FROM_RUNTIME)
    stock = {"Yaw": {"parent": "None", "pivot": np.array(parent["pivot"])},
             "Pitch": {"parent": "Yaw", "pivot": np.array(parent["pivot"]) + child["offset"]}}
    tracks = {row["name"]: dict(frames=5, q=np.array([quat(row["axis"], a) for a in row["degrees"]]),
                                p=np.empty((0, 3))) for row in (parent, child)}
    for frame in range(5):
        expected = pm.copy()
        expected[:3, :3] = rotation(tracks["Yaw"]["q"][frame])
        local = cm.copy()
        local[:3, :3] = rotation(tracks["Pitch"]["q"][frame])
        expected = expected @ local
        actual = pose_matrices(cooked, tracks, frame / 4)
        np.testing.assert_allclose(actual["Pitch"], expected, atol=1e-6)
        np.testing.assert_allclose(actual["Pitch"], pose_matrices(stock, tracks, frame / 4)["Pitch"], atol=1e-6)
        np.testing.assert_allclose(actual["Pitch_1"], expected, atol=1e-6)
        np.testing.assert_allclose(actual["Detail"], expected @ detail, atol=1e-6)
        np.testing.assert_allclose(actual["Detail_1"], expected @ detail, atol=1e-6)


def test_rotated_scaled_attachment_under_animated_parent(tmp_path):
    points = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 1]], float)
    scene = ir.Scene(meshes=[ir.Mesh("probe", [ir.Primitive(points, np.array([[0, 1, 2]]), 0)])])
    parent = np.array([[0, -2, 0, 0], [3, 0, 0, 0], [0, 0, 4, 0], [0, 0, 0, 1]], float)
    parent[:3, 3] = [5, 6, 7]
    attachment = np.array([[0, -3, 0, 1], [2, 0, 0, 2], [0, 0, 4, 3], [0, 0, 0, 1]], float)
    scene.nodes = [ir.Node("Yaw", matrix=parent, mesh=0, extras={"mc2_anim_node": "Yaw"}),
                   ir.Node("Lamp", parent=0, matrix=attachment, extras={"mc2_attachment": "light"})]
    raw = gltf_io.write_runtime_glb(scene, [ir.Material("probe")], {})
    doc, _ = gltf_io.split_glb(raw)
    lamp = next(n for n in doc["nodes"] if n["name"] == "Lamp")
    yaw = next(n for n in doc["nodes"] if n["name"] == "Yaw")
    assert doc["nodes"].index(lamp) in yaw["children"]
    expected = gltf_io.TG_FROM_RUNTIME @ parent @ attachment
    actual = gltf_io.node_matrix(yaw) @ gltf_io.node_matrix(lamp)
    np.testing.assert_allclose(actual, expected)
    path = tmp_path / "attachment.glb"
    path.write_bytes(raw)
    assert not validate.errors(validate.validate_runtime_glb(path))


def test_runtime_frame_is_axis0_inverse(tmp_path):
    src, *_ = make_gltf(tmp_path)
    asset = gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w")
    glb = gltf_io.write_runtime_glb(asset.lod0(), asset.materials, {})
    doc, binary = gltf_io.split_glb(glb)
    raw = gltf_io.read_accessor(doc, [binary], doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])
    tg = asset.lod0().flattened()[0][1].positions
    assert np.allclose(raw, tg * [-1, -1, 1], atol=1e-5)


def test_runtime_prunes_unreferenced_vertices_without_changing_corners(tmp_path):
    src, *_ = make_gltf(tmp_path)
    asset = gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w")
    prim = asset.lod0().meshes[0].primitives[0]
    prim.indices = np.array([[3, 0, 2]])
    expected = asset.lod0().flattened()[0][1]
    doc, binary = gltf_io.split_glb(gltf_io.write_runtime_glb(asset.lod0(), asset.materials, {}))
    output = doc["meshes"][0]["primitives"][0]
    idx = gltf_io.read_accessor(doc, [binary], output["indices"]).reshape(-1)
    for key, values in (("POSITION", expected.positions * [-1, -1, 1]),
                        ("COLOR_0", expected.color0)):
        actual = gltf_io.read_accessor(doc, [binary], output["attributes"][key])
        assert len(actual) == 3
        assert np.allclose(actual[idx], values[expected.indices.reshape(-1)], atol=1e-5)


def test_authored_image_rows_match_native_tg_uvs(tmp_path):
    # Assimp glTF2Importer flips V, then toMC2V flips it back. Preserve the
    # original glTF/PIL row order; a uniform-colour fixture cannot test this.
    src, *_ = make_gltf(tmp_path)
    pixels = np.full((8, 8, 4), 255, np.uint8)
    pixels[:4, :, :3] = [220, 30, 10]
    pixels[4:, :, :3] = [10, 50, 230]
    Image.fromarray(pixels).save(tmp_path / "albedo.png")
    asset = gltf_file.to_ir("panel", "staticProp:panel", src, meta={"frame": "gltf"}, work_dir=tmp_path / "w")
    out = package.build_package([asset], tmp_path / "out", mod_id="uv", compress=False)
    record = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"][0]
    doc, binary = gltf_io.split_glb((out / "data/model_overrides" / record["source"]).read_bytes())
    texture = texcook.load_rgba(next((out / "data/tgl/32").glob("*.ktx2")))
    # glTF V=.25 -> Assimp .75 -> toMC2V .25 -> top/red half.
    assert tuple(texture[2, 2, :3]) == (220, 30, 10)
    assert tuple(texture[6, 2, :3]) == (10, 50, 230)
    raw = gltf_io.read_accessor(doc, [binary], doc["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"])
    assert np.allclose(raw[:, 1], [0, 0, 1, 1])


@pytest.mark.parametrize("light_mesh", [None, 0])
def test_kit_light_and_destroyed_lods_survive_cook(tmp_path, light_mesh):
    from tools.content_pipeline.cook import build_asset
    src, doc, *_ = make_gltf(tmp_path)
    doc["nodes"][0]["children"].append(3)
    doc["nodes"].append({"name": "LIGHT_door_01", "translation": [1, 2, 3], "extras": {
        "mc2_light_schema": "mc2-building-light/1", "mc2_light_type": "spot",
        "mc2_light_position_gltf_m": [1, 2, 3], "mc2_light_direction_gltf": [0, 0, -1],
        "mc2_light_colour_linear": [1, 0.8, 0.5], "mc2_light_intensity_class": "door",
        "mc2_light_range_m": 5, "mc2_light_outer_cone_degrees": 60,
        "mc2_light_active": True, "mc2_light_lights_out_on_destroy": True}})
    if light_mesh is not None:
        doc["nodes"][-1]["mesh"] = light_mesh
    src.write_text(json.dumps(doc))
    dead = tmp_path / "dead.gltf"
    doc["nodes"][0]["children"].remove(3)
    doc["nodes"].pop()
    dead.write_text(json.dumps(doc))
    spec = {"frontend": "gltf", "id": "kit", "replaces": "staticProp:hangar", "intact": str(src),
            "wu_per_unit": 4, "destroyed": {"intact": str(dead), "lods": [[str(dead), 100]]}}
    asset = build_asset(spec, {}, tmp_path / "work")
    again = ir.from_json(ir.to_json(asset)).check()
    assert again.sockets[0].kind == "light"
    assert again.sockets[0].properties["position_tg_wu"] == [4, 8, 12]
    assert again.sockets[0].properties["range_wu"] == 20
    out = package.build_package([again], tmp_path / "out", mod_id="kit", compress=False)
    rec = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"][0]
    assert rec["lightSockets"][0]["direction_tg"] == [0, 0, -1]
    assert "lightSockets" not in rec["destroyed"]
    assert len(rec["destroyed"]["lods"]) == 1
    runtime, _ = gltf_io.split_glb((out / "data/model_overrides" / rec["source"]).read_bytes())
    light = next(n for n in runtime["nodes"] if n["name"] == "LIGHT_door_01")
    assert light["extras"]["position_tg_wu"] == [4, 8, 12]


def test_geometry_cache_tracks_source_and_cooker_across_work_dirs(tmp_path, monkeypatch):
    src, doc, *_ = make_gltf(tmp_path)
    def run(work, output):
        asset = gltf_file.to_ir("panel", "staticProp:panel", src, work_dir=tmp_path / work)
        out = package.build_package([asset], tmp_path / output, mod_id="panel", compress=False)
        return json.loads((out / "content/report.json").read_text())["assets"][0]["geometry_cache"]
    first = run("w1", "p1")["pristine_intact_lod0"]
    second = run("w2", "p2")["pristine_intact_lod0"]
    assert not first["hit"] and second["hit"] and first["key"] == second["key"]
    # Same filename and unchanged mesh: an edited source byte still invalidates the cook.
    doc["asset"]["copyright"] = "new source revision"
    src.write_text(json.dumps(doc))
    third = run("w3", "p3")["pristine_intact_lod0"]
    assert not third["hit"] and third["key"] != first["key"]
    monkeypatch.setattr(package, "COOKER_VERSION", "test-new-cooker")
    fourth = run("w4", "p4")["pristine_intact_lod0"]
    assert not fourth["hit"] and fourth["key"] != third["key"]


def test_stock_meshdump_frontend_and_package(tmp_path):
    from tools.content_pipeline.frontends import stock_meshdump
    src = Path(__file__).resolve().parents[2] / "asset_cook/tests/fixtures/2civliving_meshdump.json"
    Image.new("RGBA", (4, 4), (255, 255, 255, 255)).save(tmp_path / "A_2CivLiving.tga")
    asset = stock_meshdump.to_ir(src, texture_roots=[tmp_path])
    assert asset.lod0().triangle_count() == 56
    lo, hi = asset.lod0().bounds()
    assert np.allclose(lo, [-54.694801, 0, -54.5723], atol=1e-4)
    assert np.allclose(hi, [15.8478, 15.4678, 15.31], atol=1e-4)
    out = package.build_package([asset], tmp_path / "package", mod_id="stock-test", compress=False)
    assert package.check_package(out) == []
    assert asset.materials[0].base_color.passthrough


def test_tree_pack_uses_common_cooker_and_aliases(tmp_path):
    from tools.examples.build_modern_tree_pack import recipe
    from tools.content_pipeline.cook import cook
    src, *_ = make_gltf(tmp_path)
    families = [{"name": "poplar", "class": "tree", "replaces": "tree:pine1", "subdir": ".",
                 "lods": [{"filename": src.name, "distance_m": 0}, {"filename": src.name, "distance_m": 100}]}]
    out = cook(recipe(tmp_path, families, scale=2), tmp_path / "trees", compress=False)
    rows = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"]
    assert len(rows) == 5                       # pine + four palm aliases
    assert rows[0]["lods"][0]["distance"] == 100
    assert rows[0]["source"] == rows[1]["source"]
    assert (out / "content/poplar.mc2ir.json").is_file()


def test_bt2018_recipe_retains_skin_source_bytes(tmp_path):
    from tools.content_pipeline.cook import cook
    src, doc, *_ = make_gltf(tmp_path)
    # References-only skin: the frontend must retain skeleton/clip identity without
    # statically flattening it. Native skin math remains in the existing consumer.
    doc["skins"] = [{"joints": [0]}]
    doc["nodes"][1]["skin"] = 0
    doc["animations"] = [{"name": "Idle", "channels": [], "samplers": []}]
    src.write_text(json.dumps(doc))
    spec = {"mod_id": "mech-test", "assets": [{"frontend": "bt2018", "intact": str(src)}]}
    # Self-contained GLB is the mech runtime contract.
    binary = base64.b64decode(doc["buffers"][0]["uri"].split(",")[1])
    doc["buffers"][0].pop("uri")
    body = json.dumps(doc).encode(); body += b" " * (-len(body) % 4)
    binary += b"\0" * (-len(binary) % 4)
    glb = tmp_path / "mech.glb"
    glb.write_bytes(struct.pack("<III", gltf_io.GLB_MAGIC, 2, 28+len(body)+len(binary)) +
                    struct.pack("<II", len(body), gltf_io.CHUNK_JSON)+body +
                    struct.pack("<II", len(binary), gltf_io.CHUNK_BIN)+binary)
    spec["assets"][0]["intact"] = str(glb)
    out = cook(spec, tmp_path / "mechs", compress=False)
    assert (out / "data/tgl/mech.glb").read_bytes() == glb.read_bytes()
    assert ir.load(out / "content/mech.mc2ir.json").skeleton.clips == ["Idle"]


# --------------------------------------------------------------------------- cache key

def test_authored_materials_share_albedo_without_merging_surface_classes(tmp_path):
    import copy
    src, *_ = make_gltf(tmp_path)
    a = gltf_file.to_ir("panel", "staticProp:panel", src, work_dir=tmp_path / "work")
    normal = tmp_path / "normal.png"
    Image.new("RGBA", (4, 4), (128, 128, 255, 255)).save(normal)
    a.materials[0].normal = ir.TextureRef.of(normal, "normal")
    twin = copy.deepcopy(a.materials[0])
    twin.material_class = "painted_metal"
    twin.metallic = 0.0
    a.materials.append(twin)
    out = package.build_package([a], tmp_path / "out", mod_id="shared", compress=False)
    images = list((out / "data/tgl/128").glob("*.ktx2"))
    assert len(images) == 1
    record = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"][0]
    assert len(record["materials"]["slots"]) == 2
    assert len({s["normal"] for s in record["materials"]["slots"]}) == 1


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
    assert "shadow" not in rec           # render shapes cast; stand-ins are reported, not emitted
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


def test_inward_hull_faces_found_and_repaired():
    """A wall on the outer skin that faces into the building is culled from outside (see-through)."""
    pos = np.array([[0, 0, 0], [4, 0, 0], [4, 4, 0], [0, 4, 0],      # front wall z=0, should face -z
                    [0, 0, 4], [4, 0, 4], [4, 4, 4], [0, 4, 4]], float)
    front_inward = [[0, 1, 2], [0, 2, 3]]                              # normal +z: points inside
    back_outward = [[4, 5, 6], [4, 6, 7]]                              # normal +z at z=4: outside
    prim = ir.Primitive(pos, np.array(front_inward + back_outward), 0,
                        normals=np.tile([0.0, 0.0, 1.0], (8, 1)))
    scene = ir.Scene([ir.Node("root", -1, np.eye(4), 0)], [ir.Mesh("box", [prim])])
    faces = ir.inward_hull_faces(scene)
    assert [list(t) for _, _, t in faces] == [[0, 1]]
    assert ir.flip_faces(scene, faces) == 2
    assert ir.inward_hull_faces(scene) == []
    assert np.allclose(prim.normals[[1, 2, 3]], [0, 0, -1])               # exclusive front vertices flipped
    assert np.allclose(prim.normals[4], [0, 0, 1])                        # back wall untouched


def test_excluded_overlay_shares_albedo_but_keeps_legacy_material():
    # A real failure: a lit window overlay's constant UV lies inside the roof mask, so an atlas
    # mask cannot exclude it. The reviewed mesh moves to an appended legacy copy of its material.
    mats = [ir.Material("roof", alpha_mode="MASK")]
    quad = lambda: ir.Primitive(np.zeros((3, 3)), np.array([[0, 1, 2]]), 0)
    scene = ir.Scene([ir.Node("root", -1, np.eye(4), 0)], [ir.Mesh("body", [quad()]), ir.Mesh("LitWin_Quonset2", [quad()])])
    stock_ase.exclude_nodes(scene, mats, ["LitWin_Quonset2"])
    assert scene.meshes[0].primitives[0].material == 0
    assert scene.meshes[1].primitives[0].material == 1
    assert len(mats) == 2 and mats[1].alpha_mode == mats[0].alpha_mode and not mats[1].authored()
    for bad in (["unreviewed_typo"], ["body", "body"], [""], "body"):
        with pytest.raises(ValueError):
            stock_ase.exclude_nodes(scene, mats, bad)


def test_uvless_stock_helpers_keep_zero_uv(tmp_path):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "blender_pipeline" / "tests"))
    from test_ase_submaterial import SINGLE_MAT_ASE
    text = re.sub(r"\*MESH_TVERTLIST\s*\{[^}]*\}", "", SINGLE_MAT_ASE)
    text = re.sub(r"\*MESH_TFACELIST\s*\{[^}]*\}", "", text)
    file = tmp_path / "helper.ase"
    file.write_text(text)
    gltf, raw, _, _ = stock_ase._runtime_frame_glb(file)
    for mesh in gltf["meshes"]:
        acc = gltf["accessors"][mesh["primitives"][0]["attributes"]["TEXCOORD_0"]]
        view = gltf["bufferViews"][acc["bufferView"]]
        uv = [struct.unpack_from("<2f", raw, view["byteOffset"] + i * 8) for i in range(acc["count"])]
        assert uv == [(0, 0)] * acc["count"]   # TG's explicit no-TFACE default, not the flipped roof-corner texel
