"""Seed sweeps, corruption checks, supplied terrain and process determinism."""
import copy
import hashlib
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np

from tools.mapgen.analyze import aggregate, measure
from tools.mapgen.common import load_json, mask_of, spans
from tools.mapgen.generate import PROFILE_PATH, generate, route
from tools.mapgen.validate import validate
from tools.mapgen.scale_metrics import measure_scale

ROOT = Path(__file__).resolve().parents[2]


class GeneratedLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = load_json(PROFILE_PATH)
        cls.example = generate(180, 42, profile=cls.profile)

    def test_seed_sweep(self):
        for seed in range(24):
            size = (96, 128, 180, 300)[seed % 4]
            with self.subTest(size=size, seed=seed):
                layout = generate(size, seed, profile=self.profile)
                self.assertEqual(validate(layout, self.profile), [])

    def test_large_1024_cell_layout(self):
        layout = generate(1024, 42, profile=self.profile)
        self.assertEqual(layout["size"]["rows"], 1024)
        self.assertGreater(len(layout["bases"]), 20)
        self.assertEqual(validate(layout, self.profile), [])

        metrics = measure_scale(layout)
        # The authored cluster rule omits lone outposts; use all planned sites
        # for this placement regression, including the tiny end of the hierarchy.
        self.assertGreater(metrics['parcel_nn_cv'], .4)
        self.assertGreater(metrics['empty_land_fraction'], .70)
        self.assertGreater(metrics['parcel_area_max_min'], 25)
        self.assertGreater(metrics['tree_bin_cv'], .8)
        self.assertGreater(metrics['tiny_site_fraction'], .4)
        self.assertGreater(metrics['largest_site_structures'], 70)
        self.assertEqual(metrics['road_largest_fraction'], 1)
        self.assertEqual({b['region'] for b in layout['bases']} - {'wilderness'},
                         {'farmland','urban_core','industrial','military'})

        # A diagonal may only cross terrain reserved by the routed network.
        road = next(r for r in layout['roads_wu'] if any(a[0]!=b[0] and a[1]!=b[1]
                    for a,b in zip(r['points'],r['points'][1:])))
        road['points'] = [[-19000.,19000.],[-18000.,18000.]]
        self.assertTrue(any('unrouted diagonal' in e for e in validate(layout,self.profile)))

    def test_1024_vertex_domain_reaches_terrain_validation(self):
        # This boundary must accept 3072 cells before checking the supplied land.
        # Successful full-size generation/validation is also exercised by the
        # committed 3072-cell example, without repeating that expensive bake here.
        shape = (3072,3072)
        with self.assertRaisesRegex(ValueError,"connected dry flat land"):
            generate(3072,42,np.ones(shape),np.ones(shape,dtype=bool),self.profile)

    def test_parent_chain_corruptions_are_detected(self):
        broken = copy.deepcopy(self.example)
        turret = next(o for o in broken["objects"] if o["class"] == "turret")
        turret["parent_link"] = "missing"
        self.assertTrue(any("missing turret_control parent" in e for e in validate(broken, self.profile)))
        broken = copy.deepcopy(self.example)
        control = next(o for o in broken["objects"] if o["class"] == "turret_control")
        control["parent_cell"] = [1, 1]
        self.assertTrue(any("packed cell disagree" in e for e in validate(broken, self.profile)))
        broken = copy.deepcopy(self.example)
        generator = next(o for o in broken["objects"] if o["class"] == "turret_generator")
        generator["team"] = 3
        self.assertTrue(any("another team" in e for e in validate(broken, self.profile)))

    def test_gate_requires_wall_and_road_crossing(self):
        broken = copy.deepcopy(self.example)
        gate = next(o for o in broken["objects"] if o["class"] == "gate")
        gate["cell"] = [1, 1]
        errors = validate(broken, self.profile)
        self.assertTrue(any("gate is not between" in e for e in errors))
        self.assertTrue(any("gate is not on a road crossing" in e for e in errors))
        self.assertTrue(any("ring is incomplete" in e for e in errors))

    def test_water_slope_and_road_breaks_are_detected(self):
        broken = copy.deepcopy(self.example)
        obj = next(o for o in broken["objects"] if o["class"] == "building")
        water = mask_of(broken, "water")
        water[tuple(obj["cell"])] = True
        broken["layers"]["water"] = spans(water)
        self.assertTrue(any("on water" in e for e in validate(broken, self.profile)))
        broken = copy.deepcopy(self.example)
        r,c = obj["cell"]
        broken["terrain"]["elevation_wu"][r+1][c] += 1000
        self.assertTrue(any("flatness limit" in e for e in validate(broken, self.profile)))
        broken = copy.deepcopy(self.example)
        road = mask_of(broken, "roads")
        road[0:5, :] = False
        road[0, 0] = True
        broken["layers"]["roads"] = spans(road)
        self.assertTrue(any("components" in e for e in validate(broken, self.profile)))

    def test_chain_range_and_corpus_envelope_checks(self):
        broken = copy.deepcopy(self.example)
        turret = next(o for o in broken["objects"] if o["class"] == "turret")
        parent = next(o for o in broken["objects"] if o["id"] == turret["parent_link"])
        turret["cell"] = max(([0,0], [0,179], [179,0], [179,179]),
                             key=lambda cell: sum((a-b)**2 for a,b in zip(cell,parent["cell"])))
        self.assertTrue(any("outside measured range" in e for e in validate(broken, self.profile)))
        broken = copy.deepcopy(self.example)
        broken["bases"][0]["archetype"] = "turret_battery"
        broken["bases"][0]["bounds"][0] = 0
        broken["bases"][0]["bounds"][2] = broken["size"]["rows"]-1
        errors = validate(broken, self.profile)
        self.assertTrue(any("outside corpus range" in e for e in errors))

    def test_supplied_terrain_is_preserved_and_obstacles_avoided(self):
        size = 180
        rr, cc = np.mgrid[:size, :size]
        height = 160 + rr*.15 + cc*.1
        height[:55, 120:] += 650  # Impassable partial escarpment, route around it.
        water = (rr-95)**2 + (cc-90)**2 < 17**2
        original, original_water = height.copy(), water.copy()
        layout = generate(size, 71, height, water, self.profile)
        np.testing.assert_array_equal(height, original)
        np.testing.assert_array_equal(water, original_water)
        np.testing.assert_allclose(layout["terrain"]["elevation_wu"], height, atol=.00051)
        self.assertEqual(validate(layout, self.profile), [])

    def test_impossible_terrain_and_bounds_fail_clearly(self):
        with self.assertRaisesRegex(ValueError, "connected dry flat land"):
            generate(96, 1, np.ones((96,96)), np.ones((96,96),bool), self.profile)
        with self.assertRaisesRegex(ValueError, "96 and 3072"):
            generate(3073, 1, profile=self.profile)
        with self.assertRaisesRegex(ValueError, "finite"):
            generate(96, 1, np.full((96,96), np.nan), profile=self.profile)
        with self.assertRaisesRegex(ValueError, "finite"):
            generate(96, 1, np.ones((97,96)), profile=self.profile)

    def test_routing_requires_a_real_dry_corridor(self):
        allowed = np.ones((8,8),bool)
        allowed[:,4] = False
        with self.assertRaisesRegex(ValueError, "no flat dry road route"):
            route((1,1), (1,6), allowed)
        allowed[6,4] = True
        path = route((1,1), (1,6), allowed)
        self.assertIn((6,4), path)
        self.assertTrue(all(allowed[cell] for cell in path))

    def test_determinism_across_hash_seeds_and_processes(self):
        code = ("import hashlib,json; from tools.mapgen.generate import generate; "
                "print(hashlib.sha256(json.dumps(generate(128,71),sort_keys=True).encode()).hexdigest())")
        digests = []
        for hash_seed in ("1", "993"):
            env = dict(os.environ, PYTHONHASHSEED=hash_seed)
            result = subprocess.run([sys.executable, "-B", "-c", code], cwd=ROOT,
                                    env=env, text=True, capture_output=True, check=True)
            digests.append(result.stdout.strip())
        self.assertEqual(digests[0], digests[1])
        another = generate(128,72,profile=self.profile)
        digest = hashlib.sha256(json.dumps(another,sort_keys=True).encode()).hexdigest()
        self.assertNotEqual(digests[0],digest)

    def test_population_matches_measured_corpus_envelopes(self):
        population = [measure(generate(300, seed, profile=self.profile)) for seed in range(8)]
        measured = aggregate(population)
        stock = self.profile["groups"]["stock"]
        for field in ("battery_sizes", "building_slopes", "roads_coverage_percent",
                      "enemy_player_distance_fraction", "tree_objects_per_100k_cells"):
            with self.subTest(field=field):
                self.assertGreaterEqual(measured["metrics"][field]["median"], stock["metrics"][field]["min"])
                self.assertLessEqual(measured["metrics"][field]["median"], stock["metrics"][field]["max"])
        self.assertGreater(measured["archetypes"]["town"]["count"], 0)
        self.assertGreater(measured["archetypes"]["walled_compound"]["count"], 0)


if __name__ == "__main__":
    unittest.main()
