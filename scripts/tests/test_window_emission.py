"""The compiled window path composes and clamps linear light before encoding."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import console_child
import shader_common


def test_compiled_window_emission_decodes_adds_clamps_then_encodes(tmp_path):
    src = ROOT / 'shaders/vulkan/color_prop_tier1.frag'
    spv = tmp_path / 'prop.spv'
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(src.parent), str(src), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(spv)],
                                  text=True, **console_child.child_kwargs(redirected=True))
    body = re.search(r'%propAddWindowEmission_\w+ = OpFunction .*?OpFunctionEnd', text, re.S)[0]
    decode = re.search(r'(%\w+) = OpFunctionCall %\w+ %propDecodeColor_\w+', body)
    add = re.search(r'(%\w+) = OpFAdd %\w+ ' + re.escape(decode[1]) + r' %\w+', body)
    clamp = re.search(r'(%\w+) = OpExtInst %\w+ %\w+ FMin ' + re.escape(add[1]) + r' %\w+', body)
    assert clamp
    # glslang stores function arguments in local pointers; the clamped result must feed the encode.
    pointer = re.search(r'OpStore (%\w+) ' + re.escape(clamp[1]), body)[1]
    encoded = re.search(r'(%\w+) = OpFunctionCall %\w+ %propEncodeColor_\w+ ' + re.escape(pointer), body)[1]
    assert 'OpReturnValue ' + encoded in body
