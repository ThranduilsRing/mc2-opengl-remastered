import struct
import unittest

import numpy as np

from tools.mapgen.move_writer import build_move, validate_move, distances, global_packets


class MoveWriterTest(unittest.TestCase):
    def test_sector_split_and_blocked_cells(self):
        cells = np.full((60, 60), 0x400, dtype='<u4')
        cells[1:29, 15] = 0
        packets = build_move(cells)
        result = validate_move(packets, 60)
        self.assertEqual(result[0]['areas'], 4)
        self.assertEqual(result[0], result[1])
        self.assertEqual(result[2]['areas'], 4)

    def test_corrupt_dimension_and_link_pool(self):
        packets = build_move(np.full((60, 60), 0x400, dtype='<u4'))
        corrupt = packets.copy()
        corrupt[4+9] = struct.pack('<i', 0)
        with self.assertRaisesRegex(ValueError, 'link pool'):
            validate_move(corrupt, 60)
        corrupt = packets.copy()
        corrupt[0] = struct.pack('<i', 90)
        with self.assertRaisesRegex(ValueError, 'dimensions'):
            validate_move(corrupt, 60)

    def test_invalid_sector_size(self):
        with self.assertRaisesRegex(ValueError, 'divisible'):
            build_move(np.full((61, 61), 0x400, dtype='<u4'))

    def test_reader_dimension_limit(self):
        with self.assertRaisesRegex(ValueError, '3072'):
            global_packets(np.zeros((3090, 3090), dtype=bool))

    def test_reader_area_limit(self):
        mask = np.zeros((360, 360), dtype=bool)
        mask[::2, ::2] = True  # 32,400 isolated areas, beyond the reader's 30,000.
        with self.assertRaisesRegex(ValueError, 'areas'):
            global_packets(mask)

    def test_stock_link_cost_fixtures(self):
        # A central sector with single-cell west/east doors at known endpoints.
        # Read the serialized links directly; do not rely on validate_move.
        for start_row, goal_row, obstacle, expected in (
                (40, 40, False, 290), (30, 59, False, 435),
                (40, 40, True, 300)):
            with self.subTest(start=start_row, goal=goal_row, obstacle=obstacle):
                mask = np.zeros((90, 90), dtype=bool)
                mask[30:60, 30:60] = True
                mask[start_row, 29] = mask[goal_row, 60] = True
                if obstacle:
                    mask[40, 44:46] = False
                packets = global_packets(mask)
                # Three areas, each with door info, then two doors + scratch.
                self.assertEqual(struct.unpack('<i', packets[6])[0], 3)
                self.assertEqual(struct.unpack('<i', packets[7])[0], 2)
                for packet in (packets[17], packets[18]):
                    _, _, cost, open_cost = struct.unpack('<hbii', packet[:11])
                    self.assertEqual((cost, open_cost), (expected, expected))

        # One open flank permits a diagonal; two blocked flanks forbid it.
        area = np.zeros((3, 3), dtype=int)
        area[0, 1] = -1
        self.assertEqual(distances(area, (0, 0), 0)[1, 1], 15)
        area[1, 0] = -1
        self.assertNotIn((1, 1), distances(area, (0, 0), 0))
