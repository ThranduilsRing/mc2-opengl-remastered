"""MC2_CAPTURE_FOCUS arg plumbing: --focus / --focus-zoom -> declared env."""
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import mc2_env_pin as env_pin  # noqa: E402


def test_unset_declares_nothing():
    assert env_pin.focus_declarations(None) == []
    assert env_pin.focus_declarations("") == []


def test_mech_default_and_indexed():
    assert env_pin.focus_declarations("mech") == ["MC2_CAPTURE_FOCUS=mech"]
    assert env_pin.focus_declarations("mech:3") == ["MC2_CAPTURE_FOCUS=mech:3"]


def test_zoom_is_declared():
    assert env_pin.focus_declarations("mech:1", 350.0) == [
        "MC2_CAPTURE_FOCUS=mech:1", "MC2_CAPTURE_FOCUS_ZOOM=350"]


@pytest.mark.parametrize("bad", ["building", "mech:", "mech:x", "mech:-1", "Mech"])
def test_bad_target_rejected(bad):
    with pytest.raises(ValueError):
        env_pin.focus_declarations(bad)


def test_zoom_without_focus_or_out_of_range_rejected():
    with pytest.raises(ValueError):
        env_pin.focus_declarations(None, 200.0)
    with pytest.raises(ValueError):
        env_pin.focus_declarations("mech", 5.0)


def test_declarations_survive_env_pin():
    decl = env_pin.parse_declarations(env_pin.focus_declarations("mech:2", 250.0))
    env, record = env_pin.build_pinned_env({}, decl, environ={})
    assert env["MC2_CAPTURE_FOCUS"] == "mech:2"
    assert env["MC2_CAPTURE_FOCUS_ZOOM"] == "250"


def test_focus_tag_is_runner_owned():
    from smoke_lib import runner
    assert "CAPTURE_FOCUS" in runner.RUNNER_DIAG_TAGS


@pytest.mark.parametrize("tool", ["scripts/run_visual_capture.py",
                                  "tools/visual_verify/capture.py",
                                  "tools/visual_verify/ab.py"])
def test_cli_exposes_flags(tool):
    out = subprocess.run([sys.executable, "-B", str(ROOT / tool), "--help"],
                         capture_output=True, text=True, timeout=60)
    assert "--focus-zoom" in out.stdout and "--focus " in out.stdout.replace("--focus  ", "--focus ")
