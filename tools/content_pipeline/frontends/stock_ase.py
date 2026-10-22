"""Stock ASE (+ reviewed CC0 detail mapping) -> IR.

The stock extractor (tools/ase_to_glb.py) supplies geometry, UVs and slots;
stock per-vertex lighting tags travel as COLOR_0. Stock albedo is referenced
(passthrough), never re-encoded. A reviewed mapping may attach authored detail
to a slot: CC0 normal/roughness/AO/metal maps are tiled and baked into the
slot's atlas space, limited to reviewed regions (mask image, rectangles, and
optionally the stock albedo's opaque texels so windows keep the legacy look).
Metal comes only from the declared class or an authored metal map, never from
the albedo colour.
"""
from __future__ import annotations

import math
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

from .. import gltf_io, ir

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
import ase_to_glb as ase  # noqa: E402

PROVIDERS = ("ambientCG", "Poly Haven")
MAP_KEYS = ("normal", "roughnessMap", "metallicMap", "ao", "mask")


def checked_name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_-]+", name):
        raise ValueError(f"unsafe asset name: {name!r}")
    return name


def _scalar(row, key, default, lo=0.0, hi=1.0):
    value = row.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not lo <= value <= hi:
        raise ValueError(f"{key} outside {lo}..{hi}")
    return value


def validate_slot(row):
    """A reviewed detail mapping row (see docs/content-pipeline.md, 'stock_ase recipe')."""
    slot = row.get("slot")
    if isinstance(slot, bool) or not isinstance(slot, int) or not 0 <= slot <= 255:
        raise ValueError("invalid slot")
    if row.get("reviewed") is not True or row.get("class") not in ir.MATERIAL_CLASSES[1:]:
        raise ValueError("explicit reviewed authored material class required")
    regions = row.get("regions") or [row]
    for reg in regions:
        if reg.get("license") != "CC0" or reg.get("provider") not in PROVIDERS:
            raise ValueError("detail requires ambientCG / Poly Haven CC0 provenance")
        if reg.get("rotation", 0) not in (0, 90, 180, 270):
            raise ValueError("rotation must be a quarter turn")
        uv = reg.get("uvScale", [1, 1])
        if len(uv) != 2 or any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
                               or not 0 < v <= 64 for v in uv):
            raise ValueError("invalid UV scale")
        lo_hi = reg.get("roughnessRange", [0, 1])
        if len(lo_hi) != 2 or not 0 <= lo_hi[0] <= lo_hi[1] <= 1:
            raise ValueError("invalid roughness range")
        for x0, y0, x1, y1 in reg.get("maskRects", []):
            if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
                raise ValueError("mask rectangles must be normalized atlas coordinates")
    for key, default, hi in (("normalStrength", 1, 2), ("aoStrength", 1, 1), ("roughness", 1, 1), ("metallic", 0, 1)):
        _scalar(row, key, default, hi=hi)
    if row["class"] not in ir.METALLIC_CLASSES and row.get("metallic", 0) != 0:
        raise ValueError(f"class {row['class']} cannot be metallic")


def _sampled(path, size, scale, rotation, normal=False):
    im = Image.open(path).convert("RGB" if normal else "L")
    im = im.rotate(rotation, expand=True).resize((size, size), Image.Resampling.LANCZOS)
    data = np.asarray(im, dtype=np.float32) / 255
    if normal:
        data = data * 2 - 1
        x, y = data[..., 0].copy(), data[..., 1].copy()
        r = math.radians(rotation)
        data[..., 0] = x * math.cos(r) - y * math.sin(r)
        data[..., 1] = x * math.sin(r) + y * math.cos(r)
    xs = (np.arange(size) * scale[0]).astype(int) % size
    ys = (np.arange(size) * scale[1]).astype(int) % size
    return data[ys[:, None], xs[None, :]]


def _region_mask(reg, size, albedo_alpha):
    if reg.get("mask"):
        mask = np.asarray(Image.open(reg["mask"]).convert("L").resize((size, size), Image.Resampling.NEAREST),
                          dtype=np.float32) / 255
    elif reg.get("maskRects"):
        mask = np.zeros((size, size), dtype=np.float32)
        for x0, y0, x1, y1 in reg["maskRects"]:
            mask[round(y0 * size):round(y1 * size), round(x0 * size):round(x1 * size)] = 1
    else:
        mask = np.ones((size, size), dtype=np.float32)
    if reg.get("excludeAlpha") and albedo_alpha is not None:
        mask *= albedo_alpha
    return mask


def cook_detail(row, size, albedo_alpha=None):
    """Atlas-space (normal RGB, ORM RGBA) images; ORM.a = authored coverage."""
    n = np.zeros((size, size, 3), dtype=np.float32)
    n[..., 2] = 1
    orm = np.zeros((size, size, 3), dtype=np.float32)
    orm[..., :2] = 1
    coverage = np.zeros((size, size), dtype=np.float32)
    for reg in row.get("regions") or [row]:
        scale = reg.get("uvScale", [1, 1])
        rot = reg.get("rotation", 0)
        rn = _sampled(reg["normal"], size, scale, rot, normal=True) if reg.get("normal") else None
        ro = np.ones((size, size, 3), dtype=np.float32)
        ro[..., 2] = 0 if row["class"] not in ir.METALLIC_CLASSES else row.get("metallic", 0)
        for channel, key in enumerate(("ao", "roughnessMap", "metallicMap")):
            if reg.get(key):
                ro[..., channel] = _sampled(reg[key], size, scale, rot)
        lo, hi = reg.get("roughnessRange", [0, 1])
        ro[..., 1] = lo + (hi - lo) * ro[..., 1]
        mask = _region_mask(reg, size, albedo_alpha) * (1 - coverage)   # first region wins overlaps
        if rn is not None:
            n = n * (1 - mask[..., None]) + rn * mask[..., None]
        orm = orm * (1 - mask[..., None]) + ro * mask[..., None]
        coverage += mask
    # Uncovered texels: flat, unoccluded, rough dielectric, coverage 0 = legacy response.
    flat = np.zeros_like(n)
    flat[..., 2] = 1
    n = n * coverage[..., None] + flat * (1 - coverage[..., None])
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-8)
    orm = orm * coverage[..., None] + np.array([1, 1, 0]) * (1 - coverage[..., None])
    as_img = lambda a: np.rint(np.clip(a, 0, 1) * 255).astype(np.uint8)   # noqa: E731
    normal = np.dstack([as_img(n * 0.5 + 0.5), np.full((size, size), 255, np.uint8)])
    return normal, np.dstack([as_img(orm), as_img(coverage)])


def _stock_colors(gltf, raw, geometry, source_text):
    """Per-vertex stock lighting tags (TG keeps the last face's colour per ASE vertex)."""
    blocks = {}
    for block in source_text.split("*GEOMOBJECT")[1:]:
        name = re.search(r'\*NODE_NAME\s+"([^"\n]+)"', block)
        if name:
            blocks[name.group(1)] = block
    objects = {obj.node_name: obj for obj in geometry}
    binary = bytearray(raw)
    for mesh in gltf["meshes"]:
        obj = objects[mesh["name"]]
        block = blocks.get(mesh["name"], "")
        cv = {int(m[0]): tuple(int(float(np.float32(float(v)) * np.float32(255))) for v in m[1:])
              for m in re.findall(r'\*MESH_VERTCOL\s+(\d+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)', block)}
        cf = {int(m[0]): tuple(map(int, m[1:]))
              for m in re.findall(r'\*MESH_CFACE\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', block)}
        by_vertex = {}
        for i, face in enumerate(obj.faces):
            for corner, vi in enumerate(face[:3]):
                by_vertex[vi] = cv.get(cf.get(i, (-1, -1, -1))[corner], (0, 0, 0))
        expanded = {}
        for i, face in enumerate(obj.faces):
            uvface = obj.tfaces[i] if i < len(obj.tfaces) else [0, 0, 0]
            for vi, ti in zip(face[:3], uvface):
                expanded.setdefault((vi, ti), (*by_vertex.get(vi, (0, 0, 0)), 255))
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        for rgba in expanded.values():
            binary.extend(bytes(rgba))
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(expanded) * 4, "target": 34962})
        gltf["accessors"].append({"bufferView": len(gltf["bufferViews"]) - 1, "componentType": 5121,
                                  "normalized": True, "count": len(expanded), "type": "VEC4"})
        for prim in mesh["primitives"]:
            prim["attributes"]["COLOR_0"] = len(gltf["accessors"]) - 1
    gltf["buffers"][0]["byteLength"] = len(binary)
    return bytes(binary)


def _runtime_frame_glb(ase_path):
    """Stock extractor output re-expressed in the importer frame (positions, normals, UVs)."""
    geometry, materials, helpers = ase.parse_ase(str(ase_path))
    gltf, raw, stats = ase.build_glb_data(geometry, materials, helpers)
    raw = _stock_colors(gltf, raw, geometry, Path(ase_path).read_text(encoding="utf-8", errors="replace"))
    binary = bytearray(raw)
    nodes = gltf["nodes"]
    parent = {c: i for i, node in enumerate(nodes) for c in node.get("children", [])}

    def translation(i, chain=()):
        if i in chain:
            raise ValueError("cyclic node hierarchy")
        node = nodes[i]
        if any(k in node for k in ("matrix", "rotation", "scale")):
            raise ValueError("extractor emitted an unhandled node transform")
        p = translation(parent[i], (*chain, i)) if i in parent else [0, 0, 0]
        return [a + b for a, b in zip(p, node.get("translation", [0, 0, 0]))]
    shifts = [translation(i) for i in range(len(nodes))]
    seen = set()
    for i, node in enumerate(nodes):
        for prim in gltf["meshes"][node["mesh"]]["primitives"] if "mesh" in node else []:
            for semantic in ("POSITION", "NORMAL", "TEXCOORD_0"):
                index = prim["attributes"][semantic]
                if index in seen:
                    continue
                seen.add(index)
                acc = gltf["accessors"][index]
                view = gltf["bufferViews"][acc["bufferView"]]
                offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
                if semantic == "TEXCOORD_0":
                    # The extractor flips V; the importer flips once more. Stock TG flips once.
                    for v in range(acc["count"]):
                        u, tv = struct.unpack_from("<2f", binary, offset + v * 8)
                        struct.pack_into("<2f", binary, offset + v * 8, u, 1 - tv)
                    continue
                values = []
                for v in range(acc["count"]):
                    xyz = list(struct.unpack_from("<3f", binary, offset + v * 12))
                    if semantic == "POSITION":
                        xyz = [a + b for a, b in zip(xyz, shifts[i])]
                    xyz = [-xyz[0], -xyz[2], -xyz[1]]
                    values.append(xyz)
                    struct.pack_into("<3f", binary, offset + v * 12, *xyz)
                if semantic == "POSITION":
                    acc["min"] = [min(v[c] for v in values) for c in range(3)]
                    acc["max"] = [max(v[c] for v in values) for c in range(3)]
        node.pop("children", None)
        node.pop("translation", None)
    gltf["scenes"] = [{"nodes": list(range(len(nodes)))}]
    gltf["scene"] = 0
    bitmaps = [gltf["images"][gltf["textures"][m["pbrMetallicRoughness"]["baseColorTexture"]["index"]]["source"]]["uri"]
               if "baseColorTexture" in m.get("pbrMetallicRoughness", {}) else None for m in gltf.get("materials", [])]
    for m in gltf.get("materials", []):
        m.get("pbrMetallicRoughness", {}).pop("baseColorTexture", None)
    gltf.pop("images", None)
    gltf.pop("textures", None)
    return gltf, bytes(binary), stats, bitmaps


def resolve_texture(name, roots):
    """First match through the ordered roots (mod mount order first, stock last)."""
    for root in roots:
        root = Path(root)
        if not root.is_dir():
            continue
        hit = next((p for p in root.iterdir() if p.name.lower() == name.lower()), None)
        if hit is not None:
            return hit
    raise ValueError(f"stock texture {name} not found in {[str(r) for r in roots]}")


def scene_from_ase(ase_path, texture_roots, work_dir, slot_offset=0):
    """(Scene, [legacy_prelit Material]) for one stock ASE; slots offset by slot_offset."""
    gltf, binary, stats, bitmaps = _runtime_frame_glb(ase_path)
    work_dir = Path(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    tmp = work_dir / f"{Path(ase_path).stem}.stock.glb"
    ase.write_glb(gltf, binary, str(tmp))
    scene, _, _ = gltf_io.read_gltf(tmp, frame="mc2_runtime")
    materials = []
    for i, bitmap in enumerate(bitmaps):
        ref = None
        if bitmap:
            stem = Path(bitmap.replace("\\", "/")).name
            ref = ir.TextureRef.of(resolve_texture(stem, texture_roots), "base_color", passthrough=True)
        materials.append(ir.Material(name=f"{Path(ase_path).stem}_{i}", base_color=ref,
                                     alpha_mode="MASK" if ref and Path(ref.path).name.lower().startswith("a_") else "OPAQUE"))
    for mesh in scene.meshes:
        for p in mesh.primitives:
            p.material += slot_offset
    return scene, materials, stats


def parse_ini(path):
    return ase.parse_fitini(str(path))


def to_ir(prop, *, srcdata, texture_roots, slots=(), kind="building", work_dir=None, destroyed=True,
          provenance_extra=(), detail_size=512):
    """IR for one stock appearance; slots are reviewed detail mappings (may be empty)."""
    prop = checked_name(prop)
    srcdata = Path(srcdata)
    work_dir = Path(work_dir or tempfile.mkdtemp(prefix=f"cp-{prop}-"))
    ini_path = srcdata / f"{prop}.ini"
    ini = parse_ini(ini_path)
    lod0 = ase.ini_str(ini, "TGLData", "FileName0", prop) or prop
    provenance = [ir.Source.of("appearance_ini", ini_path)]
    src = srcdata / f"{lod0}.ase"
    scene, materials, _ = scene_from_ase(src, texture_roots, work_dir)
    provenance.append(ir.Source.of("geometry", src))
    states = {"intact": [ir.Lod(scene)]}
    dam = ase.ini_str(ini, "TGLDamage", "FileName", "")
    if destroyed and dam:
        dsrc = srcdata / f"{dam}.ase"
        dscene, dmats, _ = scene_from_ase(dsrc, texture_roots, work_dir, slot_offset=len(materials))
        materials += dmats
        states["destroyed"] = [ir.Lod(dscene)]
        provenance.append(ir.Source.of("geometry_destroyed", dsrc))
    for m in materials:
        if m.base_color:
            provenance.append(ir.Source("base_color", m.base_color.path, m.base_color.sha256, "stock"))
    for row in slots:
        validate_slot(row)
        slot = row["slot"]
        if slot >= len(materials):
            raise ValueError(f"{prop}: mapping slot {slot} does not exist")
        m = materials[slot]
        stock_name = Path(m.base_color.path).name.lower() if m.base_color else None
        if stock_name != row["stockAlbedo"].lower():
            raise ValueError(f"{prop}: reviewed albedo {row['stockAlbedo']} differs from stock slot {stock_name}")
        alpha = None
        if any(r.get("excludeAlpha") for r in (row.get("regions") or [row])):
            a = np.asarray(Image.open(m.base_color.path).convert("RGBA").resize((detail_size, detail_size),
                           Image.Resampling.NEAREST), dtype=np.float32)[..., 3] / 255
            alpha = (a >= 0.999).astype(np.float32)
        for reg in row.get("regions") or [row]:
            for key in MAP_KEYS:
                if reg.get(key):
                    provenance.append(ir.Source.of(f"detail_{key}", reg[key], license="CC0",
                                                   provider=reg["provider"], url=reg.get("sourceUrl", "")))
        normal, orm = cook_detail(row, detail_size, alpha)
        npath, opath = work_dir / f"{prop}_{slot}_normal.png", work_dir / f"{prop}_{slot}_orm.png"
        Image.fromarray(normal, "RGBA").save(npath)
        Image.fromarray(orm, "RGBA").save(opath)
        cls = row["class"]
        materials[slot] = ir.Material.for_class(
            m.name, cls, alpha_mode=m.alpha_mode, base_color_policy="stock", base_color=m.base_color,
            normal=ir.TextureRef.of(npath, "normal") if any(r.get("normal") for r in (row.get("regions") or [row])) else None,
            orm=ir.TextureRef.of(opath, "orm"), normal_strength=row.get("normalStrength", 1.0),
            ao_strength=row.get("aoStrength", 1.0), roughness=row.get("roughness", 1.0),
            metallic=row.get("metallic", ir.CLASS_DEFAULTS[cls][0] if cls in ir.METALLIC_CLASSES else 0.0),
            reviewed=True, review_note=row.get("reviewNote", ""))
    provenance += list(provenance_extra)
    return ir.Asset(id=prop.lower(), kind=kind, replaces=f"staticProp:{lod0.lower()}", materials=materials,
                    states=states, provenance=provenance,
                    frontend={"name": "stock_ase", "prop": prop, "slots": [dict(r) for r in slots]}).check()
