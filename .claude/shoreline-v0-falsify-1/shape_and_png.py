#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 3 — staircase shape metric, scale sweep, PNGs.

1. "Staircase index": length-weighted fraction of the iso-contour whose local
   direction is within +/-10 deg of axis-aligned.  A tile-locked staircase
   concentrates near 0/90 deg; an organic coastline is spread.
2. Scale sweep for mc2_17: how big must the B-spline footprint be before the
   contour moves 40 / 128 wu, and what |P| budget does that cost?
3. PNG: stock contour vs proposed contour vs unclamped-smooth contour.

Read-only; no engine.
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
from falsify import WU, load_map, bspline_smooth, grad_mag, pct  # noqa: E402
from contour_reach import iso_paths, densify, contour_cells  # noqa: E402


def staircase_index(segs, tol_deg=10.0):
    """Length-weighted fraction of contour within tol of axis-aligned."""
    tot = 0.0
    axi = 0.0
    for s in segs:
        d = np.diff(s, axis=0)
        L = np.linalg.norm(d, axis=1)
        ok = L > 1e-9
        if not ok.any():
            continue
        ang = np.degrees(np.arctan2(np.abs(d[ok, 1]), np.abs(d[ok, 0])))  # 0..90
        near = (ang < tol_deg) | (ang > 90.0 - tol_deg)
        tot += L[ok].sum()
        axi += L[ok][near].sum()
    return (axi / tot) if tot > 0 else None, tot * WU


def sweep(name, src, scales, k=1.0):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    stock = iso_paths(H, W)
    sp = densify(stock)
    tree = cKDTree(sp * WU)
    si_stock, len_stock = staircase_index(stock)
    rows = []
    for s in scales:
        Hs = bspline_smooth(H, s)
        Praw = H - Hs
        for kk in (k, None):     # kk=None -> unclamped (C = inf)
            if kk is None:
                Heff = Hs
                C = float("inf")
                P = Praw
            else:
                C = kk * b / 2.0
                P = C * np.tanh(Praw / C)
                Heff = H - P
            segs = iso_paths(Heff, W)
            pts = densify(segs)
            d, _ = tree.query(pts * WU) if len(pts) else (np.zeros(0), None)
            si, ln = staircase_index(segs)
            rows.append({
                "scale": s, "k": kk, "C_wu": C,
                "abs_P_med_all": float(np.median(np.abs(P))),
                "abs_P_p99_all": float(np.percentile(np.abs(P), 99)),
                "geom_disp_med_wu": float(np.median(d)) if len(d) else None,
                "geom_disp_p90_wu": float(np.percentile(d, 90)) if len(d) else None,
                "geom_disp_max_wu": float(d.max()) if len(d) else None,
                "staircase_index": si,
                "contour_len_wu": ln,
                "n_components": len(segs),
            })
    return {"mission": name, "water_elevation": W, "alpha_depth": b,
            "stock_staircase_index": si_stock,
            "stock_contour_len_wu": len_stock,
            "stock_components": len(stock),
            "sweep": rows}


def render(name, src, out, scale, k, window=None, tag=""):
    """PNG of stock vs proposed vs unclamped contour."""
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    Hs = bspline_smooth(H, scale)
    C = k * b / 2.0
    P = C * np.tanh((H - Hs) / C)
    Heff = H - P

    fig, ax = plt.subplots(figsize=(13, 13), dpi=110)
    ext = [0, H.shape[1] * WU, H.shape[0] * WU, 0]
    shade = np.clip((H - (W - 200)) / 400.0, 0, 1)
    ax.imshow(shade, cmap="terrain", extent=ext, interpolation="nearest",
              alpha=0.55, origin="upper")
    xs = (np.arange(H.shape[1])) * WU
    ys = (np.arange(H.shape[0])) * WU
    Xg, Yg = np.meshgrid(xs, ys)
    ax.contour(Xg, Yg, H, levels=[W], colors=["#111111"], linewidths=2.4,
               linestyles="solid")
    ax.contour(Xg, Yg, Hs, levels=[W], colors=["#1b9e77"], linewidths=1.3,
               linestyles="dashed")
    ax.contour(Xg, Yg, Heff, levels=[W], colors=["#d95f02"], linewidths=2.0)
    if window:
        ax.set_xlim(window[0], window[1])
        ax.set_ylim(window[3], window[2])
    ax.set_aspect("equal")
    ax.set_xlabel("world units (X)")
    ax.set_ylabel("world units (Y, +south)")
    ax.set_title(
        f"{name}: waterline @ elev {W:g} wu   |   black = STOCK (tile lattice)   "
        f"orange = PROPOSED (C={C:g} wu, k={k:g})   green dashed = UNCLAMPED smooth\n"
        f"B-spline footprint radius {2*scale:g} tiles = {2*scale*WU:g} wu   "
        f"| tile = {WU:g} wu", fontsize=10)
    ax.grid(True, which="both", color="#888888", alpha=0.25, linewidth=0.4)
    ax.set_xticks(np.arange(0, H.shape[1] * WU + 1, WU * 8), minor=False)
    ax.set_yticks(np.arange(0, H.shape[0] * WU + 1, WU * 8), minor=False)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    p = out / f"{name}_contour{tag}.png"
    fig.savefig(p)
    plt.close(fig)
    return p


def main():
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    res = []
    for nm in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        r = sweep(nm, src, [1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0])
        res.append(r)
        print(f"=== {r['mission']}  W={r['water_elevation']}  b={r['alpha_depth']}"
              f"  stockStair={r['stock_staircase_index']:.3f}"
              f"  stockLen={r['stock_contour_len_wu']:.0f}wu"
              f"  comps={r['stock_components']}")
        for row in r["sweep"]:
            kk = "unclamped" if row["k"] is None else f"k={row['k']:g} C={row['C_wu']:g}"
            print(f"   s={row['scale']:<4g} {kk:<16} |P|med={row['abs_P_med_all']:6.2f} "
                  f"p99={row['abs_P_p99_all']:7.2f}  disp med={row['geom_disp_med_wu']:7.1f} "
                  f"p90={row['geom_disp_p90_wu']:7.1f} max={row['geom_disp_max_wu']:7.1f}  "
                  f"stair={row['staircase_index']:.3f}  comps={row['n_components']}")
    (out / "sweep.json").write_text(json.dumps(res, indent=1))

    # decisive picture: mc2_17 at the literal design parameters
    print(render("mc2_17", src, out, 1.0, 1.0, tag="_s1_k1_full"))
    print(render("mc2_17", src, out, 2.0, 2.0, tag="_s2_k2_full"))


if __name__ == "__main__":
    main()
