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
        expected = 1.75 if suffix == "Strength" and material in ("Flat", "Slope") else value
        if suffix == "Period" and material in ("Flat", "Slope"):
            expected = value / 2   # TERRAIN-NORMAL-UNIFORM: shipped tiling x2 over the loader fallback
        assert defaults[key] == expected
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


@pytest.mark.parametrize("angle", [0.0, 0.6435011088, 0.9272952180, 2.1])
@pytest.mark.parametrize("axis", [0, 1])
def test_normalgl_bump_lights_world_facing_side(angle, axis):
    import numpy as np

    detail = (ROOT / "shaders/vulkan/include/terrain_detail_normal.glsl").read_text()
    decode = re.search(r"return texel \* vec3\(([^)]+)\) \+ vec3\(([^)]+)\);", detail)
    gain, bias = [np.fromstring(v, sep=",") for v in decode.groups()]
    c, s = np.cos(angle), np.sin(angle)
    rotation = np.array([[c, -s], [s, c]])  # GLSL column-major mat2
    light = np.zeros(3)
    light[axis], light[2] = 0.8, 0.6
    brightness = []
    for side in [-1, 1]:
        world = np.zeros(2)
        world[axis] = side * 0.25
        uv = rotation @ world
        # h=1-u*u-v*v: outward bump normal; GL green points against rows.
        normal_gl = np.array([2 * uv[0], -2 * uv[1], 1.0])
        normal_gl /= np.linalg.norm(normal_gl)
        decoded = (normal_gl * 0.5 + 0.5) * gain + bias
        decoded[:2] = rotation.T @ decoded[:2]
        brightness.append(decoded @ light)
        # Hex taps return encoded GL, so their inverse rotation is F R^T F.
        flip = np.array([1.0, -1.0])
        hex_gl = flip * (rotation.T @ (normal_gl[:2] * flip))
        assert hex_gl * flip == pytest.approx(decoded[:2])
    assert brightness[1] > brightness[0]
    hex_shader = (ROOT / "shaders/vulkan/include/terrain_hex_tiling.glsl").read_text()
    assert "vec2(1.0, -1.0) * (transpose(R)" in hex_shader
    assert "(c.rg * 2.0 - 1.0) * vec2(1.0, -1.0)" in hex_shader
    assert detail.count("mv_detailDecode(") == 12
    lamp = (ROOT / "shaders/vulkan/terrain_tier1.frag").read_text()
    assert "amount * mv_detailDecode(textureLod" in lamp
