"""Trigger paths: tools/mw5_kit/extract/** and tools/mw5_mechs/**.

Run on box after production C# --fixture; no MW5 data used.
"""
import copy
import json
import math
import os
from pathlib import Path
import struct
import tempfile
import unittest

from tools.mw5_mechs.from_native import basis, convert, inverse, matrix, multiply, read_clip, read_mesh, rotation
from tools.mw5_mechs.parity import Asset, compare


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


if __name__ == "__main__":
    unittest.main()
