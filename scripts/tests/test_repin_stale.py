"""tools/repin_stale.py lists baselines whose recorded exe is far behind HEAD."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import repin_stale as rs  # noqa: E402


def test_scan_and_stale(tmp_path, monkeypatch):
    det, sv = tmp_path / "det", tmp_path / "sv"
    det.mkdir()
    sv.mkdir()
    (det / "mc2_03.json").write_text(json.dumps({"mission": "mc2_03", "sha": "old", "frames": 10, "env": {"A": "1"}}))
    (sv / "mc2_01.json").write_text(json.dumps({"mission": "mc2_01", "exe": {"sha": "new"}}))
    (sv / "mc2_02.json").write_text(json.dumps({"mission": "mc2_02", "exe": {"sha": "ghost"}}))
    monkeypatch.setattr(rs, "commits_behind", lambda sha: {"old": 120, "new": 3}.get(sha))
    rows = rs.scan(str(det), str(sv))
    assert {(r["kind"], r["mission"]) for r in rows} == {("determinism", "mc2_03"), ("saveload", "mc2_01"), ("saveload", "mc2_02")}
    assert {r["mission"] for r in rs.stale(rows, 50)} == {"mc2_03", "mc2_02"}  # behind > limit, and an unknown sha
    assert {r["mission"] for r in rs.stale(rows, 200)} == {"mc2_02"}


def test_repin_commands(tmp_path):
    sl = {"kind": "saveload", "mission": "mc2_01", "data": {}}
    cmd = rs.repin_command(sl, "inst", 30, str(tmp_path))
    assert len(cmd) == 1 and "baseline" in cmd[0] and "--baseline" not in cmd[0] and "mc2_01" in cmd[0]
    det = {"kind": "determinism", "mission": "mc2_03", "data": {"frames": 99, "env": {"A": "1"}}}
    run, write = rs.repin_command(det, "inst", 30, str(tmp_path))
    assert "run" in run and "A=1" in run and "write-baseline" in write and "99" in write


def _gate_steps():
    import merge_gate
    return {n: c for n, c, _e in merge_gate.plan(1, "PROOF")}


def test_commands_use_the_merge_gates_exact_form(tmp_path):
    """The re-pin records with the flags the gate checks with (headless-both/ticks/sim-speed, mask, first-save-only, headless)."""
    steps = _gate_steps()
    for m in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        gate_det = steps["determinism-" + m]
        run = rs.repin_command({"kind": "determinism", "mission": m, "data": {"frames": 5}}, "inst", 30, str(tmp_path))[0]
        for flag in ("--headless-both", "--ticks", "--sim-speed"):
            inferred_ticks = flag == "--ticks" and "--headless-both" in gate_det
            assert (flag in gate_det or inferred_ticks) == (flag in run), (m, flag)
            if flag != "--headless-both" and flag in gate_det:
                assert run[run.index(flag) + 1] == gate_det[gate_det.index(flag) + 1]
        if "--headless-both" in gate_det and "--ticks" not in gate_det:
            assert run[run.index("--ticks") + 1] == "5"
        assert "--single" not in run  # check-only
        gate_sl = steps["saveload-" + m]
        sl = rs.repin_command({"kind": "saveload", "mission": m, "data": {}}, "inst", 30, str(tmp_path))[0]
        for flag in ("--mask", "--first-save-only", "--headless"):
            assert (flag in gate_sl) == (flag in sl), (m, flag)
        assert sl[sl.index("--mask") + 1] == gate_sl[gate_sl.index("--mask") + 1]
        assert sl[sl.index("--install") + 1] == "inst" and sl[sl.index("--mission") + 1] == m


@pytest.mark.parametrize("mission", ["mc2_01", "mc2_03", "mc2_16"])
def test_headless_repin_uses_recorded_ticks(tmp_path, mission):
    data = json.loads((Path(rs.DET_DIR) / (mission + ".json")).read_text())
    run, write = rs.repin_command({"kind": "determinism", "mission": mission, "data": data}, "inst", 30, str(tmp_path))
    assert "--headless-both" in run and "--single" not in run
    assert run.count("--ticks") == 1
    assert run[run.index("--ticks") + 1] == write[write.index("--frames") + 1] == str(data["frames"])
    for key, value in (data.get("env") or {}).items():
        assert f"{key}={value}" in run and f"{key}={value}" in write


@pytest.mark.parametrize("frames", [None, 0, -1, "1000", True])
def test_repin_rejects_invalid_recorded_ticks(tmp_path, frames):
    with pytest.raises(SystemExit, match="positive recorded frame count"):
        rs.repin_command({"kind": "determinism", "mission": "mc2_03", "data": {"frames": frames}}, "inst", 30, str(tmp_path))


def test_default_out_root_is_the_lane_scratch(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_LANE_SCRATCH", str(tmp_path))
    a, b = rs.scratch_root(), rs.scratch_root()
    assert a != b and Path(a).parent == tmp_path and Path(a).is_dir()  # fresh each time, never a fixed shared path
    assert rs.scratch_root(str(tmp_path / "x")) == str(tmp_path / "x")
    monkeypatch.delenv("MC2_LANE_SCRATCH")
    assert Path(rs.scratch_root()).is_dir()


def test_existing_out_dir_fails_loudly(tmp_path):
    d = tmp_path / "repin_mc2_03"
    d.mkdir()
    (d / "old.log").write_text("x")
    with pytest.raises(SystemExit) as e:
        rs.fresh_out_dir(str(d))
    assert "already exists" in str(e.value)
    row = {"kind": "determinism", "mission": "mc2_03", "data": {}}
    with pytest.raises(SystemExit):
        rs.repin(row, "inst", 30, str(tmp_path))  # refused before any command runs
    assert rs.fresh_out_dir(str(tmp_path / "fresh")) == str(tmp_path / "fresh")


def test_real_repo_scans():
    rows = rs.scan()
    assert rows and all(r["kind"] in ("determinism", "saveload") for r in rows)


def test_repin_keeps_unknown_keys(tmp_path):
    f = tmp_path / "mc2_03.json"
    f.write_text(json.dumps({"mission": "mc2_03", "sha": "new"}))
    rs.keep_unknown_keys({"path": str(f), "data": {"mission": "mc2_03", "sha": "old", "schema_migration": {"v": 2}}})
    assert json.loads(f.read_text()) == {"mission": "mc2_03", "sha": "new", "schema_migration": {"v": 2}}


def test_commits_behind_counts_first_parent_only(monkeypatch):
    seen = []
    monkeypatch.setattr(rs, "git", lambda *a: seen.append(a) or ("commit" if a[0] == "cat-file" else "7"))
    assert rs.commits_behind("abc") == 7
    assert "--first-parent" in seen[-1] and seen[-1][-1] == "abc..HEAD"
