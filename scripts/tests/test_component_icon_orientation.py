"""The cooked component icons are authored top-left (stock cell grid) and Image.cpp decodes the TGA origin header, so a
header-honouring decode of every RLE icon must put the opaque frame at the top-left corner. A pre-flipped file (the old
tools/render_component_icons.py behaviour, written for the retired RLE loader) lands at the bottom-left and fails here."""
from pathlib import Path

import pytest

pytest.importorskip("PIL.Image")
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ICONS = sorted((ROOT / "data" / "art").glob("mc2_component_*.tga"))


def test_icons_present():
    assert len(ICONS) >= 40, f"expected the cooked component icon set, found {len(ICONS)}"


@pytest.mark.parametrize("path", ICONS, ids=lambda p: p.name)
def test_icon_frame_is_top_left(path):
    header = path.read_bytes()[:18]
    assert header[2] == 10, f"{path.name}: not an RLE TGA"
    box = Image.open(path).convert("RGBA").getchannel("A").getbbox()
    assert box and box[0] == 0 and box[1] == 0, f"{path.name}: frame bbox {box} is not anchored top-left (icon stored upside down?)"


def test_some_icon_has_vertical_slack():
    """A full-height icon cannot show an inversion; at least one single-cell icon must, so the check above bites."""
    small = Image.open(ROOT / "data" / "art" / "mc2_component_armorplating.tga").convert("RGBA")
    assert small.getchannel("A").getbbox()[3] < small.height
