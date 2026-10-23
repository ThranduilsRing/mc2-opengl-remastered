import base64
from dataclasses import replace as dc_replace
import json
import os
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image
import pytest

from tools.assets.candidates import Candidate, load_candidate, load_gltf
from tools.assets.common import Mesh, OUT_ROOT, bounds, output_path
from tools.assets.replace import (fit_candidate, orientation_matrix, rank_candidate,
                                  safe_mod_id, search, select, target_spec, validate)
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
        yield path


def test_spec_carries_collision_destroyed_lods_and_real_key(entry):
    entry["id"] = "appearance_alias"
    spec = target_spec(entry)
    assert spec["selection_key"] == "staticProp:hangar"
    assert spec["footprint_wu"] == [4, 12]
    assert spec["height_wu"] == 8
    assert spec["collision"]["type_fields"][0]["fields"]["objecttype"]["extentradius"] == 35
    assert spec["destroyed_requirement"]["required"]
    assert spec["destroyed_requirement"]["runtime_geometry_override_supported"]   # destroyed chains cook now
    assert not target_spec(entry, "destroyed")["package_supported"]


def test_selection_accepts_type_id_and_rejects_ambiguity(entry):
    assert select({"objects": [entry]}, "559") is entry
    with pytest.raises(ValueError):
        select({"objects": [entry, entry]}, "hangar")


def test_animated_buildings_require_review(entry):
    from tools.assets.replace import package
    from tools.assets.catalog import load_catalog
    try:
        catalog = load_catalog()   # generated and untracked: built on demand when the installs exist
    except FileNotFoundError as error:
        pytest.skip(str(error))
    for name in ("repairbay", "rpbuilding", "sensortower"):
        assert not target_spec(select(catalog, name))["package_supported"]
    for sections in ({"Animation:0": {}}, {"TGLData": {"AnimationNodeId": "dish"}},
                     {"TGLData": {"SpinMe": True}}):
        entry["ini_sections"] = sections
        spec = target_spec(entry)
        assert not spec["package_supported"]
        with pytest.raises(ValueError, match="runtime|static props"):
            package(entry, spec, "unused.gltf", "animated", allow_stock_damage=True)


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


def test_renderer_repeats_uvs_instead_of_clamping():
    from tools.assets.render import render
    mesh = Mesh(np.array([[0, 0, 0], [0, 2, 0], [2, 0, 0]], dtype=float),
                np.array([[0, 0], [0, 1], [2, 0]], dtype=float), np.array([0]), ["tex"], [], [False])
    image = Image.fromarray(np.array([[[255, 0, 0, 255], [0, 255, 0, 255]],
                                     [[0, 0, 255, 255], [255, 255, 255, 255]]], dtype=np.uint8))
    expected = render(mesh, [image])
    shifted = dc_replace(mesh, uv=mesh.uv+[3, -2])
    np.testing.assert_array_equal(np.asarray(render(shifted, [image])), np.asarray(expected))


def _provenance(cube):
    cube.with_suffix(".asset.json").write_text(json.dumps({"category": "building", "forward_axis": "-z",
        "source_url": "local:pytest-cube", "license": "test-fixture", "orientation_reviewed": True}))


def test_package_cooks_through_the_canonical_pipeline(cube, entry, artifact_root, monkeypatch):
    """The catalog frontend fits; tools/content_pipeline cooks. The runtime GLB's world geometry is
    the fitted candidate, node transforms baked, authored normals kept (no faceting)."""
    from tools.assets.replace import package
    from tools.content_pipeline import cache, gltf_io, validate
    monkeypatch.setattr(cache, "DEFAULT_DIR", artifact_root/"cook-cache")
    doc = json.loads(cube.read_text())
    payload = base64.b64decode(doc["buffers"][0]["uri"].split(",")[1])
    authored = np.array([[1+i, 2, 3] for i in range(8)], dtype="<f4")
    authored /= np.linalg.norm(authored, axis=1, keepdims=True)
    doc["bufferViews"].append({"buffer": 0, "byteOffset": len(payload), "byteLength": authored.nbytes})
    payload += authored.tobytes()
    doc["buffers"][0] = {"uri": "data:application/octet-stream;base64,"+base64.b64encode(payload).decode(), "byteLength": len(payload)}
    doc["accessors"].append({"bufferView": 2, "componentType": 5126, "count": 8, "type": "VEC3"})
    doc["accessors"][0].update(min=[0, 0, 0], max=[2, 4, 6])
    doc["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 2
    cube.write_text(json.dumps(doc))
    _provenance(cube)
    spec = target_spec(entry)
    with pytest.raises(ValueError, match="no destroyed candidate"):
        package(entry, spec, cube, "bad")
    out = package(entry, spec, cube, "good", allow_stock_damage=True)
    record, = json.loads((out/"data/model_overrides/models.json").read_text())["overrides"]
    assert record["replaces"] == "staticProp:hangar" and record["renderOnly"] and record["fallback"] == "stock"
    assert "destroyed" not in record                     # stock damage, explicitly accepted
    glb = out/"data/model_overrides"/record["source"]
    assert validate.errors(validate.validate_runtime_glb(glb)) == []
    fitted, _ = fit_candidate(load_candidate(cube), spec, {"forward_axis": "-z"})
    scene, _, _ = gltf_io.read_gltf(glb, frame="mc2_runtime", texture_roots=None)
    world = np.concatenate([p.positions[p.indices].reshape(-1, 3) for _, p in scene.flattened()])
    np.testing.assert_allclose(np.sort(world, axis=0), np.sort(fitted.mesh.vertices, axis=0), atol=1e-4)
    normals = np.concatenate([p.normals for _, p in scene.flattened()])
    assert len({tuple(np.round(n, 4)) for n in normals}) == 8   # 8 authored corner normals, not 6 face normals
    assert (out/"content/good.mc2ir.json").is_file()
    with pytest.raises(FileExistsError):
        package(entry, spec, cube, "good", allow_stock_damage=True)


def test_destroyed_candidate_rides_the_same_package(cube, entry, artifact_root, monkeypatch):
    from tools.assets.replace import package
    from tools.content_pipeline import cache
    monkeypatch.setattr(cache, "DEFAULT_DIR", artifact_root/"cook-cache")
    _provenance(cube)
    out = package(entry, target_spec(entry), cube, "both", destroyed=cube)
    record, = json.loads((out/"data/model_overrides/models.json").read_text())["overrides"]
    assert (out/"data/model_overrides"/record["destroyed"]["source"]).is_file()


@pytest.mark.parametrize("state", ["destroyed", "animated"])
def test_review_only_never_activates(cube, entry, artifact_root, state):
    from tools.assets.replace import package
    _provenance(cube)
    if state == "animated":
        entry["ini_sections"] = {"animation:0": {"animationname": "hangar"}}
    spec = target_spec(entry, "destroyed" if state == "destroyed" else "intact")
    with pytest.raises(ValueError):
        package(entry, spec, cube, "refused", allow_stock_damage=True)
    out = package(entry, spec, cube, "review", review_only=True)
    assert not (out/"mod.json").exists()
    assert not (out/"data/model_overrides/models.json").exists()
    assert (out/"content/review.mc2ir.json").exists()
