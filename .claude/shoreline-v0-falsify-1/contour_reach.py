#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 2 — the metric that actually matters.

Restricts the reach statistic to CONTOUR-CROSSING tiles (the only place the
waterline lives) and, more decisively, MEASURES the contour displacement
geometrically: extract the stock iso-line of H = waterElevation and the
proposed iso-line of (H - P) = waterElevation, then report the distribution of
world-unit distance from each proposed-contour sample to the stock contour.

No gradient division, no singularities.  Read-only; no engine.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from scipy.spatial import cKDTree  # noqa: E402

sys.path.insert(0, str(Path(__file__).parent))
from falsify import (WU, SHORE_BAND_WU, load_map, bspline_smooth, grad_mag,  # noqa: E402
                     pct)


def contour_cells(H, W):
    """Tiles (cells i,j -> quad with corners i..i+1, j..j+1) crossed by H=W."""
    q = np.stack([H[:-1, :-1], H[:-1, 1:], H[1:, :-1], H[1:, 1:]])
    below = (q <= W).any(axis=0)
    above = (q > W).any(axis=0)
    cross = below & above
    # map back to vertex mask: any of the 4 corners of a crossing tile
    m = np.zeros_like(H, dtype=bool)
    m[:-1, :-1] |= cross
    m[:-1, 1:] |= cross
    m[1:, :-1] |= cross
    m[1:, 1:] |= cross
    return cross, m


def iso_paths(F, level):
    """World-unit polylines of the level-set of F (row=+y-index, col=+x-index).
    Coordinates are returned in LATTICE index space; multiply by WU for wu."""
    cs = plt.contour(F, levels=[level])
    segs = []
    for p in cs.allsegs[0]:
        if len(p) >= 2:
            segs.append(np.asarray(p, dtype=np.float64))  # (x=col, y=row)
    plt.close("all")
    return segs


def densify(segs, step=0.25):
    """Resample polylines to <= `step` lattice units between samples."""
    pts = []
    for s in segs:
        d = np.linalg.norm(np.diff(s, axis=0), axis=1)
        for i, dd in enumerate(d):
            n = max(int(np.ceil(dd / step)), 1)
            t = np.linspace(0, 1, n, endpoint=False)[:, None]
            pts.append(s[i][None, :] * (1 - t) + s[i + 1][None, :] * t)
        pts.append(s[-1][None, :])
    if not pts:
        return np.zeros((0, 2))
    return np.vstack(pts)


def analyse(name, src, out, scales=(1.0, 2.0), ks=(1.0, 2.0)):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    g = grad_mag(H)
    band = np.abs(H - W) < SHORE_BAND_WU
    interior = np.zeros_like(band)
    interior[2:-2, 2:-2] = True
    cross_tiles, cmask = contour_cells(H, W)
    cband = cmask & interior          # the population that matters

    rep = {
        "mission": name,
        "grid_side": m["side"],
        "water_elevation": W,
        "alpha_depth": b,
        "band_cells_abs25": int((band & interior).sum()),
        "contour_tiles": int(cross_tiles.sum()),
        "contour_cells": int(cband.sum()),
        "grad_on_contour_cells": pct(g[cband]),
        "grad_on_contour_median": float(np.median(g[cband])),
        "grad_on_band_median": float(np.median(g[band & interior])),
        "stock_contour_length_wu": None,
        "variants": {},
    }

    stock_segs = iso_paths(H, W)
    stock_pts = densify(stock_segs)
    rep["stock_contour_length_wu"] = float(sum(
        np.linalg.norm(np.diff(s, axis=0), axis=1).sum() for s in stock_segs) * WU)
    tree = cKDTree(stock_pts * WU) if len(stock_pts) else None

    for s in scales:
        Hs = bspline_smooth(H, s)
        P_raw = H - Hs
        v = {"bspline_scale_tiles": s,
             "abs_P_raw_on_contour": pct(np.abs(P_raw[cband])),
             "reach_raw_on_contour_wu": pct(np.abs(P_raw[cband]) / np.maximum(g[cband], 1e-6)),
             "tanh": {}}
        # unclamped ideal, for reference
        prop_segs = iso_paths(Hs, W)
        pp = densify(prop_segs)
        if tree is not None and len(pp):
            d, _ = tree.query(pp * WU)
            v["geom_displacement_wu_UNCLAMPED"] = pct(d)
            v["geom_displacement_mean_UNCLAMPED"] = float(d.mean())
        for k in ks:
            C = k * b / 2.0
            P = C * np.tanh(P_raw / C)
            Heff = H - P
            psegs = iso_paths(Heff, W)
            pts = densify(psegs)
            entry = {
                "C_wu": C,
                "abs_P_on_contour_wu": pct(np.abs(P[cband])),
                "reach_first_order_wu": pct(np.abs(P[cband]) / np.maximum(g[cband], 1e-6)),
                "proposed_contour_length_wu": float(sum(
                    np.linalg.norm(np.diff(x, axis=0), axis=1).sum() for x in psegs) * WU),
                "n_components_stock": len(stock_segs),
                "n_components_proposed": len(psegs),
            }
            if tree is not None and len(pts):
                d, _ = tree.query(pts * WU)
                entry["geom_displacement_wu"] = pct(d)
                entry["geom_displacement_mean_wu"] = float(d.mean())
                entry["geom_displacement_max_wu"] = float(d.max())
                # reverse (stock -> proposed): detects contour deletion
                t2 = cKDTree(pts * WU)
                d2, _ = t2.query(stock_pts * WU)
                entry["geom_displacement_rev_wu"] = pct(d2)
            v["tanh"][f"k={k}"] = entry
        rep["variants"][f"scale={s}"] = v
    return rep, (H, W, b)


def main():
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    reps = []
    for name in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        rep, _ = analyse(name, src, out)
        reps.append(rep)
        print(json.dumps(rep, indent=1))
        print("=" * 70)
    (out / "contour_reach.json").write_text(json.dumps(reps, indent=1))


if __name__ == "__main__":
    main()
