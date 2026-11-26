"""Mission batching uses one process and preserves the existing shot filenames."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_visual_capture as capture
from smoke_lib import host_lease


def pose(name="view"):
    return {"name": name, "pos": [1, 2, 3], "rot": 45, "proj": 30, "alt": 200}


@pytest.mark.parametrize("plan", [[], {}, [{"mission": "../escape", "poses": [pose()]}],
    [{"mission": "mc2_01", "poses": []}],
    [{"mission": "mc2_01", "poses": [pose(), pose()]}],
    [{"mission": "mc2_01", "poses": [pose()], "frames": 0}],
    [{"mission": "mc2_01", "poses": [pose()], "tod": [0.25, 0.75]}]])
def test_bad_plan_is_rejected(tmp_path, plan):
    source = tmp_path / "plan.json"
    source.write_text(json.dumps(plan))
    with pytest.raises(ValueError):
        capture.load_capture_plan(source, tmp_path / "inputs")


def test_two_missions_launch_once_each_and_keep_receipt_names(tmp_path, monkeypatch):
    source = tmp_path / "plan.json"
    source.write_text(json.dumps([
        {"mission": "mc2_01", "poses": [pose("a"), pose("b")], "frames": 120},
        {"mission": "mc2_10", "poses": [pose("night"), pose("night_dup")], "frames": 180}]))
    planned = capture.load_capture_plan(source, tmp_path / "inputs")
    extra = tmp_path / "other.json"
    extra.write_text(json.dumps({"mission": "mc2_01", "bookmarks": [pose("a")]}))
    calls, releases = [], []

    def run(**kw):
        mission, marks = capture._load_bookmarks(kw["bookmarks_path"])
        calls.append(mission)
        dest = kw["out_dir"]
        dest.mkdir(parents=True)
        shots = []
        for mark in marks:
            png = dest / f"{mission}_{capture._safe_name(mark['name'])}.png"
            png.write_bytes(b"png")
            png.with_suffix(".json").write_text("{}")
            shots.append({"name": mark["name"], "png": str(png), "present": True})
        return {"bookmarks": shots, "all_present": True}

    monkeypatch.setattr(capture, "run_one", run)
    monkeypatch.setattr(host_lease, "release", lambda: releases.append(True))
    results = capture.batch_capture([planned[0][0], extra, planned[1][0]], tmp_path / "out")
    assert calls == ["mc2_01", "mc2_10"]
    assert len(releases) == 2
    assert Path(results[extra]["bookmarks"][0]["png"]).name == "mc2_01_a.png"
    assert all(Path(s["png"]).exists() for r in results.values() for s in r["bookmarks"])
    receipt = json.loads((tmp_path / "out" / "capture_batch.json").read_text())
    assert receipt["launch_count"] == 2


def test_failed_launch_releases_lease(tmp_path, monkeypatch):
    source = tmp_path / "view.json"
    source.write_text(json.dumps({"mission": "mc2_01", "bookmarks": [pose()]}))
    released = []
    def fail(**kw):
        raise RuntimeError("launch failure")
    monkeypatch.setattr(capture, "run_one", fail)
    monkeypatch.setattr(host_lease, "release", lambda: released.append(True))
    with pytest.raises(RuntimeError):
        capture.batch_capture([source], tmp_path / "out")
    assert released == [True]
