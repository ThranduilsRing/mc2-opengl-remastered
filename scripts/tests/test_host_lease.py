"""smoke_lib.host_lease: one lane's engine at a time, host-wide (no real game, no sleeping)."""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts.smoke_lib import host_lease  # noqa: E402


@pytest.fixture()
def lease(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.setattr(host_lease, "_registered", False)
    yield tmp_path / "lease.json"
    host_lease.release()


def _foreign_holder(path, pid):
    path.write_text(json.dumps({"pid": pid, "created": None, "label": "other lane", "argv": ["run_smoke.py"]}), encoding="utf-8")


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


def test_times_out(lease):
    t = iter([0.0, 1.0, 2.0, 99.0, 100.0, 101.0, 102.0])
    with pytest.raises(host_lease.HostLeaseBusy, match="gave up"):
        host_lease.acquire("t", timeout_s=10, scan=lambda: [(1, "EditRel.exe")], sleep=lambda s: None,
                           clock=lambda: next(t), log=lambda m: None)
    assert not lease.exists()


def test_reentry_does_not_see_its_own_games_as_foreign(lease, monkeypatch):
    monkeypatch.delenv("MC2_CURSOR_GUARD_OFF", raising=False)
    monkeypatch.setattr(host_lease.win_input, "guard_off", lambda: False)
    host_lease.acquire("t", scan=lambda: [], log=lambda m: None)
    monkeypatch.setattr(host_lease.win_input, "find_foreign_game_pids", lambda **k: [(1, "mc2.exe")])
    host_lease.ensure_held("again")        # must return without scanning or waiting
    assert host_lease.holds()
