import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cook_dirt_sand as cds  # noqa: E402


def _flat(size):
    return np.full((size, size, 3), (128, 128, 255), np.uint8)


def test_sand_texels_is_deterministic_unit_normal_with_relief():
    variants = tuple((_flat(512), g, r) for _, g, r in cds.VARIANTS)
    a = cds.sand_texels(variants, size=512)
    assert a.shape == (512, 512, 4) and a.dtype == np.uint8
    assert np.array_equal(a, cds.sand_texels(variants, size=512))
    n = a[..., :3].astype(np.float64) / 127.5 - 1.0
    assert abs(np.linalg.norm(n, axis=-1).mean() - 1.0) < 0.02
    assert np.sqrt((n[..., :2] ** 2).mean()) > 0.02   # the stones and ripples alone carry relief on flat sources


def test_variant_sources_exist_on_the_cook_host_or_are_listed():
    assert all(len(v) == 3 and 512 % v[2] == 0 for v in cds.VARIANTS)
