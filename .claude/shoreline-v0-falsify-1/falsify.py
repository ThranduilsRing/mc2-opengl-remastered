#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 — offline test of the quantization-residual mechanism.

Reads SHIPPING mission heightfields (extracted from the deployed mission.fst),
computes the cubic B-spline reconstruction residual P_raw = Hc - Hs, and the
horizontal contour reach P/|grad(T)| in the shore band.

Read-only. No engine, no build, no launch.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

REPO = Path("A:/Games/mc2-main")
sys.path.insert(0, str(REPO / "tools" / "terrain_beautify"))
import mission_terrain_analyzer as MTA  # noqa: E402

WU = MTA.WORLD_UNITS_PER_VERTEX  # 128.0
SHORE_BAND_WU = 25.0


# ---------------------------------------------------------------- loading ---
def load_map(pak: Path, fit: Path):
    packets = MTA.read_packets(pak)
    md = MTA.locate_mapdata(packets)
    if md is None:
        raise RuntimeError(f"{pak}: no MapData packet")
    _, side, blocks = md
    water_elev = MTA.read_water_elevation(fit)
    layers = MTA.extract_layers(side, blocks, water_elev)
    alpha = read_fit_float(fit, "water", "alphadepth")
    return {
        "side": side,
        "elev": layers["elev"],
        "water_elev": water_elev,
        "alpha_depth": alpha,
    }


def read_fit_float(fit: Path, block: str, key: str):
    in_block = False
    for line in fit.read_text(errors="ignore").splitlines():
        s = line.strip()
        if s.startswith("["):
            in_block = s.lower() == f"[{block}]"
            continue
        if in_block and s.lower().replace(" ", "").startswith(f"f{key}="):
            try:
                return float(s.split("=", 1)[1])
            except (IndexError, ValueError):
                return None
    return None


# ------------------------------------------------------- B-spline smoothing --
def _conv1(a, k, axis):
    """Separable 1D convolution with edge replication."""
    r = (len(k) - 1) // 2
    pad = [(0, 0), (0, 0)]
    pad[axis] = (r, r)
    ap = np.pad(a, pad, mode="edge")
    out = np.zeros_like(a)
    n = a.shape[axis]
    for i, w in enumerate(k):
        sl = [slice(None), slice(None)]
        sl[axis] = slice(i, i + n)
        out += w * ap[tuple(sl)]
    return out


def bspline_kernel(scale: float) -> np.ndarray:
    """Cubic B-spline basis B3 sampled at integer lattice offsets, dilated by
    `scale` tiles.  scale=1 -> support radius 2 tiles -> weights [1,4,1]/6.
    B3(x) = 2/3 - |x|^2 + |x|^3/2          (|x|<1)
            (2-|x|)^3 / 6                  (1<=|x|<2)
    """
    r = int(np.ceil(2.0 * scale))
    xs = np.arange(-r, r + 1, dtype=np.float64) / scale
    ax = np.abs(xs)
    k = np.where(ax < 1.0, 2.0 / 3.0 - ax ** 2 + ax ** 3 / 2.0,
                 np.where(ax < 2.0, (2.0 - ax) ** 3 / 6.0, 0.0))
    return k / k.sum()


def bspline_smooth(H, scale=1.0):
    k = bspline_kernel(scale)
    return _conv1(_conv1(H, k, 0), k, 1)


# ------------------------------------------------------------------ stats ---
def pct(a, ps=(10, 25, 50, 75, 90, 99)):
    if a.size == 0:
        return {f"p{p}": None for p in ps}
    return {f"p{p}": float(np.percentile(a, p)) for p in ps}


def grad_mag(H):
    """Dimensionless slope magnitude |grad(z)| in wu/wu, central differences."""
    gy, gx = np.gradient(H)          # rise per 1 lattice step
    return np.sqrt((gx / WU) ** 2 + (gy / WU) ** 2)


def analyse(name, pak, fit, scales=(1.0, 2.0), ks=(1.0, 2.0)):
    m = load_map(pak, fit)
    H = m["elev"]
    W = m["water_elev"]
    b = m["alpha_depth"]
    g = grad_mag(H)
    band = np.abs(H - W) < SHORE_BAND_WU

    # exclude the 2-cell frame where the replicate-pad reconstruction is not
    # a fair test of the interior operator
    interior = np.zeros_like(band)
    interior[2:-2, 2:-2] = True
    band_i = band & interior

    rep = {
        "mission": name,
        "pak_md5_prefix": None,
        "grid_side": m["side"],
        "world_size_wu": m["side"] * WU,
        "water_elevation": W,
        "alpha_depth": b,
        "elev_min": float(H.min()),
        "elev_max": float(H.max()),
        "shore_band_cells": int(band_i.sum()),
        "shore_band_pct": round(100.0 * band_i.sum() / band_i.size, 2),
        "grad_shore": pct(g[band_i]),
        "grad_shore_median": float(np.median(g[band_i])) if band_i.any() else None,
        "variants": {},
    }

    # terracing evidence: how many distinct elevations, and run lengths
    uq = np.unique(H)
    rep["distinct_elevations"] = int(uq.size)
    rep["distinct_elevations_in_band"] = int(np.unique(H[band_i]).size)
    # horizontal run length of identical elevation along rows, within band
    runs = []
    for row in range(H.shape[0]):
        r = H[row]
        bmask = band_i[row]
        if not bmask.any():
            continue
        start = 0
        for c in range(1, len(r) + 1):
            if c == len(r) or r[c] != r[c - 1]:
                if bmask[start:c].any():
                    runs.append(c - start)
                start = c
    runs = np.array(runs, dtype=float)
    rep["shore_row_run_cells"] = pct(runs)
    rep["shore_row_run_median_wu"] = float(np.median(runs) * WU) if runs.size else None

    eps = 1e-9
    for s in scales:
        Hs = bspline_smooth(H, s)
        P_raw = H - Hs
        aP = np.abs(P_raw[band_i])
        gb = g[band_i]
        reach_raw = aP / np.maximum(gb, eps)
        # gradient-floored variant: floor |grad| at the map's own 10th pct so
        # a handful of dead-flat cells cannot manufacture an infinite median
        gfloor = max(float(np.percentile(gb, 10)), 1e-4) if gb.size else 1e-4
        reach_raw_f = aP / np.maximum(gb, gfloor)

        v = {
            "bspline_scale_tiles": s,
            "kernel_support_radius_tiles": 2.0 * s,
            "abs_P_raw": pct(aP),
            "abs_P_raw_mean": float(aP.mean()) if aP.size else None,
            "abs_P_raw_max": float(aP.max()) if aP.size else None,
            "reach_raw_wu": pct(reach_raw),
            "reach_raw_wu_gradfloored": pct(reach_raw_f),
            "grad_floor_used": gfloor,
            "tanh": {},
        }
        for k in ks:
            C = k * b / 2.0
            P = C * np.tanh(P_raw / C)
            aPt = np.abs(P[band_i])
            reach = aPt / np.maximum(gb, eps)
            reach_f = aPt / np.maximum(gb, gfloor)
            # contour-integrity: |grad(P)| vs |grad(T)|; >1 means the operator
            # can create/destroy contour components (new islands / holes)
            gP = grad_mag(P)
            ratio = gP[band_i] / np.maximum(gb, eps)
            v["tanh"][f"k={k}"] = {
                "C_wu": C,
                "saturated_pct": round(100.0 * float((np.abs(P_raw[band_i]) > C).mean()), 2)
                if aPt.size else None,
                "abs_P_wu": pct(aPt),
                "reach_wu": pct(reach),
                "reach_wu_gradfloored": pct(reach_f),
                "gradP_over_gradT": pct(ratio),
                "gradP_exceeds_gradT_pct": round(100.0 * float((ratio > 1.0).mean()), 2)
                if ratio.size else None,
            }
        rep["variants"][f"scale={s}"] = v

    return rep, m


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("out")
    out.mkdir(parents=True, exist_ok=True)
    reports = []
    for name in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        rep, _ = analyse(name, src / f"{name}.pak", src / f"{name}.fit")
        reports.append(rep)
        print(json.dumps(rep, indent=1))
        print("=" * 70)
    (out / "falsify_report.json").write_text(json.dumps(reports, indent=1))


if __name__ == "__main__":
    main()
