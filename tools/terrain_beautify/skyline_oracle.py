#!/usr/bin/env python3
"""Terrain Surface Truth Arc — skyline / surface-error oracle (slice 0).

Engine-free acceptance harness. Compares a REFERENCE (high-res) heightfield
against a CANDIDATE reduced/morphed surface and reports MEASURABLE fidelity so
LOD/reduction/tessellation slices are judged by numbers, not visual vibes.

Metrics (see docs/superpowers/specs/2026-07-03-terrain-adaptive-surface-cliff-displacement-meta.md §8):
  - skyline error   : per grazing view direction, per-column max-elevation
                      profile; drop (silhouette loss) + inflation, mean/max wu.
  - ridge loss      : elevation deficit at reference ridge cells.
  - cliff-height loss: |ref-cand| on steep (cliff) cells.
  - silhouette dev  : aggregate skyline L1/L2 across sampled directions.
  - curvature cover : how much of the reference's curvature the candidate keeps
                      (energy ratio + deviation fidelity).

Also reconstructs the surface the renderer actually draws at a vertex stride
(decimate lattice -> bilinear interp between kept vertices) with an optional
geomorph blend, so a candidate can be produced from a reference + LOD choice
without the engine. This is the renderer-agnostic surface-truth contract; the
engine bookmark captures confirm the final result ONCE, not per tuning step.

Pure numpy (+ optional Pillow for diagnostic PNGs). No engine, no GL.
"""
from __future__ import annotations

from typing import Optional

import numpy as np

# View directions sampled for the skyline. 'ns'/'ew' = axis-aligned grazing;
# 'diag_sum'/'diag_diff' = the two diagonal azimuths (match the gaea_peaks_01
# y45/y135/y225 grazing bookmark poses). Each is the silhouette = max elevation
# along the view ray, indexed by the perpendicular (screen-column) axis.
VIEW_DIRS = ("ns", "ew", "diag_sum", "diag_diff")


# --------------------------------------------------------------------------
# Surface reconstruction (produce a candidate from a reference + LOD choice)
# --------------------------------------------------------------------------
def _lattice_coords(n: int, stride: int) -> np.ndarray:
    """Vertex indices the renderer keeps at `stride`, always including the last
    sample (n-1) so interpolation never extrapolates past the edge."""
    if stride <= 1:
        return np.arange(n)
    coords = list(range(0, n, stride))
    if coords[-1] != n - 1:
        coords.append(n - 1)
    return np.asarray(coords)


def _interp_axis(arr: np.ndarray, lattice: np.ndarray, full: np.ndarray,
                 axis: int) -> np.ndarray:
    """Vectorized linear interp of `arr` (sampled at `lattice` along `axis`) onto
    `full` coordinates. Fully array-wide (no per-row python loop)."""
    idx = np.clip(np.searchsorted(lattice, full, side="right") - 1,
                  0, lattice.size - 2)
    x0 = lattice[idx].astype(np.float64)
    x1 = lattice[idx + 1].astype(np.float64)
    t = (full - x0) / np.maximum(x1 - x0, 1e-12)
    a0 = np.take(arr, idx, axis=axis)
    a1 = np.take(arr, idx + 1, axis=axis)
    shape = [1, 1]
    shape[axis] = full.size
    t = t.reshape(shape)
    return a0 * (1.0 - t) + a1 * t


def decimate_upsample(height: np.ndarray, stride: int) -> np.ndarray:
    """Surface the renderer draws at vertex `stride`: sample the height on the
    stride lattice, then bilinear-interpolate back to full resolution (the
    triangle/grid surface spanning kept vertices). stride<=1 -> identity."""
    h = np.asarray(height, dtype=np.float64)
    n, m = h.shape
    if stride <= 1:
        return h.copy()
    rc = _lattice_coords(n, stride)
    cc = _lattice_coords(m, stride)
    sampled = h[np.ix_(rc, cc)]
    mid = _interp_axis(sampled, cc, np.arange(m), axis=1)   # (rc, m)
    return _interp_axis(mid, rc, np.arange(n), axis=0)      # (n, m)


def morph_surface(height: np.ndarray, stride_fine: int, stride_coarse: int,
                  morph: float) -> np.ndarray:
    """Geomorph blend: morph=0 -> fine-stride surface, 1 -> coarse-stride
    surface (CDLOD/geo-mipmapping parent morph). Even-index verts are shared so
    they are fixed; the lerp moves the odd verts toward the parent midpoint,
    which a per-vertex linear blend of the two reconstructions reproduces."""
    m = float(np.clip(morph, 0.0, 1.0))
    fine = decimate_upsample(height, stride_fine)
    if m == 0.0:
        return fine
    coarse = decimate_upsample(height, stride_coarse)
    return (1.0 - m) * fine + m * coarse


# --------------------------------------------------------------------------
# Skyline / silhouette
# --------------------------------------------------------------------------
def skyline_profiles(height: np.ndarray, dirs=VIEW_DIRS) -> dict:
    """Per-direction silhouette: max elevation along the view ray, indexed by
    the perpendicular screen-column axis."""
    h = np.asarray(height, dtype=np.float64)
    n, m = h.shape
    prof = {}
    if "ns" in dirs:
        prof["ns"] = h.max(axis=0)          # view along +y, column = x
    if "ew" in dirs:
        prof["ew"] = h.max(axis=1)          # view along +x, column = y
    if "diag_sum" in dirs or "diag_diff" in dirs:
        ii, jj = np.indices(h.shape)
        if "diag_sum" in dirs:
            key = (ii + jj).ravel()
            p = np.full(n + m - 1, -np.inf)
            np.maximum.at(p, key, h.ravel())
            prof["diag_sum"] = p
        if "diag_diff" in dirs:
            key = (ii - jj + (m - 1)).ravel()
            p = np.full(n + m - 1, -np.inf)
            np.maximum.at(p, key, h.ravel())
            prof["diag_diff"] = p
    return prof


def skyline_error(ref: np.ndarray, cand: np.ndarray, dirs=VIEW_DIRS) -> dict:
    """Per-direction drop (ref above cand = silhouette LOSS) and inflation
    (cand above ref), mean/max in world units."""
    pr = skyline_profiles(ref, dirs)
    pc = skyline_profiles(cand, dirs)
    out = {}
    for d in pr:
        diff = pc[d] - pr[d]
        finite = np.isfinite(diff)
        diff = diff[finite]
        drop = np.maximum(-diff, 0.0)
        infl = np.maximum(diff, 0.0)
        out[d] = {
            "drop_mean": float(drop.mean()) if drop.size else 0.0,
            "drop_max": float(drop.max()) if drop.size else 0.0,
            "inflation_mean": float(infl.mean()) if infl.size else 0.0,
            "inflation_max": float(infl.max()) if infl.size else 0.0,
        }
    return out


def silhouette_deviation(ref: np.ndarray, cand: np.ndarray,
                         dirs=VIEW_DIRS) -> dict:
    """Aggregate skyline deviation across all sampled directions."""
    pr = skyline_profiles(ref, dirs)
    pc = skyline_profiles(cand, dirs)
    l1 = 0.0
    l2 = 0.0
    count = 0
    for d in pr:
        diff = (pc[d] - pr[d])
        diff = diff[np.isfinite(diff)]
        l1 += float(np.abs(diff).sum())
        l2 += float((diff * diff).sum())
        count += diff.size
    return {"l1_mean": l1 / count if count else 0.0,
            "l2_rms": float(np.sqrt(l2 / count)) if count else 0.0}


# --------------------------------------------------------------------------
# Ridge / cliff / curvature fields
# --------------------------------------------------------------------------
def _neigh_stat(h: np.ndarray, op) -> np.ndarray:
    p = np.pad(h, 1, mode="edge")
    stack = np.stack([p[0:-2, 1:-1], p[2:, 1:-1], p[1:-1, 0:-2],
                      p[1:-1, 2:], p[1:-1, 1:-1]], axis=0)
    return op(stack, axis=0)


def ridge_mask(height: np.ndarray, prominence_wu: float = 1.0) -> np.ndarray:
    """Ridge cells: a local max in the 3x3 neighborhood that stands
    `prominence_wu` above its local min (a genuine crest, not flat noise)."""
    h = np.asarray(height, dtype=np.float64)
    local_max = _neigh_stat(h, np.max)
    local_min = _neigh_stat(h, np.min)
    return (h >= local_max - 1e-9) & ((h - local_min) > prominence_wu)


def ridge_loss(ref: np.ndarray, cand: np.ndarray,
               mask: Optional[np.ndarray] = None,
               prominence_wu: float = 1.0) -> dict:
    """Elevation deficit (ref above cand) at reference ridge cells."""
    if mask is None:
        mask = ridge_mask(ref, prominence_wu)
    if not mask.any():
        return {"mean": 0.0, "max": 0.0, "cells": 0}
    deficit = np.maximum(ref[mask] - cand[mask], 0.0)
    return {"mean": float(deficit.mean()), "max": float(deficit.max()),
            "cells": int(mask.sum())}


def slope_deg(height: np.ndarray, wu_per_texel: float = 1.0) -> np.ndarray:
    """Per-cell slope in degrees from the height gradient."""
    gy, gx = np.gradient(np.asarray(height, dtype=np.float64))
    grad = np.sqrt((gx / wu_per_texel) ** 2 + (gy / wu_per_texel) ** 2)
    return np.degrees(np.arctan(grad))


def cliff_mask(height: np.ndarray, wu_per_texel: float = 1.0,
               slope_thresh_deg: float = 40.0) -> np.ndarray:
    return slope_deg(height, wu_per_texel) >= slope_thresh_deg


def cliff_height_loss(ref: np.ndarray, cand: np.ndarray,
                      wu_per_texel: float = 1.0,
                      slope_thresh_deg: float = 40.0,
                      mask: Optional[np.ndarray] = None) -> dict:
    """|ref-cand| on steep (cliff) cells — depth flattened out of cliffs."""
    if mask is None:
        mask = cliff_mask(ref, wu_per_texel, slope_thresh_deg)
    if not mask.any():
        return {"mean": 0.0, "max": 0.0, "cells": 0}
    err = np.abs(ref[mask] - cand[mask])
    return {"mean": float(err.mean()), "max": float(err.max()),
            "cells": int(mask.sum())}


def curvature(height: np.ndarray) -> np.ndarray:
    """Discrete Laplacian (edge-padded) — proxy for surface curvature/detail."""
    h = np.asarray(height, dtype=np.float64)
    p = np.pad(h, 1, mode="edge")
    return (p[0:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, 0:-2]
            + p[1:-1, 2:] - 4.0 * h)


def curvature_coverage(ref: np.ndarray, cand: np.ndarray) -> dict:
    """energy_ratio = |curv(cand)| / |curv(ref)| (1=full, <1 oversmoothed,
    >1 noisy). fidelity = 1 - |curv(ref)-curv(cand)| / |curv(ref)| (1=identical,
    lower as candidate curvature deviates in either direction)."""
    cr = curvature(ref)
    cc = curvature(cand)
    denom = float(np.abs(cr).sum())
    if denom < 1e-12:
        return {"energy_ratio": 1.0, "fidelity": 1.0}
    energy = float(np.abs(cc).sum()) / denom
    fidelity = 1.0 - float(np.abs(cr - cc).sum()) / denom
    return {"energy_ratio": energy, "fidelity": fidelity}


# --------------------------------------------------------------------------
# Top-level evaluation
# --------------------------------------------------------------------------
def evaluate(ref: np.ndarray, cand: np.ndarray, wu_per_texel: float = 1.0,
             slope_thresh_deg: float = 40.0, prominence_wu: float = 1.0,
             dirs=VIEW_DIRS) -> dict:
    """Full metric bundle comparing candidate surface `cand` to reference `ref`.
    Both must share the same grid shape."""
    ref = np.asarray(ref, dtype=np.float64)
    cand = np.asarray(cand, dtype=np.float64)
    if ref.shape != cand.shape:
        raise ValueError(f"shape mismatch ref{ref.shape} cand{cand.shape}")
    return {
        "shape": list(ref.shape),
        "skyline": skyline_error(ref, cand, dirs),
        "silhouette": silhouette_deviation(ref, cand, dirs),
        "ridge_loss": ridge_loss(ref, cand, prominence_wu=prominence_wu),
        "cliff_height_loss": cliff_height_loss(
            ref, cand, wu_per_texel, slope_thresh_deg),
        "curvature_coverage": curvature_coverage(ref, cand),
    }


def format_report(result: dict) -> str:
    lines = [f"[skyline_oracle] grid {result['shape']}"]
    for d, m in result["skyline"].items():
        lines.append(
            f"  skyline[{d:9s}] drop mean/max {m['drop_mean']:6.2f}/{m['drop_max']:6.2f}  "
            f"inflation mean/max {m['inflation_mean']:6.2f}/{m['inflation_max']:6.2f} wu")
    s = result["silhouette"]
    lines.append(f"  silhouette   L1 {s['l1_mean']:.3f}  RMS {s['l2_rms']:.3f} wu")
    r = result["ridge_loss"]
    lines.append(f"  ridge loss   mean/max {r['mean']:.2f}/{r['max']:.2f} wu ({r['cells']} cells)")
    c = result["cliff_height_loss"]
    lines.append(f"  cliff loss   mean/max {c['mean']:.2f}/{c['max']:.2f} wu ({c['cells']} cells)")
    cv = result["curvature_coverage"]
    lines.append(f"  curvature    energy {cv['energy_ratio']:.3f}  fidelity {cv['fidelity']:.3f}")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Optional diagnostic PNGs (Pillow); metric path never depends on these.
# --------------------------------------------------------------------------
def save_diagnostics(ref: np.ndarray, cand: np.ndarray, out_dir) -> list:
    """Write before/after diagnostic images: signed skyline-deficit heatmap and
    per-direction skyline overlay rasters. Returns written paths. Best-effort."""
    from pathlib import Path
    try:
        from PIL import Image
    except Exception:
        return []
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written = []

    def _gray(arr, path):
        a = np.asarray(arr, dtype=np.float64)
        lo, hi = float(a.min()), float(a.max())
        norm = (a - lo) / (hi - lo) if hi > lo else np.zeros_like(a)
        Image.fromarray((norm * 255).astype(np.uint8), "L").save(path)
        written.append(str(path))

    _gray(np.maximum(ref - cand, 0.0), out / "surface_deficit.png")
    _gray(np.abs(curvature(ref) - curvature(cand)), out / "curvature_deviation.png")

    # skyline overlays: raster each profile pair into a small image
    pr = skyline_profiles(ref)
    pc = skyline_profiles(cand)
    for d in pr:
        r, c = pr[d], pc[d]
        finite = np.isfinite(r) & np.isfinite(c)
        r, c = r[finite], c[finite]
        if r.size == 0:
            continue
        H = 128
        img = np.zeros((H, r.size, 3), dtype=np.uint8)
        lo = float(min(r.min(), c.min()))
        hi = float(max(r.max(), c.max()))
        rng = hi - lo if hi > lo else 1.0
        for x in range(r.size):
            yr = int((1 - (r[x] - lo) / rng) * (H - 1))
            yc = int((1 - (c[x] - lo) / rng) * (H - 1))
            img[yr, x] = (0, 255, 0)     # reference = green
            img[yc, x] = (255, 0, 0)     # candidate = red
        Image.fromarray(img, "RGB").save(out / f"skyline_{d}.png")
        written.append(str(out / f"skyline_{d}.png"))
    return written
