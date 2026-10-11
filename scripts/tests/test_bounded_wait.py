"""smoke_lib.bounded_wait and its users: a busy resource is waited for with a countdown, then given up on (no real sleeping)."""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
from scripts.smoke_lib import bounded_wait, deploy_lease  # noqa: E402


class FakeTime:
    def __init__(self):
        self.now = 0.0

    def clock(self):
        return self.now

    def sleep(self, s):
        self.now += s


def test_ready_at_once_logs_nothing():
    t, lines = FakeTime(), []
    assert bounded_wait.wait_until(lambda: "", "x", 60, log=lines.append, sleep=t.sleep, clock=t.clock)
    assert lines == [] and t.now == 0


def test_waits_then_succeeds_and_reports_with_a_countdown():
    t, lines = FakeTime(), []
    blockers = iter(["a run holds it"] * 40 + [""])
    assert bounded_wait.wait_until(lambda: next(blockers), "the thing", 300, poll_s=2.0, report_s=30.0, log=lines.append, sleep=t.sleep, clock=t.clock)
    assert 2 <= len(lines) <= 4
    assert "the thing" in lines[0] and "a run holds it" in lines[0] and "giving up in 300 s" in lines[0]
    assert "giving up in" in lines[1] and "giving up in 300 s" not in lines[1]


def test_gives_up_at_the_bound_and_never_sleeps_past_it():
    t, lines = FakeTime(), []
    assert not bounded_wait.wait_until(lambda: "busy", "x", 10, poll_s=4.0, log=lines.append, sleep=t.sleep, clock=t.clock)
    assert t.now == pytest.approx(10.0)


def test_zero_bound_is_one_check():
    calls = []
    assert not bounded_wait.wait_until(lambda: calls.append(1) or "busy", "x", 0, log=lambda m: None)
    assert len(calls) == 1


def test_folder_lease_waits_for_a_live_holder_then_takes_it(monkeypatch):
    attempts = []

    def fake_acquire(folder, intended_test):
        attempts.append(folder)
        if len(attempts) < 3:
            raise deploy_lease.LeaseError("folder busy (owner pid 1)")
        return folder

    monkeypatch.setattr(deploy_lease, "_acquire_locked", fake_acquire)
    monkeypatch.setattr(deploy_lease, "_registry_lock", lambda: __import__("contextlib").nullcontext())
    monkeypatch.setattr(bounded_wait.time, "sleep", lambda s: None)
    deploy_lease._acquire_waiting("F", None, "deploy folder 0.4", 600)
    assert len(attempts) == 3


def test_folder_lease_busy_after_the_bound_raises_the_lease_error(monkeypatch):
    monkeypatch.setattr(deploy_lease, "_acquire_locked", lambda f, t: (_ for _ in ()).throw(deploy_lease.LeaseError("folder busy (owner pid 1)")))
    monkeypatch.setattr(deploy_lease, "_registry_lock", lambda: __import__("contextlib").nullcontext())
    with pytest.raises(deploy_lease.LeaseError, match="busy"):
        deploy_lease._acquire_waiting("F", None, "deploy folder 0.4", 0)


def test_deploy_waits_for_a_game_running_from_the_target_folder(monkeypatch):
    import deploy_payload as dp
    answers = iter(["target exe is running from THIS deploy folder:\n  PID 5: x\n"] * 3 + [""] * 5)
    monkeypatch.setattr(dp, "_check_running_processes", lambda *a: next(answers))
    monkeypatch.setattr(dp, "is_file_locked_windows", lambda p: False)
    monkeypatch.setattr(bounded_wait.time, "sleep", lambda s: None)
    dp.check_locks("A:/nowhere", "mc2.exe", wait_s=600)          # returns instead of failing


def test_deploy_without_a_wait_still_refuses(monkeypatch):
    import deploy_payload as dp
    monkeypatch.setattr(dp, "_check_running_processes", lambda *a: "target exe is running from THIS deploy folder:\n  PID 5: x\n")
    with pytest.raises(SystemExit):
        dp.check_locks("A:/nowhere", "mc2.exe", wait_s=0)
