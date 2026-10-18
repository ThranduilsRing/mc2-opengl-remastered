"""SHORE-FIELD-1 tests for cook_shore_field.py (synthetic fields, no game data)."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cook_shore_field as csf  # noqa: E402

WE = 80.0


def _staircase(n: int = 257, step: int = 16) -> np.ndarray:
    """A 45-degree coast quantized into step x step terraces: land (120) above-left, sea (20) below-right."""
    y, x = np.mgrid[0:n, 0:n]
    q = (x // step) - (y // step)
    return np.where(q <= 0, 120.0, 20.0)


def _diag_residual(field: np.ndarray) -> float:
    """RMS distance (cells) of each row's first water cell from the best-fit straight line."""
    n = field.shape[0]
    rows, xs = [], []
    for y in range(n // 4, 3 * n // 4):
        wet = np.nonzero(field[y] < WE)[0]
        if wet.size:
            rows.append(y)
            xs.append(wet[0])
    a, b = np.polyfit(rows, xs, 1)
    return float(np.sqrt(np.mean((np.asarray(xs) - (a * np.asarray(rows) + b)) ** 2)))


def _build(h: np.ndarray, **kw) -> dict:
    args = dict(open_wu=16.0, close_wu=16.0, sdf_sigma_wu=48.0, sdf_passes=4, band_wu=320.0,
                depth_sigma_wu=96.0, shore_slope=0.2)
    args.update(kw)
    return csf.build_field(h, WE, 8.0, **args)


def test_staircase_edge_gets_straighter():
    h = _staircase()
    res = _build(h)
    # 16-cell terraces sit ~4.6 cells RMS off the diagonal; the cooked edge must be far straighter.
    assert _diag_residual(res["field"]) < 0.35 * _diag_residual(h)


def test_far_land_and_deep_water_keep_their_side():
    h = _staircase()
    res = _build(h)
    assert res["field"][0, -1] < WE          # far sea corner stays water
    assert res["field"][-1, 0] >= WE         # far land corner stays land
    assert res["field"][200, 40] >= WE      # inland, well inside the land side


def test_lone_low_vertex_has_no_deep_diamond():
    """A single deep hole in a shallow shelf must not survive as a sharp deep spot in the tint."""
    h = np.full((129, 129), WE - 4.0)
    h[64, 64] = WE - 60.0
    h[:, :8] = WE + 40.0                      # some land so the SDF exists
    res = _build(h)
    depth = WE - res["field"]
    ring = depth[60:69, 60:69]
    assert ring.max() - np.median(depth[40:90, 40:90]) < 10.0


def test_upsample_is_corner_aligned():
    g = np.arange(25, dtype=np.float64).reshape(5, 5)
    up = csf.upsample(g, 4)
    assert up.shape == (17, 17)
    assert np.allclose(up[::4, ::4], g, atol=1e-6)


def test_write_field_float16_roundtrip(tmp_path):
    f = np.linspace(-20, 700, 9 * 9).reshape(9, 9)
    f[4, 4] = 80.003                       # near the water level precision must be fine
    p = tmp_path / "shore_field.r16f"
    csf.write_field(p, f, 80.0)
    back, welev = csf.read_field(p)
    assert p.stat().st_size == 16 + 9 * 9 * 2
    assert welev == 80.0
    assert np.allclose(back, f, atol=0.5)
    assert abs(back[4, 4] - 80.003) < 1e-4
