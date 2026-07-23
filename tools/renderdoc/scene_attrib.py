#!/usr/bin/env python3
"""Enumerate capture textures, or attribute a region against an explicit
texture resid (e.g. the pre-composite HDR scene FBO, not the backbuffer).

    py -3 tools/renderdoc/scene_attrib.py --rdc X.rdc --list
    py -3 tools/renderdoc/scene_attrib.py --rdc X.rdc --resid 1234 \
        --region 1360,530,220,110 --out out.md
"""
from __future__ import annotations
import argparse, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import _rdcommon as rc  # noqa: E402


def parse_region(s):
    if not s:
        return None
    parts = [p for p in s.replace(" ", "").split(",") if p != ""]
    if len(parts) != 4:
        raise SystemExit("--region must be x,y,w,h")
    return [int(p) for p in parts]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rdc", required=True)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--resid", type=int)
    ap.add_argument("--pixel", help="px,py: dump pixel color history on --resid")
    ap.add_argument("--region")
    ap.add_argument("--grid", type=int, default=9)
    ap.add_argument("--out")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args(argv)

    rdc = Path(args.rdc).resolve()
    if not rdc.is_file():
        raise SystemExit("capture not found: %s" % rdc)
    stem = str(rdc.with_suffix(""))

    if args.list:
        md = Path(args.out).resolve() if args.out else Path(stem + "_textures.md")
        js = Path(stem + "_textures.json")
        job = {"mode": "textures", "rdc": str(rdc),
               "result_json": str(js), "result_md": str(md)}
        res = rc.run_worker(job, timeout=args.timeout)
        for r in res["textures"]:
            print("resid %-6d %-40s %5dx%-5d %-22s %s"
                  % (r["resid"], r["name"][:40], r["width"], r["height"],
                     r["format"], ",".join(r["cats"])))
        print("[textures] report: %s" % md)
        return 0

    if args.resid is None:
        raise SystemExit("need --list or --resid")
    if args.pixel:
        pxs = [int(v) for v in args.pixel.split(",")]
        md = Path(args.out).resolve() if args.out else Path(stem + "_pixhist.md")
        js = Path(str(md.with_suffix("")) + ".json")
        job = {"mode": "pixel", "rdc": str(rdc), "target_resid": args.resid,
               "px": pxs[0], "py": pxs[1],
               "result_json": str(js), "result_md": str(md)}
        res = rc.run_worker(job, timeout=args.timeout)
        for r in res["mods"]:
            print("  eid %-6d %-34s pre=%s post=%s passed=%s"
                  % (r["eid"], r["name"][:34], r["pre"], r["post"], r["passed"]))
        print("[pixhist] report: %s" % md)
        return 0
    md = Path(args.out).resolve() if args.out else Path(stem + "_scene_attrib.md")
    js = Path(str(md.with_suffix("")) + ".json")
    md.parent.mkdir(parents=True, exist_ok=True)
    job = {"mode": "attribute", "rdc": str(rdc),
           "region": parse_region(args.region), "grid": args.grid,
           "target_resid": args.resid,
           "result_json": str(js), "result_md": str(md)}
    res = rc.run_worker(job, timeout=args.timeout)
    t = res["target"]
    print("[scene_attrib] target %s (resid %d) %dx%d %s"
          % (t["name"], t["resid"], t["width"], t["height"], t["format"]))
    r = res["region"]
    print("[scene_attrib] region x=%d y=%d w=%d h=%d (%d samples)"
          % (r["x"], r["y"], r["w"], r["h"], res["samples_taken"]))
    for i, d in enumerate(res["draws"][:15]):
        vs = (d.get("vs") or {}).get("name", "?")
        ps = (d.get("ps") or {}).get("name", "?")
        print("  %2d. eid %-5d wrote %d/%d  %s | VS=%s PS=%s | %s"
              % (i + 1, d["eid"], d["write_hits"], d["sample_hits"],
                 d["name"], vs, ps,
                 "depth=%s/%s blend=%s" % (d.get("depth_test"),
                                           d.get("depth_write"), d.get("blend"))))
    print("[scene_attrib] report: %s" % md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
