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
        # One position array may be shared by multiple material sections.
        ids = {p["attributes"]["POSITION"] for m in self.doc["meshes"] for p in m["primitives"]}
        # Reference and candidate must select LOD0 only for this proof.
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


def compare(candidate, reference, bounds):
    a, b = Asset(candidate), Asset(reference)
    if a.doc.get("extras", {}).get("bridge"):
        raise ValueError("candidate has bridge fallbacks; full parity cannot pass")
    if a.bones() != b.bones() or not a.bones():
        raise ValueError("bone set differs or is empty")
    wa, wb = a.worlds(), b.worlds()
    rest_error = max(abs(x-y) for name in a.bones() for ra, rb in zip(wa[name], wb[name]) for x, y in zip(ra, rb))
    if rest_error >= 1e-4:
        raise ValueError(f"bone rest error {rest_error} >= 1e-4")
    if not a.vertices() or a.vertices() != b.vertices():
        raise ValueError("sorted vertex set differs")
    ka, kb = a.keys(), b.keys()
    if not ka or ka.keys() != kb.keys():
        raise ValueError("clip/channel set differs or is empty")
    angle = 0.0
    count = 0
    for key, (times, values) in ka.items():
        rt, rv = kb[key]
        if times != rt or len(values) != len(rv):
            raise ValueError(f"key times/count differ: {key}")
        for x, y in zip(values, rv):
            count += 1
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
            "rest_max": rest_error, "rotation_max_degrees": angle, "sockets": len(sockets)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--candidate", required=True, type=Path)
    p.add_argument("--reference", required=True, type=Path)
    p.add_argument("--weapon-bounds", required=True, type=Path)
    args = p.parse_args()
    print(json.dumps(compare(args.candidate, args.reference, json.loads(args.weapon_bounds.read_text()))))


if __name__ == "__main__":
    main()
