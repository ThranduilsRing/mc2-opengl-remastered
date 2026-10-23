"""Large-map priors, terrain costs and exact small-map compatibility."""
import hashlib
import json
import unittest
import numpy as np
from tools.mapgen.common import FLAT_DEG, slope_grid
from tools.mapgen.generate import generate, synthetic_terrain
from tools.mapgen.macro import plan, valley_route


class MacroTests(unittest.TestCase):
    def test_small_examples_remain_identical(self):
        for size,seed,digest in (
            (180,7,'e1add3e5598706fad8c12a7f70c69aa5a7c0d3b00674095b9dd7c3d2a04ec6a7'),
            (360,17,'b6a4685a283fc4cb470aba91f436880e766e10bb46099b66c82b25693b9d7a9c'),
            (720,23,'620321fa76b5c06ed6d73759168a3a135a1e34acf39a6b1177bd633e828a1e7b')):   # re-frozen in merge window 44 after the turret battery planner changed the default output
            with self.subTest(size=size):
                data = generate(size,seed)
                self.assertNotIn('macro',data)
                self.assertEqual(hashlib.sha256(json.dumps(data,sort_keys=True).encode()).hexdigest(),digest)

    def test_regional_seed_sweep_and_graph(self):
        for seed in (7,42,71):
            with self.subTest(seed=seed):
                h,wet = synthetic_terrain(1024,seed)
                slopes = slope_grid(h)
                dry = ~wet & (slopes<FLAT_DEG)
                macro,specs,paths,_ = plan(dry,h,slopes,wet,seed,40)
                self.assertGreaterEqual(len(specs),30)
                sizes = [s['population'] for s in specs]
                self.assertGreater(max(sizes)/min(sizes),50)
                self.assertGreater(sum(n<=3 for n in sizes)/len(sizes),.4)
                self.assertEqual(set(np.unique(macro['regions'])),set(range(5)))
                majors = macro['major_count']
                self.assertEqual(sum(e['kind']=='trunk' for e in macro['edges']),majors-1)
                self.assertGreater(sum(e['kind']=='loop' for e in macro['edges']),0)
                connected = {0}
                for e in macro['edges']:
                    self.assertTrue(e['a'] in connected or e['b'] in connected)
                    connected.update((e['a'],e['b']))
                self.assertEqual(len(connected),len(specs))
                self.assertTrue(all(dry[r,c] for p,_ in paths for r,c in p))
                for p,_ in paths:
                    for a,b in zip(p,p[1:]):
                        # Diagonal width has dry side blocks, no corner cutting.
                        self.assertTrue(dry[a[0],b[1]] and dry[b[0],a[1]])
                if seed==42:
                    again = plan(dry,h,slopes,wet,seed,40)
                    self.assertEqual(macro,again[0])
                    self.assertEqual(specs,again[1])
                    self.assertEqual(paths,again[2])

    def test_route_prefers_valleys_and_cannot_cut_water_corners(self):
        allowed = np.ones((20,20),bool)
        costs = np.zeros((20,20))
        costs[5:15,8:12] = 40
        path = valley_route((10,1),(10,18),allowed,costs)
        self.assertTrue(all(costs[p]==0 for p in path))
        allowed = np.eye(3,dtype=bool)
        with self.assertRaisesRegex(ValueError,'no dry regional'):
            valley_route((0,0),(2,2),allowed,np.zeros((3,3)))


if __name__=='__main__':
    unittest.main()
