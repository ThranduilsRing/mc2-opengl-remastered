"""tools/repin_stale.py lists baselines whose recorded exe is far behind HEAD."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
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
    assert len(cmd) == 1 and "--baseline" in cmd[0] and "mc2_01" in cmd[0]
    det = {"kind": "determinism", "mission": "mc2_03", "data": {"frames": 99, "env": {"A": "1"}}}
    run, write = rs.repin_command(det, "inst", 30, str(tmp_path))
    assert "run" in run and "A=1" in run and "write-baseline" in write and "99" in write


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
