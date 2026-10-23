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
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

SCRIPT = Path(__file__).resolve().parents[1] / "check-background-launch.py"


def _run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True, timeout=300, **console_child.child_kwargs(redirected=True))


def test_selftest_catches_each_rule():
    r = _run("--selftest")
    assert r.returncode == 0, r.stdout + r.stderr


def test_real_tree_is_clean():
    r = _run("--quiet")
    assert r.returncode == 0, r.stdout + r.stderr


def test_allow_json_keys_are_unique():
    import json
    root = SCRIPT.parents[1]
    def unique(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate allowlist key: {key}"
            result[key] = value
        return result
    for path in (root / "scripts").rglob("*allow*.json"):
        json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    for path in (root / "tests").rglob("*allow*.json"):
        json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
