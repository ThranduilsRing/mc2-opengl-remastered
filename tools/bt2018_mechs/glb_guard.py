"""Pack GLB hygiene: node-name dedupe at export and the build-time importer-equivalent check.

Assimp rejects a GLB with a bufferView past the BIN chunk or two nodes sharing a name; the engine then
draws a placeholder. The validator reads the GLB strictly (no tolerant parser) so the pack build fails first.
"""
from __future__ import annotations

import json
import re
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
_VARIANT = ("widow", "bha", "sldf")   # assimp_importer.cpp isVariantMat
_FOLIAGE = ("leaf", "leaves", "foliage")   # assimp_importer.cpp DeriveMC2TextureName name heuristic
_STRUCT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}


def _wants_alpha(material) -> bool:
    """DeriveMC2TextureName wantAlpha: glTF alphaMode MASK/BLEND, else a leaf/leaves/foliage material name."""
    if material.alphaMode in ("MASK", "BLEND"):
        return True
    return any(k in (material.name or "").lower() for k in _FOLIAGE)


def _tga_name(image, alpha=False) -> str:
    """The MC2 texture name the importer derives from a glTF image (mclib/mech_texname_derive.h deriveName, byte for byte:
    per-byte sanitize to [a-z0-9_-], 256 clamp, '.tga', and the 'a_' prefix when alpha)."""
    raw = (image.name or image.uri or "").replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0].encode("utf-8").lower()
    stem = "".join(chr(c) if (97 <= c <= 122 or 48 <= c <= 57 or c in b"_-") else "_" for c in raw)
    if not stem:
        return ""
    stem = stem[:256 - 4 - 1] + ".tga"
    if alpha and not stem.startswith("a_"):
        if len(stem) + 2 + 1 > 256:
            stem = stem[:256 - 2 - 1]
        stem = "a_" + stem
    return stem


def _base_tex(g, material):
    """The texture name the engine's DeriveMC2TextureName returns for a primitive's material (None: no base colour)."""
    m = g.materials[material] if material is not None else None
    t = m.pbrMetallicRoughness.baseColorTexture if m is not None and m.pbrMetallicRoughness else None
    if t is None:
        return None
    return _tga_name(g.images[g.textures[t.index].source], _wants_alpha(m)) or None


class UnsupportedGlb(ValueError):
    """A layout the guard cannot decode faithfully (it must not guess a vertex count)."""


def _buffer(g, blob, bi) -> bytes:
    b = g.buffers[bi]
    if b.uri is None:
        if bi == 0 and blob:
            return blob
        raise UnsupportedGlb(f"buffer {bi} has no uri and is not the GLB BIN chunk")
    if b.uri.startswith("data:"):
        import base64
        head, _, body = b.uri.partition(",")
        return base64.b64decode(body) if head.endswith(";base64") else body.encode("latin-1")
    raise UnsupportedGlb(f"buffer {bi} is an external file ('{b.uri}'); the guard decodes GLB BIN and data: buffers only")


def _read(g, blob, view_idx, offset, count, width, fmt):
    v = g.bufferViews[view_idx]
    data = _buffer(g, blob, v.buffer)
    base = (v.byteOffset or 0) + offset
    step = v.byteStride or width
    need = base + (count - 1) * step + width if count else base
    if need > (v.byteOffset or 0) + v.byteLength or need > len(data):
        raise UnsupportedGlb(f"bufferView {view_idx} read of {count} rows runs past its data")
    return [struct.unpack_from(fmt, data, base + i * step) for i in range(count)]


def _accessor_rows(g, blob, ai) -> list:
    """Decode accessor ai to a list of component tuples (bufferView data with stride, then the sparse overrides)."""
    a = g.accessors[ai]
    n = _ACCESSOR_SIZE[a.type]
    fmt = "<" + _STRUCT[a.componentType] * n
    width = n * _COMPONENT_SIZE[a.componentType]
    if a.bufferView is None:
        rows = [(0,) * n for _ in range(a.count)]
    else:
        rows = _read(g, blob, a.bufferView, a.byteOffset or 0, a.count, width, fmt)
    if a.sparse is not None and a.sparse.count:
        sp = a.sparse
        ic = sp.indices.componentType
        idx = _read(g, blob, sp.indices.bufferView, sp.indices.byteOffset or 0, sp.count, _COMPONENT_SIZE[ic], "<" + _STRUCT[ic])
        vals = _read(g, blob, sp.values.bufferView, sp.values.byteOffset or 0, sp.count, width, fmt)
        for (i,), v in zip(idx, vals):
            if i >= a.count:
                raise UnsupportedGlb(f"accessor {ai} sparse index {i} out of range")
            rows[i] = v
    return rows


def _used_vertices(g, blob, prim, n):
    """Vertex indices the importer keeps (glTF2Importer vertexRemappingTable + JoinVertices usedVertexIndicesMask):
    the in-range indices of the faces it emits; every vertex when the primitive has no index accessor."""
    if prim.indices is None:
        return range(n)
    idx = [r[0] for r in _accessor_rows(g, blob, prim.indices)]
    if (4 if prim.mode is None else prim.mode) == 4:
        faces = [idx[i:i + 3] for i in range(0, len(idx) - 2, 3)]
        return {v for f in faces if all(0 <= v < n for v in f) for v in f}
    return {v for v in idx if 0 <= v < n}


def _joined_vertex_count(g, blob, prim) -> int:
    """Vertex count of the aiMesh after aiProcess_JoinIdenticalVertices (the importer's flags, assimp_importer.cpp): the
    distinct rows over the referenced vertices of everything Assimp's CompareVerticesAlmostEqual compares, i.e. position,
    normal, tangent (carries the bitangent sign), every TEXCOORD_n and COLOR_n. Bone joints/weights are not compared.
    Attributes whose count differs from POSITION are ignored, as the glTF2 importer does. Raises UnsupportedGlb."""
    at = prim.attributes
    if at.POSITION is None:
        return 0
    n = g.accessors[at.POSITION].count
    names = ["POSITION"]
    if at.NORMAL is not None and g.accessors[at.NORMAL].count == n:
        names.append("NORMAL")
        if at.TANGENT is not None and g.accessors[at.TANGENT].count == n:
            names.append("TANGENT")
    extra = sorted(k for k, v in vars(at).items() if v is not None and re.fullmatch(r"(TEXCOORD|COLOR)_\d+", k))
    names += [k for k in extra if g.accessors[getattr(at, k)].count == n]
    cols = [_accessor_rows(g, blob, getattr(at, k)) for k in names]
    return len({tuple(tuple(x + 0.0 for x in c[i]) for c in cols) for i in _used_vertices(g, blob, prim, n)})


def skin_pick(g):
    """(texture .tga, [(mesh index, primitive index)]) the engine's skinned import binds to slot 0, or (None, []).
    Mirrors assimp_importer.cpp (merged skinned mesh), including the 'a_' alpha prefix: meshes in file order, skinned and not
    dropped, empty meshes skipped; the first mesh whose material name holds widow/bha/sldf wins outright, else the texture
    with the most post-join vertices, lexical on ties. Raises UnsupportedGlb when the vertex data cannot be decoded."""
    blob = g.binary_blob() if g.buffers else b""
    skinned = {n.mesh for n in g.nodes if n.mesh is not None and n.skin is not None}
    count: Counter = Counter()
    for mi, me in enumerate(g.meshes):
        if mi not in skinned or any(k in (me.name or "").lower() for k in _DROPPED):
            continue
        for p in me.primitives:
            joined = _joined_vertex_count(g, blob, p)
            tex = _base_tex(g, p.material)
            if joined == 0 or tex is None:
                continue
            if any(k in (g.materials[p.material].name or "").lower() for k in _VARIANT):
                return tex, _parts(g, skinned, tex)
            count[tex] += joined
    if not count:
        return None, []
    best = max(count.values())
    tex = min(t for t, c in count.items() if c == best)
    return tex, _parts(g, skinned, tex)


def _parts(g, skinned, tex):
    return [(mi, pi) for mi, me in enumerate(g.meshes) if mi in skinned and not any(k in (me.name or "").lower() for k in _DROPPED)
            for pi, p in enumerate(me.primitives) if _base_tex(g, p.material) == tex]


def opaque_mech_materials(path) -> list[str]:
    """Repair: while the engine's skin pick (skin_pick, with its 'a_' prefix) is an alpha material, set that BLEND material
    OPAQUE; the importer would otherwise look for 'a_<name>.tga', which the pack does not ship (raven_base was BLEND: flat
    white mech). The pick is recomputed after each change (the key changes, a tie can move). Other materials (canopy glass,
    decals) keep their alpha unless they win the pick. A MASK or foliage-named skin material is refused (no valid slot-0
    texture, no safe conversion). The result is not trusted: skin_texture_problems re-checks it. Returns fixed names."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(path))
    fixed: set[int] = set()
    for _ in range(len(g.materials) + 1):
        _, prims = skin_pick(g)
        mats = {g.meshes[mi].primitives[pi].material for mi, pi in prims}
        alpha = sorted(i for i in mats if _wants_alpha(g.materials[i]))
        if not alpha:
            break
        for i in alpha:
            if g.materials[i].alphaMode != "BLEND":
                raise ValueError(f"{Path(path).name}: skin material '{g.materials[i].name or i}' is {g.materials[i].alphaMode or 'named like foliage'}; the importer would look for an 'a_' texture and no OPAQUE conversion is safe")
            g.materials[i].alphaMode = "OPAQUE"
            fixed.add(i)
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
        if _wants_alpha(m):
            bad.append(f"skin material '{m.name}' is {m.alphaMode or 'named like foliage'}: the importer would look for an 'a_' texture ('{skin}')")
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
