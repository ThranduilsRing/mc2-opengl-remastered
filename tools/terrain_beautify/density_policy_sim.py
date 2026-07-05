#!/usr/bin/env python3
"""Terrain Surface Truth Arc — Density-Policy simulator (slice 2 evidence).

Answers ruling #2 OFFLINE, before any engine build: "can biasing the existing
per-block LOD selection by curvature/slope/ridge importance beat distance-only
LOD *at the same triangle budget*?" If yes -> the cheap chunk-bias engine slice
is justified. If no -> evidence for graduating to full CDLOD.

Model (deliberately mirrors the live LOD-chunk path, not a rewrite):
  - The map is partitioned into fixed square BLOCKS (the engine's LOD chunks).
  - Each block is assigned ONE stride from LOD_STEPS = {1,2,4,5,10,20}
    (mclib/terrain.cpp) -> a per-block LOD map.
  - The drawn surface is reconstructed per block (decimate + bilinear, reusing
    skyline_oracle) and scored by the oracle. Triangle budget = sum over blocks
    of (cells/stride)^2 * 2.

Two policies compared at EQUAL triangle budget:
  - distance:   coarser with radial distance from the camera ground point
                (top-down RTS), importance ignored.
  - importance: start from the distance allocation, then REBALANCE within the
                same budget -- promote the highest-importance blocks one LOD
                finer, pay for it by demoting the lowest-importance blocks one
                LOD coarser, until no beneficial swap keeps the budget.

The oracle then reports whether the importance allocation reduced ridge/cliff/
silhouette error for the same triangle count. Seam/crack fidelity is NOT modeled
(that is the engine tessellation slice's concern); this measures where vertices
are best SPENT.

Pure numpy. No engine, no GL.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skyline_oracle as so  # noqa: E402
import terrain_importance as ti  # noqa: E402

LOD_STEPS = (1, 2, 4, 5, 10, 20)   # mclib/terrain.cpp LOD_STEPS


def block_grid(n: int, block: int):
    """Half-open [start,end) row/col spans partitioning an n-sized axis."""
    edges = list(range(0, n, block))
    if edges[-1] != n:
        edges.append(n)
    return [(edges[i], edges[i + 1]) for i in range(len(edges) - 1)]


def _blocks(n: int, m: int, block: int):
    rows = block_grid(n, block)
    cols = block_grid(m, block)
    return [(r0, r1, c0, c1) for (r0, r1) in rows for (c0, c1) in cols]


def block_triangles(r0, r1, c0, c1, stride: int) -> int:
    """Triangle count for a block drawn at `stride` (2 tris per grid cell)."""
    rc = max(1, (r1 - r0) // stride)
    cc = max(1, (c1 - c0) // stride)
    return rc * cc * 2


def total_triangles(fine, blocks, lod_idx) -> int:
    return int(sum(block_triangles(*b, LOD_STEPS[lod_idx[i]])
                   for i, b in enumerate(blocks)))


def reconstruct(fine, blocks, lod_idx) -> np.ndarray:
    """Assemble the drawn surface from per-block LOD choices (decimate+bilinear
    per block, reusing the oracle). Seams not stitched (out of scope)."""
    out = np.array(fine, dtype=np.float64)
    for i, (r0, r1, c0, c1) in enumerate(blocks):
        sub = fine[r0:r1 + 1 if r1 < fine.shape[0] else r1,
                   c0:c1 + 1 if c1 < fine.shape[1] else c1]
        rec = so.decimate_upsample(sub, LOD_STEPS[lod_idx[i]])
        out[r0:r1, c0:c1] = rec[:r1 - r0, :c1 - c0]
    return out


def block_importance(imp, blocks) -> np.ndarray:
    """Mean importance per block (the density signal from Layer 1)."""
    return np.array([float(imp[r0:r1, c0:c1].mean()) for (r0, r1, c0, c1) in blocks])


def distance_policy(fine, blocks, camera_xy, thresholds) -> np.ndarray:
    """Per-block LOD index chosen by radial distance from the camera ground
    point (top-down RTS). thresholds = ascending distances at which LOD steps
    one level coarser."""
    cx, cy = camera_xy
    lod = np.zeros(len(blocks), dtype=int)
    for i, (r0, r1, c0, c1) in enumerate(blocks):
        bc_y = 0.5 * (r0 + r1)
        bc_x = 0.5 * (c0 + c1)
        d = float(np.hypot(bc_x - cx, bc_y - cy))
        level = int(np.searchsorted(thresholds, d))
        lod[i] = min(level, len(LOD_STEPS) - 1)
    return lod


def _block_tris(blocks, lod, i):
    return block_triangles(*blocks[i], LOD_STEPS[lod[i]])


def importance_rebalance(fine, blocks, lod_start, imp_blocks,
                         max_passes: int = 4, cand: int = 96) -> np.ndarray:
    """Within the distance allocation's triangle budget, promote high-importance
    blocks one LOD finer and pay by demoting low-importance blocks one LOD
    coarser. Greedy, deterministic (importance-sorted). Bounded to the top/bottom
    `cand` importance extremes and `max_passes` sweeps so it scales to thousands
    of blocks (the rebalance effect is dominated by the extremes)."""
    budget = total_triangles(fine, blocks, lod_start)
    lod = np.array(lod_start, dtype=int)
    order_fine = np.argsort(-imp_blocks)[:cand]    # promote candidates
    order_coarse = np.argsort(imp_blocks)[:cand]   # demote candidates
    cur = total_triangles(fine, blocks, lod)
    for _ in range(max_passes):
        changed = False
        for pi in order_fine:
            if lod[pi] == 0:
                continue
            cost = (block_triangles(*blocks[pi], LOD_STEPS[lod[pi] - 1])
                    - _block_tris(blocks, lod, pi))     # tris ADDED by promote
            for di in order_coarse:
                if di == pi or lod[di] >= len(LOD_STEPS) - 1:
                    continue
                if imp_blocks[di] >= imp_blocks[pi]:
                    break                                # never demote >= promoted
                saved = (_block_tris(blocks, lod, di)
                         - block_triangles(*blocks[di], LOD_STEPS[lod[di] + 1]))
                if cur + cost - saved <= budget:
                    lod[pi] -= 1
                    lod[di] += 1
                    cur = cur + cost - saved
                    changed = True
                    break
        if not changed:
            break
    return lod


def compare(fine, imp, block=16, camera_xy=None, thresholds=None,
            wu_per_texel=1.0, slope_thresh_deg=40.0, floor_wu=0.1) -> dict:
    """Run both policies at equal budget and score with the oracle."""
    fine = np.asarray(fine, dtype=np.float64)
    n, m = fine.shape
    if camera_xy is None:
        camera_xy = (m / 2.0, n / 2.0)
    if thresholds is None:
        diag = float(np.hypot(n, m))
        thresholds = np.linspace(diag * 0.12, diag * 0.75, len(LOD_STEPS) - 1)
    blocks = _blocks(n, m, block)
    impb = block_importance(imp, blocks)

    lod_dist = distance_policy(fine, blocks, camera_xy, thresholds)
    lod_imp = importance_rebalance(fine, blocks, lod_dist, impb)

    tris_dist = total_triangles(fine, blocks, lod_dist)
    tris_imp = total_triangles(fine, blocks, lod_imp)
    rec_dist = reconstruct(fine, blocks, lod_dist)
    rec_imp = reconstruct(fine, blocks, lod_imp)

    ev_dist = so.evaluate(fine, rec_dist, wu_per_texel, slope_thresh_deg)
    ev_imp = so.evaluate(fine, rec_imp, wu_per_texel, slope_thresh_deg)

    def _delta(a, b):   # negative = importance policy is BETTER (less error)
        return b - a
    verdict = {
        "ridge_loss_mean": _delta(ev_dist["ridge_loss"]["mean"],
                                  ev_imp["ridge_loss"]["mean"]),
        "cliff_loss_mean": _delta(ev_dist["cliff_height_loss"]["mean"],
                                  ev_imp["cliff_height_loss"]["mean"]),
        "silhouette_l1": _delta(ev_dist["silhouette"]["l1_mean"],
                                ev_imp["silhouette"]["l1_mean"]),
    }
    # Count only improvements beyond a noise floor -- a ~0.00 wu delta is NOT a
    # win, and must not read as GO (a stock gentle map where distance-only LOD
    # already covers the relief will legitimately show negligible deltas).
    improved = sum(1 for v in verdict.values() if v < -floor_wu)
    worsened = sum(1 for v in verdict.values() if v > floor_wu)
    return {
        "blocks": len(blocks), "block": block, "floor_wu": floor_wu,
        "triangles": {"distance": tris_dist, "importance": tris_imp,
                      "budget_ratio": tris_imp / tris_dist if tris_dist else 1.0},
        "distance": ev_dist, "importance": ev_imp,
        "delta_importance_minus_distance": verdict,
        "metrics_improved": improved, "metrics_worsened": worsened,
        "GO": improved >= 2 and worsened == 0 and tris_imp <= tris_dist * 1.02,
    }


def format_report(res: dict) -> str:
    t = res["triangles"]
    d = res["delta_importance_minus_distance"]
    lines = [
        f"[density_policy_sim] {res['blocks']} blocks (block={res['block']})",
        f"  triangles  distance={t['distance']}  importance={t['importance']}  "
        f"(ratio {t['budget_ratio']:.3f}, equal-budget target)",
        "  delta (importance - distance; NEGATIVE = importance policy better):",
        f"    ridge_loss_mean  {d['ridge_loss_mean']:+.3f} wu",
        f"    cliff_loss_mean  {d['cliff_loss_mean']:+.3f} wu",
        f"    silhouette_l1    {d['silhouette_l1']:+.3f} wu",
        f"  metrics improved: {res['metrics_improved']}/3 "
        f"(> {res['floor_wu']} wu floor)   "
        f"VERDICT: {'GO (chunk-bias justified)' if res['GO'] else 'NO-GO (negligible on this map)'}",
    ]
    return "\n".join(lines)


def _load(beauty: Path):
    rep = json.loads((beauty / "visual_importance_report.json").read_text(encoding="utf-8"))
    v = int(rep["grid_v"])
    fine = np.fromfile(beauty / f"visual_height_{rep['factor']}x.r32",
                       dtype="<f4").astype(np.float64).reshape(v, v)
    blob = np.fromfile(beauty / "visual_importance.r32", dtype="<f4")
    imp = ti.unpack_blob(blob, v)["importance"]
    return fine, imp, float(rep.get("wu_per_texel", 1.0)), \
        float(rep.get("slope_thresh_deg", 40.0))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Offline density-policy GO/NO-GO simulator.")
    ap.add_argument("beauty", help=".beauty dir with visual_importance.r32 (run terrain_importance first)")
    ap.add_argument("--block", type=int, default=16)
    a = ap.parse_args(argv)
    fine, imp, wu, slope = _load(Path(a.beauty))
    res = compare(fine, imp, block=a.block, wu_per_texel=wu, slope_thresh_deg=slope)
    print(format_report(res))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
