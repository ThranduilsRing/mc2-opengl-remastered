"""Cheap offline texture cleanup, alpha-aware mips and authoring normal maps.

Pillow resampling cannot recover detail that is absent in stock textures. This
path provides a reproducible starting point and preserves the stock geometry.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import struct

import numpy as np
from PIL import Image, ImageFilter

from tools.assets.common import (INSTALL_ROOT, OUT_ROOT, output_path, parse_tgl,
                                stock_install, texture_image, write_json)


def upscale(image, factor, max_edge):
    rgba = image.convert("RGBA")
    ratio = min(factor, max_edge/max(rgba.size))
    size = tuple(max(1, round(v*ratio)) for v in rgba.size)
    # Premultiplied resizing avoids black fringes around alpha lattice wires.
    enlarged = rgba.convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
    rgb = enlarged.convert("RGB").filter(ImageFilter.UnsharpMask(radius=1, percent=90, threshold=3))
    rgb.putalpha(enlarged.getchannel("A"))
    return rgb


def mip_chain(image):
    result = [image.convert("RGBA")]
    target_coverage = float(np.mean(np.asarray(result[0])[:, :, 3] >= 128))
    while max(result[-1].size) > 1:
        size = tuple(max(1, v//2) for v in result[-1].size)
        mip = result[-1].convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
        if 0 < target_coverage < 1:
            rgba = np.array(mip)
            alpha = rgba[:, :, 3].astype(float)
            # Quantile shift keeps alpha-test coverage as close as discrete pixels allow.
            threshold = np.quantile(alpha, 1-target_coverage)
            rgba[:, :, 3] = np.clip(alpha + 128-threshold, 0, 255).astype(np.uint8)
            mip = Image.fromarray(rgba)
        result.append(mip)
    return result


def normal_map(image, strength=1):
    rgb = np.asarray(image.convert("RGB"), dtype=float)/255
    value = rgb @ np.array([.2126, .7152, .0722])
    # Periodic central difference works for 1-pixel-wide/tall textures too.
    dx = (np.roll(value, -1, 1)-np.roll(value, 1, 1))*.5
    dy = (np.roll(value, -1, 0)-np.roll(value, 1, 0))*.5
    normal = np.stack([-strength*dx, strength*dy, np.ones_like(dx)], axis=2)
    normal /= np.linalg.norm(normal, axis=2)[:, :, None]
    return Image.fromarray(np.clip((normal*.5+.5)*255, 0, 255).astype(np.uint8))


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


def offline_upscalers():
    names = ("realesrgan-ncnn-vulkan", "upscayl-bin", "waifu2x-ncnn-vulkan", "magick", "texconv", "toktx", "blender")
    found = {name: shutil.which(name) for name in names}
    blender = Path("C:/Program Files/Blender Foundation/Blender 5.1/blender.exe")
    if not found["blender"] and blender.is_file():
        found["blender"] = str(blender)
    # The common installed desktop upscalers do not necessarily expose PATH binaries.
    roots = [Path("C:/Program Files/Upscayl"), Path.home()/"AppData/Local/Programs/upscayl",
             Path("G:/mc2-assets/tools"), Path.home()/".cache/torch/hub/checkpoints"]
    local_files = []
    for root in roots:
        if root.is_dir():
            local_files += [str(p) for p in root.rglob("*") if p.suffix.lower() in (".exe", ".pth", ".onnx", ".param", ".bin")]
    return {"builtin": "Pillow Lanczos + unsharp; no learned detail synthesis",
            "path_binaries": found, "known_local_locations": [str(p) for p in roots],
            "local_binaries_or_weights": sorted(local_files), "downloads_performed": False,
            "scope": "PATH and listed local locations only; no claim to scan every disk"}


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
              "runtime": "stock TGL clones, texture name fields only; installed mods untouched",
              "mips_normals": "authoring artifacts only: generic model_override loader disables mipmaps and has no generic normal binding; not auto-activated",
              "upscalers": offline_upscalers()}
    for name, image in images.items():
        newname = names[name]
        for tier in (32, 64, 128, 256):
            folder = out/"data"/"tgl"/str(tier)
            folder.mkdir(parents=True, exist_ok=True)
            image.save(folder/newname)
        mipdir = out/"authoring"/Path(newname).stem
        mipdir.mkdir(parents=True, exist_ok=True)
        normal_map(image).save(mipdir/"normal.tga")
        mips = mip_chain(image)
        for level, mip in enumerate(mips):
            mip.save(mipdir/f"mip{level:02d}.tga")
        report["textures"].append({"stock": name, "cooked": newname,
                                   "resolution": list(image.size), "mip_levels": len(mips)})
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
Authoring mips and luminance-derived normals are included for editing, not runtime
binding. Lanczos interpolation adds pixels but cannot recover missing detail.
"""
    (out/"modeller-brief.md").write_text(brief, encoding="utf-8")
    return out
