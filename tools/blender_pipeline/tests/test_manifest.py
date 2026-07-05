"""Manifest validation + inject-helper unit tests (no Blender required)."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import batch_driver as bd  # noqa: E402


@pytest.fixture
def tmp_assets(tmp_path):
    (tmp_path / "quonset.ase").write_text("*GEOMOBJECT {\n}\n")
    (tmp_path / "quonset.ini").write_text(
        'FITini\n\n[TGLData]\nst FileName0="Quonset"\nf Distance0 = 0.0\n\nFITend\n')
    (tmp_path / "maple1.glb").write_bytes(b"glTF")
    return tmp_path


def make_manifest(tmp, **overrides):
    m = {
        "schema_version": "1.0",
        "jobs": [{
            "name": "quonset_hd",
            "recipe": "upscale_mesh",
            "asset_class": "building",
            "source": {"ase": "quonset.ase", "ini": "quonset.ini"},
            "params": {"subdiv": 1},
            "out": {"kind": "tgl_ini", "dir": "out", "source_name": "QuonsetHD"},
        }],
    }
    m.update(overrides)
    return m


def test_valid_manifest(tmp_assets):
    errors = bd.validate_manifest(make_manifest(tmp_assets), tmp_assets)
    assert errors == []


def test_bad_schema_version(tmp_assets):
    errors = bd.validate_manifest(
        make_manifest(tmp_assets, schema_version="9.9"), tmp_assets)
    assert any("schema_version" in e for e in errors)


def test_unknown_recipe(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"][0]["recipe"] = "make_it_pretty"
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("recipe" in e for e in errors)


def test_missing_source_file(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"][0]["source"]["ase"] = "nope.ase"
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("not found" in e for e in errors)


def test_source_needs_exactly_one_of_ase_glb(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"][0]["source"] = {"ase": "quonset.ase", "ini": "quonset.ini",
                              "glb": "maple1.glb"}
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("exactly one" in e for e in errors)


def test_tgl_ini_requires_source_name(tmp_assets):
    m = make_manifest(tmp_assets)
    del m["jobs"][0]["out"]["source_name"]
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("source_name" in e for e in errors)


def test_duplicate_job_names(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"].append(json.loads(json.dumps(m["jobs"][0])))
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("duplicate" in e for e in errors)


def test_decimate_lods_manifest(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"] = [{
        "name": "maple1_lods",
        "recipe": "decimate_lods",
        "asset_class": "tree",
        "source": {"glb": "maple1.glb"},
        "params": {"ratios": [0.5, 0.2], "distances": [600, 1200]},
        "out": {"kind": "models_json", "dir": "out",
                "override_class": "tree", "appearance_name": "maple1"},
    }]
    assert bd.validate_manifest(m, tmp_assets) == []

    m["jobs"][0]["params"]["ratios"] = [1.5]
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("ratios" in e for e in errors)


def test_models_json_requires_decimate(tmp_assets):
    m = make_manifest(tmp_assets)
    m["jobs"][0]["out"] = {"kind": "models_json", "dir": "out",
                           "override_class": "tree", "appearance_name": "x"}
    errors = bd.validate_manifest(m, tmp_assets)
    assert any("models_json" in e for e in errors)


def test_load_manifest_raises_with_all_errors(tmp_assets):
    bad = make_manifest(tmp_assets, schema_version="0.1")
    bad["jobs"][0]["recipe"] = "nope"
    p = tmp_assets / "m.json"
    p.write_text(json.dumps(bad))
    with pytest.raises(bd.ManifestError) as exc:
        bd.load_manifest(p)
    assert len(exc.value.errors) >= 2


# ---------------------------------------------------------------------------
# Inject helpers
# ---------------------------------------------------------------------------

STOCK_INI = """FITini

[Bounds]
l UpperLeftX=-20

[TGLData]
st FileName0="Quonset"
f Distance0 = 0.0

FITend
"""


def test_override_ini_inserts_import_after_fitini():
    out = bd.build_override_ini(STOCK_INI, "QuonsetHD")
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    assert lines[0] == "FITini"
    assert lines[1] == "[Import]"
    assert lines[2] == 'st Source = "QuonsetHD"'
    assert "[TGLData]" in lines and "[Bounds]" in lines
    assert lines[-1] == "FITend"


def test_override_ini_idempotent():
    once = bd.build_override_ini(STOCK_INI, "QuonsetHD")
    twice = bd.build_override_ini(once, "QuonsetHD2")
    assert twice.count("[Import]") == 1
    assert 'st Source = "QuonsetHD2"' in twice
    assert "QuonsetHD\"" not in twice


def test_models_json_entry_shape():
    entry = bd.build_models_json_entry(
        {"override_class": "tree", "appearance_name": "maple1"},
        ["maple1_lod0.glb", "maple1_lod1.glb"], [600])
    assert entry["replaces"] == "tree:maple1"
    assert entry["renderOnly"] is True and entry["fallback"] == "stock"
    assert entry["lods"][0]["distance"] == 0
    assert entry["lods"][1] == {
        "distance": 600.0,
        "source": "data/model_overrides/cooked/maple1_lod1.glb"}


def test_models_json_merge_replaces_stale_duplicate():
    old = [{"replaces": "tree:maple1", "lods": []},
           {"replaces": "tree:pine1", "lods": []}]
    new = {"replaces": "tree:maple1", "lods": [{"distance": 0, "source": "x"}]}
    merged = bd.merge_models_json(old, new)
    assert len(merged) == 2
    assert [e for e in merged if e["replaces"] == "tree:maple1"][0]["lods"]
