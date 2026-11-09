"""TREE-REFL-SOLID: the water SSR accepts a hit within an object-sized shell behind the depth surface.

The depth buffer holds only a canopy's camera-facing surface; a reflected ray crosses that surface up to a crown's
depth behind it. The old 12 wu / 0.5% shell accepted only grazing rays, so reflected trees read as see-through dots.
"""
from pathlib import Path
import re

SRC = (Path(__file__).resolve().parents[2] / "shaders/vulkan/water_reflection.comp").read_text(encoding="utf-8")


def test_hit_shell_is_object_sized():
    m = re.search(r"length\(hit-rayPoint\)<max\(([0-9.]+),length\(hit-origin\)\*([0-9.]+)\)", SRC)
    assert m, "hit acceptance shell not found"
    floor_wu, rel = float(m[1]), float(m[2])
    assert floor_wu >= 48.0      # a tree crown, not a 12 wu skin
    assert 0.02 <= rel <= 0.05   # grows with distance, still rejects sky/far-hill discontinuities


def test_reflection_stays_spatial():
    assert "imageLoad" not in SRC and "history" not in SRC.lower().replace("no history", "")
