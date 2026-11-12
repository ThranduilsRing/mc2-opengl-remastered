"""Reflection regressions: independent geometry, spatial lookup, preserved coverage."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def test_reflection_has_no_screen_space_producer():
    assert not (ROOT / "shaders/vulkan/water_reflection.comp").exists()
    src = (ROOT / "GameOS/gameos/vulkan_water_island_run.cpp").read_text(encoding="utf-8")
    producer = src.split("record_reflection(", 1)[1].split("namespace {", 1)[0]
    assert "s->sColorRes" not in producer and "s->sDepthRes" not in producer
    assert "vkCmdDispatch" not in producer
    assert "planarTerrainRecord(cb)" in producer and "planarPropsRecord(cb)" in producer


def test_reflected_alpha_coverage_uses_the_material_passes():
    src = (ROOT / "GameOS/gameos/vulkan_color_prop_island.cpp").read_text(encoding="utf-8")
    assert "plan.opaquePipe = s->planarOpaquePipe.get()" in src
    assert "plan.corePipe = s->planarCorePipe.get()" in src
    assert "plan.fringePipe = s->planarFringePipe.get()" in src
    assert "!target && pp && pp->runVkPostHzbNative" in src


GEOM = (Path(__file__).resolve().parents[2] / "shaders/vulkan/water_geom.frag").read_text(encoding="utf-8")


def test_lookup_takes_no_wave_offset():
    m = re.search(r"vec2 reflectionUV=([^;]*);", GEOM)
    assert m and "nzGrad" not in m[1] and "waveSlope" not in m[1]


def test_below_water_geometry_is_clipped_in_both_materials():
    for shader, position in (("terrain_tier1.frag", "v_worldPos"), ("color_prop_tier1.frag", "vWorldPos")):
        src = (ROOT / "shaders/vulkan" / shader).read_text(encoding="utf-8")
        assert f"kPlanarReflection != 0 && {position}.z < uT.shorelineWaterElev" in src
        assert "layout(constant_id = 10)" in src
