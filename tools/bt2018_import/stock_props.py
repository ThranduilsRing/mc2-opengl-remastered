#!/usr/bin/env py -3
"""Stock MC2 prop table for the matcher: every non-vegetation staticprop appearance used by the stock missions.

  py -3 tools/bt2018_import/stock_props.py --corpus <tools/mapgen out>/stock --srcdata A:/Games/mc2-srcdata/tgl --out stock_props.json

size_wu is (x, y_up, z) in world units from the stock ASE (LOD0), silhouette is the matcher's
12x12 two view occupancy, uses is the object count across missions.
"""
from __future__ import annotations

import argparse
import collections
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import prop_classify as pc  # noqa: E402
import raster  # noqa: E402
from tools.content_pipeline.frontends import stock_ase  # noqa: E402

PROP_CLASSES = ("building", "terrain_object", "bridge", "unknown")


def used_appearances(corpus):
    uses = collections.Counter()
    for p in sorted(Path(corpus).glob("*.json")):
        for o in json.loads(p.read_text(encoding="utf-8"))["objects"]:
            if o.get("class") in PROP_CLASSES and o.get("type_name"):
                uses[o["type_name"].lower()] += 1
    return uses


def stock_prims(name, srcdata, work):
    ini = stock_ase.parse_ini(Path(srcdata) / f"{name}.ini")
    lod0 = stock_ase.ase.ini_str(ini, "TGLData", "FileName0", name) or name
    scene, _, _ = stock_ase.scene_from_ase(Path(srcdata) / f"{lod0}.ase", [Path(srcdata) / "128"], work)
    prims = []
    for _, p in scene.flattened():
        if len(p.indices):
            prims.append({"pos": np.asarray(p.positions, np.float32), "idx": np.asarray(p.indices, np.int64),
                          "uv": None, "tex": None, "color": (176, 160, 130)})
    return prims


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", type=Path, required=True)
    ap.add_argument("--srcdata", type=Path, default=Path("A:/Games/mc2-srcdata/tgl"))
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    table = {}
    with tempfile.TemporaryDirectory() as work:
        for name, n in sorted(used_appearances(args.corpus).items()):
            if pc.stock_category(name) == pc.EXCLUDED:
                continue
            try:
                prims = stock_prims(name, args.srcdata, work)
                allp = np.concatenate([p["pos"] for p in prims])
                ext = allp.max(0) - allp.min(0)
                table[name] = {"size_wu": [round(float(v), 1) for v in ext], "uses": n,
                               "silhouette": raster.silhouette(prims).astype(int).tolist()}
            except Exception as exc:
                print("SKIP", name, repr(exc)[:90])
    args.out.write_text(json.dumps(table, indent=1), encoding="utf-8")
    print("stock keys", len(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
