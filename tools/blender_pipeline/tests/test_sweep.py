"""sweep.py tests — enumeration, filters, manifest shape, shards, report.

No Blender required (BLENDER-ASSET-SWEEP-1).
"""
import csv
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import batch_driver as bd  # noqa: E402
import sweep  # noqa: E402


@pytest.fixture
def tgl_dir(tmp_path):
    """Fake stock tgl dir: 3 base assets (ase+ini), 2 orphans (ase only)."""
    d = tmp_path / "tgl"
    d.mkdir()
    for stem in ("2civliving", "maple1", "Quonset"):
        (d / f"{stem}.ase").write_text("*GEOMOBJECT {\n}\n")
        # note: ini stem case differs for Quonset — filesystem reality
        (d / f"{stem.lower()}.ini").write_text(
            'FITini\n\n[TGLData]\nst FileName0="x"\n\nFITend\n')
    (d / "maple1dam.ase").write_text("*GEOMOBJECT {\n}\n")   # no ini -> skipped
    (d / "maple1x.ase").write_text("*GEOMOBJECT {\n}\n")     # no ini -> skipped
    return d


# ---------------------------------------------------------------------------
# enumeration + filtering
# ---------------------------------------------------------------------------

def test_enumerate_pairs_only_sorted(tgl_dir):
    assets = sweep.enumerate_assets(tgl_dir)
    stems = [a[0] for a in assets]
    assert stems == ["2civliving", "maple1", "Quonset"]  # sorted, orphans skipped
    for stem, ase, ini in assets:
        assert ase.suffix == ".ase" and ini.suffix == ".ini"
        assert ase.is_file() and ini.is_file()


def test_enumerate_matches_ini_case_insensitively(tgl_dir):
    assets = {a[0]: a for a in sweep.enumerate_assets(tgl_dir)}
    assert "Quonset" in assets            # quonset.ini matched Quonset.ase
    assert assets["Quonset"][2].name == "quonset.ini"


def test_apply_filters_glob_exclude_limit(tgl_dir):
    assets = sweep.enumerate_assets(tgl_dir)
    assert [a[0] for a in sweep.apply_filters(assets, ["maple*"], [], 0)] == \
        ["maple1"]
    assert [a[0] for a in sweep.apply_filters(assets, ["*"], ["maple*"], 0)] == \
        ["2civliving", "Quonset"]
    assert len(sweep.apply_filters(assets, ["*"], [], 2)) == 2
    # filters are case-insensitive against the stem
    assert [a[0] for a in sweep.apply_filters(assets, ["QUONS*"], [], 0)] == \
        ["Quonset"]
    # repeatable filters union
    got = sweep.apply_filters(assets, ["maple*", "2civ*"], [], 0)
    assert [a[0] for a in got] == ["2civliving", "maple1"]


def test_parse_param_json_coercion():
    assert sweep.parse_param("subdiv=2") == ("subdiv", 2)
    assert sweep.parse_param("crease_angle=35.5") == ("crease_angle", 35.5)
    assert sweep.parse_param("shade_smooth=true") == ("shade_smooth", True)
    assert sweep.parse_param("ratios=[0.5,0.2]") == ("ratios", [0.5, 0.2])
    assert sweep.parse_param("subdiv_type=SIMPLE") == ("subdiv_type", "SIMPLE")
    with pytest.raises(ValueError):
        sweep.parse_param("noequals")


# ---------------------------------------------------------------------------
# manifest generation
# ---------------------------------------------------------------------------

def test_build_manifest_validates_and_has_expected_shape(tgl_dir, tmp_path):
    assets = sweep.enumerate_assets(tgl_dir)
    out_dir = tmp_path / "out"
    manifest = sweep.build_manifest(assets, out_dir, recipe="upscale_mesh",
                                    params={"subdiv": 2}, out_kind="tgl_ini")
    assert bd.validate_manifest(manifest, out_dir) == []  # batch_driver-valid
    assert manifest["schema_version"] == bd.SCHEMA_VERSION
    assert len(manifest["jobs"]) == 3
    names = [j["name"] for j in manifest["jobs"]]
    assert len(names) == len(set(names))  # unique job names
    job = manifest["jobs"][0]
    assert job["recipe"] == "upscale_mesh"
    assert job["params"] == {"subdiv": 2}
    assert job["out"]["kind"] == "tgl_ini"
    assert job["out"]["source_name"] == f"{job['name']}HD"
    # absolute, location-independent paths
    assert Path(job["source"]["ase"]).is_absolute()
    assert Path(job["out"]["dir"]).is_absolute()
    assert Path(manifest["work_dir"]).is_absolute()


def test_build_manifest_models_json_fields(tgl_dir, tmp_path):
    assets = sweep.enumerate_assets(tgl_dir)[:1]
    manifest = sweep.build_manifest(
        assets, tmp_path / "out", recipe="decimate_lods",
        params={"ratios": [0.5, 0.2]}, out_kind="models_json",
        asset_class="tree", override_class="tree")
    assert bd.validate_manifest(manifest, tmp_path / "out") == []
    out = manifest["jobs"][0]["out"]
    assert out["override_class"] == "tree"
    assert out["appearance_name"] == manifest["jobs"][0]["name"]


def test_build_manifest_bad_params_caught_by_driver_validation(tgl_dir, tmp_path):
    assets = sweep.enumerate_assets(tgl_dir)[:1]
    manifest = sweep.build_manifest(assets, tmp_path / "out",
                                    recipe="decimate_lods", params={},
                                    out_kind="glb_only")
    errors = bd.validate_manifest(manifest, tmp_path / "out")
    assert any("ratios" in e for e in errors)  # reuse, not duplication


# ---------------------------------------------------------------------------
# shard logic
# ---------------------------------------------------------------------------

def test_shard_jobs_round_robin_covers_all():
    jobs = [{"name": f"j{i}"} for i in range(10)]
    shards = sweep.shard_jobs(jobs, 4)
    assert len(shards) == 4
    assert sorted(len(s) for s in shards) == [2, 2, 3, 3]  # balanced
    flat = sorted(j["name"] for s in shards for j in s)
    assert flat == sorted(j["name"] for j in jobs)  # nothing lost or duplicated
    assert shards == sweep.shard_jobs(jobs, 4)  # deterministic


def test_shard_jobs_edge_cases():
    jobs = [{"name": "a"}, {"name": "b"}]
    assert sweep.shard_jobs(jobs, 1) == [jobs]
    assert len(sweep.shard_jobs(jobs, 8)) == 2   # no empty shards
    assert sweep.shard_jobs(jobs, 0) == [jobs]   # clamped to 1


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

def _fake_results():
    return [
        {"name": "quonset", "ok": True, "seconds": 12.3, "stages": [
            {"stage": "extract", "seconds": 0.2},
            {"stage": "transform", "seconds": 11.0, "stats": {"upscale_mesh": {
                "tris_before": 46, "tris_after": 276}}},
            {"stage": "inject", "seconds": 0.1},
        ]},
        {"name": "maple1", "ok": False, "seconds": 3.4,
         "error": "blender exited 1 on upscale_mesh.py\ntraceback tail here"},
    ]


def test_tris_from_result():
    ok, fail = _fake_results()
    assert sweep.tris_from_result(ok) == (46, 276)
    assert sweep.tris_from_result(fail) == (None, None)


def test_tris_from_result_decimate_lods():
    r = {"name": "x", "ok": True, "stages": [
        {"stage": "transform", "stats": {"decimate_lods": {
            "name": "x", "lods": [{"lod": 1, "tris": 40}, {"lod": 2, "tris": 12}]}}},
    ]}
    assert sweep.tris_from_result(r) == (None, 12)


def test_write_report_md_and_csv(tmp_path):
    ok, fail = sweep.write_report(_fake_results(), tmp_path, wall_seconds=15.7)
    assert (ok, fail) == (1, 1)

    md = (tmp_path / "report.md").read_text(encoding="utf-8")
    assert "ok 1, failed 1" in md
    assert "wall time: 15.7s" in md
    assert "| quonset | ok | 46 | 276 |" in md
    assert "blender exited 1" in md          # first error line surfaces
    assert "traceback tail" not in md        # ...but only the first line

    with open(tmp_path / "report.csv", newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert rows[0]["asset"] == "quonset" and rows[0]["status"] == "ok"
    assert rows[0]["tris_before"] == "46" and rows[0]["tris_after"] == "276"
    assert rows[1]["status"] == "FAIL" and "blender exited 1" in rows[1]["error"]


def test_write_report_flags_missing_stats_and_no_change(tmp_path):
    results = [
        # ok:true but transform stage produced no stats — suspect, not green
        {"name": "nostats", "ok": True, "seconds": 1.0, "stages": [
            {"stage": "transform", "seconds": 0.9}]},
        # ok:true but tri count unchanged — recipe was a no-op
        {"name": "noop", "ok": True, "seconds": 1.0, "stages": [
            {"stage": "transform", "stats": {"upscale_mesh": {
                "tris_before": 46, "tris_after": 46}}}]},
    ]
    sweep.write_report(results, tmp_path)
    with open(tmp_path / "report.csv", newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert rows[0]["status"] == "ok(no-stats)"
    assert rows[1]["status"] == "ok(NO-CHANGE)"


# ---------------------------------------------------------------------------
# CLI (plan/dry modes — never launches Blender)
# ---------------------------------------------------------------------------

def test_cli_plan_mode_writes_manifest_no_run(tgl_dir, tmp_path, capsys):
    out_dir = tmp_path / "sweepout"
    rc = sweep.main(["--srcdata", str(tgl_dir), "--limit", "2",
                     "--out-dir", str(out_dir)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "plan only" in out
    manifest = json.loads(
        (out_dir / "sweep_manifest.json").read_text(encoding="utf-8"))
    assert len(manifest["jobs"]) == 2
    assert not (out_dir / "report.md").exists()  # nothing ran


def test_cli_manifest_out_dry_mode(tgl_dir, tmp_path, capsys):
    dst = tmp_path / "dry" / "m.json"
    rc = sweep.main(["--srcdata", str(tgl_dir), "--filter", "maple*",
                     "--manifest-out", str(dst)])
    assert rc == 0
    assert "dry mode" in capsys.readouterr().out
    manifest = json.loads(dst.read_text(encoding="utf-8"))
    assert [j["name"] for j in manifest["jobs"]] == ["maple1"]
    assert bd.validate_manifest(manifest, dst.parent) == []


def test_cli_no_match_exits_two(tgl_dir, tmp_path, capsys):
    rc = sweep.main(["--srcdata", str(tgl_dir), "--filter", "zzz*",
                     "--out-dir", str(tmp_path / "o")])
    assert rc == 2
    assert "no assets matched" in capsys.readouterr().err


def test_cli_bad_param_exits_two(tgl_dir, tmp_path, capsys):
    rc = sweep.main(["--srcdata", str(tgl_dir), "--param", "nope",
                     "--out-dir", str(tmp_path / "o")])
    assert rc == 2


def test_cli_bad_srcdata_exits_two(tmp_path, capsys):
    rc = sweep.main(["--srcdata", str(tmp_path / "missing")])
    assert rc == 2
    assert "not a directory" in capsys.readouterr().err
