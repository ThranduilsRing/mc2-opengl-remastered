"""BACKGROUND-LAUNCH ratchet: the checker must fail on synthetic bad input.

WHY: a checker nobody has watched fail is indistinguishable from one that
cannot. `--selftest` builds throwaway trees containing a raw engine Popen, an
unflagged console child and an unmarked .bat launcher, and asserts each is
caught (and that the sanctioned forms pass). The second test asserts the real
tree is clean, i.e. the committed inventory/baseline match the source.
"""
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check-background-launch.py"


def _run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True, timeout=300, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def test_selftest_catches_each_rule():
    r = _run("--selftest")
    assert r.returncode == 0, r.stdout + r.stderr


def test_real_tree_is_clean():
    r = _run("--quiet")
    assert r.returncode == 0, r.stdout + r.stderr
