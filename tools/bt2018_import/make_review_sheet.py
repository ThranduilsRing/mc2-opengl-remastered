#!/usr/bin/env py -3.12
"""Owner review sheet: one row per stock prop = stock render | auto pick | alternatives, one scale per row.

  py -3.12 tools/bt2018_import/make_review_sheet.py --library lib/catalog.json --mapping mapping.json --bindings all.json --out sheets/

Offline software render (raster.py) at the gameplay camera angle (3/4 view, about 35 degrees down), albedo
textured with simple lambert lighting for stock and BT2018 alike, so the owner judges look, silhouette and
size together. Rows marked "no good match" in the mapping keep stock and say so.
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


def bt_prims(install, entry, cache, bind, textures):
    import UnityPy
    f = entry["file"]
    if f not in cache:
        env = UnityPy.load(str(Path(install) / f))
        cache[f] = {o.read().m_Name: o for o in env.objects if o.type.name == "Mesh"}
    # LOD1 when present: close to what the gameplay camera sees and fast to rasterise; wu so a row shares one scale
    return lib.lod_prims(cache[f], entry, bind, textures, lod=1, wu=True)


def glb_prims(path):
    """Raster prims straight from an extracted package GLB: what the cook gets (metres -> wu, baked albedo, glTF uv).
    The GLB already carries the Unity->glTF frame, winding and v flip that lib.mesh_arrays applies."""
    import io
    import struct
    data = Path(path).read_bytes()
    jlen = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + jlen])
    blob = data[20 + jlen + 8:]

    def view(i):
        v = doc["bufferViews"][i]
        return blob[v.get("byteOffset", 0): v.get("byteOffset", 0) + v["byteLength"]]

    def acc(i, n):
        a = doc["accessors"][i]
        return np.frombuffer(view(a["bufferView"]), "<u4" if a["componentType"] == 5125 else "<f4").reshape(-1, n)
    tex = np.asarray(Image.open(io.BytesIO(view(doc["images"][0]["bufferView"]))).convert("RGB").resize((512, 512), Image.BOX))
    return [{"pos": acc(p["attributes"]["POSITION"], 3) * lib.WU_PER_M, "idx": acc(p["indices"], 3).astype(np.int64),
             "uv": acc(p["attributes"]["TEXCOORD_0"], 2), "tex": tex, "color": (170, 168, 160)}
            for p in doc["meshes"][0]["primitives"]]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=lib.DEFAULT_INSTALL)
    ap.add_argument("--library", type=Path, required=True)
    ap.add_argument("--mapping", type=Path, required=True)
    ap.add_argument("--srcdata", type=Path, default=Path("A:/Games/mc2-srcdata/tgl"))  # drive-path: ok owner PC stock source; --srcdata overrides
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--bindings", type=Path, help="scan_bindings output: renders un-extracted alternatives (raw _MainTex)")
    ap.add_argument("--packages", type=Path, help="extract_bt2018_props.py --out: render picks from their baked GLBs")
    ap.add_argument("--tile", type=int, default=TILE)
    ap.add_argument("--max-rows", type=int, default=1000)
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    tile = args.tile
    assets = {a["id"]: a for a in json.loads(args.library.read_text(encoding="utf-8"))["assets"]}
    mapping = json.loads(args.mapping.read_text(encoding="utf-8"))["map"]
    keys = sorted(mapping, key=lambda k: -mapping[k]["uses"])[: args.max_rows]
    bind = json.loads(args.bindings.read_text(encoding="utf-8")) if args.bindings else None
    textures = lib.Textures(args.install, bind) if bind else None
    cache, rows = {}, []
    import tempfile
    work = tempfile.mkdtemp()
    for k in keys:
        m = mapping[k]
        try:
            sp = stock_props.stock_prims(k, args.srcdata, work, textured=True)
        except Exception:
            continue
        allp = np.concatenate([p["pos"] for p in sp])
        ext = allp.max(0) - allp.min(0)
        scale = 0.8 * tile / max(float(max(ext[0], ext[2])) * 0.9, float(ext[1]), 1.0)
        verdict = m.get("review", "")
        tiles = [(sp, f"STOCK {k} ({m['uses']}x) {verdict}")]
        for pid, tag in [(m["pick"], "PICK")] + [(a, "alt") for a in m["alternatives"][:3]]:
            if not pid:
                continue
            e = assets[pid]
            glb = args.packages / pid / "lod0.glb" if args.packages else None
            if glb and glb.is_file():
                tiles.append((glb_prims(glb), f"{tag} {e['family']} h{e['height_wu']:.0f} baked"))
            elif bind:
                tiles.append((bt_prims(args.install, e, cache, bind, textures), f"{tag} {e['family']} h{e['height_wu']:.0f}"))
        row = []
        for prims, label in tiles:
            img, _ = raster.render(prims, size=tile, px_per_unit=scale, **lib.VIEW)
            row.append((img, label))
        rows.append(row)
    for s in range(0, len(rows), ROWS_PER_SHEET):
        chunk = rows[s:s + ROWS_PER_SHEET]
        sheet = Image.new("RGB", (5 * tile, len(chunk) * (tile + LABEL)), (20, 22, 26))
        d = ImageDraw.Draw(sheet)
        for r, row in enumerate(chunk):
            for c, (img, label) in enumerate(row):
                sheet.paste(Image.fromarray(img), (c * tile, r * (tile + LABEL)))
                d.text((c * tile + 3, r * (tile + LABEL) + tile + 3), label[:40], fill=(230, 230, 230))
                d.text((c * tile + 3, r * (tile + LABEL) + tile + 15), label[40:80], fill=(230, 230, 230))
        sheet.save(args.out / f"review_{s // ROWS_PER_SHEET:02d}.png")
    print("rows", len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
