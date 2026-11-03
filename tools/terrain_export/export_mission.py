#!/usr/bin/env python3
"""TERRAIN-REGEN-1: export a stock mission's terrain "as it appears in game" to glTF (GLB) for Blender.

Reads the SHIPPED mission .pak/.fit (tools/mc2data/stock_missions verifies bytes), the shipped
baked colormap (<mission>.burnin.jpg, the engine's own base-colour bake) and the mission beauty
sidecar visual height when present.  Writes (to --out):

  <m>_plain.glb      terrain mesh at full tile resolution (1 vertex per 128 wu), baked colormap
                     as base colour, smooth vertex normals only ("normal maps OFF")
  <m>_normalmap.glb  same + tangent-space normal map baked from the visual height
                     ("normal maps ON"; see NOTE)
  Both GLBs also carry: a water plane at [Water] Elevation and one proxy box per terrain object
  (name obj_<type>, extras.type), as Blender-importable nodes.
  <m>_height.r32     raw float32 heights (side*side, row 0 = north), <m>_export.json metadata.

NOTE normal maps: the engine's detail normals (mat<N>_normal tiles) are shader-tiled per class and
are not a single bakeable image.  The "ON" variant bakes the per-mission visual height field (the
beauty sidecar visual_height_4x.r32 when found, else a bicubic upsample of the sim heights) into one
tangent-space map.  Sim heights are never modified.

Coordinates: world X east, world Y north (row 0 = north edge), Z = elevation.  glTF is Y-up, so a
node position is (X, Z, -Y) * --scale (default 0.1: 1 Blender unit = 10 wu; the importer turns
that back into Z-up).  Usage:
  py -3 tools/terrain_export/export_mission.py mc2_01 --out E:/.../terrain-regen-mc01/export
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "terrain_beautify"))
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))
import mission_terrain_analyzer as A  # noqa: E402
from stock_missions import StockDataError, resolve_missions_dir  # noqa: E402

WU = A.WORLD_UNITS_PER_VERTEX
Image.MAX_IMAGE_PIXELS = None


def load_mission(mission: str, missions_dir: Path):
    """-> dict(side, elev[side,side] float, texdata u32, water_elev, objects[(type,x,y,z,rot)])."""
    packets = A.read_packets(missions_dir / f"{mission}.pak")
    md = A.locate_mapdata(packets)
    if md is None:
        raise SystemExit("no MapData packet")
    _, side, blocks = md
    water_elev = A.read_water_elevation(missions_dir / f"{mission}.fit")
    lay = A.extract_layers(side, blocks, water_elev)
    n = side * side
    arr = np.frombuffer(blocks, dtype=np.uint8, count=n * A.PCV_SIZE).reshape(n, A.PCV_SIZE)
    texdata = arr[:, A.OFF_TEXDATA:A.OFF_TEXDATA + 4].copy().view(np.uint32).reshape(side, side)
    objs = []
    for _, t, payload in packets:
        dec = A.decode_packet(t, payload)
        if dec is None or len(dec) < 4:
            continue
        cnt = struct.unpack_from("<i", dec, 0)[0]
        if cnt > 0 and 4 + cnt * A.OBJREC_SIZE == len(dec):
            for i in range(cnt):
                ot, x, y, z, rot = struct.unpack_from("<i4f", dec, 4 + i * A.OBJREC_SIZE)
                objs.append((ot, x, y, z, rot))
            break
    return {"side": side, "elev": lay["elev"], "texdata": texdata,
            "water_elev": water_elev, "objects": objs}


def install_dir(arg: str | None) -> Path:
    """Game install that holds data/textures and data/missions/<m>.beauty: --install, else $MC2_INSTALL."""
    import os
    v = arg or os.environ.get("MC2_INSTALL")
    if not v:
        raise SystemExit("no install: pass --install DIR or set MC2_INSTALL (any install with data/textures)")
    return Path(v)


def find_colormap(mission: str, install: Path) -> Path | None:
    for ext in ("jpg", "tga"):
        p = install / "data" / "textures" / f"{mission}.burnin.{ext}"
        if p.is_file():
            return p
    return None


def visual_height(mission: str, elev: np.ndarray, size: int, install: Path) -> np.ndarray:
    """size x size float height for normal baking (beauty visual height if present)."""
    from scipy.ndimage import zoom
    side = elev.shape[0]
    p = install / "data" / "missions" / f"{mission}.beauty" / "visual_height_4x.r32"
    h = None
    if p.is_file():
        v = (side - 1) * 4 + 1
        raw = np.fromfile(p, dtype="<f4")
        if raw.size == v * v:
            h = raw.reshape(v, v).astype(np.float64)
    if h is None:
        h = elev
    return zoom(h, size / h.shape[0], order=3, mode="nearest")


def bake_normal_map(h: np.ndarray, extent_wu: float) -> Image.Image:
    """Tangent-space (OpenGL, +Y = image up = north) normal map from height field h (row 0 = north)."""
    pix = extent_wu / h.shape[0]
    dh_dc = np.gradient(h, axis=1) / pix
    dh_dr = np.gradient(h, axis=0) / pix
    nx, ny, nz = -dh_dc, dh_dr, np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.stack([nx * inv, ny * inv, nz * inv], -1) * 0.5 + 0.5
    return Image.fromarray((rgb * 255 + 0.5).astype(np.uint8), "RGB")


def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def material_tints(install: Path | None) -> dict:
    """rock/grass/dirt tints from data/terrain_materials.json (install, else repo copy)."""
    cands = ([install / "data" / "terrain_materials.json"] if install else []) + \
        [Path(__file__).resolve().parents[2] / "data" / "terrain_materials.json"]
    for p in cands:
        if p.is_file():
            j = json.loads(p.read_text())
            return {k: np.array([j[f"{k}_tint_{c}"] for c in "rgb"]) for k in ("rock", "grass", "dirt")}
    return {"rock": np.array([.36, .37, .40]), "grass": np.array([.35, .42, .25]), "dirt": np.array([.48, .42, .33])}


def as_rendered_colormap(cmap: Image.Image, elev: np.ndarray, tints: dict) -> Image.Image:
    """APPROXIMATE the Vulkan terrain_tier1.frag colour stage on the baked colormap:
    material tint blend (strength mix(.18,.50,smoothstep(.1,.6,luma))), cliff desaturate/darken on
    macro slope (cliffBlend=smoothstep(.85,.55,|nz|), blend*0.7), mean break-up (x0.98).
    NOT modelled: per-texel cooked material weights (grass/dirt/rock chosen by a colour heuristic here),
    snow/cement, detail normals, POM/triplanar relief, shore SDF / smoothed coastline, lighting."""
    from scipy.ndimage import zoom
    rgb = np.asarray(cmap, dtype=np.float64) / 255.0
    n = rgb.shape[0]
    h = zoom(elev, n / elev.shape[0], order=1, mode="nearest")
    gy, gx = np.gradient(h, WU * elev.shape[0] / n)
    nz = 1.0 / np.sqrt(1.0 + gx * gx + gy * gy)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    luma = rgb @ np.array([.299, .587, .114])
    wg = _smoothstep(-0.02, 0.06, g - r) * _smoothstep(0.22, 0.40, luma)
    wd = _smoothstep(-0.02, 0.06, r - g) * _smoothstep(0.22, 0.45, luma) * (1 - wg)
    wr = np.clip(1 - wg - wd, 0, 1)
    mt = wr[..., None] * tints["rock"] + wg[..., None] * tints["grass"] + wd[..., None] * tints["dirt"]
    ts = (0.18 + (0.50 - 0.18) * _smoothstep(0.1, 0.6, luma))[..., None]
    out = rgb * (1 - ts) + mt * ts
    cb = _smoothstep(0.85, 0.55, nz)[..., None]
    l2 = (out @ np.array([.299, .587, .114]))[..., None]
    cliff = (l2 * 0.4 + tints["rock"] * 0.6) * 0.8
    out = out * (1 - cb * 0.7) + cliff * (cb * 0.7)
    out *= 0.98
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


# --- minimal GLB writer ------------------------------------------------------
class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.d = {"asset": {"version": "2.0", "generator": "mc2 terrain_export"},
                  "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                  "materials": [], "accessors": [], "bufferViews": [], "images": [],
                  "textures": [], "samplers": [{"magFilter": 9729, "minFilter": 9987,
                                                "wrapS": 33071, "wrapT": 33071}]}

    def _view(self, data: bytes, target=None) -> int:
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin += data
        self.d["bufferViews"].append(v)
        return len(self.d["bufferViews"]) - 1

    def accessor(self, arr: np.ndarray, ctype: str, target=None, minmax=False) -> int:
        comp = {"f4": 5126, "u4": 5125}[arr.dtype.str[1:]]
        ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[ctype]
        a = {"bufferView": self._view(arr.tobytes(), target), "componentType": comp,
             "count": int(arr.shape[0]), "type": ctype}
        if minmax:
            a["min"] = arr.reshape(-1, ncomp).min(0).tolist()
            a["max"] = arr.reshape(-1, ncomp).max(0).tolist()
        self.d["accessors"].append(a)
        return len(self.d["accessors"]) - 1

    def image(self, im: Image.Image, fmt: str) -> int:
        import io
        b = io.BytesIO()
        im.save(b, fmt, **({"quality": 92} if fmt == "JPEG" else {}))
        self.d["images"].append({"bufferView": self._view(b.getvalue()),
                                 "mimeType": "image/jpeg" if fmt == "JPEG" else "image/png"})
        self.d["textures"].append({"sampler": 0, "source": len(self.d["images"]) - 1})
        return len(self.d["textures"]) - 1

    def mesh(self, name, pos, idx, nrm=None, uv=None, material=None) -> int:
        at = {"POSITION": self.accessor(pos, "VEC3", 34962, True)}
        if nrm is not None:
            at["NORMAL"] = self.accessor(nrm, "VEC3", 34962)
        if uv is not None:
            at["TEXCOORD_0"] = self.accessor(uv, "VEC2", 34962)
        prim = {"attributes": at, "indices": self.accessor(idx, "SCALAR", 34963)}
        if material is not None:
            prim["material"] = material
        self.d["meshes"].append({"name": name, "primitives": [prim]})
        return len(self.d["meshes"]) - 1

    def node(self, name, mesh=None, t=None, s=None, extras=None) -> int:
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if t is not None:
            n["translation"] = [float(x) for x in t]
        if s is not None:
            n["scale"] = [float(x) for x in s]
        if extras:
            n["extras"] = extras
        self.d["nodes"].append(n)
        i = len(self.d["nodes"]) - 1
        self.d["scenes"][0]["nodes"].append(i)
        return i

    def write(self, path: Path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.d["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.d, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin))


def terrain_arrays(elev: np.ndarray, scale: float):
    side = elev.shape[0]
    r, c = np.mgrid[0:side, 0:side]
    wx = -side * WU / 2 + c * WU
    wy = side * WU / 2 - r * WU
    pos = np.stack([wx, elev, -wy], -1).reshape(-1, 3).astype("<f4") * scale
    gy, gx = np.gradient(elev, WU)  # d/drow, d/dcol (per wu)
    nrm = np.stack([-gx, np.ones_like(elev), gy * 1.0], -1)  # y-up; +z = south
    # d(elev)/d(north) = -gy; glTF z = -north  => nz = -d/dz = ... (-d elev/dz) with z=south => -gy*-1 = gy
    nrm = nrm / np.linalg.norm(nrm, axis=-1, keepdims=True)
    nrm = nrm.reshape(-1, 3).astype("<f4")
    ext = side * WU  # colormap covers side*128 wu from the min corner
    uv = np.stack([(wx + ext / 2) / ext, (ext / 2 - wy) / ext], -1)  # glTF v=0 is image top (north)
    uv = uv.reshape(-1, 2).astype("<f4")
    ii = np.arange(side * side).reshape(side, side)
    a, b, cc, d = ii[:-1, :-1], ii[:-1, 1:], ii[1:, :-1], ii[1:, 1:]
    # The Vulkan renderer's fixed TL-BR diagonal (vulkan_terrain_island_island.cpp:56-66): {tl,bl,br},{tl,br,tr}.
    # a=tl b=tr cc=bl d=br.  (The sim's getTerrainElevation uses a per-quad checkerboard, mapdata.cpp:1185.)
    idx = np.stack([a, cc, d, a, d, b], -1).reshape(-1).astype("<u4")  # CCW seen from +Y
    return pos, nrm, uv, idx


VARIANTS = {
    "baked": "burnin colormap only, flat geometry",
    "as_rendered": "colormap with approximated tint + cliff darkening (see as_rendered_colormap)",
    "hires_nmap": "baked colormap + reauth hi-res normal map; the game does NOT draw this at LOD0 up close today",
}


def build(m: dict, mission: str, out: Path, scale: float, variant: str,
          cmap: Image.Image | None, nmap: Image.Image | None, elev_water: float):
    g = Glb()
    side, elev = m["side"], m["elev"]
    ext = side * WU
    mat_t = {"name": "terrain_colormap", "doubleSided": True,
             "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.95}}
    if cmap is not None:
        mat_t["pbrMetallicRoughness"]["baseColorTexture"] = {"index": g.image(cmap, "JPEG")}
    mat_t["name"] = f"terrain_{variant}"
    if variant == "hires_nmap" and nmap is not None:
        mat_t["normalTexture"] = {"index": g.image(nmap, "PNG"), "scale": 1.0}
    g.d["materials"].append(mat_t)
    pos, nrm, uv, idx = terrain_arrays(elev, scale)
    g.node("terrain", g.mesh("terrain", pos, idx, nrm, uv, 0))
    # water plane at [Water] Elevation
    g.d["materials"].append({"name": "water", "alphaMode": "BLEND", "doubleSided": True,
                             "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.22, 0.30, 0.65],
                                                      "metallicFactor": 0.0, "roughnessFactor": 0.05}})
    h = ext / 2 * scale
    wp = np.array([[-h, 0, h], [h, 0, h], [h, 0, -h], [-h, 0, -h]], "<f4")
    wn = np.tile(np.array([0, 1, 0], "<f4"), (4, 1))
    g.node("water", g.mesh("water", wp, np.array([0, 1, 2, 0, 2, 3], "<u4"), wn, None, 1),
           t=(0, elev_water * scale, 0))
    # props as proxy boxes
    bx = np.array([[x, y, z] for x in (-.5, .5) for y in (0, 1) for z in (-.5, .5)], "<f4")
    bi = np.array([0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6,
                   0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3], "<u4")
    g.d["materials"].append({"name": "prop_proxy", "pbrMetallicRoughness":
                             {"baseColorFactor": [0.85, 0.2, 0.1, 1], "metallicFactor": 0, "roughnessFactor": 0.8}})
    bmesh = g.mesh("proxy_box", bx * 1.0, bi, None, None, 2)
    sz = 64.0 * scale
    for ot, x, y, z, rot in m["objects"]:
        g.node(f"obj_{ot}", bmesh, t=(x * scale, z * scale, -y * scale), s=(sz, sz, sz),
               extras={"type": ot, "rot_deg": rot})
    p = out / f"{mission}_{variant}.glb"
    g.write(p)
    return p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mission")
    ap.add_argument("--out", required=True)
    ap.add_argument("--missions-dir")
    ap.add_argument("--scale", type=float, default=0.1)
    ap.add_argument("--normal-size", type=int, default=2048)
    ap.add_argument("--color-size", type=int, default=4096, help="downscale colormap (0 = native)")
    ap.add_argument("--install", help="game install dir (data/textures, data/missions/*.beauty); else $MC2_INSTALL")
    ap.add_argument("--allow-nonstock", action="store_true")
    a = ap.parse_args()
    try:
        res = resolve_missions_dir(a.missions_dir, [a.mission], allow_nonstock=a.allow_nonstock)
    except StockDataError as e:
        print(e)
        return 2
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    m = load_mission(a.mission, res.dir)
    side, elev = m["side"], m["elev"]
    inst = install_dir(a.install)
    cm = find_colormap(a.mission, inst)
    cmap = None
    if cm:
        cmap = Image.open(cm).convert("RGB")
        if a.color_size and cmap.width > a.color_size:
            cmap = cmap.resize((a.color_size, a.color_size), Image.LANCZOS)
    nmap = bake_normal_map(visual_height(a.mission, elev, a.normal_size, inst), side * WU)
    nmap.save(out / f"{a.mission}_normal.png")
    elev.astype("<f4").tofile(out / f"{a.mission}_height.r32")
    paths = []
    for v in VARIANTS:
        cv = cmap
        if v == "as_rendered" and cmap is not None:
            cv = as_rendered_colormap(cmap, elev, material_tints(inst))
        paths.append(build(m, a.mission, out, a.scale, v, cv, nmap, m["water_elev"]))
    meta = {"mission": a.mission, "side": side, "wu_per_vertex": WU, "scale": a.scale,
            "extent_wu": side * WU, "min_corner_wu": [-side * WU / 2, side * WU / 2],
            "water_elevation": m["water_elev"], "elev_min": float(elev.min()), "elev_max": float(elev.max()),
            "objects": len(m["objects"]), "colormap": str(cm), "diagonal": "TL-BR (render)", "variants": VARIANTS, "stock_verified": res.verified,
            "files": [str(p) for p in paths]}
    (out / f"{a.mission}_export.json").write_text(json.dumps(meta, indent=2))
    print(json.dumps(meta, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
