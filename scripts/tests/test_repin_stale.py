"""tools/repin_stale.py lists baselines whose recorded exe is far behind HEAD."""
from __future__ import annotations

import json
import os
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
    assert "--baseline" not in run


@pytest.fixture
def repin_install(tmp_path, monkeypatch):
    monkeypatch.setattr(rs, "ROOT", str(tmp_path))
    monkeypatch.setattr(rs, "git", lambda *args: "a" * 40)
    install, gate = tmp_path / "install", tmp_path / "gate"
    for folder in (install, gate):
        folder.mkdir()
        (folder / "mission.fst").write_bytes(b"stock")
    (install / "mc2.exe").write_bytes(b"MC2FP1|" + b"a" * 40 + b"|0|test|2026-11-24T00:00:00|A:/src|FP1END")
    monkeypatch.setattr(rs, "GATE_INSTALL", str(gate))
    return str(install)


@pytest.mark.parametrize("before,pair,after", [(0, 0, 0), (1, 1, 0), (1, 0, 1), (1, 0, 0)])
def test_repin_checks_current_and_restores_failed_pin(tmp_path, monkeypatch, capsys, repin_install, before, pair, after):
    path = tmp_path / "mc2_03.json"
    original = b'{"mission": "mc2_03", "sha": "old", "frames": 5}\n'
    path.write_bytes(original)
    row = {"kind": "determinism", "mission": "mc2_03", "path": str(path), "data": {"frames": 5}}
    calls = []
    replace = rs.os.replace
    restored = []
    def atomic_replace(source, destination):
        assert Path(source).parent == path.parent and Path(source) != path
        assert Path(source).read_bytes() == original
        restored.append(Path(source))
        replace(source, destination)
    monkeypatch.setattr(rs.os, "replace", atomic_replace)
    def fake_call(cmd, cwd, **kwargs):
        assert cwd == rs.ROOT
        if "check" in cmd:
            assert cmd[1:4] == ["tools/sim_determinism.py", "check", "mc2_03"]
            assert Path(cmd[cmd.index("--install") + 1]) == Path(repin_install)
            assert "--headless-both" in cmd and "--single" in cmd
            assert "--out" in cmd and "env" in kwargs
            calls.append("check")
            return before if calls.count("check") == 1 else after
        if "run" in cmd:
            calls.append("pair")
            log = Path(rs.run_dir(str(tmp_path), row)) / "a/mc2_03.log"
            log.parent.mkdir(parents=True)
            log.write_text("pair log")
            return pair
        assert "write-baseline" in cmd
        calls.append("write")
        path.write_text('{"sha": "new"}')
        return 0
    monkeypatch.setattr(rs.subprocess, "call", fake_call)
    assert rs.repin(row, repin_install, 30, str(tmp_path)) == (before == 0 or pair == after == 0)
    assert calls == (["check"] if before == 0 else ["check", "pair"] if pair else ["check", "pair", "write", "check"])
    if before == 0 or pair or after:
        assert path.read_bytes() == original
    else:
        assert json.loads(path.read_text())["sha"] == "new"
    assert bool(restored) == (before != 0 and (pair != 0 or after != 0))
    assert all(not p.exists() for p in restored)
    if before == 0:
        assert "stale by distance, still valid" in capsys.readouterr().out


@pytest.mark.parametrize("broken", ["exe", "dirty", "data", "overlay", "missing_gate"])
def test_repin_refuses_wrong_install(tmp_path, monkeypatch, repin_install, broken):
    install = Path(repin_install)
    if broken in ("exe", "dirty"):
        exe = install / "mc2.exe"
        exe.write_bytes(exe.read_bytes().replace(b"a" * 40, b"b" * 40) if broken == "exe" else exe.read_bytes().replace(b"|0|", b"|1|"))
    elif broken == "data":
        (install / "mission.fst").write_bytes(b"different stock")
    elif broken == "overlay":
        manifest = tmp_path / "tests/smoke/sim_determinism/data_overlay.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text('{"overlay": {"missing.fit": "0000"}}')
    else:
        monkeypatch.setattr(rs, "GATE_INSTALL", str(tmp_path / "absent"))
    monkeypatch.setattr(rs.subprocess, "call", lambda *a, **kw: pytest.fail("refused install launched a process"))
    row = {"kind": "determinism", "mission": "mc2_03", "data": {"frames": 5}}
    assert not rs.repin(row, repin_install, 30, str(tmp_path))


def test_gate_check_timeout_is_reported(tmp_path, monkeypatch, capsys):
    def timeout(cmd, **kwargs):
        assert kwargs['timeout'] == 600
        raise rs.subprocess.TimeoutExpired(cmd, kwargs['timeout'])
    monkeypatch.setattr(rs.subprocess, 'call', timeout)
    assert rs.gate_check({'kind': 'determinism', 'mission': 'mc2_03'}, 'inst', str(tmp_path), 'before') == 124
    assert 'timed out' in capsys.readouterr().out


@pytest.mark.parametrize('invalid', [False, True])
def test_check_valid_checks_each_stale_row_without_repin(tmp_path, monkeypatch, capsys, invalid):
    rows = []
    for mission, behind in [('mc2_01', 100), ('mc2_03', 101), ('mc2_10', 1)]:
        path = tmp_path / (mission + '.json')
        path.write_bytes(b'original baseline\n')
        rows.append(dict(kind='determinism', mission=mission, path=str(path), behind=behind, sha='old'))
    monkeypatch.setattr(rs, 'scan', lambda: rows)
    monkeypatch.setattr(rs, 'repin', lambda *a: pytest.fail('read-only check tried to re-pin'))
    calls = []
    def check(cmd, **kwargs):
        assert 'check' in cmd and 'write-baseline' not in cmd and 'run' not in cmd
        assert Path(cmd[cmd.index('--install') + 1]) == tmp_path
        calls.append(cmd[3])
        return int(invalid and cmd[3] == 'mc2_01')
    monkeypatch.setattr(rs.subprocess, 'call', check)
    assert rs.main(['--check-valid', '--install', str(tmp_path), '--out-root', str(tmp_path / 'proof')]) == int(invalid)
    assert calls == ['mc2_01', 'mc2_03']
    assert all(Path(row['path']).read_bytes() == b'original baseline\n' for row in rows)
    out = capsys.readouterr().out
    assert ('INVALID' if invalid else 'VALID') + ' determinism:mc2_01' in out
    assert 'VALID determinism:mc2_03' in out


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
    d = tmp_path / "repin_determinism_mc2_03"
    d.mkdir()
    (d / "old.log").write_text("x")
    with pytest.raises(SystemExit) as e:
        rs.fresh_out_dir(str(d))
    assert "already exists" in str(e.value)
    row = {"kind": "determinism", "mission": "mc2_03", "data": {}}
    with pytest.raises(SystemExit):
        rs.repin(row, "inst", 30, str(tmp_path))  # refused before any command runs
    assert rs.fresh_out_dir(str(tmp_path / "fresh")) == str(tmp_path / "fresh")


def test_two_kinds_for_one_mission_get_distinct_run_dirs():
    assert rs.run_dir("t", {"kind": "determinism", "mission": "m"}) != rs.run_dir("t", {"kind": "saveload", "mission": "m"})


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
