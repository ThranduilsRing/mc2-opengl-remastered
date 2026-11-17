"""lane_slot: an existing branch is never moved, short slot names, warm pid / --no-warm."""
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import lane_slot  # noqa: E402
import console_child  # noqa: E402


def _repo(tmp_path):
    r = str(tmp_path / "r")
    subprocess.run(["git", "init", "-q", r], check=True, **console_child.child_kwargs())
    for k, v in (("user.email", "t@t"), ("user.name", "t")):
        subprocess.run(["git", "-C", r, "config", k, v], check=True, **console_child.child_kwargs())
    shas = []
    for n in range(2):
        subprocess.run(["git", "-C", r, "commit", "-q", "--allow-empty", "-m", str(n)], check=True, **console_child.child_kwargs())
        shas.append(lane_slot.git(r, "rev-parse", "HEAD")[1])
    lane_slot.git(r, "branch", "lane", shas[0])
    return r, shas


def test_branch_plan_never_moves_an_existing_branch(tmp_path):
    r, (old, new) = _repo(tmp_path)
    assert lane_slot.branch_plan("lane", None, False, r) == (["switch", "lane"], "lane")   # own tip
    assert lane_slot.branch_plan("lane", old, False, r) == (["switch", "lane"], "lane")    # base == tip: no move
    with pytest.raises(SystemExit, match="refusing to move"):
        lane_slot.branch_plan("lane", new, False, r)
    assert lane_slot.branch_plan("lane", new, True, r) == (["switch", "-C", "lane", new], new)   # explicit reset only
    assert lane_slot.branch_plan("fresh", new, False, r) == (["switch", "-c", "fresh", new], new)
    assert lane_slot.branch_plan("fresh", None, False, r)[0][:2] == ["switch", "-c"]
    with pytest.raises(SystemExit, match="does not resolve"):
        lane_slot.branch_plan("lane", "nope", False, r)
    assert lane_slot.git(r, "rev-parse", "lane")[1] == old


def test_release_takes_short_names():
    for arg in ("slot03", "mc2-slot03", "3", "03", "A:/Games/mc2-slot03", "A:/Games/mc2-slot03/"):
        assert lane_slot.slot_path(arg) == "A:/Games/mc2-slot03"
    assert lane_slot.slot_path("A:/Games/mc2-main") == "A:/Games/mc2-main"
    assert lane_slot.slot_path("slot13") == "slot13"


def test_acquire_records_warm_pid_and_no_warm(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(lane_slot, "MAIN_ROOT", str(tmp_path))
    slot = tmp_path / "slot"
    (slot / "build64-ninja").mkdir(parents=True)
    free = {"path": str(slot), "exists": True, "clean": True, "claimed": False, "merged": True, "branch": None}
    monkeypatch.setattr(lane_slot, "claimed_worktrees", lambda: set())
    monkeypatch.setattr(lane_slot, "SLOT_PATHS", [str(slot)])
    monkeypatch.setattr(lane_slot, "slot_info", lambda p, c: free)
    monkeypatch.setattr(lane_slot, "branch_plan", lambda *a: (["switch", "b"], "b"))
    monkeypatch.setattr(lane_slot, "git", lambda root, *a: (0, "deadbeef", ""))
    monkeypatch.setitem(sys.modules, "lane_registry", type("M", (), {"register_lane": staticmethod(lambda *a, **k: None)}))
    out = type("R", (), {"stdout": "warm: pid 4242 building build64-ninja at HEAD; log x\n", "stderr": ""})
    monkeypatch.setattr(lane_slot.subprocess, "run", lambda *a, **k: out)
    lane_slot.acquire("b", None)
    assert (slot / "build64-ninja" / "warm.pid").read_text().strip() == "4242"
    (slot / "build64-ninja" / "warm.pid").unlink()
    lane_slot.acquire("b", None, no_warm=True)
    assert not (slot / "build64-ninja" / "warm.pid").exists() and "HEAD deadbeef" in capsys.readouterr().out


def test_slot_claim_covers_selection_switch_and_registration(tmp_path, monkeypatch):
    import build_tokens
    from contextlib import contextmanager
    held = []
    @contextmanager
    def lock(directory):
        held.append(directory)
        try:
            yield
        finally:
            held.pop()
    monkeypatch.setattr(build_tokens, "_queue_lock", lock)
    monkeypatch.setattr(lane_slot, "MAIN_ROOT", str(tmp_path))
    monkeypatch.setattr(lane_slot, "strip_stale_excludes", lambda: None)
    def claims():
        assert held, "slot selection must hold the acquisition lock"
        return set()
    monkeypatch.setattr(lane_slot, "claimed_worktrees", claims)
    monkeypatch.setattr(lane_slot, "pick_slot", lambda slots: (str(tmp_path), False))
    monkeypatch.setattr(lane_slot, "SLOT_PATHS", [])
    monkeypatch.setattr(lane_slot, "branch_plan", lambda *a: (["switch", "b"], "b"))
    def git(*args):
        if args[1:] == ("rev-parse", "--git-common-dir"):
            return 0, str(tmp_path / ".git"), ""
        assert held
        return 0, "tip", ""
    def register(*args, **kwargs):
        assert held
    monkeypatch.setattr(lane_slot, "git", git)
    monkeypatch.setattr(lane_slot, "slot_install", lambda path: "unused")
    monkeypatch.setitem(sys.modules, "lane_registry", type("M", (), {"register_lane": staticmethod(register)}))
    lane_slot.acquire("b", no_warm=True)
    assert not held
