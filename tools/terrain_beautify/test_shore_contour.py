#!/usr/bin/env python3
"""SHORE-CONTOUR-1 unit tests (pure functions, no .pak needed).

Synthetic oracle: a coarse island whose bilinear upsample produces a BLOCKY
waterline (staircase where the coarse mesh crosses the water plane). The slice's
promise (user ruling, mc2_17 river): extract that blocky waterline, smooth it to
a continuous curve, and reshape the shore band so the terrain/water intersection
follows the smoothed curve. All render-only; deterministic (no RNG).
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_heightfield import (  # noqa: E402
    upsample_corner_pinned, extract_waterline_contours, smooth_contour,
    contour_facet_stats, shore_contour_reshape, _polyline_arclen,
    reauth_visual, WORLD_UNITS_PER_VERTEX,
)

FACTOR = 4
WATER = 100.0


def synth_island(side: int = 40, peak_h: float = 200.0):
    """Coarse heightfield: a chebyshev (square-terraced) hill above the water
    plane. Chebyshev distance makes the coarse mesh cross WATER on a BLOCKY,
    axis-aligned staircase ring — exactly the artefact the slice targets (the
    mc2_17 river's polygonal waterline), not a pre-smooth circle."""
    r = np.arange(side)
    cy = cx = side // 2
    cheb = np.maximum(np.abs(r[:, None] - cy), np.abs(r[None, :] - cx))
    elev = np.clip(peak_h - 13.0 * cheb.astype(np.float64), 0.0, None)
    return elev


@pytest.fixture(scope="module")
def island():
    elev = synth_island()
    base = upsample_corner_pinned(elev, FACTOR)
    return elev, base


def test_extract_finds_a_waterline(island):
    _elev, base = island
    contours = extract_waterline_contours(base, WATER)
    assert contours, "no waterline contour extracted from a crossing surface"
    total = sum(_polyline_arclen(c["pts"], c["closed"]) for c in contours)
    assert total > 0.0


def test_extract_is_deterministic(island):
    """Marching squares + walk must be byte-stable (bake reproducibility)."""
    _elev, base = island
    a = extract_waterline_contours(base, WATER)
    b = extract_waterline_contours(base, WATER)
    assert len(a) == len(b)
    for ca, cb in zip(a, b):
        assert ca["closed"] == cb["closed"]
        assert np.array_equal(ca["pts"], cb["pts"])


def test_smoothing_reduces_corners(island):
    """Arc-length Gaussian smoothing must LOWER the corner census (no facet
    < ~1-2 cells survives) while keeping the contour length ~stable."""
    _elev, base = island
    cell = WORLD_UNITS_PER_VERTEX / FACTOR
    keep = [c for c in extract_waterline_contours(base, WATER)
            if _polyline_arclen(c["pts"], c["closed"]) >= 3.0 * FACTOR]
    assert keep
    before = contour_facet_stats(keep, cell)
    sm = [{"pts": smooth_contour(c["pts"], c["closed"], 3.5 * FACTOR, 0.5),
           "closed": c["closed"]} for c in keep]
    after = contour_facet_stats(sm, cell)
    assert after["corner_count"] <= before["corner_count"]
    assert after["median_facet_wu"] >= before["median_facet_wu"]
    # length preserved within a modest band (smoothing a convex ring shrinks it
    # slightly; a collapse would signal the sigma cap failed).
    assert after["total_len_wu"] >= 0.75 * before["total_len_wu"]


def test_smooth_contour_deterministic(island):
    _elev, base = island
    keep = [c for c in extract_waterline_contours(base, WATER)
            if _polyline_arclen(c["pts"], c["closed"]) >= 3.0 * FACTOR]
    c = keep[0]
    a = smooth_contour(c["pts"], c["closed"], 3.5 * FACTOR, 0.5)
    b = smooth_contour(c["pts"], c["closed"], 3.5 * FACTOR, 0.5)
    assert np.array_equal(a, b)


def test_reshape_moves_waterline_toward_smoothed_curve(island):
    """The deliverable metric: the reshaped surface's true waterline sits
    CLOSER to the smoothed target curve than the blocky bilinear one."""
    from scipy.ndimage import distance_transform_edt
    _elev, base = island
    cell = WORLD_UNITS_PER_VERTEX / FACTOR
    keep = [c for c in extract_waterline_contours(base, WATER)
            if _polyline_arclen(c["pts"], c["closed"]) >= 3.0 * FACTOR]
    P = np.vstack([smooth_contour(c["pts"], c["closed"], 3.5 * FACTOR, 0.5)
                   for c in keep])
    V = base.shape[0]
    occ = np.zeros((V, V), bool)
    occ[np.clip(np.round(P[:, 0]).astype(int), 0, V - 1),
        np.clip(np.round(P[:, 1]).astype(int), 0, V - 1)] = True
    dist = distance_transform_edt(~occ)

    def mean_dist(surf):
        cs = [c for c in extract_waterline_contours(surf, WATER)
              if _polyline_arclen(c["pts"], c["closed"]) >= 3.0 * FACTOR]
        pts = np.vstack([c["pts"] for c in cs])
        d = dist[np.clip(np.round(pts[:, 0]).astype(int), 0, V - 1),
                 np.clip(np.round(pts[:, 1]).astype(int), 0, V - 1)]
        return float(d.mean()) * cell

    out, w, info = shore_contour_reshape(base, FACTOR, WATER,
                                         smooth_radius_cells=3.5)
    assert w is not None and info is not None
    assert mean_dist(out) < mean_dist(base), \
        "reshaped waterline is not closer to the smoothed curve"


def test_reshape_is_deterministic(island):
    _elev, base = island
    o1, w1, i1 = shore_contour_reshape(base, FACTOR, WATER,
                                       smooth_radius_cells=3.5)
    o2, w2, i2 = shore_contour_reshape(base, FACTOR, WATER,
                                       smooth_radius_cells=3.5)
    assert np.array_equal(o1, o2)
    assert np.array_equal(w1, w2)
    assert i1["bank_adjust_wu"]["max"] == i2["bank_adjust_wu"]["max"]


def test_reshape_bank_adjust_bounded(island):
    """Bank adjustment is tanh-bounded by max_adjust (cliff-drift class)."""
    _elev, base = island
    max_adj = 48.0
    out, _w, info = shore_contour_reshape(base, FACTOR, WATER,
                                          smooth_radius_cells=3.5,
                                          max_adjust=max_adj)
    assert info["bank_adjust_wu"]["max"] <= max_adj + 1e-6
    assert np.abs(out - base).max() <= max_adj + 1e-6


def test_reshape_only_touches_the_band(island):
    """Cells far from the shore (well outside +/- band_cells) are untouched:
    the reshape is band-limited, not a global move."""
    _elev, base = island
    out, w, _info = shore_contour_reshape(base, FACTOR, WATER,
                                          smooth_radius_cells=3.5,
                                          band_cells=1.5)
    far = w <= 1e-4
    assert far.any()
    assert np.allclose(out[far], base[far], atol=1e-6)


def test_no_water_returns_noop():
    """A surface entirely above the water plane has no waterline: no-op."""
    elev = np.full((40, 40), 500.0)
    base = upsample_corner_pinned(elev, FACTOR)
    out, w, info = shore_contour_reshape(base, FACTOR, WATER)
    assert w is None and info is None
    assert np.array_equal(out, base)


def test_full_reauth_shore_stage_composes(island):
    """End-to-end: reauth with the shore stage ON runs, reports shore info,
    keeps extrema violations at zero (the compose order preserves the gate)."""
    elev, _base = island
    protect = np.zeros_like(elev, dtype=bool)
    water_coarse = elev <= WATER
    work, info = reauth_visual(
        elev, FACTOR, protect, water_coarse=water_coarse,
        water_elev=WATER, shore_smooth_radius=3.5, mountainify_amp=14.0)
    assert info["shore"] is not None
    assert info["shore"]["contours_processed"] >= 1
    assert info["extrema"]["violations"] == 0
    # the honest deliverable metric is recorded
    assert "waterline_to_target_wu" in info["shore"]
