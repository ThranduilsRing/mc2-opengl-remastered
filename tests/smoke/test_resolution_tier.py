"""The weekly `resolution` smoke tier and the pinned capture/trace size (scripts/smoke_lib/render_resolution.py)."""
from pathlib import Path

import pytest

from scripts.smoke_lib import render_resolution
from scripts.smoke_lib.manifest import parse_manifest

ROOT = Path(__file__).resolve().parents[2]


def test_parse_accepts_wxh_and_rejects_the_rest():
    assert render_resolution.parse("1920x1080") == (1920, 1080)
    assert render_resolution.parse(" 3840 X 2160 ") == (3840, 2160)
    for bad in ("1920", "x1080", "0x1080", "1920x-1", "axb", ""):
        with pytest.raises(ValueError):
            render_resolution.parse(bad)


def test_size_env_is_the_engine_pair():
    assert render_resolution.size_env(1280, 720) == {"MC2_WIDTH": "1280", "MC2_HEIGHT": "720"}
    assert render_resolution.pinned_env() == {"MC2_WIDTH": "1920", "MC2_HEIGHT": "1080"}


def test_manifest_parses_resolution_entries(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text('tier1 mc2_01\nresolution mc2_01 resolution=1920x1080\nresolution mc2_01 resolution=BAD\n')
    entries = parse_manifest(p)
    res = [e for e in entries if e.tier == "resolution"]
    assert [e.resolution for e in res] == ["1920x1080", ""]   # a malformed size is dropped with a warning, not kept
    assert next(e for e in entries if e.tier == "tier1").resolution == ""


def test_shipped_manifest_has_one_entry_per_tier_size():
    entries = [e for e in parse_manifest(ROOT / "tests" / "smoke" / "smoke_missions.txt") if e.tier == "resolution"]
    assert [render_resolution.parse(e.resolution) for e in entries] == list(render_resolution.RESOLUTION_TIER_SIZES)
    assert {e.stem for e in entries} == {"mc2_01"}


@pytest.mark.parametrize("driver", ["scripts/run_visual_capture.py", "tools/tracy_capture_compare.py"])
def test_capture_and_trace_drivers_read_the_pinned_size(driver):
    assert "render_resolution.pinned_env()" in (ROOT / driver).read_text(encoding="utf-8")
