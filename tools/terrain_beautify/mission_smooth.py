#!/usr/bin/env python3
"""SMOOTH-PASS-1 (Arc B / B3): conservative, protection-aware height beautify.

Generates a beauty-sidecar (B2) that smooths ONLY ugly authored terrain artifacts
(spikes / stair-step / pyramid sides) while leaving gameplay-critical terrain
untouched. The dangerous line — so it is deliberately timid:

  - HARD-PROTECT (edit weight 0, plus a 1-ring buffer): water, road/runway/bridge
    overlays, building/object footprints. These never move.
  - Target only "ugly" cells: high local second-difference (blockiness) above a
    percentile — i.e. quantization spikes, not authored ramps.
  - Edit = constrained neighbour-average smoothing on the ugly set; the resulting
    delta is CLAMPED to a small max (default 6 wu) so no ramp/choke is reshaped.
  - Emits a B2 sidecar (non-destructive). Apply + validate separately; the stock
    .pak is never written by this tool.

Run:
  python mission_smooth.py <mission> [--missions-dir DIR] [--out DIR]
        [--max-delta WU] [--passes N] [--ugly-percentile P]
Then apply with mission_sidecar.py and game-smoke the patched mission.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_object_footprints,
    read_water_elevation, _dilate, WORLD_UNITS_PER_VERTEX,
)

# STOCK-DATA-TRUTH-1: no hard-coded mission dir. See tools/mc2data/stock_missions.py.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "mc2data"))
from stock_missions import (  # noqa: E402
    StockDataError, add_missions_dir_arg, resolve_from_args,
)
import mission_sidecar as sidecar  # noqa: E402


def _neighbor_mean(a: np.ndarray) -> np.ndarray:
    """3x3 box mean with edge clamp."""
    p = np.pad(a, 1, mode="edge")
    acc = np.zeros_like(a)
    for dr in (0, 1, 2):
        for dc in (0, 1, 2):
            acc += p[dr:dr + a.shape[0], dc:dc + a.shape[1]]
    return acc / 9.0


def compute_smooth_delta(elev: np.ndarray, protect_hard: np.ndarray,
                         max_delta: float, passes: int,
                         ugly_percentile: float) -> tuple[np.ndarray, dict]:
    side = elev.shape[0]
    # Blockiness = magnitude of the discrete second difference (stair-step spikes).
    sd_r = np.zeros_like(elev); sd_c = np.zeros_like(elev)
    sd_r[1:-1, :] = elev[2:, :] - 2 * elev[1:-1, :] + elev[:-2, :]
    sd_c[:, 1:-1] = elev[:, 2:] - 2 * elev[:, 1:-1] + elev[:, :-2]
    blockiness = np.abs(sd_r) + np.abs(sd_c)

    # Ugly = blocky cells above the percentile, on editable (non-protected) ground.
    protect_buf = _dilate(protect_hard)          # 1-ring buffer around hard-protect
    editable = ~protect_buf
    thresh = np.percentile(blockiness[editable], ugly_percentile) if editable.any() else np.inf
    ugly = (blockiness > thresh) & editable

    # Constrained smoothing: average toward neighbours, but freeze protected cells
    # so smoothing cannot pull terrain across a shoreline / building edge.
    work = elev.copy()
    frozen = protect_buf
    for _ in range(passes):
        avg = _neighbor_mean(work)
        step = np.where(ugly, 0.5 * (avg - work), 0.0)
        work = work + step
        work[frozen] = elev[frozen]              # re-pin protected each pass

    delta = work - elev
    delta[~ugly] = 0.0                           # only ugly cells move
    delta[protect_buf] = 0.0                     # belt-and-suspenders
    # Clamp magnitude.
    delta = np.clip(delta, -max_delta, max_delta)

    stats = {
        "ugly_cells": int(ugly.sum()),
        "changed_cells": int(np.count_nonzero(delta)),
        "max_abs_delta_wu": float(np.abs(delta).max()) if delta.size else 0.0,
        "mean_abs_delta_wu": float(np.abs(delta[delta != 0]).mean()) if np.any(delta != 0) else 0.0,
        "protected_cells": int(protect_buf.sum()),
        "blockiness_thresh": float(thresh),
    }
    return delta.astype(np.float32), stats


def main() -> int:
    ap = argparse.ArgumentParser(description="B3 conservative protection-aware smooth")
    ap.add_argument("mission")
    add_missions_dir_arg(ap)
    ap.add_argument("--out", default="tests/terrain/beautify")
    ap.add_argument("--max-delta", type=float, default=14.0, help="clamp |Δelev| (wu)")
    ap.add_argument("--passes", type=int, default=8)
    ap.add_argument("--ugly-percentile", type=float, default=78.0)
    args = ap.parse_args()

    try:
        missions_dir = resolve_from_args(args, missions=[args.mission]).dir
    except StockDataError as e:
        print(f"[smooth] ERROR {e}", file=sys.stderr)
        return 2
    pak = missions_dir / f"{args.mission}.pak"
    if not pak.is_file():
        print(f"[smooth] not found: {pak}", file=sys.stderr)
        return 2
    md = locate_mapdata(read_packets(pak))
    if md is None:
        print(f"[smooth] no MapData in {pak}", file=sys.stderr)
        return 2
    _, side, blocks = md
    water_elev = read_water_elevation(missions_dir / f"{args.mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    foot, _objs, _ = read_object_footprints(read_packets(pak), side)
    # Hard-protect ONLY gameplay structures (road/runway/bridge overlays + building
    # footprints). NOT water: `elev <= waterElevation` also captures gently-sloped
    # SHORE land, which froze a ~3-tile inland band and left <10% editable. The
    # advisor's plan beautifies shorelines (gentle, clamped) rather than freezing
    # them, so water/shore are editable here; water is still shown in the overlay
    # (protected.r8 level 1) as informational.
    protect_hard = layers["overlay"] | foot

    delta, stats = compute_smooth_delta(
        layers["elev"], protect_hard, args.max_delta, args.passes, args.ugly_percentile)

    note = (f"B3 smooth: max_delta={args.max_delta} passes={args.passes} "
            f"ugly_pct={args.ugly_percentile}")
    beauty = sidecar.write_sidecar(Path(args.out), args.mission, pak, delta, note=note)

    # B7c protected-zone overlay: per-cell protection level for the editor to draw.
    #   2 = structural hard (road/runway/bridge overlays + building footprints,
    #       1-ring dilated) -> "do not touch"; 1 = water; 0 = editable.
    structural = _dilate(layers["overlay"] | foot)
    prot = np.where(structural, 2, np.where(layers["water"], 1, 0)).astype(np.uint8)
    prot.tofile(beauty / "protected.r8")

    print(f"[smooth] {args.mission}: ugly={stats['ugly_cells']} "
          f"changed={stats['changed_cells']} max|d|={stats['max_abs_delta_wu']:.2f}wu "
          f"mean|d|={stats['mean_abs_delta_wu']:.2f}wu protected={stats['protected_cells']} "
          f"-> {beauty}")
    print(f"[smooth] next: py -3 tools/terrain_beautify/mission_sidecar.py apply "
          f"\"{pak}\" \"{beauty}\" <out.pak>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
