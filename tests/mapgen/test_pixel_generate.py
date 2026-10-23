"""World-unit geometry and deliberate errors beyond point-cell checks."""
import copy
import unittest

from tools.mapgen.common import CELL_WU, load_json
from tools.mapgen.generate import PROFILE_PATH, generate
from tools.mapgen.footprints import table
from tools.mapgen.placement import Spatial, cell_of, overlaps
from tools.mapgen.raster import polygon
from tools.mapgen.validate import validate


class PixelPlacementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = load_json(PROFILE_PATH)
        cls.example = generate(180,7,profile=cls.profile)

    def test_native_ids_continuous_positions_and_native_walls(self):
        layout = self.example
        self.assertEqual(validate(layout,self.profile),[])
        for obj in layout["objects"]:
            self.assertIsInstance(obj["type_id"],int)
            self.assertEqual(cell_of(layout,obj["position_wu"]),obj["cell"])
            self.assertEqual(obj["footprint"]["width_wu"],table()["types"][obj["footprint_key"]]["width_wu"])
        self.assertTrue(any(abs(o["position_wu"][0]/CELL_WU-round(o["position_wu"][0]/CELL_WU))>.05 for o in layout["objects"]))
        self.assertEqual(len(layout["wall_edges_wu"]),4)
        walls = [o for o in layout["objects"] if o["class"]=="wall"]
        self.assertTrue(all(o["footprint"]["depth_wu"]>3*CELL_WU for o in walls))

    def test_building_overlap_and_model_resize_detected(self):
        layout = copy.deepcopy(self.example)
        buildings = [o for o in layout["objects"] if o["class"]=="building"]
        a,b = buildings[:2]
        b["position_wu"] = list(a["position_wu"])
        b["position_wu"][0] += 10  # Whole envelopes collide even with different pivots.
        b["cell"] = cell_of(layout,b["position_wu"])
        self.assertTrue(any("footprint overlaps" in e for e in validate(layout,self.profile)))
        layout = copy.deepcopy(self.example)
        layout["objects"][0]["footprint"]["width_wu"] *= 2
        self.assertTrue(any("differs from native" in e for e in validate(layout,self.profile)))

    def test_wall_gap_and_world_parent_spacing_detected(self):
        layout = copy.deepcopy(self.example)
        ident = layout["wall_edges_wu"][0]["wall_ids"][1]
        layout["objects"] = [o for o in layout["objects"] if o["id"]!=ident]
        self.assertTrue(any("ring is incomplete" in e for e in validate(layout,self.profile)))
        layout = copy.deepcopy(self.example)
        turret = next(o for o in layout["objects"] if o["class"]=="turret")
        parent = next(o for o in layout["objects"] if o["id"]==turret["parent_link"])
        turret["position_wu"][:2] = parent["position_wu"][:2]
        self.assertTrue(any("world parent distance" in e for e in validate(layout,self.profile)))

    def test_rotated_rectangles_use_separating_axes(self):
        fp = {"bounds_wu":[[-10,-1,0],[10,1,1]]}
        a = polygon({"rotation":45},fp,(0,0),rectangle=True)
        b = polygon({"rotation":45},fp,(0,5),rectangle=True)
        self.assertFalse(overlaps(a,b))  # AABBs overlap; the thin rectangles do not.
        self.assertTrue(overlaps(a,polygon({"rotation":135},fp,(0,0),rectangle=True)))

    def test_width_polyline_corruption_detected(self):
        layout = copy.deepcopy(self.example)
        layout["roads_wu"][0]["width_wu"] = 10000
        self.assertTrue(any("road width crosses" in e for e in validate(layout,self.profile)))

    def test_clearance_query_crosses_bucket_boundary(self):
        index = Spatial()
        index.add({"id":"a"},[(120,0),(127,0),(127,10),(120,10)])
        neighbours = list(index.candidates([(130,0),(135,0),(135,10),(130,10)]))
        self.assertEqual(neighbours[0][0]["id"],"a")


if __name__ == "__main__":
    unittest.main()
