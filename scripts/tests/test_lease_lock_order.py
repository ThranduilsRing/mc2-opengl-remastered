"""Lock order SLOT -> FOLDER (smoke_lib/host_lease.py LOCK ORDER): the W114 inversion with two real processes, and its fixes.

W114: a headless det run (pid 41032) took the candidate install's folder lease and then waited for a host slot in nofocus;
tier1 (pid 33244) held a slot and waited for that folder. Only the 3600 s bounds broke it. No game is launched here.
"""
import os
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts import console_child  # noqa: E402
from scripts.smoke_lib import deploy_lease, host_lease, nofocus  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
QUIET = lambda m: None  # noqa: E731


@pytest.fixture()
def leases(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "1")
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "1")
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.setattr(host_lease, "_registered", False)
    monkeypatch.setattr(deploy_lease, "LEASE_REGISTRY_PATH", tmp_path / "deploy.json")
    install = tmp_path / "install"
    install.mkdir()
    yield str(install)
    deploy_lease.release_lease(str(install))
    host_lease.release()


def test_a_slot_after_a_folder_lease_is_refused(leases):
    deploy_lease.acquire_lease(leases, "missions:mc2_03")
    with pytest.raises(host_lease.LockOrderError, match="holds the deploy-folder lease"):
        host_lease.acquire("nofocus det", scan=lambda: [], log=QUIET)
    assert not host_lease.holds()
    deploy_lease.release_lease(leases)
    host_lease.acquire("nofocus det", scan=lambda: [], log=QUIET)
    assert host_lease.holds()


def test_slot_then_folder_is_the_allowed_order_and_a_held_slot_stays_reentrant(leases):
    host_lease.acquire("run_smoke tier1", scan=lambda: [], log=QUIET)
    deploy_lease.acquire_lease(leases, "tier:tier1")
    host_lease.ensure_held("nofocus run_smoke.py")      # the engine launch under the run's own slot
    assert host_lease.holds() and deploy_lease.held_by_pid(os.getpid())


def test_nofocus_takes_no_slot_for_a_headless_engine(monkeypatch):
    calls = []
    monkeypatch.setattr(host_lease, "ensure_held", calls.append)
    nofocus._hold_host_lease(["A:/x/mc2.exe", "--ticks", "600", "--headless"])
    assert calls == []
    nofocus._hold_host_lease(["A:/x/mc2.exe", "--ticks", "600"])
    assert len(calls) == 1


_CHILD = r'''
import os, sys, time
sys.path.insert(0, os.getcwd())
from scripts.smoke_lib import deploy_lease, host_lease
role, folder, flags, mode = sys.argv[1:5]
quiet = lambda m: None
def mark(name): open(os.path.join(flags, name), "w").close()
def wait_for(name):
    while not os.path.exists(os.path.join(flags, name)): time.sleep(0.05)
if role == "tier1":                       # holds a slot, then waits for the install folder
    host_lease.acquire("run_smoke tier1", scan=lambda: [], log=quiet)
    mark("tier1.slot"); wait_for("det.folder")
    deploy_lease.BLOCKED_HOLDER_S = float(mode)
    t0 = time.monotonic()
    try:
        deploy_lease.acquire_lease_waiting(folder, "tier:tier1", 4.0 if float(mode) > 100 else 60.0)
        print("FOLDER_OK", round(time.monotonic() - t0, 1), flush=True)
        deploy_lease.release_lease(folder)
    except deploy_lease.LeaseDeadlock as exc:
        print("DEADLOCK", round(time.monotonic() - t0, 1), exc, flush=True)
    except deploy_lease.LeaseError as exc:
        print("FOLDER_TIMEOUT", round(time.monotonic() - t0, 1), exc, flush=True)
    host_lease.release()
else:                                     # det: took the folder first, then asks for a slot
    wait_for("tier1.slot")
    deploy_lease.acquire_lease(folder, "missions:mc2_03")
    mark("det.folder")
    if mode == "old":
        host_lease._folders_held_here = lambda: []   # a launcher without the order check (the pre-fix code)
    try:
        host_lease.acquire("nofocus det", timeout_s=6.0, scan=lambda: [], log=quiet)
        print("SLOT_OK", flush=True)
    except host_lease.LockOrderError as exc:
        print("LOCKORDER", exc, flush=True)
    except host_lease.HostLeaseBusy as exc:
        print("SLOT_TIMEOUT", exc, flush=True)
    finally:
        deploy_lease.release_lease(folder)
        host_lease.release()
'''


def _pair(tmp_path, det_mode, tier1_blocked_s):
    """tier1 takes the only slot, det takes the folder; returns (tier1 output, det output, det pid)."""
    install, flags = tmp_path / "install", tmp_path / "flags"
    install.mkdir()
    flags.mkdir()
    env = dict(os.environ, MC2_HOST_LEASE_PATH=str(tmp_path / "lease.json"), MC2_HOST_GAME_SLOTS="1",
               MC2_HOST_CORES_PER_SLOT="1", MC2_DEPLOY_LEASE_PATH=str(tmp_path / "deploy.json"), MC2_IN_GATE="1")
    def child(role, mode):
        return subprocess.Popen([sys.executable, "-c", _CHILD, role, str(install), str(flags), mode], cwd=ROOT, env=env,
                                **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    tier1 = child("tier1", str(tier1_blocked_s))
    det = child("det", det_mode)
    def result(out):                                    # the outcome line, past the [wait] countdown lines
        return next((ln for ln in out.splitlines() if ln.split()[:1] and ln.split()[0].isupper()), out)
    try:
        return result(tier1.communicate(timeout=60)[0]), result(det.communicate(timeout=60)[0]), det.pid
    finally:
        for proc in (tier1, det):
            if proc.poll() is None:
                proc.kill()


def test_w114_inversion_reproduced_without_the_fix_only_the_bounds_break_it(tmp_path):
    tier1, det, _ = _pair(tmp_path, "old", 1000)
    assert tier1.startswith("FOLDER_TIMEOUT") and float(tier1.split()[1]) >= 4.0, tier1
    assert det.startswith("SLOT_OK"), det               # it got the slot only after tier1 gave up


def test_fix_order_check_refuses_the_inverted_launch_and_tier1_goes_ahead(tmp_path):
    tier1, det, _ = _pair(tmp_path, "new", 1000)
    assert det.startswith("LOCKORDER"), det
    assert tier1.startswith("FOLDER_OK") and float(tier1.split()[1]) < 4.0, tier1


def test_fix_gate_folder_wait_on_a_blocked_holder_fails_fast_and_names_it(tmp_path):
    tier1, det, det_pid = _pair(tmp_path, "old", 0.5)
    assert tier1.startswith("DEADLOCK") and float(tier1.split()[1]) < 10.0, tier1
    assert f"holder pid {det_pid}" in tier1 and "holds a host slot" in tier1, tier1


@pytest.mark.skipif(sys.platform != "win32", reason="pid reuse check reads the Windows process start time")
def test_a_dead_holders_lease_under_a_reused_pid_is_not_ours(leases):
    """W122a determinism-mc2_24: a dead mod_smoke left a lease naming pid 40736; a later run_smoke got that pid and
    its slot request tripped LockOrderError on the stale folder."""
    entry = deploy_lease._owner_record(leases, "2020-01-01T00:00:00Z", "missions:a_0_1")   # written before we started
    reg = {deploy_lease._folder_key(leases): entry}
    deploy_lease._write_registry(reg)
    assert deploy_lease.held_by_pid(os.getpid()) == []
    host_lease.acquire("run_smoke reused pid", scan=lambda: [], log=QUIET)
    assert host_lease.holds()
    deploy_lease.release_lease(leases)
    deploy_lease.acquire_lease(leases, "missions:real")                 # a genuine lease is still seen
    assert deploy_lease.held_by_pid(os.getpid())
