"""Reflection regressions: independent geometry, spatial lookup, preserved coverage."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def test_reflection_has_no_screen_space_producer():
    assert not (ROOT / "shaders/vulkan/water_reflection.comp").exists()
    src = (ROOT / "GameOS/gameos/vulkan_water_island_run.cpp").read_text(encoding="utf-8")
    producer = src.split("record_reflection(", 1)[1].split("namespace {", 1)[0]
    # f1a3dc1d3 added a planar-pixel mask: main-view depth is the coverage pass's depth TEST only, and the sole
    # compute dispatch dilates that mask (water_reflection_mask.comp). Scene colour is still never read, and the
    # reflection stays independent planar geometry.
    assert "s->sColorRes" not in producer
    assert producer.count("s->sDepthRes") == 1 and "coverage.setDepth(dr::attachment(s->sDepthRes" in producer
    assert producer.count("vkCmdDispatch") == 1 and "s->maskCompute" in producer
    assert "planarTerrainRecord(cb)" in producer and "planarPropsRecord(cb)" in producer


def test_reflected_alpha_coverage_uses_the_material_passes():
    src = (ROOT / "GameOS/gameos/vulkan_color_prop_island.cpp").read_text(encoding="utf-8")
    assert "plan.opaquePipe = s->planarOpaquePipe.get()" in src
    assert "plan.corePipe = s->planarCorePipe.get()" in src
    assert "plan.fringePipe = s->planarFringePipe.get()" in src
    assert "!target && pp && pp->runVkPostHzbNative" in src


def test_coverage_attachment_is_distinct_from_sampled_reflection():
    # Trigger: vulkan_water_island_{images,run}.cpp coverage image usage.
    images = (ROOT / "GameOS/gameos/vulkan_water_island_images.cpp").read_text()
    run = (ROOT / "GameOS/gameos/vulkan_water_island_run.cpp").read_text()
    producer = run.split("record_reflection(", 1)[1].split("namespace {", 1)[0]
    coverage = producer.split("dr::end(ctx,cb);", 1)[0]
    assert "coverage.color[0]=dr::attachment(s->coverageView.get()," in coverage
    assert "dr::attachment(s->reflectionView.get()," not in coverage
    assert "s->coverageImage.create(" in images
    assert "imageInfo{VK_NULL_HANDLE,s->coverageView.get(),VK_IMAGE_LAYOUT_GENERAL}" in images
    transitions = re.findall(r"barrier\(s->coverageImage.get\(\),(.*?);", producer, re.S)
    assert len(transitions) == 2
    assert "VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL" in transitions[0]
    assert "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT" in transitions[0]
    assert "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL" in transitions[1]
    assert "VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT" in transitions[1]
    assert "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT" in transitions[1]


GEOM = (Path(__file__).resolve().parents[2] / "shaders/vulkan/water_geom.frag").read_text(encoding="utf-8")


def test_lookup_takes_no_wave_offset():
    m = re.search(r"vec2 reflectionUV=([^;]*);", GEOM)
    assert m and "nzGrad" not in m[1] and "waveSlope" not in m[1]


def test_below_water_geometry_is_clipped_in_both_materials():
    for shader, position in (("terrain_tier1.frag", "v_worldPos"), ("color_prop_tier1.frag", "vWorldPos")):
        src = (ROOT / "shaders/vulkan" / shader).read_text(encoding="utf-8")
        assert f"kPlanarReflection != 0 && {position}.z < uT.shorelineWaterElev" in src
        assert "layout(constant_id = 10)" in src
