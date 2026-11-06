#!/usr/bin/env python3
"""Per-pixel difference histogram + allowed-count buckets + largest changed region, for capture A/B.

WHY: MAD is not a pass signal (mc2_10 with every building missing scored MAD 0.62, close to what a harmless global
shift scores). A pixel-count view separates the two: noise touches many pixels by a few levels; a missing object
touches few pixels by a lot and forms one connected blob. Idea borrowed from Stride's image regression tester; the
code here is our own.

Per-pixel delta = max over R,G,B of |A-B| (0..255). Verdict inputs:
  * buckets: [{"over": D, "max_frac": F}, ...]  FAIL if the fraction of pixels with delta > D exceeds F (fractions, not
    counts, so one file serves every capture resolution; N pixels = F * W * H).
  * region: largest 8-connected region of pixels with delta > region_delta, found on an 8x8 block grid (blocks are
    "changed" when any pixel is, so a broken-up object still reads as one region). Reported as bbox + area fraction;
    FAIL if area_frac > max_region_frac.

Thresholds: tools/visual_verify/pixel_hist_thresholds.json, layered default < gpu < bookmark < bookmark+gpu
(later layers replace the keys they set). "gpu" keys are case-insensitive substrings of the device name in the capture
manifest/sidecar (ab.capture_gpus); the owner's 7900 XTX and RTX 2070 SUPER render differently, so each gets its own
gold variant once real pairs exist. No GPU name logged -> default layers only.

CALIBRATION (2026-11-06, 3840x2160 archived pairs under E:/mc2-archive-2026-10/proofs/claude-vk-cpu-frontier-{1,2}):
  mc2_01 day, same-code reruns / no-op diffs (3 pairs):  over8 0.55-0.64%  over24 0.09-0.11%  over64 0.000%  MAD 0.09-0.11
  mc2_10 night, no-op diff:                              over8 0.050%      over24 0.013%      over64 0.000%  MAD 0.011
  Noise is sparse and never reaches delta 64; its largest region is 0.005-0.009% of the frame (day), 0.0014% (night).
  Defaults sit about 1.6-3x above the worst day noise (over8 1.0%, over24 0.2%, over64 0.005%, region 0.03%); mc2_10
  night is the quiet map, so it gets a 3x-tighter bookmark override. Synthetic probes (tests): a missing 40x40 block or building-sized dark hole trips
  over64 (MAD 0.009 on a 4K day frame, below the 0.09-0.11 of pure noise); a global +6 brightness shift has MAD 6 yet trips only
  the over-8 bucket, i.e. the two metrics disagree in exactly the useful direction.
Re-calibrate: run `py -3 tools/visual_verify/pixel_hist.py A.png B.png [--bookmark NAME] [--gpu NAME]` on fresh no-op pairs.
"""
from __future__ import annotations

import argparse
import json
from collections import deque
from pathlib import Path

import numpy as np

THRESHOLDS_FILE = Path(__file__).resolve().parent / "pixel_hist_thresholds.json"
BLOCK = 8


def delta_map(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """uint8 HxW: max over channels of |a-b|. Inputs HxWx3 (or HxW) uint8/int arrays of equal shape."""
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return (d.max(axis=2) if d.ndim == 3 else d).astype(np.uint8)


def histogram(d: np.ndarray) -> np.ndarray:
    """256-bin pixel count per delta value."""
    return np.bincount(d.ravel(), minlength=256)


def frac_over(hist: np.ndarray, level: int) -> float:
    return float(hist[level + 1:].sum()) / max(1, int(hist.sum()))


def largest_region(d: np.ndarray, region_delta: int = 24, block: int = BLOCK):
    """Largest 8-connected region of delta > region_delta, on a block grid.
    Returns {"bbox": [x0,y0,x1,y1] (pixels, x1/y1 exclusive), "pixels": changed pixels in it, "area_frac": pixels/total} or None."""
    h, w = d.shape
    m = d > region_delta
    if not m.any():
        return None
    bh, bw = -(-h // block), -(-w // block)
    pad = np.zeros((bh * block, bw * block), dtype=bool)
    pad[:h, :w] = m
    cnt = pad.reshape(bh, block, bw, block).sum(axis=(1, 3))
    blocks = cnt > 0
    seen = np.zeros_like(blocks)
    best = None
    for sy, sx in zip(*np.nonzero(blocks)):
        if seen[sy, sx]:
            continue
        q = deque([(sy, sx)])
        seen[sy, sx] = True
        px, y0, x0, y1, x1 = 0, sy, sx, sy, sx
        while q:
            y, x = q.popleft()
            px += int(cnt[y, x])
            y0, x0, y1, x1 = min(y0, y), min(x0, x), max(y1, y), max(x1, x)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < bh and 0 <= nx < bw and blocks[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
        if best is None or px > best[0]:
            best = (px, x0, y0, x1, y1)
    px, x0, y0, x1, y1 = best
    # tighten the bbox to real changed pixels inside the block bbox
    sub = m[y0 * block:(y1 + 1) * block, x0 * block:(x1 + 1) * block]
    ys, xs = np.nonzero(sub)
    return {"bbox": [int(x0 * block + xs.min()), int(y0 * block + ys.min()),
                     int(x0 * block + xs.max()) + 1, int(y0 * block + ys.max()) + 1],
            "pixels": px, "area_frac": px / float(h * w)}


def load_thresholds(path: Path = THRESHOLDS_FILE) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def resolve_thresholds(cfg: dict, bookmark: str | None = None, gpu: str | list | None = None) -> dict:
    """Layer default < gpu < bookmark < bookmark+gpu. gpu is a device name (or list of names); gpu keys match by substring."""
    names = [g.lower() for g in ([gpu] if isinstance(gpu, str) else (gpu or []))]
    hit = lambda key: any(key.lower() in n for n in names)
    out = dict(cfg.get("default", {}))
    for key, layer in (cfg.get("gpu") or {}).items():
        if hit(key):
            out.update(layer)
    bm = (cfg.get("bookmarks") or {}).get(bookmark or "", {})
    out.update({k: v for k, v in bm.items() if k != "gpu"})
    for key, layer in (bm.get("gpu") or {}).items():
        if hit(key):
            out.update(layer)
    return out


def evaluate(a: np.ndarray, b: np.ndarray, th: dict) -> dict:
    """Histogram verdict for one image pair. Returns a JSON-able dict: verdict PASS/FAIL, reasons, per-bucket fractions, region."""
    d = delta_map(a, b)
    hist = histogram(d)
    reasons, buckets = [], []
    for bk in th.get("buckets", []):
        f = frac_over(hist, int(bk["over"]))
        buckets.append({"over": int(bk["over"]), "frac": f, "max_frac": bk["max_frac"]})
        if f > bk["max_frac"]:
            reasons.append(f"{f * 100:.4f}% of pixels over delta {bk['over']} (limit {bk['max_frac'] * 100:.4f}%)")
    reg = largest_region(d, int(th.get("region_delta", 24)))
    if reg and reg["area_frac"] > th.get("max_region_frac", 1.0):
        reasons.append(f"changed region {reg['bbox']} covers {reg['area_frac'] * 100:.4f}% (limit {th['max_region_frac'] * 100:.4f}%)")
    return {"verdict": "FAIL" if reasons else "PASS", "reasons": reasons, "buckets": buckets,
            "region": reg, "max_delta": int(d.max()), "hist_nonzero": int(hist[1:].sum())}


def summary_line(r: dict) -> str:
    reg = r["region"]
    bk = " ".join(f">{b['over']}:{b['frac'] * 100:.3f}%" for b in r["buckets"])
    rg = "none" if not reg else f"bbox={reg['bbox']} {reg['area_frac'] * 100:.4f}%"
    return f"{r['verdict']} {bk} region {rg}" + ("" if not r["reasons"] else " | " + "; ".join(r["reasons"]))


def main(argv=None) -> int:
    from PIL import Image
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--bookmark")
    ap.add_argument("--gpu")
    ap.add_argument("--thresholds", default=str(THRESHOLDS_FILE))
    ns = ap.parse_args(argv)
    A = np.asarray(Image.open(ns.a).convert("RGB"))
    B = np.asarray(Image.open(ns.b).convert("RGB"))
    if A.shape != B.shape:
        print(f"FAIL size mismatch {A.shape} vs {B.shape}")
        return 2
    r = evaluate(A, B, resolve_thresholds(load_thresholds(ns.thresholds), ns.bookmark, ns.gpu))
    print(summary_line(r))
    return 0 if r["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
