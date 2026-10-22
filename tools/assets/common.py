"""Read-only stock geometry, texture access and guarded artifact writes.

TGL layout mirrors mclib/{msl_load,tgl_types}.cpp: Win32 serialized structs,
not the host's native ABI. Coordinates are TG (+Y up), in game world units.
"""
from __future__ import annotations

import io
import json
import math
import re
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

from tools.assets.mapgen_data import Install, fit_sections, key

INSTALL_ROOT = Path("A:/Games/mc2-opengl")
OUT_ROOT = Path("G:/mc2-assets/out")
INSTALLS = {"stock": "mc2-win64-v0.4", "MC2X-CVE-G": "MC2X-CVE-G",
            "MC2X-REAP3": "MC2X-REAP3", "Exodus": "MC2-Exodus",
            "MagicExpansion": "MC2-MagicExpansion", "MechCommanderOmnitech": "MechCommanderOmnitech",
            "MCO-Carver5": "MCO-Carver5"}


def output_path(path, *, mod=False):
    path = Path(path).resolve()
    if path.is_relative_to(INSTALL_ROOT.resolve()):
        raise ValueError(f"read-only install: {path}")
    if mod and not path.is_relative_to(OUT_ROOT.resolve()):
        raise ValueError(f"mod output must be inside {OUT_ROOT}: {path}")
    return path


def write_json(path, value):
    path = output_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    tmp.replace(path)


def bounds(vertices):
    v = np.asarray(vertices)
    if v.ndim != 2 or v.shape[1] != 3 or not len(v) or not np.isfinite(v).all():
        raise ValueError("geometry must contain finite XYZ vertices")
    lo, hi = v.min(axis=0), v.max(axis=0)
    return {"min": lo.tolist(), "max": hi.tolist(), "size": (hi-lo).tolist(),
            "footprint_wu": [float(hi[0]-lo[0]), float(hi[2]-lo[2])],
            "height_wu": float(hi[1]-lo[1]),
            "extent_radius_wu": float(np.linalg.norm(v, axis=1).max())}


@dataclass
class Mesh:
    # Expanded corners retain per-face UV seams and material slots.
    vertices: np.ndarray
    uv: np.ndarray
    materials: np.ndarray
    textures: list[str]
    nodes: list[dict]
    texture_alpha: list[bool]
    normals: np.ndarray | None = None

    @property
    def triangles(self):
        return len(self.vertices) // 3

    def summary(self):
        return {"triangles": self.triangles, "bounds": bounds(self.vertices),
                "textures": self.textures, "texture_alpha": self.texture_alpha,
                "nodes": self.nodes}


def transform_normals(normals, linear):
    if normals is None:
        return None
    transformed = np.asarray(normals) @ np.linalg.inv(linear)
    lengths = np.linalg.norm(transformed, axis=1, keepdims=True)
    if not np.isfinite(transformed).all() or np.any(lengths < 1e-12):
        raise ValueError("invalid vertex normals")
    return transformed/lengths


def face_normals(vertices):
    triangles = np.asarray(vertices).reshape(-1, 3, 3)
    normals = np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0])
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
    return np.repeat(normals, 3, axis=0)


def parse_tgl(raw):
    """Strict legacy TGL decoder; reject truncation, unknown nodes and bad indices."""
    pos = 0

    def take(n):
        nonlocal pos
        if n < 0 or pos+n > len(raw):
            raise ValueError(f"truncated TGL at {pos}, need {n}")
        data = raw[pos:pos+n]
        pos += n
        return data

    def count():
        n, = struct.unpack("<I", take(4))
        if n > 2_000_000:
            raise ValueError(f"invalid TGL count {n}")
        return n

    if struct.unpack("<I", take(4))[0] != 0xBAFDECAF:
        raise ValueError("unsupported TGL version")
    nn, nt = count(), count()
    names, alpha = [], []
    for _ in range(nt):
        tex = take(268)
        names.append(key(tex[:256].split(b"\0")[0].decode("latin1")).split("/")[-1])
        alpha.append(bool(tex[264]))
    vertices, uvs, mats, nodes = [], [], [], []
    for _ in range(nn):
        kind = count()
        if kind not in (0, 1):
            raise ValueError(f"unknown TGL node type {kind}")
        if kind == 1:
            nv = count()
            v = np.frombuffer(take(nv*28), dtype=np.dtype([
                ("pos", "<f4", 3), ("normal", "<f4", 3), ("argb", "<u4")]))["pos"]
            nf = count()
            t = np.frombuffer(take(nf*56), dtype=np.dtype([
                ("idx", "<u4", 3), ("tex", "<u4"), ("flags", "<u4"),
                ("uv", "<f4", (3, 2)), ("normal", "<f4", 3)]))
            take(count()*12)
            if nf and (t["idx"].max() >= nv or (nt and t["tex"].max() >= nt)):
                raise ValueError("out-of-range TGL vertex/material index")
        center = struct.unpack("<3f", take(12))
        relative = struct.unpack("<3f", take(12))
        name = take(25).split(b"\0")[0].decode("latin1")
        parent = take(25).split(b"\0")[0].decode("latin1")
        helper = name.lower().startswith(("_pab", "los_", "spotlight"))
        nodes.append({"name": name, "parent": parent, "center_wu": list(center),
                      "relative_center_wu": list(relative), "helper": helper,
                      "triangles": nf if kind == 1 else 0})
        if kind == 1 and nf and not helper:
            # Binary verts are node-local; absolute nodeCenter assembles the rest pose.
            vertices.append(v[t["idx"]].reshape(-1, 3) + center)
            uvs.append(t["uv"].reshape(-1, 2))
            mats.append(t["tex"])
    if pos != len(raw):
        raise ValueError(f"unexpected trailing TGL bytes: {len(raw)-pos}")
    if not vertices:
        raise ValueError("TGL has no visible triangles")
    mesh = Mesh(np.concatenate(vertices), np.concatenate(uvs), np.concatenate(mats), names, nodes, alpha)
    bounds(mesh.vertices)
    if not np.isfinite(mesh.uv).all():
        raise ValueError("nonfinite TGL UVs")
    return mesh


def model_refs(sections):
    """INI references are authoritative; never guess 'x' means destroyed (it's shadow)."""
    result = []
    for section, values in sections.items():
        for field, name in values.items():
            if not isinstance(name, str) or not name or not (field.startswith("filename") or field == "shadowname"):
                continue
            if section not in ("tgldata", "tgldamage"):
                continue  # destruct effects / animations are not object geometry
            suffix = field[len("filename"):]
            lod = int(suffix) if suffix.isdigit() else 0
            role = ("damage_shadow" if section == "tgldamage" else "shadow") if field == "shadowname" else (
                "destroyed" if section == "tgldamage" else "intact")
            base = key(name).split("/")[-1].removesuffix(".ase").removesuffix(".tgl")
            result.append({"role": role, "lod": lod, "distance_wu": values.get(f"distance{lod}", 0),
                           "tgl": f"data/tgl/{base}.tgl", "ase": f"data/tgl/{base}.ase"})
    return result


def model_sections(raw):
    """Fold continued quoted INI filenames (stock WireFence damage shadow).

    The generic mapgen scalar parser only reads single-line strings. Restrict
    this repair to typed assignments so adjacent, well-formed strings cannot
    accidentally be joined across sections.
    """
    text = raw.decode("latin1") if isinstance(raw, bytes) else raw
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    pattern = r'(?m)(^\s*\w+\s+\w+\s*=\s*)"([^"]*)"'
    text = re.sub(pattern, lambda m: m[1]+'"'+re.sub(r"\n\s*", "", m[2])+'"', text)
    return fit_sections(text)


def texture_image(install, name):
    # ObjectTextureSize defaults to 128. Resolve tier explicitly, not arbitrary basename matches.
    base = key(name).split("/")[-1]
    for tier in (128, 256, 64, 32):
        path = f"data/tgl/{tier}/{base}"
        if install.has(path):
            raw, origin = install.read(path)
            image = Image.open(io.BytesIO(raw)).convert("RGBA")
            return image, path, origin
    path = f"data/tgl/{base}"
    raw, origin = install.read(path)
    return Image.open(io.BytesIO(raw)).convert("RGBA"), path, origin


def stock_install(root=INSTALL_ROOT):
    # The dev install has experimental loose missions/object2. Stock means FST baseline.
    install = Install(Path(root)/INSTALLS["stock"], include_loose=False)
    # object2 is not an FST member in this distribution. Use the pristine source
    # data, not the dev install's mod-extended object2 and Buildings.csv.
    fresh = Path(root)/"mc2srcdata-fresh"
    for name, relative in (("data/objects/object2.pak", "objects/object2.pak"),
                           ("data/art/buildings.csv", "art/buildings.csv")):
        path = fresh/relative
        if path.is_file():
            install.files[name] = (path,)
        elif not install.has(name):
            raise FileNotFoundError(f"pristine stock data required: {path}")
    return install
