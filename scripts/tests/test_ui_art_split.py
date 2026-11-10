"""UI-CHROME-SPLIT-1: tools/ui_art_split.py cuts sheets into widgets the engine can re-pack pixel-identically."""
import importlib.util
import json
import os
import sys
from pathlib import Path

import pytest

PIL = pytest.importorskip("PIL.Image")
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ui_art_split", ROOT / "tools" / "ui_art_split.py")
split = importlib.util.module_from_spec(spec)
sys.modules["ui_art_split"] = split
spec.loader.exec_module(split)

FIT = """[PanelStatic0]
l XLocation = 0
st FileName = "Sheet1"
l UNormal = 0
l VNormal = 0
l UWidth = 16
l VHeight = 16

[PanelButton0]
st FileName = "Sheet1.tga"
l UNormal = 16
l VNormal = 0
l UPressed = 0
l VPressed = 16
l UWidth = 16
l VHeight = 16
"""


def make_install(tmp_path):
    ui = tmp_path / "data" / "defs" / "ui" / "legacy" / "game"
    ui.mkdir(parents=True)
    (ui / "Demo.fit").write_text(FIT)
    art = tmp_path / "data" / "art" / "gui"
    art.mkdir(parents=True)
    im = Image.new("RGB", (32, 32), (255, 0, 255))
    for x in range(32):
        for y in range(32):
            im.putpixel((x, y), ((x * 8) % 256, (y * 8) % 256, (x + y) % 256))
    im.save(art / "Sheet1.png")
    return tmp_path


def test_collect_finds_every_state_rect(tmp_path):
    inst = make_install(tmp_path)
    uses = split.collect(str(inst / "data" / "defs" / "ui"))
    assert set(uses["sheet1"]) == {(0, 0, 16, 16), (16, 0, 16, 16), (0, 16, 16, 16)}


def test_split_repack_is_pixel_identical_and_deterministic(tmp_path):
    inst = make_install(tmp_path)
    out1, out2 = tmp_path / "o1", tmp_path / "o2"
    m1, files, skipped = split.build(str(inst), str(out1), True)
    m2, _, _ = split.build(str(inst), str(out2), True)
    assert split.verify(str(inst), str(out1), m1) == 0
    assert json.dumps(m1, sort_keys=True) == json.dumps(m2, sort_keys=True)
    for rel in files:
        assert (out1 / rel).read_bytes() == (out2 / rel).read_bytes()
    assert len(m1["sheets"]["sheet1"]["widgets"]) == 3


def test_nine_insets_only_for_exactly_stretchable_art():
    flat = Image.new("RGB", (24, 24), (10, 20, 30))
    for x in range(24):
        for y in range(24):
            d = min(x, 23 - x, y, 23 - y)   # concentric frame: constant along each edge
            if d < 4:
                flat.putpixel((x, y), (d * 50 + 5, 90, 7))
    ins = split.nine_insets(flat)
    assert ins == [4, 4, 4, 4]
    noisy = Image.new("RGB", (24, 24))
    for x in range(24):
        for y in range(24):
            noisy.putpixel((x, y), (x * 10, y * 10, (x * y) % 256))
    assert split.nine_insets(noisy) is None


def test_checked_in_manifest_matches_files():
    root = ROOT / "data" / "art" / "ui"
    m = json.loads((root / "manifest.json").read_text())
    n = 0
    for key, sh in m["sheets"].items():
        for name, ent in sh["widgets"].items():
            with Image.open(root / key / (name + ".png")) as im:
                assert list(im.size) == ent["rect"][2:]
            n += 1
    assert n > 1000


@pytest.mark.skipif(not os.path.isdir("A:/Games/mc2-opengl/mc2-win64-v0.4/data/defs/ui/legacy"),
                    reason="stock install not present")
def test_every_stock_fit_rect_of_a_listed_sheet_resolves_to_a_widget():
    inst = "A:/Games/mc2-opengl/mc2-win64-v0.4"
    uses = split.collect(inst + "/data/defs/ui")
    m = json.loads((ROOT / "data" / "art" / "ui" / "manifest.json").read_text())
    for key, sh in m["sheets"].items():
        have = {tuple(e["rect"]) for e in sh["widgets"].values()}
        missing = set(uses[key]) - have
        assert not missing, (key, sorted(missing)[:3])
