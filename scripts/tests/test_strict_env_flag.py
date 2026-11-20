"""--strict-env on run_smoke.py / run_visual_capture.py: dropped MC2_* names warn loudly by default and fail on request."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_run_smoke_warns_and_has_strict_env():
    text = (ROOT / "scripts" / "run_smoke.py").read_text(encoding="utf-8")
    assert "[ENV-DROPPED] WARNING:" in text
    assert '"--strict-env"' in text
    # strict exits non-zero right after the warning, and the dropped set is unchanged (still recorded in the manifest)
    block = text[text.index("[ENV-DROPPED] WARNING:"):]
    assert 'getattr(args, "strict_env", False)' in block[:900] and "sys.exit(2)" in block[:900]
    assert '_env_manifest["dropped_unregistered_env"]' in text


def test_run_smoke_help_lists_strict_env():
    out = subprocess.run([sys.executable, "-B", str(ROOT / "scripts" / "run_smoke.py"), "--help"],
                         capture_output=True, text=True, timeout=60).stdout
    assert "--strict-env" in out


def test_visual_capture_strict_env_fails_on_stray_name():
    env = dict(os.environ, MC2_NOT_A_REAL_GATE_ZZ="1")
    r = subprocess.run([sys.executable, "-B", str(ROOT / "scripts" / "run_visual_capture.py"), "--strict-env"],
                       capture_output=True, text=True, timeout=60, env=env)
    assert r.returncode == 2
    assert "MC2_NOT_A_REAL_GATE_ZZ" in r.stderr and "--strict-env" in r.stderr
