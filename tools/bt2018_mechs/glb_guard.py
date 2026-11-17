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


def validate_pack_glbs(root) -> dict[str, list[str]]:
    """{relative path: problems} for every bad *.glb under root."""
    root = Path(root)
    out = {}
    for p in sorted(root.rglob("*.glb")):
        bad = validate_glb(p)
        if bad:
            out[p.relative_to(root).as_posix()] = bad
    return out
