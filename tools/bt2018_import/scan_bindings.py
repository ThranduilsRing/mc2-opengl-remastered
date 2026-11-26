#!/usr/bin/env py -3.12
"""Scan BT2018 scene files (levelN) for MeshFilter/MeshRenderer bindings: mesh name -> material names,
material name -> texture/scalar properties, and
mesh name -> sampled world scales of its placements (Transform parent chain; rotation ignored). Read only. Output JSON feeds build_prop_library.py.

  py -3.12 tools/bt2018_import/scan_bindings.py --levels 10,13,16 --out bind_a.json [--max-renderers 30000]

Several runs (disjoint --levels) merge with --merge a.json b.json --out all.json.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import UnityPy

DEFAULT_INSTALL = "A:/Games/BATTLETECH/BattleTech_Data"  # drive-path: ok owner PC read-only source; --install overrides
MAP_KEYS = ("_MainTex", "_BumpMap", "_MetallicGlossMap", "_OcclusionMap", "_EmissionMap")


def _v3(v):
    return [float(getattr(v, a, getattr(v, a.upper(), 1.0))) for a in ("x", "y", "z")]


def world_scale(tptr, cache):
    """Lossy world scale of a Transform: product of local scales up the parent chain (rotation ignored)."""
    key = (tptr.assets_file.name if hasattr(tptr, "assets_file") else "", tptr.file_id, tptr.path_id)
    if key not in cache:
        t = tptr.read()
        s = _v3(t.m_LocalScale)
        f = t.m_Father
        if f is not None and f.path_id:
            ps = world_scale(f, cache)
            s = [a * b for a, b in zip(s, ps)]
        cache[key] = s
    return cache[key]


def scan_level(path, max_renderers, meshes, materials, scales=None):
    """path: a levelN scene, or an already loaded environment (prefab MeshRenderers in sharedassets)."""
    env = path if hasattr(path, "objects") else UnityPy.load(str(path))
    seen, count, names, tcache = set(), 0, {}, {}
    for o in env.objects:
        if o.type.name != "MeshRenderer":
            continue
        count += 1
        if count > max_renderers:
            break
        r = o.read()
        try:
            go = r.m_GameObject.read()
        except Exception:
            continue
        mesh_ptr = None
        for comp in go.m_Components:
            ptr = comp.component if hasattr(comp, "component") else comp
            if ptr.path_id and ptr.type.name == "MeshFilter":
                mesh_ptr = ptr.read().m_Mesh
        if mesh_ptr is None or not mesh_ptr.path_id:
            continue
        if scales is not None:
            try:
                mk = (mesh_ptr.file_id, mesh_ptr.path_id)
                if mk not in names:
                    names[mk] = mesh_ptr.read().m_Name
                lst = scales.setdefault(names[mk], [])
                if len(lst) < 64:
                    lst.append([round(v, 4) for v in world_scale(go.m_Transform, tcache)])
            except Exception:
                pass
        key = (mesh_ptr.file_id, mesh_ptr.path_id, tuple((m.file_id, m.path_id) for m in r.m_Materials))
        if key in seen:
            continue
        seen.add(key)
        try:
            name = mesh_ptr.read().m_Name
            mats = []
            for m in r.m_Materials:
                mat = m.read()
                mats.append(mat.m_Name)
                if mat.m_Name not in materials:
                    sp = mat.m_SavedProperties
                    maps = {}
                    for k, v in sp.m_TexEnvs:
                        if k in MAP_KEYS and v.m_Texture.path_id:
                            try:
                                maps[k] = v.m_Texture.read().m_Name
                            except Exception:
                                pass
                    fl = dict(sp.m_Floats)
                    materials[mat.m_Name] = {"maps": maps, "shader": getattr(getattr(mat.m_Shader, "read", lambda: None)(), "m_Name", ""),
                                             "cutoff": fl.get("_Cutoff", 0.5), "mode": fl.get("_Mode", 0.0),
                                             "bump_scale": fl.get("_BumpScale", 1.0)}
        except Exception:
            continue
        meshes.setdefault(name, mats)
    return count


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--levels", help="comma separated level numbers")
    ap.add_argument("--max-renderers", type=int, default=30000)
    ap.add_argument("--merge", nargs="*")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    meshes, materials, scales = {}, {}, {}
    if args.merge:
        for p in args.merge:
            d = json.loads(Path(p).read_text(encoding="utf-8"))
            for k, v in d["meshes"].items():
                meshes.setdefault(k, v)
            materials.update(d["materials"])
            for k, v in d.get("scales", {}).items():
                scales.setdefault(k, []).extend(v[: 64 - len(scales.get(k, []))])
    else:
        for n in args.levels.split(","):
            c = scan_level(Path(args.install) / f"level{n}", args.max_renderers, meshes, materials, scales)
            print(f"level{n}: {c} renderers, {len(meshes)} meshes bound", flush=True)
            args.out.write_text(json.dumps({"meshes": meshes, "materials": materials, "scales": scales}), encoding="utf-8")
    args.out.write_text(json.dumps({"meshes": meshes, "materials": materials, "scales": scales}), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
