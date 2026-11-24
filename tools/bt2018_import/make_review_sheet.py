#!/usr/bin/env py -3.12
"""Owner review sheet: one row per stock prop = stock render | auto pick | alternatives, one scale per row.

  py -3.12 tools/bt2018_import/make_review_sheet.py --library lib/catalog.json --mapping mapping.json --out sheets/

Offline software render (raster.py), 3/4 view at a gameplay-like pitch, flat shaded (stock and BT2018 alike) so
silhouette and size are comparable; textured engine captures are a separate step for the chosen picks.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_prop_library as lib  # noqa: E402
import raster  # noqa: E402
import stock_props  # noqa: E402

TILE, LABEL = 150, 30
ROWS_PER_SHEET = 10


def bt_prims(install, entry, cache):
    import UnityPy
    f = entry["file"]
    if f not in cache:
        env = UnityPy.load(str(Path(install) / f))
        cache[f] = {o.read().m_Name: o for o in env.objects if o.type.name == "Mesh"}
    pos, uv, subs = lib.mesh_arrays(cache[f][entry["lod_meshes"][-1]].read())  # lowest LOD: fast and what the camera sees
    pos = pos * lib.WU_PER_M  # metres -> world units so a row shares one scale
    return [{"pos": pos, "idx": np.concatenate(subs).astype(np.int64), "uv": None, "tex": None, "color": (150, 170, 190)}]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=lib.DEFAULT_INSTALL)
    ap.add_argument("--library", type=Path, required=True)
    ap.add_argument("--mapping", type=Path, required=True)
    ap.add_argument("--srcdata", type=Path, default=Path("A:/Games/mc2-srcdata/tgl"))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--max-rows", type=int, default=60)
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    assets = {a["id"]: a for a in json.loads(args.library.read_text(encoding="utf-8"))["assets"]}
    mapping = json.loads(args.mapping.read_text(encoding="utf-8"))["map"]
    keys = sorted(mapping, key=lambda k: -mapping[k]["uses"])[: args.max_rows]
    cache, rows = {}, []
    import tempfile
    work = tempfile.mkdtemp()
    for k in keys:
        m = mapping[k]
        try:
            sp = stock_props.stock_prims(k, args.srcdata, work)
        except Exception:
            continue
        allp = np.concatenate([p["pos"] for p in sp])
        ext = allp.max(0) - allp.min(0)
        scale = 0.8 * TILE / max(float(max(ext[0], ext[2])) * 0.9, float(ext[1]), 1.0)
        tiles = [(sp, f"STOCK {k} ({m['uses']}x)")]
        for pid, tag in [(m["pick"], "PICK")] + [(a, "alt") for a in m["alternatives"][:3]]:
            if not pid:
                continue
            e = assets[pid]
            tiles.append((bt_prims(args.install, e, cache), f"{tag} {e['family']} h{e['height_wu']:.0f}"))
        row = []
        for prims, label in tiles:
            img, _ = raster.render(prims, size=TILE, yaw=35, pitch=32, px_per_unit=scale)
            row.append((img, label))
        rows.append(row)
    for s in range(0, len(rows), ROWS_PER_SHEET):
        chunk = rows[s:s + ROWS_PER_SHEET]
        sheet = Image.new("RGB", (5 * TILE, len(chunk) * (TILE + LABEL)), (20, 22, 26))
        d = ImageDraw.Draw(sheet)
        for r, row in enumerate(chunk):
            for c, (img, label) in enumerate(row):
                sheet.paste(Image.fromarray(img), (c * TILE, r * (TILE + LABEL)))
                d.text((c * TILE + 3, r * (TILE + LABEL) + TILE + 3), label[:26], fill=(230, 230, 230))
                d.text((c * TILE + 3, r * (TILE + LABEL) + TILE + 15), label[26:52], fill=(230, 230, 230))
        sheet.save(args.out / f"review_{s // ROWS_PER_SHEET:02d}.png")
    print("rows", len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
