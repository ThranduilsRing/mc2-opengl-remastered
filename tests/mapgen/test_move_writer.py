import struct
import unittest

import numpy as np

from tools.mapgen.move_writer import build_move, validate_move


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
