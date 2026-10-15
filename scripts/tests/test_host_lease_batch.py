"""Batch lease: `hold --tag X` keeps the host lease across a chain of runner processes, which enter it with MC2_HOST_LEASE_BATCH=X;
other processes still wait; the lease ends on release, at the TTL, and when the holder pid dies. Private lease file, no engine."""
import json
import os
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
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.delenv(host_lease.BATCH_ENV, raising=False)
    monkeypatch.setattr(host_lease, "_registered", False)
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
    return subprocess.run([sys.executable, "-c", code], env=env, timeout=30, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          **console_child.child_kwargs(redirected=True)).returncode


def _wait_gone(pid, s=15.0):
    end = time.time() + s
    while time.time() < end and child_registry.pid_alive(pid):
        time.sleep(0.2)
    return not child_registry.pid_alive(pid)


def test_hold_chain_enters_others_wait_release_ends(lease):
    pid = host_lease.hold("proof-a", ttl_s=120, timeout_s=20, log=lambda m: None)
    try:
        rec = json.loads(lease.read_text())
        assert rec["pid"] == pid and rec["batch"] == "proof-a" and pid != os.getpid()
        assert _try_child("proof-a") == 0            # a chain step enters
        assert _try_child("proof-a") == 0            # and the next one: the lease survived the first step's exit
        assert _try_child() == 3                     # another lane waits
        assert _try_child("other") == 3              # a different batch tag is another lane
    finally:
        removed, _ = host_lease.release_batch("proof-a")
    assert removed and not lease.exists()
    assert _wait_gone(pid)                           # the holder notices and exits


def test_dead_holder_expires(lease):
    pid = host_lease.hold("proof-b", ttl_s=120, timeout_s=20, log=lambda m: None)
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15,
                   **console_child.child_kwargs(redirected=True))
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
