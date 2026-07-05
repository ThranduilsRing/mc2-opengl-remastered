#!/usr/bin/env python3
"""TERRAIN-LOD-GEOMORPH-1 mips staleness-guard unit tests (pure functions).

Mirrors the engine loader guard in mclib/terrain.cpp
("TERRAIN-LOD-GEOMORPH-1 STALENESS GUARD"): every max-mip level is a MAX over a
footprint that INCLUDES the coarse vertex's own fine sample, so
`mip[L][v] >= fineCorner[v]` for a CONSISTENT bake. A stale mips sidecar (built
from a DIFFERENT fine array) violates this — that violation is exactly what the
guard drops on.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_heightfield import (  # noqa: E402
    build_max_mips, mips_corner_floor_check, MIP_STRIDES,
)

FACTOR = 4


def synth_fine(side: int = 40, seed: int = 11) -> np.ndarray:
    """A fine (V x V) visual heightfield with sharp ridges/valleys — the
    silhouette content the mips exist to preserve. V = (side-1)*FACTOR + 1."""
    V = (side - 1) * FACTOR + 1
    r = np.arange(V)
    # crossed ridges + a corner plateau + noise: lots of between-vertex maxima.
    z = (60.0 * np.abs(np.sin(r[:, None] / 9.0))
         + 40.0 * np.abs(np.cos(r[None, :] / 7.0)))
    rng = np.random.default_rng(seed)
    z = z + rng.uniform(0.0, 8.0, size=z.shape)
    return z.astype(np.float64)


def test_blob_shape_and_strides():
    fine = synth_fine()
    side = 40
    mm = build_max_mips(fine, FACTOR, side)
    assert mm["strides"] == list(MIP_STRIDES)
    assert mm["blob"].size == len(MIP_STRIDES) * side * side
    assert mm["blob"].dtype == np.dtype("<f4")


def test_fresh_mips_satisfy_corner_floor():
    """Consistent bake: NO level ever dips below the fine corner sample."""
    fine = synth_fine()
    side = 40
    mm = build_max_mips(fine, FACTOR, side)
    chk = mips_corner_floor_check(fine, FACTOR, side, mm["blob"])
    assert chk["levels"] == len(MIP_STRIDES)
    assert chk["violations"] == 0, chk
    assert chk["worst_deficit_wu"] == 0.0


def test_lift_is_nonnegative_and_present():
    """Mips must actually lift (silhouette-preservation), never lower."""
    fine = synth_fine()
    side = 40
    mm = build_max_mips(fine, FACTOR, side)
    corners = fine[::FACTOR, ::FACTOR][:side, :side]
    plane = side * side
    total_lift = 0.0
    for lv in range(len(MIP_STRIDES)):
        level = mm["blob"][lv * plane:(lv + 1) * plane].reshape(side, side)
        assert (level >= corners - 1e-3).all(), "a mip dipped below its corner"
        total_lift += float((level - corners).sum())
    assert total_lift > 0.0, "ridged input must produce a positive max-lift"


def test_stale_cross_bake_is_caught():
    """The landmine: mips from bake A checked against a DIFFERENT fine bake B
    (same side, so the engine SIZE check passes) must be flagged stale."""
    side = 40
    fine_a = synth_fine(seed=1)
    fine_b = synth_fine(seed=999)          # different surface, identical dims
    mm_a = build_max_mips(fine_a, FACTOR, side)
    chk = mips_corner_floor_check(fine_b, FACTOR, side, mm_a["blob"])
    assert chk["violations"] > 0, "stale cross-bake mips slipped past the guard"
    assert chk["worst_deficit_wu"] > 0.05


def test_tolerance_absorbs_f32_roundtrip():
    """A genuine bake round-tripped through the on-disk <f4 blob still passes
    (float32 max-cast rounding must not trip the guard)."""
    fine = synth_fine(seed=7)
    side = 40
    mm = build_max_mips(fine, FACTOR, side)
    # simulate the on-disk sidecar: write/read as little-endian float32 bytes
    roundtrip = np.frombuffer(mm["blob"].tobytes(), dtype="<f4")
    chk = mips_corner_floor_check(fine, FACTOR, side, roundtrip)
    assert chk["violations"] == 0, chk
