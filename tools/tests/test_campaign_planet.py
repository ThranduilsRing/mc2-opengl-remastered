"""Pixel checks for the atlas shared by campaign mission markers."""
from PIL import Image
import pytest

from tools.cook_campaign_planet import ROOT, SOURCE, OUTPUT, marker_alpha


def test_black_key_preserves_other_icon_colours_and_coverage():
    source = Image.new("RGBA", (4, 1))
    source.putdata([(0, 0, 0, 209), (0, 0, 0, 255),
                    (255, 255, 255, 143), (0, 47, 85, 255)])
    assert list(marker_alpha(source).getdata()) == [
        (0, 0, 0, 0), (0, 0, 0, 0),
        (255, 255, 255, 143), (0, 47, 85, 255)]


@pytest.mark.parametrize("name", ["mcl_cm_5", "mcl_mn_4"])
def test_shipped_marker_pixels_match_the_cook_and_have_no_black_square(name):
    with Image.open(SOURCE.with_name(name + ".tga")) as source:
        expected = marker_alpha(source)
        original = list(source.convert("RGBA").getdata())
    for extension in ("png", "tga"):
        with Image.open(OUTPUT / f"{name}.{extension}") as atlas:
            assert atlas.size == (128, 128)
            assert atlas.convert("RGBA").tobytes() == expected.tobytes()
            pixels = list(atlas.convert("RGBA").getdata())
            assert all(a == 0 for r, g, b, a in pixels if r == g == b == 0)
            assert any(a == 255 and r == g == b == 255 for r, g, b, a in pixels)
            assert any(p != q for p, q in zip(pixels, original))
            # Both button states have clear padding and clear triangle interiors.
            if name == "mcl_cm_5":
                for x in (0, 50, 51, 101):
                    assert atlas.getpixel((x, 58))[3] == 0


def test_both_loader_formats_are_in_the_deploy_payload(monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "scripts"))
    from deploy_payload import GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_cm_5.png" in GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_cm_5.tga" in GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_mn_4.png" in GAME_DATA_PAYLOAD
    assert "data/art/gui/mcl_mn_4.tga" in GAME_DATA_PAYLOAD


def test_chrome_repack_of_the_marker_atlas_keeps_black_padding_transparent():
    # The manifest re-packs this sheet at load from per-widget PNGs. RGB widgets with opaque black
    # padding drew a translucent grey square behind the triangle (r1 capture); the sheet is RGBA
    # and no widget carries opaque pure black.
    import json
    manifest = json.loads((ROOT / "data/art/ui/manifest.json").read_text(encoding="utf-8"))
    assert manifest["sheets"]["mcl_cm_5"]["mode"] == "RGBA"
    widgets = sorted((ROOT / "data/art/ui/mcl_cm_5").glob("*.png"))
    assert len(widgets) == manifest["sheets"]["mcl_cm_5"]["rects"]
    for path in widgets:
        with Image.open(path) as im:
            assert im.mode == "RGBA", path.name
            assert all(a == 0 for r, g, b, a in im.getdata() if r == g == b == 0), path.name
