#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 5 — tile-scale zooms the user can grade.

Renders the stock waterline vs the proposed one over a 128 wu tile grid, so
"is this stair-stepping on the tile lattice?" is answerable by eye.

World->grid: mapTopLeft3d = (-side*128/2, +side*128/2)  (mclib/terrain.h:446)
             gx = worldX - topLeftX ; gy = topLeftY - worldY
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).parent))
from falsify import WU, load_map  # noqa: E402
from fine import fields, F  # noqa: E402


def world_to_grid(side, wx, wy):
    tlx, tly = -side * WU / 2.0, side * WU / 2.0
    return wx - tlx, tly - wy


def zoom(name, src, out, cx_w, cy_w, half, scale, k, tag, title_extra=""):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    H_f, Hs_f, Heff, C, _ = fields(H, scale, k, b)
    wpu = WU / F
    n = H_f.shape[0]
    xs = np.arange(n) * wpu
    Xg, Yg = np.meshgrid(xs, xs)
    gx, gy = world_to_grid(m["side"], cx_w, cy_w)

    fig, ax = plt.subplots(figsize=(13, 13), dpi=120)
    ax.imshow(np.clip((H_f - (W - 60)) / 120.0, 0, 1), cmap="gist_earth",
              extent=[0, n * wpu, n * wpu, 0], interpolation="bilinear",
              alpha=0.75, origin="upper")
    ax.contour(Xg, Yg, H_f, levels=[W], colors=["#000000"], linewidths=3.0)
    ax.contour(Xg, Yg, H_f, levels=[W + b / 2.0], colors=["#000000"],
               linewidths=1.0, linestyles="dotted")
    ax.contour(Xg, Yg, Hs_f, levels=[W], colors=["#1b9e77"], linewidths=1.4,
               linestyles="dashed")
    ax.contour(Xg, Yg, Heff, levels=[W], colors=["#e6550d"], linewidths=2.4)
    ax.set_xlim(gx - half, gx + half)
    ax.set_ylim(gy + half, gy - half)
    ax.set_aspect("equal")
    # 128 wu tile lattice
    t0x = (int((gx - half) // WU)) * WU
    t0y = (int((gy - half) // WU)) * WU
    ax.set_xticks(np.arange(t0x, gx + half + WU, WU))
    ax.set_yticks(np.arange(t0y, gy + half + WU, WU))
    ax.grid(True, color="#3355aa", alpha=0.45, linewidth=0.6)
    ax.tick_params(labelsize=6, rotation=90)
    ax.set_xlabel("grid X (world units) — blue lines are the 128 wu TERRAIN TILE lattice")
    ax.set_ylabel("grid Y (world units)")
    ax.set_title(
        f"{name} {title_extra}\n"
        f"BLACK solid = STOCK waterline (elev {W:g})   BLACK dotted = stock outer "
        f"alpha edge (elev {W + b/2:g})\n"
        f"ORANGE = PROPOSED (B-spline residual, C={C:g} wu)   "
        f"GREEN dashed = unclamped smooth reconstruction\n"
        f"footprint radius {2*scale:g} tiles = {2*scale*WU:g} wu   |   "
        f"window {2*half:g} x {2*half:g} wu   |   world centre "
        f"({cx_w:g}, {cy_w:g})", fontsize=9)
    fig.tight_layout()
    p = out / f"{name}_zoom_{tag}.png"
    fig.savefig(p)
    plt.close(fig)
    return p


if __name__ == "__main__":
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    # mc2_17 mission-authored camera (the intro view): [Cameras] PositionX/Y
    print(zoom("mc2_17", src, out, -6656.0, -184.9, 1400, 1.0, 1.0, "intro_s1_k1",
               "INTRO VIEW (mission [Cameras] PositionX/Y = -6656, -185)"))
    print(zoom("mc2_17", src, out, -6656.0, -184.9, 1400, 2.0, 2.0, "intro_s2_k2",
               "INTRO VIEW — aggressive settings (footprint 4 tiles, C = full alphaDepth)"))
    # WATER-MEANDER-CLAMP-1 verdict poses
    print(zoom("mc2_17", src, out, 4032.0, -4800.0, 1800, 1.0, 1.0, "shelf_east_s1_k1",
               "bookmark shelf_east_wide"))
    print(zoom("mc2_17", src, out, 960.0, 320.0, 1800, 1.0, 1.0, "shelf_center_s1_k1",
               "bookmark shelf_center_wide"))
    # mc2_10: the map with the measurably worst tile-locked waterline
    print(zoom("mc2_10", src, out, 0.0, 0.0, 2200, 1.0, 1.0, "center_s1_k1",
               "map centre (mc2_10 has the highest measured axis-lock)"))
