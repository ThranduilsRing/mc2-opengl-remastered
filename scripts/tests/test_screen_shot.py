"""SCREEN-SETTLE-SHOT-1: scripts/screen_shot.py env, verdict parsing, black-frame check, and the box_run wiring."""
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import screen_shot as ss  # noqa: E402


def test_parse_screen():
    assert ss.parse_screen("mechbay") == ("mechbay", 30)
    assert ss.parse_screen("Campaign_Missions@12") == ("campaign_missions", 12)
    for bad in ("nowhere", "mechbay@", "mechbay@0", "mechbay@x"):
        with pytest.raises(ValueError):
            ss.parse_screen(bad)


def test_build_env_pins_and_drops_ambient(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_SOME_AMBIENT_GATE", "1")
    env, _ = ss.build_env("logistics@10", tmp_path / "a.tga")
    assert env["MC2_SHOT_SCREEN"] == "logistics@10"
    assert env["MC2_BOOT_TO_BAY"] == "campaign" and env["MC2_BOOT_TO_MISSION"] == "mc2_03"
    assert env["MC2_SCREENSHOT_PATH"].endswith("a.tga")
    assert env["MC2_WIDTH"] == "1920" and env["MC2_NO_CURSOR_GRAB"] == "1"
    assert "MC2_SOME_AMBIENT_GATE" not in env


def test_verdict_parsing():
    assert ss.parse_verdict("noise\n") == (None, "")
    v, rest = ss.parse_verdict('x\n[SCREEN_SHOT v1] FAIL screen=mechbay reason="ImGui not ready" imgui_vk_ready=0\n')
    assert v == "FAIL" and "imgui_vk_ready=0" in rest
    assert ss.parse_verdict("[SCREEN_SHOT v1] OK screen=a w=1 h=1")[0] == "OK"


def _tga(path, pixels):
    hdr = bytes([0, 0, 2]) + bytes(9) + struct.pack("<HH", 1, max(len(pixels) // 3, 1)) + bytes([24, 0])
    path.write_bytes(hdr + bytes(pixels))


def test_black_frame_detection(tmp_path):
    _tga(tmp_path / "b.tga", [0] * 300)
    _tga(tmp_path / "c.tga", [0] * 150 + [200] * 150)
    assert ss.tga_is_black(tmp_path / "b.tga")
    assert not ss.tga_is_black(tmp_path / "c.tga")
    assert ss.tga_is_black(tmp_path / "missing.tga")


def test_box_run_wiring():
    src = (ROOT / "tools" / "box_run.py").read_text(encoding="utf-8")
    assert "-DMC2_IMGUI=ON" in src   # front-end screens render black on the box without ImGui
    assert "screen:*)" in src and "scripts/screen_shot.py" in src
