#!/usr/bin/env python3
"""Attribute a screen region to the draws that touched it.

Opens a .rdc capture headlessly (via qrenderdoc --python + the renderdoc
replay module) and, for every sampled pixel in a region, walks RenderDoc's
per-pixel history to report which draws touched those pixels -- with each
draw's program/shader names, vertex/instance counts, blend state, depth/cull
state, and the pass/marker context. Draws that actually *wrote* the sampled
pixels (the visible contributors) are listed first.

This is the automation the manual "black-band band-hunt" needed: one capture
plus one command instead of a dozen tint-instrumentation build cycles.

Example -- attribute the top strip of an mc2_10 capture::

    py -3 tools/renderdoc/attribute.py \\
        --rdc tests/smoke/captures/mc2_10_frame120.rdc \\
        --region 0,0,1920,60 \\
        --out tests/smoke/captures/mc2_10_topstrip.md

Region is x,y,w,h in target-texture (backbuffer) pixels; omit for whole frame.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _rdcommon as rc  # noqa: E402


def parse_region(s: str | None):
    if not s:
        return None
    parts = [p for p in s.replace(" ", "").split(",") if p != ""]
    if len(parts) != 4:
        raise SystemExit("--region must be x,y,w,h (got %r)" % s)
    return [int(p) for p in parts]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rdc", required=True, help="capture file (.rdc)")
    ap.add_argument("--region",
                    help="x,y,w,h in backbuffer pixels (default: whole frame)")
    ap.add_argument("--grid", type=int, default=11,
                    help="NxN pixel sample grid across the region (default 11)")
    ap.add_argument("--target", type=int, default=None,
                    help="output-target index to sample (default: backbuffer)")
    ap.add_argument("--out", help="markdown report path "
                    "(default: alongside the .rdc)")
    ap.add_argument("--json", dest="json_out",
                    help="json report path (default: alongside the .rdc)")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--qrenderdoc", help="explicit qrenderdoc.exe path")
    args = ap.parse_args(argv)

    rdc = Path(args.rdc).resolve()
    if not rdc.is_file():
        raise SystemExit("capture not found: %s" % rdc)
    stem = rdc.with_suffix("")
    md = Path(args.out).resolve() if args.out else Path(str(stem) + "_attrib.md")
    js = (Path(args.json_out).resolve() if args.json_out
          else Path(str(stem) + "_attrib.json"))
    md.parent.mkdir(parents=True, exist_ok=True)

    job = {
        "mode": "attribute",
        "rdc": str(rdc),
        "region": parse_region(args.region),
        "grid": args.grid,
        "target": args.target,
        "result_json": str(js),
        "result_md": str(md),
    }
    res = rc.run_worker(job, timeout=args.timeout, qrenderdoc=args.qrenderdoc)

    draws = res.get("draws", [])
    print("[attribute] %s" % rdc.name)
    print("[attribute] target %s %dx%d %s"
          % (res["target"]["name"], res["target"]["width"],
             res["target"]["height"], res["target"]["format"]))
    r = res["region"]
    print("[attribute] region x=%d y=%d w=%d h=%d  (%d samples)"
          % (r["x"], r["y"], r["w"], r["h"], res["samples_taken"]))
    print("[attribute] %d contributing draw(s):" % len(draws))
    for i, d in enumerate(draws[:12]):
        vs = (d.get("vs") or {}).get("name", "?")
        ps = (d.get("ps") or {}).get("name", "?")
        print("  %2d. eid %-5d wrote %d/%d  %s"
              % (i + 1, d["eid"], d["write_hits"], d["sample_hits"],
                 d["name"]))
        print("       VS=%s PS=%s  blend=%s"
              % (vs, ps, d.get("blend")))
    print("[attribute] report: %s" % md)
    print("[attribute] json:   %s" % js)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
