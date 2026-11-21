"""ab.py reports the resolution each arm captured at beside the requested one and warns when they differ."""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def _png(d, size):
    (d / "bm").mkdir(parents=True, exist_ok=True)
    Image.new("RGB", size, (1, 2, 3)).save(d / "bm" / "p01.png")


def test_matching_size_has_no_warning(tmp_path):
    _png(tmp_path / "a", (64, 36))
    lines = ab.resolution_lines(["MC2_WIDTH=64", "MC2_HEIGHT=36"], (("A", tmp_path / "a"),), ["bm"])
    assert lines == ["[ab] resolution A: requested 64x36, captured 64x36"]


def test_4k_request_captured_at_1080p_warns(tmp_path):
    _png(tmp_path / "a", (1920, 1080))
    _png(tmp_path / "b", (3840, 2160))
    lines = ab.resolution_lines(["MC2_WIDTH=3840", "MC2_HEIGHT=2160"],
                                (("A", tmp_path / "a"), ("B", tmp_path / "b")), ["bm"])
    assert lines[0].startswith("[ab] WARNING: resolution A") and "captured 1920x1080" in lines[0] and "DIFFERS" in lines[0]
    assert "WARNING" not in lines[1]


def test_no_request_uses_pin_and_missing_png_warns(tmp_path):
    assert ab.requested_size([]) == (1920, 1080)
    lines = ab.resolution_lines([], (("A", tmp_path / "none"),), ["bm"])
    assert "WARNING" in lines[0] and "no usable png" in lines[0]
