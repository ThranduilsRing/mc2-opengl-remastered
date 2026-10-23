"""Small orthographic, textured, depth-buffered CPU renderer (Pillow + NumPy)."""
from __future__ import annotations

from collections import defaultdict
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from tools.assets.common import bounds, output_path, parse_tgl, texture_image, write_json


def font(size=13):
    try:
        return ImageFont.truetype("C:/Windows/Fonts/consola.ttf", size)
    except OSError:
        return ImageFont.load_default()


def render(mesh, textures=(), *, size=(224, 160), frame=None, yaw=-35):
    """TG +Y is up. Face lighting + alpha cutouts; no runtime shadow/PBR claim."""
    width, height = size
    pixels = np.full((height, width, 3), (32, 39, 48), dtype=np.uint8)
    depth = np.full((height, width), -np.inf)
    angle, elev = math.radians(yaw), math.radians(28)
    right = np.array([math.cos(angle), 0, -math.sin(angle)])
    look = np.array([math.sin(angle)*math.cos(elev), math.sin(elev), math.cos(angle)*math.cos(elev)])
    up = np.cross(look, right)
    basis = np.array([right, up, look]).T
    vertices = np.asarray(mesh.vertices) @ basis
    if frame is None:
        lo, hi = vertices.min(axis=0), vertices.max(axis=0)
    else:
        lo0, hi0 = np.array(frame["min"]), np.array(frame["max"])
        corners = np.array([[x, y, z] for x in (lo0[0], hi0[0]) for y in (lo0[1], hi0[1]) for z in (lo0[2], hi0[2])]) @ basis
        lo, hi = corners.min(axis=0), corners.max(axis=0)
    scale = min((width-20)/max(hi[0]-lo[0], 1e-5), (height-16)/max(hi[1]-lo[1], 1e-5))
    vertices[:, 0] = (vertices[:, 0]-(lo[0]+hi[0])/2)*scale+width/2
    vertices[:, 1] = height/2-(vertices[:, 1]-(lo[1]+hi[1])/2)*scale
    texarrays = [np.asarray(t.convert("RGBA")) if t is not None else None for t in textures]
    light = np.array([-.4, .8, .5]); light /= np.linalg.norm(light)
    tris = np.asarray(mesh.vertices).reshape(-1, 3, 3)
    for i, tri in enumerate(vertices.reshape(-1, 3, 3)):
        minx, miny = np.maximum(np.floor(tri[:, :2].min(axis=0)).astype(int), 0)
        maxx, maxy = np.minimum(np.ceil(tri[:, :2].max(axis=0)).astype(int), (width-1, height-1))
        if maxx < minx or maxy < miny:
            continue
        a, b, c = tri
        denom = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        if abs(denom) < 1e-9:
            continue
        yy, xx = np.mgrid[miny:maxy+1, minx:maxx+1].astype(float)
        xx += .5; yy += .5
        w0 = ((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/denom
        w1 = ((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/denom
        w2 = 1-w0-w1
        z = w0*a[2]+w1*b[2]+w2*c[2]
        target = depth[miny:maxy+1, minx:maxx+1]
        mask = (w0 >= -1e-7) & (w1 >= -1e-7) & (w2 >= -1e-7) & (z > target)
        if not mask.any():
            continue
        material = int(mesh.materials[i])
        tex = texarrays[material] if material < len(texarrays) else None
        if tex is not None:
            uv = mesh.uv[i*3:i*3+3]
            u = w0*uv[0, 0]+w1*uv[1, 0]+w2*uv[2, 0]
            v = w0*uv[0, 1]+w1*uv[1, 1]+w2*uv[2, 1]
            rgba = tex[np.floor(v*tex.shape[0]).astype(int)%tex.shape[0],
                       np.floor(u*tex.shape[1]).astype(int)%tex.shape[1]]
            mask &= rgba[:, :, 3] >= 128
            rgb = rgba[:, :, :3]
        else:
            rgb = np.full((*z.shape, 3), (160, 172, 184))
        n = np.cross(tris[i, 1]-tris[i, 0], tris[i, 2]-tris[i, 0])
        shade = .45+.55*abs(float(n @ light))/max(np.linalg.norm(n), 1e-10)
        dest = pixels[miny:maxy+1, minx:maxx+1]
        dest[mask] = np.clip(rgb[mask]*shade, 0, 255).astype(np.uint8)
        target[mask] = z[mask]
    return Image.fromarray(pixels)


def mesh_textures(mesh, install, cache=None):
    cache = cache if cache is not None else {}
    result = []
    for name in mesh.textures:
        if name not in cache:
            try:
                cache[name] = texture_image(install, name)[0]
            except (OSError, ValueError):
                cache[name] = None
        result.append(cache[name])
    return result


def comparison(stock_mesh, candidate_mesh, stock_tex, candidate_tex, path):
    frame = bounds(np.concatenate([stock_mesh.vertices, candidate_mesh.vertices]))
    out = Image.new("RGB", (768, 300), (24, 30, 38))
    out.paste(render(stock_mesh, stock_tex, size=(384, 272), frame=frame), (0, 28))
    out.paste(render(candidate_mesh, candidate_tex, size=(384, 272), frame=frame), (384, 28))
    draw = ImageDraw.Draw(out)
    draw.text((12, 7), "STOCK", font=font(15), fill="white")
    draw.text((396, 7), "PROPOSED (shared camera and scale)", font=font(15), fill="white")
    path = output_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    out.save(path)


def sheets(catalog, install, out):
    out = output_path(out)
    (out/"thumbs").mkdir(parents=True, exist_ok=True)
    groups, textures, rendered, failures = defaultdict(list), {}, {}, []
    for obj in catalog["objects"]:
        for model in obj["models"]:
            if model["role"] in ("intact", "destroyed"):
                groups[obj["category"]].append((obj["id"], model))
            elif "shadow" in model["role"]:
                groups["shadow"].append((obj["id"], model))
    # Include remaining binary shapes so the sheet index accounts for the full archive.
    for model in catalog["unassigned_models"]:
        groups["unassigned"].append((Path(model["tgl"]).stem, model))
    index = {"renderer": "offline orthographic TG +Y up; alpha>=128; two-sided face lighting; no engine PBR/shadow simulation", "pages": [], "errors": failures}
    links = ["# Stock model contact sheets", "", index["renderer"], "", "Every tile fits its own bounds; compare dimensions in catalog.json. LOD and damage roles are labeled. Grey means a missing texture; explicit red labels mean a decode failure.", ""]
    for category, items in sorted(groups.items()):
        # One tile per unique referenced model in each category.
        unique = {}
        for name, model in items:
            unique.setdefault(model["tgl"], (name, model))
        items = sorted(unique.values(), key=lambda t: (t[0], t[1]["role"], t[1]["lod"]))
        for offset in range(0, len(items), 30):
            page = Image.new("RGB", (224*6, 220*5+38), (23, 29, 37))
            draw = ImageDraw.Draw(page)
            draw.text((12, 9), f"{category.upper()}   {offset+1}-{min(offset+30, len(items))} / {len(items)}", font=font(18), fill="white")
            for tile, (name, model) in enumerate(items[offset:offset+30]):
                path = model["tgl"]
                if path not in rendered:
                    try:
                        mesh = parse_tgl(install.read(path)[0])
                        thumb = render(mesh, mesh_textures(mesh, install, textures))
                    except (OSError, ValueError) as e:
                        thumb = Image.new("RGB", (224, 160), (60, 20, 25))
                        ImageDraw.Draw(thumb).text((6, 65), "DECODE FAILED", fill="white", font=font())
                        failures.append({"tgl": path, "error": str(e)})
                    thumbname = Path(path).stem+".png"
                    thumb.save(out/"thumbs"/thumbname)
                    rendered[path] = thumb
                x, y = (tile%6)*224, (tile//6)*220+38
                page.paste(rendered[path], (x, y))
                draw.text((x+7, y+162), name[:28], font=font(12), fill="white")
                draw.text((x+7, y+179), f"{model['role']} L{model['lod']} | {model.get('triangles', '?')} tris", font=font(12), fill=(162, 195, 219))
                b = model.get("bounds", {})
                if b:
                    draw.text((x+7, y+196), "x/y/z " + "/".join(f"{v:.1f}" for v in b["size"]), font=font(11), fill=(159, 168, 179))
            filename = f"{category}-{offset//30+1:02d}.jpg"
            page.save(out/filename, quality=88)
            index["pages"].append({"file": filename, "category": category,
                                    "models": [m["tgl"] for _, m in items[offset:offset+30]]})
            links.append(f"- [{category} {offset//30+1}]({filename})")
        print(f"Sheets: {category}, {len(items)} tiles", flush=True)
    index["unique_thumbnails"] = len(rendered)
    index["present_tgl_files"] = sum(install.has(path) for path in rendered)
    index["decoded_thumbnails"] = len(rendered)-len(failures)
    expected_thumbs = {Path(path).stem+".png" for path in rendered}
    # Only remove obsolete generated thumbnails inside the resolved output root.
    for path in (out/"thumbs").glob("*.png"):
        if path.name not in expected_thumbs:
            if not path.resolve().is_relative_to(out):
                raise ValueError(f"thumbnail escapes sheet output: {path}")
            path.unlink()
    write_json(out/"index.json", index)
    (out/"index.md").write_text("\n".join(links)+"\n", encoding="utf-8")
