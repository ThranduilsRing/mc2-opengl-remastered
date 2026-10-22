"""Disk-layout, coordinate, fallback and pivot regression fixtures."""
import struct
import unittest

import numpy as np

from tools.mapgen.footprints import ase_points, hull, measure, radius_footprint, tgl_points


def node(name, vertices, centre=(0, 0, 0)):
    data = struct.pack("<II", 1, len(vertices))
    for vertex in vertices:
        data += struct.pack("<6fI", *vertex, 0, 1, 0, 0xFFFFFFFF)
    data += struct.pack("<II6f", 0, 0, 0, 0, 0, *centre)
    return data + name.encode().ljust(25, b"\0") + bytes(25)


class FootprintTests(unittest.TestCase):
    def test_tgl_relative_centres_axis_mapping_and_helper_exclusion(self):
        raw = struct.pack("<III", 0xBAFDECAF, 2, 0)
        raw += node("building", [(-10, 0, -20), (30, 50, 40)], (5, 10, 15))
        raw += node("LOS_helper", [(-900, -900, -900), (900, 900, 900)])
        result = measure(tgl_points(raw))
        self.assertEqual(result["bounds_wu"], [[-5, -5, 10], [35, 55, 60]])
        self.assertEqual([result[k] for k in ("width_wu", "depth_wu", "height_wu")], [40, 60, 50])
        for invalid in (raw[:-1], bytes(12), raw+b"trailing"):
            with self.assertRaises(ValueError):
                tgl_points(invalid)

    def test_ase_absolute_vertices_and_helper_exclusion(self):
        raw = b'''*GEOMOBJECT { *NODE_NAME "house"
        *MESH_VERTEX 0 -10 20 30
        *MESH_VERTEX 1 40 -50 60
        } *GEOMOBJECT { *NODE_NAME "SpotLight_1"
        *MESH_VERTEX 0 999 999 999 }'''
        np.testing.assert_array_equal(ase_points(raw), [[10,20,30],[-40,-50,60]])

    def test_fallback_never_calls_unknown_dimensions_real(self):
        self.assertEqual(radius_footprint(12)["width_wu"], 24)
        self.assertEqual(radius_footprint(0)["method"], "unresolved")
        self.assertEqual(len(hull([[0,0],[1,0],[1,1],[0,1],[.5,.5]])), 4)


if __name__ == "__main__":
    unittest.main()
