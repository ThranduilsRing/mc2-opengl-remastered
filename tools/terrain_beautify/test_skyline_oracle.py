#!/usr/bin/env python3
"""Terrain Surface Truth Arc — skyline_oracle unit tests (pure, engine-free).

Synthetic surfaces with KNOWN answers, so the acceptance harness is itself
verified before any slice is judged by it.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skyline_oracle import (  # noqa: E402
    decimate_upsample, morph_surface, skyline_profiles, skyline_error,
    silhouette_deviation, ridge_mask, ridge_loss, slope_deg, cliff_mask,
    cliff_height_loss, curvature, curvature_coverage, evaluate, VIEW_DIRS,
)


def _thin_ridge(n=41, h=100.0, col=21):
    """Flat plain with a single one-cell-wide ridge line at `col`. Default col
    21 is OFF the stride-2 and stride-4 lattices (odd, not a multiple of 4), so
    decimation genuinely drops it -- a ridge sitting on a kept vertex would be
    preserved and prove nothing."""
    z = np.zeros((n, n))
    z[:, col] = h
    return z


def _tent(n=41, h=100.0, col=21, half=3):
    """A finite-width triangular ridge, so a finer stride reconstructs MORE of
    the peak than a coarser one (a 1-cell ridge is all-or-nothing)."""
    c = np.arange(n)
    w = np.maximum(0.0, 1.0 - np.abs(c - col) / half)
    return np.tile(h * w, (n, 1))


def _cliff_step(n=40, h=100.0):
    """Half-height step: a vertical cliff across the middle."""
    z = np.zeros((n, n))
    z[n // 2:, :] = h
    return z


def _bumpy(n=48, seed=3):
    rng = np.random.default_rng(seed)
    x = np.linspace(0, 6 * np.pi, n)
    z = 30 * np.sin(x)[:, None] + 25 * np.cos(x)[None, :]
    return z + rng.uniform(0, 5, size=(n, n))


# --- reconstruction ---------------------------------------------------------
def test_stride1_is_identity():
    h = _bumpy()
    assert np.allclose(decimate_upsample(h, 1), h)


def test_lattice_values_preserved():
    """Kept vertices keep their exact height; only spans are interpolated."""
    h = _bumpy(n=41)
    up = decimate_upsample(h, 4)
    assert np.allclose(up[::4, ::4], h[::4, ::4], atol=1e-9)


def test_coarse_drops_thin_ridge():
    """A stride that steps over the 1-cell ridge cannot represent it."""
    h = _thin_ridge(n=41, h=100.0, col=21)
    up = decimate_upsample(h, 4)
    # the ridge column peak is lost (bilinear between two zero lattice lines)
    assert up[:, 21].max() < 60.0


def test_morph_endpoints_and_monotone():
    h = _bumpy(n=41)
    fine = morph_surface(h, 2, 4, 0.0)
    coarse = morph_surface(h, 2, 4, 1.0)
    mid = morph_surface(h, 2, 4, 0.5)
    assert np.allclose(fine, decimate_upsample(h, 2))
    assert np.allclose(coarse, decimate_upsample(h, 4))
    assert np.allclose(mid, 0.5 * (fine + coarse))


# --- skyline ----------------------------------------------------------------
def test_skyline_profiles_shapes():
    h = _bumpy(n=32)
    p = skyline_profiles(h)
    assert set(p) == set(VIEW_DIRS)
    assert p["ns"].shape == (32,)
    assert p["ew"].shape == (32,)
    assert p["diag_sum"].shape == (63,)


def test_skyline_ns_is_column_max():
    h = _bumpy(n=20)
    assert np.allclose(skyline_profiles(h)["ns"], h.max(axis=0))


def test_identity_has_zero_error():
    h = _bumpy()
    err = skyline_error(h, h)
    for d, m in err.items():
        assert m["drop_max"] == 0.0 and m["inflation_max"] == 0.0
    assert silhouette_deviation(h, h)["l1_mean"] == 0.0


def test_ridge_drop_shrinks_with_finer_stride():
    """Finer reconstruction preserves more of a finite-width ridge's silhouette.
    Use the aggregate silhouette deviation (robust) rather than a single
    direction's drop_max, which can tie when both strides share the nearest
    kept sample."""
    h = _tent(n=41, h=100.0, col=22, half=3)
    dev_coarse = silhouette_deviation(h, decimate_upsample(h, 5))["l1_mean"]
    dev_fine = silhouette_deviation(h, decimate_upsample(h, 2))["l1_mean"]
    assert dev_coarse > dev_fine >= 0.0


# --- ridge / cliff / curvature ---------------------------------------------
def test_ridge_mask_finds_the_ridge_only():
    h = _thin_ridge(n=21, h=50.0, col=11)
    m = ridge_mask(h, prominence_wu=1.0)
    assert m[:, 11].all()                      # ridge column flagged
    assert not m[:, 0].any() and not m[:, 5].any()   # plain not flagged


def test_flat_has_no_ridge_no_cliff():
    flat = np.full((30, 30), 7.0)
    assert not ridge_mask(flat).any()
    assert not cliff_mask(flat).any()
    assert ridge_loss(flat, flat)["cells"] == 0
    assert curvature_coverage(flat, flat)["fidelity"] == 1.0


def test_ridge_loss_positive_when_ridge_dropped():
    h = _thin_ridge(n=41, h=100.0, col=21)
    cand = decimate_upsample(h, 4)
    rl = ridge_loss(h, cand)
    assert rl["cells"] > 0 and rl["max"] > 30.0


def test_cliff_mask_flags_the_step():
    h = _cliff_step(n=40, h=100.0)
    cm = cliff_mask(h, wu_per_texel=1.0, slope_thresh_deg=40.0)
    assert cm.any()
    # the step is around the middle rows, not at the flat top/bottom
    assert cm[18:22, :].any()
    assert not cm[0:5, :].any()


def test_cliff_height_loss_when_smoothed():
    h = _cliff_step(n=40, h=100.0)
    smoothed = decimate_upsample(h, 5)     # blurs the vertical step
    chl = cliff_height_loss(h, smoothed, slope_thresh_deg=40.0)
    assert chl["cells"] > 0 and chl["max"] > 5.0


def test_curvature_coverage_drops_on_oversmooth():
    h = _bumpy(seed=9)
    smoothed = decimate_upsample(h, 6)
    cov = curvature_coverage(h, smoothed)
    assert cov["energy_ratio"] < 0.95        # detail lost
    assert cov["fidelity"] < 1.0
    # identity retains everything
    assert curvature_coverage(h, h)["energy_ratio"] == pytest.approx(1.0)


# --- top level --------------------------------------------------------------
def test_evaluate_bundle_keys_and_shape_guard():
    h = _bumpy(n=32)
    res = evaluate(h, decimate_upsample(h, 4))
    for k in ("skyline", "silhouette", "ridge_loss", "cliff_height_loss",
              "curvature_coverage"):
        assert k in res
    with pytest.raises(ValueError):
        evaluate(h, h[:-1, :])


def test_finer_candidate_scores_better_overall():
    """A finer reconstruction must not score WORSE on any headline metric."""
    h = _bumpy(n=41, seed=5)
    fine = evaluate(h, decimate_upsample(h, 2))
    coarse = evaluate(h, decimate_upsample(h, 5))
    assert fine["ridge_loss"]["mean"] <= coarse["ridge_loss"]["mean"] + 1e-9
    assert (fine["silhouette"]["l1_mean"]
            <= coarse["silhouette"]["l1_mean"] + 1e-9)
    assert (fine["curvature_coverage"]["fidelity"]
            >= coarse["curvature_coverage"]["fidelity"] - 1e-9)
