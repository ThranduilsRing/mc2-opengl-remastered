"""glTF 2.0 <-> IR.

read_gltf: a glTF/GLB file -> (Scene, materials, skeleton info), hierarchy and
instancing preserved, every attribute kept (normals, tangents, COLOR_0, UV0).
write_runtime_glb: an IR Scene -> the GLB the engine importer loads. Node
transforms are baked into vertices and the result carries identity nodes, so
the engine sees the same geometry whether or not its importer applies node
transforms. The runtime GLB frame is the importer's axis0 inverse:
p_glb = (-x, -y, z) of TG space (a rotation, so winding is unchanged).
"""
from __future__ import annotations

import base64
import json
import struct
from pathlib import Path
from urllib.parse import unquote

import numpy as np

from .ir import Material, Mesh, Node, Primitive, Scene, TextureRef, sha256_bytes, transform_primitive

GLB_MAGIC, CHUNK_JSON, CHUNK_BIN = 0x46546C67, 0x4E4F534A, 0x004E4942
COMPONENT = {5120: "i1", 5121: "u1", 5122: "<i2", 5123: "<u2", 5125: "<u4", 5126: "<f4"}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
# Frames a frontend may declare for its source file.
#   gltf        : standard glTF (+Y up, right-handed); scaled by wu_per_unit.
#   mc2_runtime : already in the engine importer frame (inverse axis0, wu).
TG_FROM_RUNTIME = np.diag([-1.0, -1.0, 1.0, 1.0])
SUPPORTED_EXTENSIONS = {"KHR_materials_emissive_strength", "KHR_materials_specular", "KHR_materials_ior"}


def split_glb(raw: bytes):
    if len(raw) < 20:
        raise ValueError("truncated GLB")
    magic, version, size = struct.unpack_from("<III", raw)
    if magic != GLB_MAGIC or version != 2 or size != len(raw):
        raise ValueError("invalid GLB header")
    pos, doc, binary = 12, None, None
    while pos < len(raw):
        if pos + 8 > len(raw):
            raise ValueError("truncated GLB chunk header")
        length, kind = struct.unpack_from("<II", raw, pos)
        pos += 8
        if pos + length > len(raw) or length % 4:
            raise ValueError("truncated or unaligned GLB chunk")
        if kind == CHUNK_JSON and doc is None:
            doc = json.loads(raw[pos:pos + length].decode("utf-8"))
        elif kind == CHUNK_BIN and binary is None:
            binary = raw[pos:pos + length]
        pos += length
    if doc is None:
        raise ValueError("GLB has no JSON chunk")
    return doc, binary


def load_document(path):
    """(doc, buffers, base_dir) for a .glb or .gltf with local resources only."""
    path = Path(path).resolve()
    raw = path.read_bytes()
    if path.suffix.lower() == ".glb":
        doc, binary = split_glb(raw)
    else:
        doc, binary = json.loads(raw.decode("utf-8")), None
    buffers = []
    for i, b in enumerate(doc.get("buffers", [])):
        if "uri" in b:
            buffers.append(resolve_uri(path.parent, b["uri"]))
        elif i == 0 and binary is not None:
            buffers.append(binary)
        else:
            raise ValueError(f"buffer {i} has no data")
        if len(buffers[-1]) < b.get("byteLength", 0):
            raise ValueError(f"buffer {i} shorter than byteLength")
    return doc, buffers, path.parent


def resolve_uri(base: Path, uri: str) -> bytes:
    if uri.startswith("data:"):
        if ";base64," not in uri:
            raise ValueError("only base64 data URIs are supported")
        return base64.b64decode(uri.split(",", 1)[1], validate=True)
    target = (base / unquote(uri)).resolve()
    if ":" in uri or uri.startswith(("/", "\\")) or not target.is_relative_to(base.resolve()):
        raise ValueError(f"resource {uri!r} is not a local relative path")
    return target.read_bytes()


def read_accessor(doc, buffers, index):
    a = doc["accessors"][index]
    if "sparse" in a:
        raise ValueError("sparse accessors are not supported; export without sparse storage")
    view = doc["bufferViews"][a["bufferView"]]
    dtype = np.dtype(COMPONENT[a["componentType"]])
    width = WIDTH[a["type"]]
    count = a["count"]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = view.get("byteStride", width * dtype.itemsize)
    end = start + max(count - 1, 0) * stride + width * dtype.itemsize
    if count < 1 or stride < width * dtype.itemsize or end > view.get("byteOffset", 0) + view["byteLength"] \
            or end > len(buffers[view["buffer"]]):
        raise ValueError(f"accessor {index} exceeds its buffer view")
    arr = np.ndarray((count, width), dtype=dtype, buffer=buffers[view["buffer"]], offset=start,
                     strides=(stride, dtype.itemsize)).copy()
    if a.get("normalized") and dtype.kind in "iu":
        arr = np.maximum(arr.astype(np.float64) / np.iinfo(dtype).max, -1.0)
    elif dtype.kind == "f":
        arr = arr.astype(np.float64)
    if not np.isfinite(arr.astype(np.float64)).all():
        raise ValueError(f"accessor {index} holds nonfinite values")
    return arr


def node_matrix(node):
    if "matrix" in node:
        if any(k in node for k in ("translation", "rotation", "scale")):
            raise ValueError("node has both matrix and TRS")
        return np.array(node["matrix"], dtype=float).reshape(4, 4).T
    x, y, z, w = node.get("rotation", (0, 0, 0, 1))
    n = np.linalg.norm((x, y, z, w))
    if abs(n - 1) > 1e-3:
        raise ValueError("node rotation is not a unit quaternion")
    x, y, z, w = np.array((x, y, z, w)) / n
    rot = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                    [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = rot @ np.diag(node.get("scale", (1, 1, 1)))
    m[:3, 3] = node.get("translation", (0, 0, 0))
    return m


def frame_matrix(frame: str, wu_per_unit: float = 1.0, yaw_degrees: float = 0.0):
    """Source frame -> TG space."""
    if frame == "mc2_runtime":
        base = TG_FROM_RUNTIME.copy()
    elif frame == "gltf":
        base = np.diag([wu_per_unit, wu_per_unit, wu_per_unit, 1.0])
    else:
        raise ValueError(f"unknown source frame {frame!r}")
    r = np.radians(yaw_degrees)
    yaw = np.eye(4)
    yaw[[0, 0, 2, 2], [0, 2, 0, 2]] = [np.cos(r), np.sin(r), -np.sin(r), np.cos(r)]
    return yaw @ base


def _image_ref(doc, buffers, base_dir, texinfo, role, extract_dir, passthrough=False, texture_roots=()):
    if texinfo is None:
        return None
    if texinfo.get("texCoord", 0) != 0:
        raise ValueError(f"{role}: only TEXCOORD_0 textures are supported")
    if texinfo.get("extensions", {}).get("KHR_texture_transform"):
        raise ValueError(f"{role}: KHR_texture_transform must be baked before import")
    image = doc["images"][doc["textures"][texinfo["index"]]["source"]]
    if "uri" in image and not image["uri"].startswith("data:"):
        path = (base_dir / unquote(image["uri"])).resolve()
        if not path.is_relative_to(base_dir.resolve()):
            raise ValueError(f"{role}: image escapes the source folder")
        if not path.is_file():
            # Runtime-frame exports name engine textures that live under data/tgl/<tier>.
            name = Path(unquote(image["uri"])).name.lower()
            path = next((p for r in (texture_roots or ()) if Path(r).is_dir() for p in Path(r).iterdir()
                         if p.name.lower() == name), None)
            if path is None:
                if texture_roots is None:      # reading back a cooked runtime GLB: names only
                    return None
                raise ValueError(f"{role}: image {image['uri']} missing")
        return TextureRef.of(path, role, passthrough)
    if "uri" in image:
        data = resolve_uri(base_dir, image["uri"])
    else:
        view = doc["bufferViews"][image["bufferView"]]
        start = view.get("byteOffset", 0)
        data = buffers[view["buffer"]][start:start + view["byteLength"]]
    if extract_dir is None:
        raise ValueError(f"{role}: embedded image needs an extract directory")
    ext = ".png" if data[:8] == b"\x89PNG\r\n\x1a\n" else ".jpg" if data[:2] == b"\xff\xd8" else ".bin"
    out = Path(extract_dir) / f"{sha256_bytes(data)[:16]}{ext}"
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists():
        out.write_bytes(data)
    return TextureRef.of(out, role, passthrough)


def read_materials(doc, buffers, base_dir, extract_dir=None, class_of=None, texture_roots=(), unclassified="error", source_refs=None):
    """IR materials from glTF materials (+ the implicit default material when used).

    class_of(name, extras) may override the class; otherwise extras.mc2_material_class
    (or extras.mc2.class) is used. A slot that declares no class and carries no
    authored maps is legacy_prelit; authored maps without a declared class are an error.
    """
    from .ir import CLASS_DEFAULTS
    out = []
    for i, m in enumerate(doc.get("materials", [])):
        name = m.get("name") or f"material_{i}"
        extras = m.get("extras", {}) or {}
        cls = (class_of(name, extras) if class_of else None) or extras.get("mc2_material_class") \
            or (extras.get("mc2") or {}).get("class")
        pbr = m.get("pbrMetallicRoughness", {})
        normal = _image_ref(doc, buffers, base_dir, m.get("normalTexture"), "normal", extract_dir, texture_roots=texture_roots)
        mr = _image_ref(doc, buffers, base_dir, pbr.get("metallicRoughnessTexture"), "orm", extract_dir, texture_roots=texture_roots)
        occ = _image_ref(doc, buffers, base_dir, m.get("occlusionTexture"), "orm", extract_dir, texture_roots=texture_roots)
        orm = mr or occ
        if mr and occ and mr.sha256 != occ.sha256:
            # glTF permits independent AO and MR images on UV0. Pack their authored
            # channels rather than silently dropping both maps (Blender kit exports).
            from PIL import Image
            mr_pixels = np.array(Image.open(mr.path).convert("RGBA"))
            ao = Image.open(occ.path).convert("RGBA").resize((mr_pixels.shape[1], mr_pixels.shape[0]), Image.Resampling.LANCZOS)
            mr_pixels[..., 0] = np.asarray(ao)[..., 0]
            if extract_dir is None:
                raise ValueError("separate AO/MR maps need an extract directory")
            packed = Path(extract_dir) / f"orm-{mr.sha256[:16]}-{occ.sha256[:16]}.png"
            packed.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(mr_pixels, "RGBA").save(packed)
            orm = TextureRef.of(packed, "orm")
        if cls is None:
            if (normal or orm) and unclassified == "error":
                raise ValueError(f"{name}: authored maps need an explicit material class (extras.mc2_material_class)")
            cls = "legacy_prelit"
        metallic_default, rough_default = CLASS_DEFAULTS.get(cls, (0.0, 1.0))
        mat = Material(
            name=name, material_class=cls, alpha_mode=m.get("alphaMode", "OPAQUE"),
            alpha_cutoff=float(m.get("alphaCutoff", 0.5)), double_sided=bool(m.get("doubleSided", False)),
            base_color_policy="authored",
            base_color=_image_ref(doc, buffers, base_dir, pbr.get("baseColorTexture"), "base_color", extract_dir, texture_roots=texture_roots),
            base_color_factor=tuple(pbr.get("baseColorFactor", (1, 1, 1, 1))),
            normal=normal if cls != "legacy_prelit" else None,
            normal_strength=float((m.get("normalTexture") or {}).get("scale", 1.0)),
            orm=orm if cls != "legacy_prelit" else None,
            roughness=float(pbr.get("roughnessFactor", rough_default)) if cls != "legacy_prelit" else 1.0,
            metallic=float(pbr.get("metallicFactor", metallic_default)) if cls != "legacy_prelit" else 0.0,
            ao_strength=float((m.get("occlusionTexture") or {}).get("strength", 1.0)),
            emissive=_image_ref(doc, buffers, base_dir, m.get("emissiveTexture"), "emissive", extract_dir, texture_roots=texture_roots),
            emissive_factor=tuple(m.get("emissiveFactor", (0, 0, 0))),
            reviewed=bool(extras.get("mc2_reviewed", cls != "legacy_prelit")))
        if source_refs is not None:
            source_refs.extend(ref for ref in (normal, mr, occ, mat.base_color, mat.emissive) if ref is not None)
        if mat.base_color is None and mat.base_color_factor[:3] != (1, 1, 1):
            mat.base_color_policy = "authored"
        out.append(mat)
    uses_default = any("material" not in p for mesh in doc.get("meshes", []) for p in mesh["primitives"])
    default_slot = len(out)
    if uses_default:
        out.append(Material(name="gltf_default", base_color_policy="authored"))   # never borrows slot 0
    return out, default_slot


def read_gltf(path, *, frame="gltf", wu_per_unit=1.0, yaw_degrees=0.0, extract_dir=None,
              allow_skin=False, class_of=None, texture_roots=(), unclassified="error"):
    """Read a glTF/GLB into a Scene in TG space. Returns (scene, materials, info)."""
    doc, buffers, base_dir = load_document(path)
    required = set(doc.get("extensionsRequired", []))
    if required:
        raise ValueError(f"unsupported required extensions: {sorted(required)}")
    if (doc.get("skins") or doc.get("animations")) and not allow_skin:
        raise ValueError("skinned/animated source: static assets need a static export (mechs use allow_skin)")
    source_refs = []
    materials, default_slot = read_materials(doc, buffers, base_dir, extract_dir, class_of, texture_roots, unclassified, source_refs)
    scene = Scene()
    mesh_cache = {}

    def mesh_index(mi):
        if mi in mesh_cache:
            return mesh_cache[mi]
        src = doc["meshes"][mi]
        mesh = Mesh(src.get("name") or f"mesh_{mi}")
        for p in src["primitives"]:
            if p.get("mode", 4) != 4:
                raise ValueError(f"{mesh.name}: only triangle primitives are supported")
            if p.get("targets"):
                raise ValueError(f"{mesh.name}: morph targets are not supported")
            attrs = p["attributes"]
            pos = read_accessor(doc, buffers, attrs["POSITION"])
            n = len(pos)
            idx = read_accessor(doc, buffers, p["indices"]).ravel() if "indices" in p else np.arange(n)
            if len(idx) % 3 or idx.min() < 0 or idx.max() >= n:
                raise ValueError(f"{mesh.name}: invalid triangle indices")

            def opt(key, width):
                if key not in attrs:
                    return None
                a = read_accessor(doc, buffers, attrs[key])
                if len(a) != n:
                    raise ValueError(f"{mesh.name}: {key} count differs from POSITION")
                if a.shape[1] == 3 and width == 4:
                    a = np.concatenate([a, np.ones((n, 1))], axis=1)
                return a.astype(np.float64)
            uv = opt("TEXCOORD_0", 2)
            normals = opt("NORMAL", 3)
            if normals is not None:
                lens = np.linalg.norm(normals, axis=1, keepdims=True)
                if np.any(lens < 1e-6):
                    raise ValueError(f"{mesh.name}: zero-length normal")
                normals = normals / lens
            mat = p.get("material", default_slot)
            if not 0 <= mat < len(materials):
                raise ValueError(f"{mesh.name}: material {mat} missing")
            if uv is None and materials[mat].base_color is not None:
                raise ValueError(f"{mesh.name}: textured primitive has no TEXCOORD_0")
            mesh.primitives.append(Primitive(
                positions=pos.astype(np.float64), indices=idx.reshape(-1, 3).astype(np.int64), material=mat,
                normals=normals, tangents=opt("TANGENT", 4),
                uv0=None if uv is None else np.stack([uv[:, 0], 1.0 - uv[:, 1]], axis=1),
                color0=opt("COLOR_0", 4)))
        mesh_cache[mi] = len(scene.meshes)
        scene.meshes.append(mesh)
        return mesh_cache[mi]

    root = Node("source_frame", -1, frame_matrix(frame, wu_per_unit, yaw_degrees))
    scene.nodes.append(root)
    parents = {}
    for i, node in enumerate(doc.get("nodes", [])):
        for c in node.get("children", []):
            if c in parents:
                raise ValueError(f"node {c} has two parents")
            parents[c] = i
    scene_def = doc.get("scenes", [{}])[doc.get("scene", 0)] if doc.get("scenes") else None
    roots = scene_def.get("nodes") if scene_def else [i for i in range(len(doc.get("nodes", []))) if i not in parents]

    def walk(i, parent, chain):
        if i in chain:
            raise ValueError("cyclic node graph")
        src = doc["nodes"][i]
        node = Node(src.get("name") or f"node_{i}", parent, node_matrix(src),
                    mesh_index(src["mesh"]) if "mesh" in src else None, src.get("extras", {}))
        scene.nodes.append(node)
        me = len(scene.nodes) - 1
        for c in src.get("children", []):
            walk(c, me, chain | {i})
    for r in roots or []:
        walk(r, 0, set())
    info = {"skins": len(doc.get("skins", [])), "joints": [doc["nodes"][j].get("name", str(j))
                                                          for s in doc.get("skins", []) for j in s["joints"]],
            "clips": [a.get("name", f"clip_{k}") for k, a in enumerate(doc.get("animations", []))],
            "extensionsUsed": sorted(set(doc.get("extensionsUsed", [])) - SUPPORTED_EXTENSIONS),
            "dependencies": [{"role": "geometry_buffer", "path": str(base_dir / unquote(b["uri"])),
                              "sha256": sha256_bytes(data)} for b, data in zip(doc.get("buffers", []), buffers)
                             if "uri" in b and not b["uri"].startswith("data:")] +
                            [{"role": ref.role, "path": ref.path, "sha256": ref.sha256} for ref in source_refs]}
    return scene, materials, info


# --------------------------------------------------------------------------- writer

class _Builder:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors = [], []

    def add(self, array, gl_type, component, target, minmax=False, normalized=False):
        array = np.ascontiguousarray(array)
        while len(self.bin) % 4:
            self.bin.append(0)
        start = len(self.bin)
        self.bin.extend(array.tobytes())
        self.views.append({"buffer": 0, "byteOffset": start, "byteLength": array.nbytes, "target": target})
        acc = {"bufferView": len(self.views) - 1, "componentType": component,
               "count": int(array.shape[0]) if gl_type != "SCALAR" else int(array.size), "type": gl_type}
        if normalized:
            acc["normalized"] = True
        if minmax:
            acc["min"] = [float(v) for v in array.min(axis=0)]
            acc["max"] = [float(v) for v in array.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1


def smooth_normals(positions, indices):
    """Angle-weighted vertex normals for a primitive that arrived without any."""
    tri = positions[indices]
    out = np.zeros_like(positions)
    for k in range(3):
        a, b, c = tri[:, k], tri[:, (k + 1) % 3], tri[:, (k + 2) % 3]
        e1, e2 = b - a, c - a
        n = np.cross(e1, e2)
        cosang = np.einsum("ij,ij->i", e1, e2) / np.maximum(np.linalg.norm(e1, axis=1) * np.linalg.norm(e2, axis=1), 1e-20)
        w = np.arccos(np.clip(cosang, -1, 1))
        n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20) * w[:, None]
        np.add.at(out, indices[:, k], n)
    lens = np.linalg.norm(out, axis=1, keepdims=True)
    out[lens[:, 0] < 1e-12] = (0, 1, 0)
    return out / np.maximum(lens, 1e-12)


def write_runtime_glb(scene: Scene, materials: list[Material], texture_names: dict[int, str],
                      generator="mc2 content_pipeline") -> bytes:
    """Engine-frame GLB: baked transforms, identity nodes, one mesh per node instance.

    texture_names maps material slot -> runtime base-colour image name (e.g. "a_x.tga");
    the importer derives the texture slot name from that URI stem.
    """
    b = _Builder()
    to_runtime = TG_FROM_RUNTIME   # the frame flip is its own inverse
    doc = {"asset": {"version": "2.0", "generator": generator}, "scene": 0, "scenes": [{"nodes": []}],
           "nodes": [], "meshes": [], "materials": [], "buffers": [], "bufferViews": [], "accessors": []}
    images, textures = [], []
    for slot, m in enumerate(materials):
        out = {"name": m.name, "pbrMetallicRoughness": {"metallicFactor": float(m.metallic),
                                                         "roughnessFactor": float(m.roughness)},
               "extras": {"mc2_material_class": m.material_class}}
        if m.alpha_mode != "OPAQUE":
            out["alphaMode"] = m.alpha_mode
            if m.alpha_mode == "MASK":
                out["alphaCutoff"] = float(m.alpha_cutoff)
        if m.double_sided:
            out["doubleSided"] = True
        if slot in texture_names:
            images.append({"uri": texture_names[slot]})
            textures.append({"source": len(images) - 1})
            out["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(textures) - 1}
        doc["materials"].append(out)
    used = {}
    extras = {n.name: n.extras for n in scene.nodes if n.extras}
    pivots = {n.name: n.extras["mc2_pivot_tg_wu"] for n in scene.nodes if "mc2_pivot_tg_wu" in n.extras}
    # Attachment-only nodes carry authored light contracts and must survive baking.
    for node, world in zip(scene.nodes, scene.world_matrices()):
        if node.mesh is not None or not node.extras:
            continue
        matrix = to_runtime @ world
        doc["nodes"].append({"name": node.name, "matrix": matrix.T.reshape(-1).tolist(), "extras": node.extras})
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        used[node.name] = 1
    for node_name, prim in scene.flattened():
        pivot = pivots.get(node_name)
        properties = extras.get(node_name, {})
        # The engine importer keys shapes by node name: every runtime node name is unique.
        k = used.get(node_name, 0)
        used[node_name] = k + 1
        node_name = node_name if k == 0 else f"{node_name}_{k}"
        prim = transform_primitive(prim, to_runtime)
        if pivot is not None:
            prim.positions -= np.asarray(pivot) * [-1, -1, 1]
        normals = prim.normals if prim.normals is not None else smooth_normals(prim.positions, prim.indices)
        attrs = {"POSITION": b.add(prim.positions.astype("<f4"), "VEC3", 5126, 34962, minmax=True),
                 "NORMAL": b.add(normals.astype("<f4"), "VEC3", 5126, 34962)}
        if prim.uv0 is not None:
            uv = np.stack([prim.uv0[:, 0], 1.0 - prim.uv0[:, 1]], axis=1)
            attrs["TEXCOORD_0"] = b.add(uv.astype("<f4"), "VEC2", 5126, 34962)
        elif prim.material in texture_names and materials[prim.material].base_color is None:
            # A uniform material factor is cooked as a 1x1 image; it needs no authored UVs.
            attrs["TEXCOORD_0"] = b.add(np.zeros((len(prim.positions), 2), "<f4"), "VEC2", 5126, 34962)
        if prim.tangents is not None:
            attrs["TANGENT"] = b.add(prim.tangents.astype("<f4"), "VEC4", 5126, 34962)
        if prim.color0 is not None:
            rgba = np.clip(np.rint(prim.color0 * 255), 0, 255).astype("u1")
            attrs["COLOR_0"] = b.add(rgba, "VEC4", 5121, 34962, normalized=True)
        big = len(prim.positions) > 65535
        idx = b.add(prim.indices.astype("<u4" if big else "<u2").reshape(-1), "SCALAR", 5125 if big else 5123, 34963)
        doc["meshes"].append({"name": node_name, "primitives": [
            {"attributes": attrs, "indices": idx, "material": int(prim.material), "mode": 4}]})
        node = {"name": node_name, "mesh": len(doc["meshes"]) - 1}
        if properties and k == 0:
            node["extras"] = dict(properties)
        if pivot is not None:
            node["translation"] = (np.asarray(pivot) * [-1, -1, 1]).tolist()
            node.setdefault("extras", {})["mc2_pivot_tg_wu"] = pivot
        doc["nodes"].append(node)
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
    if not doc["meshes"]:
        raise ValueError("scene has no geometry to write")
    if images:
        doc["images"], doc["textures"] = images, textures
    while len(b.bin) % 4:
        b.bin.append(0)
    doc["buffers"] = [{"byteLength": len(b.bin)}]
    doc["bufferViews"], doc["accessors"] = b.views, b.accessors
    body = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode("utf-8")
    body += b" " * (-len(body) % 4)
    total = 12 + 8 + len(body) + 8 + len(b.bin)
    return (struct.pack("<III", GLB_MAGIC, 2, total) + struct.pack("<II", len(body), CHUNK_JSON) + body
            + struct.pack("<II", len(b.bin), CHUNK_BIN) + bytes(b.bin))

