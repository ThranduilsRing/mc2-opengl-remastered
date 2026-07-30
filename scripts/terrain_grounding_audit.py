#!/usr/bin/env python3
"""TERRAIN-GROUNDING-AUDIT-1: drift between gameplay height and resampled-render height.

Reads MC2 mission .pak files, extracts the source heightfield, and
estimates how much the rendered ground would shift if we replaced the
current triangle-linear interpolation with the bilinear-resampled height
texture (which is what TERRAIN-NORMALS-FROM-HEIGHT-1 / TERRAIN-RESAMPLE-1
already feed into the shader). This is the precondition de-risk for a
future TERRAIN-DISPLACE-VISUAL-1: it quantifies where unit feet, building
footprints, and shadow projection would visually float or sink relative
to the gameplay-authoritative `getTerrainElevation()` value.

Read-only. No engine binary required. No gameplay or render mutation.

Reuses parsing from scripts/terrain_height_audit.py (TERRAIN-HEIGHT-
AUDIT-SCRIPT-1).
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import sys
from pathlib import Path

# Vendor the parser/locator from the slice-3 script.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from terrain_height_audit import (  # noqa: E402
    StockDataError,
    add_missions_dir_arg,
    resolve_from_args,
    DEFAULT_TIER1,
    DEFAULT_OUT_DIR,
    POSTCOMP_VERTEX_SIZE,
    ELEV_OFFSET,
    WORLD_UNITS_PER_VERTEX,
    STORAGE_NAME,
    read_packets,
    locate_mapdata,
    format_bytes,
)
import struct

# --- Drift sampler ------------------------------------------------------------

def _extract_elev_grid(side: int, blocks: bytes) -> list[float]:
    return [
        struct.unpack_from('<f', blocks, i * POSTCOMP_VERTEX_SIZE + ELEV_OFFSET)[0]
        for i in range(side * side)
    ]


def _bilinear(h00: float, h10: float, h01: float, h11: float,
              fx: float, fy: float) -> float:
    h0 = h00 * (1.0 - fx) + h10 * fx
    h1 = h01 * (1.0 - fx) + h11 * fx
    return h0 * (1.0 - fy) + h1 * fy


def _triangle_linear(h00: float, h10: float, h01: float, h11: float,
                     fx: float, fy: float, diag: str = "tl_br") -> float:
    """Triangle (barycentric) interpolation over a quad split by a diagonal.

    'tl_br' = top-left to bottom-right split (h00→h11).
       upper-left triangle: h00, h10, h11   (when fx + fy <= 1 swapped)
       lower-right triangle: h00, h01, h11
    'tr_bl' = top-right to bottom-left split (h10→h01).
    The renderer's actual diagonal depends on tile vertex ordering; we sample
    BOTH and report the max-error case so the bound is conservative.
    """
    if diag == "tl_br":
        if fx + fy <= 1.0:
            # Upper-left triangle (h00, h10, h01).
            return h00 * (1.0 - fx - fy) + h10 * fx + h01 * fy
        else:
            # Lower-right triangle (h11, h01, h10).
            u, v = 1.0 - fx, 1.0 - fy
            return h11 * (1.0 - u - v) + h10 * v + h01 * u
    else:  # "tr_bl"
        if fx >= fy:
            # Upper-right triangle (h00, h10, h11).
            return h00 * (1.0 - fx) + h10 * (fx - fy) + h11 * fy
        else:
            # Lower-left triangle (h00, h11, h01).
            return h00 * (1.0 - fy) + h01 * (fy - fx) + h11 * fx


# Sub-cell sample positions to probe. Cell midpoint is the canonical worst
# case for diagonal triangle interp vs bilinear. We probe a small grid so
# the histogram captures distribution rather than a single number.
SUBSAMPLE_POSITIONS = [
    (0.25, 0.25), (0.50, 0.25), (0.75, 0.25),
    (0.25, 0.50), (0.50, 0.50), (0.75, 0.50),
    (0.25, 0.75), (0.50, 0.75), (0.75, 0.75),
]


def compute_drift(side: int, elev: list[float]) -> dict:
    """Per-mission drift distribution between triangle-linear and bilinear."""
    drifts_tl_br: list[float] = []
    drifts_tr_bl: list[float] = []
    drifts_worst: list[float] = []  # max(tl_br, tr_bl) per probe — conservative

    def H(r, c): return elev[r * side + c]

    for r in range(side - 1):
        for c in range(side - 1):
            h00 = H(r,   c)
            h10 = H(r,   c+1)
            h01 = H(r+1, c)
            h11 = H(r+1, c+1)
            for fx, fy in SUBSAMPLE_POSITIONS:
                hb = _bilinear(h00, h10, h01, h11, fx, fy)
                ht1 = _triangle_linear(h00, h10, h01, h11, fx, fy, "tl_br")
                ht2 = _triangle_linear(h00, h10, h01, h11, fx, fy, "tr_bl")
                d1 = abs(hb - ht1)
                d2 = abs(hb - ht2)
                drifts_tl_br.append(d1)
                drifts_tr_bl.append(d2)
                drifts_worst.append(max(d1, d2))

    def stats(series: list[float]) -> dict:
        if not series:
            return {"n": 0}
        s = sorted(series)
        def pct(p): return s[max(0, min(len(s)-1, int(round(p*(len(s)-1)))))]
        return {
            "n":   len(s),
            "min": s[0],
            "p50": pct(0.50),
            "p90": pct(0.90),
            "p99": pct(0.99),
            "max": s[-1],
            "mean": sum(s) / len(s),
        }

    return {
        "grid_side":     side,
        "cells_probed":  (side - 1) * (side - 1),
        "samples_per_cell": len(SUBSAMPLE_POSITIONS),
        "tl_br_diagonal": stats(drifts_tl_br),
        "tr_bl_diagonal": stats(drifts_tr_bl),
        "worst_diagonal": stats(drifts_worst),
    }


def render_mission_section(name: str, path: Path, result: dict) -> str:
    if result.get("error"):
        return (f"### {name}\n\n"
                f"- Path: `{path}`\n"
                f"- **ERROR:** {result['error']}\n\n")
    m = result["metrics"]
    pkt = result["packet"]
    out = []
    out.append(f"### {name}\n")
    out.append(f"- Path: `{path}`")
    out.append(f"- MapData packet: index {pkt['index']} "
               f"(storage {STORAGE_NAME.get(pkt['storage'], pkt['storage'])})")
    out.append(f"- Grid: {m['grid_side']}×{m['grid_side']}; "
               f"{m['cells_probed']} cells × {m['samples_per_cell']} sub-positions "
               f"= {m['cells_probed'] * m['samples_per_cell']} drift samples\n")
    out.append("**Drift |bilinear − triangle-linear| in world units:**\n")
    out.append("| diagonal | n | min | p50 | p90 | p99 | max | mean |")
    out.append("|---|---|---|---|---|---|---|---|")
    for label, key in (("tl→br", "tl_br_diagonal"),
                       ("tr→bl", "tr_bl_diagonal"),
                       ("worst", "worst_diagonal")):
        s = m[key]
        out.append(
            f"| {label} | {s['n']} | {s['min']:.4f} | {s['p50']:.4f} | "
            f"{s['p90']:.4f} | {s['p99']:.4f} | {s['max']:.4f} | {s['mean']:.4f} |"
        )
    out.append("")
    # Quick interpretation guard-band — flag missions where worst-case max
    # drift exceeds 5 wu (≈ small-unit foot height) so the displace slice
    # knows whether a fade-out is required.
    worst_max = m["worst_diagonal"]["max"]
    if worst_max > 5.0:
        out.append(f"⚠ worst-case drift {worst_max:.2f} wu exceeds 5.0 wu threshold — "
                   f"near-unit displacement fade strongly recommended for this mission.")
    elif worst_max > 1.0:
        out.append(f"ℹ worst-case drift {worst_max:.2f} wu — likely visible at close "
                   f"camera; consider strength<1 or near-unit fade.")
    else:
        out.append(f"✓ worst-case drift {worst_max:.2f} wu — sub-1-wu; visible "
                   f"displacement likely safe without near-unit fade.")
    out.append("")
    return "\n".join(out) + "\n"


def audit_one(mission_name: str, missions_dir: Path) -> tuple[Path, dict]:
    path = missions_dir / f"{mission_name}.pak"
    if not path.is_file():
        return path, {"error": f"file not found: {path}"}
    try:
        packets = read_packets(path)
    except Exception as ex:
        return path, {"error": f"read_packets failed: {ex}"}
    candidates = locate_mapdata(packets)
    if not candidates:
        return path, {"error": "no packet matched MapData size signature"}
    idx, t, side, blocks = max(candidates, key=lambda c: len(c[3]))
    elev = _extract_elev_grid(side, blocks)
    metrics = compute_drift(side, elev)
    return path, {
        "metrics": metrics,
        "packet":  {"index": idx, "storage": t},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("missions", nargs="*", default=DEFAULT_TIER1,
                    help="Mission names (without .pak). Default: tier1.")
    add_missions_dir_arg(ap)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    try:
        missions_dir = resolve_from_args(args, missions=args.missions).dir
    except StockDataError as e:
        print(f"[grounding] ERROR {e}", file=sys.stderr)
        return 4

    results: list[tuple[str, Path, dict]] = []
    for name in args.missions:
        path, result = audit_one(name, missions_dir)
        results.append((name, path, result))
        if result.get("error"):
            print(f"[grounding] {name}: ERROR {result['error']}", file=sys.stderr)
        else:
            wm = result["metrics"]["worst_diagonal"]
            print(f"[grounding] {name}: grid={result['metrics']['grid_side']}^2 "
                  f"worst-drift p99={wm['p99']:.3f} max={wm['max']:.3f} wu")

    DEFAULT_OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_path = (Path(args.out) if args.out
                else DEFAULT_OUT_DIR / f"terrain_grounding_audit_"
                f"{dt.datetime.now().strftime('%Y%m%dT%H%M%S')}.md")

    head = []
    head.append("# Terrain Grounding Audit — empirical run\n")
    head.append("- Script: `scripts/terrain_grounding_audit.py` (TERRAIN-GROUNDING-AUDIT-1)")
    head.append(f"- Run at: {dt.datetime.now().isoformat(timespec='seconds')}")
    head.append(f"- Missions dir: `{missions_dir}`")
    head.append(f"- Missions: {', '.join(args.missions)}")
    head.append(f"- Sub-cell probe positions: {SUBSAMPLE_POSITIONS}\n")
    head.append("Measures the height delta between the bilinear-resampled "
                "render heightfield (what TERRAIN-NORMALS-FROM-HEIGHT-1 / "
                "TERRAIN-RESAMPLE-1 sample) and the triangle-linear "
                "interpolation the existing terrain mesh actually renders. "
                "Drift = how much the ground would visually move under a "
                "future TERRAIN-DISPLACE-VISUAL-1 with strength=1. "
                "Gameplay height (Terrain::getTerrainElevation) is unaffected — "
                "units/buildings would continue to read CPU values; this is "
                "the float/sink budget if the visual surface starts moving.\n")
    head.append("Read-only audit; reads only mission `.pak` files, makes no "
                "engine, gameplay, or render mutation.\n")
    head.append("Diagonals are the two possible mesh tessellations of each "
                "quad (top-left↔bottom-right vs top-right↔bottom-left). The "
                "renderer's actual choice depends on tile vertex order; the "
                "'worst' column reports max(both) per probe — a conservative "
                "upper bound.\n")

    body = []
    for name, path, result in results:
        body.append(render_mission_section(name, path, result))

    out_path.write_text("\n".join(head) + "\n" + "".join(body), encoding="utf-8")
    print(f"[grounding] wrote {out_path}")
    return 0 if all(not r.get("error") for _, _, r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
