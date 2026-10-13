"""scripts/check-ab-kit.py refuses an A/B arm that sets a gate the engine no longer reads.

WHY: arms that set a deleted gate (or one that became an options.cfg entry) A/B two identical builds and read as "no difference".
Uses temp kit folders only.
"""
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))
import console_child  # noqa: E402  (windowless console children, scripts/console_child.py)

SCRIPT = Path(__file__).resolve().parents[1] / "check-ab-kit.py"

COMMON = """REM header
cd /d "%~dp0"
REM  COMMON BLOCK -- byte-identical in every AB-*.bat in this folder.
set MC2_LAUNCHER_ENV_JSON=0
set MC2_CLOUD_SHADOW=0
set MC2_TREE_DIAG_TRACE=
set MC2_LOG=
"""
BASELINE_TAIL = "REM  #### This arm overrides nothing. It IS the baseline. ####\n"
ARM_TAIL = "REM  #### THE ONLY LINE(S) THIS ARM CHANGES FROM AB-0 -- BELOW HERE ####\n"


def kit(tmp_path, arm_override):
    (tmp_path / "AB-0-DEFAULT-baseline.bat").write_text(COMMON + BASELINE_TAIL, newline="\n")
    (tmp_path / "AB-3-MC2_CLOUD_SHADOW-1-on.bat").write_text(COMMON + ARM_TAIL + arm_override, newline="\n")
    return subprocess.run([sys.executable, str(SCRIPT), str(tmp_path)], capture_output=True, text=True, **console_child.child_kwargs())


def test_a_kit_of_live_gates_passes(tmp_path):
    r = kit(tmp_path, "set MC2_CLOUD_SHADOW=1\n")
    assert r.returncode == 0, r.stdout


def test_an_arm_that_sets_a_retired_gate_fails_and_names_it(tmp_path):
    r = kit(tmp_path, "set MC2_CLOUD_SHADOW=1\nset MC2_SUN_AXIS_FIX=1\n")
    assert r.returncode == 1
    assert "MC2_SUN_AXIS_FIX" in r.stdout and "no longer lists" in r.stdout
