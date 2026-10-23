import unittest

import numpy as np

from tools.mapgen.ascii import render
from tools.mapgen.common import distance_field, spans


class PreviewTests(unittest.TestCase):
    def test_priority_crop_and_north_up(self):
        layout = {"name": "test", "source": {"corpus": "test"},
                  "size": {"rows": 4, "cols": 4}, "layers": {"roads": [[0, 0, 4]]},
                  "objects": [{"class": "turret", "cell": [0, 1], "team": 1}],
                  "player_start": [3, 0], "objectives": []}
        text = render(layout, 1)
        self.assertIn("|=T==|", text)
        self.assertIn("|@   |", text)
        self.assertIn("|T=|", render(layout, 1, [0, 1, 2, 3]))
        with self.assertRaises(ValueError):
            render(layout, 0)

    def test_distance_transform_matches_exhaustive(self):
        raster = np.zeros((11, 13), bool)
        targets = [(2, 1), (8, 11), (5, 6)]
        for r, c in targets:
            raster[r, c] = True
        measured = distance_field(raster)
        expected = [[min(max(abs(r-rr), abs(c-cc)) for rr, cc in targets)
                     for c in range(13)] for r in range(11)]
        np.testing.assert_array_equal(measured, expected)
