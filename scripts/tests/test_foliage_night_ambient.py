"""Foliage night ambient: zero by day, cool and shape-giving at night, wired into the tree branch."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import console_child
import shader_common

NIGHT = ROOT / 'shaders/vulkan/include/prop_night_vk.glsl'
FRAG = ROOT / 'shaders/vulkan/color_prop_tier1.frag'


def _consts():
    text = NIGHT.read_text()
    vec = {m[1]: tuple(float(x) for x in m[2].split(','))
           for m in re.finditer(r'const vec3\s+(kFoliageNight\w+)\s*=\s*vec3\(([^)]*)\)', text)}
    wrap = float(re.search(r'const float\s+kFoliageNightWrap\s*=\s*([\d.]+)', text)[1])
    return vec['kFoliageNightSky'], vec['kFoliageNightGround'], wrap


def _ambient(nz, leaf, night):
    sky, ground, wrap = _consts()
    up = 0.5 + 0.5 * nz
    hemi = [g + (s - g) * up for s, g in zip(sky, ground)]
    if leaf:
        hemi = [h + s * wrap * (1.0 - up) for h, s in zip(hemi, sky)]
    return [h * night for h in hemi]


def test_day_adds_nothing():
    for nz in (-1.0, 0.0, 1.0):
        assert _ambient(nz, True, 0.0) == [0.0, 0.0, 0.0]


def test_night_is_cool_shaped_and_dim():
    top, side, under = (_ambient(nz, True, 1.0) for nz in (1.0, 0.0, -1.0))
    assert top[2] > top[1] > top[0]                      # cool blue-grey sky
    assert sum(top) > sum(side) > sum(under) > 0.0      # crown shape: lit top, darker underside
    assert sum(_ambient(-1.0, False, 1.0)) < sum(under)  # leaves wrap sky to their underside
    assert max(top) < 0.5                                # a fill, not a glow (min(albedo, ...) caps at 1)


def test_tree_branch_adds_ambient_before_the_cap():
    src = FRAG.read_text()
    tree = src[src.index('if (kBaseQuality == 2 && treeMaterial)'):]
    tree = tree[:tree.index('rgb = propEncodeColor(min(albedo, albedo*irradiance));')]
    assert 'irradiance += propFoliageNightAmbient(' in tree
    # Grass clumps and terrain are not prop materials; only the prop shader calls it.
    users = [p for p in (ROOT / 'shaders').rglob('*') if p.is_file() and 'propFoliageNightAmbient(' in p.read_text(errors='ignore')]
    assert sorted(p.name for p in users) == ['color_prop_tier1.frag', 'prop_night_vk.glsl']


def test_call_survives_high_quality_specialization(tmp_path):
    spv, specialized = tmp_path / 'prop.spv', tmp_path / 'specialized.spv'
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(FRAG.parent), str(FRAG), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    subprocess.run([shader_common.find_tool('spirv-opt'), str(spv),
                    '--set-spec-const-default-value=0:2', '--freeze-spec-const',
                    '--fold-spec-const-op-composite', '--eliminate-dead-branches',
                    '--eliminate-dead-functions', '-o', str(specialized)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(specialized)],
                                   text=True, **console_child.child_kwargs(redirected=True))
    main = re.search(r'%main = OpFunction .*?OpFunctionEnd', text, re.S)[0]
    assert re.search(r'OpFunctionCall %\w+ %propFoliageNightAmbient_\w+', main)
