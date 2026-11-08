"""lane_slot release: drops the slot's registry claim, refuses a dirty slot (--discard archives the diff), status shows the claimer."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "repo_intel"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import lane_slot  # noqa: E402
import lane_registry  # noqa: E402
import console_child  # noqa: E402


def _git(r, *a):
    subprocess.run(["git", "-C", r, *a], check=True, capture_output=True, **console_child.child_kwargs(redirected=True))


@pytest.fixture
def slot(tmp_path, monkeypatch):
    """A temp git repo acting as both main root and slot01, on a lane branch, with a temp registry."""
    r = str(tmp_path / "repo").replace("\\", "/")
    _git(str(tmp_path), "init", "-q", r)
    for k, v in (("user.email", "t@t"), ("user.name", "t")):
        _git(r, "config", k, v)
    Path(r, "a.txt").write_text("one\n")
    _git(r, "add", "a.txt")
    _git(r, "commit", "-qm", "init")
    _git(r, "branch", "-M", lane_slot.NIFTY)
    _git(r, "switch", "-qc", "claude/lane-x")
    monkeypatch.setenv("MC2_LANE_REGISTRY_PATH", str(tmp_path / "reg.json"))
    monkeypatch.setattr(lane_slot, "MAIN_ROOT", r)
    monkeypatch.setattr(lane_slot, "SLOT_PATHS", [r])
    monkeypatch.setattr(lane_slot, "SCRATCH", str(tmp_path / "scratch"))
    monkeypatch.setattr(lane_slot, "INSTALLS", str(tmp_path / "inst"))
    monkeypatch.setattr(lane_slot, "STRAY_DIR", str(tmp_path / "stray"))
    return r


def _names():
    return {e["name"] for e in lane_registry.list_lanes(Path(lane_slot.MAIN_ROOT))["lanes"]}


def test_release_drops_claim_even_when_lane_name_differs_from_branch(slot):
    lane_registry.register_lane(Path(slot), "some-lane", worktree=slot)
    lane_registry.register_lane(Path(slot), "other-lane", worktree="A:/Games/mc2-slot09")
    assert lane_slot.claimed_worktrees() == {lane_slot.norm(slot), lane_slot.norm("A:/Games/mc2-slot09")}
    lane_slot.release(slot)
    assert _names() == {"other-lane"}
    assert lane_slot.git(slot, "symbolic-ref", "-q", "HEAD")[0] != 0  # detached


def test_dirty_slot_refused_then_discard_archives_diff(slot, tmp_path):
    lane_registry.register_lane(Path(slot), "claude/lane-x", worktree=slot)
    Path(slot, "a.txt").write_text("one\ntwo\n")
    Path(slot, "new.txt").write_text("untracked\n")
    with pytest.raises(SystemExit, match="--discard"):
        lane_slot.release(slot)
    assert "claude/lane-x" in _names()  # a refused release changes nothing
    lane_slot.release(slot, discard=True)
    patches = list((tmp_path / "stray").glob("*.patch"))
    assert len(patches) == 1
    text = patches[0].read_text()
    assert "+two" in text and "new.txt" in text
    assert lane_slot.git(slot, "status", "--porcelain")[1] == ""
    assert not Path(slot, "new.txt").exists()
    assert _names() == set()


def test_status_shows_claimer_and_age(slot, capsys):
    lane_registry.register_lane(Path(slot), "some-lane", worktree=slot)
    lane_slot.status()
    out = capsys.readouterr().out
    assert "some-lane (" in out and "claim" in out
    assert lane_slot.fmt_age(5 * 3600 + 120) == "5h02m" and lane_slot.fmt_age(90) == "1m"
