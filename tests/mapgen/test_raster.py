"""Raster scale limits, coordinate mapping and portable PNG output."""
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.mapgen.common import CELL_WU
from tools.mapgen.raster import polygon, render, world_position


class RasterTests(unittest.TestCase):
    def test_asymmetric_pivot_rotation(self):
        fp = {"bounds_wu":[[-10,-20,0],[30,50,80]]}
        points = polygon({"rotation":90},fp,(100,200),rectangle=True)
        self.assertAlmostEqual(points[0][0],120)
        self.assertAlmostEqual(points[0][1],190)
        layout = {"size":{"rows":6,"cols":6}}
        self.assertEqual(world_position(layout,{"cell":[2,3]}),[CELL_WU/2,CELL_WU/2])

    def test_png_and_sidecar_scale(self):
        layout = {"name":"fixture","source":{"corpus":"fixture"},"size":{"rows":6,"cols":6},
                  "terrain":{"cell_step":3,"terrain_type":[[1,1],[1,1]],"elevation_wu":[[0,0],[0,0]],
                             "overlay_type":[[-1,-1],[-1,-1]]},
                  "objects":[],"layers":{},"parts":[],"objectives":[],"player_start":None}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"map.png"
            meta = render(layout,path,1,catalog={"types":{},"surfaces":{}})
            self.assertEqual(meta["map_pixels"],[256,256])
            self.assertEqual(Image.open(path).size,(600,324))
            self.assertTrue(path.with_suffix(".render.json").exists())
            with self.assertRaisesRegex(ValueError,"too large"):
                render(layout,path,.001,catalog={"types":{},"surfaces":{}})
            with self.assertRaisesRegex(ValueError,"positive"):
                render(layout,path,-1,catalog={"types":{},"surfaces":{}})


if __name__ == "__main__":
    unittest.main()
