"""asset_hd.py CLI tests — name resolution, param parsing, manifest synthesis,
dry-run plan (no Blender required)."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_hd as ah  # noqa: E402
import batch_driver as bd  # noqa: E402

STOCK_INI = 'FITini\n\n[TGLData]\nst FileName0="Quonset"\n\nFITend\n'


@pytest.fixture
def srcroot(tmp_path):
    """Fake srcdata root: tgl/ with quonset + Bldg_Arm (+ warehouse sans ini)."""
    tgl = tmp_path / "srcdata" / "tgl"
    tgl.mkdir(parents=True)
    for stem in ("quonset", "Bldg_Arm", "warehouse"):
        (tgl / f"{stem}.ase").write_text("*GEOMOBJECT {\n}\n")
    (tgl / "quonset.ini").write_text(STOCK_INI)
    (tgl / "Bldg_Arm.ini").write_text(STOCK_INI)
    return tmp_path / "srcdata"


# ---------------------------------------------------------------------------
# Resolution
# ---------------------------------------------------------------------------

def test_find_tgl_dir_accepts_root_or_tgl(srcroot):
    assert ah.find_tgl_dir(srcroot) == srcroot / "tgl"
    assert ah.find_tgl_dir(srcroot / "tgl") == srcroot / "tgl"
    with pytest.raises(RuntimeError, match="not a directory"):
        ah.find_tgl_dir(srcroot / "nope")


def test_list_assets_substring_filter(srcroot):
    assert ah.list_assets(srcroot / "tgl") == ["Bldg_Arm", "quonset", "warehouse"]
    assert ah.list_assets(srcroot / "tgl", "QUON") == ["quonset"]


def test_resolve_case_insensitive_and_ase_suffix(srcroot):
    stem, ase, ini = ah.resolve_asset("QUONSET", srcroot / "tgl")
    assert (stem, ase.name, ini.name) == ("quonset", "quonset.ase", "quonset.ini")
    stem, ase, ini = ah.resolve_asset("bldg_arm.ase", srcroot / "tgl")
    assert stem == "Bldg_Arm" and ini.name == "Bldg_Arm.ini"


def test_resolve_missing_suggests_closest(srcroot):
    with pytest.raises(ah.AssetResolutionError, match="quonset"):
        ah.resolve_asset("quonsit", srcroot / "tgl")


def test_resolve_missing_ini_is_clear(srcroot):
    with pytest.raises(ah.AssetResolutionError, match=r"\.ini"):
        ah.resolve_asset("warehouse", srcroot / "tgl")


# ---------------------------------------------------------------------------
# Param parsing
# ---------------------------------------------------------------------------

def test_parse_params_types_and_key_normalization():
    params = ah.parse_params([
        "subdiv=2", "weld=0.0001", "shade-smooth=false", "subdiv_type=SIMPLE",
        "ratios=0.5,0.2", "distances=600,1200", "flag=true"])
    assert params == {
        "subdiv": 2, "weld": 0.0001, "shade_smooth": False,
        "subdiv_type": "SIMPLE", "ratios": [0.5, 0.2],
        "distances": [600, 1200], "flag": True}


def test_parse_params_rejects_bare_key():
    with pytest.raises(ValueError, match="KEY=VALUE"):
        ah.parse_params(["subdiv"])


# ---------------------------------------------------------------------------
# Manifest synthesis
# ---------------------------------------------------------------------------

def example_manifest():
    path = Path(ah.TOOL_DIR) / "examples" / "quonset_hd.manifest.json"
    return json.loads(path.read_text(encoding="utf-8"))


def synth_quonset(srcroot, tmp_path, recipe="upscale_mesh", params=None,
                  out_kind="tgl_ini", **kw):
    resolved = [ah.resolve_asset("quonset", srcroot / "tgl")]
    out_dirs = {resolved[0][0]: tmp_path / "out" / "quonset"}
    return ah.synthesize_manifest(resolved, recipe, params or {}, out_kind,
                                  out_dirs, **kw)


def test_synth_matches_example_manifest_shape(srcroot, tmp_path):
    """Synthesized job must match the known-good hand-authored example."""
    synth = synth_quonset(srcroot, tmp_path, params={"subdiv": 1,
                                                     "crease_angle": 40.0})
    ex = example_manifest()
    ex_job, job = ex["jobs"][0], synth["jobs"][0]

    assert synth["schema_version"] == ex["schema_version"]
    assert set(job.keys()) == set(ex_job.keys())
    assert job["recipe"] == ex_job["recipe"] == "upscale_mesh"
    assert job["asset_class"] == ex_job["asset_class"] == "building"
    assert job["params"] == ex_job["params"]
    assert job["out"]["kind"] == ex_job["out"]["kind"] == "tgl_ini"
    assert job["out"]["source_name"] == ex_job["out"]["source_name"] == "QuonsetHD"
    assert job["source"]["ase"].endswith("quonset.ase")
    assert job["source"]["ini"].endswith("quonset.ini")
    # and it passes the driver's own validation with zero errors
    assert bd.validate_manifest(synth, tmp_path) == []


def test_synth_decimate_defaults_validate(srcroot, tmp_path):
    synth = synth_quonset(srcroot, tmp_path, recipe="decimate_lods",
                          out_kind="models_json", asset_class="prop")
    job = synth["jobs"][0]
    assert job["params"]["ratios"] == list(ah.DEFAULT_LOD_RATIOS)
    assert job["out"]["override_class"] == "staticprop"
    assert job["out"]["appearance_name"] == "quonset"
    assert bd.validate_manifest(synth, tmp_path) == []


def test_default_source_name():
    assert ah.default_source_name("quonset") == "QuonsetHD"
    assert ah.default_source_name("Bldg_Arm") == "Bldg_ArmHD"


# ---------------------------------------------------------------------------
# CLI end-to-end (dry-run — no Blender execution)
# ---------------------------------------------------------------------------

def test_cli_dry_run_writes_manifest_and_plans(srcroot, tmp_path, capsys):
    out_dir = tmp_path / "o"
    rc = ah.main(["quonset", "--dry-run", "--srcdata-root", str(srcroot),
                  "--out-dir", str(out_dir), "--param", "subdiv=2"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "DRY RUN" in out and "upscale_mesh.py" in out and "--subdiv=2" in out

    manifest_path = out_dir / "_synth" / "asset_hd_upscale_mesh.manifest.json"
    assert manifest_path.is_file()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    job = manifest["jobs"][0]
    assert job["params"]["subdiv"] == 2
    assert job["out"]["dir"] == str(out_dir)  # single asset: exact --out-dir


def test_cli_multi_asset_out_dirs(srcroot, tmp_path, capsys):
    out_dir = tmp_path / "o"
    rc = ah.main(["quonset", "bldg_arm", "--dry-run",
                  "--srcdata-root", str(srcroot), "--out-dir", str(out_dir)])
    assert rc == 0
    manifest = json.loads(
        (out_dir / "_synth" / "asset_hd_upscale_mesh.manifest.json")
        .read_text(encoding="utf-8"))
    dirs = [j["out"]["dir"] for j in manifest["jobs"]]
    assert dirs == [str(out_dir / "quonset"), str(out_dir / "bldg_arm")]
    names = [j["name"] for j in manifest["jobs"]]
    assert names == ["quonset_upscale_mesh", "bldg_arm_upscale_mesh"]


def test_cli_unknown_asset_exits_two_with_suggestion(srcroot, capsys):
    rc = ah.main(["quonsit", "--srcdata-root", str(srcroot)])
    assert rc == 2
    assert "quonset" in capsys.readouterr().err


def test_cli_list_and_missing_args(srcroot, capsys):
    rc = ah.main(["--list", "quon", "--srcdata-root", str(srcroot)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "quonset" in out and "warehouse" not in out

    rc = ah.main(["--srcdata-root", str(srcroot)])
    assert rc == 2
    assert "ASSET" in capsys.readouterr().err
