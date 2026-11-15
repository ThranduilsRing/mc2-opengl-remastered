"""Compiled prop shading preserves HDR window light and padded atlas repeats."""
from pathlib import Path
import re
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import console_child
import shader_common


@pytest.mark.parametrize('quality', [0, 1, 2])
def test_tree_vertex_quality_removes_only_unused_sh(tmp_path, quality):
    src = ROOT / 'shaders/vulkan/color_prop_tier1.vert'
    text = re.sub(r'bool tree = [^;]+;', 'bool tree = true;', src.read_text())
    variant, spv, optimized = (tmp_path / n for n in ('tree.vert', 'tree.spv', 'tree-opt.spv'))
    variant.write_text(text)
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(src.parent), str(variant), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    subprocess.run([shader_common.find_tool('spirv-opt'), str(spv),
                    '--set-spec-const-default-value=0:' + str(quality), '--freeze-spec-const',
                    '--ssa-rewrite', '--ccp', '--eliminate-dead-branches',
                    '--eliminate-dead-functions', '-o', str(optimized)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    dis = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(optimized)],
                                 text=True, **console_child.child_kwargs(redirected=True))
    assert ('OpName %evalShL2' in dis) is (quality != 2)


@pytest.mark.parametrize('regressed', [False, True])
def test_compiled_prop_atlas_wraps_before_scaling(tmp_path, regressed):
    # A fence repeats U three times inside a small atlas subregion. Scaling first
    # makes repeats 2/3 sample transparent padding and appear to lose segments.
    production = ROOT / 'shaders/vulkan/color_prop_tier1.frag'
    src = tmp_path / 'prop.frag'
    shader = production.read_text()
    if regressed:
        shader = shader.replace('fract(vUv) * vUvScale', 'fract(vUv * vUvScale)')
    src.write_text(shader)
    spv = tmp_path / 'prop.spv'
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(production.parent), str(src), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(spv)],
                                  text=True, **console_child.child_kwargs(redirected=True))
    main = re.search(r'%main = OpFunction .*?OpFunctionEnd', text, re.S)[0]
    wraps = re.findall(r'(%\w+) = OpExtInst %v2float %\w+ Fract (%\w+)', main)
    assert len(wraps) == 2  # alpha-ON and alpha-OFF texture arrays
    for wrapped, source in wraps:
        from_unscaled_uv = re.search(re.escape(source) + r' = OpLoad %v2float %vUv\b', main)
        assert bool(from_unscaled_uv) is not regressed
        if not regressed:
            scale = re.search(r'(%\w+) = OpLoad %v2float %vUvScale\b', main[main.index(wrapped + ' ='):])[1]
            assert re.search(r'OpFMul %v2float ' + re.escape(wrapped) + ' ' + re.escape(scale), main)
    # Mip derivatives must use the unwrapped coordinates on both sampling paths.
    assert 'OpImageSampleExplicitLod' in main and ' Grad ' in main


def test_compiled_window_emission_decodes_adds_hdr_then_encodes(tmp_path):
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
    assert ' FMin ' not in body
    pointer = re.search(r'OpStore (%\w+) ' + re.escape(add[1]), body)[1]
    encoded = re.search(r'(%\w+) = OpFunctionCall %\w+ %propEncodeColor_\w+ ' + re.escape(pointer), body)[1]
    assert 'OpReturnValue ' + encoded in body


@pytest.mark.parametrize("quality", [0, 1, 2])
def test_window_emission_survives_each_quality_specialization(tmp_path, quality):
    src = ROOT / 'shaders/vulkan/color_prop_tier1.frag'
    spv, specialized = tmp_path / 'prop.spv', tmp_path / 'specialized.spv'
    subprocess.run([shader_common.find_tool('glslangValidator'), '-V',
                    '-I' + str(src.parent), str(src), '-o', str(spv)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    subprocess.run([shader_common.find_tool('spirv-opt'), str(spv),
                    '--set-spec-const-default-value=0:' + str(quality), '--freeze-spec-const',
                    '--fold-spec-const-op-composite', '--eliminate-dead-branches',
                    '--eliminate-dead-functions', '-o', str(specialized)],
                   check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool('spirv-dis'), str(specialized)],
                                  text=True, **console_child.child_kwargs(redirected=True))
    main = re.search(r'%main = OpFunction .*?OpFunctionEnd', text, re.S)[0]
    assert re.search(r'OpFunctionCall %\w+ %propAddWindowEmission_\w+', main)
