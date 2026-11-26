#!/usr/bin/env py -3.12
"""Whole-library BT2018 prop catalog: every non-vegetation prop family in the owner's install.

  py -3.12 tools/bt2018_import/build_prop_library.py --out E:/mc2-tmp/slotNN/lib \
      --bindings all.json [--names names.json] [--limit N]

Per family (the *_LODn set of a prop; a *_shell remnant only when no LOD exists): id, category (name +
bounds), size in metres and wu at the placement scale the scenes use (median world scale from the
bindings, 1 when unplaced), footprint, height, triangle counts per LOD, material bindings (scene renderers, then the file's prefab renderers), a column-filled
solid (volume, solidity, top cover) for the fragment filter, a 12x12 two view silhouette and a textured
thumbnail. Writes <out>/catalog.json, <out>/thumbs/<id>.png and one contact sheet per category. Read only on
the install; nothing extracted is committed.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prop_classify as pc  # noqa: E402
import raster  # noqa: E402
import scan_bindings  # noqa: E402

DEFAULT_INSTALL = "A:/Games/BATTLETECH/BattleTech_Data"  # drive-path: ok owner PC read-only source; --install overrides
WU_PER_M = 4.0
FILES = ["resources.assets"] + [f"sharedassets{i}.assets" for i in range(94)]
VIEW = {"yaw": 35.0, "pitch": 35.0}  # gameplay camera: about 35 degrees down, 3/4 view
TEX_PX = 128


def enumerate_meshes(root):
    import UnityPy
    names = {}
    for f in FILES:
        p = Path(root) / f
        if not p.exists():
            continue
        for o in UnityPy.load(str(p)).objects:
            if o.type.name == "Mesh":
                names.setdefault(o.read().m_Name, f)
    return names


def mesh_arrays(mesh):
    from UnityPy.helpers.MeshHelper import MeshHandler
    h = MeshHandler(mesh)
    h.process()
    pos = np.asarray(h.m_Vertices, np.float32).reshape(-1, 3).copy()
    uv = np.asarray(h.m_UV0, np.float32).reshape(-1, 2).copy() if h.m_UV0 is not None and len(h.m_UV0) else None
    pos[:, 0] *= -1
    if uv is not None:
        uv[:, 1] = 1 - uv[:, 1]
    subs = [np.asarray(t, np.uint32).reshape(-1, 3)[:, ::-1] for t in h.get_triangles()]
    return pos, uv, subs


def placement_scale(bind, mesh_name):
    """Median (x, y, z) world scale of the mesh's scene placements; (1, 1, 1) when never placed."""
    s = bind.get("scales", {}).get(mesh_name)
    if not s:
        return np.ones(3, np.float32)
    return np.array([statistics.median(v[i] for v in s) for i in range(3)], np.float32)


class Textures:
    """Albedo thumbnails (TEX_PX square) by material name, through extract_bt2018_props.Install."""

    def __init__(self, install, bind):
        from extract_bt2018_props import Install
        self.inst = Install(install)
        self.bind = bind
        self.cache = {}

    def albedo(self, material):
        if material not in self.cache:
            img = None
            name = self.bind.get("materials", {}).get(material, {}).get("maps", {}).get("_MainTex")
            if name:
                try:
                    img = np.asarray(self.inst.texture(name).convert("RGB").resize((TEX_PX, TEX_PX), Image.BILINEAR))
                except Exception:
                    img = None
            self.cache[material] = img
        return self.cache[material]


def prims_for(pos, uv, subs, mats, textures, color=(170, 168, 160)):
    """One raster prim per submesh, textured when its bound material has an albedo."""
    prims = []
    for si, idx in enumerate(subs):
        if not len(idx):
            continue
        tex = textures.albedo(mats[min(si, len(mats) - 1)]) if (textures and mats and uv is not None) else None
        prims.append({"pos": pos, "idx": idx.astype(np.int64), "uv": uv if tex is not None else None, "tex": tex, "color": color})
    return prims


def lod_prims(env_objs, entry, bind, textures, lod=0, wu=False):
    """Raster prims of one catalog entry's LOD at placement scale (metres, or wu when wu=True)."""
    names = entry["lod_meshes"]
    name = names[max(min(lod, len(names) - 1), -len(names))]
    pos, uv, subs = mesh_arrays(env_objs[name].read())
    pos = pos * np.asarray(entry.get("placement_scale", (1, 1, 1)), np.float32) * (WU_PER_M if wu else 1.0)
    mats = bind.get("meshes", {}).get(name) or entry.get("materials")
    return prims_for(pos, uv, subs, mats, textures)


def main(argv=None):
    import UnityPy
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--bindings", type=Path, required=True)
    ap.add_argument("--names", type=Path, help="cached mesh name -> file json (written when missing)")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args(argv)
    out = args.out
    (out / "thumbs").mkdir(parents=True, exist_ok=True)
    if args.names and args.names.exists():
        names = json.loads(args.names.read_text())
    else:
        names = enumerate_meshes(args.install)
        if args.names:
            args.names.write_text(json.dumps(names))
    bind = json.loads(args.bindings.read_text(encoding="utf-8"))
    textures = Textures(args.install, bind)
    groups = pc.group_assets(list(names))
    todo = [(f, g) for f, g in sorted(groups.items()) if pc.classify(f) != pc.EXCLUDED]
    if args.limit:
        todo = todo[: args.limit]
    by_file = {}
    for fam, g in todo:
        by_file.setdefault(names[g["lods"][0]], []).append((fam, g))
    entries, taken = [], set()
    for fname, fams in sorted(by_file.items()):
        env = UnityPy.load(str(Path(args.install) / fname))
        # prefab renderers in the asset file bind meshes no scanned scene places (scene bindings win)
        scan_bindings.scan_level(env, 10 ** 6, bind["meshes"], bind["materials"])
        objs = {}
        for o in env.objects:
            if o.type.name == "Mesh":
                objs.setdefault(o.read().m_Name, o)
        for fam, g in fams:
            try:
                lods = [n for n in g["lods"] if n in objs]
                if not lods:
                    continue
                pos, uv, subs = mesh_arrays(objs[lods[0]].read())
                idx = np.concatenate(subs) if subs else np.zeros((0, 3), np.uint32)
                if len(idx) < 12 or len(pos) < 8:
                    continue
                scale = placement_scale(bind, lods[0])
                pos = pos * scale
                lo, hi = pos.min(0), pos.max(0)
                size = hi - lo
                cat = pc.classify(fam, tuple(size))
                if cat == pc.EXCLUDED or max(size) < 0.3:
                    continue
                mats = bind["meshes"].get(lods[0])
                geo = [{"pos": pos, "idx": idx.astype(np.int64), "uv": None, "tex": None}]
                solid = raster.solid_metrics(geo)
                thumb, _ = raster.render(prims_for(pos, uv, subs, mats, textures), size=128, **VIEW)
                sid = pc.short_id(fam, taken)
                taken.add(sid)
                Image.fromarray(thumb).save(out / "thumbs" / f"{sid}.png")
                entries.append({
                    "id": sid, "family": fam, "category": cat, "file": fname, "lod_meshes": lods, "shell": g["shell"],
                    "placement_scale": [round(float(v), 4) for v in scale],
                    "size_m": [round(float(v), 3) for v in size],
                    "footprint_wu": [round(float(size[0]) * WU_PER_M, 1), round(float(size[2]) * WU_PER_M, 1)],
                    "height_wu": round(float(size[1]) * WU_PER_M, 1),
                    "volume_wu3": round(solid["volume"] * WU_PER_M ** 3, 1), "solidity": round(solid["solidity"], 3),
                    "top_cover": round(solid["top_cover"], 3),
                    "tris": [int(sum(len(s) for s in mesh_arrays(objs[n].read())[2])) for n in lods],
                    "materials": mats, "materials_resolved": bool(mats and all(m in bind["materials"] for m in mats)),
                    "submeshes": len(subs), "silhouette": raster.silhouette(geo).astype(int).tolist(),
                })
                e = entries[-1]
                print(sid, cat, e["height_wu"], e["solidity"], e["materials_resolved"], flush=True)
            except Exception as exc:  # one bad mesh must not stop a library scan
                print("SKIP", fam, repr(exc)[:100], flush=True)
    (out / "catalog.json").write_text(json.dumps({"schema": "bt2018-prop-library/2", "wu_per_m": WU_PER_M, "assets": entries}, indent=1), encoding="utf-8")
    for cat in pc.CATEGORIES:
        es = [e for e in entries if e["category"] == cat]
        for part in range(0, len(es), 120):
            chunk = es[part:part + 120]
            tiles = [np.asarray(Image.open(out / "thumbs" / f"{e['id']}.png").convert("RGB")) for e in chunk]
            labels = [f"{e['family']} {e['height_wu']:.0f}wu s{e['solidity']:.2f}" for e in chunk]
            raster.contact_sheet(tiles, labels, 12).save(out / f"sheet_{cat}_{part // 120}.png")
    print("assets", len(entries), "resolved", sum(e["materials_resolved"] for e in entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
