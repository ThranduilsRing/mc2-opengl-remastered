#!/usr/bin/env python3
"""TERRAIN-REAUTH-UNPIN-1 unit tests (pure functions, no .pak needed).

Synthetic oracle: a faceted pyramid island on a water plane — exactly the
artefact the user ruling targets ("smoothing the lines of a pyramid doesn't do
anything. we need terrain-aware multi-point smoothing/re-auth... UN PIN THE
CORNERS. KEEP THE SHAPE").
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_heightfield import (  # noqa: E402
    upsample_corner_pinned, reauth_visual, detect_coarse_extrema,
    build_object_damp, mountain_rock_mask, _crease_energy,
)

FACTOR = 4


def synth_pyramid(side: int = 48, peak_h: float = 120.0, slope: float = 14.0):
    """Coarse heightfield: flat plain at 0 with a centered faceted pyramid
    (chebyshev cone -> 4 planar facets + sharp creases + sharp summit)."""
    r = np.arange(side)
    cy = cx = side // 2
    cheb = np.maximum(np.abs(r[:, None] - cy), np.abs(r[None, :] - cx))
    elev = np.clip(peak_h - slope * cheb, 0.0, None).astype(np.float64)
    return elev, (cy, cx)


@pytest.fixture(scope="module")
def reauth_result():
    elev, peak = synth_pyramid()
    protect = np.zeros_like(elev, dtype=bool)
    work, info = reauth_visual(elev, FACTOR, protect, shape_tolerance=0.10,
                               max_drift=24.0, passes=150)
    return elev, peak, work, info


def test_corners_actually_unpinned(reauth_result):
    """The point of the slice: coarse vertices must MOVE (not corner-pinned)."""
    elev, _peak, work, info = reauth_result
    corner_move = np.abs(work[::FACTOR, ::FACTOR] - elev)
    assert corner_move.max() > 0.5, "corners still pinned - reauth did nothing"


def test_drift_bounded(reauth_result):
    """Soft bound: |out - bilinear| <= max_drift (+ extrema-restore headroom)."""
    elev, _peak, work, info = reauth_result
    base = upsample_corner_pinned(elev, FACTOR)
    assert np.abs(work - base).max() <= 24.0 * 1.25 + 1e-6
    assert info["drift_vs_bilinear_wu"]["mean"] <= 24.0


def test_peak_height_preserved(reauth_result):
    """Summit may move at most shape_tolerance (10%) of its local relief."""
    elev, (cy, cx), work, info = reauth_result
    h0 = elev[cy, cx]
    h1 = work[cy * FACTOR, cx * FACTOR]
    ex = [e for e in detect_coarse_extrema(elev) if (e["r"], e["c"]) == (cy, cx)]
    assert ex, "synthetic summit not detected as an extremum"
    assert abs(h1 - h0) <= 0.10 * ex[0]["relief"] + 1e-6
    assert info["extrema"]["violations"] == 0


def test_facet_creases_rounded(reauth_result):
    """Pyramid facet edges become curves: crease energy well below bilinear."""
    elev, _peak, work, info = reauth_result
    ce = info["facet_crease_energy"]
    assert ce["smoothed"] < ce["bilinear_base"] * 0.85


def test_landform_correlation(reauth_result):
    """KEEP THE SHAPE: coarse landform correlation stays ~1."""
    _elev, _peak, _work, info = reauth_result
    assert info["landform_correlation"] >= 0.99


def test_deterministic():
    elev, _ = synth_pyramid()
    protect = np.zeros_like(elev, dtype=bool)
    a, _ = reauth_visual(elev, FACTOR, protect, passes=30)
    b, _ = reauth_visual(elev, FACTOR, protect, passes=30)
    assert np.array_equal(a, b)


def test_protect_pin():
    """Protected (roads/buildings) cells stay ON the bilinear baseline."""
    elev, _ = synth_pyramid()
    protect = np.zeros_like(elev, dtype=bool)
    protect[10:14, 10:14] = True
    work, _ = reauth_visual(elev, FACTOR, protect, passes=60)
    base = upsample_corner_pinned(elev, FACTOR)
    # exact pin on the protected core (feather is outside it)
    from visual_heightfield import _fine_mask
    pin = _fine_mask(protect, FACTOR)
    assert np.abs(work[pin] - base[pin]).max() < 1e-9


# --- mountainify -------------------------------------------------------------

def test_mountainify_adds_detail_on_rock_only():
    elev, _ = synth_pyramid(side=48, peak_h=160.0, slope=18.0)  # steep faces
    protect = np.zeros_like(elev, dtype=bool)
    plain, _ = reauth_visual(elev, FACTOR, protect, passes=60)
    mtn, info = reauth_visual(elev, FACTOR, protect, passes=60,
                              mountainify_amp=14.0, seed=1337)
    diff = np.abs(mtn - plain)
    rock = mountain_rock_mask(plain, elev, FACTOR) > 0.35
    flat = mountain_rock_mask(plain, elev, FACTOR) < 0.05
    assert info["mountainify"]["detail_rms_on_rock_wu"] > 0.5
    assert diff[rock].mean() > 5.0 * max(1e-9, diff[flat].mean()), \
        "detail must concentrate on the rock channel, not the plains"


def test_mountainify_deterministic_and_seeded():
    elev, _ = synth_pyramid(side=48, peak_h=160.0, slope=18.0)
    protect = np.zeros_like(elev, dtype=bool)
    a, _ = reauth_visual(elev, FACTOR, protect, passes=30, mountainify_amp=14.0, seed=7)
    b, _ = reauth_visual(elev, FACTOR, protect, passes=30, mountainify_amp=14.0, seed=7)
    c, _ = reauth_visual(elev, FACTOR, protect, passes=30, mountainify_amp=14.0, seed=8)
    assert np.array_equal(a, b)
    assert not np.array_equal(a, c)


def test_mountainify_keeps_shape():
    elev, (cy, cx) = synth_pyramid(side=48, peak_h=160.0, slope=18.0)
    protect = np.zeros_like(elev, dtype=bool)
    work, info = reauth_visual(elev, FACTOR, protect, passes=60,
                               mountainify_amp=14.0, seed=1337)
    assert info["landform_correlation"] >= 0.99
    assert info["extrema"]["violations"] == 0
    base = upsample_corner_pinned(elev, FACTOR)
    assert np.abs(work - base).max() <= 24.0 * 1.25 + 1e-6


def test_mountainify_respects_water():
    elev, _ = synth_pyramid(side=48, peak_h=160.0, slope=18.0)
    water = elev <= 0.0
    protect = np.zeros_like(elev, dtype=bool)
    plain, _ = reauth_visual(elev, FACTOR, protect, passes=30, water_coarse=water)
    mtn, _ = reauth_visual(elev, FACTOR, protect, passes=30, water_coarse=water,
                           mountainify_amp=14.0, seed=1337)
    from visual_heightfield import _fine_mask
    wf = _fine_mask(water, FACTOR)
    # ridged detail itself is masked off water; only sub-0.1wu extrema-restore
    # Gaussian tails may differ (visually nothing).
    assert np.abs((mtn - plain)[wf]).max() < 0.1, "no ridged detail on water"


# --- object damp (engine Half-B seed) ----------------------------------------

def test_damp_zero_on_footprint_one_far():
    side = 32
    foot = np.zeros((side, side), dtype=bool)
    foot[16, 16] = True
    damp = build_object_damp(side, foot, radius_wu=256.0)
    assert damp[16, 16] == 0.0
    assert damp[17, 16] == 0.0            # 1-cell core dilation
    assert damp[0, 0] == 1.0              # far away: full displacement
    assert damp.min() >= 0.0 and damp.max() <= 1.0
    # monotone ramp along a ray
    ray = damp[16, 16:26]
    assert np.all(np.diff(ray) >= -1e-12)


def test_crease_energy_metric_sane():
    flat = np.zeros((32, 32))
    assert _crease_energy(flat) == 0.0
    ridge = np.abs(np.arange(32, dtype=np.float64) - 16)[None, :].repeat(32, axis=0)
    assert _crease_energy(ridge) > 0.0
