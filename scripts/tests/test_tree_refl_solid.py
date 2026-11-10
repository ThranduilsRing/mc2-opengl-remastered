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


GEOM = (Path(__file__).resolve().parents[2] / "shaders/vulkan/water_geom.frag").read_text(encoding="utf-8")


def test_lookup_takes_no_wave_offset():
    m = re.search(r"vec2 reflectionUV=([^;]*);", GEOM)
    assert m and "nzGrad" not in m[1] and "waveSlope" not in m[1]


def test_foliage_depth_is_dilated_and_colour_follows():
    assert "nearestDepth" in SRC
    assert "textureLod(sceneDepth" in SRC.split("float nearestDepth")[1].split("void main")[0]
    assert "coverageColour(inv,hit,nuv)" in SRC   # colour from texels around the nearest texel, not the leaf-hole texel
    assert "const int STEPS=64" in SRC
    assert "smoothstep(0.0,0.04" not in SRC  # no wide screen-edge fade
