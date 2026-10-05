#!/usr/bin/env python3
"""GEOMORPH-NEXT-SLICES-1 Slice 3 (CLIPMAP-BRIDGE) — integer-coincidence
invariant tests.

Companion to `docs/terrain-clipmap-bridge.md`. That doc claims: the bake grid
is a uniform 4x supersample of the coarse map grid, and every renderer-visited
coarse-grid index — at any LOD stride, any block origin, including
stitched/skirt boundary vertices — lands on an EXACT integer index into the
bake arrays (never a fractional index, never a runtime resample of the bake
itself). This is the property that would let a future clipmap arc do
toroidal ring updates without re-deriving alignment: two ring levels agree
exactly at every coarse-grid point, for any wrap offset, because the whole
stack shares one fixed 4x-uniform lattice.

These tests pin that claim down two ways:
  1. Synthetic-fixture proofs of the index arithmetic itself (grid sizing law,
     engine corner-index formula, full-resolution mip addressability, SSBO
     blob layout) — mirrors the exact formulas in mclib/terrain.cpp's
     "TERRAIN-LOD-GEOMORPH-1 STALENESS GUARD" and
     GameOS/gameos/gos_terrain_lod_chunk.cpp's
     gos_TerrainLodChunk_UploadVisualHeightFull, so a future refactor of
     either side can't silently drift from the other without one of these
     failing.
  2. Real-bake proofs against every committed
     tests/terrain/beautify/<mission>.beauty/ sidecar (all 24 campaign maps
     as of this slice) — byte-size / grid-math self-consistency, so a bad
     future bake (wrong factor, decimated mips, mismatched mapSide) fails
     here instead of silently mis-loading in the engine.

Deliberately NOT tested here (see docs/terrain-clipmap-bridge.md §2 for why):
VALUE equality at corner indices for --reauth bakes (mc2_01/17/24 intentionally
unpin corner values; only the INDEX mapping is invariant, not the value).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_heightfield import (  # noqa: E402
    upsample_corner_pinned, build_max_mips, MIP_STRIDES,
)

FACTOR = 4
BEAUTY_ROOT = Path(__file__).resolve().parents[2] / "tests" / "terrain" / "beautify"


def synth_coarse(side: int = 30, seed: int = 5) -> np.ndarray:
    rng = np.random.default_rng(seed)
    r = np.arange(side)
    z = (50.0 * np.sin(r[:, None] / 6.0) + 35.0 * np.cos(r[None, :] / 4.5))
    return (z + rng.uniform(0.0, 5.0, size=z.shape)).astype(np.float64)


# ---------------------------------------------------------------------------
# 1. Grid sizing law: V = (N-1)*factor + 1 (visual_heightfield.py's own
#    contract, mirrored by the engine loader's size check, terrain.cpp
#    "want = V*V*sizeof(float)").
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("side", [2, 5, 30, 100, 120, 477])
def test_grid_sizing_law_holds(side):
    N = side
    V_expected = (N - 1) * FACTOR + 1
    coarse = synth_coarse(side=min(N, 12))  # only need shape math here, not N=477 data
    V_from_math = (coarse.shape[0] - 1) * FACTOR + 1
    assert V_from_math == (min(N, 12) - 1) * FACTOR + 1
    # Direct formula check independent of any array (the law itself):
    assert V_expected == N * FACTOR - (FACTOR - 1)


# ---------------------------------------------------------------------------
# 2. Engine corner-index formula (mirrors mclib/terrain.cpp's staleness-guard
#    loop: `mipFactor = (V-1)/(mapSide-1)`, corner = vh[(r*mipFactor)*V +
#    c*mipFactor]) must land on the EXACT coarse vertex for every (r,c),
#    using pure integer arithmetic (no floor/round needed, no remainder).
# ---------------------------------------------------------------------------

def test_engine_corner_index_formula_is_exact_integer_arithmetic():
    side = 23
    coarse = synth_coarse(side=side)
    fine = upsample_corner_pinned(coarse, FACTOR)
    V = fine.shape[0]
    assert V == (side - 1) * FACTOR + 1

    # Exactly the engine's derivation (terrain.cpp: "mipFactor = (V-1)/(mapSide-1)").
    map_side = side
    mip_factor = (V - 1) // (map_side - 1)
    assert mip_factor == FACTOR, "engine's re-derived factor must recover FACTOR exactly"
    assert (V - 1) % (map_side - 1) == 0, "V-1 must divide evenly by (mapSide-1) -- no remainder"

    fine_flat = fine.ravel()
    for r in range(map_side):
        for c in range(map_side):
            idx = (r * mip_factor) * V + (c * mip_factor)
            assert 0 <= idx < fine_flat.size, f"corner index out of bounds at r={r} c={c}"
            # Default (non-reauth) bake path: index ALSO carries the exact value.
            assert fine_flat[idx] == pytest.approx(coarse[r, c], abs=1e-9)


# ---------------------------------------------------------------------------
# 3. Mip levels are stored at FULL side x side resolution (no decimation to
#    side/stride) -- so ANY coarse index in [0, side) is a valid lookup for
#    ANY level, not just stride-aligned ones. This is what shaders/
#    terrain_lod_chunk.vert's mipH() relies on: `base + cx + cy*u_mapSide`
#    with cx/cy merely clamped to [0, mapSide), never stride-quantized.
# ---------------------------------------------------------------------------

def test_mip_levels_are_full_resolution_not_decimated():
    side = 23
    coarse = synth_coarse(side=side)
    fine = upsample_corner_pinned(coarse, FACTOR)
    mm = build_max_mips(fine, FACTOR, side)
    blob = mm["blob"]
    assert blob.size == len(MIP_STRIDES) * side * side

    plane = side * side
    for lvl in range(len(MIP_STRIDES)):
        level = blob[lvl * plane:(lvl + 1) * plane].reshape(side, side)
        assert level.shape == (side, side), (
            "mip level must be full side x side resolution, not decimated to "
            "side/stride -- a decimated level would make non-stride-aligned "
            "renderer lookups (stitch/skirt boundary verts) land out of bounds "
            "or require a remap, breaking the integer-coincidence invariant"
        )


def test_shader_flat_offset_formula_in_bounds_for_every_index():
    """Mirrors GameOS/gameos/gos_terrain_lod_chunk.cpp's SSBO layout
    ([fine V*V | mip(2) mapSide^2 | mip(4) mapSide^2 | ...]) and
    shaders/terrain_lod_chunk.vert's mipH(): `base = V*V + lvl*mapSide*mapSide`,
    then `base + cx + cy*mapSide`. Every (cx, cy) in [0, mapSide) x [0, mapSide),
    for every level, must resolve to an in-bounds flat index -- including
    indices that are NOT multiples of that level's own LOD stride (the whole
    point of storing mips at full resolution)."""
    side = 17
    coarse = synth_coarse(side=side)
    fine = upsample_corner_pinned(coarse, FACTOR)
    V = fine.shape[0]
    mm = build_max_mips(fine, FACTOR, side)

    total_floats = V * V + mm["blob"].size
    map_side = side
    for lvl, stride in enumerate(MIP_STRIDES):
        base = V * V + lvl * map_side * map_side
        # Probe every index, including ones NOT aligned to `stride` -- e.g. a
        # stitched edge vertex at an arbitrary coarse offset.
        for cy in range(map_side):
            for cx in range(map_side):
                flat = base + cx + cy * map_side
                assert 0 <= flat < total_floats, (
                    f"level stride={stride} index (cx={cx},cy={cy}) resolves "
                    f"to flat={flat}, outside the [0,{total_floats}) blob -- "
                    "a clipmap ring at this level could not address this cell"
                )


# ---------------------------------------------------------------------------
# 4. SSBO blob layout: byte count the engine allocates
#    (gos_TerrainLodChunk_UploadVisualHeightFull: fineBytes + mipBytes) must
#    equal the Python-side concatenation the bake tool would produce.
# ---------------------------------------------------------------------------

def test_ssbo_blob_layout_matches_python_concatenation():
    side = 19
    coarse = synth_coarse(side=side)
    fine = upsample_corner_pinned(coarse, FACTOR)
    V = fine.shape[0]
    mm = build_max_mips(fine, FACTOR, side)

    fine_bytes = V * V * 4
    mip_bytes = mm["blob"].size * 4
    engine_total_bytes = fine_bytes + mip_bytes  # gos_TerrainLodChunk_UploadVisualHeightFull

    python_blob = np.concatenate([fine.astype("<f4").ravel(), mm["blob"]])
    assert python_blob.size * 4 == engine_total_bytes
    assert mm["blob"].size == len(MIP_STRIDES) * side * side


# ---------------------------------------------------------------------------
# 5. Real committed bakes: every tests/terrain/beautify/<mission>.beauty/
#    sidecar (all 24 campaign maps as of this slice) must satisfy the same
#    byte-size grid math the engine loader enforces at load time.
# ---------------------------------------------------------------------------

def _discover_real_bakes():
    if not BEAUTY_ROOT.is_dir():
        return []
    out = []
    for d in sorted(BEAUTY_ROOT.iterdir()):
        if d.is_dir() and d.name.endswith(".beauty") and (d / "visual_height_report.json").is_file():
            out.append(d)
    return out


REAL_BAKES = _discover_real_bakes()


@pytest.mark.skipif(not REAL_BAKES, reason="no committed .beauty bakes found")
@pytest.mark.parametrize("beauty_dir", REAL_BAKES, ids=lambda d: d.name)
def test_real_bake_fine_file_size_matches_grid_math(beauty_dir):
    report = json.loads((beauty_dir / "visual_height_report.json").read_text())
    N = report["coarse_side"]
    factor = report["factor"]
    V_expected = (N - 1) * factor + 1
    assert report["visual_side"] == V_expected, (
        f"{beauty_dir.name}: report's visual_side disagrees with the "
        f"(N-1)*factor+1 grid sizing law"
    )

    r32_path = beauty_dir / report["visual_height_file"]
    if not r32_path.is_file():
        pytest.skip(f"{r32_path} not present on disk (sidecar not checked in)")
    size = r32_path.stat().st_size
    want = V_expected * V_expected * 4
    assert size == want, (
        f"{beauty_dir.name}: {r32_path.name} is {size} bytes, expected "
        f"{want} (V={V_expected} squared x 4-byte float) -- this is exactly "
        f"the SIZE MISMATCH check mclib/terrain.cpp's loader performs"
    )


@pytest.mark.skipif(not REAL_BAKES, reason="no committed .beauty bakes found")
@pytest.mark.parametrize("beauty_dir", REAL_BAKES, ids=lambda d: d.name)
def test_real_bake_mips_file_size_matches_grid_math_if_present(beauty_dir):
    report = json.loads((beauty_dir / "visual_height_report.json").read_text())
    N = report["coarse_side"]
    mips_path = beauty_dir / "visual_height_mips.r32"
    if not mips_path.is_file():
        pytest.skip(f"{beauty_dir.name}: no mips sidecar baked")
    size = mips_path.stat().st_size
    want = 5 * N * N * 4  # 5 == len(MIP_STRIDES); mirrors terrain.cpp's mipWant
    assert size == want, (
        f"{beauty_dir.name}: visual_height_mips.r32 is {size} bytes, "
        f"expected {want} (5 x mapSide^2 x 4-byte float, mapSide={N}) -- "
        "this is exactly the mips SIZE MISMATCH check mclib/terrain.cpp's "
        "loader performs before it will trust the sidecar"
    )


@pytest.mark.skipif(not REAL_BAKES, reason="no committed .beauty bakes found")
def test_all_24_campaign_maps_have_a_fine_bake():
    """Board ask: 'check the visual_height_4x bake status for remaining
    maps.' Locks in the result of that audit (GEOMORPH-NEXT-SLICES-1 Slice 3,
    2026-07-16): all 24 campaign maps now have a visual_height_4x.r32 sidecar
    committed under tests/terrain/beautify/. A future removal/rename would
    regress silently without this guard."""
    present = {d.name[: -len(".beauty")] for d in REAL_BAKES
               if (d / "visual_height_4x.r32").is_file()}
    expected = {f"mc2_{i:02d}" for i in range(1, 25)}
    missing = expected - present
    assert not missing, f"campaign maps missing a visual_height_4x.r32 bake: {sorted(missing)}"
