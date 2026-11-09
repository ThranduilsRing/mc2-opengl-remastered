"""Cutout mip coverage and conservative card geometry, independent of Vulkan."""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.cook import veg_card_cook as cook


def test_alpha_scale_recovers_half_coverage_after_filtering():
    alpha = np.arange(64, dtype=np.uint8).reshape(8, 8)
    result = cook.preserve_coverage(alpha, 0.5)
    assert np.count_nonzero(result >= 128) == 32
    assert result[0, 0] == 0


def test_dilation_keeps_alpha_and_existing_colours():
    rgba = np.zeros((9, 9, 4), dtype=np.uint8)
    rgba[4, 4] = (120, 80, 40, 255)
    result = cook.dilate_rgb(rgba)
    np.testing.assert_array_equal(result[:, :, 3], rgba[:, :, 3])
    np.testing.assert_array_equal(result[4, 4], rgba[4, 4])
    np.testing.assert_array_equal(result[4, 5, :3], rgba[4, 4, :3])
    assert not cook.dilate_rgb(np.zeros_like(rgba)).any()


def test_strip_encloses_every_opaque_texel_including_between_rows():
    rgba = np.zeros((96, 96, 4), dtype=np.uint8)
    rgba[:, 45:51, 3] = 255
    rgba[48, 10:86, 3] = 255
    strip = cook.trim_strip(Image.fromarray(rgba))
    for y, x in zip(*np.where(rgba[:, :, 3] >= 128)):
        t = (y + 0.5) / 96 * 3
        band = min(2, int(t))
        fraction = t - band
        left = strip[2*band][0] * (1-fraction) + strip[2*band+2][0] * fraction
        right = strip[2*band+1][0] * (1-fraction) + strip[2*band+3][0] * fraction
        assert left <= x / 96
        assert right >= (x+1) / 96
    assert sum(strip[i+1][0] - strip[i][0] for i in range(0, 8, 2)) < 4


def test_mip_chain_is_complete_and_repeatable():
    rgba = np.zeros((64, 64, 4), dtype=np.uint8)
    rgba[:, :32] = (120, 80, 40, 255)
    image = Image.fromarray(rgba)
    first, second = cook.mip_chain(image), cook.mip_chain(image)
    assert [m.size for m in first] == [(64 >> i, 64 >> i) for i in range(7)]
    for a, b in zip(first, second):
        assert a.tobytes() == b.tobytes()
        if a.width >= 2:
            assert np.count_nonzero(np.array(a)[:, :, 3] >= 128) == a.width * a.height // 2


def test_shipped_cards_and_biome_recipes_load():
    import json
    import struct

    root = Path(__file__).resolve().parents[2] / "data/vegetation"
    manifest = json.loads((root / "cooked/cards.json").read_text())
    assert [card["name"] for card in manifest] == cook.CARDS
    for card in manifest:
        name = card["name"]
        source = root / "cards" / f"{name}.png"
        cooked = root / "cooked" / f"{name}.ktx2"
        assert source.stat().st_size < 1_000_000
        assert cooked.stat().st_size < 1_000_000
        image = Image.open(source)
        assert image.size == (512,512) and image.mode == "RGBA"
        assert 0.01 < np.count_nonzero(np.array(image)[:,:,3] >= 128) / (512*512) < 0.9
        header = cooked.read_bytes()[:48]
        assert header[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
        fields = struct.unpack_from("<9I",header,12)
        assert fields[0] == 145 and fields[2:4] == (512,512)  # BC7_UNORM
        assert fields[7] == 10  # full mip chain
        assert len(card["strip"]) == 8
    for path in (root / "schemas").glob("*.json"):
        recipe = json.loads(path.read_text())
        assert 4 <= recipe["clump"]["radius"] <= 16
        assert 0 <= recipe["clump"]["tree_chance"] <= 1
        assert 6 <= recipe["clump"]["grass"] <= 16
        for name in recipe["cards_full"] + recipe["cards_sparse"]:
            assert (root / "cards" / f"{name}.png").is_file()


def test_procedural_woody_cards_repeat_without_external_sources(tmp_path):
    first, second = tmp_path/"first", tmp_path/"second"
    cook.procedural_woody_cards(first,tmp_path/"sheet-a.png")
    cook.procedural_woody_cards(second,tmp_path/"sheet-b.png")
    for name in cook.CARDS[4:]:
        assert (first/f"{name}.png").read_bytes() == (second/f"{name}.png").read_bytes()
