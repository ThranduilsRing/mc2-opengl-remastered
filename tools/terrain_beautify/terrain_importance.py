#!/usr/bin/env python3
"""Terrain Surface Truth Arc — Layer 1 importance-field bake (slice 1).

Reads an existing `.beauty` fine height bake (`visual_height_<F>x.r32`) and emits
canonical per-cell SURFACE-TRUTH fields the runtime Density Policy (slice 2+)
will consume to decide WHERE to spend geometry:

  slope_deg      : per-cell slope in degrees.
  curvature      : discrete Laplacian (wu) — surface detail/relief.
  ridge_prom     : continuous ridge prominence at crest cells, else 0.
  cliff_mask     : 1.0 where slope >= threshold, else 0.0.
  reduction_err  : worst-case |fine - reconstructed| over the LOD strides, i.e.
                   where the CURRENT geo-mipmapping reduction is losing shape.
  importance     : combined [0,1] density-importance (v0 weights, tunable) —
                   the single scalar a Density Policy can bias LOD selection by.

Field maths are IMPORTED from skyline_oracle so the bake and the acceptance
oracle agree by construction (no drift between "what we measure" and "what we
bake"). Standalone: does NOT modify the working visual_heightfield.py bake
writer, so existing bakes stay byte-identical. Wiring into the main bake is a
later slice once the fields prove out against the oracle.

Output: `<beauty>/visual_importance.r32` (K channels x V x V, row-major, <f4,
channel order = CHANNELS) + `<beauty>/visual_importance_report.json` (provenance
sha of the fine bake, channel order, per-field stats). Pure numpy (+ optional
Pillow for diagnostics).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skyline_oracle as so  # noqa: E402

# Same strides visual_heightfield.py / the engine LOD bands use. The reduction
# error is the worst-case surface loss across these.
MIP_STRIDES = (2, 4, 5, 10, 20)

# Fixed channel order for the packed r32 blob. Do NOT reorder (breaks readers).
CHANNELS = ("slope_deg", "curvature", "ridge_prom", "cliff_mask",
            "reduction_err", "importance")

# v0 combined-importance weights (tunable; the density-policy slice tunes these
# against the oracle). Normalised inputs, so weights are relative emphasis.
DEFAULT_WEIGHTS = {"slope": 0.4, "curvature": 0.3, "ridge": 0.3}


def ridge_prominence(height: np.ndarray, prominence_wu: float = 1.0) -> np.ndarray:
    """Continuous ridge prominence: (height - local_min) at 3x3 local-max cells
    that clear `prominence_wu`, else 0. Reuses the oracle's ridge_mask so the
    bake and the acceptance harness flag the same crests."""
    h = np.asarray(height, dtype=np.float64)
    p = np.pad(h, 1, mode="edge")
    local_min = np.stack([p[0:-2, 1:-1], p[2:, 1:-1], p[1:-1, 0:-2],
                          p[1:-1, 2:], p[1:-1, 1:-1]], axis=0).min(axis=0)
    prom = h - local_min
    mask = so.ridge_mask(h, prominence_wu)
    return np.where(mask, prom, 0.0)


def reduction_error(height: np.ndarray, strides=MIP_STRIDES) -> np.ndarray:
    """Worst-case |fine - reconstructed| over the LOD strides — where the
    current geo-mipmapping reduction loses the most shape."""
    h = np.asarray(height, dtype=np.float64)
    err = np.zeros_like(h)
    for s in strides:
        err = np.maximum(err, np.abs(h - so.decimate_upsample(h, s)))
    return err


def _norm(a: np.ndarray) -> np.ndarray:
    """Scale to [0,1] by max magnitude (0 -> all-zero, stays 0)."""
    m = float(np.abs(a).max())
    return np.abs(a) / m if m > 1e-12 else np.zeros_like(a)


def compute_fields(height: np.ndarray, wu_per_texel: float = 1.0,
                   slope_thresh_deg: float = 40.0, prominence_wu: float = 1.0,
                   strides=MIP_STRIDES, weights=None) -> dict:
    """All Layer-1 fields for a fine heightfield. Returns {name: VxV float64}."""
    h = np.asarray(height, dtype=np.float64)
    w = dict(DEFAULT_WEIGHTS if weights is None else weights)
    slope = so.slope_deg(h, wu_per_texel)
    curv = so.curvature(h)
    ridge = ridge_prominence(h, prominence_wu)
    cliff = so.cliff_mask(h, wu_per_texel, slope_thresh_deg).astype(np.float64)
    red = reduction_error(h, strides)
    imp = (w["slope"] * _norm(slope) + w["curvature"] * _norm(curv)
           + w["ridge"] * _norm(ridge))
    imp = np.maximum(imp, cliff)          # cliffs are always max-importance
    imp = np.clip(imp / max(imp.max(), 1e-12), 0.0, 1.0)
    return {"slope_deg": slope, "curvature": curv, "ridge_prom": ridge,
            "cliff_mask": cliff, "reduction_err": red, "importance": imp}


def pack_blob(fields: dict, channels=CHANNELS) -> np.ndarray:
    """Concatenate channels in fixed order as a flat little-endian float32 blob."""
    return np.concatenate([np.asarray(fields[c], dtype="<f4").ravel()
                           for c in channels])


def unpack_blob(blob: np.ndarray, side: int, channels=CHANNELS) -> dict:
    """Inverse of pack_blob: split a flat blob back into named VxV arrays."""
    plane = side * side
    out = {}
    for i, c in enumerate(channels):
        out[c] = np.asarray(blob[i * plane:(i + 1) * plane],
                            dtype=np.float64).reshape(side, side)
    return out


def field_stats(fields: dict) -> dict:
    def _s(a):
        a = np.asarray(a, dtype=np.float64)
        return {"min": float(a.min()), "max": float(a.max()),
                "mean": float(a.mean())}
    return {c: _s(fields[c]) for c in fields}


# --------------------------------------------------------------------------
# CLI: read an existing .beauty fine bake, emit the importance sidecar.
# --------------------------------------------------------------------------
def _load_fine(beauty: Path, factor: int):
    """Return (fine VxV float64, V, side, wu_per_texel) from a .beauty dir."""
    report = beauty / "visual_height_report.json"
    wu_per_texel = 1.0
    if report.exists():
        meta = json.loads(report.read_text(encoding="utf-8"))
        factor = int(meta.get("factor", factor))
        wu_per_texel = float(meta.get("world_units_per_vertex_visual",
                                      wu_per_texel))
    fpath = beauty / f"visual_height_{factor}x.r32"
    if not fpath.exists():
        raise FileNotFoundError(f"fine bake not found: {fpath}")
    raw = np.fromfile(fpath, dtype="<f4").astype(np.float64)
    v = int(round(raw.size ** 0.5))
    if v * v != raw.size:
        raise ValueError(f"{fpath} is not square ({raw.size} floats)")
    fine = raw.reshape(v, v)
    side = (v - 1) // factor + 1
    return fine, v, side, wu_per_texel


def bake_importance(beauty_dir, factor: int = 4, wu_per_texel=None,
                    slope_thresh_deg: float = 40.0, diagnostics: bool = False):
    beauty = Path(beauty_dir)
    fine, v, side, wu = _load_fine(beauty, factor)
    if wu_per_texel is not None:
        wu = float(wu_per_texel)
    fields = compute_fields(fine, wu_per_texel=wu,
                            slope_thresh_deg=slope_thresh_deg)
    blob = pack_blob(fields)
    (beauty / "visual_importance.r32").write_bytes(blob.tobytes())
    fine_sha = hashlib.sha256(fine.astype("<f4").tobytes()).hexdigest()
    report = {
        "file": "visual_importance.r32",
        "grid_v": v,
        "coarse_side": side,
        "factor": factor,
        "wu_per_texel": wu,
        "slope_thresh_deg": slope_thresh_deg,
        "channels": list(CHANNELS),
        "built_from_sha256": fine_sha,
        "strides": list(MIP_STRIDES),
        "weights": DEFAULT_WEIGHTS,
        "stats": field_stats(fields),
    }
    (beauty / "visual_importance_report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8")
    written = [str(beauty / "visual_importance.r32"),
               str(beauty / "visual_importance_report.json")]
    if diagnostics:
        try:
            from PIL import Image
            for c in CHANNELS:
                a = fields[c]
                lo, hi = float(a.min()), float(a.max())
                norm = (a - lo) / (hi - lo) if hi > lo else np.zeros_like(a)
                path = beauty / f"visual_importance_{c}.png"
                Image.fromarray((norm * 255).astype(np.uint8), "L").save(path)
                written.append(str(path))
        except Exception:
            pass
    return report, written


def main(argv=None):
    ap = argparse.ArgumentParser(description="Bake terrain Layer-1 importance fields.")
    ap.add_argument("beauty", help="path to a <mission>.beauty directory")
    ap.add_argument("--factor", type=int, default=4)
    ap.add_argument("--wu-per-texel", type=float, default=None)
    ap.add_argument("--slope-thresh", type=float, default=40.0)
    ap.add_argument("--diagnostics", action="store_true")
    a = ap.parse_args(argv)
    report, written = bake_importance(a.beauty, a.factor, a.wu_per_texel,
                                      a.slope_thresh, a.diagnostics)
    print(f"[terrain_importance] wrote {len(written)} file(s) to {a.beauty}")
    print(f"  grid_v={report['grid_v']} side={report['coarse_side']} "
          f"channels={report['channels']}")
    for c, s in report["stats"].items():
        print(f"  {c:14s} min/mean/max {s['min']:.3f}/{s['mean']:.3f}/{s['max']:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
