"""Box hardening: box_health.py (PC client), box-supervisor (worker restart, reaping, GPU 3 guard), quiet-slot and the filler result checks.

Everything runs against fake /proc, lock and filler dirs (the module globals are monkeypatched); nothing touches the box.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import types

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "box"))
sys.path.insert(0, str(ROOT / "tools" / "box" / "filler"))
import box_health
import box_supervisor as sup
import quiet_slot
try:
    import fcntl  # noqa: F401  (run_job is a Linux script; on Windows a stub lets its pure helpers be tested)
except ImportError:
    sys.modules["fcntl"] = types.ModuleType("fcntl")
import run_job

BASH = shutil.which("bash") or "bash"
BOX = ROOT / "tools" / "box"


# --- PC client ------------------------------------------------------------------------------------------------------

class Done:
    def __init__(self, stdout="", stderr="", returncode=0):
        self.stdout, self.stderr, self.returncode = stdout, stderr, returncode


def runner_of(result):
    calls = []

    def run(cmd, **kw):
        calls.append((cmd, kw))
        if isinstance(result, BaseException):
            raise result
        return result
    run.calls = calls
    return run


def test_client_exit_codes(capsys):
    good = {"ok": True, "time": "t", "problems": [], "warnings": ["disk: 20 GB free"], "gpus": [{"index": 0, "util_pct": 3}],
            "disk": {"free_gb": 20, "floor_gb": 15}, "cpu": {"load": [4.0], "quota_cpus": 30.7}, "filler": {"workers": {"0": {"alive": True}}}}
    assert box_health.main([], runner=runner_of(Done(json.dumps(good)))) == box_health.RC_OK
    out = capsys.readouterr().out
    assert "[box_health] OK" in out and "warn    disk: 20 GB free" in out and "workers=1/1" in out
    bad = {**good, "ok": False, "problems": ["disk: 20 GB free < floor 9999 GB"]}
    r = runner_of(Done(json.dumps(bad), returncode=1))
    assert box_health.main(["--floor-gb", "9999"], runner=r) == box_health.RC_BAD
    assert "PROBLEM disk: 20 GB free < floor 9999 GB" in capsys.readouterr().out
    assert r.calls[0][0][-1] == "/workspace/bin/box-health --floor-gb 9999 --rows 12"
    assert box_health.main([], runner=runner_of(Done("", "ssh: connect to host x port 1: Connection refused", 255))) == box_health.RC_UNREACHABLE
    assert "Connection refused" in capsys.readouterr().out
    assert box_health.main([], runner=runner_of(subprocess.TimeoutExpired("ssh", 30))) == box_health.RC_UNREACHABLE
    assert box_health.main([], runner=runner_of(Done('{"no_ok": 1}'))) == box_health.RC_UNREACHABLE


def test_remote_health_parsers(tmp_path, monkeypatch):
    import box_health_remote as bhr
    assert bhr.max_gb("12.0 GB") == 12.0 and bhr.max_gb("2.0 TiB") == 2048 and bhr.max_gb("") is None and bhr.max_gb(None) is None
    monkeypatch.setattr(sup, "FILLER", str(tmp_path))
    assert bhr.filler_rows(5) == {}                      # no current_sha yet
    (tmp_path / "current_sha").write_text("abc\n")
    rd = tmp_path / "results" / "abc"
    (rd / "failed").mkdir(parents=True)
    (rd / "mc2_10-wolfman-2-1.json").write_text("{}")
    (rd / "failed" / "mc2_01-cross_map_view-1-1.log").write_text("")
    assert bhr.filler_rows(5) == {"ok": ["mc2_10-wolfman-2-1"], "failed": ["mc2_01-cross_map_view-1-1"]}


def test_client_ssh_fails_fast():
    cmd = box_health.ssh_cmd("x")
    s = " ".join(cmd)
    for opt in ("BatchMode=yes", "ConnectTimeout=5", "ServerAliveInterval=5", "ServerAliveCountMax=2"):
        assert opt in s
    assert cmd[1] == "-n" and cmd[-1] == "x"


# --- fake box ------------------------------------------------------------------------------------------------------

@pytest.fixture
def box(tmp_path, monkeypatch):
    d = types.SimpleNamespace(proc=tmp_path / "proc", locks=tmp_path / "locks", filler=tmp_path / "filler", log=tmp_path / "sup.log")
    for p in (d.proc, d.locks, d.filler):
        p.mkdir()
    monkeypatch.setattr(sup, "PROC", str(d.proc))
    monkeypatch.setattr(sup, "LOCKS", str(d.locks))
    monkeypatch.setattr(sup, "FILLER", str(d.filler))
    monkeypatch.setattr(sup, "LOG", str(d.log))
    monkeypatch.setattr(sup, "signal", types.SimpleNamespace(SIGSTOP=19, SIGCONT=18))   # Windows has neither

    def proc(pid, cmd, parent=1, start="777"):
        p = d.proc / str(pid)
        p.mkdir(exist_ok=True)
        (p / "cmdline").write_bytes(cmd.replace(" ", "\0").encode() + b"\0")
        (p / "stat").write_text(f"{pid} (x) " + " ".join(["S", str(parent)] + ["0"] * 17 + [start]) + "\n")
    d.add = proc
    return d


def aged(path, s):
    t = time.time() - s
    os.utime(path, (t, t))


def test_worker_restart_and_backoff(box):
    (box.filler / "worker.sh").write_text("#!/bin/bash\n")
    started = []

    def start(g):
        started.append(g)
        return 9000 + g
    state = sup.load_state()
    box.add(500, "bash /workspace/filler/worker.sh 0")
    (box.filler / "worker0.pid").write_text("500")
    (box.filler / "worker1.pid").write_text("12345")    # dead
    out = sup.check_workers(state, 1000.0, start=start)
    assert out == {"0": "alive", "1": "restarted", "2": "restarted"} and started == [1, 2]
    assert "worker1 dead (was pid 12345): restarted pid 9001" in box.log.read_text()
    for i in range(sup.FLAP_N):   # 1 + FLAP_N restarts inside the window: backoff, no more starts
        sup.check_workers(state, 1001.0 + i, start=start)
    started.clear()
    assert sup.check_workers(state, 1100.0, start=start)["1"] == "backoff" and 1 not in started
    assert "worker1 BACKOFF" in box.log.read_text()
    assert sup.check_workers(state, 1000.0 + sup.FLAP_WINDOW_S + 50, start=start)["1"] == "restarted"   # the window cleared
    (box.filler / "STOP").write_text("")
    assert set(sup.check_workers(state, 5000.0, start=start).values()) == {"stopped"}


def test_reap_only_dead_and_stale(box):
    box.add(4242, "bash /workspace/lanes/.box_lane_x.sh")
    L = box.locks
    (L / "gpu0").write_text("")
    (L / "gpu0.owner").write_text("99999 claude/x abcdef12 2026-11-05T00:00:00Z\n"); aged(L / "gpu0.owner", 300)
    (L / "gpu0.row").write_text("gpu:mc2 row\n"); aged(L / "gpu0.row", 300)
    (L / "gpu1.owner").write_text("4242 claude/y 12345678 t\n"); aged(L / "gpu1.owner", 300)   # live holder
    (L / "gpu1.row").write_text("gpu:live\n"); aged(L / "gpu1.row", 300)
    (L / "gpu2.owner").write_text("99998 claude/z 12345678 t\n")                               # dead but fresh: flock/owner race window
    (L / "gpu2.want.x").write_text("")
    (L / "old.want").write_text(""); aged(L / "old.want", sup.STALE_WANT_S + 60)
    (L / "new.want").write_text("")
    (L / "quiet").write_text("99997 1 t\n")
    (L / "gpu3").write_text("")
    (L / "gpu3.owner").write_text("99996 queue t\n"); aged(L / "gpu3.owner", 300)              # dead writer, but the lock is held
    gone = {Path(p).name for p in sup.reap(time.time(), holders={os.stat(L / "gpu3").st_ino: 1})}
    assert gone == {"gpu0.owner", "gpu0.row", "old.want", "quiet"} and (L / "gpu3.owner").exists()
    assert (L / "gpu0").exists() and (L / "gpu1.owner").exists() and (L / "gpu1.row").exists() and (L / "gpu2.owner").exists()
    assert (L / "new.want").exists()
    assert "reaped " in box.log.read_text() and "owner pid dead" in box.log.read_text()


def test_gpu3_guard_stops_intruders_and_fails_closed(box):
    (box.locks / "gpu3").write_text("")
    ino = os.stat(box.locks / "gpu3").st_ino
    box.add(100, "bash /workspace/lanes/.box_lane_claude_x.sh")
    box.add(200, "./mc2 --gate", parent=100)       # the gate's own game
    box.add(300, "python3 /workspace/filler/run_job.py 3", start="888")   # not under the gate
    (box.locks / "gpu3.owner").write_text("100 claude/x abcdef12 t\n")
    buses = {0: "A", 1: "B", 2: "C", 3: "D"}
    apps = [("D", 200, "mc2", "900"), ("D", 300, "mc2", "800"), ("A", 400, "mc2", "700")]
    sent = []

    def kill(pid, sig):
        sent.append((pid, sig))
    state = sup.load_state()
    problems = sup.guard_gpu3(state, 0.0, query=lambda: (buses, apps), holders={ino: 7}, kill=kill)
    assert problems == [] and sent == [(300, 19)] and state["gpu3"]["gate_active"]
    assert (box.locks / "gpu3.stopped").read_text().split() == ["300", "888", "mc2"]
    sup.guard_gpu3(state, 30.0, query=lambda: (buses, apps), holders={ino: 7}, kill=kill)
    assert sent == [(300, 19)]                    # stopped once
    sup.guard_gpu3(state, 60.0, query=lambda: (buses, apps), holders={}, kill=kill)   # gate ended
    assert sent[-1] == (300, 18) and not (box.locks / "gpu3.stopped").exists()
    assert "SIGSTOP 300" in box.log.read_text() and "SIGCONT 300" in box.log.read_text()
    assert sup.guard_gpu3(state, 90.0, query=lambda: (None, None), holders={}, kill=kill)[0].startswith("nvidia-smi failed")
    box.add(101, "sleep 999")                      # a gpu3 lock held by something that is not a box_run lane script
    (box.locks / "gpu3.owner").write_text("101 x y t\n")
    p = sup.guard_gpu3(state, 120.0, query=lambda: (buses, []), holders={ino: 7}, kill=kill)
    assert p and "not a box_run gate script" in p[0]


def test_env_sh_refuses_gpu3_without_gate():
    txt = (BOX / "env.sh").read_text()
    assert "GPU 3 is reserved for gate rows" in txt and "gpu3.owner" in txt
    w = (BOX / "filler" / "worker.sh").read_text()
    assert "case \"$G\" in 0|1|2)" in w      # filler never runs on GPU 3


# --- quiet slot and the filler result checks -------------------------------------------------------------------------

def test_quiet_slot_claim_drain_release(box):
    assert quiet_slot.parse_cpus("0-3,8, 10-11") == {0, 1, 2, 3, 8, 10, 11}
    (box.locks / "cores.env").write_text("BUILD_CPUS=32-63\nGPU1_CPUS=8-15\n")
    assert quiet_slot.gpu_cpus(1) == "8-15" and quiet_slot.gpu_cpus(2) == "16-23"
    assert quiet_slot.claim(1, 0)
    assert (box.locks / "quiet").read_text().split()[:2] == [str(os.getpid()), "1"]
    (box.filler / "active.2").write_text("")
    clock = iter([0.0, 5.0, 10.0, 15.0])
    ok, waited = quiet_slot.drain(10, now=lambda: next(clock), nap=lambda s: None)
    assert not ok and waited == 10.0
    (box.filler / "active.2").unlink()
    assert quiet_slot.drain(10, nap=lambda s: None)[0]
    quiet_slot.release()
    assert not (box.locks / "quiet").exists()


def test_quiet_slot_live_and_device_verdict(tmp_path):
    q = tmp_path / "quiet"
    assert not run_job.quiet_slot_live(str(q))
    q.write_text("1 2 t\n")
    assert run_job.quiet_slot_live(str(q)) == os.path.isdir("/proc/1")
    good = '[VK_ENGINE_CONTEXT] engine Vulkan device created: device_name="NVIDIA GeForce RTX 3060" api=1.4'
    assert run_job.device_verdict(good) == ("NVIDIA GeForce RTX 3060", None)
    name, why = run_job.device_verdict(good.replace("NVIDIA GeForce RTX 3060", "llvmpipe (LLVM 19.1.1, 256 bits)"))
    assert why.startswith("device=cpu-fallback") and name.startswith("llvmpipe")
    assert run_job.device_verdict(good.replace("RTX 3060", "RTX 4090"))[1].startswith("device=wrong")
    assert run_job.device_verdict("no device here")[1].startswith("device=missing")


# --- the box scripts ------------------------------------------------------------------------------------------------

def test_box_scripts_parse_and_are_strict():
    # literal argv (the callers index counts these as the scripts' callers); executable= is Git bash, not the WSL launcher
    r = [subprocess.run(["bash", "-n", "tools/box/filler/worker.sh"], cwd=ROOT, executable=BASH, capture_output=True, text=True),
         subprocess.run(["bash", "-n", "tools/box/filler/run.sh"], cwd=ROOT, executable=BASH, capture_output=True, text=True),
         subprocess.run(["bash", "-n", "tools/box/filler/stop.sh"], cwd=ROOT, executable=BASH, capture_output=True, text=True),
         subprocess.run(["bash", "-n", "tools/box/deploy.sh"], cwd=ROOT, executable=BASH, capture_output=True, text=True)]
    assert [x.returncode for x in r] == [0, 0, 0, 0], [x.stderr for x in r]
    for f in ("filler/run.sh", "filler/stop.sh", "deploy.sh", "filler/sync.sh"):
        assert "set -euo pipefail" in (BOX / f).read_text(), f
    for f in BOX.rglob("*"):
        if f.is_file():
            assert b"\r" not in f.read_bytes(), f"CRLF in {f}"
    assert "tools/box/** text eol=lf" in (ROOT / ".gitattributes").read_text()


def test_deploy_map_sources_exist_and_atomic():
    txt = (BOX / "deploy.sh").read_text()
    m = txt.split('MAP="', 1)[1].split('"', 1)[0].split()
    srcs = [ln[2:].split("=", 1)[0] for ln in m if ln[:2] in ("L:", "C:")]
    assert len(srcs) >= 15 and all((BOX / s).is_file() for s in srcs), [s for s in srcs if not (BOX / s).is_file()]
    assert "mv -Tf" in txt and 'mv -T "$tmp" "$REL/$TS"' in txt          # staged release, one rename
    assert "toolchain)" in txt and "--revert" in txt
    assert "/etc/environment" not in txt
    for f in ("box_health_remote.py", "box_supervisor.py"):
        t = (BOX / f).read_text()
        assert "/etc/environment" not in t.replace("Never reads the process environment or /etc/environment", "")
        assert "environ" not in t.replace("os.environ.get(", "").replace("Never reads the process environment or /etc/environment", "")
    assert "nifty.bundle.tmp" in (ROOT / "tools" / "box_filler_push.py").read_text()
