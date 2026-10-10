#!/usr/bin/env python3
"""Compact before/after check: where did the frame change, outside the mech under test?

WHY: a full-size before/after/diff montage costs a lot of tokens to look at. This keeps
the whole AFTER frame at reduced scale with the changed regions boxed in red, and prints
one summary line, so a reviewer sees "what moved and where" in a single small image.

Usage:
  py -3 tools/visual_verify/montage_compact.py <before.png> <after.png> [--mech-box x,y,w,h]
        [--out compact.png] [--scale 0.25] [--threshold 200]
  py -3 tools/visual_verify/montage_compact.py --montage <name_before_after_diff.png> [...]
      (a side-by-side before | after | diff strip, split into equal thirds)

A pixel counts as changed when any channel differs by more than 8. Changed pixels are
grouped into clusters (connected after an 8 px dilation); a cluster whose bounding box lies
fully inside --mech-box (frame coordinates, the object being changed on purpose) is ignored.

Prints: DIFF outside_box=<px> clusters=<n> max_cluster=<w>x<h>@<x>,<y> inside_box=<px>
Exit: 0 ok; 1 outside_box > --threshold; 2 bad input (size mismatch, unreadable, bad box).
"""
from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image, ImageDraw

DELTA = 8
DILATE_PX = 8


def load_rgb(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"))


def split_montage(path: str) -> tuple[np.ndarray, np.ndarray]:
    """BEFORE and AFTER panels of a before | after | diff strip."""
    im = load_rgb(path)
    h, w, _ = im.shape
    if w % 3:
        raise ValueError(f"montage width {w} is not divisible by 3")
    t = w // 3
    return im[:, :t], im[:, t:2 * t]


def diff_mask(before: np.ndarray, after: np.ndarray) -> np.ndarray:
    d = np.abs(before.astype(np.int16) - after.astype(np.int16)).max(axis=2)
    return d > DELTA


def dilate(mask: np.ndarray, r: int) -> np.ndarray:
    """Square dilation by r pixels (box sum over a (2r+1) window > 0)."""
    if r <= 0:
        return mask
    p = np.pad(mask.astype(np.int32), r)
    c = np.pad(np.cumsum(p, axis=0), ((1, 0), (0, 0)))
    rows = c[2 * r + 1:] - c[:-(2 * r + 1)]
    c = np.pad(np.cumsum(rows, axis=1), ((0, 0), (1, 0)))
    return (c[:, 2 * r + 1:] - c[:, :-(2 * r + 1)]) > 0


def label_components(mask: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """8-connected components of `mask` via run-length runs.

    Returns (run_start_key, run_component, row_stride, n_components) where a pixel (y, x) belongs to
    the run found by searchsorted(run_start_key, y*stride + x, 'right') - 1.
    """
    h, w = mask.shape
    stride = w + 2
    padded = np.zeros((h, w + 2), dtype=np.int8)
    padded[:, 1:-1] = mask
    d = np.diff(padded, axis=1)
    sr, sc = np.nonzero(d == 1)   # run starts (column in padded coords == x of first pixel)
    er, ec = np.nonzero(d == -1)  # run ends, exclusive x
    startkey = sr.astype(np.int64) * stride + sc
    endkey = er.astype(np.int64) * stride + ec
    n = len(startkey)
    parent = np.arange(n)
    if n:
        # Runs of the row above that touch run i (8-connectivity): ends >= start_i and starts <= end_i.
        prev = (sr - 1).astype(np.int64) * stride
        lo = np.searchsorted(endkey, prev + sc, side="left")
        hi = np.searchsorted(startkey, prev + ec, side="right")
        valid = (sr > 0) & (hi > lo)
        cnt = np.where(valid, hi - lo, 0)
        a = np.repeat(np.arange(n), cnt)
        offs = np.arange(cnt.sum()) - np.repeat(np.cumsum(cnt) - cnt, cnt)
        b = np.repeat(lo, cnt) + offs
        while len(a):
            m = np.minimum(parent[a], parent[b])
            np.minimum.at(parent, a, m)
            np.minimum.at(parent, b, m)
            while True:
                jumped = parent[parent]
                if np.array_equal(jumped, parent):
                    break
                parent = jumped
            if np.array_equal(parent[a], parent[b]):
                break
    return startkey, parent, np.int64(stride), np.int64(n)


def clusters_of(mask: np.ndarray) -> list[dict]:
    """Clusters of changed pixels: bounding box of the changed pixels and their count."""
    ys, xs = np.nonzero(mask)
    if len(ys) == 0:
        return []
    startkey, comp, stride, _ = label_components(dilate(mask, DILATE_PX))
    run = np.searchsorted(startkey, ys.astype(np.int64) * stride + xs, side="right") - 1
    lab = comp[run]
    order = np.argsort(lab, kind="stable")
    lab, ys, xs = lab[order], ys[order], xs[order]
    first = np.flatnonzero(np.r_[True, lab[1:] != lab[:-1]])
    x0 = np.minimum.reduceat(xs, first)
    y0 = np.minimum.reduceat(ys, first)
    x1 = np.maximum.reduceat(xs, first)
    y1 = np.maximum.reduceat(ys, first)
    px = np.diff(np.r_[first, len(lab)])
    return [{"x": int(a), "y": int(b), "w": int(c - a + 1), "h": int(d - b + 1), "px": int(p)}
            for a, b, c, d, p in zip(x0, y0, x1, y1, px)]


def inside(c: dict, box: tuple[int, int, int, int]) -> bool:
    bx, by, bw, bh = box
    return c["x"] >= bx and c["y"] >= by and c["x"] + c["w"] <= bx + bw and c["y"] + c["h"] <= by + bh


def render(after: np.ndarray, clusters: list[dict], scale: float) -> Image.Image:
    img = Image.fromarray(after)
    out = img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.BOX)
    dr = ImageDraw.Draw(out)
    for c in clusters:
        x0, y0 = c["x"] * scale - 1, c["y"] * scale - 1
        x1, y1 = (c["x"] + c["w"]) * scale + 1, (c["y"] + c["h"]) * scale + 1
        dr.rectangle([x0, y0, x1, y1], outline=(255, 0, 0), width=1)
    return out


def parse_box(text: str) -> tuple[int, int, int, int]:
    parts = [int(p) for p in text.split(",")]
    if len(parts) != 4 or parts[2] <= 0 or parts[3] <= 0:
        raise ValueError("expected x,y,w,h with positive w and h")
    return parts[0], parts[1], parts[2], parts[3]


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="*", help="before.png after.png")
    ap.add_argument("--montage", help="a before|after|diff strip to split into thirds instead of two files")
    ap.add_argument("--mech-box", help="x,y,w,h in frame pixels; clusters fully inside are ignored")
    ap.add_argument("--out", default="compact.png")
    ap.add_argument("--scale", type=float, default=0.25)
    ap.add_argument("--threshold", type=int, default=200, help="exit 1 when outside_box exceeds this")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.montage:
            if args.images:
                raise ValueError("give either --montage or before/after, not both")
            before, after = split_montage(args.montage)
        else:
            if len(args.images) != 2:
                raise ValueError("expected <before.png> <after.png> (or --montage)")
            before, after = load_rgb(args.images[0]), load_rgb(args.images[1])
        if before.shape != after.shape:
            raise ValueError(f"size mismatch: before {before.shape[1]}x{before.shape[0]} "
                             f"vs after {after.shape[1]}x{after.shape[0]}")
        box = parse_box(args.mech_box) if args.mech_box else None
        if not 0 < args.scale <= 1:
            raise ValueError("--scale must be in (0, 1]")
    except (ValueError, OSError) as e:
        print(f"montage_compact: {e}", file=sys.stderr)
        return 2

    clusters = clusters_of(diff_mask(before, after))
    ignored = [c for c in clusters if box and inside(c, box)]
    kept = [c for c in clusters if not (box and inside(c, box))]
    outside_px = sum(c["px"] for c in kept)
    inside_px = sum(c["px"] for c in ignored)
    big = max(kept, key=lambda c: c["w"] * c["h"], default=None)
    biggest = f"{big['w']}x{big['h']}@{big['x']},{big['y']}" if big else "0x0@0,0"

    render(after, kept, args.scale).save(args.out)
    print(f"DIFF outside_box={outside_px} clusters={len(kept)} max_cluster={biggest} inside_box={inside_px}")
    return 1 if outside_px > args.threshold else 0


if __name__ == "__main__":
    sys.exit(main())
