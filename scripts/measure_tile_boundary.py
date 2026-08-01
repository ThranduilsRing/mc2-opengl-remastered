#!/usr/bin/env python3
"""measure_tile_boundary.py -- adjacent-strip tone measurement across a tile boundary.

Motivation (2026-08-01, mc2_23/mc2_24 concrete complaint): wide-area or point
sampling is confounded by smooth lighting gradients (~32 luma across an apron)
and by accidentally sampling roads/shadows. The robust method is to sample two
THIN strips immediately adjacent to a known tile boundary: lighting is
near-constant across a few pixels by construction, so any per-side delta is
material/modulation tone, not lighting.

Usage:
  py -3 scripts/measure_tile_boundary.py IMAGE.png \
      --boundary "label:x1,y1,x2,y2" [--boundary ...] \
      [--width 6] [--gap 1] [--json out.json]

Each --boundary is a line segment lying ON the boundary (screen pixels).
Side A = left of the segment direction (x1,y1)->(x2,y2); side B = right.
Strips of --width px are sampled starting --gap px off the line on each side,
stepped ~1px along the segment (bilinear).

Reports per side: mean/median RGB, mean luma (Rec.601), and A-B deltas.
A control boundary (a segment drawn inside one uniform tile) should show
|delta luma| < ~2; a real tone break shows much more.

Boundary coords are chosen by eye from crops (dump with PIL, inspect); no
auto-detection -- screen-space tile grids are perspective-diagonal and Hough
without cv2 is not worth it.
"""

import argparse
import json
import sys

import numpy as np
from PIL import Image

LUMA = np.array([0.299, 0.587, 0.114])


def bilinear(img, xs, ys):
    """img: HxWx3 float. xs, ys: 1-D float arrays. Returns Nx3."""
    h, w = img.shape[:2]
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 2)
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 2)
    fx = np.clip(xs - x0, 0.0, 1.0)[:, None]
    fy = np.clip(ys - y0, 0.0, 1.0)[:, None]
    p00 = img[y0, x0]
    p10 = img[y0, x0 + 1]
    p01 = img[y0 + 1, x0]
    p11 = img[y0 + 1, x0 + 1]
    return (p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy)
            + p01 * (1 - fx) * fy + p11 * fx * fy)


def sample_strip(img, x1, y1, x2, y2, gap, width, sign):
    """Sample a strip on one side (sign=+1 left, -1 right) of the segment."""
    dx, dy = x2 - x1, y2 - y1
    length = float(np.hypot(dx, dy))
    if length < 4:
        raise ValueError("boundary segment too short")
    ux, uy = dx / length, dy / length
    # left normal of (ux,uy) in image coords (y down): (uy, -ux)
    nx, ny = sign * uy, sign * -ux
    ts = np.arange(0.0, length, 1.0)
    ds = np.arange(gap + 0.5, gap + width, 1.0)
    T, D = np.meshgrid(ts, ds)
    xs = (x1 + T * ux + D * nx).ravel()
    ys = (y1 + T * uy + D * ny).ravel()
    return bilinear(img, xs, ys)


def stats(px):
    mean = px.mean(axis=0)
    med = np.median(px, axis=0)
    return {
        "n": int(px.shape[0]),
        "mean_rgb": [round(float(v), 2) for v in mean],
        "median_rgb": [round(float(v), 2) for v in med],
        "mean_luma": round(float(mean @ LUMA), 2),
        "std_luma": round(float((px @ LUMA).std()), 2),
    }


def parse_boundary(spec):
    if ":" in spec:
        label, coords = spec.split(":", 1)
    else:
        label, coords = spec, spec
    vals = [float(v) for v in coords.split(",")]
    if len(vals) != 4:
        raise argparse.ArgumentTypeError(
            "boundary must be 'label:x1,y1,x2,y2' (got %r)" % spec)
    return label, vals


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image")
    ap.add_argument("--boundary", action="append", required=True,
                    type=parse_boundary, metavar="label:x1,y1,x2,y2",
                    help="segment on the boundary; repeatable")
    ap.add_argument("--width", type=float, default=6.0,
                    help="strip width in px per side (default 6)")
    ap.add_argument("--gap", type=float, default=1.0,
                    help="px to skip next to the line (AA/filter blend, default 1)")
    ap.add_argument("--json", help="write results JSON here")
    args = ap.parse_args()

    img = np.asarray(Image.open(args.image).convert("RGB"), dtype=np.float64)

    results = []
    for label, (x1, y1, x2, y2) in args.boundary:
        a = sample_strip(img, x1, y1, x2, y2, args.gap, args.width, +1)
        b = sample_strip(img, x1, y1, x2, y2, args.gap, args.width, -1)
        sa, sb = stats(a), stats(b)
        delta = {
            "mean_rgb": [round(sa["mean_rgb"][i] - sb["mean_rgb"][i], 2)
                         for i in range(3)],
            "mean_luma": round(sa["mean_luma"] - sb["mean_luma"], 2),
        }
        results.append({"label": label,
                        "segment": [x1, y1, x2, y2],
                        "side_a_left": sa, "side_b_right": sb,
                        "delta_a_minus_b": delta})

    for r in results:
        sa, sb, d = r["side_a_left"], r["side_b_right"], r["delta_a_minus_b"]
        print("[%s] seg=%s" % (r["label"], r["segment"]))
        print("  A(left)  meanRGB=%s luma=%.2f (std %.2f)"
              % (sa["mean_rgb"], sa["mean_luma"], sa["std_luma"]))
        print("  B(right) meanRGB=%s luma=%.2f (std %.2f)"
              % (sb["mean_rgb"], sb["mean_luma"], sb["std_luma"]))
        print("  DELTA A-B: luma %+.2f  RGB %s" % (d["mean_luma"], d["mean_rgb"]))

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"image": args.image, "width_px": args.width,
                       "gap_px": args.gap, "boundaries": results}, f, indent=2)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
