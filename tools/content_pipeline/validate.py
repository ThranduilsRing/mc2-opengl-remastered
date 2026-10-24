"""Validation: glTF structure (the Khronos glTF-Validator rules that matter here,
reimplemented offline) plus the MC2 runtime profile for cooked GLBs.

validate_gltf(path)             -> list of issues for any glTF/GLB
validate_runtime_glb(path, ...) -> issues for an engine-bound GLB (stricter)
Each issue is (severity, code, message); severity "error" fails a cook.
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from . import gltf_io
from .ir import ALPHA_MODES, MATERIAL_CLASSES

ACCESSOR_TYPES = gltf_io.WIDTH
ELEMENT = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
INDEX_TYPES = {5121, 5123, 5125}


def _err(issues, code, msg, severity="error"):
    issues.append((severity, code, msg))


def validate_document(doc, buffers):
    issues = []
    if doc.get("asset", {}).get("version") != "2.0":
        _err(issues, "ASSET_VERSION", "asset.version must be 2.0")
    if doc.get("extensionsRequired"):
        _err(issues, "EXTENSION_REQUIRED", f"required extensions {doc['extensionsRequired']}")
    for i, v in enumerate(doc.get("bufferViews", [])):
        b = v.get("buffer", -1)
        if not 0 <= b < len(buffers):
            _err(issues, "VIEW_BUFFER", f"bufferView {i} references buffer {b}")
            continue
        end = v.get("byteOffset", 0) + v.get("byteLength", 0)
        if v.get("byteLength", 0) <= 0 or end > len(buffers[b]):
            _err(issues, "VIEW_RANGE", f"bufferView {i} exceeds buffer {b}")
        stride = v.get("byteStride")
        if stride is not None and (stride < 4 or stride > 252 or stride % 4):
            _err(issues, "VIEW_STRIDE", f"bufferView {i} byteStride {stride} invalid")
    for i, a in enumerate(doc.get("accessors", [])):
        if a.get("componentType") not in ELEMENT or a.get("type") not in ACCESSOR_TYPES:
            _err(issues, "ACCESSOR_TYPE", f"accessor {i} has an invalid type")
            continue
        if "bufferView" not in a:
            _err(issues, "ACCESSOR_VIEW", f"accessor {i} has no bufferView (zero-filled accessors unsupported)")
            continue
        if (a.get("byteOffset", 0) + doc["bufferViews"][a["bufferView"]].get("byteOffset", 0)) % ELEMENT[a["componentType"]]:
            _err(issues, "ACCESSOR_ALIGN", f"accessor {i} is not aligned to its component size")
        try:
            gltf_io.read_accessor(doc, buffers, i)
        except (ValueError, KeyError, IndexError) as e:
            _err(issues, "ACCESSOR_DATA", f"accessor {i}: {e}")
    nodes = doc.get("nodes", [])
    parent = {}
    for i, n in enumerate(nodes):
        if "matrix" in n and any(k in n for k in ("translation", "rotation", "scale")):
            _err(issues, "NODE_MATRIX_TRS", f"node {i} has matrix and TRS")
        if "rotation" in n and abs(np.linalg.norm(n["rotation"]) - 1) > 1e-3:
            _err(issues, "NODE_ROTATION", f"node {i} rotation is not unit")
        if "mesh" in n and not 0 <= n["mesh"] < len(doc.get("meshes", [])):
            _err(issues, "NODE_MESH", f"node {i} references a missing mesh")
        for c in n.get("children", []):
            if not 0 <= c < len(nodes) or c == i:
                _err(issues, "NODE_CHILD", f"node {i} has an invalid child {c}")
            elif c in parent:
                _err(issues, "NODE_PARENTS", f"node {c} has two parents")
            parent[c] = i
    for i in range(len(nodes)):
        seen, j = set(), i
        while j in parent:
            if j in seen:
                _err(issues, "NODE_LOOP", f"node {i} is in a cycle")
                break
            seen.add(j)
            j = parent[j]
    for s, scene in enumerate(doc.get("scenes", [])):
        for r in scene.get("nodes", []):
            if not 0 <= r < len(nodes) or r in parent:
                _err(issues, "SCENE_ROOT", f"scene {s} root {r} invalid")
    textures, images = doc.get("textures", []), doc.get("images", [])
    for i, t in enumerate(textures):
        if not 0 <= t.get("source", -1) < len(images):
            _err(issues, "TEXTURE_SOURCE", f"texture {i} has no image")
    for i, m in enumerate(doc.get("materials", [])):
        if m.get("alphaMode", "OPAQUE") not in ALPHA_MODES:
            _err(issues, "ALPHA_MODE", f"material {i} alphaMode invalid")
        if "alphaCutoff" in m and m.get("alphaMode") != "MASK":
            _err(issues, "ALPHA_CUTOFF", f"material {i} alphaCutoff without MASK", "warning")
        cls = (m.get("extras") or {}).get("mc2_material_class")
        if cls is not None and cls not in MATERIAL_CLASSES:
            _err(issues, "MATERIAL_CLASS", f"material {i} class {cls!r} unknown")
        for key in ("normalTexture", "occlusionTexture", "emissiveTexture"):
            if key in m and not 0 <= m[key].get("index", -1) < len(textures):
                _err(issues, "MATERIAL_TEXTURE", f"material {i} {key} missing")
        pbr = m.get("pbrMetallicRoughness", {})
        for key in ("baseColorTexture", "metallicRoughnessTexture"):
            if key in pbr and not 0 <= pbr[key].get("index", -1) < len(textures):
                _err(issues, "MATERIAL_TEXTURE", f"material {i} {key} missing")
    for mi, mesh in enumerate(doc.get("meshes", [])):
        for pi, p in enumerate(mesh.get("primitives", [])):
            where = f"mesh {mi} primitive {pi}"
            attrs = p.get("attributes", {})
            if "POSITION" not in attrs:
                _err(issues, "PRIM_POSITION", f"{where} has no POSITION")
                continue
            try:
                pos = gltf_io.read_accessor(doc, buffers, attrs["POSITION"])
            except (ValueError, KeyError, IndexError):
                continue
            acc = doc["accessors"][attrs["POSITION"]]
            if "min" not in acc or "max" not in acc:
                _err(issues, "POSITION_BOUNDS", f"{where} POSITION lacks min/max")
            elif not (np.allclose(acc["min"], pos.min(axis=0), atol=1e-4) and np.allclose(acc["max"], pos.max(axis=0), atol=1e-4)):
                _err(issues, "POSITION_BOUNDS", f"{where} POSITION min/max disagree with data")
            for key, arr_check in (("NORMAL", "unit"), ("TANGENT", "tangent")):
                if key in attrs:
                    try:
                        a = gltf_io.read_accessor(doc, buffers, attrs[key])
                    except (ValueError, KeyError, IndexError):
                        continue
                    if len(a) != len(pos):
                        _err(issues, "ATTR_COUNT", f"{where} {key} count differs")
                    elif np.any(np.abs(np.linalg.norm(a[:, :3], axis=1) - 1) > 5e-3):
                        _err(issues, "ATTR_UNIT", f"{where} {key} vectors are not unit length")
                    elif arr_check == "tangent" and not np.all(np.isin(np.round(a[:, 3]), (-1, 1))):
                        _err(issues, "TANGENT_W", f"{where} TANGENT.w must be +-1")
            for key, a_id in attrs.items():
                if key != "POSITION" and doc["accessors"][a_id]["count"] != len(pos):
                    _err(issues, "ATTR_COUNT", f"{where} {key} count differs from POSITION")
            if p.get("mode", 4) != 4:
                _err(issues, "PRIM_MODE", f"{where} is not TRIANGLES")
            if "indices" in p:
                ia = doc["accessors"][p["indices"]]
                if ia.get("componentType") not in INDEX_TYPES or ia.get("type") != "SCALAR":
                    _err(issues, "INDEX_TYPE", f"{where} indices type invalid")
                try:
                    idx = gltf_io.read_accessor(doc, buffers, p["indices"]).ravel()
                except (ValueError, KeyError, IndexError):
                    continue
                if len(idx) % 3:
                    _err(issues, "INDEX_COUNT", f"{where} index count not a multiple of 3")
                elif idx.max() >= len(pos):
                    _err(issues, "INDEX_RANGE", f"{where} index out of range")
                else:
                    t = idx.reshape(-1, 3)
                    degenerate = int(np.sum((t[:, 0] == t[:, 1]) | (t[:, 1] == t[:, 2]) | (t[:, 0] == t[:, 2])))
                    if degenerate:
                        _err(issues, "DEGENERATE", f"{where} has {degenerate} degenerate triangles", "warning")
            mat = p.get("material")
            if mat is not None and not 0 <= mat < len(doc.get("materials", [])):
                _err(issues, "PRIM_MATERIAL", f"{where} material {mat} missing")
            elif mat is not None:
                pbr = doc["materials"][mat].get("pbrMetallicRoughness", {})
                if "baseColorTexture" in pbr and "TEXCOORD_0" not in attrs:
                    _err(issues, "PRIM_UV", f"{where} is textured but has no TEXCOORD_0")
    return issues


def validate_gltf(path):
    try:
        doc, buffers, _ = gltf_io.load_document(path)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as e:
        return [("error", "LOAD", str(e))]
    return validate_document(doc, buffers)


def validate_runtime_glb(path, *, max_triangles=None, texture_root=None):
    """The engine-bound profile: structurally valid, no skins/animations, identity
    nodes (transforms already baked), NORMAL on every primitive, base-colour images
    resolvable under texture_root (data/tgl/<tier>), triangle budget."""
    issues = validate_gltf(path)
    if any(s == "error" for s, _, _ in issues):
        return issues
    doc, buffers, _ = gltf_io.load_document(path)
    if doc.get("skins") or doc.get("animations"):
        _err(issues, "RUNTIME_SKIN", "static runtime GLB carries skins/animations")
    for i, n in enumerate(doc.get("nodes", [])):
        pivot = "mc2_pivot_tg_wu" in n.get("extras", {}) and not any(k in n for k in ("matrix", "rotation", "scale"))
        animated_splits = pivot and n.get("extras", {}).get("mc2_anim_node") == n.get("name") and all(
            isinstance(c, int) and 0 <= c < len(doc["nodes"])
            and doc["nodes"][c].get("translation") == [0, 0, 0]
            and doc["nodes"][c].get("extras", {}).get("mc2_pivot_tg_wu") == n["extras"]["mc2_pivot_tg_wu"]
            and not any(k in doc["nodes"][c] for k in ("matrix", "rotation", "scale", "children"))
            for c in n.get("children", []))
        if ("mesh" in n and not pivot and any(k in n for k in ("matrix", "translation", "rotation", "scale"))) or (n.get("children") and not animated_splits):
            _err(issues, "RUNTIME_NODE", f"node {i} is not an identity leaf (transforms must be baked)")
    names = [n.get("name", "") for n in doc.get("nodes", [])]
    dups = sorted({n for n in names if n and names.count(n) > 1})
    if dups:
        _err(issues, "RUNTIME_NODE_NAME", f"duplicate node names {dups[:4]} (the importer keys shapes by node name)")
    tris = 0
    for mi, mesh in enumerate(doc["meshes"]):
        for p in mesh["primitives"]:
            if "NORMAL" not in p["attributes"]:
                _err(issues, "RUNTIME_NORMAL", f"mesh {mi} has no NORMAL (the importer would smooth it)")
            tris += doc["accessors"][p["indices"]]["count"] // 3 if "indices" in p else \
                doc["accessors"][p["attributes"]["POSITION"]]["count"] // 3
    if max_triangles is not None and tris > max_triangles:
        _err(issues, "RUNTIME_BUDGET", f"{tris} triangles > budget {max_triangles}")
    if texture_root is not None:
        for img in doc.get("images", []):
            uri = img.get("uri", "")
            if not uri or "/" in uri or "\\" in uri or not (Path(texture_root) / uri).is_file() \
                    and not (Path(texture_root) / uri).with_suffix(".ktx2").is_file():
                _err(issues, "RUNTIME_TEXTURE", f"image {uri!r} not found under {texture_root}")
    return issues


def errors(issues):
    return [f"{code}: {msg}" for sev, code, msg in issues if sev == "error"]
