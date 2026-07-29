#!/usr/bin/env python3
"""VISUAL-VERIFY-PIPELINE-1 -- bless a capture run's region crops as a golden set.

A golden is the reference a future "verify this build" run diffs against. We store
GOLDENS AS PER-REGION CROPS (not full 4K frames): the committed regions are small
strips, so a whole bookmark set is a few hundred KB of PNG -- cheap to keep in the
repo under tests/visual/goldens/<name>/. (Full frames are ~5-10 MB each; if you
ever need frame-level goldens, keep them in an external dir and record the path in
golden.json's `frames_dir` -- see docs/visual-verify.md.)

This tool:
  1. Re-emits every region crop from the run's captured PNGs (via verify's crop
     path) into tests/visual/goldens/<name>/<bookmark>/<region>.png.
  2. Writes golden.json: the identity block (install, exe, frame, per-crop sha256,
     source capture manifest) so the golden is self-describing and auditable.

Bless is a deliberate, reviewable act -- the crops + golden.json are what a human
commits. Nothing here pushes.

Usage:
  py -3 tools/visual_verify/bless.py --run <capture-dir> \
      --to tests/visual/goldens/v0.5.1-baseline
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

_THIS = Path(__file__).resolve()
ROOT = _THIS.parents[2]
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
sys.path.insert(0, str(_THIS.parent))
import regions as regionlib  # noqa: E402

DEFAULT_GOLDENS = ROOT / "tests" / "visual" / "goldens"


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def bless(run_dir: Path, to_dir: Path) -> dict:
    run_dir = Path(run_dir).resolve()
    cap_path = run_dir / "capture_run.json"
    if not cap_path.exists():
        raise SystemExit(f"[vv-bless] no capture_run.json in {run_dir}")
    cap = json.loads(cap_path.read_text(encoding="utf-8"))
    to_dir = Path(to_dir).resolve()
    to_dir.mkdir(parents=True, exist_ok=True)

    manifests = regionlib.load_all_manifests()
    members = []
    total_bytes = 0
    _frame_res: set = set()
    for pose in cap.get("poses", []):
        bm = pose["bookmark"]
        doc = manifests.get(bm)
        png_path = pose.get("primary_png")
        if doc is None or not png_path or not Path(png_path).exists():
            continue
        png_bytes = Path(png_path).read_bytes()
        # CAPTURE-RESOLUTION-ASSERT-1: record the resolution this golden was
        # actually blessed at, measured from the frame's own IHDR. Without it
        # scripts/check-capture-resolution.py has no reference to compare a
        # future candidate against, and a wrong-resolution capture grades
        # cleanly (region rects are normalized; verify.py upscales silently).
        try:
            _w, _h = struct.unpack(">II", png_bytes[16:24])
            _frame_res.add(f"{int(_w)}x{int(_h)}")
        except Exception:
            pass
        bm_dir = to_dir / bm
        bm_dir.mkdir(parents=True, exist_ok=True)
        for reg in doc["regions"]:
            rid = reg["id"]
            try:
                cw, ch, crop_png = regionlib.crop_region_png(png_bytes, reg["rect"])
            except Exception as e:
                print(f"[vv-bless] WARN {bm}/{rid} crop failed: {e}", file=sys.stderr)
                continue
            out = bm_dir / f"{rid}.png"
            out.write_bytes(crop_png)
            total_bytes += len(crop_png)
            members.append({
                "bookmark": bm, "region": rid,
                "path": str(out.relative_to(to_dir)).replace("\\", "/"),
                "size": f"{cw}x{ch}",
                "sha256": hashlib.sha256(crop_png).hexdigest(),
                "severity": reg.get("severity"),
                "thresholds": reg.get("thresholds"),
                "check": reg.get("check"),
            })

    golden = {
        "kind": "visual-verify-golden",
        "schema_v": 1,
        "golden_id": to_dir.name,
        "blessed_at": _now_iso(),
        "blessed_commit": None,   # set by the human's commit that lands this
        "source_capture": str(cap_path),
        "install": cap.get("install"),
        "exe": cap.get("exe"),
        "exe_mtime": cap.get("exe_mtime"),
        "frame": cap.get("frame"),
        "deterministic_clock": cap.get("deterministic_clock"),
        "n_regions": len(members),
        "total_crop_bytes": total_bytes,
        "members": members,
    }
    # A golden that cannot state its own reference resolution cannot be
    # checked. Refuse to mint one rather than mint an untrustworthy one.
    if len(_frame_res) != 1:
        raise SystemExit(
            f"[vv-bless] refusing to bless: source frames have "
            f"{'inconsistent' if _frame_res else 'unreadable'} resolutions "
            f"{sorted(_frame_res) or '[]'}. A golden must carry exactly one "
            f"capture_env.resolution (CAPTURE-RESOLUTION-ASSERT-1).")
    golden["capture_env"] = {"resolution": next(iter(_frame_res))}
    (to_dir / "golden.json").write_text(json.dumps(golden, indent=2) + "\n",
                                        encoding="utf-8")
    return golden


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--run", required=True, help="capture dir to bless")
    ap.add_argument("--to", required=True,
                    help="golden dir (e.g. tests/visual/goldens/v0.5.1-baseline)")
    args = ap.parse_args(argv)

    golden = bless(Path(args.run), Path(args.to))
    kb = golden["total_crop_bytes"] / 1024.0
    print(f"[vv-bless] blessed {golden['n_regions']} region crops "
          f"({kb:.1f} KB) -> {args.to}")
    print(f"[vv-bless] golden.json: {Path(args.to).resolve() / 'golden.json'}")
    print("[vv-bless] BLESS is complete on disk; commit the golden dir to make it "
          "the reference. Set blessed_commit in the commit message.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
