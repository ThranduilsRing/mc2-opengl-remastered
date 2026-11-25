import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cook_forest_floor as cff  # noqa: E402


def test_forest_texels_are_unit_normals_at_array_size():
    src = np.random.default_rng(1).integers(100, 156, (4096, 4096, 3), dtype=np.uint8)
    src[..., 2] = 255
    a = cff.forest_texels(src)
    assert a.shape == (cff.SIZE, cff.SIZE, 4) and a.dtype == np.uint8 and (a[..., 3] == 128).all()
    n = a[..., :3].astype(np.float64) / 127.5 - 1.0
    assert abs(np.linalg.norm(n, axis=-1).mean() - 1.0) < 0.02
