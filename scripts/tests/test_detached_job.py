import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import detached_job  # noqa: E402

PY = sys.executable


def test_start_wait_captures_exit_code_and_log(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    st = detached_job.start([PY, "-c", "print('hi'); raise SystemExit(3)"], name="a")
    assert st["name"] == "a"
    st = detached_job.wait("a", timeout=60)
    assert st["state"] == "done" and st["rc"] == 3 and st["pid"] and st["supervisor_pid"]
    assert "hi" in open(st["log"]).read()
    assert detached_job.verdict_line(st).startswith("FAIL rc=3 ")


def test_job_survives_the_starting_process_and_reattaches(tmp_path, monkeypatch):
    """Start from a child python that exits at once; the job keeps running, status sees it, a second start re-attaches, wait gets rc."""
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    launcher = tmp_path / "launch.py"
    launcher.write_text(
        "import sys\nsys.path.insert(0, %r)\nimport detached_job\n"
        "detached_job.start([sys.executable, '-c', 'import time; time.sleep(4); print(\"done\")'], name='b')\n" % os.path.join(ROOT, "tools"))
    subprocess.run([PY, str(launcher)], check=True, env=dict(os.environ, MC2_JOBS_DIR=str(tmp_path)), timeout=60)
    st = detached_job.status("b")   # the launching process is gone
    assert st["state"] == "running" and detached_job.pid_alive(st["supervisor_pid"])
    again = detached_job.start([PY, "-c", "raise SystemExit(9)"], name="b")   # re-attach, no second copy
    assert again["supervisor_pid"] == st["supervisor_pid"]
    st = detached_job.wait("b", timeout=60)
    assert st["state"] == "done" and st["rc"] == 0
    assert detached_job.run([PY, "-c", "pass"], name="b")[0] == 0   # a finished name may be reused


def test_timeout_kills_and_reports_124(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    rc, _, st = detached_job.run([PY, "-c", "import time; time.sleep(60)"], name="t", timeout=1)
    assert rc == 124 and st["state"] == "done"


def test_unknown_command_is_127_and_dead_supervisor_is_lost(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    rc, _, st = detached_job.run(["definitely-not-a-command-xyz"], name="n")
    assert rc == 127
    detached_job.write_state({"name": "dead", "cmd": ["x"], "cwd": ".", "log": "x", "started": time.time() - 100, "state": "running",
                              "supervisor_pid": 2 ** 22 + 12345, "pid": None, "rc": None, "ended": None})
    assert detached_job.status("dead")["state"] == "lost"
    assert detached_job.run([PY, "-c", "pass"], name="dead")[0] == 0   # a lost job is relaunched, not waited on forever


def test_cli_status_and_wait(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("MC2_JOBS_DIR", str(tmp_path))
    assert detached_job.main(["run", "--name", "c", "--", PY, "-c", "pass"]) == 0
    assert capsys.readouterr().out.startswith("PASS rc=0 ")
    assert detached_job.main(["status", "c"]) == 0
    assert detached_job.main(["wait", "c"]) == 0
    assert detached_job.main(["status", "nope"]) == 2
