#!/usr/bin/env python3
"""Terrain Surface Truth Arc — terrain_importance (Layer-1 fields) unit tests.

Synthetic surfaces with known answers, plus a full .beauty round-trip through
the CLI bake. Pure/engine-free.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from terrain_importance import (  # noqa: E402
    compute_fields, ridge_prominence, reduction_error, pack_blob, unpack_blob,
    field_stats, bake_importance, CHANNELS, MIP_STRIDES,
)

FACTOR = 4


def _fine(n=41, seed=4):
    x = np.linspace(0, 6 * np.pi, n)
    rng = np.random.default_rng(seed)
    return 30 * np.sin(x)[:, None] + 25 * np.cos(1.3 * x)[None, :] \
        + rng.uniform(0, 4, (n, n))


def _ramp(n=40, k=2.0):
    """Linear ramp: nonzero slope, ZERO curvature (Laplacian of a plane=0)."""
    return (k * np.arange(n))[None, :].repeat(n, 0).astype(float)


def _step(n=40, h=100.0):
    z = np.zeros((n, n)); z[n // 2:, :] = h
    return z


# --- field correctness ------------------------------------------------------
def test_plane_all_zero_fields():
    flat = np.full((30, 30), 5.0)
    f = compute_fields(flat)
    for c in ("slope_deg", "curvature", "ridge_prom", "cliff_mask",
              "reduction_err", "importance"):
        assert np.allclose(f[c], 0.0), c


def test_ramp_has_slope_but_no_curvature():
    f = compute_fields(_ramp(), wu_per_texel=1.0)
    assert f["slope_deg"].max() > 30.0       # steep ramp
    # interior curvature of a linear ramp is ~0 (edges may be tiny from padding)
    assert np.abs(f["curvature"][2:-2, 2:-2]).max() < 1e-6


def test_step_flags_cliff_and_reduction_error():
    f = compute_fields(_step(n=40, h=100.0), wu_per_texel=1.0,
                       slope_thresh_deg=40.0)
    assert f["cliff_mask"].sum() > 0
    # the vertical step is exactly what geo-mip strides smear -> big red-err
    assert f["reduction_err"].max() > 20.0


def test_ridge_prominence_only_on_crest():
    z = np.zeros((21, 21)); z[:, 11] = 40.0
    prom = ridge_prominence(z, prominence_wu=1.0)
    assert prom[:, 11].min() > 30.0          # crest carries prominence
    assert prom[:, 0].max() == 0.0           # plain carries none


def test_reduction_error_zero_on_plane_positive_on_ridge():
    flat = np.full((41, 41), 3.0)
    assert reduction_error(flat).max() == pytest.approx(0.0)
    ridge = np.zeros((41, 41)); ridge[:, 21] = 100.0   # off-lattice col
    assert reduction_error(ridge).max() > 30.0


def test_importance_in_unit_range_and_higher_on_cliff():
    f = compute_fields(_step(n=40, h=100.0), wu_per_texel=1.0,
                       slope_thresh_deg=40.0)
    imp = f["importance"]
    assert imp.min() >= 0.0 and imp.max() <= 1.0
    cliff = f["cliff_mask"] > 0.5
    assert imp[cliff].mean() > imp[~cliff].mean()


# --- pack / unpack ----------------------------------------------------------
def test_pack_unpack_roundtrip():
    h = _fine(n=41)
    side = 41
    f = compute_fields(h)
    blob = pack_blob(f)
    assert blob.dtype == np.dtype("<f4")
    assert blob.size == len(CHANNELS) * side * side
    back = unpack_blob(blob, side)
    for c in CHANNELS:
        assert np.allclose(back[c], f[c].astype("<f4"), atol=1e-4), c


def test_field_stats_keys():
    f = compute_fields(_fine(n=32))
    st = field_stats(f)
    assert set(st) == set(CHANNELS)
    for c in CHANNELS:
        assert {"min", "mean", "max"} <= set(st[c])


# --- full .beauty CLI round-trip -------------------------------------------
def test_bake_importance_roundtrip(tmp_path):
    """Write a fine bake + report, run bake_importance, verify sidecar + report
    + that the packed blob round-trips to the recomputed fields."""
    side = 21
    v = (side - 1) * FACTOR + 1                 # 81
    x = np.linspace(0, 4 * np.pi, v)
    fine = (50 * np.sin(x)[:, None] + 40 * np.cos(x)[None, :]).astype("<f4")
    beauty = tmp_path / "synth.beauty"
    beauty.mkdir()
    fine.tofile(beauty / f"visual_height_{FACTOR}x.r32")
    (beauty / "visual_height_report.json").write_text(json.dumps({
        "factor": FACTOR, "world_units_per_vertex_visual": 1.0}), encoding="utf-8")

    report, written = bake_importance(beauty, factor=FACTOR)
    assert (beauty / "visual_importance.r32").exists()
    assert report["grid_v"] == v and report["coarse_side"] == side
    assert report["channels"] == list(CHANNELS)
    assert len(report["built_from_sha256"]) == 64

    blob = np.fromfile(beauty / "visual_importance.r32", dtype="<f4")
    assert blob.size == len(CHANNELS) * v * v
    back = unpack_blob(blob, v)
    recomputed = compute_fields(fine.astype(np.float64), wu_per_texel=1.0)
    assert np.allclose(back["importance"], recomputed["importance"].astype("<f4"),
                       atol=1e-4)


def test_bake_missing_fine_raises(tmp_path):
    beauty = tmp_path / "empty.beauty"
    beauty.mkdir()
    with pytest.raises(FileNotFoundError):
        bake_importance(beauty, factor=FACTOR)
