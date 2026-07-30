#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 7 — the SHORE ALPHA RAMP, not the contour.

The engine's shore term is
    shore = smoothstep(-shoreBlend*0.5, shoreBlend, WaterThickness)
with WaterThickness BILINEARLY interpolated per tile.  Bilinear interpolation is
C0 but not C1: its slope jumps at every 128 wu tile boundary.  On a flat shelf
the ramp is spread over 1.5*alphaDepth/|grad| world units — hundreds of wu — so
those slope jumps become visible facets ON THE TILE LATTICE even when the
waterline contour itself is perfectly smooth.

This part measures that directly:
  facet_index = mean |d2(alpha)/dx2| on tile-boundary columns
                / mean |d2(alpha)/dx2| in tile interiors
(1.0 = no lattice preference; >>1 = ramp is faceted on the tile grid)

and renders the alpha field, stock vs proposed, for eyeball grading.

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

sys.path.insert(0, str(Path(__file__).parent))
from falsify import WU, load_map  # noqa: E402
import fine as FI  # noqa: E402


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def alpha_fields(H, W, b, scale, k, f):
    FI.F = f
    n = H.shape[0]
    A_lin = FI.resample_matrix(n, FI.tent, 1.0, f)
    A_bsp = FI.resample_matrix(n, FI.b3, scale, f)
    H_f = A_lin @ H @ A_lin.T
    Hs_f = A_bsp @ H @ A_bsp.T
    C = k * b / 2.0
    Heff = H_f - C * np.tanh((H_f - Hs_f) / C)
    sb = max(b, 1.0)
    a_stock = smoothstep(-sb * 0.5, sb, W - H_f)
    a_prop = smoothstep(-sb * 0.5, sb, W - Heff)
    return H_f, a_stock, a_prop, C


def facet_index(a, f, band):
    """mean |2nd difference| on tile-boundary lines vs interior, x and y."""
    d2x = np.abs(a[:, 2:] - 2 * a[:, 1:-1] + a[:, :-2])
    d2y = np.abs(a[2:, :] - 2 * a[1:-1, :] + a[:-2, :])
    bx = band[:, 1:-1]
    by = band[1:-1, :]
    colidx = np.arange(a.shape[1])[1:-1]
    rowidx = np.arange(a.shape[0])[1:-1]
    on_x = (colidx % f) == 0
    on_y = (rowidx % f) == 0
    vx_on = d2x[:, on_x][bx[:, on_x]]
    vx_off = d2x[:, ~on_x][bx[:, ~on_x]]
    vy_on = d2y[on_y, :][by[on_y, :]]
    vy_off = d2y[~on_y, :][by[~on_y, :]]
    on = np.concatenate([vx_on, vy_on])
    off = np.concatenate([vx_off, vy_off])
    mo = float(off.mean()) if off.size else 0.0
    return (float(on.mean()) / mo) if mo > 0 else None, float(on.mean()), mo


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    f = 16
    rep = []
    for nm in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        m = load_map(src / f"{nm}.pak", src / f"{nm}.fit")
        H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
        H_f, a_s, a_p, C = alpha_fields(H, W, b, 1.0, 1.0, f)
        # the shore ramp itself: where alpha is strictly between 0 and 1
        band = (a_s > 0.002) & (a_s < 0.998)
        fi_s = facet_index(a_s, f, band)
        fi_p = facet_index(a_p, f, band)
        # how wide is the ramp, in world units, per map
        area_wu2 = band.sum() * (WU / f) ** 2
        rep.append({"mission": nm, "water_elevation": W, "alpha_depth": b,
                    "ramp_area_wu2": area_wu2,
                    "ramp_pct_of_map": 100.0 * band.mean(),
                    "facet_index_stock": fi_s[0], "facet_index_proposed": fi_p[0],
                    "d2_on_lattice_stock": fi_s[1], "d2_interior_stock": fi_s[2],
                    "d2_on_lattice_proposed": fi_p[1], "d2_interior_proposed": fi_p[2]})
        print(f"{nm}: ramp covers {100.0*band.mean():5.2f}% of map  "
              f"facet_index stock={fi_s[0]:.3f}  proposed={fi_p[0]:.3f}  "
              f"(d2 on-lattice {fi_s[1]:.2e} vs interior {fi_s[2]:.2e})")
    (out / "alpha_band.json").write_text(json.dumps(rep, indent=1))

    # visual: mc2_17 intro neighbourhood, alpha field stock vs proposed
    for nm, cx, cy, half, tag in (
            ("mc2_17", -6656.0, -184.9, 1400, "intro"),
            ("mc2_17", 960.0, 320.0, 1800, "shelf_center"),
            ("mc2_10", 0.0, 0.0, 2200, "center")):
        m = load_map(src / f"{nm}.pak", src / f"{nm}.fit")
        H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
        H_f, a_s, a_p, C = alpha_fields(H, W, b, 1.0, 1.0, f)
        wpu = WU / f
        tlx, tly = -m["side"] * WU / 2.0, m["side"] * WU / 2.0
        gx, gy = cx - tlx, tly - cy
        i0, i1 = int((gy - half) / wpu), int((gy + half) / wpu)
        j0, j1 = int((gx - half) / wpu), int((gx + half) / wpu)
        i0, j0 = max(i0, 0), max(j0, 0)
        i1, j1 = min(i1, a_s.shape[0]), min(j1, a_s.shape[1])
        ext = [j0 * wpu, j1 * wpu, i1 * wpu, i0 * wpu]
        fig, axes = plt.subplots(1, 2, figsize=(22, 11), dpi=110)
        for ax, A, ttl in ((axes[0], a_s, "STOCK shore alpha (bilinear WaterThickness)"),
                           (axes[1], a_p, f"PROPOSED (B-spline residual, C={C:g} wu)")):
            ax.imshow(A[i0:i1, j0:j1], cmap="gray", extent=ext, origin="upper",
                      vmin=0, vmax=1, interpolation="nearest")
            ax.set_xticks(np.arange((j0 * wpu // WU) * WU, j1 * wpu + WU, WU))
            ax.set_yticks(np.arange((i0 * wpu // WU) * WU, i1 * wpu + WU, WU))
            ax.grid(True, color="#ff3333", alpha=0.35, linewidth=0.5)
            ax.tick_params(labelsize=5, rotation=90)
            ax.set_title(ttl, fontsize=11)
            ax.set_aspect("equal")
        fig.suptitle(f"{nm} — shore ALPHA ramp, {tag}   "
                     f"(red grid = 128 wu tile lattice; alphaDepth={b:g}, "
                     f"waterElevation={W:g})\n"
                     f"look for banding aligned to the red grid — that is the "
                     f"'stair stepping', independent of where the waterline sits",
                     fontsize=12)
        fig.tight_layout()
        p = out / f"{nm}_alpha_{tag}.png"
        fig.savefig(p)
        plt.close(fig)
        print(p)


if __name__ == "__main__":
    main()
