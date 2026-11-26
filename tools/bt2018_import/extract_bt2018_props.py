#!/usr/bin/env py -3.12
"""BT2018 environment props -> compiled prop packages + a content-pipeline recipe.

Reads the owner's OWN BATTLETECH install (read only) with UnityPy 1.25 and writes, per prop in
props_catalog.json, <out>/<id>/{lod<N>.glb, bt2018_prop_package.json}. The GLBs are standard
glTF 2.0 (metres, right handed, PBR maps embedded) so the common cooker consumes them:

    py -3.12 tools/bt2018_import/extract_bt2018_props.py --out E:/mc2-tmp/slotNN/bt2018-props
    py -3 tools/content_pipeline/cook.py <out>/recipe.json            (-> mods/bt2018-props)
    py -3 tools/bt2018_import/check_prop_packages.py <out>

or, driven by the reviewed auto-matcher output instead of props_catalog.json:

    py -3.12 tools/bt2018_import/extract_bt2018_props.py --out <out>         --mapping mapping.json --library lib/catalog.json --bindings all.json
    py -3 tools/content_pipeline/cook.py <out>/recipe.json --no-compress   (proof cook)
    py -3 tools/bt2018_import/check_prop_packages.py <out> --mapping mapping.json --cooked <out>/mod/bt2018-props

Extracted bytes never enter git; the catalog (mesh names, texture stems, classes, the stock mapping
table) is the committed recipe. Frame: Unity (left handed) -> glTF by negating X and flipping winding;
UV v flips; the cooker applies 4 wu per metre (wu_per_unit in the recipe).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bake_materials  # noqa: E402

WU_PER_M = 4.0
BAKE_PX = (2048, 1024, 512)  # baked texture set per LOD
DEFAULT_INSTALL = "A:/Games/BATTLETECH/BattleTech_Data"  # drive-path: ok owner PC read-only source; --install overrides
LICENSE = "personal use: extracted from the owner's own BATTLETECH install; not redistributable"
SCHEMA = "bt2018-prop-package/1"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Install:
    def __init__(self, root):
        self.root = Path(root)
        self._env = {}
        self._tex = None

    def env(self, name):
        if name not in self._env:
            import UnityPy  # lazy: pure helpers import without it
            self._env[name] = UnityPy.load(str(self.root / name))
        return self._env[name]

    def texture_index(self):
        """Texture2D name -> (file, path_id) over resources.assets and every sharedassets file."""
        if self._tex is None:
            self._tex = {}
            files = ["resources.assets"] + [f"sharedassets{i}.assets" for i in range(94)]
            for f in files:
                if not (self.root / f).exists():
                    continue
                for o in self.env(f).objects:
                    if o.type.name == "Texture2D":
                        self._tex.setdefault(o.read().m_Name, (f, o.path_id))
                self._env.pop(f, None)
        return self._tex

    def texture(self, name):
        f, pid = self.texture_index()[name]
        return next(o for o in self.env(f).objects if o.path_id == pid).read().image.convert("RGBA")


def find_mesh(inst, file, name):
    for o in inst.env(file).objects:
        if o.type.name == "Mesh" and o.read().m_Name == name:
            return o.read()
    raise KeyError(f"{file}: mesh {name}")


def renderer_materials(inst, file, mesh_name):
    """Material names the prefab's MeshRenderer assigns to this mesh, or None when the prefab is elsewhere."""
    env = inst.env(file)
    ids = {o.path_id for o in env.objects if o.type.name == "Mesh" and o.read().m_Name == mesh_name}
    for o in env.objects:
        if o.type.name != "MeshFilter":
            continue
        flt = o.read()
        if flt.m_Mesh.path_id not in ids:
            continue
        for comp in flt.m_GameObject.read().m_Components:
            c = (comp.read() if hasattr(comp, "read") else comp.component.read())
            if type(c).__name__ == "MeshRenderer":
                out = []
                for m in c.m_Materials:
                    mat = m.read()
                    props = {k: v.m_Texture.read().m_Name for k, v in mat.m_SavedProperties.m_TexEnvs if v.m_Texture.path_id}
                    floats = dict(mat.m_SavedProperties.m_Floats)
                    out.append({"material": mat.m_Name, "maps": props, "cutoff": floats.get("_Cutoff", 0.5),
                                "bump_scale": floats.get("_BumpScale", 1.0)})
                return out
    return None


def png(arr):
    b = io.BytesIO()
    Image.fromarray(arr).save(b, "PNG")
    return b.getvalue()


def write_glb(path, name, prims, images, materials):
    """prims: [(pos, nrm, uv, tan|None, idx, material_index)]; images: [png bytes]; materials: glTF dicts."""
    blob = bytearray()
    views, accessors = [], []

    def add(data, kind, comp, count, target=None, minmax=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), **({"target": target} if target else {})})
        blob.extend(data)
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": kind}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1
    gprims = []
    for pos, nrm, uv, tan, idx, mat in prims:
        attrs = {"POSITION": add(pos.astype("<f4").tobytes(), "VEC3", 5126, len(pos), 34962,
                                 (pos.min(0).tolist(), pos.max(0).tolist())),
                 "NORMAL": add(nrm.astype("<f4").tobytes(), "VEC3", 5126, len(nrm), 34962),
                 "TEXCOORD_0": add(uv.astype("<f4").tobytes(), "VEC2", 5126, len(uv), 34962)}
        if tan is not None:
            attrs["TANGENT"] = add(tan.astype("<f4").tobytes(), "VEC4", 5126, len(tan), 34962)
        gprims.append({"attributes": attrs, "indices": add(idx.astype("<u4").tobytes(), "SCALAR", 5125, len(idx), 34963),
                       "material": mat})
    gimages = []
    for data in images:
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        gimages.append({"bufferView": len(views) - 1, "mimeType": "image/png"})
    doc = {"asset": {"version": "2.0", "generator": "bt2018_import/extract_bt2018_props"},
           "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
           "meshes": [{"name": name, "primitives": gprims}], "materials": materials,
           "images": gimages, "textures": [{"source": i, "sampler": 0} for i in range(len(gimages))],
           "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
           "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    while len(blob) % 4:
        blob.append(0)
    out = struct.pack("<III", 0x46546C67, 2, 28 + len(js) + len(blob)) + struct.pack("<II", len(js), 0x4E4F534A) + js \
        + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    Path(path).write_bytes(out)
    return sha(out)


def mesh_prims(mesh):
    from UnityPy.helpers.MeshHelper import MeshHandler
    h = MeshHandler(mesh)
    h.process()
    pos = np.asarray(h.m_Vertices, np.float32).reshape(-1, 3).copy()
    nrm = np.asarray(h.m_Normals, np.float32).reshape(-1, 3).copy()
    uv = np.asarray(h.m_UV0, np.float32).reshape(-1, 2).copy()
    tan = np.asarray(h.m_Tangents, np.float32).reshape(-1, 4).copy() if h.m_Tangents is not None and len(h.m_Tangents) else None
    col = h.m_Colors
    col = np.asarray(col, np.float32).reshape(-1, 4)[:, 0] / 255.0 if col is not None and len(col) else None
    pos[:, 0] *= -1  # Unity left handed -> glTF right handed
    nrm[:, 0] *= -1
    uv[:, 1] = 1 - uv[:, 1]
    if tan is not None:
        tan[:, 0] *= -1
        tan[:, 3] *= -1
    subs = []
    for tris in h.get_triangles():
        t = np.asarray(tris, np.uint32).reshape(-1, 3)[:, ::-1].reshape(-1)  # flip winding
        subs.append(t)
    return pos, nrm, uv, tan, subs, col


def spec_material(spec):
    """props_catalog.json texture spec {alb, nrm, amb?, mtl?} -> a decoded single-layer material."""
    maps = {slot: (spec[k], (1.0, 1.0), (0.0, 0.0)) for k, slot in
            (("alb", "_MainTex"), ("nrm", "_BumpMap"), ("amb", "_OcclusionMap"), ("mtl", "_MetallicGlossMap")) if spec.get(k)}
    return {"maps": maps, "color": (1.0, 1.0, 1.0), "floats": {"_BumpScale": float(spec.get("bump_scale", 1.0))}}


def fit_stock(size, stock):
    """Uniform XZ fit, allowing a quarter turn; clamp height independently. Reject poor aspect matches."""
    size, stock = np.asarray(size, float), np.asarray(stock, float)
    if np.any(size <= 0) or np.any(stock <= 0):
        raise ValueError("degenerate bounds")
    candidates = []
    for turn in (False, True):
        oriented = size[[2, 1, 0]] if turn else size
        xz = min(stock[0] / oriented[0], stock[2] / oriented[2])
        scale = np.array([xz, np.clip(oriented[1] * xz, stock[1] * 0.6, stock[1] * 1.6) / oriented[1], xz])
        ratio = oriented * scale / stock
        candidates.append((min(ratio[0], ratio[2]), turn, scale, ratio))
    coverage, turn, scale, ratio = max(candidates, key=lambda c: c[0])
    if coverage < 0.9:
        raise ValueError(f"footprint mismatch {ratio.round(3).tolist()}")
    return turn, scale, ratio


def albedo_metrics(rgba):
    rgb = np.asarray(rgba)[..., :3].reshape(-1, 3).astype(float) / 255.0
    if np.asarray(rgba).shape[-1] == 4:
        rgb = rgb[np.asarray(rgba)[..., 3].reshape(-1) > 0]
    if not len(rgb):
        raise ValueError("empty albedo")
    mean = rgb.mean(0)
    if abs(mean[0] - 0.5) < 0.12 and abs(mean[1] - 0.5) < 0.12 and mean[2] > 0.85:
        raise ValueError("normal map bound as albedo")
    hi, lo = rgb.max(1), rgb.min(1)
    return {"luminance": float((rgb @ [0.2126, 0.7152, 0.0722]).mean()),
            "saturation": float(((hi - lo) / np.maximum(hi, 1e-6)).mean())}


def check_albedo(rgba, stock):
    metrics = albedo_metrics(rgba)
    lum, sat = metrics["luminance"], metrics["saturation"]
    if not max(0.04, stock["luminance"] * 0.5) <= lum <= min(0.9, stock["luminance"] * 1.7):
        raise ValueError(f"albedo luminance {lum:.3f} versus stock {stock['luminance']:.3f}")
    if not stock["saturation"] * 0.15 <= sat <= min(0.8, stock["saturation"] * 2.5 + 0.1):
        raise ValueError(f"albedo saturation {sat:.3f} versus stock {stock['saturation']:.3f}")
    return metrics


def build_prop(inst, prop, out, matidx, texcache):
    """One prop package: per LOD one GLB with one baked texture set (albedo, normal, ORM) over all submeshes."""
    pid = prop["id"]
    pdir = out / pid
    pdir.mkdir(parents=True, exist_ok=True)
    lods, materials_doc, bounds = [], [], None
    from stock_props import stock_prims
    stock = stock_prims(prop["replaces"][0].split(":", 1)[-1], "A:/Games/mc2-srcdata/tgl", out / "stock", textured=True)  # drive-path: ok owner PC stock source
    stock_pos = np.concatenate([p["pos"] for p in stock])
    stock_size = stock_pos.max(0) - stock_pos.min(0)
    stock_tex = [p["tex"].reshape(-1, 3) for p in stock if p["tex"] is not None]
    if not stock_tex:
        raise ValueError("stock texture unavailable")
    stock_metrics = albedo_metrics(np.concatenate(stock_tex))
    # Independent LOD bakes have different UV layouts/material slots. Keep the full-detail atlas and mesh.
    for lod_i, lod in enumerate(prop["lods"][:1]):
        pos, nrm, uv, _tan, subs, col = mesh_prims(find_mesh(inst, prop["file"], lod["mesh"]))
        pos = pos * np.asarray(prop.get("placement_scale", (1.0, 1.0, 1.0)), np.float32)
        turn, scale, ratio = fit_stock((pos.max(0) - pos.min(0)) * WU_PER_M, stock_size)
        if turn:
            pos, nrm = pos[:, [2, 1, 0]].copy(), nrm[:, [2, 1, 0]].copy()
            pos[:, 2] *= -1
            nrm[:, 2] *= -1
        pos *= scale
        pos -= np.array([(pos[:, 0].min() + pos[:, 0].max()) / 2, pos[:, 1].min(),
                         (pos[:, 2].min() + pos[:, 2].max()) / 2])
        nrm /= scale
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
        auto = renderer_materials(inst, prop["file"], lod["mesh"])
        names = [m["material"] for m in auto] if auto else list(prop.get("material_names") or [])
        specs = prop.get("materials") or []
        mats, missing = [], set()
        for si in range(len(subs)):
            if names:
                n = names[min(si, len(names) - 1)]
                mats.append(matidx.decode(n))
                if mats[-1] is None:
                    missing.add(n)
            else:
                mats.append(spec_material(specs[min(si, len(specs) - 1)]) if specs else None)
        if not names and not specs:
            raise ValueError(f"{pid} {lod['mesh']}: no material (renderer unresolved and no catalog materials)")
        size = BAKE_PX[min(lod_i, len(BAKE_PX) - 1)]
        unity_uv = np.stack([uv[:, 0], 1.0 - uv[:, 1]], -1)  # mesh_prims already flipped v for glTF
        alb, nimg, orm, new_uvs = bake_materials.bake(unity_uv, col, [s.reshape(-1, 3) for s in subs], mats, texcache, size)
        for mat in mats:
            for slot in ("_MainTex", "_BlendMainTex"):
                spec = mat["maps"].get(slot) if mat else None
                if spec:
                    image = texcache.full[spec[0]]
                    if min(image.size) < 128:
                        raise ValueError(f"tiny albedo source {spec[0]}: {image.size}")
                    albedo_metrics(np.asarray(image))
        metrics = check_albedo(alb, stock_metrics)
        if missing:
            raise ValueError(f"unresolved materials: {sorted(missing)}")
        images = [png(alb), png(nimg), png(orm)]
        gl = {"name": f"{pid}_0", "doubleSided": False,
              "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 1.0,
                                       "metallicRoughnessTexture": {"index": 2}},
              "normalTexture": {"index": 1, "scale": 1.0}, "occlusionTexture": {"index": 2, "strength": 1.0},
              "extras": {"mc2_material_class": prop["class"]}}
        if lod_i == 0:
            materials_doc.append({"slot": 0, "class": prop["class"], "alphaMode": "OPAQUE", "baked": True,
                                  "source_materials": names or [s.get("alb") for s in specs], "unresolved": sorted(missing),
                                  "bake_px": size,
                                  "sha256": {"albedo": sha(images[0]), "normal": sha(images[1]), "orm": sha(images[2])}})
            lo, hi = pos.min(0), pos.max(0)
            bounds = {"min_m": lo.round(3).tolist(), "max_m": hi.round(3).tolist(),
                      "size_wu": ((hi - lo) * WU_PER_M).round(2).tolist()}
        prims = [(pos, nrm, new_uvs[si], None, idx, 0) for si, idx in enumerate(subs) if len(idx)]
        glb = pdir / f"lod{lod_i}.glb"
        digest = write_glb(glb, f"{pid}_lod{lod_i}", prims, images, [gl])
        lods.append({"glb": glb.name, "mesh": lod["mesh"], "distance_wu": lod.get("distance_wu", 0),
                     "vertices": int(len(pos)), "triangles": int(sum(len(p[4]) for p in prims) // 3), "sha256": digest})
    lo, hi = np.array(bounds["min_m"]), np.array(bounds["max_m"])
    footprint = [[x, z] for x in (-hi[0] * WU_PER_M, -lo[0] * WU_PER_M) for z in (lo[2] * WU_PER_M, hi[2] * WU_PER_M)]
    manifest = {
        "schema": SCHEMA, "id": pid, "category": prop["category"], "license": LICENSE,
        "source_id": prop.get("source_id", pid),
        "stock_fit": {"scale": scale.tolist(), "yaw_degrees": 90 if turn else 0,
                      "stock_size_wu": stock_size.tolist(), "ratio": ratio.tolist(),
                      "albedo": metrics, "stock_albedo": stock_metrics},
        "source": {"file": prop["file"], "meshes": [l["mesh"] for l in lods], "unity_frame": "left-handed Y-up, metres"},
        "units": {"source": "m", "wu_per_m": WU_PER_M, "frame": "gltf"},
        "bounds": bounds, "footprint_wu": [[round(a, 2), round(b, 2)] for a, b in footprint],
        "lods": lods, "materials": materials_doc,
        "impostor_hint": prop.get("impostor", {"kind": "none"}),
        "replaces": prop.get("replaces", []),
    }
    (pdir / "bt2018_prop_package.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def build_recipe(manifests, out, catalog):
    assets, aliases = [], {}
    for m in manifests:
        keys = m["replaces"]
        if not keys:
            continue
        pdir = out / m["id"]
        assets.append({"frontend": "gltf", "id": m["id"], "replaces": keys[0], "kind": "building" if m["category"] in ("building", "industrial") else "prop",
                       "intact": str(pdir / "lod0.glb"), "frame": "gltf", "wu_per_unit": WU_PER_M,
                       "stock_fit": m["stock_fit"], "texel_scale": 8,
                       "lods": [[str(pdir / l["glb"]), l["distance_wu"]] for l in m["lods"][1:]],
                       "license": LICENSE})
        for extra in keys[1:]:
            aliases[extra] = keys[0]
    return {"mod_id": "bt2018-props", "name": "BT2018 props (personal-use import)", "out": str(out / "mod" / "bt2018-props"),
            "description": "Generated from the owner's BATTLETECH install by tools/bt2018_import; never committed.",
            "assets": assets, "aliases": aliases}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--catalog", type=Path, default=Path(__file__).with_name("props_catalog.json"))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--only", help="comma separated prop ids")
    ap.add_argument("--mapping", type=Path, help="match_stock_props.py output; with --library replaces --catalog")
    ap.add_argument("--library", type=Path)
    ap.add_argument("--bindings", type=Path, help="optional scan_bindings.py output (material names come from the library)")
    ap.add_argument("--material-index", type=Path, help="material name index cache (built on first use)")
    args = ap.parse_args(argv)
    from match_stock_props import mapping_props, check_ids
    if args.mapping:
        bind = json.loads(args.bindings.read_text(encoding="utf-8")) if args.bindings else None
        catalog = {"props": mapping_props(json.loads(args.mapping.read_text(encoding="utf-8"))["map"],
                                          json.loads(args.library.read_text(encoding="utf-8")), bind)}
    else:
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    check_ids(catalog["props"])
    inst = Install(args.install)
    matidx = bake_materials.MaterialIndex(inst, args.material_index or args.out / "material_index.json")
    texcache = bake_materials.TexCache(inst)
    wanted = set(args.only.split(",")) if args.only else None
    manifests, dropped = [], {}
    args.out.mkdir(parents=True, exist_ok=True)
    for prop in catalog["props"]:
        if wanted and prop["id"] not in wanted:
            continue
        try:
            manifest = build_prop(inst, prop, args.out, matidx, texcache)
        except ValueError as exc:
            print("DROP", prop["replaces"], str(exc))
            dropped.update({key: str(exc) for key in prop["replaces"]})
            texcache.full.clear()
            continue
        manifests.append(manifest)
        texcache.full.clear()
        print("prop", prop["id"], manifests[-1]["bounds"]["size_wu"], [l["triangles"] for l in manifests[-1]["lods"]])
    (args.out / "recipe.json").write_text(json.dumps(build_recipe(manifests, args.out, catalog), indent=2) + "\n", encoding="utf-8")
    selection = {"kept": {key: m["source_id"] for m in manifests for key in m["replaces"]}, "dropped": dropped}
    if args.mapping:
        mapping = json.loads(args.mapping.read_text(encoding="utf-8"))["map"]
        selection["dropped"].update({m["key"]: "reviewed none" for m in mapping.values()
                                     if not m.get("pick") or m.get("review") == "none"})
    (args.out / "selection.json").write_text(json.dumps(selection, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
