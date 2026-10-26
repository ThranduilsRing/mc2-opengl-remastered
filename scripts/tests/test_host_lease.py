"""smoke_lib.host_lease: one lane's engine at a time, host-wide (no real game; the two-lane proof runs real child processes from two
worktree copies against one shared lease file)."""
import json
import os
import shutil
import subprocess
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts import console_child  # noqa: E402
from scripts.smoke_lib import host_lease, nofocus  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


@pytest.fixture()
def lease(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "1")
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "1")
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


@pytest.mark.skipif(sys.platform != "win32", reason="Windows open readers deny replace and unlink")
@pytest.mark.parametrize("operation", ["replace", "unlink"])
def test_publication_and_release_survive_an_open_reader(lease, operation):
    host_lease.acquire("publication")
    opened = threading.Event()

    def reader():
        with lease.open("rb") as stream:
            stream.read()
            opened.set()
            time.sleep(0.05)

    thread = threading.Thread(target=reader)
    thread.start()
    try:
        assert opened.wait(60), "reader never opened the lease"
        if operation == "replace":
            rec = dict(host_lease._read(lease), batch="publication")
            host_lease._write_record(lease, rec)
            assert host_lease._read(lease) == rec
        else:
            host_lease.release()
            assert not lease.exists()
    finally:
        thread.join(timeout=60)
    assert not thread.is_alive()


@pytest.mark.parametrize("exclusive", [False, True])
def test_a_live_holder_blocks_with_no_wait(lease, monkeypatch, exclusive):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "3")
    _foreign_holder(lease, os.getppid())
    with pytest.raises(host_lease.HostLeaseBusy, match="held by other lane"):
        host_lease.acquire("t", wait=False, scan=lambda: [], log=lambda m: None, exclusive=exclusive)
    assert not any(host_lease.slot_path(i).exists() for i in range(1, 6))


def test_exclusive_retries_legacy_release_and_partial_allocation(lease, monkeypatch):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "3")
    _foreign_holder(lease, os.getppid())
    original = host_lease._try_take
    attempts, sleeps = [], []

    def take(path, label):
        attempts.append(path)
        if attempts == [lease]:
            _foreign_holder(lease, os.getppid())
        elif len(attempts) == 3:
            _foreign_holder(path, os.getppid())
        return original(path, label)

    def release_holder(_):
        assert not any(r and r.get("pid") == os.getpid() for _, r in host_lease._records())
        for path, rec in host_lease._records():
            if rec and rec.get("pid") == os.getppid():
                path.unlink()
        sleeps.append(True)

    monkeypatch.setattr(host_lease, "_try_take", take)
    host_lease.acquire("exclusive", exclusive=True, sleep=release_holder, timeout_s=10, log=lambda m: None)
    assert len(sleeps) == 3
    assert len([r for _, r in host_lease._records() if r and r.get("pid") == os.getpid()]) == 3


def test_lease_publication_never_exposes_empty_record(lease, monkeypatch):
    original = os.link
    published = []

    def publish(source, target):
        published.append(host_lease._read(source))
        return original(source, target)

    monkeypatch.setattr(os, "link", publish)
    host_lease.acquire("complete record", wait=False)
    assert len(published) == 1 and published[0]["pid"] == os.getpid()


def test_legacy_acquirer_waits_after_slot_zero_releases(lease, monkeypatch):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "2")
    host_lease.acquire("slot zero")
    neighbor = dict(host_lease._read(lease), pid=os.getppid(), created=None, slot=1)
    host_lease._write_record(host_lease.slot_path(1), neighbor)
    host_lease.release()
    assert host_lease._read(lease)["legacy_guard"]
    assert not host_lease._records()[0][1]
    assert host_lease._holder_alive(host_lease._read(lease))
    assert not host_lease._try_take(lease, "legacy single-file acquirer")[0]
    host_lease.acquire("reuse slot zero", wait=False)
    assert host_lease._read(lease)["slot"] == 0 and not host_lease._read(lease).get("legacy_guard")
    host_lease.release()
    host_lease._remove_records([(host_lease.slot_path(1), neighbor)])
    assert not lease.exists()
    host_lease._write_record(host_lease.slot_path(1), neighbor)
    host_lease._remove_records([])
    assert host_lease._read(lease)["legacy_guard"]
    assert not host_lease._try_take(lease, "legacy single-file acquirer")[0]


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


def _wait_until(condition, description, proc=None, timeout_s=60.0):
    deadline = time.monotonic() + timeout_s
    while not condition():
        if time.monotonic() >= deadline or (proc is not None and proc.poll() is not None):
            stderr = b""
            if proc is not None:
                if proc.poll() is None:
                    proc.kill()
                _, stderr = proc.communicate(timeout=10)
            pytest.fail(f"timed out waiting for {description}; child stderr: {stderr!r}")
        time.sleep(0.05)


def _wait_for(path, timeout_s=60.0, proc=None):
    _wait_until(lambda: os.path.exists(path), str(path), proc, timeout_s)


def test_two_lanes_from_two_worktree_copies_share_one_lease(tmp_path, monkeypatch):
    """Lane B, run from its own worktree copy, must queue behind lane A's lease and print the countdown; it gets the lease only after
    A releases. The lease file is one shared path, not something under either worktree."""
    lease_file = tmp_path / "host" / "lease.json"
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "1")
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
            deadline = time.monotonic() + 20
            while not (tmp_path / "b.log").stat().st_size:
                assert b.poll() is None, "lane B exited before waiting for the lease"
                assert time.monotonic() < deadline, "lane B never reported lease contention"
                time.sleep(0.05)
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


@pytest.mark.parametrize("exclusive", [False, True])
def test_fifo_exclusive_barrier_and_disjoint_slots(lease, monkeypatch, tmp_path, exclusive):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "4")
    host_lease.acquire("initial")
    script = """
import json, sys, time
from pathlib import Path
from scripts.smoke_lib import host_lease as h
h.POLL_S = 0.03
tag, root, exclusive = sys.argv[1:]
h.acquire(tag, exclusive=exclusive == 'True', timeout_s=120, log=lambda m: None)
root = Path(root)
records = [r for _, r in h._records() if r and r['pid'] == __import__('os').getpid()]
(root / (tag + '.held')).write_text(json.dumps(records))
while not (root / (tag + '.go')).exists():
    time.sleep(0.03)
h.release()
"""
    children = []

    def start(tag, exclusive=False):
        proc = subprocess.Popen([sys.executable, "-c", script, tag, str(tmp_path), str(exclusive)],
                                cwd=ROOT, env=dict(os.environ),
                                **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        children.append(proc)
        return proc

    def queued(proc):
        def present():
            tickets = (host_lease._read(lease.with_suffix(".queue.json")) or {}).get("tickets", [])
            return any(t["pid"] == proc.pid for t in tickets)
        _wait_until(present, "waiter entering queue", proc)

    def finished(proc):
        _wait_until(lambda: proc.poll() is not None, "child exit", proc)
        _, stderr = proc.communicate(timeout=10)
        assert proc.returncode == 0, stderr

    try:
        occupants = [start(str(i)) for i in range(3)]
        for i in range(3):
            _wait_for(tmp_path / f"{i}.held", proc=occupants[i])
        records = [r for _, r in host_lease._records() if r]
        assert len(records) == 4 and len({r["slot"] for r in records}) == 4
        if exclusive:
            first = start("first", True)
            queued(first)
            later = start("later")
            queued(later)
            assert not (tmp_path / "later.held").exists()
            host_lease.release()
            _wait_until(lambda: all(not r or r["pid"] != os.getpid() for _, r in host_lease._records()),
                        "initial slot release", first)
            assert not (tmp_path / "first.held").exists()
            for i, proc in enumerate(occupants):
                (tmp_path / f"{i}.go").touch()
                finished(proc)
            _wait_for(tmp_path / "first.held", proc=first)
            records = json.loads((tmp_path / "first.held").read_text())
            assert len(records) == host_lease.slot_count()
            assert not (tmp_path / "later.held").exists()
            (tmp_path / "first.go").touch()
            finished(first)
            _wait_for(tmp_path / "later.held", proc=later)
        else:
            first = start("first")
            queued(first)
            later = start("later")
            queued(later)
            host_lease.release()
            _wait_for(tmp_path / "first.held", proc=first)
            assert not (tmp_path / "later.held").exists()
            (tmp_path / "first.go").touch()
            finished(first)
            _wait_for(tmp_path / "later.held", proc=later)
    finally:
        for proc in children:
            if proc.poll() is None:
                proc.kill()
            proc.communicate(timeout=10)


def test_dead_ticket_and_holder_do_not_block_queue(lease):
    dead = {"pid": 2 ** 22 + 12345, "created": None}
    lease.write_text(json.dumps(dead))
    lease.with_suffix(".queue.json").write_text(json.dumps({"tickets": [dead]}))
    host_lease.acquire("recovered", wait=False)
    assert host_lease.holds()
    assert host_lease._read(lease.with_suffix(".queue.json"))["tickets"] == []


def test_dead_queue_lock_owner_cannot_wedge_waiters(lease, tmp_path):
    flag = tmp_path / "locked"
    script = """
import sys, time
from pathlib import Path
from scripts.smoke_lib import host_lease as h
with h._queue_lock():
    Path(sys.argv[1]).touch()
    time.sleep(20)
"""
    proc = subprocess.Popen([sys.executable, "-c", script, str(flag)], cwd=ROOT,
                            **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
    try:
        _wait_for(flag)
    finally:
        proc.kill()
        proc.communicate(timeout=10)
    host_lease.acquire("after crash", wait=False)
    assert host_lease.holds()


def test_registered_batch_engine_allows_another_slot_but_reused_pid_blocks(lease, monkeypatch):
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "2")
    holder = {"pid": os.getppid(), "created": None, "slot": 0}
    lease.write_text(json.dumps(holder))
    original = host_lease.child_registry.process_create_time
    monkeypatch.setattr(host_lease.child_registry, "process_create_time", lambda pid: 123 if pid == 4242 else original(pid))
    record = {"runner_pid": 9999, "create_time": 123, "lease_holder": {"pid": os.getppid(), "created": None}}
    monkeypatch.setattr(host_lease.child_registry, "read_record", lambda pid: record)
    host_lease.acquire("neighbor", wait=False, scan=lambda: [(4242, "mc2.exe")])
    assert host_lease._read(host_lease.slot_path(1))["pid"] == os.getpid()
    host_lease.release()
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "2")
    with pytest.raises(host_lease.HostLeaseBusy):
        host_lease.acquire("incompatible core layout", wait=False, scan=lambda: [])
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "1")
    record["create_time"] = 456
    with pytest.raises(host_lease.HostLeaseBusy, match="mc2.exe pid 4242"):
        host_lease.acquire("reused engine pid", wait=False, scan=lambda: [(4242, "mc2.exe")])


def test_affinity_reserves_only_physical_game_cores(monkeypatch):
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "1")
    assert [host_lease.affinity_mask(i) for i in range(6)] == [0x30, 0xC0, 0x300, 0xC00, 0x3000, 0xC000]
    monkeypatch.delenv("MC2_HOST_GAME_SLOTS", raising=False)
    monkeypatch.delenv("MC2_HOST_CORES_PER_SLOT", raising=False)
    assert host_lease.slot_count() == 2
    assert host_lease.cores_per_slot() == 1
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "2")
    assert [host_lease.affinity_mask(i) for i in range(3)] == [0xF0, 0xF00, 0xF000]
    with pytest.raises(ValueError):
        host_lease.affinity_mask(3)
    for invalid in ("0", "4"):
        monkeypatch.setenv("MC2_HOST_GAME_SLOTS", invalid)
        with pytest.raises(ValueError):
            host_lease.slot_count()
    monkeypatch.delenv("MC2_HOST_GAME_SLOTS", raising=False)
    for invalid in ("0", "7"):
        monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", invalid)
        with pytest.raises(ValueError):
            host_lease.slot_count()


@pytest.mark.parametrize("width,exclusive,expected", [(1, False, 0x30), (1, True, 0x30), (2, False, 0xF0), (2, True, 0xF0)])
def test_slot_affinity_reaches_child_process(lease, monkeypatch, width, exclusive, expected):
    import ctypes
    from ctypes import wintypes

    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", str(width))
    host_lease.acquire("affinity", exclusive=exclusive)
    proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"],
                            **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
    try:
        host_lease.apply_affinity(proc)
        if sys.platform == "win32":
            get_mask = ctypes.WinDLL("kernel32", use_last_error=True).GetProcessAffinityMask
            get_mask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
            get_mask.restype = wintypes.BOOL
            process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
            assert get_mask(int(proc._handle), ctypes.byref(process_mask), ctypes.byref(system_mask))
            assert process_mask.value == expected
        else:
            assert os.sched_getaffinity(proc.pid) == {i for i in range(16) if expected & (1 << i)}
    finally:
        proc.kill()
        proc.communicate(timeout=10)


def test_exclusive_window_holds_all_slots_only_inside(lease):
    host_lease.acquire("load", scan=lambda: [], log=lambda m: None)
    with host_lease.exclusive_window("measured", scan=lambda: [], log=lambda m: None):
        assert all(host_lease._ours(host_lease._read(host_lease.slot_path(i))) for i in range(host_lease.slot_count()))
    assert not any(host_lease.slot_path(i).exists() for i in range(6)) and not host_lease.holds()
