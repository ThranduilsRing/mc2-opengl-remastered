"""Material-normal profile -> resolved CPU environment -> Vulkan UBO -> samples."""
import json
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("material,period", [("Flat", 768), ("Slope", 320), ("Cliff", 320), ("Cement", 48)])
def test_material_normal_profile_reaches_shader(material, period):
    defaults = json.loads((ROOT / "data/visual_tuning.json").read_text())["defaults"]
    loader = (ROOT / "GameOS/gameos/visual_tuning_profile.cpp").read_text()
    upload = (ROOT / "GameOS/gameos/vulkan_terrain_island_run.cpp").read_text()
    abi = (ROOT / "RenderCore/gpu_abi/GpuTerrainFrameUBO.h").read_text()
    ubo = (ROOT / "shaders/vulkan/include/terrain_frame_ubo_vk.glsl").read_text()
    sources = [ROOT / "shaders/vulkan/terrain_tier1.frag"]
    sources += list((ROOT / "shaders/vulkan/include").glob("*.glsl"))
    consumers = "\n".join(p.read_text() for p in sources if p.name != "terrain_frame_ubo_vk.glsl")
    for suffix, value in [("Period", period), ("Strength", 1)]:
        field = f"normal{material}{suffix}"
        key = f"terrainNormal{material}{suffix}"
        assert defaults[key] == value  # baseline-preserving until owner comparison
        assert re.search(r'\{"' + key + r'",' + str(value) + r'\}', loader)
        assert f'u.{field} = normalTuning.at("{key}");' in upload
        assert re.search(r"float\s+" + field + r";", abi)
        assert re.search(r"float\s+" + field + r";", ubo)
        assert f"uT.{field}" in consumers
    assert "visualTuning_environment().legacyTuning" in upload


def test_normal_controls_keep_derivative_filtering():
    detail = (ROOT / "shaders/vulkan/include/terrain_detail_normal.glsl").read_text()
    assert "uvDx * MATV_TILING.y" in detail
    assert "mv_detailTexelGM * MATV_TILING.y" in detail
    cliff = (ROOT / "shaders/vulkan/terrain_tier1.frag").read_text()
    assert "dFdx(v_worldPos) / MATV_CLIFF_TS" in cliff
