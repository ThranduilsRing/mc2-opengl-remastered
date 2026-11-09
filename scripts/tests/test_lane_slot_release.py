"""lane_slot release: drops the slot's registry claim, refuses a dirty slot (--discard archives the diff), status shows the claimer."""
import os
import subprocess
import sys
import tarfile
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
    Path(slot, "a.txt").write_text("one\nstaged\n")
    _git(slot, "add", "a.txt")
    Path(slot, "a.txt").write_text("one\nstaged\nunstaged\n")
    Path(slot, "new.txt").write_bytes(b"untracked\n")
    with pytest.raises(SystemExit, match="--discard"):
        lane_slot.release(slot)
    assert "claude/lane-x" in _names()  # a refused release changes nothing
    lane_slot.release(slot, discard=True)
    archive, = (tmp_path / "stray").iterdir()
    assert "+staged" in (archive / "index.patch").read_text()
    assert "unstaged" not in (archive / "index.patch").read_text()
    assert "+unstaged" in (archive / "worktree.patch").read_text()
    with tarfile.open(archive / "untracked.tar") as tar:
        assert tar.extractfile("new.txt").read() == b"untracked\n"
    assert lane_slot.git(slot, "status", "--porcelain")[1] == ""
    assert not Path(slot, "new.txt").exists()
    assert _names() == set()


def test_release_keeps_foreign_claim_named_like_branch(slot):
    lane_registry.register_lane(Path(slot), "claude/lane-x", worktree="A:/Games/mc2-slot09")
    lane_registry.register_lane(Path(slot), "local", worktree=slot)
    lane_slot.release(slot)
    assert _names() == {"claude/lane-x"}


def test_index_lock_failure_never_cleans_untracked_or_drops_claim(slot, tmp_path):
    lane_registry.register_lane(Path(slot), "local", worktree=slot)
    Path(slot, "a.txt").write_text("modified\n")
    Path(slot, "new.bin").write_bytes(b"\x00keep\xff")
    Path(slot, ".git/index.lock").write_text("injected lock")
    with pytest.raises(SystemExit, match="reset failed"):
        lane_slot.release(slot, discard=True)
    assert Path(slot, "new.bin").read_bytes() == b"\x00keep\xff"
    assert Path(slot, "a.txt").read_text() == "modified\n"
    assert _names() == {"local"}
    archive, = (tmp_path / "stray").iterdir()
    with tarfile.open(archive / "untracked.tar") as tar:
        assert tar.extractfile("new.bin").read() == b"\x00keep\xff"


@pytest.mark.parametrize("failure", ["index", "worktree", "list", "tar", "clean"])
def test_archive_and_cleanup_failures_abort_release(slot, monkeypatch, failure):
    lane_registry.register_lane(Path(slot), "local", worktree=slot)
    Path(slot, "new.txt").write_text("keep")
    real_git = lane_slot.git
    calls = []

    def git(path, *args):
        calls.append(args)
        if ((failure == "index" and "--cached" in args)
                or (failure == "worktree" and args[0] == "diff" and "--cached" not in args)
                or (failure == "clean" and args[0] == "clean")):
            return 1, "", "injected failure"
        return real_git(path, *args)

    monkeypatch.setattr(lane_slot, "git", git)
    if failure == "list":
        real_run = lane_slot.subprocess.run

        def run(cmd, **kwargs):
            if "ls-files" in cmd:
                raise subprocess.CalledProcessError(1, cmd)
            return real_run(cmd, **kwargs)

        monkeypatch.setattr(lane_slot.subprocess, "run", run)
    if failure == "tar":
        monkeypatch.setattr(tarfile.TarFile, "add", lambda *a, **k: (_ for _ in ()).throw(OSError("injected archive failure")))
    with pytest.raises(SystemExit, match="failed"):
        lane_slot.release(slot, discard=True)
    assert Path(slot, "new.txt").read_text() == "keep"
    assert _names() == {"local"}
    if failure != "clean":
        assert not any(c[0] in ("reset", "clean") for c in calls)


def test_registry_failure_is_reported(slot, monkeypatch):
    monkeypatch.setattr(lane_registry, "release_worktree", lambda *a: (_ for _ in ()).throw(OSError("registry unavailable")))
    with pytest.raises(SystemExit, match="registry release failed"):
        lane_slot.drop_claims(slot)


def test_status_shows_claimer_and_age(slot, capsys):
    lane_registry.register_lane(Path(slot), "some-lane", worktree=slot)
    lane_slot.status()
    out = capsys.readouterr().out
    assert "some-lane (" in out and "claim" in out
    assert lane_slot.fmt_age(5 * 3600 + 120) == "5h02m" and lane_slot.fmt_age(90) == "1m"
