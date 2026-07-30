#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 4 — FAITHFUL continuous model.

The engine interpolates the heightfield BILINEARLY across each 128 wu tile, and
the proposed cure evaluates a CONTINUOUS C2 cubic B-spline reconstruction
per-fragment.  Sampling both only at lattice vertices (parts 1-3) understates
the smoothness the operator can deliver.  This part evaluates both fields on an
8x supersampled grid (16 wu resolution) and measures:

  * geometric displacement of the waterline, in world units
  * axis-alignment "staircase index" (isotropic baseline = 0.222)
  * corner density per 1000 wu of coastline (facet count)

H_fine   = bilinear reconstruction of Hc              (what the engine draws)
Hs_fine  = continuous cubic B-spline reconstruction   (the proposed smooth field)
P        = C * tanh((H_fine - Hs_fine) / C),  C = k * alphaDepth / 2
H_eff    = H_fine - P                                 (proposed silhouette field)

Read-only; no engine, no build, no launch.
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
from falsify import WU, load_map, pct  # noqa: E402

F = 8            # supersample factor -> 128/8 = 16 wu fine step


def b3(x):
    ax = np.abs(x)
    return np.where(ax < 1.0, 2.0 / 3.0 - ax ** 2 + ax ** 3 / 2.0,
                    np.where(ax < 2.0, (2.0 - ax) ** 3 / 6.0, 0.0))


def tent(x):
    ax = np.abs(x)
    return np.maximum(0.0, 1.0 - ax)


def resample_matrix(n, kernel, scale=1.0, f=F):
    """(n*f, n) matrix mapping lattice samples to fine positions.
    Fine sample j sits at lattice coordinate j/f.  Edge handling: replicate by
    folding out-of-range taps onto the boundary index."""
    xf = np.arange(n * f, dtype=np.float64) / f
    idx = np.arange(n, dtype=np.float64)
    Wt = kernel((xf[:, None] - idx[None, :]) / scale)
    s = Wt.sum(axis=1, keepdims=True)
    s[s == 0] = 1.0
    return Wt / s


def fields(H, scale, k, b):
    n = H.shape[0]
    A_lin = resample_matrix(n, tent, 1.0)
    A_bsp = resample_matrix(n, b3, scale)
    H_f = A_lin @ H @ A_lin.T
    Hs_f = A_bsp @ H @ A_bsp.T
    C = k * b / 2.0
    P = C * np.tanh((H_f - Hs_f) / C)
    return H_f, Hs_f, H_f - P, C, P


def iso(Fld, level):
    cs = plt.contour(Fld, levels=[level])
    segs = [np.asarray(p, float) for p in cs.allsegs[0] if len(p) >= 2]
    plt.close("all")
    return segs


def densify(segs, step=0.5):
    pts = []
    for s in segs:
        d = np.linalg.norm(np.diff(s, axis=0), axis=1)
        for i, dd in enumerate(d):
            nn = max(int(np.ceil(dd / step)), 1)
            t = np.linspace(0, 1, nn, endpoint=False)[:, None]
            pts.append(s[i][None, :] * (1 - t) + s[i + 1][None, :] * t)
        pts.append(s[-1][None, :])
    return np.vstack(pts) if pts else np.zeros((0, 2))


def shape_metrics(segs, wu_per_unit):
    """axis-alignment fraction + corner density per 1000 wu."""
    tot = axi = 0.0
    corners = 0
    for s in segs:
        d = np.diff(s, axis=0)
        L = np.linalg.norm(d, axis=1)
        ok = L > 1e-12
        if ok.sum() < 3:
            continue
        d, L = d[ok], L[ok]
        ang = np.degrees(np.arctan2(np.abs(d[:, 1]), np.abs(d[:, 0])))
        near = (ang < 10.0) | (ang > 80.0)
        tot += L.sum()
        axi += L[near].sum()
        # turning angle between consecutive segments
        u = d / L[:, None]
        cosang = np.clip((u[:-1] * u[1:]).sum(axis=1), -1, 1)
        turn = np.degrees(np.arccos(cosang))
        corners += int((turn > 20.0).sum())
    length_wu = tot * wu_per_unit
    return {
        "staircase_index": (axi / tot) if tot > 0 else None,
        "length_wu": length_wu,
        "corners_per_1000wu": (1000.0 * corners / length_wu) if length_wu > 0 else None,
        "n_components": len(segs),
    }


def analyse(name, src, scales, ks):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    wpu = WU / F
    A_lin = resample_matrix(H.shape[0], tent, 1.0)
    H_f = A_lin @ H @ A_lin.T
    stock = iso(H_f, W)
    sp = densify(stock)
    tree = cKDTree(sp * wpu)
    rep = {"mission": name, "water_elevation": W, "alpha_depth": b,
           "grid_side": m["side"], "fine_step_wu": wpu,
           "stock": shape_metrics(stock, wpu), "rows": []}
    for s in scales:
        for k in ks + [None]:
            _, Hs_f, Heff, C, P = fields(H, s, k if k else 1.0, b)
            if k is None:
                Heff, C = Hs_f, float("inf")
                P = H_f - Hs_f
            segs = iso(Heff, W)
            pts = densify(segs)
            d, _ = tree.query(pts * wpu) if len(pts) else (np.zeros(0), None)
            sm = shape_metrics(segs, wpu)
            # residual population restricted to the shore band on the fine grid
            bandm = np.abs(H_f - W) < 25.0
            aP = np.abs(P[bandm])
            rep["rows"].append({
                "scale": s, "k": k, "C_wu": C,
                "abs_P_band": pct(aP),
                "sat_pct": round(100.0 * float((np.abs(H_f - Hs_f)[bandm] > C).mean()), 2)
                if np.isfinite(C) else 0.0,
                "disp": pct(d) if len(d) else None,
                "disp_mean": float(d.mean()) if len(d) else None,
                "disp_max": float(d.max()) if len(d) else None,
                **{f"shape_{kk}": vv for kk, vv in sm.items()},
            })
    return rep, (H, W, b, wpu, stock)


def render(name, src, out, scale, k, tag, window=None):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    H_f, Hs_f, Heff, C, _ = fields(H, scale, k, b)
    wpu = WU / F
    n = H_f.shape[0]
    xs = np.arange(n) * wpu
    Xg, Yg = np.meshgrid(xs, xs)
    fig, ax = plt.subplots(figsize=(14, 14), dpi=110)
    ax.imshow(np.clip((H_f - (W - 250)) / 500.0, 0, 1), cmap="terrain",
              extent=[0, n * wpu, n * wpu, 0], interpolation="bilinear",
              alpha=0.5, origin="upper")
    ax.contour(Xg, Yg, H_f, levels=[W], colors=["#111111"], linewidths=2.6)
    ax.contour(Xg, Yg, Hs_f, levels=[W], colors=["#1b9e77"], linewidths=1.2,
               linestyles="dashed")
    ax.contour(Xg, Yg, Heff, levels=[W], colors=["#d95f02"], linewidths=2.0)
    if window:
        ax.set_xlim(window[0], window[1]); ax.set_ylim(window[3], window[2])
    ax.set_aspect("equal")
    ax.set_xlabel("world units (X)"); ax.set_ylabel("world units (Y)")
    ax.set_title(f"{name} waterline @ {W:g} wu   BLACK = stock (what ships)   "
                 f"ORANGE = proposed (C={C:g} wu)   GREEN dashed = unclamped smooth\n"
                 f"B-spline footprint radius {2*scale:g} tiles = {2*scale*WU:g} wu"
                 f"   |   1 tile = 128 wu   |   grid lines every 1024 wu",
                 fontsize=10)
    ax.set_xticks(np.arange(0, n * wpu + 1, 1024))
    ax.set_yticks(np.arange(0, n * wpu + 1, 1024))
    ax.grid(True, color="#666", alpha=0.3, linewidth=0.5)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    p = out / f"{name}_fine{tag}.png"
    fig.savefig(p); plt.close(fig)
    return p


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    res = []
    for nm in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        r, _ = analyse(nm, src, [1.0, 2.0, 4.0], [1.0, 2.0])
        res.append(r)
        st = r["stock"]
        print(f"=== {nm} W={r['water_elevation']:g} b={r['alpha_depth']:g}  STOCK: "
              f"stair={st['staircase_index']:.3f} corners/1000wu={st['corners_per_1000wu']:.1f} "
              f"len={st['length_wu']:.0f} comps={st['n_components']}")
        for row in r["rows"]:
            kk = "UNCLAMPED" if row["k"] is None else f"k={row['k']:g} C={row['C_wu']:g}"
            print(f"   s={row['scale']:<3g} {kk:<14} |P|med={row['abs_P_band']['p50']:6.2f} "
                  f"sat={row['sat_pct']:5.1f}%  disp med={row['disp']['p50']:6.1f} "
                  f"p90={row['disp']['p90']:6.1f} max={row['disp_max']:6.1f}  "
                  f"stair={row['shape_staircase_index']:.3f} "
                  f"corners={row['shape_corners_per_1000wu']:5.1f} "
                  f"comps={row['shape_n_components']}")
    (out / "fine.json").write_text(json.dumps(res, indent=1))
    for tag, s, k in (("_s1_k1", 1.0, 1.0), ("_s2_k2", 2.0, 2.0), ("_s4_k2", 4.0, 2.0)):
        print(render("mc2_17", src, out, s, k, tag))
    print(render("mc2_10", src, out, 1.0, 1.0, "_s1_k1"))


if __name__ == "__main__":
    main()
