"""--ref-exe / --ref-commit reach every run_smoke.py child of sim_determinism and saveload_roundtrip.

WHY: the run_smoke fingerprint override did not reach these wrappers, so an A/B of an exe built from another commit failed the
fingerprint gate and lanes wrote one-off wrappers around them (twice).
"""
from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sim_determinism as sd  # noqa: E402
import saveload_roundtrip as sl  # noqa: E402


class _Stop(Exception):
    pass


def _capture_argv(module, invoke):
    """Run `invoke` until its first run_smoke child; -> that child's argv."""
    seen = []

    def fake_run(argv, **_kw):
        seen.append(argv)
        raise _Stop()

    with patch.object(module.subprocess, "run", fake_run):
        with pytest.raises(_Stop):
            invoke()
    assert seen and seen[0][1].endswith("run_smoke.py")
    return seen[0]


@pytest.mark.parametrize("flag,value", [("--ref-exe", "C:/ref/mc2.exe"), ("--ref-commit", "abc1234")])
def test_sim_determinism_run_forwards_ref(flag, value, tmp_path):
    argv = _capture_argv(sd, lambda: sd.main(["run", "mc2_01", "--install", "lane3", "--out", str(tmp_path), flag, value]))
    assert argv[argv.index(flag) + 1] == value


def test_sim_determinism_pairs_forward_ref(tmp_path):
    with patch.object(sd, "host_load", lambda: {"cpu": 0.0, "procs": {}}):
        argv = _capture_argv(sd, lambda: sd.main(["pairs-until-fail", "--mission", "mc2_01", "--max", "1", "--install", "lane3",
                                                  "--out", str(tmp_path), "--ref-commit", "abc1234"]))
    assert argv[argv.index("--ref-commit") + 1] == "abc1234"


def test_no_ref_means_no_ref_flags(tmp_path):
    argv = _capture_argv(sd, lambda: sd.main(["run", "mc2_01", "--install", "lane3", "--out", str(tmp_path)]))
    assert "--ref-exe" not in argv and "--ref-commit" not in argv


def test_ref_flags_are_exclusive(capsys):
    with pytest.raises(SystemExit):
        sd.main(["run", "mc2_01", "--ref-exe", "x", "--ref-commit", "y"])
    with pytest.raises(ValueError):
        sd.smoke_target("lane3", "x", "y")


@pytest.mark.parametrize("cmd", ["resave", "check"])
def test_saveload_fixture_forwards_ref(cmd, tmp_path):
    with patch.object(sl.savegame_backup, "guarded") as guarded:
        guarded.return_value.__enter__ = lambda *_: None
        guarded.return_value.__exit__ = lambda *_: False
        argv = _capture_argv(sl, lambda: sl.main([cmd, "--mission", "mc2_01", "--install", "lane3", "--save-dir", str(tmp_path),
                                                  "--out", str(tmp_path), "--ref-exe", "C:/ref/mc2.exe"]))
    assert argv[argv.index("--ref-exe") + 1] == "C:/ref/mc2.exe"


def test_saveload_run_forwards_ref(tmp_path):
    with patch.object(sl.savegame_backup, "guarded") as guarded:
        guarded.return_value.__enter__ = lambda *_: None
        guarded.return_value.__exit__ = lambda *_: False
        argv = _capture_argv(sl, lambda: sl.main(["run", "--install", "lane3", "--save-dir", str(tmp_path), "--ref-commit", "abc1234"]))
    assert argv[argv.index("--ref-commit") + 1] == "abc1234"


def test_selftests_still_pass():
    assert sd.selftest() == 0
    assert sl.selftest() == 0
