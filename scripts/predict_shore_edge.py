#!/usr/bin/env python3
"""predict_shore_edge.py -- OFFLINE water/land shore-edge validator (WATER-SHORE-EDGE-1 support).

Predicts, from shipped sidecar data alone (no game launch), what the water/land
edge alpha looks like under each shore mode, and quantifies the staircase:

  mode 0 (legacy):   shoreWT = waterElevation - bilinear(COARSE height, 128wu lattice)
                     shore   = smoothstep(-shoreBlend*0.5, shoreBlend, shoreWT),
                     shoreBlend = max(alphaDepth, 1.0)
                     (coarse grid approximated as visual_height_4x[::4, ::4])
  mode 1 (shipped):  shoreline_mask.png (3810x3810 over 15360wu, ~4.03 wu/texel)
                     maskDistWu = (r - 0.5) * 256.0
                     shore      = 1.0 - smoothstep(-12.0, 6.0, maskDistWu)
  mode 2 (new):      shoreDist = waterElevation - bilinear(visual_height_4x, 32wu lattice)
                     shore     = smoothstep(-K, K*0.5, shoreDist)   (K ~ 6-8 wu)

Staircase metric: extract the alpha=0.5 boundary of the binarized shore map,
group boundary pixel-edges into maximal axis-aligned straight runs, report the
contour-length-weighted mean run length (wu) and % of contour sitting in runs
longer than a threshold (default 6 wu).

Usage:
  py -3 scripts/predict_shore_edge.py --mission mc2_17 --modes 0,1,2 --k 6 \
      [--roi X0 Y0 SIZE | --auto-roi east] [--step 1.0] \
      [--out scratch/user0801/shore_predict]

Inputs are resolved from (first hit wins):
  height: tests/terrain/beautify/<stem>.beauty/visual_height_4x.r32
          A:/Games/mc2-opengl/mc2-win64-v0.4/data/missions/<stem>.beauty/visual_height_4x.r32
  mask:   tests/terrain/beautify/<stem>/shoreline_mask.png (+ .bounds.txt)
  fit:    A:/Games/mc2-srcdata/missions/<stem>.fit  ([Water] Elevation, AlphaDepth)

Read-only w.r.t. the repo; writes PNGs + metrics JSON to --out only.
"""

import argparse
import json
import math
import os
import re
import sys

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANE_04 = "A:/Games/mc2-opengl/mc2-win64-v0.4/data/missions"
SRCDATA_MISSIONS = "A:/Games/mc2-srcdata/missions"

WORLD_MIN = -7680.0
WORLD_SIZE = 15360.0


# ---------------------------------------------------------------- loading

def find_height(stem, override=None):
    cands = [override] if override else [
        os.path.join(REPO, "tests/terrain/beautify", stem + ".beauty", "visual_height_4x.r32"),
        os.path.join(LANE_04, stem + ".beauty", "visual_height_4x.r32"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise FileNotFoundError("visual_height_4x.r32 for " + stem + " (tried: %s)" % cands)


def find_mask(stem, override=None):
    cands = [override] if override else [
        os.path.join(REPO, "tests/terrain/beautify", stem, "shoreline_mask.png"),
        os.path.join(LANE_04, stem + ".beauty", "shoreline_mask.png"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise FileNotFoundError("shoreline_mask.png for " + stem)


def load_r32(path):
    raw = np.fromfile(path, dtype="<f4")
    side = int(round(math.sqrt(raw.size)))
    if side * side != raw.size:
        raise ValueError("%s: %d floats is not square" % (path, raw.size))
    return raw.reshape(side, side)


def load_mask(path):
    arr = np.asarray(Image.open(path))
    if arr.ndim == 3:
        red = arr[..., 0].astype(np.float32) / 255.0
        alpha = (arr[..., 3].astype(np.float32) / 255.0) if arr.shape[-1] == 4 \
            else np.ones_like(red)
    else:
        red = arr.astype(np.float32) / 255.0
        alpha = np.ones_like(red)
    bounds_path = os.path.splitext(path)[0] + ".bounds.txt"
    if os.path.isfile(bounds_path):
        vals = [float(v) for v in open(bounds_path).read().split()[:4]]
    else:
        vals = [WORLD_MIN, -WORLD_MIN, WORLD_SIZE, WORLD_SIZE]
    return red, alpha, vals  # bounds: topLeftX topLeftY(=maxY) sizeX sizeY


def parse_fit_water(stem):
    """Return (waterElevation, alphaDepth) from the mission .fit [Water] block."""
    path = os.path.join(SRCDATA_MISSIONS, stem + ".fit")
    elev, adepth = 0.0, 0.0
    if os.path.isfile(path):
        txt = open(path, "r", errors="ignore").read()
        m = re.search(r"\[Water\](.*?)(\n\[|\Z)", txt, re.S)
        if m:
            blk = m.group(1)
            me = re.search(r"Elevation\s*=\s*([-\d.]+)", blk)
            ma = re.search(r"AlphaDepth\s*=\s*([-\d.]+)", blk)
            if me:
                elev = float(me.group(1))
            if ma:
                adepth = float(ma.group(1))
    else:
        print("WARN: no .fit at %s; waterElevation=0" % path)
    return elev, adepth


# ---------------------------------------------------------------- sampling

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def bilinear_grid(grid, gx, gy):
    """Sample grid[row, col] at fractional (col=gx, row=gy) coordinates."""
    h, w = grid.shape
    gx = np.clip(gx, 0.0, w - 1.001)
    gy = np.clip(gy, 0.0, h - 1.001)
    x0 = np.floor(gx).astype(np.int32)
    y0 = np.floor(gy).astype(np.int32)
    fx = gx - x0
    fy = gy - y0
    g = grid
    return ((g[y0, x0] * (1 - fx) + g[y0, x0 + 1] * fx) * (1 - fy) +
            (g[y0 + 1, x0] * (1 - fx) + g[y0 + 1, x0 + 1] * fx) * fy)


def sample_height_wu(grid, wu_per_vertex, wx, wy):
    """Grid vertex (0,0) sits at world (WORLD_MIN, WORLD_MIN); row=+y, col=+x."""
    return bilinear_grid(grid, (wx - WORLD_MIN) / wu_per_vertex,
                         (wy - WORLD_MIN) / wu_per_vertex)


def sample_mask_wu(mask, bounds, wx, wy):
    """World -> mask pixel. EMPIRICAL orientation (verified vs visual_height_4x,
    97% water-region agreement on mc2_17): mask row 0 = MIN y on disk, same
    handedness as the height grid. The shader's v-flip UV is compensated by the
    texture loader's row order, so world-space content alignment is this one."""
    tlx, tly, sx, sy = bounds
    u = (wx - tlx) / sx
    v = (wy - (tly - sy)) / sy
    h, w = mask.shape
    return bilinear_grid(mask, u * w - 0.5, v * h - 0.5)


def alpha_for_mode(mode, wx, wy, ctx):
    if mode == 0:
        sb = max(ctx["alphaDepth"], 1.0)
        wt = ctx["waterElev"] - sample_height_wu(ctx["coarse"], 128.0, wx, wy)
        return smoothstep(-sb * 0.5, sb, wt)
    if mode == 1:
        r = sample_mask_wu(ctx["mask"], ctx["maskBounds"], wx, wy)
        a = sample_mask_wu(ctx["maskA"], ctx["maskBounds"], wx, wy)
        d = (r - 0.5) * 256.0
        shore_m = 1.0 - smoothstep(-12.0, 6.0, d)
        # shader: mask only applies where sl.a > 0.5, and only via
        # shore = min(shore, shoreM) on top of the mode-0 depth term
        base = alpha_for_mode(0, wx, wy, ctx)
        return np.where(a > 0.5, np.minimum(base, shore_m), base)
    if mode == 2:
        k = ctx["K"]
        d = ctx["waterElev"] - sample_height_wu(ctx["visual"], ctx["visual_wupv"], wx, wy)
        return smoothstep(-k, k * 0.5, d)
    raise ValueError("mode " + str(mode))


# ---------------------------------------------------------------- staircase metric

def boundary_runs(binary, step_wu):
    """Maximal axis-aligned straight runs of the land/water pixel boundary.

    Horizontal boundary edges lie between vertically adjacent differing pixels;
    contiguous same-row edges chain into one run (and vice versa for vertical).
    Returns (runs_wu list, total_contour_wu).
    """
    runs = []
    # horizontal edges: rows where binary[r] != binary[r+1], per column
    hdiff = binary[:-1, :] != binary[1:, :]
    for r in range(hdiff.shape[0]):
        row = hdiff[r]
        # split into contiguous True runs
        idx = np.flatnonzero(row)
        if idx.size:
            splits = np.flatnonzero(np.diff(idx) > 1)
            for seg in np.split(idx, splits + 1):
                runs.append(len(seg) * step_wu)
    vdiff = binary[:, :-1] != binary[:, 1:]
    for c in range(vdiff.shape[1]):
        col = vdiff[:, c]
        idx = np.flatnonzero(col)
        if idx.size:
            splits = np.flatnonzero(np.diff(idx) > 1)
            for seg in np.split(idx, splits + 1):
                runs.append(len(seg) * step_wu)
    total = float(sum(runs))
    return runs, total


def staircase_metrics(alpha, step_wu, thresh_wu):
    binary = alpha >= 0.5
    runs, total = boundary_runs(binary, step_wu)
    if not runs:
        return {"contour_wu": 0.0, "mean_run_wu": 0.0, "median_run_wu": 0.0,
                "pct_in_runs_gt_thresh": 0.0, "n_runs": 0}
    runs = np.array(runs)
    # contour-length weighting: a run of length L contributes L of contour
    long_len = float(runs[runs > thresh_wu].sum())
    return {
        "contour_wu": round(total, 1),
        "mean_run_wu": round(float((runs * runs).sum() / total), 2),  # length-weighted mean
        "median_run_wu": round(float(np.median(runs)), 2),
        "pct_in_runs_gt_thresh": round(100.0 * long_len / total, 1),
        "n_runs": int(len(runs)),
    }


# ---------------------------------------------------------------- ROI

def auto_roi(mask, bounds, size_wu, region):
    """Pick the size_wu window with the most shoreline contour texels
    (|signed dist| < 8wu i.e. |r-0.5| < 8/256), restricted to a half-map region."""
    tlx, tly, sx, sy = bounds
    h, w = mask.shape
    near = np.abs(mask - 0.5) < (8.0 / 256.0)
    # also require the window to contain actual water (r well below 0.5)
    water = mask < 0.45
    px_wu = sx / w
    win = max(4, int(round(size_wu / px_wu)))
    # coarse scan with stride win//4
    stride = max(1, win // 4)
    best, best_rc = -1, (0, 0)
    y_min = tly - sy
    for r in range(0, h - win, stride):
        for c in range(0, w - win, stride):
            wx = tlx + (c + win / 2.0) * px_wu
            if region == "east" and wx < 0:
                continue
            if region == "west" and wx > 0:
                continue
            n = int(near[r:r + win, c:c + win].sum())
            if n > best and water[r:r + win, c:c + win].any():
                best, best_rc = n, (r, c)
    r, c = best_rc
    x0 = tlx + c * px_wu
    y0 = y_min + r * px_wu         # mask row 0 = min y (empirical orientation)
    return x0, y0, win * px_wu


# ---------------------------------------------------------------- rendering

def save_png(arr01, path):
    Image.fromarray((np.clip(arr01, 0, 1) * 255).astype(np.uint8)).save(path)


def contour_overlay(alpha):
    """RGB: greyscale alpha with the 0.5 boundary painted red."""
    g = (np.clip(alpha, 0, 1) * 255).astype(np.uint8)
    rgb = np.stack([g, g, g], axis=-1)
    b = alpha >= 0.5
    edge = np.zeros_like(b)
    edge[:-1, :] |= b[:-1, :] != b[1:, :]
    edge[:, :-1] |= b[:, :-1] != b[:, 1:]
    rgb[edge] = [255, 40, 40]
    return rgb


# ---------------------------------------------------------------- main

def run_mission(stem, modes, args, outdir):
    height_path = find_height(stem, args.height)
    mask_path = find_mask(stem, args.mask)
    visual = load_r32(height_path)
    mask, mask_a, bounds = load_mask(mask_path)
    water_elev, alpha_depth = parse_fit_water(stem)
    if args.water_elev is not None:
        water_elev = args.water_elev
    if args.alpha_depth is not None:
        alpha_depth = args.alpha_depth

    vside = visual.shape[0]
    visual_wupv = 32.0  # 4x sidecar convention (477 verts over 120-vert coarse map)
    coarse = visual[::4, ::4]

    print("[%s] height=%s (%dx%d, %.0fwu/v)  mask=%s (%dx%d, %.2fwu/texel)" %
          (stem, height_path, vside, vside, visual_wupv, mask_path,
           mask.shape[1], mask.shape[0], bounds[2] / mask.shape[1]))
    print("[%s] waterElevation=%.2f alphaDepth=%.2f K=%.1f" %
          (stem, water_elev, alpha_depth, args.k))

    if args.roi:
        x0, y0, size = args.roi
    else:
        x0, y0, size = auto_roi(mask, bounds, args.roi_size, args.auto_roi)
    print("[%s] ROI: x0=%.0f y0=%.0f size=%.0f wu" % (stem, x0, y0, size))

    step = args.step
    n = int(size / step)
    xs = x0 + (np.arange(n) + 0.5) * step
    ys = y0 + (np.arange(n) + 0.5) * step
    wx, wy = np.meshgrid(xs, ys)
    wy = wy[::-1]  # row 0 = north (max y) for image orientation

    ctx = {"coarse": coarse, "visual": visual, "visual_wupv": visual_wupv,
           "mask": mask, "maskA": mask_a, "maskBounds": bounds,
           "waterElev": water_elev, "alphaDepth": alpha_depth, "K": args.k}

    results = {}
    panels = []
    for mode in modes:
        alpha = alpha_for_mode(mode, wx, wy, ctx)
        m = staircase_metrics(alpha, step, args.thresh)
        results[mode] = m
        save_png(alpha, os.path.join(outdir, "%s_mode%d_alpha.png" % (stem, mode)))
        panels.append(contour_overlay(alpha))
        print("  mode %d: contour=%.0fwu  mean_run=%.2fwu  median=%.2fwu  "
              "%%>%.0fwu=%.1f%%  n_runs=%d" %
              (mode, m["contour_wu"], m["mean_run_wu"], m["median_run_wu"],
               args.thresh, m["pct_in_runs_gt_thresh"], m["n_runs"]))

    if panels:
        gap = 8
        h = panels[0].shape[0]
        strip = np.full((h, sum(p.shape[1] for p in panels) + gap * (len(panels) - 1), 3),
                        30, dtype=np.uint8)
        x = 0
        for p in panels:
            strip[:, x:x + p.shape[1]] = p
            x += p.shape[1] + gap
        combo = os.path.join(outdir, "%s_modes_%s_contours.png" %
                             (stem, "-".join(str(m) for m in modes)))
        Image.fromarray(strip).save(combo)
        print("  side-by-side: %s" % combo)

    return {"roi": [x0, y0, size], "waterElevation": water_elev,
            "alphaDepth": alpha_depth, "K": args.k, "step_wu": step,
            "modes": {str(k): v for k, v in results.items()}}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mission", required=True, help="mission stem, e.g. mc2_17")
    ap.add_argument("--modes", default="0,1,2")
    ap.add_argument("--k", type=float, default=6.0, help="mode-2 smoothstep K (wu)")
    ap.add_argument("--step", type=float, default=1.0, help="sample spacing (wu)")
    ap.add_argument("--thresh", type=float, default=6.0,
                    help="staircase run-length threshold (wu)")
    ap.add_argument("--roi", nargs=3, type=float, metavar=("X0", "Y0", "SIZE"),
                    help="ROI: min-x min-y size, world units")
    ap.add_argument("--auto-roi", default="east", choices=["east", "west", "any"],
                    help="auto-pick densest shoreline window in this half")
    ap.add_argument("--roi-size", type=float, default=1536.0)
    ap.add_argument("--water-elev", type=float, default=None)
    ap.add_argument("--alpha-depth", type=float, default=None)
    ap.add_argument("--height", help="override visual_height_4x.r32 path")
    ap.add_argument("--mask", help="override shoreline_mask.png path")
    ap.add_argument("--out", default=os.path.join(REPO, "scratch/user0801/shore_predict"))
    args = ap.parse_args()

    modes = [int(m) for m in args.modes.split(",")]
    os.makedirs(args.out, exist_ok=True)
    report = run_mission(args.mission, modes, args, args.out)
    jpath = os.path.join(args.out, args.mission + "_metrics.json")
    with open(jpath, "w") as f:
        json.dump(report, f, indent=2)
    print("metrics: %s" % jpath)


if __name__ == "__main__":
    sys.exit(main())
