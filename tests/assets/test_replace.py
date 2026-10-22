import base64
from dataclasses import replace as dc_replace
import json
import os
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image
import pytest

from tools.assets.candidates import Candidate, export_runtime_gltf, load_candidate, load_gltf
from tools.assets.common import Mesh, OUT_ROOT, bounds, output_path
from tools.assets.replace import (fit_candidate, orientation_matrix, rank_candidate,
                                  safe_mod_id, search, select, target_spec, validate)
from tools.assets.rework import mip_chain, normal_map, rename_tgl_textures, upscale
from test_catalog import tgl_fixture


@pytest.fixture
def cube(tmp_path):
    # Non-cubic dimensions discriminate axis swaps; scene translation/scale
    # prove the candidate importer does not silently drop node transforms.
    v = np.array([[x, y, z] for x in (0, 2) for y in (0, 4) for z in (0, 6)], dtype="<f4")
    faces = np.array([[0, 4, 6], [0, 6, 2], [1, 3, 7], [1, 7, 5],
                      [0, 1, 5], [0, 5, 4], [2, 6, 7], [2, 7, 3],
                      [0, 2, 3], [0, 3, 1], [4, 5, 7], [4, 7, 6]], dtype="<u2")
    payload = v.tobytes()+faces.tobytes()
    doc = {"asset": {"version": "2.0"}, "buffers": [{"uri": "data:application/octet-stream;base64,"+base64.b64encode(payload).decode(), "byteLength": len(payload)}],
        "bufferViews": [{"buffer": 0, "byteLength": v.nbytes}, {"buffer": 0, "byteOffset": v.nbytes, "byteLength": faces.nbytes}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3"},
                      {"bufferView": 1, "componentType": 5123, "count": 36, "type": "SCALAR"}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [.8, .3, .1, 1]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
        "nodes": [{"translation": [10, 20, 30], "scale": [2, 2, 2], "mesh": 0}],
        "scenes": [{"nodes": [0]}], "scene": 0}
    path = tmp_path/"warehouse.gltf"
    path.write_text(json.dumps(doc))
    return path


@pytest.fixture
def entry():
    b = bounds([[0, 0, 0], [4, 8, 12]])
    return {"id": "hangar", "category": "building", "instances": 40,
        "models": [{"role": "intact", "lod": 0, "distance_wu": 0, "tgl": "data/tgl/hangar.tgl",
                    "triangles": 12, "bounds": b, "sha256": "fixture"},
                   {"role": "destroyed", "lod": 0, "distance_wu": 0, "tgl": "data/tgl/hangardam.tgl", "triangles": 12, "bounds": b}],
        "types": [{"type_id": 559, "type_name": "Hangar", "collision_fields": {"objecttype": {"extentradius": 35}}}],
        "missions": [{"instances": 20, "corpus": "stock", "mission": "one"}], "ini_sections": {}}


@pytest.fixture
def artifact_root(monkeypatch, tmp_path):
    # Windows task artifacts stay under the owner's permitted mod output root.
    parent = OUT_ROOT/"_tests" if os.name == "nt" else tmp_path
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="asset-", dir=parent) as folder:
        path = Path(folder).resolve()
        assert path.is_relative_to(parent.resolve())
        monkeypatch.setattr("tools.assets.common.OUT_ROOT", path)
        monkeypatch.setattr("tools.assets.replace.OUT_ROOT", path)
        monkeypatch.setattr("tools.assets.rework.OUT_ROOT", path)
        yield path


def test_spec_carries_collision_destroyed_lods_and_real_key(entry):
    entry["id"] = "appearance_alias"
    spec = target_spec(entry)
    assert spec["selection_key"] == "staticProp:hangar"
    assert spec["footprint_wu"] == [4, 12]
    assert spec["height_wu"] == 8
    assert spec["collision"]["type_fields"][0]["fields"]["objecttype"]["extentradius"] == 35
    assert spec["destroyed_requirement"]["required"]
    assert not spec["destroyed_requirement"]["runtime_geometry_override_supported"]
    assert not target_spec(entry, "destroyed")["package_supported"]


def test_selection_accepts_type_id_and_rejects_ambiguity(entry):
    assert select({"objects": [entry]}, "559") is entry
    with pytest.raises(ValueError):
        select({"objects": [entry, entry]}, "hangar")


def test_animated_buildings_require_review(entry):
    from tools.assets.replace import _package
    catalog = json.loads(Path("docs/assets/catalog.json").read_text())
    for name in ("repairbay", "rpbuilding", "sensortower"):
        assert not target_spec(select(catalog, name))["package_supported"]
    for sections in ({"Animation:0": {}}, {"TGLData": {"AnimationNodeId": "dish"}},
                     {"TGLData": {"SpinMe": True}}):
        entry["ini_sections"] = sections
        spec = target_spec(entry)
        assert not spec["package_supported"]
        with pytest.raises(ValueError, match="runtime|static props"):
            _package(entry, spec, "unused.gltf", "animated", allow_stock_damage=True)


def test_loader_bakes_scene_transforms(cube):
    candidate = load_candidate(cube)
    b = bounds(candidate.mesh.vertices)
    assert b["min"] == [10, 20, 30]
    assert b["max"] == [14, 28, 42]
    assert candidate.mesh.triangles == 12
    # glTF factors are linear; the cooked base-color image is sRGB encoded.
    assert candidate.images[0].getpixel((0, 0)) == (231, 149, 89, 255)


def test_omitted_material_uses_default_without_requiring_uv(cube):
    doc = json.loads(cube.read_text())
    del doc["meshes"][0]["primitives"][0]["material"]
    Image.new("RGBA", (1, 1), "red").save(cube.parent/"red.png")
    doc["images"] = [{"uri": "red.png"}]
    doc["textures"] = [{"source": 0}]
    doc["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    cube.write_text(json.dumps(doc))
    candidate = load_gltf(cube)
    assert set(candidate.mesh.materials) == {1}
    assert candidate.images[1].getpixel((0, 0)) == (255, 255, 255, 255)
    del doc["materials"]
    cube.write_text(json.dumps(doc))
    candidate = load_gltf(cube)
    assert set(candidate.mesh.materials) == {0}
    assert candidate.images[0].getpixel((0, 0)) == (255, 255, 255, 255)


def test_loader_rejects_network_and_path_escape(cube):
    doc = json.loads(cube.read_text())
    for uri in ("https://invalid.test/file.bin", "../outside.bin"):
        doc["buffers"][0]["uri"] = uri
        cube.write_text(json.dumps(doc))
        with pytest.raises(ValueError, match="local relative|escapes"):
            load_candidate(cube)


def test_ranking_prefers_shape_and_category(cube, entry):
    spec, candidate = target_spec(entry), load_candidate(cube)
    metadata = {"category": "building", "forward_axis": "-z"}
    good = rank_candidate(spec, candidate, metadata)
    wide = Candidate(dc_replace(candidate.mesh, vertices=candidate.mesh.vertices*[3, 1, 1]), candidate.images, cube)
    assert good["score"] < rank_candidate(spec, wide, metadata)["score"]
    assert good["score"] < rank_candidate(spec, candidate, {"category": "fence"})["score"]
    assert good["near_match"]


def test_search_empty_and_import_errors(tmp_path, cube, entry):
    assert search(target_spec(entry), tmp_path/"absent")["results"] == []
    cube.with_suffix(".asset.json").write_text(json.dumps({"category": "building", "forward_axis": "-z"}))
    (tmp_path/"bad.glb").write_bytes(b"bad")
    result = search(target_spec(entry), tmp_path)
    assert len(result["results"]) == 1
    assert len(result["errors"]) == 1


def test_fit_preserves_stock_pivot_and_detects_bad_bounds(cube, entry):
    spec = target_spec(entry)
    candidate, transform = fit_candidate(load_candidate(cube), spec, {"forward_axis": "-z", "orientation_reviewed": True})
    assert validate(candidate, spec, transform)["valid"]
    np.testing.assert_allclose(bounds(candidate.mesh.vertices)["min"], [0, 0, 0])
    moved = Candidate(dc_replace(candidate.mesh, vertices=candidate.mesh.vertices+[5, 0, 0]), candidate.images, cube)
    assert not validate(moved, spec, transform)["valid"]


def test_fit_planar_damage_geometry_uses_nonzero_dimensions(cube, entry):
    candidate = load_candidate(cube)
    candidate.mesh.vertices[:, 1] = 0
    flat = bounds([[0, 0, 0], [4, 0, 12]])
    entry["models"][0]["bounds"] = flat
    spec = target_spec(entry)
    fitted, transform = fit_candidate(candidate, spec, {"forward_axis": "-z"})
    assert validate(fitted, spec, transform)["valid"]


def test_validation_blocks_triangle_budget_nonfinite_and_rotation(cube, entry):
    spec = target_spec(entry)
    candidate, transform = fit_candidate(load_candidate(cube), spec, {"forward_axis": "-z"})
    spec["budgets"]["max_triangles_lod0"] = 1
    assert any("triangle budget" in e for e in validate(candidate, spec, transform)["errors"])
    spec["budgets"]["max_triangles_lod0"] = 100
    transform["orientation_matrix"][0][0] = -1
    assert not validate(candidate, spec, transform)["valid"]
    bad = Candidate(dc_replace(candidate.mesh, vertices=candidate.mesh.vertices*np.nan), candidate.images, cube)
    assert not validate(bad, spec)["valid"]


def test_runtime_export_inverts_axis_and_binds_real_texture_names(cube, entry, artifact_root):
    candidate, transform = fit_candidate(load_candidate(cube), target_spec(entry), {"forward_axis": "-z"})
    folder = artifact_root/"pack"/"data"/"model_overrides"/"cooked"
    path, images, names = export_runtime_gltf(candidate, folder, "test", 512)
    cooked = load_gltf(path)
    np.testing.assert_allclose(cooked.mesh.vertices*[-1, -1, 1], candidate.mesh.vertices, atol=1e-6)
    doc = json.loads(path.read_text())
    assert doc["nodes"] == [{"name": "modern_prop", "mesh": 0}]
    for name in names:
        assert (artifact_root/"pack"/"data"/"tgl"/"128"/name).is_file()
    assert cooked.images[0].getpixel((0, 0)) == images[0].getpixel((0, 0))


def test_orientation_uses_declared_z_up_and_front():
    matrix = orientation_matrix({"up_axis": "z", "forward_axis": "y"})
    np.testing.assert_allclose(matrix@[0, 0, 1], [0, 1, 0])
    np.testing.assert_allclose(matrix@[0, 1, 0], [0, 0, -1])
    assert np.isclose(np.linalg.det(matrix), 1)


def test_mod_and_output_path_guards():
    for name in ("../oops", "a/b", "A", "", "a:b"):
        with pytest.raises(ValueError):
            safe_mod_id(name)
    with pytest.raises(ValueError):
        output_path("A:/Games/mc2-opengl/anything.json")
    with pytest.raises(ValueError):
        output_path("C:/outside/mod", mod=True)


def test_rework_isolates_texture_names_and_preserves_geometry_bytes():
    raw = tgl_fixture()
    cooked = rename_tgl_textures(raw, {"a_test.tga": "a_unique.tga"})
    assert cooked[12+256:] == raw[12+256:]
    assert cooked[:12] == raw[:12]
    assert b"a_unique.tga" in cooked[:268]


def test_alpha_upscale_mips_and_normal_range():
    image = Image.new("RGBA", (8, 8), (150, 180, 200, 0))
    for x in range(4):
        for y in range(8):
            image.putpixel((x, y), (150, 180, 200, 255))
    large = upscale(image, 4, 64)
    assert large.size == (32, 32)
    mips = mip_chain(large)
    assert mips[-1].size == (1, 1)
    assert abs(float(np.mean(np.asarray(mips[2])[:, :, 3] >= 128))-.5) <= .125
    normal = normal_map(Image.new("RGBA", (4, 4), (100, 100, 100, 255)))
    assert normal.getpixel((0, 0)) == (127, 127, 255)


def test_renderer_repeats_uvs_instead_of_clamping():
    from tools.assets.render import render
    mesh = Mesh(np.array([[0, 0, 0], [0, 2, 0], [2, 0, 0]], dtype=float),
                np.array([[0, 0], [0, 1], [2, 0]], dtype=float), np.array([0]), ["tex"], [], [False])
    image = Image.fromarray(np.array([[[255, 0, 0, 255], [0, 255, 0, 255]],
                                     [[0, 0, 255, 255], [255, 255, 255, 255]]], dtype=np.uint8))
    expected = render(mesh, [image])
    shifted = dc_replace(mesh, uv=mesh.uv+[3, -2])
    np.testing.assert_array_equal(np.asarray(render(shifted, [image])), np.asarray(expected))


def test_package_end_to_end_schema_textures_preview_and_stock_damage(cube, entry, artifact_root, monkeypatch):
    from tools.assets.replace import package
    class FakeStock:
        def read(self, name):
            return tgl_fixture(), "fixture"
    monkeypatch.setattr("tools.assets.replace.stock_install", lambda root: FakeStock())
    monkeypatch.setattr("tools.assets.render.mesh_textures", lambda mesh, stock: [Image.new("RGBA", (2, 2), "white")])
    cube.with_suffix(".asset.json").write_text(json.dumps({"category": "building", "forward_axis": "-z",
        "source_url": "local:pytest-cube", "license": "test-fixture", "orientation_reviewed": True}))
    spec = target_spec(entry)
    with pytest.raises(ValueError, match="damage stays stock"):
        package(entry, spec, cube, "bad")
    assert not (artifact_root/"bad").exists()
    assert list(artifact_root.iterdir()) == []  # failed transaction removes its staging directory
    out = package(entry, spec, cube, "good", allow_stock_damage=True)
    assert json.loads((out/"mod.json").read_text())["schema"] == "mc2-mod/1"
    manifest = json.loads((out/"data/model_overrides/models.json").read_text())
    record, = manifest["overrides"]
    assert record["replaces"] == "staticProp:hangar"
    assert record["scale"] == 1 and record["fallback"] == "stock" and record["renderOnly"]
    assert (out/"data/model_overrides"/record["source"]).is_file()
    assert (out/"preview.png").is_file()
    assert json.loads((out/"validation.json").read_text())["validation"]["valid"]
    with pytest.raises(FileExistsError):
        package(entry, spec, cube, "good", allow_stock_damage=True)


@pytest.mark.parametrize("state", ["destroyed", "animated"])
def test_destroyed_review_package_cannot_silently_activate(cube, entry, artifact_root, monkeypatch, state):
    from tools.assets.replace import package
    class FakeStock:
        def read(self, name):
            return tgl_fixture(), "fixture"
    monkeypatch.setattr("tools.assets.replace.stock_install", lambda root: FakeStock())
    monkeypatch.setattr("tools.assets.render.mesh_textures", lambda mesh, stock: [Image.new("RGBA", (2, 2), "white")])
    cube.with_suffix(".asset.json").write_text(json.dumps({"category": "building", "forward_axis": "-z",
        "source_url": "local:pytest-cube", "license": "test-fixture"}))
    if state == "animated":
        entry["ini_sections"] = {"animation:0": {"animationname": "hangar"}}
    spec = target_spec(entry, "destroyed" if state == "destroyed" else "intact")
    with pytest.raises(ValueError):
        package(entry, spec, cube, "refused", allow_stock_damage=True)
    out = package(entry, spec, cube, "review", review_only=True)
    assert not (out/"mod.json").exists()
    assert not (out/"data/model_overrides/models.json").exists()
    assert (out/"data/model_overrides/models.review.json").exists()
