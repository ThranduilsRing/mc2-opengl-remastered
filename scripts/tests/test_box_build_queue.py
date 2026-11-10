"""Box build semaphore (box_run BUILD_SEM), client tie and the box_health build-queue report. Fakes only; nothing touches the box."""
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "box"))
sys.path.insert(0, str(ROOT / "tools" / "box" / "filler"))
import types
try:
    import fcntl  # noqa: F401
except ImportError:
    sys.modules["fcntl"] = types.ModuleType("fcntl")   # run_job is a Linux script; a stub lets its pure helpers be tested
import box_run
import box_health
import box_health_remote as bhr
import box_supervisor as sup
sys.path.append(str(ROOT / "scripts"))
import console_child  # noqa: E402  (no console window for the bash children)

BASH = shutil.which("bash") or "bash"


def run_sem(tmp_path, body, cpus="8-35", extra_env=None, timeout=60):
    script = tmp_path / "t.sh"
    script.write_text("set -u\nBR=lane-a\n" + box_run.BUILD_SEM + body, newline="\n")
    env = {"MC2_BOX_BUILDQ": str(tmp_path / "q").replace("\\", "/"), "BUILD_CPUS": cpus, "MC2_BOX_BUILDQ_POLL": "0.2", "PATH": __import__("os").environ["PATH"]}
    env.update(extra_env or {})
    return subprocess.run([BASH, str(script)], capture_output=True, text=True, timeout=timeout, env=env,
                          **console_child.child_kwargs(redirected=True))


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
    assert "[box_run] waiting: position 1, holder holder since " in r.stdout, r.stdout + r.stderr
    assert "build slot granted" in r.stdout
    assert r.stdout.index("waiting") < r.stdout.index("granted")
    assert "reclaimed stale build ticket 0000000000000000000.deadlane.999999" in r.stdout   # the dead-pid ticket is reclaimed and announced
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
    assert "waiting: position 2, holder holder since " in r.stdout, r.stdout + r.stderr


def test_remote_script_uses_the_semaphore_and_client_tie():
    rs = box_run.REMOTE
    assert "#@@CLIENT_TIE@@" not in rs and "#@@BUILD_SEM_USE@@" not in rs and "#@@FRESH_FN@@" not in rs
    assert "bq_acquire" in rs and "bq_release" in rs and "-j$BUILD_J mc2" in rs and "-j16" not in rs and "-j$BUILD_J -k 0" in rs and "32-63" not in rs
    assert "client gone" in rs and "grep -q '^sshd'" in rs
    assert subprocess.run([BASH, "-n"], input=rs.encode(), capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0
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


def test_cores_env_must_match_the_hardware(tmp_path):
    env = tmp_path / "cores.env"
    env.write_text("BUILD_CPUS=32-63\nGPU0_CPUS=0-7\nGPU3_CPUS=24-31\nGATE_GPU=3\n# GPU9_CPUS=1-2\n")
    probs = []
    bhr.cores_check(probs, [0], nproc=36, path=str(env))
    assert any("BUILD_CPUS=32-63" in p and "core 63" in p for p in probs)
    assert any("GPU 3" in p for p in probs) and any("GATE_GPU=3" in p for p in probs)
    assert not any("GPU 9" in p for p in probs)          # a comment is not config
    probs = []
    bhr.cores_check(probs, [0], nproc=36, path=str(ROOT / "tools/box/cores.env"))
    assert probs == []                                    # the tracked map fits this box (36 cores, GPU 0)


def test_filler_gpus_and_device_name_come_from_cores_env(tmp_path, monkeypatch):
    monkeypatch.setattr(sup, "LOCKS", str(tmp_path))
    (tmp_path / "cores.env").write_text('GPU0_CPUS=0-7\nGPU_NAME="RTX 4060"\n')
    assert sup.filler_gpus() == (0,)
    import run_job
    assert run_job.gpu_name(str(tmp_path / "cores.env")) == "RTX 4060"
    assert run_job.device_verdict('device_name="NVIDIA GeForce RTX 3060" api=1', want="RTX 4060")[1]


def test_an_unprovisioned_lane_source_is_box_infra_not_a_silent_exit():
    rs = box_run.REMOTE
    i = rs.index("worktree prune")
    assert "exit 77" in rs[i:i + 700] and rs.index("tick source") > i


def run_check(tmp_path, cmd, rc, logtext):
    out = tmp_path / "out"
    out.mkdir(exist_ok=True)
    mark = out / ".rowmark"
    mark.write_text("")
    import os
    os.utime(mark, (1000, 1000))
    if logtext is not None:
        (out / "diag.log").write_text(logtext)
    script = tmp_path / "g.sh"
    script.write_text(box_run.GAME_CHECK + 'game_not_run "$1" "$2" "$3" "$4" && echo NOT_RUN || echo OK\n', newline="\n")
    r = subprocess.run([BASH, str(script), cmd, str(rc), str(out).replace("\\", "/"), str(mark).replace("\\", "/")], capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    return r.stdout.strip()


def test_a_row_that_never_launched_mc2_is_not_a_pass(tmp_path):
    cmd = 'cd $I && for m in mc2_10; do ./mc2 --mission $m > $OUT/diag_$m.log 2>&1; done'
    assert run_check(tmp_path, cmd, 0, None) == "NOT_RUN"                         # the perf-next case: rc 0, no game log
    assert run_check(tmp_path, cmd, 0, "[VK_ENGINE_CONTEXT] device_name=x\n") == "OK"   # the engine logged: it ran
    assert run_check(tmp_path, cmd, 3, None) == "OK"                              # a nonzero rc is already a FAIL
    assert run_check(tmp_path, "echo hi", 0, None) == "OK"                        # rows that do not name mc2 are untouched


def test_remote_exports_install_and_out_and_wires_the_game_check():
    rs = box_run.REMOTE
    assert "export I OUT S" in rs and rs.index("export I OUT S") < rs.index("game_not_run \"$cmd\"")
    assert "GAME_NOT_RUN" in rs and "rc=81" in rs and subprocess.run([BASH, "-n"], input=rs.encode(), capture_output=True,
                                                                     **console_child.child_kwargs(redirected=True)).returncode == 0


def test_waiting_line_parse_names_position_holder_and_progress_age(tmp_path):
    """The waiting line is the one signal a lane uses to tell queued from hung: position, holder lane, time since its last progress."""
    body = r"""
mkdir -p "$BQ"
sleep 30 & H=$!; sleep 30 & W=$!
echo $H > "$BQ/1700000000000000000.holder.$H"; touch -d @1700000100 "$BQ/1700000000000000000.holder.$H"
echo $W > "$BQ/1700000000000000001.me.$W"
bq_scan 1700000000000000001.me.$W
echo "POS=$QPOS M=$QM HOLD=$QHOLD"
bq_waiting_line "$((QPOS - 1))" "$QHOLD" 1700000130
kill $H $W
"""
    r = run_sem(tmp_path, body)
    assert "POS=2 M=2 HOLD=1700000000000000000.holder." in r.stdout, r.stdout + r.stderr
    line = next(ln for ln in r.stdout.splitlines() if ln.startswith("[box_run] waiting:"))
    assert re.fullmatch(r"\[box_run\] waiting: position 1, holder holder since \S+, last progress 30s ago", line), line   # 130 - 100


def test_stale_holder_is_flagged_but_not_killed_from_the_waiter(tmp_path):
    body = r"""
mkdir -p "$BQ"
sleep 30 & H=$!
echo $H > "$BQ/1700000000000000000.holder.$H"; touch -d @1700000000 "$BQ/1700000000000000000.holder.$H"
bq_waiting_line 1 1700000000000000000.holder.$H 1700000700
kill -0 $H && echo "holder-still-alive"
kill $H
"""
    r = run_sem(tmp_path, body)
    assert "last progress 700s ago (HOLDER STALE" in r.stdout and "holder-still-alive" in r.stdout, r.stdout + r.stderr


def test_stall_timer_with_a_fake_clock(tmp_path):
    body = r"""
bq_stalled 1700000599 1700000000 && echo "599:stalled" || echo "599:ok"
bq_stalled 1700000600 1700000000 && echo "600:stalled" || echo "600:ok"
MC2_BOX_STALL_SECS=30 bq_stalled 1700000029 1700000000 && echo "30s-at-29:stalled" || echo "30s-at-29:ok"
MC2_BOX_NOW=1700000123 bq_now
"""
    r = run_sem(tmp_path, body)
    out = r.stdout.split()
    assert out[:4] == ["599:ok", "600:stalled", "30s-at-29:ok", "1700000123"], r.stdout + r.stderr


def test_guard_reports_stalled_with_last_log_line_and_ends_only_its_own_child(tmp_path):
    body = r"""
LOG="$BQ.log"; MARK="$BQ.mark"; mkdir -p "$BQ"; echo "compiling a.o" > "$LOG"
sleep 60 & OTHER=$!        # another lane's process: must survive
bq_acquire
export MC2_BOX_STALL_SECS=2
bq_guard "$LOG" "$MARK" &
GP=$!
wait $GP 2>/dev/null
kill -0 $OTHER 2>/dev/null && echo "other-alive"; kill $OTHER 2>/dev/null
echo "mark:$(cat "$MARK")"
"""
    r = run_sem(tmp_path, body, timeout=40)
    assert "[box_run] STALLED: no log progress for" in r.stdout and "last line: compiling a.o" in r.stdout, r.stdout + r.stderr
    assert "mark:[box_run] STALLED" in r.stdout


def test_stalled_has_its_own_verdict_and_exit_code():
    assert box_run.RC_STALLED == 79
    assert box_run.verdict(79, []) == ("STALLED", 79)
    assert box_run.verdict(1, ["0", "79"]) == ("STALLED", 79)
    assert "bq_guard" in box_run.REMOTE and "exit 79" in box_run.REMOTE
