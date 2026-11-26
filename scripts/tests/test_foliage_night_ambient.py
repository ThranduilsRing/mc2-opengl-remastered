"""Foliage uses the mission sky at every quality, without a second fixed night fill."""
from pathlib import Path
import re
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import console_child
import shader_common

FRAG = ROOT / 'shaders/vulkan/color_prop_tier1.frag'


@pytest.mark.parametrize('quality', [0, 1, 2])
def test_mission_lighting_survives_quality_specialization(tmp_path, quality):
    spv, specialized = tmp_path / 'prop.spv', tmp_path / 'specialized.spv'
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(FRAG.parent), str(FRAG), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    subprocess.run([shader_common.find_tool('spirv-opt'), str(spv),
                    '--set-spec-const-default-value=0:' + str(quality), '--freeze-spec-const',
                    '--fold-spec-const-op-composite', '--eliminate-dead-branches',
                    '--eliminate-dead-functions', '-o', str(specialized)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(specialized)],
                                   text=True, **console_child.child_kwargs(redirected=True))
    main = re.search(r'%main = OpFunction .*?OpFunctionEnd', text, re.S)[0]
    assert 'propFoliageNightAmbient' not in text
    assert re.search(r'OpFunctionCall %\w+ %evalShL2_\w+', main)
    assert re.search(r'OpFunctionCall %\w+ %propNightSunWeight_\w+', main)
