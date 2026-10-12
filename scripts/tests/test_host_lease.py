"""smoke_lib.host_lease: one lane's engine at a time, host-wide (no real game; the two-lane proof runs real child processes from two
worktree copies against one shared lease file)."""
import json
import os
import shutil
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts import console_child  # noqa: E402
from scripts.smoke_lib import host_lease, nofocus  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


@pytest.fixture()
def lease(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.setattr(host_lease, "_registered", False)
    monkeypatch.setattr(host_lease, "_off_logged", False)
    yield tmp_path / "lease.json"
    host_lease.release()


def _foreign_holder(path, pid):
    path.write_text(json.dumps({"pid": pid, "created": None, "label": "other lane", "argv": ["run_smoke.py"], "cwd": "A:/Games/mc2-lane9"}),
                    encoding="utf-8")


def test_free_host_takes_the_lease(lease):
    host_lease.acquire("t", scan=lambda: [], log=lambda m: None)
    assert json.loads(lease.read_text())["pid"] == os.getpid() and host_lease.holds()
    host_lease.release()
    assert not lease.exists()


def test_a_live_holder_blocks_with_no_wait(lease):
    _foreign_holder(lease, os.getppid())
    with pytest.raises(host_lease.HostLeaseBusy, match="held by other lane"):
        host_lease.acquire("t", wait=False, scan=lambda: [], log=lambda m: None)


def test_a_dead_holder_is_taken_over(lease, monkeypatch):
    _foreign_holder(lease, 2 ** 22 + 12345)
    monkeypatch.setattr(host_lease.child_registry, "pid_alive", lambda pid: pid == os.getpid())
    host_lease.acquire("t", wait=False, scan=lambda: [], log=lambda m: None)
    assert json.loads(lease.read_text())["pid"] == os.getpid()


def test_waits_until_the_foreign_game_exits(lease):
    games = [[(4242, "mc2.exe")], [(4242, "mc2.exe")], []]
    sleeps = []
    host_lease.acquire("t", scan=lambda: games.pop(0), sleep=sleeps.append, log=lambda m: None)
    assert len(sleeps) == 2 and host_lease.holds()


def test_foreign_game_with_no_wait_fails_and_does_not_keep_the_lease(lease):
    with pytest.raises(host_lease.HostLeaseBusy, match="mc2.exe pid 4242"):
        host_lease.acquire("t", wait=False, scan=lambda: [(4242, "mc2.exe")], log=lambda m: None)
    assert not lease.exists()


def test_times_out_with_a_countdown_line(lease):
    t = iter([0.0, 1.0, 2.0, 99.0, 100.0, 101.0, 102.0])
    lines = []
    with pytest.raises(host_lease.HostLeaseBusy, match="gave up"):
        host_lease.acquire("t", timeout_s=10, scan=lambda: [(1, "EditRel.exe")], sleep=lambda s: None,
                           clock=lambda: next(t), log=lines.append)
    assert not lease.exists()
    assert any(line.startswith("[wait] host game lease:") and "giving up in" in line for line in lines)


def test_reentry_does_not_see_its_own_games_as_foreign(lease, monkeypatch):
    host_lease.acquire("t", scan=lambda: [], log=lambda m: None)
    monkeypatch.setattr(host_lease, "foreign_engines", lambda: [(1, "mc2.exe")])
    host_lease.ensure_held("again")        # must return without scanning or waiting
    assert host_lease.holds()


# ------------------------------------------------------------- the 2026-10-12 regression: test switches never switch the lease off

def test_cursor_guard_switch_does_not_disable_the_lease(lease, monkeypatch):
    """Three lanes ran three engines at once because MC2_CURSOR_GUARD_OFF=1 (set for every test tree and exported by the lanes' smoke
    shells) used to mean "no host lease". A launch under that switch must still queue behind a live holder."""
    monkeypatch.setenv("MC2_CURSOR_GUARD_OFF", "1")
    monkeypatch.setenv("MC2_HOST_LEASE_TIMEOUT", "0")
    _foreign_holder(lease, os.getppid())
    assert not host_lease.disabled(lambda m: None)
    with pytest.raises(host_lease.HostLeaseBusy, match="held by other lane"):
        nofocus._hold_host_lease(["A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe", "--mission", "mc2_01"])


def test_off_switch_needs_the_owner(lease, monkeypatch):
    lines = []
    monkeypatch.setenv("MC2_HOST_LEASE", "off")
    monkeypatch.delenv("MC2_OWNER_NO_LEASE", raising=False)
    assert not host_lease.disabled(lines.append) and "MC2_OWNER_NO_LEASE=1" in lines[0]
    _foreign_holder(lease, os.getppid())
    with pytest.raises(host_lease.HostLeaseBusy):
        host_lease.acquire("t", wait=False, scan=lambda: [], log=lines.append)
    monkeypatch.setenv("MC2_OWNER_NO_LEASE", "1")
    assert host_lease.disabled(lines.append)
    host_lease.acquire("t", wait=False, scan=lambda: [], log=lines.append)      # returns at once, takes nothing
    assert json.loads(lease.read_text())["label"] == "other lane" and any("OFF" in line for line in lines)


def test_a_test_without_a_private_lease_file_trips(monkeypatch):
    monkeypatch.delenv("MC2_HOST_LEASE_PATH", raising=False)
    with pytest.raises(RuntimeError, match="private file"):
        host_lease.lease_path()


def test_status_and_clear_dead(lease, monkeypatch, capsys):
    assert host_lease.main(["status"]) == 0 and "free" in capsys.readouterr().out
    _foreign_holder(lease, os.getppid())
    assert host_lease.main(["status"]) == 1 and "ALIVE" in capsys.readouterr().out
    assert host_lease.main(["clear-dead"]) == 1 and lease.exists()           # a live holder is never cleared
    monkeypatch.setattr(host_lease.child_registry, "pid_alive", lambda pid: pid == os.getpid())
    assert host_lease.main(["status"]) == 2 and "DEAD" in capsys.readouterr().out
    assert host_lease.main(["--clear-dead"]) == 0 and not lease.exists()
    assert host_lease.main(["clear-dead"]) == 0 and "no lease" in capsys.readouterr().out


def test_private_lease_never_scans_the_host(lease, monkeypatch):
    monkeypatch.setattr(host_lease.child_registry, "engine_processes", lambda *a, **k: [(1, "mc2.exe")])
    assert host_lease.foreign_engines() == []
    assert host_lease.status()["engines"] == []


# ------------------------------------------------------------- two lanes, two worktree copies, one lease file

_LANE_SCRIPT = """
import os, sys, time
sys.path.insert(0, os.getcwd())
from scripts.smoke_lib import host_lease
host_lease.POLL_S = 0.05
host_lease.REPORT_EVERY_S = 0.5
flag, go = sys.argv[1], sys.argv[2]
host_lease.acquire("lane " + os.path.basename(os.getcwd()), log=lambda m: print(m, flush=True))
open(flag, "w").write(str(os.getpid()))
while not os.path.exists(go):
    time.sleep(0.05)
host_lease.release()
"""


def _worktree_copy(root, name):
    lane = root / name
    (lane / "scripts" / "smoke_lib").mkdir(parents=True)
    (lane / "scripts" / "__init__.py").write_text("", encoding="utf-8")
    for mod in ("__init__.py", "bounded_wait.py", "child_registry.py", "host_lease.py"):
        shutil.copy(os.path.join(ROOT, "scripts", "smoke_lib", mod), lane / "scripts" / "smoke_lib" / mod)
    return lane


def _wait_for(path, timeout_s=20.0):
    deadline = time.monotonic() + timeout_s
    while not os.path.exists(path):
        assert time.monotonic() < deadline, f"timed out waiting for {path}"
        time.sleep(0.05)


def test_two_lanes_from_two_worktree_copies_share_one_lease(tmp_path, monkeypatch):
    """Lane B, run from its own worktree copy, must queue behind lane A's lease and print the countdown; it gets the lease only after
    A releases. The lease file is one shared path, not something under either worktree."""
    lease_file = tmp_path / "host" / "lease.json"
    lane_a, lane_b = _worktree_copy(tmp_path, "laneA"), _worktree_copy(tmp_path, "laneB")
    env = dict(os.environ, MC2_HOST_LEASE_PATH=str(lease_file), MC2_CURSOR_GUARD_OFF="1", PYTHONIOENCODING="utf-8")

    def lane(cwd, tag):
        # Output goes to a file: a pipe nobody drains blocks the lane on its countdown lines.
        log = open(tmp_path / f"{tag}.log", "w", encoding="utf-8")
        return subprocess.Popen([sys.executable, "-c", _LANE_SCRIPT, str(tmp_path / f"{tag}.held"), str(tmp_path / f"{tag}.go")],
                                cwd=str(cwd), env=env, **console_child.child_kwargs(redirected=True, stdout=log, stderr=subprocess.STDOUT))

    a = lane(lane_a, "a")
    try:
        _wait_for(tmp_path / "a.held")
        assert json.loads(lease_file.read_text())["label"] == "lane laneA"
        status = subprocess.run([sys.executable, "-m", "scripts.smoke_lib.host_lease", "status"], cwd=str(lane_b), env=env,
                                **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
        assert status.returncode == 1 and "lane laneA" in status.stdout and "ALIVE" in status.stdout
        b = lane(lane_b, "b")
        try:
            time.sleep(1.0)
            assert b.poll() is None and not (tmp_path / "b.held").exists(), "lane B took the lease while lane A held it"
            (tmp_path / "a.go").write_text("go", encoding="utf-8")
            assert a.wait(timeout=20) == 0
            _wait_for(tmp_path / "b.held")
            assert json.loads(lease_file.read_text())["label"] == "lane laneB"
            (tmp_path / "b.go").write_text("go", encoding="utf-8")
            assert b.wait(timeout=20) == 0
        finally:
            if b.poll() is None:
                b.kill()
        out = (tmp_path / "b.log").read_text(encoding="utf-8")
        assert "[wait] host game lease: the host game lease is held by lane laneA" in out, out
        assert not lease_file.exists()
    finally:
        if a.poll() is None:
            a.kill()
