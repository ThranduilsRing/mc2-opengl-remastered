#!/usr/bin/env py -3
"""Stock MC2 prop table for the matcher: every non-vegetation staticprop appearance used by the stock missions.

  py -3 tools/bt2018_import/stock_props.py --corpus <tools/mapgen out>/stock --srcdata A:/Games/mc2-srcdata/tgl --out stock_props.json
  py -3 tools/bt2018_import/stock_props.py --table old_stock_props.json --out stock_props.json   (same keys and uses, fresh metrics)

size_wu is (x, y_up, z) in world units from the stock ASE (LOD0), silhouette is the matcher's
12x12 two view occupancy, volume_wu3/solidity/top_cover the column-filled solid (raster.solid_metrics),
uses is the object count across missions.
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


def stock_prims(name, srcdata, work, textured=False):
    ini = stock_ase.parse_ini(Path(srcdata) / f"{name}.ini")
    lod0 = stock_ase.ase.ini_str(ini, "TGLData", "FileName0", name) or name
    scene, mats, _ = stock_ase.scene_from_ase(Path(srcdata) / f"{lod0}.ase", [Path(srcdata) / "128"], work)
    prims, texs = [], {}
    for _, p in scene.flattened():
        if len(p.indices):
            tex = None
            ref = mats[p.material].base_color if textured and p.material < len(mats) else None
            if ref is not None and p.uv0 is not None:
                if ref.path not in texs:
                    from PIL import Image
                    texs[ref.path] = np.asarray(Image.open(ref.path).convert("RGB").resize((128, 128), Image.BILINEAR))
                tex = texs[ref.path]
            prims.append({"pos": np.asarray(p.positions, np.float32), "idx": np.asarray(p.indices, np.int64),
                          "uv": np.asarray(p.uv0, np.float32) if tex is not None else None, "tex": tex, "color": (176, 160, 130)})
    return prims


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--corpus", type=Path)
    src.add_argument("--table", type=Path, help="previous stock_props.json: reuse its keys and use counts")
    ap.add_argument("--srcdata", type=Path, default=Path("A:/Games/mc2-srcdata/tgl"))
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    table = {}
    with tempfile.TemporaryDirectory() as work:
        uses = ({k: v["uses"] for k, v in json.loads(args.table.read_text(encoding="utf-8")).items()} if args.table
                else used_appearances(args.corpus))
        for name, n in sorted(uses.items()):
            if pc.stock_category(name) == pc.EXCLUDED:
                continue
            try:
                prims = stock_prims(name, args.srcdata, work)
                allp = np.concatenate([p["pos"] for p in prims])
                ext = allp.max(0) - allp.min(0)
                solid = raster.solid_metrics(prims)
                table[name] = {"size_wu": [round(float(v), 1) for v in ext], "uses": n,
                               "volume_wu3": round(solid["volume"], 1), "solidity": round(solid["solidity"], 3),
                               "top_cover": round(solid["top_cover"], 3),
                               "silhouette": raster.silhouette(prims).astype(int).tolist()}
            except Exception as exc:
                print("SKIP", name, repr(exc)[:90])
    args.out.write_text(json.dumps(table, indent=1), encoding="utf-8")
    print("stock keys", len(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
