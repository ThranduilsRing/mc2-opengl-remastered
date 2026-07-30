#!/usr/bin/env python3
"""SHORELINE-V0-FALSIFY-1 part 6 — measure at the level the SILHOUETTE lives on.

The visible outer edge of water is NOT the elev = waterElevation iso-line.  The
water FS computes
    shore = smoothstep(-shoreBlend*0.5, shoreBlend, WaterThickness);
    if (shore <= 0.0) discard;
with shoreBlend = max(alphaDepth, 1) and WaterThickness = waterElevation - z,
so the discard boundary — the drawn silhouette — is the iso-line of

    z = waterElevation + alphaDepth/2

Part 4 measured the elev = waterElevation line.  This part measures BOTH.

Read-only; no engine.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
from scipy.spatial import cKDTree  # noqa: E402

sys.path.insert(0, str(Path(__file__).parent))
from falsify import WU, load_map, pct  # noqa: E402
from fine import F, fields, iso, densify, shape_metrics, resample_matrix, tent  # noqa: E402


def run(name, src, scales, ks):
    m = load_map(src / f"{name}.pak", src / f"{name}.fit")
    H, W, b = m["elev"], m["water_elev"], m["alpha_depth"]
    wpu = WU / F
    A = resample_matrix(H.shape[0], tent, 1.0)
    H_f = A @ H @ A.T
    out = {"mission": name, "water_elevation": W, "alpha_depth": b, "levels": {}}
    for lname, lev in (("iso_W", W), ("silhouette_W_plus_half_b", W + b / 2.0)):
        stock = iso(H_f, lev)
        sp = densify(stock)
        tree = cKDTree(sp * wpu)
        L = {"level_elev": lev, "stock": shape_metrics(stock, wpu), "rows": []}
        for s in scales:
            for k in ks:
                _, Hs_f, Heff, C, _ = fields(H, s, k, b)
                segs = iso(Heff, lev)
                pts = densify(segs)
                d, _ = tree.query(pts * wpu)
                sm = shape_metrics(segs, wpu)
                L["rows"].append({
                    "scale": s, "k": k, "C_wu": C,
                    "disp": pct(d), "disp_max": float(d.max()),
                    **{f"shape_{a}": bq for a, bq in sm.items()},
                })
        out["levels"][lname] = L
    return out


def main():
    src, outd = Path(sys.argv[1]), Path(sys.argv[2])
    res = []
    for nm in ("mc2_17", "mc2_10", "mc2_01", "mc2_05"):
        r = run(nm, src, [1.0, 2.0], [1.0, 2.0])
        res.append(r)
        print(f"===== {nm}  W={r['water_elevation']:g}  alphaDepth={r['alpha_depth']:g}")
        for lname, L in r["levels"].items():
            st = L["stock"]
            print(f"  [{lname} @ elev {L['level_elev']:g}]  STOCK stair={st['staircase_index']:.3f}"
                  f" corners/1000wu={st['corners_per_1000wu']:.1f}"
                  f" len={st['length_wu']:.0f} comps={st['n_components']}")
            for row in L["rows"]:
                print(f"     s={row['scale']:g} k={row['k']:g} C={row['C_wu']:g}: "
                      f"disp med={row['disp']['p50']:6.1f} p90={row['disp']['p90']:6.1f} "
                      f"max={row['disp_max']:6.1f}  stair={row['shape_staircase_index']:.3f} "
                      f"corners={row['shape_corners_per_1000wu']:5.1f} "
                      f"comps={row['shape_n_components']}")
    (outd / "fine_level.json").write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
