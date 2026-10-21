"""Format regressions: synthetic bytes, no dependency on private installs."""
import struct
import unittest
import zlib

import numpy as np

from tools.mapgen.common import mask_of, packed_cell, spans, world_cell
from tools.mapgen.data import fit_sections, packet
from tools.mapgen.extract import OBJECT, VERTEX, extract_mission


def pak(*contents, compressed=False):
    offset = 8 + 4 * len(contents)
    entries, payload = [], bytearray()
    for content in contents:
        raw = struct.pack("<I", len(content)) + zlib.compress(content) if compressed else content
        entries.append(offset | ((4 if compressed else 0) << 29))
        offset += len(raw)
        payload.extend(raw)
    return struct.pack("<II", 0xFEEDFACE, 8+4*len(contents)) + struct.pack(f"<{len(entries)}I", *entries) + payload


class MemoryInstall:
    def __init__(self, files):
        self.files = files

    def read(self, name):
        return self.files[name], "fixture::" + name


class ExtractionTests(unittest.TestCase):
    def test_packet_seek_table_and_final_packet(self):
        for compressed in (False, True):
            raw = pak(b"terrain", b"objects", compressed=compressed)
            self.assertEqual(packet(raw, 0), b"terrain")
            self.assertEqual(packet(raw, 1), b"objects")
            with self.assertRaises(ValueError):
                packet(raw, 2)
        damaged = bytearray(pak(b"a", compressed=True))
        damaged[12:16] = struct.pack("<I", 99)
        with self.assertRaises(ValueError):
            packet(damaged, 0)

    def test_xy_cell_math_and_water_low_bit(self):
        self.assertEqual(world_cell(-128, 128, 2), [0, 0])
        self.assertEqual(world_cell(0, 0, 2), [3, 3])
        self.assertEqual(OBJECT.size, 40)
        self.assertEqual(VERTEX.itemsize, 32)
        vertices = np.zeros((2, 2), dtype=VERTEX)
        vertices["water"] = [[0x40, 0x80], [0x41, 0]]
        vertices["type"][0, 0] = 10
        vertices["texture"][0, 1] = 255 << 16
        records = [OBJECT.pack(1, -100, 100, 0, 90, 0, 1, -1, 0, 0),
                   OBJECT.pack(2, 0, 0, 9000, 180, 0, 1, packed_cell([0, 0]), 0, 0)]
        install = MemoryInstall({"data/missions/test.pak": pak(vertices.tobytes(), struct.pack("<i", 2)+b"".join(records)),
                                 "data/missions/test.fit": b"[Part1]\nf PositionX=0\nf PositionY=0\nb PlayerPart=TRUE\n"})
        result = extract_mission(install, "test", {1: {"class": "turret_control", "type_name": "control"},
            2: {"class": "turret", "type_name": "turret"}}, {}, {255: {"id": 0}}, None, "fixture")
        self.assertEqual(result["objects"][1]["cell"], [3, 3])
        self.assertEqual(result["objects"][1]["parent_link"], "object_0")
        self.assertEqual(int(mask_of(result, "water").sum()), 9)
        self.assertEqual(int(mask_of(result, "roads").sum()), 9)
        self.assertEqual(result["player_start"], [3, 3])

    def test_fit_scalars_and_span_roundtrip(self):
        fit = fit_sections('[Obj]\r\nst Name="Airfield"\r\nl TeamID=-1\r\nb Active=TRUE\r\nf X=2.5\r\n')
        self.assertEqual(fit["obj"], {"name": "Airfield", "teamid": -1, "active": True, "x": 2.5})
        raster = np.array([[True, False, True], [False, True, True]])
        layout = {"size": {"rows": 2, "cols": 3}, "layers": {"roads": spans(raster)}}
        np.testing.assert_array_equal(mask_of(layout, "roads"), raster)


if __name__ == "__main__":
    unittest.main()
