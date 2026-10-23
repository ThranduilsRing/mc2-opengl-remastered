import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from PIL import Image

from tools.mapgen.common import load_json, mask_of
from tools.mapgen.extract import VERTEX
from tools.mapgen.generate import generate
from tools.mapgen.mission_writer import collision_grid, place_units, snapped, terrain_packet
from tools.mapgen.validate import validate, validate_footprints
from tools.mapgen.suburban_forest import cook_colormap


class SuburbanForestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = generate(180, 7, biome='suburban_forest')
        cls.profile = load_json('tools/mapgen/profile.json')

    def test_stock_validator_and_bounded_parcels(self):
        self.assertEqual(validate(self.layout, self.profile), [])
        trees = [o['type_name'].lower() for o in self.layout['objects'] if o['class'] == 'tree']
        self.assertTrue(trees)
        self.assertTrue(all(n.startswith(('birch', 'oak', 'maple', 'pine', 'willow'))
                            and 'dead' not in n for n in trees))
        ids = np.asarray(self.layout['regions']['parcel_ids'])
        for parcel in self.layout['regions']['parcels']:
            r0, c0, r1, c1 = parcel['bounds']
            rr, cc = np.where(ids == parcel['id'])
            self.assertTrue(len(rr))
            self.assertTrue(np.all((rr >= r0) & (rr < r1) & (cc >= c0) & (cc < c1)))
            self.assertLessEqual(max(r1-r0, c1-c0), 41)
        for feature in ('forest_core', 'forest_edge', 'clearings', 'river_banks',
                        'water', 'field_borders', 'verge', 'shoulder', 'ditch'):
            self.assertTrue(mask_of(self.layout, feature).any(), feature)
        zones = mask_of(self.layout, 'objective_zones')
        self.assertTrue(all(zones[tuple(m['cell'])] for m in self.layout['objectives']))

    def test_full_native_canopy_exclusion_rejects_corruption(self):
        layout = copy.deepcopy(self.layout)
        tree = next(o for o in layout['objects'] if o['class'] == 'tree')
        r, c = tree['cell']
        layout['layers']['vegetation_exclusion'].append([r, c, c+1])
        self.assertTrue(any('shared vegetation exclusion' in e for e in validate_footprints(layout)))

    def test_water_packets_and_causeway_pathing(self):
        layout = snapped(self.layout)
        wet = mask_of(layout, 'water')
        self.assertFalse((wet & mask_of(layout, 'roads')).any())
        blocked = collision_grid(layout)
        self.assertTrue(blocked[wet].all())
        self.assertTrue(place_units(layout, blocked))  # also checks objective approaches
        source = np.zeros(1, VERTEX)
        source['type'] = 2
        source['texture'] = 0xffff0000
        with patch('tools.mapgen.mission_writer.packet', return_value=source.tobytes()):
            vertices = np.frombuffer(terrain_packet(layout, b''), VERTEX).reshape(60, 60)
        np.testing.assert_array_equal(vertices['water'] & 1, wet[::3, ::3])
        np.testing.assert_array_equal(vertices['height'], np.asarray(layout['terrain']['elevation_wu'])[::3, ::3])
        self.assertGreater(np.unique(vertices['type']).size, 3)

    def test_preset_rejects_ambiguous_inputs(self):
        with self.assertRaisesRegex(ValueError, 'owns terrain'):
            generate(180, 7, np.full((180, 180), 1000.), biome='suburban_forest')
        with self.assertRaisesRegex(ValueError, 'divisible by 60'):
            generate(210, 7, biome='suburban_forest')

    def test_seed_reproduces_authoring_and_native_placement(self):
        self.assertEqual(self.layout, generate(180, 7, biome='suburban_forest'))

    def test_colormap_ships_lit_sidecar_for_ordinary_loader(self):
        lighting = dict(lightdirpitch=50, lightdiryaw=11,
                        lightred=0, lightgreen=0, lightblue=0,
                        ambientred=128, ambientgreen=128, ambientblue=128)
        with tempfile.TemporaryDirectory() as folder:
            path = cook_colormap(self.layout, Path(folder)/'mission.tga', lighting)
            with Image.open(path) as raw, Image.open(path.with_suffix('.burnin.tga')) as baked:
                self.assertEqual(raw.size, (1024, 1024))
                np.testing.assert_array_equal(np.asarray(baked),
                    (np.asarray(raw)* (128/255)).astype(np.uint8))
