"""lane_slot release: drops the slot's registry claim, refuses a dirty slot (--discard archives the diff), status shows the claimer."""
import os
import subprocess
import sys
import time
import tarfile
from pathlib import Path
from types import SimpleNamespace

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


def test_release_and_reacquire_keep_install_and_scratch(slot):
    install = Path(lane_slot.INSTALLS) / "lane-x"
    scratch = Path(lane_slot.SCRATCH) / (Path(slot).name + "-proof")
    for p in (install, scratch):
        p.mkdir(parents=True)
        (p / "retained").write_text("cached")
    lane_registry.register_lane(Path(slot), "claude/lane-x", worktree=slot)
    assert lane_slot.release(slot) == []
    lane_slot.acquire("claude/lane-x", no_warm=True)
    assert all((p / "retained").read_text() == "cached" for p in (install, scratch))


def test_two_branches_acquire_the_same_slot_install(slot, capsys):
    expected = lane_slot.slot_install(slot)
    assert expected == f"{lane_slot.INSTALLS}/pc-lane-01"
    lane_slot.release(slot)
    for branch in ("claude/first", "claude/second"):
        lane_slot.acquire(branch, no_warm=True)
        entries = lane_registry.list_lanes(Path(slot))["lanes"]
        assert next(e for e in entries if e["name"] == branch)["deploy_lane"] == expected
        assert f"install {expected}" in capsys.readouterr().out
        Path(expected).mkdir(parents=True, exist_ok=True)
        Path(expected, "retained").write_text("cache")
        lane_slot.release(slot)
        assert Path(expected, "retained").read_text() == "cache"


@pytest.fixture
def gc_setup(slot, tmp_path, monkeypatch):
    lane_registry.register_lane(Path(slot), "unrelated", worktree="other-slot")
    monkeypatch.setattr(lane_slot, "CRASH_ARCHIVE", str(tmp_path / "archive"))
    monkeypatch.setattr(lane_slot.shutil, "disk_usage", lambda p: SimpleNamespace(free=100 * 2**30))
    Path(lane_slot.INSTALLS).mkdir()
    return Path(lane_slot.INSTALLS)


def test_gc_archives_before_delete_and_never_touches_protected(slot, gc_setup, monkeypatch):
    install = gc_setup / "lane-x"
    (install / "crashes").mkdir(parents=True)
    (install / "crashes/failure.dmp").write_bytes(b"dump")
    (install / "mc2.pdb").write_bytes(b"symbols")
    (install / "outside.dmp").write_bytes(b"wer")
    protected = gc_setup / "claude/owner-test-do-not-touch"
    protected.mkdir(parents=True)
    _git(slot, "branch", "claude/owner-test-do-not-touch")
    rmtree = lane_slot.shutil.rmtree

    def checked_delete(path):
        archive = Path(lane_slot.CRASH_ARCHIVE) / "claude/lane-x/crashes"
        assert len(list(archive.rglob("*.dmp"))) == 2
        assert len(list(archive.rglob("*.pdb"))) == 1
        rmtree(path)

    monkeypatch.setattr(lane_slot.shutil, "rmtree", checked_delete)
    assert str(install).replace("\\", "/") in [p.replace("\\", "/") for p in lane_slot.gc()]
    assert not install.exists() and protected.exists()


@pytest.mark.parametrize("policy", ["young", "old", "low", "claimed"])
def test_gc_only_unclaimed_eligible_trees(slot, gc_setup, monkeypatch, policy):
    Path(slot, "a.txt").write_text("unmerged")
    _git(slot, "commit", "-qam", "unmerged")
    install = gc_setup / "lane-x"
    install.mkdir()
    scratch = Path(lane_slot.SCRATCH) / (Path(slot).name + "-proof")
    scratch.mkdir(parents=True)
    if policy == "old":
        for p in (install, scratch):
            os.utime(p, (time.time() - 100 * 3600,) * 2)
    if policy in ("low", "claimed"):
        monkeypatch.setattr(lane_slot.shutil, "disk_usage", lambda p: SimpleNamespace(free=1))
    if policy == "claimed":
        lane_registry.register_lane(Path(slot), "different-name", worktree=slot)
    lane_slot.gc(hours=72)
    assert install.exists() == (policy in ("young", "claimed"))
    assert scratch.exists() == install.exists()


def test_gc_archive_failure_preserves_install(slot, gc_setup, monkeypatch):
    install = gc_setup / "lane-x"
    install.mkdir()
    (install / "fault.dmp").write_bytes(b"evidence")
    monkeypatch.setattr(lane_slot.shutil, "move", lambda *a: (_ for _ in ()).throw(OSError("archive failed")))
    with pytest.raises(OSError, match="archive failed"):
        lane_slot.gc()
    assert (install / "fault.dmp").exists()


def test_gc_corrupt_registry_fails_closed(slot, gc_setup):
    install = gc_setup / "lane-x"
    install.mkdir()
    lane_registry.store_path(Path(slot)).write_text("broken")
    with pytest.raises(ValueError):
        lane_slot.gc()
    assert install.exists()


def test_gc_protects_claimed_slot_aliases_even_under_disk_pressure(slot, gc_setup, monkeypatch):
    lane_registry.register_lane(Path(slot), "other-lane", worktree="A:/Games/mc2-slot09", deploy_lane="custom-install")
    paths = [gc_setup / name for name in ("lane9", "lane09", "slot09", "custom-install")]
    for p in paths:
        p.mkdir()
    monkeypatch.setattr(lane_slot.shutil, "disk_usage", lambda p: SimpleNamespace(free=1))
    lane_slot.gc()
    assert all(p.exists() for p in paths)


def test_gc_retains_slot_installs_until_disk_pressure(slot, gc_setup, monkeypatch):
    install = Path(lane_slot.slot_install(slot))
    install.mkdir()
    os.utime(install, (time.time() - 100 * 3600,) * 2)
    lane_slot.gc(hours=0)
    assert install.exists()
    lane_registry.register_lane(Path(slot), "active", worktree=slot)
    monkeypatch.setattr(lane_slot.shutil, "disk_usage", lambda p: SimpleNamespace(free=1))
    lane_slot.gc(hours=0)
    assert install.exists()
    lane_registry.release_lane(Path(slot), "active")
    lane_slot.gc(hours=0)
    assert not install.exists()


def test_gc_ages_out_orphaned_branch_install(slot, gc_setup):
    orphan = gc_setup / "deleted-branch"
    orphan.mkdir()
    os.utime(orphan, (time.time() - 100 * 3600,) * 2)
    lane_slot.gc(hours=72)
    assert not orphan.exists()


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


def _exclude_text(r):
    rc, p, _ = lane_slot.git(r, "rev-parse", "--git-path", "info/exclude")
    f = Path(p) if os.path.isabs(p) else Path(r, p)
    return f.read_text() if f.exists() else ""


def test_hook_seeded_agent_file_alone_is_clean(slot):
    Path(slot, ".claude", "agents").mkdir(parents=True)
    Path(slot, ".claude", "agents", "mc2-lane.md").write_text("seeded")
    assert lane_slot.slot_info(slot, set())["clean"]


def test_notes_only_worktree_is_not_free(slot):
    Path(slot, "notes").mkdir()
    Path(slot, "notes", "lane-state.md").write_text("n")
    info = lane_slot.slot_info(slot, set())
    assert not info["clean"] and not lane_slot.is_free(info)


def test_discard_archives_notes_and_acquire_writes_no_common_exclude(slot, tmp_path):
    lane_slot.acquire("claude/lane-y", no_warm=True)
    assert "notes/" not in _exclude_text(slot).splitlines()
    Path(slot, "notes").mkdir()
    Path(slot, "notes", "lane-state.md").write_text("keep me")
    lane_slot.release(slot, discard=True)
    archives = list(Path(lane_slot.STRAY_DIR).rglob("untracked.tar"))
    assert archives
    with tarfile.open(archives[0]) as tar:
        assert "notes/lane-state.md" in tar.getnames()


def test_acquire_refuses_when_registration_fails(slot, monkeypatch):
    monkeypatch.setattr(lane_registry, "register_lane", lambda *a, **k: (_ for _ in ()).throw(OSError("registry unavailable")))
    with pytest.raises(SystemExit, match="registration failed"):
        lane_slot.acquire("claude/lane-z", no_warm=True)
