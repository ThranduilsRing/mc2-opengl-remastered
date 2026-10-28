import os
import sys
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import run_battle_bench as rbb  # noqa: E402


def test_baseline_command_mirrors_current_shape():
    args = Namespace(baseline_exe=Path("E:/base/mc2.exe"), runs=2, duration=30, resolution="4k", acceptance=True)
    cmd = rbb.baseline_command(args, Path("out-baseline"))
    assert "--allow-non-head" in cmd and "--acceptance" in cmd
    assert cmd[cmd.index("--exe") + 1].replace("\\", "/") == "E:/base/mc2.exe"
    assert cmd[cmd.index("--out") + 1] == "out-baseline"
    assert cmd[cmd.index("--runs") + 1] == "2" and cmd[cmd.index("--resolution") + 1] == "4k"
    args.acceptance = False
    assert "--acceptance" not in rbb.baseline_command(args, Path("o"))
