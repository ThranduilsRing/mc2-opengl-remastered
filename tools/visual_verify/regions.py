#!/usr/bin/env python3
"""VISUAL-VERIFY-PIPELINE-1 -- region-manifest loader + crop extractor.

A *region manifest* names the artifact-prone rectangles of a single bookmark
pose -- the exact strips a human (or the 11-agent verification fleet) hand-cropped
at full resolution and graded against the complaint checklist: horizon strips,
shore segments, shadow zones, light clusters, pad edges. Instead of re-deriving
those rectangles every session, we commit them once (tools/visual_verify/regions/
<bookmark>.json) with:

  * a rect (normalized 0..1 fractions, or absolute px against a reference res),
  * a plain-English "check" describing what to look for,
  * a severity, and
  * per-region diff thresholds used by verify.py to grade a region PASS/FAIL.

This module is pure stdlib. It reuses the shared PNG codec in tools/visual/png_io
so the whole pipeline stays PIL/numpy-free (the smoke/gate environment has no
imaging deps).

Rect schema (per region):
  "rect": {"mode": "norm", "x": 0.61, "y": 0.30, "w": 0.05, "h": 0.12}
      normalized fractions of the frame; resolution-independent (preferred).
  "rect": {"mode": "px", "x": 2350, "y": 656, "w": 150, "h": 258,
           "ref_w": 3840, "ref_h": 2160}
      absolute pixels measured against a reference resolution; scaled to the
      actual capture resolution at crop time.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

# Reuse the shared stdlib PNG codec (tools/visual/png_io.py).
_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
from png_io import decode_png, encode_png  # noqa: E402

REGIONS_DIR = _THIS.parent / "regions"


class RegionError(Exception):
    pass


def manifest_path(bookmark: str) -> Path:
    return REGIONS_DIR / f"{bookmark}.json"


def load_manifest(bookmark: str) -> dict:
    """Load the region manifest for a bookmark stem (e.g. mc2_10_saved_204523)."""
    p = manifest_path(bookmark)
    if not p.exists():
        raise RegionError(f"no region manifest for {bookmark!r} ({p})")
    doc = json.loads(p.read_text(encoding="utf-8"))
    _validate(doc, p)
    return doc


def load_all_manifests() -> "dict[str, dict]":
    out: dict[str, dict] = {}
    if not REGIONS_DIR.is_dir():
        return out
    for p in sorted(REGIONS_DIR.glob("*.json")):
        if p.name.startswith("_"):
            continue
        doc = json.loads(p.read_text(encoding="utf-8"))
        _validate(doc, p)
        out[doc.get("bookmark", p.stem)] = doc
    return out


def _validate(doc: dict, path: Path) -> None:
    if "regions" not in doc or not isinstance(doc["regions"], list):
        raise RegionError(f"{path}: missing 'regions' list")
    seen = set()
    for r in doc["regions"]:
        rid = r.get("id")
        if not rid:
            raise RegionError(f"{path}: region without 'id'")
        if rid in seen:
            raise RegionError(f"{path}: duplicate region id {rid!r}")
        seen.add(rid)
        if "rect" not in r:
            raise RegionError(f"{path}: region {rid!r} missing 'rect'")


def resolve_rect(rect: dict, width: int, height: int) -> "tuple[int, int, int, int]":
    """Return (x0, y0, x1, y1) clamped pixel bounds for a rect on a WxH frame."""
    mode = rect.get("mode", "norm")
    if mode == "norm":
        x0 = rect["x"] * width
        y0 = rect["y"] * height
        w = rect["w"] * width
        h = rect["h"] * height
    elif mode == "px":
        ref_w = rect.get("ref_w", width)
        ref_h = rect.get("ref_h", height)
        sx = width / float(ref_w)
        sy = height / float(ref_h)
        x0 = rect["x"] * sx
        y0 = rect["y"] * sy
        w = rect["w"] * sx
        h = rect["h"] * sy
    else:
        raise RegionError(f"unknown rect mode {mode!r}")
    ix0 = max(0, min(width, int(round(x0))))
    iy0 = max(0, min(height, int(round(y0))))
    ix1 = max(0, min(width, int(round(x0 + w))))
    iy1 = max(0, min(height, int(round(y0 + h))))
    if ix1 <= ix0:
        ix1 = min(width, ix0 + 1)
    if iy1 <= iy0:
        iy1 = min(height, iy0 + 1)
    return ix0, iy0, ix1, iy1


def crop_rgb(pixels: bytearray, channels: int, width: int, height: int,
             box: "tuple[int, int, int, int]") -> "tuple[int, int, bytearray]":
    """Crop a decoded image to `box`, returning (cw, ch, rgb_bytes) as 3-channel."""
    x0, y0, x1, y1 = box
    cw, ch = x1 - x0, y1 - y0
    out = bytearray(cw * ch * 3)
    stride = width * channels
    for row in range(ch):
        src = (y0 + row) * stride + x0 * channels
        dst = row * cw * 3
        for col in range(cw):
            s = src + col * channels
            out[dst] = pixels[s]
            out[dst + 1] = pixels[s + 1]
            out[dst + 2] = pixels[s + 2]
            dst += 3
    return cw, ch, out


def crop_region_png(png_bytes: bytes, rect: dict) -> "tuple[int, int, bytes]":
    """Decode a full frame, crop to `rect`, return (cw, ch, png_bytes) for the crop."""
    w, h, ch, px = decode_png(png_bytes)
    box = resolve_rect(rect, w, h)
    cw, chh, rgb = crop_rgb(px, ch, w, h, box)
    return cw, chh, encode_png(cw, chh, 3, rgb)


def coverage_summary() -> "list[dict]":
    """One row per manifest: bookmark, mission, region count, severities."""
    rows = []
    for bm, doc in load_all_manifests().items():
        regs = doc["regions"]
        sev = {}
        for r in regs:
            s = r.get("severity", "unspecified")
            sev[s] = sev.get(s, 0) + 1
        rows.append({
            "bookmark": bm,
            "mission": doc.get("mission", ""),
            "framing": doc.get("framing", ""),
            "regions": len(regs),
            "severities": sev,
        })
    return rows


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="region-manifest inspector / crop tool")
    sub = ap.add_subparsers(dest="cmd")

    ap_cov = sub.add_parser("coverage", help="print manifest coverage table")

    ap_crop = sub.add_parser("crop", help="crop one region from a PNG (debug)")
    ap_crop.add_argument("--png", required=True)
    ap_crop.add_argument("--bookmark", required=True)
    ap_crop.add_argument("--region", required=True)
    ap_crop.add_argument("--out", required=True)

    args = ap.parse_args(argv)

    if args.cmd == "coverage" or args.cmd is None:
        rows = coverage_summary()
        total = sum(r["regions"] for r in rows)
        print(f"{'bookmark':32} {'mission':8} {'#reg':>4}  severities")
        print("-" * 78)
        for r in rows:
            sev = ",".join(f"{k}:{v}" for k, v in sorted(r["severities"].items()))
            print(f"{r['bookmark']:32} {r['mission']:8} {r['regions']:>4}  {sev}")
        print("-" * 78)
        print(f"{len(rows)} manifests, {total} regions total")
        return 0

    if args.cmd == "crop":
        doc = load_manifest(args.bookmark)
        reg = next((r for r in doc["regions"] if r["id"] == args.region), None)
        if reg is None:
            print(f"no region {args.region!r} in {args.bookmark}", file=sys.stderr)
            return 2
        data = Path(args.png).read_bytes()
        cw, ch, cp = crop_region_png(data, reg["rect"])
        Path(args.out).write_bytes(cp)
        print(f"wrote {args.out} ({cw}x{ch})")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
