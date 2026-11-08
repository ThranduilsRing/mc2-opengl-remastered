"""p31 item 6: the laptop gate's build and the farm runner's jobs share one queue (tools/farm/queue_lock.py)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "farm"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import pytest  # noqa: E402
import queue_lock  # noqa: E402
import runner  # noqa: E402


def test_a_second_holder_waits_and_times_out_while_the_first_holds(tmp_path):
    lock = tmp_path / "q.lock"
    with queue_lock.hold(lock):
        child = subprocess.run([sys.executable, "-B", "-c",
                                "import sys; sys.path.insert(0, sys.argv[1]); import queue_lock\n"
                                "try:\n    queue_lock.hold(sys.argv[2], timeout=1, poll=0.1).__enter__()\nexcept TimeoutError:\n    sys.exit(7)\n",
                                str(ROOT / "tools" / "farm"), str(lock)])
        assert child.returncode == 7
    with queue_lock.hold(lock, timeout=1):   # released on exit
        pass


def test_the_lock_dies_with_its_holder(tmp_path):
    lock = tmp_path / "q.lock"
    holder = subprocess.Popen([sys.executable, "-B", "-c",
                               "import sys, time; sys.path.insert(0, sys.argv[1]); import queue_lock\n"
                               "with queue_lock.hold(sys.argv[2]):\n    print('held', flush=True); time.sleep(60)\n",
                               str(ROOT / "tools" / "farm"), str(lock)], stdout=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == "held"
        with pytest.raises(TimeoutError):
            with queue_lock.hold(lock, timeout=0.5, poll=0.1):
                pass
    finally:
        holder.kill()
        holder.wait()
    with queue_lock.hold(lock, timeout=5, poll=0.1):   # a crashed holder cannot wedge the queue
        pass


def test_runner_does_not_claim_a_job_while_a_gate_build_holds_the_queue(tmp_path, monkeypatch):
    (tmp_path / "jobs").mkdir()
    job = tmp_path / "jobs" / "j1.json"
    job.write_text("{}")
    ran = []
    monkeypatch.setattr(runner, "run_job", lambda claimed, root: ran.append(claimed))
    real_hold = queue_lock.hold
    monkeypatch.setattr(queue_lock, "hold", lambda p, timeout=None, **k: real_hold(p, timeout=0.3, poll=0.05))
    with real_hold(tmp_path / "queue.lock"):   # the laptop gate's build + gate
        runner.poll_once(tmp_path)
        assert not ran and job.exists()   # still unclaimed, still cancellable
    runner.poll_once(tmp_path)
    assert len(ran) == 1 and not job.exists()


def test_laptop_gate_run_takes_the_farm_queue(tmp_path, monkeypatch):
    import laptop_gate
    seen = []
    monkeypatch.setattr(laptop_gate, "FARM_QUEUE_LOCK", str(tmp_path / "queue.lock"))
    monkeypatch.setattr(laptop_gate, "_r_run", lambda w, extra: seen.append(("run", w)) or 0)
    real_hold = queue_lock.hold
    monkeypatch.setattr(queue_lock, "hold", lambda p, timeout=None, **k: real_hold(p, timeout=0.3, poll=0.05))
    with real_hold(tmp_path / "queue.lock"):   # a perf job in flight
        with pytest.raises(TimeoutError):
            laptop_gate.r_run("7", [])
        assert not seen   # the build never started under a running job
    assert laptop_gate.r_run("7", []) == 0 and seen == [("run", "7")]
