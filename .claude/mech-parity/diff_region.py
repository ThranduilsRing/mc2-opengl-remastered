#!/usr/bin/env python3
"""Co-covered mech parity diff. Compares two PNGs over a region; reports
coverage (pixels differing > threshold) and mean/max per-channel abs diff over
the COVERED (differing) pixels and over the whole region."""
import sys
from PIL import Image
import numpy as np

def load(p): return np.asarray(Image.open(p).convert("RGB"), dtype=np.int32)

def report(a_path, b_path, box=None, thr=8):
    a, b = load(a_path), load(b_path)
    h = min(a.shape[0], b.shape[0]); w = min(a.shape[1], b.shape[1])
    a, b = a[:h,:w], b[:h,:w]
    if box:
        x0,y0,x1,y1 = box
        a, b = a[y0:y1, x0:x1], b[y0:y1, x0:x1]
    d = np.abs(a - b)
    dmax_pix = d.max(axis=2)                     # per-pixel max channel diff
    covered = dmax_pix > thr
    ncov = int(covered.sum()); total = d.shape[0]*d.shape[1]
    print(f"  region {d.shape[1]}x{d.shape[0]} = {total} px; covered(>{thr})={ncov} ({100.0*ncov/total:.2f}%)")
    for ci,cn in enumerate("RGB"):
        ch = d[...,ci]
        mean_all = ch.mean(); max_all = ch.max()
        mean_cov = ch[covered].mean() if ncov else 0.0
        print(f"    {cn}: mean_all={mean_all:6.3f} max_all={max_all:3d} mean_over_covered={mean_cov:6.3f}")
    # overall
    print(f"    ALL: mean_all={d.mean():6.3f} max_all={d.max():3d} "
          f"mean_over_covered={d[covered].mean() if ncov else 0.0:6.3f}")

if __name__ == "__main__":
    d = sys.argv[1]
    # mech region from the capture (health bars + mechs), plus whole frame.
    box = (400, 100, 620, 200)
    print(f"== BEFORE: gl vs vk_flat (mech region {box}) ==")
    report(f"{d}/gl.png", f"{d}/vk_flat.png", box)
    print(f"== AFTER: gl vs vk_mat (mech region {box}) ==")
    report(f"{d}/gl.png", f"{d}/vk_mat.png", box)
    print(f"== AFTER: gl vs vk_mat (WHOLE FRAME) ==")
    report(f"{d}/gl.png", f"{d}/vk_mat.png", None)
    print(f"== SUBSTRATE CONTROL: gl vs vk_flat (WHOLE FRAME, non-mech should be ~0) ==")
    report(f"{d}/gl.png", f"{d}/vk_flat.png", None)
