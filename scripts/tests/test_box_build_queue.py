"""Box build semaphore (box_run BUILD_SEM), client tie and the box_health build-queue report. Fakes only; nothing touches the box."""
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "box"))
sys.path.insert(0, str(ROOT / "tools" / "box" / "filler"))
import box_run
import box_health
import box_health_remote as bhr
import box_supervisor as sup

BASH = shutil.which("bash") or "bash"


def run_sem(tmp_path, body, cpus="8-35", extra_env=None, timeout=60):
    script = tmp_path / "t.sh"
    script.write_text("set -u\nBR=lane-a\n" + box_run.BUILD_SEM + body, newline="\n")
    env = {"MC2_BOX_BUILDQ": str(tmp_path / "q").replace("\\", "/"), "BUILD_CPUS": cpus, "MC2_BOX_BUILDQ_POLL": "0.2", "PATH": __import__("os").environ["PATH"]}
    env.update(extra_env or {})
    return subprocess.run([BASH, str(script)], capture_output=True, text=True, timeout=timeout, env=env)


def test_j_is_the_build_cpu_count_capped_at_the_quota_and_one_build_at_a_time(tmp_path):
    r = run_sem(tmp_path, 'BUILD_J=$(bq_j "$BUILD_CPUS"); bq_acquire; echo "J=$BUILD_J"; bq_release; ls "$BQ" | wc -l', extra_env={"MC2_BOX_QUOTA_CPUS": "17"})
    assert "build slot granted (n=1)" in r.stdout and "J=17" in r.stdout, r.stdout + r.stderr   # 28 cores, quota 17
    assert r.stdout.strip().endswith("0")      # released: no ticket left
    r = run_sem(tmp_path, 'echo "J=$(bq_j "$BUILD_CPUS")"', extra_env={"MC2_BOX_QUOTA_CPUS": "40"})
    assert "J=28" in r.stdout                  # fewer cores than the quota
    r = run_sem(tmp_path, 'echo "J=$(bq_j "$BUILD_CPUS")"', cpus="0-3,8", extra_env={"MC2_BOX_QUOTA_CPUS": "40"})
    assert "J=5" in r.stdout


def test_fifo_queue_prints_position_and_holder_then_grants_when_the_holder_dies(tmp_path):
    body = r'''
mkdir -p "$BQ"
sleep 30 & H=$!
echo $H > "$BQ/0000000000000000001.holder.$H"
echo 999999 > "$BQ/0000000000000000000.deadlane.999999"     # dead pid: pruned, never counted
( sleep 1.5; kill $H ) &
bq_acquire
echo "after-grant:$(ls "$BQ" | wc -l)"
bq_release
'''
    r = run_sem(tmp_path, body)
    assert "[box_run] build queued: position 1 of 1, holder holder " in r.stdout, r.stdout + r.stderr
    assert "build slot granted" in r.stdout
    assert r.stdout.index("queued") < r.stdout.index("granted")
    assert "after-grant:1" in r.stdout          # only our own ticket remained (holder died, dead ticket pruned)


def test_second_waiter_stays_behind_the_first(tmp_path):
    body = r'''
mkdir -p "$BQ"
sleep 30 & H=$!; sleep 30 & W=$!
echo $H > "$BQ/0000000000000000001.holder.$H"
echo $W > "$BQ/0000000000000000002.earlier.$W"
( sleep 1.5; kill $H $W ) &
bq_acquire
'''
    r = run_sem(tmp_path, body)
    assert "position 2 of 2, holder holder" in r.stdout, r.stdout + r.stderr


def test_remote_script_uses_the_semaphore_and_client_tie():
    rs = box_run.REMOTE
    assert "#@@CLIENT_TIE@@" not in rs and "#@@BUILD_SEM_USE@@" not in rs and "#@@FRESH_FN@@" not in rs
    assert "bq_acquire" in rs and "bq_release" in rs and "-j$BUILD_J mc2" in rs and "-j16" not in rs and "-j$BUILD_J -k 0" in rs and "32-63" not in rs
    assert "client gone" in rs and "grep -q '^sshd'" in rs
    assert subprocess.run([BASH, "-n"], input=rs.encode(), capture_output=True).returncode == 0
    # the lane trap releases a held ticket on any exit
    assert '[ -n "${TK:-}" ] && rm -f "$TK"' in rs


def fake_proc(root, pid, cmd, ppid):
    d = root / str(pid)
    d.mkdir(parents=True)
    (d / "cmdline").write_bytes(cmd.replace(" ", "\0").encode())
    (d / "stat").write_text(f"{pid} (bash) S {ppid} 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n")


def test_health_reports_holders_waiters_and_orphans(tmp_path, monkeypatch):
    proc, q, cg = tmp_path / "proc", tmp_path / "q", tmp_path / "cg"
    for d in (proc, q, cg):
        d.mkdir()
    (cg / "cpu.max").write_text("1730000 100000\n")
    fake_proc(proc, 10, "bash /workspace/lanes/.box_lane_holder.sh 0 holder", 500)
    fake_proc(proc, 11, "bash /workspace/lanes/.box_lane_waiter.sh 0 waiter", 500)
    fake_proc(proc, 12, "bash /workspace/lanes/.box_lane_lost.sh 0 lost", 1)         # reparented to init: client gone
    fake_proc(proc, 13, "sh -c cat > /workspace/lanes/.box_lane_lost.sh", 1)         # the wrapper shell is not counted
    for name, pid in (("0001.holder.10", 10), ("0002.waiter.11", 11), ("0000.dead.99", 99)):
        (q / name).write_text(str(pid))
    monkeypatch.setattr(sup, "PROC", str(proc))
    monkeypatch.setattr(bhr, "CGROUP", str(cg))
    monkeypatch.setenv("MC2_BOX_BUILDQ", str(q))
    monkeypatch.delenv("MC2_BOX_BUILDS", raising=False)
    warns = []
    bq = bhr.build_queue(__import__("time").time(), warns)
    assert bq["slots"] == 1
    assert [t["lane"] for t in bq["holders"]] == ["holder"] and [t["lane"] for t in bq["waiters"]] == ["waiter"]
    assert bq["orphans"] == [{"lane": "lost", "pid": 12}]
    assert any("orphan lane builds" in w and "lost" in w for w in warns)
    text = box_health.summary({"ok": True, "time": "t", "build_queue": bq})
    assert "builds  slots=1 holders=[holder 0m] waiting=1[waiter 0m] orphans=1" in text
