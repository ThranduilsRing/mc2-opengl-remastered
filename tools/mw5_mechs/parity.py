"""PC-only numeric comparison against the umodel bridge's glTF/GLB export."""
import argparse
import base64
import json
import math
from pathlib import Path
import struct

from tools.mw5_mechs.from_native import matrix, multiply


class Asset:
    def __init__(self, path):
        path = Path(path)
        raw = path.read_bytes()
        binary = None
        if raw[:4] == b"glTF":
            magic, version, size = struct.unpack_from("<4sII", raw)
            if version != 2 or size != len(raw):
                raise ValueError("invalid GLB")
            offset = 12
            chunks = {}
            while offset < size:
                length, kind = struct.unpack_from("<II", raw, offset)
                chunks[kind] = raw[offset+8:offset+8+length]
                offset += 8+length
            self.doc = json.loads(chunks[0x4e4f534a])
            binary = chunks.get(0x004e4942)
        else:
            self.doc = json.loads(raw)
        self.buffers = []
        for buf in self.doc["buffers"]:
            uri = buf.get("uri")
            self.buffers.append(binary if uri is None else base64.b64decode(uri.split(",", 1)[1])
                                if uri.startswith("data:") else (path.parent/uri).read_bytes())

    def values(self, index):
        a = self.doc["accessors"][index]
        if "sparse" in a or a.get("normalized"):
            raise ValueError("parity needs expanded, non-normalized accessors")
        view = self.doc["bufferViews"][a["bufferView"]]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
        fmt = "<" + {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]*width
        offset = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = view.get("byteStride", struct.calcsize(fmt))
        return [struct.unpack_from(fmt, self.buffers[view["buffer"]], offset+i*stride) for i in range(a["count"])]

    def worlds(self):
        nodes = self.doc["nodes"]
        parents = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
        result = {}
        def world(i, visiting):
            if i in visiting:
                raise ValueError("cyclic glTF")
            n = nodes[i]
            m = [list(row) for row in zip(*[n["matrix"][j:j+4] for j in range(0, 16, 4)])] if "matrix" in n else matrix(n)
            return multiply(world(parents[i], visiting | {i}), m) if i in parents else m
        for i, n in enumerate(nodes):
            if "name" in n:
                if n["name"] in result:
                    raise ValueError("duplicate node name")
                result[n["name"]] = world(i, set())
        return result

    def bones(self):
        return {self.doc["nodes"][i]["name"] for s in self.doc["skins"] for i in s["joints"]}

    def vertices(self):
        # One position array may be shared by multiple material sections. Only meshes reachable from the
        # default scene count: from_native keeps extra LODs out of it, so this selects LOD0.
        nodes, seen, stack, meshes = self.doc["nodes"], set(), list(self.doc["scenes"][self.doc.get("scene", 0)]["nodes"]), set()
        while stack:
            i = stack.pop()
            if i not in seen:
                seen.add(i)
                stack += nodes[i].get("children", [])
                if "mesh" in nodes[i]:
                    meshes.add(nodes[i]["mesh"])
        ids = {p["attributes"]["POSITION"] for i in meshes for p in self.doc["meshes"][i]["primitives"]}
        return sorted(v for i in ids for v in self.values(i))

    def keys(self):
        result = {}
        for anim in self.doc.get("animations", []):
            for c in anim["channels"]:
                sampler = anim["samplers"][c["sampler"]]
                if sampler.get("interpolation", "LINEAR") != "LINEAR":
                    raise ValueError("parity needs native linear keys")
                key = (anim["name"], self.doc["nodes"][c["target"]["node"]]["name"], c["target"]["path"])
                if key in result:
                    raise ValueError("duplicate animation channel")
                result[key] = self.values(sampler["input"]), self.values(sampler["output"])
        return result


def sample(times, values, t, rotation):
    """Linear sample, held at both ends; rotations are normalised lerps on the shorter arc."""
    if t <= times[0] or len(times) == 1:
        return values[0]
    if t >= times[-1]:
        return values[-1]
    hi = next(i for i, k in enumerate(times) if k >= t)
    lo, f = hi-1, (t-times[hi-1])/(times[hi]-times[hi-1])
    x, y = values[lo], values[hi]
    if rotation and sum(i*j for i, j in zip(x, y)) < 0:
        y = [-v for v in y]
    return [i+(j-i)*f for i, j in zip(x, y)]


def match_vertices(va, vb, tolerance=1e-4):
    """One-to-one match within tolerance (two exporters round metres differently); returns the max matched distance."""
    import numpy as np
    from scipy.sparse import csr_matrix
    from scipy.sparse.csgraph import maximum_bipartite_matching
    from scipy.spatial import cKDTree
    a, b = np.asarray(va, dtype=np.float64), np.asarray(vb, dtype=np.float64)
    near = cKDTree(a).query_ball_tree(cKDTree(b), tolerance)
    rows = [i for i, row in enumerate(near) for _ in row]
    cols = [j for row in near for j in row]
    graph = csr_matrix((np.ones(len(rows)), (rows, cols)), shape=(len(a), len(b)))
    match = maximum_bipartite_matching(graph, perm_type="column")
    if len(a) != len(b) or (match < 0).any():
        raise ValueError(f"vertex positions do not match one-to-one within {tolerance}")
    return float(np.abs(a[match >= 0] - b[match[match >= 0]]).max()) if len(a) else 0.0


def compare(candidate, reference, bounds):
    a, b = Asset(candidate), Asset(reference)
    fallbacks = a.doc.get("extras", {}).get("bridge", [])
    if any(f["kind"] in {"skeletal_mesh", "animation", "skeleton_sockets", "socket"} for f in fallbacks):
        raise ValueError("candidate has geometry/animation/socket bridge fallbacks")
    if a.bones() != b.bones() or not a.bones():
        raise ValueError("bone set differs or is empty")
    wa, wb = a.worlds(), b.worlds()
    rest_error = max(abs(x-y) for name in a.bones() for ra, rb in zip(wa[name], wb[name]) for x, y in zip(ra, rb))
    if rest_error >= 1e-4:
        raise ValueError(f"bone rest error {rest_error} >= 1e-4")
    va, vb = a.vertices(), b.vertices()
    if not va or len(va) != len(vb):
        raise ValueError("vertex count differs or is empty")
    vertex_error = match_vertices(va, vb)
    ka, kb = a.keys(), b.keys()
    if not ka or {k[0] for k in ka} != {k[0] for k in kb} or not ka.keys() <= kb.keys():
        raise ValueError("clip name sets differ, or a candidate channel is missing from the reference")
    # The extractor keeps constant tracks as one key and drops rest-valued channels; the bridge writes every
    # frame of every bone. Compare the candidate sampled at each reference key time (missing channel = rest).
    rest = {n["name"]: n for n in a.doc["nodes"]}
    default = {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}
    angle = 0.0
    count = 0
    for key, (rt, rv) in kb.items():
        times, values = ka.get(key) or ([(0.0,)], [tuple(rest[key[1]].get(key[2], default[key[2]]))])
        times = [k[0] for k in times]
        rt = [k[0] for k in rt]
        for t, y in zip(rt, rv):
            count += 1
            x = sample(times, values, t, key[2] == "rotation")
            if key[2] == "rotation":
                norm = math.sqrt(sum(v*v for v in x)*sum(v*v for v in y))
                if norm == 0:
                    raise ValueError("zero quaternion")
                error = math.degrees(2*math.acos(min(1, abs(sum(i*j for i, j in zip(x, y)))/norm)))
                angle = max(angle, error)
                if error >= .1:
                    raise ValueError(f"clip key error {error} >= 0.1 deg: {key}")
            elif max(abs(i-j) for i, j in zip(x, y)) >= 1e-4:
                raise ValueError(f"clip vector key differs: {key}")
    sockets = {n["name"] for n in a.doc["nodes"] if n.get("extras", {}).get("socket")}
    if not sockets or sockets != bounds.keys():
        raise ValueError("weapon bounds must cover every socket, without extras")
    for name, box in bounds.items():
        position = [wa[name][i][3] for i in range(3)]
        if any(not lo <= v <= hi for lo, v, hi in zip(box["min"], position, box["max"])):
            raise ValueError(f"socket outside weapon bounds: {name}")
    return {"status": "PASS", "bones": len(a.bones()), "keys": count,
            "rest_max": rest_error, "vertices": len(va), "vertex_max": vertex_error, "rotation_max_degrees": angle, "sockets": len(sockets),
            "skipped_sockets": sum(f["kind"] == "socket_skipped" for f in fallbacks),
            "other_bridge_fallbacks": len(fallbacks)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--candidate", required=True, type=Path)
    p.add_argument("--reference", required=True, type=Path)
    p.add_argument("--weapon-bounds", required=True, type=Path)
    args = p.parse_args()
    print(json.dumps(compare(args.candidate, args.reference, json.loads(args.weapon_bounds.read_text()))))


if __name__ == "__main__":
    main()
