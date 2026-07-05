#!/usr/bin/env python3
"""Terrain Surface Truth Arc — density_policy_sim unit tests (pure, engine-free)."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import terrain_importance as ti  # noqa: E402
from density_policy_sim import (  # noqa: E402
    block_grid, _blocks, block_triangles, total_triangles, reconstruct,
    block_importance, distance_policy, importance_rebalance, compare, LOD_STEPS,
)
import skyline_oracle as so  # noqa: E402


def _corner_cliff(n=64, h=120.0):
    """Flat map with a localized steep cliff feature in ONE corner region, so a
    good policy should concentrate vertices there."""
    z = np.zeros((n, n))
    x = np.linspace(0, 3 * np.pi, n)
    # a compact ridge cluster in the top-left quadrant
    z[4:28, 4:28] = h * np.abs(np.sin(x[:24]))[:, None] * np.abs(np.cos(x[:24]))[None, :]
    return z


# --- grid / counting --------------------------------------------------------
def test_block_grid_partitions():
    g = block_grid(64, 16)
    assert g[0] == (0, 16) and g[-1][1] == 64
    assert all(b <= a2 for (_, b), (a2, _) in zip(g, g[1:]))


def test_triangle_count_monotone_in_stride():
    n = 64
    blocks = _blocks(n, n, 16)
    fine = np.zeros((n, n))
    coarse_all = total_triangles(fine, blocks, np.full(len(blocks), 3))
    fine_all = total_triangles(fine, blocks, np.zeros(len(blocks), dtype=int))
    assert fine_all > coarse_all               # finer LOD = more triangles


def test_reconstruct_uniform_stride_matches_oracle_ish():
    """All-one-block at uniform stride reconstructs ~ the oracle's whole-map
    decimate (block covers the map)."""
    n = 41
    x = np.linspace(0, 4 * np.pi, n)
    fine = 20 * np.sin(x)[:, None] + 15 * np.cos(x)[None, :]
    blocks = _blocks(n, n, n)                   # single block = whole map
    lod = np.array([2])                         # LOD_STEPS[2] == 4
    rec = reconstruct(fine, blocks, lod)
    assert np.allclose(rec, so.decimate_upsample(fine, 4), atol=1e-9)


# --- policies ---------------------------------------------------------------
def test_distance_policy_coarsens_with_distance():
    n = 96
    blocks = _blocks(n, n, 16)
    thr = np.linspace(10, 80, len(LOD_STEPS) - 1)
    lod = distance_policy(np.zeros((n, n)), blocks, (0.0, 0.0), thr)
    # block nearest the camera corner must be at least as fine as the farthest
    centers = [np.hypot(0.5 * (c0 + c1), 0.5 * (r0 + r1))
               for (r0, r1, c0, c1) in blocks]
    near = int(np.argmin(centers)); far = int(np.argmax(centers))
    assert lod[near] <= lod[far]


def test_rebalance_keeps_budget():
    n = 64
    fine = _corner_cliff(n)
    imp = ti.compute_fields(fine)["importance"]
    blocks = _blocks(n, n, 16)
    thr = np.linspace(np.hypot(n, n) * 0.12, np.hypot(n, n) * 0.75, len(LOD_STEPS) - 1)
    lod0 = distance_policy(fine, blocks, (n / 2, n / 2), thr)
    impb = block_importance(imp, blocks)
    lod1 = importance_rebalance(fine, blocks, lod0, impb)
    b0 = total_triangles(fine, blocks, lod0)
    b1 = total_triangles(fine, blocks, lod1)
    assert b1 <= b0                            # never exceeds the budget


# --- headline experiment ----------------------------------------------------
def test_importance_policy_helps_on_localized_cliff():
    """The central bet: at equal budget, importance-biased allocation should not
    be worse, and should improve at least one silhouette/cliff metric on a map
    with a compact high-importance feature."""
    fine = _corner_cliff(64, h=120.0)
    imp = ti.compute_fields(fine)["importance"]
    res = compare(fine, imp, block=16)
    assert res["triangles"]["importance"] <= res["triangles"]["distance"] * 1.02
    # at equal budget, no headline metric should get materially worse...
    d = res["delta_importance_minus_distance"]
    assert all(v <= 1e-6 for v in d.values())
    # ...and at least one should improve (or the feature was already covered)
    assert res["metrics_improved"] >= 1


def test_compare_report_shape():
    fine = _corner_cliff(48)
    imp = ti.compute_fields(fine)["importance"]
    res = compare(fine, imp, block=16)
    for k in ("triangles", "distance", "importance",
              "delta_importance_minus_distance", "GO"):
        assert k in res
