"""Cheap offline texture cleanup with alpha-aware runtime texture tiers.

Pillow resampling cannot recover detail that is absent in stock textures. This
path provides a reproducible starting point and preserves the stock geometry.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct

import numpy as np
from PIL import Image, ImageFilter

from tools.assets.common import (INSTALL_ROOT, OUT_ROOT, output_path, parse_tgl,
                                stock_install, texture_image, texture_tiers, write_json)


def upscale(image, factor, max_edge):
    rgba = image.convert("RGBA")
    ratio = min(factor, max_edge/max(rgba.size))
    size = tuple(max(1, round(v*ratio)) for v in rgba.size)
    # Premultiplied resizing avoids black fringes around alpha lattice wires.
    enlarged = rgba.convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
    rgb = enlarged.convert("RGB").filter(ImageFilter.UnsharpMask(radius=1, percent=90, threshold=3))
    rgb.putalpha(enlarged.getchannel("A"))
    return rgb


def rename_tgl_textures(raw, names):
    """Change only fixed-width texture-name fields; all geometry bytes stay intact."""
    result = bytearray(raw)
    version, _, count = struct.unpack_from("<III", result)
    if version != 0xBAFDECAF or 12+268*count > len(result):
        raise ValueError("invalid TGL texture table")
    for i in range(count):
        start = 12+i*268
        current = bytes(result[start:start+256]).split(b"\0")[0].decode("latin1").replace("\\", "/").split("/")[-1].lower()
        if current in names:
            name = names[current].encode("ascii")
            if len(name) >= 256:
                raise ValueError("texture name too long")
            result[start:start+256] = name.ljust(256, b"\0")
    return bytes(result)


def rework(entry, mod_id, *, root=INSTALL_ROOT, factor=4):
    import tempfile
    from tools.assets.replace import safe_mod_id
    destination = output_path(OUT_ROOT/safe_mod_id(mod_id), mod=True)
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite package: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{mod_id}-", dir=destination.parent) as folder:
        staging = output_path(folder, mod=True)
        _rework(entry, mod_id, root=root, factor=factor, destination=staging)
        staging.rename(destination)
    return destination


def _rework(entry, mod_id, *, root=INSTALL_ROOT, factor=4, destination=None):
    from tools.assets.replace import safe_mod_id, target_spec
    from tools.assets.render import comparison, mesh_textures
    mod_id = safe_mod_id(mod_id)
    if factor not in (2, 4):
        raise ValueError("factor must be 2 or 4")
    spec = target_spec(entry)
    stock = stock_install(root)
    models = [m for m in entry["models"] if m["role"] in ("intact", "destroyed") and "bounds" in m]
    meshes = {m["tgl"]: parse_tgl(stock.read(m["tgl"])[0]) for m in models}
    used_names = sorted({mesh.textures[int(slot)] for mesh in meshes.values() for slot in set(mesh.materials)})
    images, names = {}, {}
    for index, name in enumerate(used_names):
        image, _, _ = texture_image(stock, name)  # preflight every required texture before any writes
        images[name] = upscale(image, factor, spec["budgets"]["max_texture_edge"])
        names[name] = ("a_" if name.startswith("a_") else "")+f"{mod_id}_t{index:03d}.tga"
    out = output_path(destination or OUT_ROOT/mod_id, mod=True)
    if out.exists() and destination is None:
        raise FileExistsError(f"refusing to overwrite package: {out}")
    report = {"method": "Lanczos premultiplied-alpha resize + RGB unsharp mask",
              "learned_upscaling": False, "factor_requested": factor, "geometry_preserved": True,
              "texture_names_isolated": True, "models": [], "textures": [],
              "runtime": "stock TGL clones, texture name fields only; installed mods untouched"}
    for name, image in images.items():
        newname = names[name]
        resolutions = {}
        for tier, reduced in texture_tiers(image):
            folder = out/"data"/"tgl"/str(tier)
            folder.mkdir(parents=True, exist_ok=True)
            reduced.save(folder/newname)
            resolutions[str(tier)] = list(reduced.size)
        report["textures"].append({"stock": name, "cooked": newname,
                                   "resolution": list(image.size), "tier_resolutions": resolutions})
    for model in models:
        raw = stock.read(model["tgl"])[0]
        cooked = rename_tgl_textures(raw, names)
        original, check = meshes[model["tgl"]], parse_tgl(cooked)
        if not (np.array_equal(original.vertices, check.vertices) and np.array_equal(original.uv, check.uv)
                and np.array_equal(original.materials, check.materials)):
            raise ValueError("geometry changed during texture-name patch")
        target = out/model["tgl"]
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(cooked)
        report["models"].append({"tgl": model["tgl"], "role": model["role"], "lod": model["lod"],
                                 "stock_sha256": hashlib.sha256(raw).hexdigest(),
                                 "cooked_sha256": hashlib.sha256(cooked).hexdigest(), "geometry_equal": True})
        if model["lod"] == 0:
            original_tex = mesh_textures(original, stock)
            tex = [images.get(name, original_tex[i]) for i, name in enumerate(original.textures)]
            comparison(original, check, original_tex, tex, out/f"preview-{model['role']}.png")
    write_json(out/"mod.json", {"schema": "mc2-mod/1", "id": mod_id,
        "name": "Texture rework "+entry["id"], "version": "1.0.0", "dependencies": []})
    write_json(out/"target-spec.json", spec); write_json(out/"rework-report.json", report)
    brief = f"""# Modeller brief: {entry['id']}

Keep the stock pivot (0,0,0), TG +Y up, and measured bounds {spec['bounds']}.
Nominal front is -Z; verify the hangar entrance / fence endpoints in the stock preview.
Target at most {spec['budgets']['max_triangles_lod0']} triangles and
{spec['budgets']['max_texture_edge']} px texture edges. Preserve gameplay collision
and fence tiling endpoints; bake scene transforms and use identity nodes.
Rebuild silhouette, roof seams, posts and damage rubble before adding small detail.
Provide intact and destroyed states, UVs, albedo and authored normals; lower LODs
must preserve footprint and silhouette. Use opaque structural posts plus a sparse
alpha lattice for wire fences, rather than thousands of tiny wire cylinders.
Current runtime activates intact staticProp replacement only; destroyed geometry
stays stock, while this texture package reworks both states via stock TGL clones.
Tier 256 retains the enlarged image; lower tiers halve each dimension per step.
Material authoring belongs to the Blender/PBR pipeline. Lanczos interpolation
adds pixels but cannot recover missing detail.
"""
    (out/"modeller-brief.md").write_text(brief, encoding="utf-8")
    return out
