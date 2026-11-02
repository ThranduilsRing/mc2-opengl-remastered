import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
pytest.importorskip("PIL.Image")
from PIL import Image, ImageDraw  # noqa: E402
import mech_capture_check as m  # noqa: E402


def _ground():
    return Image.new("RGB", (320, 240), (90, 110, 70))


def _with_mech():
    im = _ground()
    d = ImageDraw.Draw(im)
    for i in range(0, 60, 6):
        for j in range(0, 60, 6):
            d.rectangle((130 + i, 90 + j, 135 + i, 95 + j), fill=(40 + 180 * ((i + j) // 6 % 2), 60, 200 - 150 * ((i // 6) % 2)))
    return im


def test_mech_passes_empty_ground_fails():
    ref = _ground()
    assert m.pixel_check(_with_mech(), ref)["passed"]
    assert not m.pixel_check(_ground(), ref)["passed"]      # hidden-mech stand-in must fail


def test_reference_ratio_rejects_busy_ground():
    assert not m.pixel_check(_with_mech(), _with_mech())["passed"]


def test_gen_bookmarks():
    d = m.gen_bookmarks("mc2_01", 3, True)
    assert [b["name"] for b in d["bookmarks"]] == ["walk_00", "walk_01", "walk_02", "empty_ground"]


def test_collect_and_sheet(tmp_path):
    for k, (im, mid) in enumerate([(_with_mech(), 11), (_ground(), 12)]):
        im.save(tmp_path / f"walk_{k:02d}.png")
        (tmp_path / f"walk_{k:02d}.json").write_text(json.dumps(
            {"focus": {"found": True},
             "focus_walk": {"mode": "all", "index": k, "total": 2, "id": mid, "team": 1, "name": f"M{k}"}}))
    (tmp_path / "walk_02.json").write_text(json.dumps(
        {"focus": {"found": False},
         "focus_walk": {"mode": "all", "index": 2, "total": 2, "id": -1, "team": -1, "name": ""}}))
    (tmp_path / "other.json").write_text("{}")
    _ground().save(tmp_path / "ref.png")
    rows = m.collect(tmp_path, tmp_path / "ref.png")
    assert [r["passed"] for r in rows] == [True, False]   # surplus index is skipped
    assert m.contact_sheet(rows, tmp_path / "sheet.png") and (tmp_path / "sheet.png").exists()
