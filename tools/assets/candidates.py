"""Local candidate import. No network code; scene transforms are baked explicitly."""
from __future__ import annotations

import base64
from dataclasses import dataclass
import io
import json
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image

from tools.assets.common import Mesh, bounds, output_path


@dataclass
class Candidate:
    mesh: Mesh
    images: list
    source: Path


def local_uri(root, uri):
    if uri.startswith("data:"):
        if ";base64," not in uri:
            raise ValueError("only base64 data URIs supported")
        return base64.b64decode(uri.split(",", 1)[1], validate=True)
    from urllib.parse import unquote
    if ":" in uri or uri.startswith(("/", "\\")):
        raise ValueError("candidate resources must be local relative paths")
    path = (root/unquote(uri)).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("candidate resource escapes its folder")
    return path.read_bytes()


def node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=float).reshape(4, 4).T
    x, y, z, w = node.get("rotation", (0, 0, 0, 1))
    norm = np.linalg.norm((x, y, z, w))
    if norm < 1e-10:
        raise ValueError("invalid node quaternion")
    x, y, z, w = np.array((x, y, z, w))/norm
    rot = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                    [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                    [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    m = np.eye(4)
    m[:3, :3] = rot @ np.diag(node.get("scale", (1, 1, 1)))
    m[:3, 3] = node.get("translation", (0, 0, 0))
    return m


def load_gltf(path):
    path = Path(path).resolve()
    raw = path.read_bytes()
    binary = None
    if path.suffix.lower() == ".glb":
        if len(raw) < 20:
            raise ValueError("truncated GLB")
        magic, version, size = struct.unpack_from("<III", raw)
        if magic != 0x46546C67 or version != 2 or size != len(raw):
            raise ValueError("invalid GLB header")
        pos, doc = 12, None
        while pos < len(raw):
            if pos+8 > len(raw):
                raise ValueError("truncated GLB chunk header")
            length, kind = struct.unpack_from("<II", raw, pos); pos += 8
            if pos+length > len(raw):
                raise ValueError("truncated GLB chunk")
            if kind == 0x4E4F534A:
                doc = json.loads(raw[pos:pos+length])
            elif kind == 0x004E4942:
                binary = raw[pos:pos+length]
            pos += length
        if doc is None:
            raise ValueError("missing GLB JSON chunk")
    else:
        doc = json.loads(raw)
    if doc.get("skins") or doc.get("animations"):
        raise ValueError("rigged/animated candidates require an authored static export")
    if doc.get("extensionsRequired"):
        raise ValueError(f"unsupported required glTF extensions: {doc['extensionsRequired']}")
    buffers = [local_uri(path.parent, b["uri"]) if "uri" in b else binary for b in doc.get("buffers", [])]
    if any(b is None for b in buffers):
        raise ValueError("missing glTF binary buffer")
    sizes = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
    dtypes = {5120: "i1", 5121: "u1", 5122: "<i2", 5123: "<u2", 5125: "<u4", 5126: "<f4"}

    def accessor(index):
        a = doc["accessors"][index]
        if "sparse" in a:
            raise ValueError("sparse glTF accessors require a static export")
        view = doc["bufferViews"][a["bufferView"]]
        dtype = np.dtype(dtypes[a["componentType"]])
        count, components = a["count"], sizes[a["type"]]
        start = view.get("byteOffset", 0)+a.get("byteOffset", 0)
        stride = view.get("byteStride", components*dtype.itemsize)
        end = start + max(count-1, 0)*stride+components*dtype.itemsize
        if count < 1 or stride < components*dtype.itemsize or end > len(buffers[view["buffer"]]) or end > view.get("byteOffset", 0)+view["byteLength"]:
            raise ValueError("invalid glTF accessor range/stride")
        arr = np.ndarray((count, components), dtype=dtype, buffer=buffers[view["buffer"]], offset=start, strides=(stride, dtype.itemsize)).copy()
        if a.get("normalized") and a["componentType"] != 5126:
            arr = np.maximum(arr.astype(float)/np.iinfo(dtype).max, -1)
        if not np.isfinite(arr).all():
            raise ValueError("nonfinite glTF attribute")
        return arr

    images = []
    materials = list(doc.get("materials", []))
    default_material = len(materials)
    if any("material" not in p for mesh in doc.get("meshes", []) for p in mesh["primitives"]):
        materials.append({})  # glTF's implicit material is independent of slot zero.
    for material in materials:
        pbr = material.get("pbrMetallicRoughness", {})
        factor = np.array(pbr.get("baseColorFactor", [1, 1, 1, 1]))
        if "baseColorTexture" in pbr:
            texture = pbr["baseColorTexture"]
            if texture.get("texCoord", 0) != 0 or texture.get("extensions"):
                raise ValueError("only untransformed TEXCOORD_0 supported")
            image = doc["images"][doc["textures"][texture["index"]]["source"]]
            if "uri" in image:
                content = local_uri(path.parent, image["uri"])
            else:
                view = doc["bufferViews"][image["bufferView"]]
                start = view.get("byteOffset", 0)
                content = buffers[view["buffer"]][start:start+view["byteLength"]]
            img = Image.open(io.BytesIO(content)).convert("RGBA")
        else:
            img = Image.new("RGBA", (1, 1), (255, 255, 255, 255))
        rgba = np.array(img)
        srgb = rgba[:, :, :3].astype(float)/255
        linear = np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4)*factor[:3]
        encoded = np.where(linear <= .0031308, linear*12.92, 1.055*np.maximum(linear, 0)**(1/2.4)-.055)
        rgba[:, :, :3] = np.clip(np.round(encoded*255), 0, 255).astype(np.uint8)
        rgba[:, :, 3] = np.clip(np.round(rgba[:, :, 3].astype(float)*factor[3]), 0, 255).astype(np.uint8)
        if material.get("alphaMode", "OPAQUE") == "OPAQUE":
            rgba[:, :, 3] = 255
        images.append(Image.fromarray(rgba))
    positions, uv, slots, nodes = [], [], [], []

    def walk(index, parent, chain):
        if index in chain:
            raise ValueError("cyclic glTF node graph")
        node = doc["nodes"][index]
        transform = parent @ node_matrix(node)
        if not np.isfinite(transform).all() or abs(np.linalg.det(transform[:3, :3])) < 1e-12:
            raise ValueError("singular/nonfinite node transform")
        if "mesh" in node:
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                if primitive.get("mode", 4) != 4 or primitive.get("targets"):
                    raise ValueError("only static triangle primitives supported")
                attrs = primitive["attributes"]
                v = accessor(attrs["POSITION"])
                if v.shape[1] != 3:
                    raise ValueError("POSITION must be VEC3")
                v = v @ transform[:3, :3].T + transform[:3, 3]
                idx = accessor(primitive["indices"]).ravel() if "indices" in primitive else np.arange(len(v))
                if len(idx)%3 or not np.issubdtype(idx.dtype, np.integer) or idx.min() < 0 or idx.max() >= len(v):
                    raise ValueError("invalid glTF triangle indices")
                idx = idx.reshape(-1, 3)
                if np.linalg.det(transform[:3, :3]) < 0:
                    idx = idx[:, [0, 2, 1]]
                tex = accessor(attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else np.zeros((len(v), 2))
                material = primitive.get("material", default_material)
                if material < 0 or material >= len(images) or tex.shape != (len(v), 2):
                    raise ValueError("invalid glTF UV/material")
                if "TEXCOORD_0" not in attrs and "baseColorTexture" in materials[material].get("pbrMetallicRoughness", {}):
                    raise ValueError("textured primitive has no TEXCOORD_0")
                positions.append(v[idx].reshape(-1, 3)); uv.append(tex[idx].reshape(-1, 2))
                slots.append(np.full(len(idx), material, dtype=int))
            nodes.append({"name": node.get("name", str(index)), "transform_baked": True})
        for child in node.get("children", []):
            walk(child, transform, chain | {index})

    roots = doc.get("scenes", [{}])[doc.get("scene", 0)].get("nodes")
    if roots is None:
        children = {i for n in doc.get("nodes", []) for i in n.get("children", [])}
        roots = [i for i in range(len(doc.get("nodes", []))) if i not in children]
    for node in roots:
        walk(node, np.eye(4), set())
    if not positions:
        raise ValueError("candidate scene has no triangles")
    mesh = Mesh(np.concatenate(positions), np.concatenate(uv), np.concatenate(slots),
                [f"material_{i}" for i in range(len(images))], nodes,
                [im.getextrema()[3][0] < 255 for im in images])
    bounds(mesh.vertices)
    return Candidate(mesh, images, path)


def load_candidate(path):
    path = Path(path).resolve()
    if path.suffix.lower() in (".glb", ".gltf"):
        return load_gltf(path)
    if path.suffix.lower() != ".obj":
        raise ValueError("use GLB/glTF/OBJ; convert local FBX with the documented Blender adapter")
    import trimesh
    scene = trimesh.load(path, force="scene", process=False)
    vertices, uv, mats, images = [], [], [], []
    for node in sorted(scene.graph.nodes_geometry):
        transform, geom = scene.graph[node]
        g = scene.geometry[geom]
        if not len(g.faces):
            continue
        visual = g.visual
        material = getattr(visual, "material", None)
        if hasattr(material, "materials"):
            raise ValueError("multi-material OBJ: export as GLB to preserve its slots")
        image = getattr(material, "baseColorTexture", None)
        if image is None:
            image = getattr(material, "image", None)
        if image is None:
            color = getattr(material, "diffuse", (200, 200, 200, 255))
            image = Image.new("RGBA", (1, 1), tuple(int(c) for c in color))
        image = image.convert("RGBA")
        tex = getattr(visual, "uv", None)
        tex = np.asarray(tex).copy() if tex is not None else np.zeros((len(g.vertices), 2))
        tex[:, 1] = 1-tex[:, 1]  # OBJ bottom-origin -> glTF/Pillow top-origin
        idx = g.faces.copy()
        if np.linalg.det(transform[:3, :3]) < 0:
            idx = idx[:, [0, 2, 1]]
        points = np.asarray(g.vertices) @ transform[:3, :3].T + transform[:3, 3]
        vertices.append(points[idx].reshape(-1, 3)); uv.append(tex[idx].reshape(-1, 2))
        mats.append(np.full(len(idx), len(images), dtype=int)); images.append(image)
    if not vertices:
        raise ValueError("OBJ has no triangles")
    mesh = Mesh(np.concatenate(vertices), np.concatenate(uv), np.concatenate(mats),
                [f"material_{i}" for i in range(len(images))], [],
                [im.getextrema()[3][0] < 255 for im in images])
    bounds(mesh.vertices)
    if not np.isfinite(mesh.uv).all():
        raise ValueError("nonfinite OBJ UVs")
    return Candidate(mesh, images, path)


def convert_fbx(source, destination, blender):
    """Optional installed-Blender adapter; never auto-install a converter."""
    destination = output_path(destination, mod=True)
    if destination.suffix.lower() != ".glb":
        raise ValueError("FBX conversion output must be GLB")
    if destination.exists():
        raise FileExistsError(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    expression = ("import bpy; bpy.ops.wm.read_factory_settings(use_empty=True); "
                  f"bpy.ops.import_scene.fbx(filepath={str(Path(source).resolve())!r}); "
                  f"bpy.ops.export_scene.gltf(filepath={str(destination)!r}, export_format='GLB', export_animations=False)")
    sys_path = Path(__file__).resolve().parents[2]/"scripts"
    import sys
    sys.path.insert(0, str(sys_path))
    import console_child
    result = subprocess.run([str(blender), "--background", "--factory-startup", "--python-expr", expression],
                            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if result.returncode or not destination.is_file():
        raise ValueError("Blender conversion failed: "+result.stderr[-2000:])
    return destination


def export_runtime_gltf(candidate, folder, stem, max_texture_edge):
    """Bake inverse default prop axis map; emit identity nodes + PNG/TGA textures.

    Runtime default axis0 is TG=(-src.x,-src.y,src.z). StaticProp autoGround
    is disabled by bdactor_type.cpp. The exported glTF is intentionally -Y-up
    so runtime TG is +Y-up. Source candidate previews remain conventional +Y-up.
    """
    from tools.assets.common import write_json
    folder = output_path(folder, mod=True)
    folder.mkdir(parents=True, exist_ok=True)
    binary = bytearray()
    doc = {"asset": {"version": "2.0", "generator": "mc2-assets offline prop cooker"},
           "buffers": [], "bufferViews": [], "accessors": [], "images": [],
           "textures": [], "materials": [], "meshes": [{"primitives": []}],
           "nodes": [{"name": "modern_prop", "mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0}

    def accessor(array, kind):
        array = np.asarray(array, dtype="<f4")
        while len(binary)%4:
            binary.append(0)
        start = len(binary); binary.extend(array.tobytes())
        view = len(doc["bufferViews"])
        doc["bufferViews"].append({"buffer": 0, "byteOffset": start, "byteLength": array.nbytes})
        record = {"bufferView": view, "componentType": 5126, "count": len(array), "type": kind}
        if kind == "VEC3":
            record.update(min=array.min(axis=0).tolist(), max=array.max(axis=0).tolist())
        doc["accessors"].append(record)
        return len(doc["accessors"])-1

    cooked_images, texnames = [], []
    for index, image in enumerate(candidate.images):
        image = image.copy().convert("RGBA")
        if max(image.size) > max_texture_edge:
            ratio = max_texture_edge/max(image.size)
            image = image.resize(tuple(max(1, round(v*ratio)) for v in image.size), Image.Resampling.LANCZOS)
        alpha = image.getextrema()[3][0] < 255
        name = ("a_" if alpha else "")+f"{stem}_mat{index:03d}"
        image.save(folder/(name+".png"))
        image.save(folder/(name+".tga"))
        # Runtime searches data/tgl/ObjectTextureSize, not manifestDir.
        modroot = folder.parents[2]
        for tier in (32, 64, 128, 256):
            target = output_path(modroot/"data"/"tgl"/str(tier), mod=True)
            target.mkdir(parents=True, exist_ok=True)
            image.save(target/(name+".tga"))
        doc["images"].append({"uri": name+".png", "name": name})
        doc["textures"].append({"source": index})
        doc["materials"].append({"name": name, "doubleSided": alpha,
            "alphaMode": "MASK" if alpha else "OPAQUE", "alphaCutoff": .5,
            "pbrMetallicRoughness": {"baseColorTexture": {"index": index}, "metallicFactor": 0, "roughnessFactor": 1}})
        cooked_images.append(image); texnames.append(name+".tga")
    mesh = candidate.mesh
    for material in sorted(set(int(n) for n in mesh.materials)):
        face_mask = mesh.materials == material
        positions = mesh.vertices.reshape(-1, 3, 3)[face_mask].reshape(-1, 3)*[-1, -1, 1]
        tex = mesh.uv.reshape(-1, 3, 2)[face_mask].reshape(-1, 2)
        triangles = positions.reshape(-1, 3, 3)
        normals = np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0])
        normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-12)
        attrs = {"POSITION": accessor(positions, "VEC3"),
                 "NORMAL": accessor(np.repeat(normals, 3, axis=0), "VEC3"),
                 "TEXCOORD_0": accessor(tex, "VEC2")}
        doc["meshes"][0]["primitives"].append({"attributes": attrs, "material": material, "mode": 4})
    doc["buffers"] = [{"uri": stem+".bin", "byteLength": len(binary)}]
    (folder/(stem+".bin")).write_bytes(binary)
    write_json(folder/(stem+".gltf"), doc)
    return folder/(stem+".gltf"), cooked_images, texnames
