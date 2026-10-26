"""Batch lease: `hold --tag X` keeps the host lease across a chain of runner processes, which enter it with MC2_HOST_LEASE_BATCH=X;
other processes still wait; the lease ends on release, at the TTL, and when the holder pid dies. Private lease file, no engine."""
import json
import os
import signal
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts import console_child  # noqa: E402
from scripts.smoke_lib import child_registry, host_lease  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


@pytest.fixture()
def lease(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "1")
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.delenv(host_lease.BATCH_ENV, raising=False)
    monkeypatch.setattr(host_lease, "_registered", False)
    original_spawn = subprocess.Popen
    original_hold = host_lease.hold
    stderr_path = tmp_path / "holder.stderr"

    def spawn(argv, *args, **kwargs):
        if isinstance(argv, list) and "_holder" in argv:
            with stderr_path.open("w", encoding="utf-8") as stderr:
                kwargs["stderr"] = stderr
                return original_spawn(argv, *args, **kwargs)
        return original_spawn(argv, *args, **kwargs)

    def hold(*args, **kwargs):
        kwargs["timeout_s"] = 60
        try:
            return original_hold(*args, **kwargs)
        except host_lease.HostLeaseBusy as exc:
            pytest.fail(f"{exc}; child stderr: {stderr_path.read_text(encoding='utf-8')}")

    monkeypatch.setattr(subprocess, "Popen", spawn)
    monkeypatch.setattr(host_lease, "hold", hold)
    yield tmp_path / "lease.json"
    host_lease.release()


def _try_child(tag=None):
    """A separate runner-like process asking for the lease without waiting. -> its exit code (0 held, 3 busy)."""
    code = ("import sys; sys.path.insert(0, r'%s'); from scripts.smoke_lib import host_lease as h\n"
            "try:\n    h.acquire('child', wait=False, log=lambda m: None)\nexcept h.HostLeaseBusy:\n    sys.exit(3)\n"
            "sys.exit(0 if h.holds() else 5)\n") % ROOT
    env = dict(os.environ)
    env.pop(host_lease.BATCH_ENV, None)
    if tag:
        env[host_lease.BATCH_ENV] = tag
    try:
        result = subprocess.run([sys.executable, "-c", code], env=env, timeout=60, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired as exc:
        pytest.fail(f"child timed out; child stderr: {exc.stderr!r}")
    assert result.returncode in (0, 3), result.stderr
    return result.returncode


def _wait_gone(pid, s=60.0):
    end = time.monotonic() + s
    while time.monotonic() < end and child_registry.pid_alive(pid):
        time.sleep(0.2)
    return not child_registry.pid_alive(pid)


@pytest.mark.parametrize("exclusive", [False, True])
def test_hold_chain_enters_others_wait_release_ends(lease, exclusive):
    pid = host_lease.hold("proof-a", ttl_s=120, timeout_s=20, log=lambda m: None, exclusive=exclusive)
    try:
        rec = json.loads(lease.read_text())
        assert rec["pid"] == pid and rec["batch"] == "proof-a" and pid != os.getpid()
        deadline = time.monotonic() + 60
        while sum(bool(r and r.get("batch") == "proof-a") for _, r in host_lease._records()) != (host_lease.slot_count() if exclusive else 1):
            assert child_registry.pid_alive(pid) and time.monotonic() < deadline, (
                f"batch publication timed out; child stderr: {lease.with_name('holder.stderr').read_text(encoding='utf-8')}")
            time.sleep(0.05)
        assert _try_child("proof-a") == 0
        assert _try_child("proof-a") == 0
        assert _try_child() == 3
        assert _try_child("other") == 3
    finally:
        removed, _ = host_lease.release_batch("proof-a")
    assert removed and not lease.exists()
    assert not any(r for _, r in host_lease._records())
    assert _wait_gone(pid), f"holder exit timed out; child stderr: {lease.with_name('holder.stderr').read_text(encoding='utf-8')}"


def test_dead_holder_expires(lease):
    pid = host_lease.hold("proof-b", ttl_s=120, timeout_s=20, log=lambda m: None)
    os.kill(pid, signal.SIGTERM)
    assert _wait_gone(pid)
    assert _try_child() == 0                         # the usual dead-pid takeover


def test_ttl_ends_the_batch(lease):
    pid = host_lease.hold("proof-c", ttl_s=1, timeout_s=20, log=lambda m: None)
    assert _wait_gone(pid, 20)
    assert not lease.exists()


def test_release_leaves_other_leases_alone(lease):
    lease.write_text(json.dumps({"pid": os.getpid(), "created": None, "label": "mine"}), encoding="utf-8")
    assert host_lease.release_batch("x") == (False, json.loads(lease.read_text()))
    assert lease.exists()


def test_bad_tag_and_cli_help(lease):
    with pytest.raises(ValueError):
        host_lease.hold("bad tag", log=lambda m: None)
    assert host_lease.main(["release", "--tag", "nothing"]) == 1


def test_holder_loop_stops_when_lease_taken_away(lease):
    sleeps = []

    def sleep(s):
        sleeps.append(s)
        lease.unlink()                               # someone ran release
    assert host_lease.holder_loop("t", 60, log=lambda m: None, sleep=sleep) == 0
    assert len(sleeps) == 1
