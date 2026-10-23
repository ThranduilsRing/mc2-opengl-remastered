"""Read model geometry and type radii into a portable, install-specific catalog."""
from __future__ import annotations

import argparse
from collections import Counter
from functools import lru_cache
import hashlib
import io
from pathlib import Path
import re
import struct
import sys

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import CELL_WU, INSTALL_ROOT, building_role, load_json, spans, write_json
from tools.mission_codec import Install, catalog, fit_sections, packet
from tools.mapgen.extract import DEFAULT_OUT, INSTALLS

TABLE_PATH = Path(__file__).with_name("footprints.json")


def hull(points):
    """Monotone convex hull in local world X,Y, retaining an asymmetric pivot."""
    points = sorted(set(tuple(round(float(v), 3) for v in p) for p in points))
    def cross(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    chains = []
    for seq in (points, points[::-1]):
        chain = []
        for p in seq:
            while len(chain) >= 2 and cross(chain[-2], chain[-1], p) <= 0:
                chain.pop()
            chain.append(p)
        chains.append(chain[:-1])
    return [list(p) for p in chains[0]+chains[1]]


def tgl_points(raw):
    """32-bit disk structs from tgl_types.cpp, bounds rules from msl_load.cpp.

    TGL is X,Y-up,Z-depth. renderToMc2 maps local -X,Z to world X,Y.
    Relative node centres, already stored in the file, match LoadBinaryCopy.
    """
    offset = 0
    def take(size):
        nonlocal offset
        if size < 0 or offset+size > len(raw):
            raise ValueError("truncated TGL")
        value = raw[offset:offset+size]
        offset += size
        return value
    def integer():
        value = struct.unpack("<I", take(4))[0]
        if value > 1_000_000:
            raise ValueError("unsupported TGL count")
        return value
    if struct.unpack("<I", take(4))[0] != 0xBAFDECAF:
        raise ValueError("unsupported TGL version")
    nodes, textures = integer(), integer()
    take(textures*268)
    result = []
    for _ in range(nodes):
        kind = integer()
        if kind not in (0, 1):
            raise ValueError(f"unsupported TGL node {kind}")
        verts = np.empty((0, 3))
        if kind == 1:
            n = integer()
            verts = np.frombuffer(take(n*28), dtype=np.dtype(
                [("p", "<f4", (3,)), ("rest", "u1", (16,))]))["p"].astype(float)
            take(integer()*56)  # TG_TypeTriangle: indices, flags, UVs, normal
            take(integer()*12)  # TG_TinyTexture
        centres = struct.unpack("<6f", take(24))
        name = take(25).split(b"\0")[0].lower()
        take(25)
        if len(verts) and not name.startswith((b"_pab", b"los_", b"spotlight")):
            verts += np.asarray(centres[3:])
            result.extend((verts[:, [0, 2, 1]]*[-1,1,1]).tolist())
    if offset != len(raw) or not result or not np.isfinite(result).all():
        raise ValueError("empty, nonfinite or unsupported TGL layout")
    return np.asarray(result)


def ase_points(raw):
    # ASE MESH_VERTEX positions are absolute, before engine recentering.
    # Parse per GEOMOBJECT so helper/LOS/spotlight vertices stay excluded.
    result = []
    for block in re.split(r"\*GEOMOBJECT\s*\{", raw.decode("latin1"))[1:]:
        name = re.search(r'\*NODE_NAME\s+"([^"]+)"', block)
        if name and name[1].lower().startswith(("_pab", "los_", "spotlight")):
            continue
        for match in re.finditer(r"\*MESH_VERTEX\s+\d+\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)", block):
            x, y, z = map(float, match.groups())
            result.append([x, y, z])
    if not result or not np.isfinite(result).all():
        raise ValueError("empty/nonfinite ASE")
    return np.asarray(result)


def measure(points):
    low, high = points.min(axis=0), points.max(axis=0)
    dims = high-low
    return {"width_wu": round(float(dims[0]), 3), "depth_wu": round(float(dims[1]), 3),
            "height_wu": round(float(dims[2]), 3), "bounds_wu": np.round([low, high], 3).tolist(),
            "outline_wu": hull(points[:, :2])}


def radius_footprint(radius):
    radius = float(radius or 0)
    # Zero radius is common for decorative types: explicitly mark unknown,
    # rather than misrepresent an invented dimension as a measured footprint.
    return {"width_wu": radius*2, "depth_wu": radius*2, "height_wu": radius*2,
            "bounds_wu": [[-radius, -radius, 0], [radius, radius, radius*2]],
            "outline_wu": [[-radius,-radius], [radius,-radius], [radius,radius], [-radius,radius]],
            "method": "extentRadius" if radius > 0 else "unresolved", "extent_radius_wu": radius}


def surface_catalog(install):
    """Preserve authored tac colours and actual overlay alpha silhouettes."""
    from PIL import Image
    raw, source = install.read("data/textures/textures.fit")
    sections = fit_sections(raw)
    terrain = {}
    overlays = {}
    for name, values in sections.items():
        if name.startswith("terraintype") and name[11:].isdigit():
            colour = int(values.get("terraintacrgb", 0x586747)) & 0xFFFFFF
            terrain[name[11:]] = {"name": values.get("terrainname"), "rgb": [colour>>16, (colour>>8)&255, colour&255]}
        if not name.startswith("overlaytype"):
            continue
        overlay = str(values.get("overlayname", ""))
        for tile in range(int(values.get("numoverlays", 0))):
            filename = f"data/textures/64overlays/{overlay}{tile:04}.tga"
            try:
                data, origin = install.read(filename)
                im = Image.open(io.BytesIO(data)).convert("RGBA")
                alpha = np.asarray(im)[:, :, 3] >= 128
                overlays[f"{name[11:]}:{tile}"] = {"shape": list(alpha.shape), "spans": spans(alpha),
                    "source": origin, "sha256": hashlib.sha256(data).hexdigest()}
            except (FileNotFoundError, OSError, ValueError):
                pass
    return {"source": source, "terrain": terrain, "overlays": overlays}


def build(corpus=DEFAULT_OUT, root=INSTALL_ROOT):
    index = load_json(corpus / "index.json")
    stock = Install(root / INSTALLS["stock"])
    omni = Install(root / INSTALLS["MechCommanderOmnitech"], fallback=stock)
    result = {"schema": "mc2-footprints-1", "units": "world units", "types": {}, "surfaces": {}}
    for label in INSTALLS:
        install = stock if label == "stock" else omni if label == "MechCommanderOmnitech" else Install(
            root / INSTALLS[label], fallback=omni if label == "MCO-Carver5" else stock)
        used = {}
        for row in index["missions"]:
            if row["corpus"] == label:
                for obj in load_json(corpus / row["file"])["objects"]:
                    used[obj["type_id"]] = obj
        types, _ = catalog(install)
        raw, source = install.read("data/objects/object2.pak")
        packet_count = struct.unpack_from("<I", raw, 4)[0]//4-2
        cache = {}
        for number, obj in sorted(used.items()):
            info = types.get(number, obj)
            sections = fit_sections(packet(raw, number)) if number < packet_count else {}
            fit_source = f"{source}::packet:{number}"
            if number >= packet_count and install.has(f"data/objects/{info['type_name']}.fit"):
                data, fit_source = install.read(f"data/objects/{info['type_name']}.fit")
                sections = fit_sections(data)
            radius = max((float(v.get("extentradius", 0) or 0) for v in sections.values()), default=0)
            entry = {**{k: info.get(k) for k in ("class", "appearance", "type_name", "object_class")},
                     "type_id": number, "install": label, "role": building_role(info), "type_source": fit_source}
            appearance = info.get("appearance", "")
            if appearance not in cache:
                footprint = radius_footprint(radius)
                errors = []
                shape = appearance
                try:
                    ini, ini_source = install.read(f"data/tgl/{appearance}.ini")
                    fields = fit_sections(ini).get("tgldata", {})
                    shape = fields.get("filename", fields.get("filename0", appearance))
                    entry["appearance_source"] = ini_source
                except FileNotFoundError:
                    pass
                shape = str(shape).replace("\\", "/").rsplit("/", 1)[-1]
                shape = re.sub(r"\.(ase|tgl)$", "", shape, flags=re.I)
                for extension, parser in (("tgl", tgl_points), ("ase", ase_points)):
                    try:
                        data, origin = install.read(f"data/tgl/{shape}.{extension}")
                        footprint = {**measure(parser(data)), "method": extension, "shape_source": origin,
                                     "shape_sha256": hashlib.sha256(data).hexdigest()}
                        break
                    except (FileNotFoundError, ValueError, struct.error) as error:
                        errors.append(str(error))
                if footprint["method"] not in ("tgl", "ase"):
                    footprint["reason"] = errors
                cache[appearance] = footprint
            footprint = cache[appearance]
            if footprint["method"] not in ("tgl", "ase"):
                footprint = {**footprint, **radius_footprint(radius)}
            result["types"][f"{label}:{number}"] = {**entry, **footprint}
        result["surfaces"][label] = surface_catalog(install)
        print(label, Counter(v["method"] for v in result["types"].values() if v["install"] == label), flush=True)
    result["coverage"] = dict(Counter(v["method"] for v in result["types"].values()))
    return result


@lru_cache(maxsize=1)
def table():
    return load_json(TABLE_PATH)


def lookup(layout, obj, catalog=None):
    if obj.get("footprint"):
        return obj["footprint"]
    catalog = catalog or table()
    key = f"{layout['source'].get('asset_install', layout['source']['corpus'])}:{obj['type_id']}"
    return catalog["types"].get(key, radius_footprint(0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--root", type=Path, default=INSTALL_ROOT)
    parser.add_argument("--out", type=Path, default=TABLE_PATH)
    args = parser.parse_args()
    result = build(args.corpus, args.root)
    write_json(args.out, result)
    print(result["coverage"])


if __name__ == "__main__":
    main()
