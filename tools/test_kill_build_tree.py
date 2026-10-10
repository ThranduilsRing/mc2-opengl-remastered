"""kill_build_tree: only the named build tree's processes die (real child processes, no build)."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kill_build_tree as kbt  # noqa: E402

console_child = kbt.console_child


def _sleeper(cwd):
    return subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"], cwd=str(cwd),
                            **console_child.child_kwargs(redirected=True))


def test_select_is_pure_and_respects_prefix_and_protection():
    procs = [{"pid": 1, "name": "cl.exe", "cmdline": "cl /Fo A:/w/build-x/a.obj", "cwd": ""},
             {"pid": 2, "name": "cl.exe", "cmdline": "cl /Fo A:/w/build-x2/a.obj", "cwd": "A:/w/build-x2"},
             {"pid": 3, "name": "ninja.exe", "cmdline": "ninja", "cwd": "A:/w/build-x/sub"},
             {"pid": 4, "name": "bash.exe", "cmdline": "bash A:/w/build-x", "cwd": "A:/w/build-x"},
             {"pid": 5, "name": "cl.exe", "cmdline": "cl", "cwd": "A:/w/build-x"}]
    got = [p["pid"] for p in kbt.select_victims(procs, "A:/w/build-x", protect={5})]
    assert got == [1, 3]                    # not build-x2, not a shell, not a protected pid


def test_kills_only_the_process_in_the_named_tree(tmp_path):
    inside, outside = tmp_path / "build-a", tmp_path / "build-b"
    inside.mkdir()
    outside.mkdir()
    a, b = _sleeper(inside), _sleeper(outside)
    try:
        time.sleep(1.0)
        victims = kbt.kill(str(inside), images=None)
        assert [v["pid"] for v in victims] == [a.pid]
        a.wait(timeout=15)
        assert b.poll() is None             # the other tree's process is untouched
    finally:
        for p in (a, b):
            if p.poll() is None:
                p.kill()


def test_dry_run_ends_nothing(tmp_path):
    p = _sleeper(tmp_path)
    try:
        time.sleep(1.0)
        assert [v["pid"] for v in kbt.kill(str(tmp_path), images=None, dry_run=True)] == [p.pid]
        assert p.poll() is None
    finally:
        p.kill()
