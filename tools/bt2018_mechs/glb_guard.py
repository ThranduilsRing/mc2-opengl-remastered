"""Pack GLB hygiene: node-name dedupe at export and the build-time importer-equivalent check.

Assimp rejects a GLB with a bufferView past the BIN chunk or two nodes sharing a name; the engine then
draws a placeholder. The validator reads the GLB strictly (no tolerant parser) so the pack build fails first.
"""
from __future__ import annotations

import json
import struct
from collections import Counter
from pathlib import Path

_ACCESSOR_SIZE = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
_COMPONENT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}


def dedupe_node_names(path) -> dict[str, int]:
    """Rename the 2nd..nth node of a repeated name to <name>__dupN (N from 2, file order). The first keeps
    its name, so bone/anim binding by name is unchanged. Returns {name: renamed_count}; rewrites only if needed."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(path))
    taken = {n.name for n in g.nodes if n.name}
    seen: Counter = Counter()
    renamed: Counter = Counter()
    for n in g.nodes:
        if not n.name:
            continue
        seen[n.name] += 1
        if seen[n.name] > 1:
            k = seen[n.name]
            new = f"{n.name}__dup{k}"
            while new in taken:
                k += 1
                new = f"{n.name}__dup{k}"
            taken.add(new)
            renamed[n.name] += 1
            n.name = new
    if renamed:
        g.save(str(path))
    return dict(renamed)


def validate_glb(path) -> list[str]:
    """Problems Assimp would reject: bad container, bufferView/accessor out of range, duplicate node names."""
    data = Path(path).read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        return ["not a GLB"]
    pos, doc, blob = 12, None, b""
    while pos + 8 <= len(data):
        n, kind = struct.unpack_from("<II", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if kind == 0x4E4F534A:
            doc = json.loads(body.decode("utf-8"))
        elif kind == 0x004E4942 and not blob:
            blob = body
        pos += 8 + n
    if doc is None:
        return ["no JSON chunk"]
    bad = []
    views = doc.get("bufferViews", [])
    buffers = doc.get("buffers", [])
    if buffers and buffers[0].get("byteLength", 0) > len(blob):
        bad.append(f"buffer 0 byteLength {buffers[0]['byteLength']} > BIN chunk {len(blob)}")
    for i, v in enumerate(views):
        end = v.get("byteOffset", 0) + v["byteLength"]
        if end > len(blob):
            bad.append(f"bufferView {i} offset/length ({v.get('byteOffset', 0)}/{v['byteLength']}) out of range (BIN {len(blob)})")
    for i, a in enumerate(doc.get("accessors", [])):
        if "bufferView" not in a:
            continue
        if a["bufferView"] >= len(views):
            bad.append(f"accessor {i} bufferView {a['bufferView']} missing")
            continue
        v = views[a["bufferView"]]
        need = a.get("byteOffset", 0) + a["count"] * _ACCESSOR_SIZE[a["type"]] * _COMPONENT_SIZE[a["componentType"]]
        if not v.get("byteStride") and need > v["byteLength"]:
            bad.append(f"accessor {i} needs {need} bytes, bufferView {a['bufferView']} has {v['byteLength']}")
    dup = [k for k, c in Counter(n.get("name") for n in doc.get("nodes", []) if n.get("name")).items() if c > 1]
    bad += [f"duplicate node name '{k}'" for k in sorted(dup)]
    return bad


_DROPPED = ("_explode", "_dmg", "blip", "indc", "uix")   # mclib/mech_texname_derive.h isDroppedMeshName


def _tga_name(image) -> str:
    """The MC2 texture name the importer derives from a glTF image (mech_texname::deriveName, no alpha prefix)."""
    stem = (image.name or image.uri or "").replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0].lower()
    return "".join(c if (c.isalnum() and c.isascii()) or c in "_-" else "_" for c in stem) + ".tga"


def _base_tex(g, material):
    m = g.materials[material] if material is not None else None
    t = m.pbrMetallicRoughness.baseColorTexture if m is not None and m.pbrMetallicRoughness else None
    return None if t is None else _tga_name(g.images[g.textures[t.index].source])


def _joined_vertex_count(g, blob, prim) -> int:
    """Vertex count after Assimp's aiProcess_JoinIdenticalVertices: distinct (position, normal, uv, joints, weights)
    rows of the primitive's accessors; the raw accessor count when the data is not in the BIN chunk."""
    cols = []
    for attr in ("POSITION", "NORMAL", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"):
        ai = getattr(prim.attributes, attr, None)
        if ai is None:
            continue
        a = g.accessors[ai]
        if a.bufferView is None or not blob:
            return g.accessors[prim.attributes.POSITION].count
        v = g.bufferViews[a.bufferView]
        w = _ACCESSOR_SIZE[a.type] * _COMPONENT_SIZE[a.componentType]
        stride, base = v.byteStride or w, (v.byteOffset or 0) + (a.byteOffset or 0)
        cols.append([blob[base + i * stride: base + i * stride + w] for i in range(a.count)])
    return len(set(zip(*cols))) if cols else 0


_VARIANT = ("widow", "bha", "sldf")   # assimp_importer.cpp isVariantMat


def skin_pick(g):
    """(texture .tga, [(mesh index, primitive index)]) the engine's skinned import binds to slot 0, or (None, []).
    Mirrors assimp_importer.cpp (merged skinned mesh): meshes in file order, skinned and not dropped; the first mesh whose
    material name holds widow/bha/sldf wins outright, else the texture with the most post-join vertices, lexical on ties."""
    blob = g.binary_blob() if g.buffers else b""
    skinned = {n.mesh for n in g.nodes if n.mesh is not None and n.skin is not None}
    count: Counter = Counter()
    for mi, me in enumerate(g.meshes):
        if mi not in skinned or any(k in (me.name or "").lower() for k in _DROPPED):
            continue
        for p in me.primitives:
            tex = _base_tex(g, p.material)
            if tex is None:
                continue
            if any(k in (g.materials[p.material].name or "").lower() for k in _VARIANT):
                return tex, _parts(g, skinned, tex)
            count[tex] += _joined_vertex_count(g, blob, p)
    if not count:
        return None, []
    best = max(count.values())
    tex = min(t for t, c in count.items() if c == best)
    return tex, _parts(g, skinned, tex)


def _parts(g, skinned, tex):
    return [(mi, pi) for mi, me in enumerate(g.meshes) if mi in skinned and not any(k in (me.name or "").lower() for k in _DROPPED)
            for pi, p in enumerate(me.primitives) if _base_tex(g, p.material) == tex]


def opaque_mech_materials(path) -> list[str]:
    """Set alphaMode OPAQUE on the materials that hold the engine's skin texture (skin_pick) when they are BLEND. The
    importer reads glTF BLEND/MASK as a leaf cutout and renames the texture 'a_<name>.tga', which the pack does not ship:
    the mech renders flat white (raven_base was BLEND). Other materials (canopy glass, decals) keep their alpha. A MASK
    skin material is refused (a real cutout body has no valid slot-0 texture). Returns fixed names; rewrites only if needed."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(path))
    _, prims = skin_pick(g)
    mats = {g.meshes[mi].primitives[pi].material for mi, pi in prims}
    for i in sorted(mats):
        if g.materials[i].alphaMode == "MASK":
            raise ValueError(f"{Path(path).name}: skin material '{g.materials[i].name or i}' is MASK; the importer would look for an 'a_' texture and no OPAQUE conversion is safe")
    fixed = [i for i in sorted(mats) if g.materials[i].alphaMode == "BLEND"]
    for i in fixed:
        g.materials[i].alphaMode = "OPAQUE"
    if fixed:
        g.save(str(path))
    return sorted({g.materials[i].name or f"#{i}" for i in fixed})


def skin_texture_problems(path, texture_dirs=()) -> list[str]:
    """What the engine's skinned-mech import (assimp_importer.cpp slot-0 pick, see skin_pick) needs: every intact skinned
    part's material has a base colour texture; the skin materials are OPAQUE; the picked texture's .tga ships in
    texture_dirs (when given). Skin joints and animation targets must not be dedupe-renamed (name-bound everywhere)."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(path))
    bad = []
    skinned = {n.mesh: n.name for n in g.nodes if n.mesh is not None and n.skin is not None}
    for mi, me in enumerate(g.meshes):
        if mi not in skinned or any(k in (me.name or "").lower() for k in _DROPPED):
            continue
        for p in me.primitives:
            if _base_tex(g, p.material) is None:
                bad.append(f"intact part '{skinned[mi]}' uses material {p.material} without a base colour texture")
    skin, prims = skin_pick(g)
    for mi, pi in prims:
        m = g.materials[g.meshes[mi].primitives[pi].material]
        if m.alphaMode not in (None, "OPAQUE"):
            bad.append(f"skin material '{m.name}' is {m.alphaMode}: the importer would look for an 'a_' texture")
    if skin and texture_dirs and not any((Path(d) / skin).is_file() for d in texture_dirs):
        bad.append(f"skin texture '{skin}' (engine pick) is not in the pack")
    joints = {j for s in g.skins for j in s.joints} | {c.target.node for a in g.animations for c in a.channels if c.target and c.target.node is not None}
    bad += [f"joint/animation node '{g.nodes[j].name}' was dedupe-renamed" for j in sorted(joints) if "__dup" in (g.nodes[j].name or "")]
    return bad


def validate_pack_glbs(root) -> dict[str, list[str]]:
    """{relative path: problems} for every bad *.glb under root."""
    root = Path(root)
    out = {}
    for p in sorted(root.rglob("*.glb")):
        bad = validate_glb(p) + (skin_texture_problems(p, [root / "data" / "tgl"]) if (root / "data" / "tgl").is_dir() else [])
        if bad:
            out[p.relative_to(root).as_posix()] = bad
    return out
