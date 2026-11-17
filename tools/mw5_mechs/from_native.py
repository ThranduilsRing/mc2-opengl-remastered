"""Convert mw5_extract native data once: UE LH Z-up cm -> glTF RH Y-up m.

Only stdlib; synthetic tests and parity work without game files or a renderer.
"""
import argparse
import base64
import json
import math
from pathlib import Path
import struct


def basis(v, unit=1.0):
    return [unit * v[0], unit * v[2], unit * v[1]]


def rotation(q):
    # For reflection B: q'.xyz = det(B) B q.xyz; q'.w = q.w.
    n = math.sqrt(sum(x*x for x in q))
    if n < 1e-12 or not math.isfinite(n):
        raise ValueError("invalid quaternion")
    return [-q[0]/n, -q[2]/n, -q[1]/n, q[3]/n]


class Binary:
    def __init__(self, path, magic):
        self.data = Path(path).read_bytes()
        self.offset = 0
        if self.take("4sI") != (magic, 1):
            raise ValueError("unsupported native header")

    def take(self, fmt):
        size = struct.calcsize("<" + fmt)
        if self.offset + size > len(self.data):
            raise ValueError("truncated native file")
        result = struct.unpack_from("<" + fmt, self.data, self.offset)
        self.offset += size
        if any(isinstance(x, float) and not math.isfinite(x) for x in result):
            raise ValueError("non-finite native value")
        return result

    def count(self, minimum=1):
        n, = self.take("I")
        if n > (len(self.data) - self.offset) // minimum:
            raise ValueError("native count exceeds file")
        return n

    def finish(self):
        if self.offset != len(self.data):
            raise ValueError("trailing native bytes")


def read_mesh(path, bone_count):
    r = Binary(path, b"MW5M")
    nv, ni = r.take("II")
    if nv > len(r.data)//36 or ni > len(r.data)//4 or ni % 3:
        raise ValueError("invalid mesh counts")
    vertices = []
    for _ in range(nv):
        v = r.take("8f")
        influences = [r.take("If") for _ in range(r.count(8))]
        if not influences or any(b >= bone_count or w < 0 for b, w in influences):
            raise ValueError("invalid skin influences")
        total = sum(w for _, w in influences)
        if total <= 0:
            raise ValueError("zero skin weights")
        normal = basis(v[3:6])
        length = math.sqrt(sum(x*x for x in normal))
        if length < 1e-12:
            raise ValueError("zero normal")
        vertices.append((basis(v[:3], .01), [x/length for x in normal], v[6:8],
                         [(b, w/total) for b, w in influences]))
    indices = list(r.take(f"{ni}I"))
    if any(i >= nv for i in indices):
        raise ValueError("mesh index outside vertices")
    # Reflection changes triangle orientation, so reverse winding exactly once.
    for i in range(0, ni, 3):
        indices[i+1], indices[i+2] = indices[i+2], indices[i+1]
    r.finish()
    return vertices, indices


def read_clip(path, bone_count):
    r = Binary(path, b"MW5A")
    frames, fps = r.take("If")
    if frames < 1 or fps <= 0:
        raise ValueError("invalid clip rate")
    tracks = []
    seen = set()
    for _ in range(r.count(16)):
        bone, = r.take("I")
        if bone >= bone_count or bone in seen:
            raise ValueError("invalid clip bone")
        seen.add(bone)
        for name, width, transform in (("translation", 3, lambda v: basis(v, .01)),
                                       ("rotation", 4, rotation), ("scale", 3, basis)):
            keys = [r.take(f"{width+1}f") for _ in range(r.count((width+1)*4))]
            if any(k[0] < 0 for k in keys) or any(a[0] >= b[0] for a, b in zip(keys, keys[1:])):
                raise ValueError("clip times must strictly increase")
            if keys:
                values = [transform(k[1:]) for k in keys]
                if name == "rotation":
                    for a, b in zip(values, values[1:]):
                        if sum(x*y for x, y in zip(a, b)) < 0:
                            b[:] = [-x for x in b]
                tracks.append((bone, name, [k[0] for k in keys], values))
    r.finish()
    return tracks


def matrix(node):
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    t = node.get("translation", [0, 0, 0])
    rows = [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), t[0]],
            [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), t[1]],
            [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), t[2]], [0, 0, 0, 1]]
    return [[rows[i][j]*s[j] if j < 3 else rows[i][j] for j in range(4)] for i in range(4)]


def multiply(a, b):
    return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse(a):
    rows = [row[:] + [int(i == j) for j in range(4)] for i, row in enumerate(a)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda i: abs(rows[i][col]))
        rows[col], rows[pivot] = rows[pivot], rows[col]
        factor = rows[col][col]
        if abs(factor) < 1e-12:
            raise ValueError("singular bind matrix")
        rows[col] = [x/factor for x in rows[col]]
        for i in range(4):
            if i != col:
                factor = rows[i][col]
                rows[i] = [x-factor*y for x, y in zip(rows[i], rows[col])]
    return [row[4:] for row in rows]


class Gltf:
    def __init__(self):
        self.blob = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "mw5 from_native v1"},
                    "bufferViews": [], "accessors": [], "nodes": [], "meshes": [], "animations": []}

    def accessor(self, values, kind, component=5126):
        widths = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
        width = widths[kind]
        while len(self.blob) % 4:
            self.blob.append(0)
        start = len(self.blob)
        fmt = {5126: "f", 5125: "I", 5123: "H"}[component]
        for row in values:
            self.blob.extend(struct.pack("<" + fmt*width, *row))
        views, accessors = self.doc["bufferViews"], self.doc["accessors"]
        views.append({"buffer": 0, "byteOffset": start, "byteLength": len(self.blob)-start})
        a = {"bufferView": len(views)-1, "componentType": component, "count": len(values), "type": kind}
        if values:
            a.update(min=[min(v[i] for v in values) for i in range(width)],
                     max=[max(v[i] for v in values) for i in range(width)])
        accessors.append(a)
        return len(accessors)-1


def convert(source):
    source = Path(source)
    skeleton = json.loads((source/"skeleton.json").read_text())
    if skeleton.get("space") != "UE_LH_Z_UP_CM" or skeleton.get("version") != 1:
        raise ValueError("native skeleton space/version mismatch")
    bones = skeleton["bones"]
    if not bones or len({b["name"] for b in bones}) != len(bones):
        raise ValueError("empty skeleton or duplicate bone names")
    g = Gltf()
    nodes, roots, world = g.doc["nodes"], [], []
    for i, b in enumerate(bones):
        p = b["parent"]
        if not -1 <= p < i:
            raise ValueError("skeleton must have parents before children")
        node = {"name": b["name"], "translation": basis(b["translation"], .01),
                "rotation": rotation(b["rotation"]), "scale": basis(b["scale"])}
        nodes.append(node)
        if p >= 0:
            nodes[p].setdefault("children", []).append(i)
        else:
            roots.append(i)
        world.append(multiply(world[p], matrix(node)) if p >= 0 else matrix(node))
    binds = [[x for col in zip(*inverse(m)) for x in col] for m in world]
    g.doc["skins"] = [{"joints": list(range(len(bones))), "inverseBindMatrices": g.accessor(binds, "MAT4")}]
    lodfile = source/"lods.json"
    lods = json.loads(lodfile.read_text()) if lodfile.exists() else [{"file": "mesh.bin"}]
    for lod_index, lod in enumerate(lods):
        vertices, indices = read_mesh(source/lod["file"], len(bones))
        if not vertices or not indices:
            raise ValueError("empty LOD")
        attrs = {key: g.accessor([v[i] for v in vertices], kind) for i, key, kind in
                 ((0, "POSITION", "VEC3"), (1, "NORMAL", "VEC3"), (2, "TEXCOORD_0", "VEC2"))}
        for group in range((max(len(v[3]) for v in vertices)+3)//4):
            influences = [(v[3][group*4:group*4+4] + [(0, 0)]*4)[:4] for v in vertices]
            attrs[f"JOINTS_{group}"] = g.accessor([[b for b, _ in row] for row in influences], "VEC4", 5123)
            attrs[f"WEIGHTS_{group}"] = g.accessor([[w for _, w in row] for row in influences], "VEC4")
        sections = lod.get("sections", [{"firstIndex": 0, "indexCount": len(indices), "material": -1}])
        primitives = []
        for section in sections:
            start, count = section["firstIndex"], section["indexCount"]
            if start < 0 or count <= 0 or start % 3 or count % 3 or start+count > len(indices):
                raise ValueError("invalid mesh section")
            primitives.append({"attributes": attrs, "indices": g.accessor([(x,) for x in indices[start:start+count]], "SCALAR", 5125),
                               "extras": {"nativeMaterialIndex": section["material"]}})
        g.doc["meshes"].append({"primitives": primitives})
        nodes.append({"name": f"mesh_LOD{lod_index}", "mesh": lod_index, "skin": 0})
        # Additional LODs retained, but only LOD0 appears in the default scene.
        if lod_index == 0:
            roots.append(len(nodes)-1)
    sockets = json.loads((source/"sockets.json").read_text())
    bone_ids = {b["name"]: i for i, b in enumerate(bones)}
    for s in sockets:
        if s["bone"] not in bone_ids or s["name"] in bone_ids:
            # Sockets on bones the mesh lacks (the skeleton asset has more bones), and sockets that only
            # repeat a bone's name (node names must stay unique), stay in sockets.json only.
            continue
        nodes[bone_ids[s["bone"]]].setdefault("children", []).append(len(nodes))
        nodes.append({"name": s["name"], "translation": basis(s["translation"], .01),
                      "rotation": rotation(s["rotation"]), "scale": basis(s["scale"]), "extras": {"socket": True}})
    for path in sorted((source/"clips").glob("*.bin")):
        channels, samplers = [], []
        for bone, name, times, values in read_clip(path, len(bones)):
            channels.append({"sampler": len(samplers), "target": {"node": bone, "path": name}})
            samplers.append({"input": g.accessor([(t,) for t in times], "SCALAR"),
                             "output": g.accessor(values, "VEC4" if name == "rotation" else "VEC3"), "interpolation": "LINEAR"})
        meta = path.with_suffix(".json")
        name = json.loads(meta.read_text())["name"] if meta.exists() else path.stem
        if channels:
            g.doc["animations"].append({"name": name, "channels": channels, "samplers": samplers})
    g.doc.update(scene=0, scenes=[{"nodes": roots}], buffers=[{
        "byteLength": len(g.blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(g.blob).decode()}])
    g.doc["extras"] = {name: json.loads((source/f"{name}.json").read_text()) for name in ("materials", "loadout", "bridge")}
    return g.doc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    # Refuse repositories, even when output parents do not exist yet.
    if any((p/".git").exists() for p in [args.output.resolve().parent, *args.output.resolve().parents]):
        parser.error("derived assets must remain outside repositories")
    doc = convert(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(doc, allow_nan=False))
    print(f"Converted {len(doc['skins'][0]['joints'])} bones, {len(doc['animations'])} clips; basis applied once")


if __name__ == "__main__":
    main()
