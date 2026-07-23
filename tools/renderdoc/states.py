#!/usr/bin/env python3
"""Dump the full pipeline state for a single draw in a capture.

Answers, in one command, the questions the manual band-hunt kept re-deriving
by hand in the RenderDoc UI: what shaders are bound, what the color-blend
state is per render target (the "is the opaque cone additive?" question),
depth test/write, cull, topology, viewport/scissor, and the bound textures.

Example::

    py -3 tools/renderdoc/states.py \\
        --rdc tests/smoke/captures/mc2_10_frame120.rdc --draw 1115

--draw is a RenderDoc eventId (the eid column in attribute.py's report). If
the eid is not itself a drawcall, the nearest drawcall at/after it is used.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _rdcommon as rc  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rdc", required=True, help="capture file (.rdc)")
    ap.add_argument("--draw", required=True, type=int,
                    help="RenderDoc eventId of the draw")
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
    md = (Path(args.out).resolve() if args.out
          else Path("%s_draw%d.md" % (stem, args.draw)))
    js = (Path(args.json_out).resolve() if args.json_out
          else Path("%s_draw%d.json" % (stem, args.draw)))
    md.parent.mkdir(parents=True, exist_ok=True)

    job = {
        "mode": "states",
        "rdc": str(rdc),
        "draw": args.draw,
        "result_json": str(js),
        "result_md": str(md),
    }
    res = rc.run_worker(job, timeout=args.timeout, qrenderdoc=args.qrenderdoc)

    print("[states] eid %d -- %s" % (res["eid"], res["name"]))
    if res.get("marker"):
        print("[states] pass/marker: %s" % res["marker"])
    print("[states] %d indices x %d instances, topology %s"
          % (res["num_indices"], res["num_instances"], res["topology"]))
    for s in res.get("shaders", []):
        print("[states]   %-8s %s (entry %s)"
              % (s["stage"], s["name"], s["entry"]))
    for b in res.get("blends", [])[:8]:
        print("[states]   RT%d blend: %s" % (b["target"], b["desc"]))
    if res.get("depth_test") is not None:
        print("[states]   depthTest=%s depthWrite=%s cull=%s"
              % (res.get("depth_test"), res.get("depth_write"),
                 res.get("cull")))
    print("[states] report: %s" % md)
    print("[states] json:   %s" % js)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
