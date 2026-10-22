"""Raster scale limits, coordinate mapping and portable PNG output."""
from pathlib import Path
import tempfile
import unittest

from PIL import Image
import numpy as np

from tools.mapgen.common import CELL_WU, load_json, write_json
from tools.mapgen.footprints import table
from tools.mapgen.raster import (COLOURS, TEAM_COLOURS, TERRAIN_COLOURS,
                                 pixel_rectangle, polygon, quonset_scale, render, world_position)


class RasterTests(unittest.TestCase):
    def fixture(self,objects=()):
        return {"name":"fixture","source":{"corpus":"fixture"},"size":{"rows":6,"cols":6},
                "terrain":{"cell_step":3,"terrain_type":[[1,1],[1,1]],"elevation_wu":[[0,0],[0,0]],
                           "overlay_type":[[-1,-1],[-1,-1]]},
                "objects":list(objects),"layers":{},"parts":[],"objectives":[],"player_start":None}

    def test_asymmetric_pivot_rotation(self):
        fp = {"bounds_wu":[[-10,-20,0],[30,50,80]]}
        points = polygon({"rotation":90},fp,(100,200),rectangle=True)
        self.assertAlmostEqual(points[0][0],120)
        self.assertAlmostEqual(points[0][1],190)
        layout = {"size":{"rows":6,"cols":6}}
        self.assertEqual(world_position(layout,{"cell":[2,3]}),[CELL_WU/2,CELL_WU/2])

    def test_png_and_sidecar_scale(self):
        layout = self.fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"map.png"
            meta = render(layout,path,1,catalog={"types":{},"surfaces":{}})
            self.assertEqual(meta["map_pixels"],[256,256])
            with Image.open(path) as png:
                self.assertEqual(png.size,(256,256))
                original = np.asarray(png)
            with Image.open(path.with_stem("map_4x")) as preview:
                self.assertEqual(preview.size,(1024,1024))
                blocks = np.asarray(preview).reshape(256,4,256,4,3)
                self.assertTrue(np.all(blocks==original[:,None,:,None,:]))
            self.assertTrue(path.with_suffix(".render.json").exists())
            with self.assertRaisesRegex(ValueError,"too large"):
                render(layout,path,.001,catalog={"types":{},"surfaces":{}})
            with self.assertRaisesRegex(ValueError,"positive"):
                render(layout,path,-1,catalog={"types":{},"surfaces":{}})

    def test_quonset_is_two_by_one_at_default_scale(self):
        asset = table()["types"]["stock:335"]
        self.assertEqual(asset["type_name"],"Quonset")
        self.assertAlmostEqual(quonset_scale(),84.160/2)
        self.assertEqual(pixel_rectangle({"rotation":0},asset,quonset_scale()),(1,2))
        self.assertEqual(pixel_rectangle({"rotation":80},asset,quonset_scale()),(2,1))
        obj = {"id":"hut","class":"building","type_id":335,"position_wu":[0,0,0],
               "rotation":0,"team":-1,"role":"military","footprint":asset}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"hut.png"
            full = render(self.fixture([obj]),path)
            with Image.open(path) as png:
                colours = list(png.getdata())
                self.assertEqual(colours.count(COLOURS["military"]),2)
            crop = render(self.fixture(),path,crop=[0,0,3,3])
            self.assertEqual(full["wu_per_pixel"],crop["wu_per_pixel"])
            self.assertFalse(crop["scale_overridden"])
            self.assertEqual(crop["header_pixels"],0)

    def test_turret_is_one_team_pixel(self):
        obj = {"id":"turret","class":"turret","type_id":335,"position_wu":[0,0,0],
               "rotation":45,"team":1,"footprint":table()["types"]["stock:335"]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"turret.png"
            render(self.fixture([obj]),path)
            with Image.open(path) as png:
                self.assertEqual(list(png.getdata()).count(TEAM_COLOURS[1]),1)

    def test_player_start_accepts_world_xyz(self):
        layout = self.fixture()
        layout["player_start"] = [3,3]
        layout["player_start_wu"] = [0,0,160]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"start.png"
            render(layout,path)
            with Image.open(path) as png:
                self.assertIn(TEAM_COLOURS[0],list(png.getdata()))

    def test_large_json_gzip_roundtrip_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            a,b = Path(directory)/"a.json.gz",Path(directory)/"b.json.gz"
            value = {"terrain":[[1.25,2.5]],"unicode":"snow \u2603","nan":None}
            write_json(a,value)
            write_json(b,value)
            self.assertEqual(load_json(a),value)
            self.assertEqual(a.read_bytes(),b.read_bytes())


if __name__ == "__main__":
    unittest.main()
