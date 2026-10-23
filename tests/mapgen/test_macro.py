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
            (180,7,'8979e4c2a61c43e52bfc822f469ea29c1b851badc0f1ffdd7ca42445a0b97c77'),
            (360,17,'d1582a18021bb9303021e5ae307c5b1ba11c6e36041cdb3213ea21bcccb73230'),
            (720,23,'13261f7070759cf6e8107e04aaa2714e082466fefe1fd51d8fffecbf9bc2db0e')):
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
