"""Two lane GPUs, one core per lane game, six cores for compiles (owner 2026-11-07): slot i = lane GPU i = game core i."""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from scripts import build_tokens, console_child  # noqa: E402
from scripts.smoke_lib import host_lease  # noqa: E402
import lint_slots  # noqa: E402

SPLIT_TEXTS = ["", "game0=0x3\ngame1=0xC\ncompile=0xFFF0\n", "game0=0x30\ngame1=0xC0\ncompile = 0xFF0F\n", "lane=0x0FF0\ngate=0xF000\n",
               "lane=0x0FF0\ncompile=0xF0F0\n", "# c\ngame0=oops\ngame1=0xC\nbogus=1\ncompile=\n"]


@pytest.fixture()
def lease(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "1")
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "2")
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    for k in ("MC2_VK_DEVICE", "MC2_HARNESS_GPU", "MC2_IN_GATE"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setattr(host_lease, "_registered", False)
    monkeypatch.setattr(host_lease, "_off_logged", False)
    yield tmp_path / "lease.json"
    host_lease.release()


_HOLDER = """
import json, os, sys, time
from pathlib import Path
from scripts.smoke_lib import host_lease as h
h.POLL_S = 0.05
tag, root = sys.argv[1:]
h.acquire(tag, timeout_s=120, log=lambda m: None)
records = [r for _, r in h._records() if r and r['pid'] == os.getpid()]
(Path(root) / (tag + '.held')).write_text(json.dumps(records))
while not (Path(root) / (tag + '.go')).exists():
    time.sleep(0.05)
h.release()
"""


def _wait(condition, what, proc=None, timeout_s=60.0):
    deadline = time.monotonic() + timeout_s
    while not condition():
        if time.monotonic() >= deadline or (proc is not None and proc.poll() is not None):
            err = b""
            if proc is not None:
                if proc.poll() is None:
                    proc.kill()
                _, err = proc.communicate(timeout=10)
            pytest.fail(f"timed out waiting for {what}; child stderr: {err!r}")
        time.sleep(0.05)


def _start(tag, root):
    return subprocess.Popen([sys.executable, "-c", _HOLDER, tag, str(root)], cwd=ROOT, env=dict(os.environ),
                            **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))


def test_two_concurrent_acquires_land_on_different_gpus_and_a_third_queues(lease, tmp_path):
    a, b, c = _start("a", tmp_path), None, None
    try:
        _wait(lambda: (tmp_path / "a.held").exists(), "a held", a)
        b = _start("b", tmp_path)
        _wait(lambda: (tmp_path / "b.held").exists(), "b held", b)
        rec_a, rec_b = json.loads((tmp_path / "a.held").read_text())[0], json.loads((tmp_path / "b.held").read_text())[0]
        assert {rec_a["device"], rec_b["device"]} == {"AMD", "NVIDIA"} and rec_a["slot"] != rec_b["slot"]
        assert not rec_a["affinity_mask"] & rec_b["affinity_mask"]
        c = _start("c", tmp_path)
        queue = lease.with_suffix(".queue.json")
        _wait(lambda: any(t["pid"] == c.pid for t in (host_lease._read(queue) or {}).get("tickets", [])), "c queued", c)
        time.sleep(0.3)
        assert not (tmp_path / "c.held").exists()                      # both GPUs are busy: the third waits
        (tmp_path / "a.go").touch()
        _wait(lambda: (tmp_path / "c.held").exists(), "c takes the freed slot", c)
        assert json.loads((tmp_path / "c.held").read_text())[0]["device"] == rec_a["device"]
        for tag in ("b", "c"):
            (tmp_path / f"{tag}.go").touch()
    finally:
        for p in (a, b, c):
            if p is not None:
                if p.poll() is None:
                    p.kill()
                p.communicate(timeout=15)


@pytest.mark.parametrize("forced,device", [("NVIDIA", "NVIDIA"), ("2070", "NVIDIA"), ("AMD", "AMD"), ("xtx", "AMD")])
def test_a_forced_device_only_takes_its_own_slot(lease, monkeypatch, forced, device):
    monkeypatch.setenv("MC2_VK_DEVICE", forced)
    assert [host_lease.slot_device(i) for i in host_lease.eligible_slots(host_lease.device_hint())] == [device]
    host_lease.acquire("forced", wait=False, scan=lambda: [], log=lambda m: None)
    assert host_lease.held_device() == device
    host_lease.release()


@pytest.mark.parametrize("mode,device", [("2070", "NVIDIA"), ("xtx", "AMD")])
def test_harness_gpu_override_forces_the_device(lease, monkeypatch, mode, device):
    monkeypatch.setenv("MC2_HARNESS_GPU", mode)
    assert host_lease.device_hint() == device
    assert [host_lease.slot_device(i) for i in host_lease.eligible_slots(host_lease.device_hint())] == [device]
    monkeypatch.setenv("MC2_HARNESS_GPU", "any")
    assert host_lease.device_hint() is None and host_lease.eligible_slots(None) == [0, 1]


def test_game_and_compile_masks_are_disjoint_and_cover_the_machine():
    m = lint_slots.CPU_SPLIT_DEFAULT
    assert m["game0"] & m["game1"] == 0 and (m["game0"] | m["game1"]) & m["compile"] == 0
    assert m["game0"] | m["game1"] | m["compile"] == 0xFFFF
    for half in (m["game0"], m["game1"]):                              # one physical core: an adjacent SMT sibling pair
        low = (half & -half).bit_length() - 1
        assert half == 0b11 << low and low % 2 == 0
    assert host_lease.CPU_SPLIT_DEFAULT == m


@pytest.mark.parametrize("text", SPLIT_TEXTS)
def test_lint_slots_and_host_lease_parse_cpu_split_the_same(text):
    assert lint_slots.parse_cpu_split(text) == host_lease.parse_cpu_split(text)


def test_cpu_split_legacy_keys_for_one_window():
    assert lint_slots.parse_cpu_split("lane=0x0FF0\ngate=0xF000\n")["compile"] == 0x0FF0     # old lane mask = compile, gate ignored
    assert lint_slots.parse_cpu_split("lane=0x0FF0\ncompile=0xF0F0\n")["compile"] == 0xF0F0  # an explicit compile line wins
    assert lint_slots.parse_cpu_split("")["compile"] == 0xFFF0


def test_ninja_pool_is_capped_by_the_compile_mask(tmp_path, monkeypatch):
    monkeypatch.setattr(build_tokens, "OPS", str(tmp_path))
    (tmp_path / "CPU_SPLIT").write_text("game0=0x3\ngame1=0xC\ncompile=0xFFF0\n", encoding="utf-8")
    (tmp_path / "BUILD_POOL").write_text("32\n", encoding="utf-8")
    monkeypatch.delenv("MC2_BUILD_POOL_FILE", raising=False)
    assert build_tokens.compile_cpu_count() == 12
    assert build_tokens.pool_size() == 12                              # a larger BUILD_POOL never exceeds the six compile cores
    (tmp_path / "BUILD_POOL").write_text("8\n", encoding="utf-8")
    assert build_tokens.pool_size() == 8
    (tmp_path / "BUILD_POOL").unlink()
    monkeypatch.setattr(build_tokens.os, "cpu_count", lambda: 64)
    assert build_tokens.pool_size() == 12
    (tmp_path / "CPU_SPLIT").write_text("compile=0xFF0\n", encoding="utf-8")                   # a narrower file narrows the pool
    assert build_tokens.pool_size() == 8


def _physical_core_masks():
    """Windows GetLogicalProcessorInformationEx(RelationProcessorCore) -> one affinity mask per physical core (group 0)."""
    import ctypes
    k32 = ctypes.windll.kernel32
    n = ctypes.c_ulong(0)
    k32.GetLogicalProcessorInformationEx(0, None, ctypes.byref(n))
    buf = ctypes.create_string_buffer(n.value)
    assert k32.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(n))
    raw, off, masks = buf.raw, 0, []
    while off < n.value:
        size = int.from_bytes(raw[off + 4:off + 8], "little")
        masks.append(int.from_bytes(raw[off + 32:off + 40], "little"))   # PROCESSOR_RELATIONSHIP.GroupMask[0].Mask
        off += size
    return masks


@pytest.mark.skipif(os.name != "nt" or (os.cpu_count() or 0) != 16, reason="the 16-CPU PC topology")
def test_game_masks_are_whole_physical_cores_per_windows_topology():
    cores = _physical_core_masks()
    assert len(cores) == 8 and sorted(cores)[0] == 0x3
    m = lint_slots.cpu_masks()
    for half in (m["game0"], m["game1"]):
        assert half in cores                                           # exactly one complete core, SMT siblings together
    assert all(not (c & m["compile"] and c & (m["game0"] | m["game1"])) for c in cores)   # no core is shared between games and compiles


_GRANDCHILD = "import ctypes;k=ctypes.windll.kernel32;p=ctypes.c_size_t();s=ctypes.c_size_t();k.GetProcessAffinityMask(ctypes.c_void_p(k.GetCurrentProcess()),ctypes.byref(p),ctypes.byref(s));print(p.value)"
_PARENT = ("import subprocess,sys,time;print('ready',flush=True);sys.stdin.readline();"
           "print(subprocess.run([sys.executable,'-c',sys.argv[1]],capture_output=True,text=True).stdout.strip(),flush=True)")


@pytest.mark.skipif(os.name != "nt", reason="Windows process affinity")
def test_a_server_repinned_by_pid_passes_the_compile_mask_to_its_later_children():
    """Stands in for sccache.exe -> cl.exe: a persistent process born unpinned is re-pinned from outside; a child it spawns afterwards
    inherits the compile mask (what lint_slots.pin_sccache_servers does to the live cache server)."""
    server = subprocess.Popen([sys.executable, "-c", _PARENT, _GRANDCHILD], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                              **console_child.child_kwargs(redirected=True))
    try:
        assert server.stdout.readline().strip() == "ready"
        applied = lint_slots.pin_pid(server.pid, "compile")
        want = lint_slots.cpu_masks()["compile"] & ((1 << (os.cpu_count() or 1)) - 1)
        assert applied == want or not want
        server.stdin.write("go\n")
        server.stdin.flush()
        assert int(server.stdout.readline().strip()) == applied and applied & 0x3 == 0   # the descendant sits off the game0 core
    finally:
        if server.poll() is None:
            server.kill()
        server.communicate(timeout=15)


def test_pin_sccache_servers_honours_the_pin_opt_out(monkeypatch):
    monkeypatch.setenv("MC2_NO_CPU_PIN", "1")
    monkeypatch.setattr(lint_slots, "sccache_server_pids", lambda: [1])
    monkeypatch.setattr(lint_slots, "pin_pid", lambda *a: pytest.fail("pinned despite MC2_NO_CPU_PIN"))
    assert lint_slots.pin_sccache_servers() == []
    monkeypatch.delenv("MC2_NO_CPU_PIN")
    monkeypatch.setattr(lint_slots, "pin_pid", lambda pid, mode: 0xFFF0)
    assert lint_slots.pin_sccache_servers() == [1]


def test_a_capture_reaps_only_its_own_children_not_another_captures(monkeypatch):
    """Landmine: a capture that reaped on start used to kill foreign games. Two captures in two processes are two _ACTIVE lists;
    the reap of one must leave the other's child alive."""
    import run_visual_capture as rvc
    sleeper = [sys.executable, "-c", "import time; time.sleep(60)"]
    kw = console_child.child_kwargs(redirected=True)
    own, foreign = subprocess.Popen(sleeper, **kw), subprocess.Popen(sleeper, **kw)
    try:
        monkeypatch.setattr(rvc, "_ACTIVE", [own])
        rvc._kill_existing_mc2()
        assert own.poll() is not None                                  # our child is reaped
        assert foreign.poll() is None                                  # the other capture's child is untouched
        assert rvc._ACTIVE == []
    finally:
        for p in (own, foreign):
            if p.poll() is None:
                p.kill()
            p.wait(timeout=15)
