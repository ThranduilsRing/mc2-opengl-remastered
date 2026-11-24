#!/usr/bin/env py -3.12
"""Whole-library BT2018 prop catalog: every non-vegetation prop family in the owner's install.

  py -3.12 tools/bt2018_import/build_prop_library.py --out E:/mc2-tmp/slotNN/lib \
      [--bindings bind_all.json] [--limit N] [--no-textures]

Per family (a *_shell building, or the *_LODn set of a prop): id, category (name + bounds), size in
metres and wu, footprint, height, triangle counts per LOD, material bindings parsed from the scene
MeshRenderers (scan_bindings.py), a 12x12 two view silhouette and a thumbnail. Writes
<out>/catalog.json, <out>/thumbs/<id>.png and one contact sheet per category. Read only on the install;
nothing extracted is committed.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prop_classify as pc  # noqa: E402
import raster  # noqa: E402

DEFAULT_INSTALL = "A:/Games/BATTLETECH/BattleTech_Data"
WU_PER_M = 4.0
FILES = ["resources.assets"] + [f"sharedassets{i}.assets" for i in range(94)]


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


def decode(mesh):
    from UnityPy.helpers.MeshHandler import MeshHandler  # noqa: F401  (import path differs by version)


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


def main(argv=None):
    import UnityPy
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--bindings", type=Path)
    ap.add_argument("--names", type=Path, help="cached mesh name -> file json")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--no-textures", action="store_true")
    args = ap.parse_args(argv)
    out = args.out
    (out / "thumbs").mkdir(parents=True, exist_ok=True)
    names = json.loads(args.names.read_text()) if args.names and args.names.exists() else enumerate_meshes(args.install)
    bind = json.loads(args.bindings.read_text()) if args.bindings else {"meshes": {}, "materials": {}}
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
        objs = {}
        for o in env.objects:
            if o.type.name == "Mesh":
                objs.setdefault(o.read().m_Name, o)
        for fam, g in fams:
            try:
                lods = []
                for n in g["lods"]:
                    if n not in objs:
                        continue
                    pos, uv, subs = mesh_arrays(objs[n].read())
                    lods.append((n, pos, uv, subs))
                if not lods:
                    continue
                n0, pos, uv, subs = lods[0]
                idx = np.concatenate(subs) if subs else np.zeros((0, 3), np.uint32)
                if len(idx) < 12 or len(pos) < 8:
                    continue
                lo, hi = pos.min(0), pos.max(0)
                size = (hi - lo)
                cat = pc.classify(fam, tuple(size))
                if cat == pc.EXCLUDED or max(size) < 0.3:
                    continue
                prims = [{"pos": pos, "idx": idx.astype(np.int64), "uv": uv, "tex": None, "color": (170, 168, 160)}]
                thumb, _ = raster.render(prims, size=128, yaw=35, pitch=22)
                sid = pc.short_id(fam, taken)
                taken.add(sid)
                Image.fromarray(thumb).save(out / "thumbs" / f"{sid}.png")
                mats = bind["meshes"].get(n0)
                entries.append({
                    "id": sid, "family": fam, "category": cat, "file": fname, "lod_meshes": [l[0] for l in lods], "shell": g["shell"],
                    "size_m": [round(float(v), 3) for v in size],
                    "footprint_wu": [round(float(size[0]) * WU_PER_M, 1), round(float(size[2]) * WU_PER_M, 1)],
                    "height_wu": round(float(size[1]) * WU_PER_M, 1),
                    "tris": [int(sum(len(s) for s in l[3])) for l in lods],
                    "materials": mats, "materials_resolved": bool(mats and all(m in bind["materials"] for m in mats)),
                    "submeshes": len(subs), "silhouette": raster.silhouette(prims).astype(int).tolist(),
                })
                print(sid, cat, entries[-1]["height_wu"], entries[-1]["tris"][0], flush=True)
            except Exception as exc:  # one bad mesh must not stop a library scan
                print("SKIP", fam, repr(exc)[:100], flush=True)
    (out / "catalog.json").write_text(json.dumps({"schema": "bt2018-prop-library/1", "wu_per_m": WU_PER_M, "assets": entries}, indent=1), encoding="utf-8")
    for cat in pc.CATEGORIES:
        es = [e for e in entries if e["category"] == cat]
        for part in range(0, len(es), 120):
            chunk = es[part:part + 120]
            tiles = [np.asarray(Image.open(out / "thumbs" / f"{e['id']}.png").convert("RGB")) for e in chunk]
            raster.contact_sheet(tiles, [f"{e['family']} {e['height_wu']:.0f}wu" for e in chunk], 12).save(out / f"sheet_{cat}_{part // 120}.png")
    print("assets", len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
