#!/usr/bin/env python3
"""Cook reviewed prop/slot detail into a Vulkan override bundle.

Uses the stock ASE extractor; keeps albedo bytes, geometry and UVs. Detail
sources are explicit CC0 mappings, never guesses from albedo colour. Sources
are copied into the authoring archive before use; Downloads remains read-only.
Run: py -3 tools/blender_pipeline/material_map.py mapping.json --out BUNDLE
"""
import argparse
import hashlib
import json
import math
import re
import shutil
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import ase_to_glb as ase
sys.path.insert(0, str(HERE))
import batch_driver as pipeline
from srcdata import find_tgl_dir

CLASSES = {"dielectric", "painted_metal", "bare_metal", "rust"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def checked_name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_-]+", name):
        raise ValueError(f"unsafe asset/mod name: {name!r}")
    return name


def scalar(row, key, default, lo=0, hi=1):
    value = row.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not lo <= value <= hi:
        raise ValueError(f"{key} outside {lo}..{hi}")
    return value


def validate_mapping(mapping):
    if mapping.get("version") != 1 or not mapping.get("props"):
        raise ValueError("mapping requires version 1 and props")
    checked_name(mapping["id"])
    names = set()
    for prop in mapping["props"]:
        name = checked_name(prop["prop"])
        if name in names:
            raise ValueError("duplicate prop")
        names.add(name)
        slots = set()
        for row in prop["slots"]:
            slot = row["slot"]
            if isinstance(slot, bool) or not isinstance(slot, int) or not 0 <= slot <= 255 or slot in slots:
                raise ValueError("invalid/duplicate slot")
            slots.add(slot)
            if row.get("reviewed") is not True or row.get("class") not in CLASSES:
                raise ValueError("explicit reviewed material class required")
            if row.get("license") != "CC0" or row.get("provider") not in ("ambientCG", "Poly Haven"):
                raise ValueError("detail requires ambientCG/Poly Haven CC0 provenance")
            if not row.get("normal") and not row.get("roughnessMap"):
                raise ValueError("normal or roughness map required")
            for key, default, hi in (("normalStrength", 1, 2), ("aoStrength", 1, 1), ("roughness", 1, 1), ("metallic", 0, 1)):
                scalar(row, key, default, hi=hi)
            if row["class"] == "dielectric" and row.get("metallic", 0) != 0:
                raise ValueError("dielectric cannot be metallic")
            uv = row.get("uvScale", [1, 1])
            if len(uv) != 2 or any(not isinstance(v, (int, float)) or not math.isfinite(v) or not 0 < v <= 64 for v in uv):
                raise ValueError("invalid UV scale")
            if row.get("rotation", 0) not in (0, 90, 180, 270):
                raise ValueError("rotation must be a quarter turn")
            bounds = row.get("roughnessRange", [0, 1])
            if len(bounds) != 2 or any(not isinstance(v, (int, float)) or not math.isfinite(v) for v in bounds) or not 0 <= bounds[0] <= bounds[1] <= 1:
                raise ValueError("invalid roughness range")
        if not slots:
            raise ValueError("prop has no mapped slots")


def archive_source(path, archive, report):
    path = Path(path).resolve()
    sha = digest(path)
    dest = archive / sha / path.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists():
        shutil.copyfile(path, dest)
    if digest(dest) != sha:
        raise ValueError(f"archive differs: {dest}")
    report.append({"source": str(path), "archived": str(dest), "sha256": sha})
    return dest


def sampled_image(path, size, scale, rotation, normal=False):
    im = Image.open(path).convert("RGB" if normal else "L")
    im = im.rotate(rotation, expand=True).resize((size, size), Image.Resampling.LANCZOS)
    data = np.asarray(im, dtype=np.float32) / 255
    if normal:
        data = data * 2 - 1
        x, y = data[..., 0].copy(), data[..., 1].copy()
        radians = math.radians(rotation)
        data[..., 0] = x * math.cos(radians) - y * math.sin(radians)
        data[..., 1] = x * math.sin(radians) + y * math.cos(radians)
    # Bake UV tiling before the atlas mask, keeping stock atlas exclusions fixed.
    xs = (np.arange(size) * scale[0]).astype(int) % size
    ys = (np.arange(size) * scale[1]).astype(int) % size
    return data[ys[:, None], xs[None, :]]


def cook_detail(row, size, paths):
    scale = row.get("uvScale", [1, 1]); rotation = row.get("rotation", 0)
    n = np.zeros((size, size, 3), dtype=np.float32); n[..., 2] = 1
    if "normal" in paths:
        n = sampled_image(paths["normal"], size, scale, rotation, normal=True)
    orm = np.ones((size, size, 3), dtype=np.float32)
    for channel, key in enumerate(("ao", "roughnessMap", "metallicMap")):
        if key in paths:
            orm[..., channel] = sampled_image(paths[key], size, scale, rotation)
    low, high = row.get("roughnessRange", [0, 1])
    orm[..., 1] = low + (high-low) * orm[..., 1]
    if "mask" in paths:
        mask = np.asarray(Image.open(paths["mask"]).convert("L").resize((size, size), Image.Resampling.NEAREST), dtype=np.float32) / 255
    elif row.get("maskRects"):
        mask = np.zeros((size, size), dtype=np.float32)
        for x0, y0, x1, y1 in row["maskRects"]:
            if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
                raise ValueError("mask rectangles must be normalized atlas coordinates")
            mask[round(y0*size):round(y1*size), round(x0*size):round(x1*size)] = 1
    else:
        mask = None
    if mask is not None:
        flat = np.zeros_like(n); flat[..., 2] = 1
        n = n * mask[..., None] + flat * (1 - mask[..., None])
        # Excluded atlas regions: flat, no AO, rough dielectric (metal zero).
        orm = orm * mask[..., None] + np.array([1, 1, 0]) * (1 - mask[..., None])
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-8)
    def image(array):
        return Image.fromarray(np.rint(np.clip(array, 0, 1) * 255).astype(np.uint8))
    return image(n * 0.5 + 0.5), image(orm)


def add_stock_colors(gltf, raw, geometry, source_text):
    """Preserve TG's per-vertex lighting tags (last face wins per ASE vertex)."""
    blocks = {}
    for block in source_text.split("*GEOMOBJECT")[1:]:
        name = re.search(r'\*NODE_NAME\s+"([^"\n]+)"', block)
        if name:
            blocks[name.group(1)] = block
    objects = {obj.node_name: obj for obj in geometry}
    binary = bytearray(raw)
    for mesh in gltf["meshes"]:
        obj = objects[mesh["name"]]; block = blocks.get(mesh["name"], "")
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
        count = len(expanded)
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        for rgba in expanded.values():
            binary.extend(bytes(rgba))
        view = len(gltf["bufferViews"])
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": count*4, "target": 34962})
        accessor = len(gltf["accessors"])
        gltf["accessors"].append({"bufferView": view, "componentType": 5121, "normalized": True, "count": count, "type": "VEC4"})
        for primitive in mesh["primitives"]:
            primitive["attributes"]["COLOR_0"] = accessor
    gltf["buffers"][0]["byteLength"] = len(binary)
    return bytes(binary)


def stock_glb(ase_path):
    """Use stock extractor, baking its node translations and axis into vertices.

    Stock ASE vertices are world coordinates. The importer default maps glTF
    (-x,-y,z); the stock TG parser maps ASE (-x,z,y). This conversion composes
    them without a runtime axis override or changed node/triangle/UV layout.
    """
    geometry, materials, helpers = ase.parse_ase(str(ase_path))
    gltf, raw, stats = ase.build_glb_data(geometry, materials, helpers)
    raw = add_stock_colors(gltf, raw, geometry, Path(ase_path).read_text(encoding="utf-8", errors="replace"))
    binary = bytearray(raw)
    nodes = gltf["nodes"]
    parent = {child: i for i, node in enumerate(nodes) for child in node.get("children", [])}
    def translation(i, chain=()):
        if i in chain:
            raise ValueError("cyclic node hierarchy")
        node = nodes[i]
        if "matrix" in node or "rotation" in node or "scale" in node:
            raise ValueError("extractor emitted an unhandled node transform")
        p = translation(parent[i], (*chain, i)) if i in parent else [0, 0, 0]
        return [a+b for a, b in zip(p, node.get("translation", [0, 0, 0]))]
    shifts = [translation(i) for i in range(len(nodes))]
    seen = set()
    for i, node in enumerate(nodes):
        if "mesh" in node:
            for prim in gltf["meshes"][node["mesh"]]["primitives"]:
                for semantic in ("POSITION", "NORMAL", "TEXCOORD_0"):
                    index = prim["attributes"][semantic]
                    if index in seen:
                        continue
                    seen.add(index)
                    acc = gltf["accessors"][index]; view = gltf["bufferViews"][acc["bufferView"]]
                    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
                    if semantic == "TEXCOORD_0":
                        # The converter and Assimp each flip V. Stock TG flips
                        # once, so undo the converter's flip before import.
                        for v in range(acc["count"]):
                            u, tex_v = struct.unpack_from("<2f", binary, offset+v*8)
                            struct.pack_into("<2f", binary, offset+v*8, u, 1-tex_v)
                        continue
                    values = []
                    for v in range(acc["count"]):
                        xyz = list(struct.unpack_from("<3f", binary, offset+v*12))
                        if semantic == "POSITION":
                            xyz = [a+b for a, b in zip(xyz, shifts[i])]
                        xyz = [-xyz[0], -xyz[2], -xyz[1]]
                        values.append(xyz)
                        struct.pack_into("<3f", binary, offset+v*12, *xyz)
                    if semantic == "POSITION":
                        acc["min"] = [min(v[c] for v in values) for c in range(3)]
                        acc["max"] = [max(v[c] for v in values) for c in range(3)]
        node.pop("children", None); node.pop("translation", None)
        if "mesh" not in node:
            x, y, z = shifts[i]; node["translation"] = [-x, -z, -y]
    gltf["scenes"] = [{"nodes": list(range(len(nodes)))}]
    return gltf, bytes(binary), stats


def build(mapping, out, archive, srcdata, textures, size=512, only=None):
    validate_mapping(mapping)
    if size not in (128, 256, 512, 1024):
        raise ValueError("detail size must be 128/256/512/1024")
    out, archive, srcdata, textures = map(Path, (out, archive, srcdata, textures))
    report = {"version": 1, "id": mapping["id"], "mapping": mapping, "sources": [], "props": [],
              "accepted": False, "note": "Candidate only until viewed day/night montages pass; intact overrides only."}
    records = []
    for prop in mapping["props"]:
        name = prop["prop"]
        if only and name not in only:
            continue
        source = archive_source(srcdata / f"{name}.ase", archive, report["sources"])
        archive_source(srcdata / f"{name}.ini", archive, report["sources"])
        gltf, binary, stats = stock_glb(source)
        # BuildTextureList preserves one slot per Assimp material. Two slots
        # may share stock pixels while selecting different detail materials.
        slots = {}
        original_images, original_textures = gltf.get("images", []), gltf.get("textures", [])
        gltf["images"], gltf["textures"] = [], []
        for slot, material in enumerate(gltf.get("materials", [])):
            pbr = material.get("pbrMetallicRoughness", {})
            if "baseColorTexture" not in pbr:
                continue
            original = original_images[original_textures[pbr["baseColorTexture"]["index"]]["source"]]
            source_name = Path(original["uri"].replace("\\", "/")).name
            stock = next((p for p in textures.iterdir() if p.name.lower() == source_name.lower()), None)
            if stock is None:
                raise ValueError(f"missing stock albedo {source_name}")
            archived = archive_source(stock, archive, report["sources"])
            private = f"{'a_' if source_name.lower().startswith('a_') else ''}pbr_{mapping['id']}_{name}_{slot}.tga"
            target = out / "data/tgl/128" / private
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(archived, target)  # byte-for-byte stock albedo; no diffuse from PBR set
            # Preserve the displayed stock decode too when the loader prefers
            # its BC7 sidecar over TGA. A private name must not lose that sidecar.
            ktx = stock.with_suffix(".ktx2")
            if ktx.is_file():
                copied = archive_source(ktx, archive, report["sources"])
                shutil.copyfile(copied, target.with_suffix(".ktx2"))
            pbr["baseColorTexture"]["index"] = len(gltf["textures"])
            gltf["textures"].append({"source": len(gltf["images"])})
            gltf["images"].append({"uri": private})
            slots[slot] = source_name.lower()
        runtime = []
        for row in prop["slots"]:
            if slots.get(row["slot"]) != row["stockAlbedo"].lower():
                raise ValueError(f"{name}: reviewed albedo/slot differs from stock extractor")
            paths = {key: archive_source(row[key], archive, report["sources"])
                     for key in ("normal", "roughnessMap", "metallicMap", "ao", "mask") if row.get(key)}
            normal, orm = cook_detail(row, size, paths)
            stem = f"detail/{name}_{row['slot']}"
            folder = out / "data/model_overrides/detail"; folder.mkdir(parents=True, exist_ok=True)
            normal.convert("RGBA").save(out / "data/model_overrides" / f"{stem}_normal.tga", compression=None)
            orm.convert("RGBA").save(out / "data/model_overrides" / f"{stem}_orm.tga", compression=None)
            entry = {key: row[key] for key in ("slot", "reviewed", "class", "normalStrength", "aoStrength", "roughness", "metallic") if key in row}
            entry.update(normal=f"{stem}_normal.tga", orm=f"{stem}_orm.tga", uvScale=[1, 1])
            runtime.append(entry)
        model = out / "data/model_overrides/models" / f"{name}.glb"
        model.parent.mkdir(parents=True, exist_ok=True)
        pipeline.write_glb(gltf, binary, model)
        records.append({"type": "model", "replaces": f"staticProp:{name}", "source": f"models/{name}.glb",
                        "renderOnly": True, "fallback": "stock", "scale": 1,
                        "materials": {"version": 1, "slots": runtime}})
        report["props"].append({"name": name, "stock_geometry": stats, "glb_sha256": digest(model)})
    if not records:
        raise ValueError("no selected props")
    (out / "data/model_overrides/models.json").write_text(json.dumps({"overrides": records}, indent=2)+"\n", encoding="utf-8")
    (out / "mod.json").write_text(json.dumps({"id": mapping["id"], "name": mapping["id"], "version": "1.0", "description": "Reviewed PBR detail, stock albedo and geometry"}, indent=2)+"\n", encoding="utf-8")
    (out / "material-report.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mapping", type=Path); parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--archive", type=Path, default=Path("G:/mc2-assets/pbr"))
    parser.add_argument("--srcdata", type=Path, default=None)
    parser.add_argument("--textures", type=Path, required=True)
    parser.add_argument("--size", type=int, default=512); parser.add_argument("--only", action="append")
    args = parser.parse_args(argv)
    try:
        report = build(json.loads(args.mapping.read_text(encoding="utf-8")), args.out, args.archive, find_tgl_dir(args.srcdata), args.textures, args.size, args.only)
    except (ValueError, KeyError, OSError, RuntimeError) as error:
        parser.exit(2, f"material_map: {error}\n")
    print(f"material_map: {len(report['props'])} candidate props -> {args.out}; stock albedo bytes preserved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
