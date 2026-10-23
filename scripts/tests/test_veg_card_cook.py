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
