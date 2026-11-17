"""Trigger paths: tools/mw5_kit/extract/** and tools/mw5_mechs/**.

Run on box after production C# --fixture; no MW5 data used.
"""
import copy
import base64
import json
import math
import os
import shutil
from pathlib import Path
import struct
import tempfile
import unittest

from tools.mw5_mechs.from_native import basis, convert, inverse, matrix, multiply, read_clip, read_mesh, rotation
from tools.mw5_mechs.parity import Asset, compare, match_vertices


@unittest.skipUnless(os.environ.get("MW5_SYNTHETIC_FIXTURE"), "requires C# --fixture output; see tools/mw5_kit/README.md")
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = Path(os.environ["MW5_SYNTHETIC_FIXTURE"])
        cls.doc = convert(cls.fixture)

    def test_rest_keys_sockets_share_one_basis(self):
        nodes = self.doc["nodes"]
        self.assertEqual(nodes[0]["translation"], [1, 3, 2])
        self.assertEqual(nodes[1]["translation"], [0, 1, 0])
        self.assertEqual(nodes[-1]["translation"], [0, .5, 0])
        tracks = read_clip(self.fixture/"clips/turn.bin", 2)
        self.assertEqual(tracks[0][2], [0, 1])
        self.assertEqual(tracks[0][3], [[0, 1, 0], [0, 2, 0]])
        self.assertAlmostEqual(tracks[1][3][-1][1], -math.sqrt(.5))

    def test_basis_rotation_equivariance(self):
        # Independent matrix action verifies quaternion conjugation including reflection.
        q = [.2, .3, .4, math.sqrt(.71)]
        v = [3, -7, 2]
        a, b = matrix({"rotation": q}), matrix({"rotation": rotation(q)})
        apply = lambda m, x: [sum(m[i][j]*x[j] for j in range(3)) for i in range(3)]
        for x, y in zip(basis(apply(a, v)), apply(b, basis(v))):
            self.assertAlmostEqual(x, y)

    def test_winding_skin_uv_and_normal(self):
        vertices, indices = read_mesh(self.fixture/"mesh.bin", 2)
        self.assertEqual(indices, [0, 2, 1])
        self.assertEqual(vertices[1], ([1, 0, 0], [0, 1, 0], (1, 0), [(0, .25), (1, .75)]))

    def test_inverse_bind_with_rotated_scaled_parent(self):
        a = matrix({"translation": [1, 2, 3], "rotation": rotation([.2, .3, .4, math.sqrt(.71)]), "scale": [2, 3, 4]})
        b = matrix({"translation": [4, 5, 6]})
        world = multiply(a, b)
        for i, row in enumerate(multiply(world, inverse(world))):
            for j, value in enumerate(row):
                self.assertAlmostEqual(value, int(i == j))

    def test_binary_rejects_corruption(self):
        raw = (self.fixture/"mesh.bin").read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"mesh.bin"
            for data in (raw[:30], raw+b"x", b"BAD!"+raw[4:], raw[:8]+struct.pack("<I", 0xffffffff)+raw[12:]):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    read_mesh(path, 2)
            path.write_bytes(raw)
            with self.assertRaises(ValueError):
                read_mesh(path, 1)

    def test_gltf_accessors_and_bind_matrices(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/"model.gltf"
            p.write_text(json.dumps(self.doc))
            a = Asset(p)
            self.assertEqual(a.vertices(), [(0, 0, 0), (0, 0, 1), (1, 0, 0)])
            bind = a.values(self.doc["skins"][0]["inverseBindMatrices"])[1]
            self.assertEqual(bind[12:15], (-1, -4, -2))

    def test_parity_accepts_sign_equivalent_keys_and_rejects_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, ref = Path(tmp)/"a.gltf", Path(tmp)/"b.gltf"
            p.write_text(json.dumps(self.doc))
            ref.write_text(json.dumps(self.doc))
            bounds = {"muzzle": {"min": [0, 4, 1], "max": [2, 5, 3]}}
            self.assertEqual(compare(p, ref, bounds)["status"], "PASS")
            # Reverse quaternion sign in the reference binary: same rotation.
            bad = copy.deepcopy(self.doc)
            sampler = bad["animations"][0]["samplers"][1]
            accessor = bad["accessors"][sampler["output"]]
            view = bad["bufferViews"][accessor["bufferView"]]
            data = bytearray(base64.b64decode(bad["buffers"][0]["uri"].split(",")[1]))
            offset = view["byteOffset"] + 16
            q = struct.unpack_from("<4f", data, offset)
            struct.pack_into("<4f", data, offset, *[-x for x in q])
            bad["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(data).decode()
            ref.write_text(json.dumps(bad))
            self.assertEqual(compare(p, ref, bounds)["status"], "PASS")
            struct.pack_into("<4f", data, offset, 0, 0, 0, 1)
            bad["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(data).decode()
            ref.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError, "clip key error"):
                compare(p, ref, bounds)
            bad = copy.deepcopy(self.doc)
            bad["nodes"][1]["translation"][1] += .01
            ref.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError, "bone rest"):
                compare(p, ref, bounds)
            ref.write_text(json.dumps(self.doc))
            with self.assertRaisesRegex(ValueError, "outside weapon"):
                compare(p, ref, {"muzzle": {"min": [0, 0, 0], "max": [1, 1, 1]}})
            with self.assertRaisesRegex(ValueError, "cover every socket"):
                compare(p, ref, {})

    def test_zero_scale_is_authored_data(self):
        # A zero scale key that the extractor wrote is data; absent scale is an empty channel (no track).
        def clip(scale_keys):
            body = struct.pack("<4sIIfI", b"MW5A", 1, 2, 30, 1) + struct.pack("<I", 0)
            body += struct.pack("<I", 0) + struct.pack("<I", 0)  # empty translation and rotation
            return body + struct.pack("<I", len(scale_keys)) + b"".join(struct.pack("<4f", 0, *k) for k in scale_keys)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"c.bin"
            path.write_bytes(clip([(0, 0, 0)]))
            self.assertEqual([(t[1], t[3]) for t in read_clip(path, 1)], [("scale", [[0, 0, 0]])])
            path.write_bytes(clip([]))
            self.assertEqual(read_clip(path, 1), [])

    def test_missing_clip_fails_parity(self):
        bounds = {"muzzle": {"min": [0, 4, 1], "max": [2, 5, 3]}}
        with tempfile.TemporaryDirectory() as tmp:
            p, ref = Path(tmp)/"a.gltf", Path(tmp)/"b.gltf"
            bad = copy.deepcopy(self.doc)
            bad["animations"] = []
            p.write_text(json.dumps(bad))
            ref.write_text(json.dumps(self.doc))
            with self.assertRaisesRegex(ValueError, "clip name sets differ"):
                compare(p, ref, bounds)
            two = copy.deepcopy(self.doc)
            two["animations"].append({**two["animations"][0], "name": "extra"})
            ref.write_text(json.dumps(two))
            p.write_text(json.dumps(self.doc))
            with self.assertRaisesRegex(ValueError, "clip name sets differ"):
                compare(p, ref, bounds)

    def test_sockets_keep_distinct_nodes_or_are_counted(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)/"n"
            shutil.copytree(self.fixture, src)
            sockets = [{"name": n, "bone": b, "translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}
                       for n, b in (("muzzle", "weapon"), ("root", "weapon"), ("lost", "gone"))]
            (src/"sockets.json").write_text(json.dumps(sockets))
            doc = convert(src)
            names = [n["name"] for n in doc["nodes"]]
            self.assertEqual(len(names), len(set(names)))
            self.assertIn("muzzle", names)
            self.assertIn("root_socket", names)
            self.assertNotIn("lost", names)
            skipped = [f for f in doc["extras"]["bridge"] if f["kind"] == "socket_skipped"]
            self.assertEqual([f["asset"] for f in skipped], ["lost"])
            p = Path(tmp)/"a.gltf"
            p.write_text(json.dumps(doc))
            box = {"min": [-9, -9, -9], "max": [9, 9, 9]}
            result = compare(p, p, {"muzzle": box, "root_socket": box})
            self.assertEqual((result["skipped_sockets"], result["sockets"]), (1, 2))


class VertexMatchTests(unittest.TestCase):
    def test_one_to_one_multiplicity(self):
        a, b = (0, 0, 0), (1, 0, 0)
        self.assertEqual(match_vertices([a, a, b], [b, a, a]), 0)
        self.assertLess(match_vertices([a, b], [(0, 0, 5e-5), b]), 1e-4)
        for bad in (([a, a, b], [a, b, b]), ([a, b], [a, (1, 0, 1e-3)]), ([a], [a, a])):
            with self.assertRaises(ValueError):
                match_vertices(*bad)


if __name__ == "__main__":
    unittest.main()
