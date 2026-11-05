"""saveload pins follow tools/sim_determinism.py: Linux reads and writes tests/saveload/baselines/linux/, every other platform the root dir."""
from __future__ import annotations

import os
import sys
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import saveload_roundtrip as sl  # noqa: E402


def test_linux_pins_live_in_the_linux_subdir():
    with patch.object(sl.sys, "platform", "linux"):
        assert sl.baseline_path("mc2_01") == os.path.join(sl.BASELINE_DIR, "linux", "mc2_01.json")
        assert sl.text_ref_path("mc2_01") == os.path.join(sl.BASELINE_DIR, "linux", "mc2_01.text.txt")


def test_windows_pins_stay_in_the_root_dir():
    with patch.object(sl.sys, "platform", "win32"):
        assert sl.baseline_path("mc2_01") == os.path.join(sl.BASELINE_DIR, "mc2_01.json")
        assert sl.text_ref_path("mc2_01") == os.path.join(sl.BASELINE_DIR, "mc2_01.text.txt")
