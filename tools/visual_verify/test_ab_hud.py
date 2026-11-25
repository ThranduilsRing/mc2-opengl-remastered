"""What: ab.py's --hud flag. Why: the capture argv hardcodes --clean (HUD hidden), so a HUD change can never show in an
A/B; with --hud the flag must drop --clean from the capture command line and the choice must be recorded in summary.json.
No engine is run: only the argv building and the summary recording are under test."""
import json
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab  # noqa: E402


def test_run_capture_accepts_path_install(tmp_path, monkeypatch):
    class Completed:
        def wait(self):
            return 0
    monkeypatch.setattr(ab.subprocess, "Popen", lambda *args, **kwargs: Completed())
    ab.run_capture(tmp_path, "bm1", tmp_path / "out", [])


def test_capture_argv_keeps_clean_by_default():
    cmd = ab.capture_argv("A:/inst", "bm1,bm2", Path("out/a"), [])
    assert "--clean" in cmd


def test_capture_argv_drops_clean_with_hud():
    cmd = ab.capture_argv("A:/inst", "bm1,bm2", Path("out/a"), [], hud=True)
    assert "--clean" not in cmd
    assert cmd[cmd.index("--bookmarks") + 1] == "bm1,bm2"
    assert cmd[cmd.index("--out") + 1] == str(Path("out/a"))


def test_capture_argv_hud_keeps_env_and_other_flags():
    cmd = ab.capture_argv("A:/inst", "bm1", Path("out/a"), ["K=V"], hud=True)
    assert cmd[cmd.index("--env") + 1] == "K=V"
    assert "--install" in cmd and cmd[cmd.index("--install") + 1] == "A:/inst"


def _png(path, value):
    from PIL import Image
    Image.new("RGB", (16, 12), value).save(path)


def test_compare_records_hud_in_summary(tmp_path):
    for side, value in (("a", (10, 20, 30)), ("b", (12, 22, 33))):
        d = tmp_path / side / "bm1"
        d.mkdir(parents=True)
        _png(d / "bm1.png", value)
    out = tmp_path / "cmp"
    ab.compare(tmp_path / "a", tmp_path / "b", out, [], bookmarks=["bm1"], hud=True)
    summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    assert summary["hud"] is True
    ab.compare(tmp_path / "a", tmp_path / "b", out, [], bookmarks=["bm1"])
    summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    assert summary["hud"] is False


def test_main_accepts_the_hud_flag():
    # --hud must be consumed by argparse (not "unrecognized"); without --bookmarks main then exits with its own message
    with pytest.raises(SystemExit) as exc:
        ab.main(["--hud"])
    assert str(exc.value) == "--bookmarks required"
