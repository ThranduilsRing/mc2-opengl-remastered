"""
fence_gen.py - headless Blender recipe: procedural chain-link fence segment.

Replaces the stock 4-triangle wirefence quad pair with a real fence (posts,
top rail, tension wire, double-sided chain-link sheet) built in the STOCK
vertex frame, plus a tileable alpha diamond-lattice TGA.

Frame (GLB frame as written by tools/ase_to_glb.py for the stock asset; the
engine importer maps glb (x,y,z) -> engine (-x,-y,z), i.e. back to the ASE
frame, so stock-derived GLBs need NO extra flip):
  x = thickness, y = run (Y0..Y1), z = height (0..H). Built here with
  export_yup=False so Blender coords == glTF coords.
Stock numbers (wirefence): y -42.9175..0.4255, z 0..15.9699, x -0.1576..0.2036.
Stock numbers (wirefencediag): y -60.6417..0.3017, z -0.0243..15.9757.

Args (after `--`):
  --out=PATH         output GLB
  --tex-dir=DIR      output dir for the TGAs
  --variant=NAME     wirefence | wirefencediag
  --lattice=NAME     lattice texture stem (default a_wirefence_proc)
  --metal=NAME       metal texture stem   (default fence_metal_proc)
  --tex-res=N        lattice TGA size (default 512)
  --in=PATH          stock GLB (unused; accepted for driver symmetry)
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _recipe_util import arg, emit_stats, script_args  # noqa: E402

VARIANTS = {
    # y0, y1, z0, z1, x_center, sheet half-gap, mid posts
    "wirefence":     dict(y0=-42.9175, y1=0.4255, z0=0.0,     z1=15.9699, cx=0.0230, gap=0.12, mids=1),
    "wirefencediag": dict(y0=-60.6417, y1=0.3017, z0=-0.0243, z1=15.9757, cx=0.0828, gap=0.05, mids=2),
}
TILE_U = 5.418   # world units per lattice tile along the run
TILE_V = 5.32    # world units per lattice tile in height


def hex_prism(cx, cy, r, z0, z1, n=6):
    """Side quads of an n-gon prism about a vertical axis, outward CCW."""
    out = []
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = 2 * math.pi * (i + 1) / n
        p0 = (cx + r * math.cos(a0), cy + r * math.sin(a0))
        p1 = (cx + r * math.cos(a1), cy + r * math.sin(a1))
        out.append([(p0[0], p0[1], z0), (p1[0], p1[1], z0), (p1[0], p1[1], z1), (p0[0], p0[1], z1)])
    return out


def cone_cap(cx, cy, r, z0, z1, n=6):
    out = []
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = 2 * math.pi * (i + 1) / n
        out.append([(cx + r * math.cos(a0), cy + r * math.sin(a0), z0),
                    (cx + r * math.cos(a1), cy + r * math.sin(a1), z0),
                    (cx, cy, z1)])
    return out


def run_tube(cx, z, y0, y1, r, n):
    """Tube along y (rail), sides only (ends buried in posts), outward CCW."""
    out = []
    for i in range(n):
        a0 = 2 * math.pi * i / n + math.pi / n
        a1 = 2 * math.pi * (i + 1) / n + math.pi / n
        p0 = (cx + r * math.cos(a0), z + r * math.sin(a0))
        p1 = (cx + r * math.cos(a1), z + r * math.sin(a1))
        out.append([(p0[0], y0, p0[1]), (p1[0], y0, p1[1]), (p1[0], y1, p1[1]), (p0[0], y1, p0[1])])
    return out


def build(variant):
    v = VARIANTS[variant]
    y0, y1, z0, z1, cx, gap = v["y0"], v["y1"], v["z0"], v["z1"], v["cx"], v["gap"]
    L = y1 - y0
    H = z1 - z0
    metal = []
    px = [y0, y1] + [y0 + L * (k + 1) / (v["mids"] + 1) for k in range(v["mids"])]
    for i, py in enumerate(px):
        r = 0.55 if i < 2 else 0.42
        metal += hex_prism(cx, py, r, z0, z1 - 0.35)
        metal += cone_cap(cx, py, r * 1.15, z1 - 0.35, z1)
    metal += run_tube(cx, z1 - 0.9, y0, y1, 0.28, 6)
    metal += run_tube(cx, z0 + 0.45, y0, y1, 0.14, 4)
    sz0, sz1 = z0 + 0.7, z1 - 1.2
    sheet = []
    for sgn in (+1, -1):
        x = cx + sgn * gap
        q = [(x, y0, sz0), (x, y1, sz0), (x, y1, sz1), (x, y0, sz1)]
        if sgn < 0:
            q = q[::-1]
        sheet.append(q)

    def sheet_uv(p):
        return ((p[1] - y0) / TILE_U, (p[2] - sz0) / TILE_V)

    def metal_uv(p):
        return (0.5, (p[2] - z0) / H)

    return sheet, sheet_uv, metal, metal_uv


def tri_count(polys):
    return sum(1 if len(p) == 3 else 2 for p in polys)


def write_tga_rgba(path, w, h, rgba):
    """Uncompressed 32-bit TGA, bottom-left origin. rgba: uint8 (h,w,4), row 0 = TOP."""
    import numpy as np
    bgra = rgba[:, :, [2, 1, 0, 3]][::-1]
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(np.ascontiguousarray(bgra).tobytes())


def lattice_rgba(res, ndiam=5, wire_px=None, ss=3):
    """Tileable diamond lattice, alpha = wire coverage. Wire width ~ res/34 so
    it survives without mipmaps."""
    import numpy as np
    wire_px = wire_px or res / 34.0
    n = res * ss
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    s = n / ndiam
    d1 = ((xs + ys) / s) % 1.0
    d2 = ((xs - ys) / s) % 1.0
    f1 = np.minimum(d1, 1 - d1) * s / math.sqrt(2)
    f2 = np.minimum(d2, 1 - d2) * s / math.sqrt(2)
    hw = wire_px * ss / 2.0
    c1 = f1 < hw
    c2 = f2 < hw
    cov = c1 | c2

    def shade(f):
        t = np.clip(f / hw, 0, 1)
        return 1.0 - 0.45 * t ** 2

    sh = np.where(c1 & ~c2, shade(f1),
                  np.where(c2 & ~c1, shade(f2), np.maximum(shade(f1), shade(f2)) * 0.82))
    base = np.array([196.0, 202.0, 208.0])
    rgb = base[None, None, :] * sh[:, :, None]
    a = cov.astype(np.float64)

    def down(arr):
        return arr.reshape(res, ss, res, ss, *arr.shape[2:]).mean(axis=(1, 3))

    A = down(a)
    RGB = down(rgb * a[:, :, None])
    RGBn = np.where(A[:, :, None] > 1e-4, RGB / np.maximum(A[:, :, None], 1e-4),
                    base[None, None, :] * 0.9)
    out = np.zeros((res, res, 4), np.uint8)
    out[:, :, :3] = np.clip(RGBn, 0, 255).astype(np.uint8)
    out[:, :, 3] = np.clip(A * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return out


def metal_rgba(res=64):
    import numpy as np
    rng = np.random.default_rng(7)
    col = np.repeat(rng.normal(0, 1, (1, res)), res, axis=0)
    y = np.linspace(0, 1, res)[:, None]
    base = 150 + 14 * col + 18 * np.sin(y * math.pi) + rng.normal(0, 3, (res, res))
    out = np.zeros((res, res, 4), np.uint8)
    out[:, :, 0] = np.clip(base * 0.97, 0, 255)
    out[:, :, 1] = np.clip(base, 0, 255)
    out[:, :, 2] = np.clip(base * 1.04, 0, 255)
    out[:, :, 3] = 255
    return out


def patch_glb_textures(path, mat_to_uri):
    """Attach external image URIs (stock-converter style) so the engine
    importer derives the texture name from the URI stem. Materials by name."""
    data = open(path, "rb").read()
    jl = struct.unpack("<I", data[12:16])[0]
    gltf = json.loads(data[20:20 + jl])
    rest = data[20 + jl:]
    gltf["images"] = []
    gltf["textures"] = []
    gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}]
    for m in gltf["materials"]:
        uri = mat_to_uri[m["name"].split(".")[0]]
        gltf["images"].append({"uri": uri})
        gltf["textures"].append({"source": len(gltf["images"]) - 1, "sampler": 0})
        pbr = m.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"] = {"index": len(gltf["textures"]) - 1}
        pbr["metallicFactor"] = 0.0
        pbr["roughnessFactor"] = 0.8
        if m["name"].startswith("fence_lattice"):
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
            m["doubleSided"] = True
    for k in ("extensionsUsed", "extensionsRequired", "extensions"):
        gltf.pop(k, None)
    body = json.dumps(gltf, separators=(",", ":")).encode()
    body += b" " * ((4 - len(body) % 4) % 4)
    total = 12 + 8 + len(body) + len(rest)
    open(path, "wb").write(data[:8] + struct.pack("<I", total) + struct.pack("<I", len(body))
                           + b"JSON" + body + rest)


def main():
    import bpy
    args = script_args()
    out = arg(args, "out", required=True)
    tex_dir = arg(args, "tex-dir", required=True)
    variant = arg(args, "variant", "wirefence")
    lat_name = arg(args, "lattice", "a_wirefence_proc")
    met_name = arg(args, "metal", "fence_metal_proc")
    res = arg(args, "tex-res", 512, int)
    if variant not in VARIANTS:
        print(f"[recipe:error] unknown variant {variant}", flush=True)
        raise SystemExit(2)

    sheet, sheet_uv, metal, metal_uv = build(variant)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    me = bpy.data.meshes.new(variant)
    verts, faces, uvs, mats = [], [], [], []
    for polys, uvf, mi in ((sheet, sheet_uv, 0), (metal, metal_uv, 1)):
        for p in polys:
            base = len(verts)
            verts += p
            faces.append(tuple(range(base, base + len(p))))
            uvs += [uvf(q) for q in p]
            mats.append(mi)
    me.from_pydata(verts, [], faces)
    for name in ("fence_lattice", "fence_metal"):
        me.materials.append(bpy.data.materials.new(name))
    for poly, mi in zip(me.polygons, mats):
        poly.material_index = mi
        poly.use_smooth = False
    uvl = me.uv_layers.new(name="UVMap")
    for li in range(len(me.loops)):
        uvl.data[li].uv = uvs[li]
    me.update()
    ob = bpy.data.objects.new(variant, me)
    bpy.context.scene.collection.objects.link(ob)

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_yup=False,
                              export_image_format="NONE", export_apply=True)
    patch_glb_textures(out, {"fence_lattice": lat_name + ".tga", "fence_metal": met_name + ".tga"})

    os.makedirs(tex_dir, exist_ok=True)
    write_tga_rgba(os.path.join(tex_dir, lat_name + ".tga"), res, res, lattice_rgba(res))
    write_tga_rgba(os.path.join(tex_dir, lat_name + "_256.tga"), 256, 256, lattice_rgba(256))
    write_tga_rgba(os.path.join(tex_dir, met_name + ".tga"), 64, 64, metal_rgba(64))

    xs = [q[0] for q in verts]
    ys = [q[1] for q in verts]
    zs = [q[2] for q in verts]
    emit_stats("fence_gen", {"variant": variant, "tris": tri_count(sheet) + tri_count(metal),
                             "sheet_tris": tri_count(sheet), "metal_tris": tri_count(metal),
                             "bounds": [[min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)]]})


if __name__ == "__main__":
    main()
