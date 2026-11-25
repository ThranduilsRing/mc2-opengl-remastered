#!/usr/bin/env python3
"""Add the MW5 Timber Wolf cockpit nose and canopy glass to a built pack GLB.

The hull SKM carries neither: the third-person nose (TBR_TP_Cockpit_DM0_STM, body-atlas UVs) and the canopy glass
(Timberwolf_CockpitGlass_STM) are separate static meshes in the cockpit bone's local frame. The mech import merges every
part into ONE texture, so glass cannot have its own material: its UVs point at a small unused atlas tile, which
`stamp()` fills with dark glossy glass in the albedo / ORM / normal maps before cooking.

  py -3 tools/mw5_mechs/add_cockpit.py --glb in.glb --out out.glb --hull TBR_TP_Cockpit_DM0_STM.gltf --glass Timberwolf_CockpitGlass_STM.gltf
  py -3 tools/mw5_mechs/add_cockpit.py --stamp-dir <dir with alb.png nrm.png orm.png>
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_glb import load_recipe, read_accessor, trs_mat  # noqa: E402
from sockets import load_glb  # noqa: E402

ATLAS = 2048


def static_primitives(path):
    """(positions, normals, uvs, triangles) per primitive of an umodel static mesh, biggest first."""
    path = Path(path)
    g = json.loads(path.read_text())
    buf = (path.parent / g["buffers"][0]["uri"]).read_bytes()
    out = []
    for pr in g["meshes"][0]["primitives"]:
        a = pr["attributes"]
        out.append(tuple(read_accessor(g, buf, a[k]).astype(np.float32) for k in ("POSITION", "NORMAL", "TEXCOORD_0"))
                   + (read_accessor(g, buf, pr["indices"]).astype(np.uint32).reshape(-1, 3),))
    return sorted(out, key=lambda p: -len(p[3]))


def glass_uv(tile):
    x, y, size = tile
    return np.array([(x + size / 2) / ATLAS, (y + size / 2) / ATLAS], np.float32)


def add_cockpit(glb_in, glb_out, hull, glass, recipe=None):
    recipe = load_recipe() if recipe is None else recipe
    cfg = recipe["cockpit"]
    doc, buf = load_glb(glb_in)
    nodes = doc["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}

    def world(i):
        return (world(parent[i]) if i in parent else np.eye(4)) @ trs_mat(nodes[i])
    joint = next(i for i, n in enumerate(nodes) if n["name"] == cfg["bone"])
    frame = world(joint)
    blob = bytearray(buf)
    views, accs = doc["bufferViews"], doc["accessors"]

    def add(arr, ctype, typ, target=None, minmax=False):
        while len(blob) % 4:
            blob.append(0)
        arr = np.ascontiguousarray(arr)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes, **({"target": target} if target else {})})
        blob.extend(arr.tobytes())
        a = {"bufferView": len(views) - 1, "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a["min"], a["max"] = arr.min(axis=0).tolist(), arr.max(axis=0).tolist()
        accs.append(a)
        return len(accs) - 1

    hull_parts = static_primitives(hull)
    glass_parts = hull_parts[1:] + (static_primitives(glass) if glass else [])  # window insets + canopy pane
    parts = {"cockpit_hull": [hull_parts[0]]}
    if not glass:
        parts["cockpit_hull"] = hull_parts  # authored window panes share the frame's node and skin
    elif glass_parts:
        parts["cockpit_glass"] = glass_parts
    doc["materials"].append({"name": "mw5_glass", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": 0}, "metallicFactor": cfg["glass"]["metallic"], "roughnessFactor": cfg["glass"]["roughness"]}})
    glass_material = len(doc["materials"]) - 1
    tile_uv = glass_uv(cfg["glass"]["tile"])
    for name, plist in parts.items():
        pos = np.concatenate([(np.c_[p, np.ones(len(p))] @ frame.T)[:, :3] for p, _, _, _ in plist]).astype(np.float32)
        nrm = np.concatenate([n @ frame[:3, :3].T for _, n, _, _ in plist]).astype(np.float32)
        uv = np.concatenate([np.tile(tile_uv, (len(u), 1)) if name.endswith("glass") or (not glass and i > 0) else u
                             for i, (_, _, u, _) in enumerate(plist)]).astype(np.float32)
        base, idx = 0, []
        for p, _, _, t in plist:
            idx.append(t + base)
            base += len(p)
        nv = len(pos)
        attrs = {"POSITION": add(pos, 5126, "VEC3", 34962, True), "NORMAL": add(nrm, 5126, "VEC3", 34962),
                 "TEXCOORD_0": add(uv, 5126, "VEC2", 34962),
                 "JOINTS_0": add(np.zeros((nv, 4), np.uint16), 5123, "VEC4", 34962),
                 "WEIGHTS_0": add(np.tile(np.array([1, 0, 0, 0], np.float32), (nv, 1)), 5126, "VEC4", 34962)}
        prim = {"attributes": attrs, "indices": add(np.concatenate(idx).astype(np.uint32).ravel(), 5125, "SCALAR", 34963),
                "material": glass_material if name.endswith("glass") else 0}
        doc["meshes"].append({"name": name, "primitives": [prim]})
        ibm = add(np.linalg.inv(frame).T.astype(np.float32).reshape(1, 16), 5126, "MAT4")
        doc["skins"].append({"joints": [joint], "inverseBindMatrices": ibm})
        nodes.append({"name": name + "_n", "mesh": len(doc["meshes"]) - 1, "skin": len(doc["skins"]) - 1})
        doc["scenes"][doc.get("scene", 0)]["nodes"].append(len(nodes) - 1)
    doc["buffers"][0]["byteLength"] = len(blob)
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob.extend(b"\0" * (-len(blob) % 4))
    Path(glb_out).write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON")
                              + js + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))


def trim_proud_glass(glb_in, glb_out, margin=0.0):
    """Drop connected canopy-glass pieces whose top pokes above the nose hull (they showed as a black teardrop)."""
    doc, buf = load_glb(glb_in)
    by = {m["name"]: m["primitives"][0] for m in doc["meshes"] if m["name"] in ("cockpit_hull", "cockpit_glass")}
    if "cockpit_glass" not in by:
        Path(glb_out).write_bytes(Path(glb_in).read_bytes())
        return 0
    hull_top = read_accessor(doc, buf, by["cockpit_hull"]["attributes"]["POSITION"])[:, 1].max()
    gp = by["cockpit_glass"]
    pos = read_accessor(doc, buf, gp["attributes"]["POSITION"])
    tris = read_accessor(doc, buf, gp["indices"]).astype(np.uint32).reshape(-1, 3)
    parent = list(range(len(pos)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for t in tris:
        parent[find(t[1])] = find(t[0])
        parent[find(t[2])] = find(t[0])
    root = np.array([find(t[0]) for t in tris])
    keep = np.ones(len(tris), bool)
    for r in np.unique(root):
        sel = root == r
        if pos[np.unique(tris[sel])][:, 1].max() > hull_top + margin:
            keep[sel] = False
    blob = bytearray(buf)
    while len(blob) % 4:
        blob.append(0)
    new = np.ascontiguousarray(tris[keep].ravel().astype(np.uint32))
    doc["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": new.nbytes, "target": 34963})
    blob.extend(new.tobytes())
    acc = doc["accessors"][gp["indices"]]
    acc.update(bufferView=len(doc["bufferViews"]) - 1, byteOffset=0, count=int(new.size), componentType=5125)
    doc["buffers"][0]["byteLength"] = len(blob)
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob.extend(b"\0" * (-len(blob) % 4))
    Path(glb_out).write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON")
                              + js + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))
    return int((~keep).sum())


def srgb_to_byte(c):
    c = np.asarray(c, np.float64)
    return np.uint8(np.clip(np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255 + 0.5, 0, 255))


def stamp(directory, recipe=None):
    """Fill the glass atlas tile in the baked alb / nrm / orm PNGs (in place)."""
    recipe = load_recipe() if recipe is None else recipe
    glass = recipe["cockpit"]["glass"]
    x, y, size = glass["tile"]
    values = {"alb": srgb_to_byte(glass["albedo_linear"]), "nrm": np.uint8([128, 128, 255]),
              "orm": np.uint8([255, round(glass["roughness"] * 255), round(glass["metallic"] * 255)])}
    for tag, rgb in values.items():
        path = Path(directory) / f"{tag}.png"
        img = Image.open(path)
        a = np.array(img.convert("RGBA"))
        a[y:y + size, x:x + size, :3] = rgb
        a[y:y + size, x:x + size, 3] = 255
        Image.fromarray(a).convert(img.mode if img.mode in ("RGB", "RGBA") else "RGBA").save(path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--glb")
    ap.add_argument("--out")
    ap.add_argument("--hull")
    ap.add_argument("--glass")
    ap.add_argument("--stamp-dir")
    ap.add_argument("--trim-glb", help="drop canopy glass pieces poking through the hull: --trim-glb in.glb --out out.glb")
    a = ap.parse_args()
    if a.trim_glb:
        print("dropped triangles:", trim_proud_glass(a.trim_glb, a.out))
    if a.stamp_dir:
        stamp(a.stamp_dir)
    if a.glb:
        add_cockpit(a.glb, a.out, a.hull, a.glass)
