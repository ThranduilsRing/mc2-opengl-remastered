"""LintSlots: host-wide cross-process counting semaphore for lint workers."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import lane_merge  # noqa: E402

HOLD = (
    "import sys,time;sys.path.insert(0,sys.argv[1]);import lane_merge;"
    "s=lane_merge.LintSlots(directory=sys.argv[2],n=1);fh=s.acquire();print('held',flush=True);time.sleep(60)"
)


def test_acquire_release_and_cap(tmp_path):
    s = lane_merge.LintSlots(directory=str(tmp_path), n=2)
    a, b = s.acquire(), s.acquire()
    assert s.try_acquire() is None          # both slots busy
    a.close()
    c = s.try_acquire()
    assert c is not None                    # released slot is reusable
    b.close()
    c.close()


def test_run_releases_on_exception(tmp_path):
    s = lane_merge.LintSlots(directory=str(tmp_path), n=1)

    def boom():
        raise RuntimeError("x")
    try:
        s.run(boom)
    except RuntimeError:
        pass
    fh = s.try_acquire()
    assert fh is not None
    fh.close()


def test_dead_process_frees_slot(tmp_path):
    tools = os.path.join(os.path.dirname(__file__), "..", "..", "tools")
    p = subprocess.Popen([sys.executable, "-B", "-c", HOLD, tools, str(tmp_path)], stdout=subprocess.PIPE, text=True)
    try:
        assert p.stdout.readline().strip() == "held"
        s = lane_merge.LintSlots(directory=str(tmp_path), n=1)
        assert s.try_acquire() is None      # the other process holds the only slot
        p.kill()
        p.wait()
        deadline = time.monotonic() + 10
        fh = None
        while fh is None and time.monotonic() < deadline:
            fh = s.try_acquire()
            time.sleep(0.1)
        assert fh is not None               # the OS dropped the dead holder's lock
        fh.close()
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
