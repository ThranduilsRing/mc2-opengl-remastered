#!/usr/bin/env python3
"""Gameplay-height signature for .beauty sidecars (TERRAIN-BEAUTY-SIG-1).

A bake belongs to ONE gameplay height grid. The sidecar carries
`gameplay_height.sig` = "<mapSide> <fnv1a64 hex>" over (mapSide u32 LE, then the
mapSide*mapSide float32 elevations, row-major, LE). The engine
(mclib/terrain_init.cpp) recomputes it from the loaded pak and applies the bake
only on an exact match, so a mod that edits one plateau is rejected.

  height_sig.py --write            write the sig for every stock mission with a sidecar
  height_sig.py --report           per-mission |visual - gameplay| at unit-reachable cells
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))

FNV_OFF = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
M64 = 0xFFFFFFFFFFFFFFFF


def fnv1a64(data: bytes) -> int:
    h = FNV_OFF
    # numpy-free tight loop is slow for 40 KB; use a chunked python loop (fine for offline)
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def height_sig(elev: np.ndarray) -> str:
    """elev: (side, side) array of gameplay elevations. Returns 'side hex16'."""
    side = int(elev.shape[0])
    if elev.shape != (side, side):
        raise ValueError("elev must be square")
    payload = np.uint32(side).tobytes() + np.ascontiguousarray(elev, dtype="<f4").tobytes()
    return f"{side} {fnv1a64(payload):016x}"


def load_elev(mission: str, missions_dir: Path):
    import mission_terrain_analyzer as A
    md = A.locate_mapdata(A.read_packets(missions_dir / f"{mission}.pak"))
    if md is None:
        return None
    _, side, blocks = md
    water = A.read_water_elevation(missions_dir / f"{mission}.fit")
    return A.extract_layers(side, blocks, water)


def reachable_mask(elev: np.ndarray, water_elev: float) -> np.ndarray:
    """Approximation of nav passability per cell (editor rule): land, and no
    corner height difference over 128*sin(45) = 90.5 wu inside the tile."""
    c = [elev[:-1, :-1], elev[:-1, 1:], elev[1:, :-1], elev[1:, 1:]]
    hi = np.maximum.reduce(c)
    lo = np.minimum.reduce(c)
    land = lo > water_elev
    return land & ((hi - lo) <= 128.0 * np.sin(np.radians(45.0)))


def float_stats(elev: np.ndarray, vis: np.ndarray, water_elev: float) -> dict:
    side = elev.shape[0]
    F = (vis.shape[0] - 1) // (side - 1)
    # gameplay surface = bilinear of the coarse grid at the fine lattice
    from visual_heightfield import upsample_corner_pinned
    base = upsample_corner_pinned(elev, F)
    d = np.abs(vis.astype(np.float64) - base)
    ok = reachable_mask(elev, water_elev)
    # per cell: max over its F x F fine samples
    n = side - 1
    cellmax = np.zeros((n, n))
    cellmean = np.zeros((n, n))
    for i in range(F):
        for j in range(F):
            s = d[i:i + F * n:F, j:j + F * n:F][:n, :n]
            cellmax = np.maximum(cellmax, s)
            cellmean += s / (F * F)
    v = cellmax[ok]
    if v.size == 0:
        return {"reachable_cells": 0}
    return {
        "reachable_cells": int(v.size),
        "max_wu": round(float(v.max()), 1),
        "mean_of_cell_max_wu": round(float(v.mean()), 2),
        "mean_wu": round(float(cellmean[ok].mean()), 2),
        "p99_wu": round(float(np.percentile(v, 99)), 1),
        "cells_gt10": int((v > 10).sum()),
        "cells_gt30": int((v > 30).sum()),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--beauty-root", default=str(ROOT / "tests" / "terrain" / "beautify"))
    ap.add_argument("--missions-dir", default=None)
    ap.add_argument("--json-out", default=None)
    a = ap.parse_args()
    from stock_missions import resolve_missions_dir
    broot = Path(a.beauty_root)
    stems = sorted(p.name[:-7] for p in broot.glob("*.beauty") if p.is_dir())
    res = resolve_missions_dir(a.missions_dir, stems)
    mdir = Path(res.dir)
    out = {}
    for m in stems:
        if not (mdir / f"{m}.pak").is_file():
            continue
        L = load_elev(m, mdir)
        if L is None:
            continue
        elev = L["elev"].astype(np.float32)
        sig = height_sig(elev)
        if a.write:
            (broot / f"{m}.beauty" / "gameplay_height.sig").write_text(sig + "\n", encoding="ascii")
        vp = broot / f"{m}.beauty" / "visual_height_4x.r32"
        if a.report and vp.is_file():
            V = (elev.shape[0] - 1) * 4 + 1
            vis = np.fromfile(vp, dtype="<f4")
            if vis.size == V * V:
                out[m] = float_stats(elev.astype(np.float64), vis.reshape(V, V), float(L["water_elev"]))
                print(m, json.dumps(out[m]))
        if a.write:
            print(m, sig)
    if a.json_out:
        Path(a.json_out).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
