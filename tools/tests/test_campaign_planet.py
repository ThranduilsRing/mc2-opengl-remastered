"""Pixel checks for the atlas shared by campaign mission markers."""
from PIL import Image

from tools.cook_campaign_planet import ROOT, SOURCE, OUTPUT, marker_alpha


def test_black_key_preserves_other_icon_colours_and_coverage():
    source = Image.new("RGBA", (4, 1))
    source.putdata([(0, 0, 0, 209), (0, 0, 0, 255),
                    (255, 255, 255, 143), (0, 47, 85, 255)])
    assert list(marker_alpha(source).getdata()) == [
        (0, 0, 0, 0), (0, 0, 0, 0),
        (255, 255, 255, 143), (0, 47, 85, 255)]


def test_shipped_marker_pixels_match_the_cook_and_have_no_black_square():
    with Image.open(SOURCE) as source:
        expected = marker_alpha(source)
        original = list(source.convert("RGBA").getdata())
    for extension in ("png", "tga"):
        with Image.open(OUTPUT / f"mcl_cm_5.{extension}") as atlas:
            assert atlas.size == (128, 128)
            assert atlas.convert("RGBA").tobytes() == expected.tobytes()
            pixels = list(atlas.convert("RGBA").getdata())
            assert all(a == 0 for r, g, b, a in pixels if r == g == b == 0)
            assert any(a == 255 and r == g == b == 255 for r, g, b, a in pixels)
            assert any(p != q for p, q in zip(pixels, original))
            # Both button states have clear padding and clear triangle interiors.
            for x in (0, 50, 51, 101):
                assert atlas.getpixel((x, 58))[3] == 0


def test_both_loader_formats_are_in_the_deploy_payload(monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "scripts"))
    from deploy_payload import GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_cm_5.png" in GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_cm_5.tga" in GAME_DATA_PAYLOAD
