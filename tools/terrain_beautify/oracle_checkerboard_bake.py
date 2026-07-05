#!/usr/bin/env python3
"""TERRAIN-DISPLACEMENT-TRUTH-1 oracle bake: a BRUTAL synthetic visual-height
field for proving MC2_TERRAIN_VISUAL_DISPLACE actually moves geometry.

It takes an existing real corner-pinned `visual_height_4x.r32` bake and ADDS a
large checkerboard offset (default +150 wu on 500-wu squares) on top of it. If
the renderer's displacement path works, the terrain silhouette must break into
a gross regular ridge/valley pattern; a gate-OFF vs gate-ON pixel diff of the
same deterministic camera is then a truth oracle:
  * near-zero diff  -> displacement is BROKEN (SSBO not consumed / branch dead)
  * massive diff    -> displacement works; measure which screen fraction moved.

Corner-pinning is deliberately VIOLATED (we want visible chaos), so this bake
must NEVER be shipped/deployed as a real beautify layer -- oracle only.

Usage:
  oracle_checkerboard_bake.py --in  <real visual_height_4x.r32>
                              --out <oracle visual_height_4x.r32>
                              [--amp 150] [--square-wu 500] [--fine-wu 32]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


def make_checkerboard(base: np.ndarray, amp: float, square_verts: int) -> np.ndarray:
    V = base.shape[0]
    ii = np.arange(V)[:, None]
    jj = np.arange(V)[None, :]
    # (row_block + col_block) parity -> checkerboard of `square_verts`-sized cells.
    cb = ((ii // square_verts) + (jj // square_verts)) % 2  # 0 or 1
    return base + cb.astype(np.float64) * amp


def main() -> int:
    ap = argparse.ArgumentParser(description="Brutal synthetic displacement oracle bake.")
    ap.add_argument("--in", dest="src", required=True, help="real visual_height_4x.r32")
    ap.add_argument("--out", dest="dst", required=True, help="oracle output .r32")
    ap.add_argument("--amp", type=float, default=150.0, help="checkerboard offset (wu)")
    ap.add_argument("--square-wu", type=float, default=500.0, help="checker square size (wu)")
    ap.add_argument("--fine-wu", type=float, default=32.0, help="fine vertex spacing (wu)")
    args = ap.parse_args()

    a = np.fromfile(args.src, dtype="<f4")
    V = int(round(len(a) ** 0.5))
    if V * V != len(a):
        print(f"[oracle] ERROR: {args.src} is not square: {len(a)} floats", file=sys.stderr)
        return 2
    base = a.reshape(V, V).astype(np.float64)
    square_verts = max(1, int(round(args.square_wu / args.fine_wu)))
    out = make_checkerboard(base, args.amp, square_verts)
    Path(args.dst).parent.mkdir(parents=True, exist_ok=True)
    out.astype("<f4").tofile(args.dst)
    print(f"[oracle] V={V} square={square_verts} fine-verts amp={args.amp}wu "
          f"in[min={base.min():.0f} max={base.max():.0f}] "
          f"out[min={out.min():.0f} max={out.max():.0f}] -> {args.dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
