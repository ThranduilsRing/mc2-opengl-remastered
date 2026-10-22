import copy
import unittest

import numpy as np

from tools.mapgen.generate import generate
from tools.mapgen.mission_writer import snapped, collision_grid, place_units


class MissionWriterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = generate(180, 7, np.full((180,180),1000.))

    def test_parent_repacking_and_collision_free_spawns(self):
        layout = snapped(self.layout)
        by_id = {o['id']:o for o in layout['objects']}
        for obj in layout['objects']:
            if obj['parent_link']:
                self.assertEqual(obj['parent_cell'], by_id[obj['parent_link']]['cell'])
        blocked = collision_grid(layout)
        units = place_units(layout,blocked)
        self.assertEqual(sum(u['player'] for u in units),3)
        self.assertTrue(all(not blocked[tuple(u['cell'])] for u in units))

    def test_reject_nonflat_and_missing_parent(self):
        layout = copy.deepcopy(self.layout)
        layout['terrain']['elevation_wu'][0][0] += 10
        with self.assertRaisesRegex(ValueError,'flat dry'):
            snapped(layout)
        layout = copy.deepcopy(self.layout)
        next(o for o in layout['objects'] if o['class']=='turret')['parent_link'] = 'missing'
        with self.assertRaisesRegex(ValueError,'parent chain'):
            snapped(layout)

    def test_reject_duplicate_cell(self):
        layout = copy.deepcopy(self.layout)
        layout['objects'][1]['position_wu'] = layout['objects'][0]['position_wu'][:]
        with self.assertRaisesRegex(ValueError,'ambiguous cell'):
            snapped(layout)

    def test_reject_incomplete_object_blocks_and_mismatched_vertices(self):
        for side in (210, 270):
            with self.subTest(side=side):
                layout = copy.deepcopy(self.layout)
                layout['size'].update(rows=side, cols=side, vertices=side // 3)
                with self.assertRaisesRegex(ValueError, 'divisible by 60'):
                    snapped(layout)
        layout = copy.deepcopy(self.layout)
        layout['size']['vertices'] += 20
        with self.assertRaisesRegex(ValueError, 'size.vertices'):
            snapped(layout)
