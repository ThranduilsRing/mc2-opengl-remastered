"""Exercise the real bake and the artifacts + reflection families with corrupted descriptor/layout copies."""
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools/shader_offline_build'), str(ROOT / 'tools')]
import build_variants
import gpu_abi_gen
import check_shader_interface as csi
import shader_common
import slang_compile


def test_descriptor_classes_and_abi_survive_bake_and_broken_sidecars_fail(tmp_path, monkeypatch):
    if not all(shader_common.find_tool(t) for t in ('glslangValidator', 'spirv-cross')):
        pytest.skip('Vulkan SDK unavailable')
    src = tmp_path / 'probe.comp'
    src.write_text('''#version 450
layout(local_size_x=1) in;
struct Gpu { float weight; };
layout(set=1, binding=0, std430) buffer Payload { Gpu value; } payload;
layout(set=1, binding=1, rgba8) uniform writeonly image2D destination;
layout(set=1, binding=2) uniform sampler2D source;
void main() { imageStore(destination, ivec2(0), texture(source, vec2(0)) * payload.value.weight); }
''')
    (tmp_path / 'Gpu.h').write_text('// @gpu std430 glsl=Gpu\nstruct Gpu {\nfloat weight;\n};\n')
    monkeypatch.setattr(gpu_abi_gen, 'STRUCTS', [('Gpu.h', '', '')])
    manifest = tmp_path / 'tools/shader_offline_build/pilots.json'
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps({'spv_dir': 'out', 'pilots': [{
        'program': 'probe', 'stages': {'comp': 'probe.comp'},
        'variants': [{'name': 'default', 'defines': []}]}]}))
    census = tmp_path / 'docs/render-backend-seams/binding-slot-occupancy.json'
    census.parent.mkdir(parents=True)
    census.write_text(json.dumps({'occupancy': {
        f'set1:binding{i}': [{'type': typ, 'role': name, 'file': 'probe.comp'}]
        for i, typ, name in ((0, 'buffer', 'Payload'), (1, 'image2D', 'destination'),
                             (2, 'sampler2D', 'source'))}}))
    monkeypatch.setattr(sys, 'argv', ['build_variants', '--root', str(tmp_path),
                                    '--pilots', str(manifest), '--quiet'])
    assert build_variants.main() == 0
    sidecar = tmp_path / 'out/probe.comp.default.json'
    clean = json.loads(sidecar.read_text())
    assert clean['bindings']['images'][0]['set'] == 1
    assert clean['bindings']['samplers'][0]['set'] == 1

    def check():
        return [csi.main(['--root', str(tmp_path), '--quiet', '--families', fam])
                for fam in ('artifacts', 'reflection')]

    assert check() == [0, 0]
    for group in ('samplers', 'images'):
        broken = json.loads(json.dumps(clean))
        broken['bindings'][group][0]['binding'] = 99
        sidecar.write_text(json.dumps(broken))
        assert check() == [1, 1]
    broken = json.loads(json.dumps(clean))
    gpu = next(t for t in broken['types'].values()
               if t['name'] == 'Gpu' and 'offset' in t['members'][0])
    gpu['members'][0]['offset'] = 16
    sidecar.write_text(json.dumps(broken))
    assert check() == [1, 1]
    del gpu['members'][0]['offset']
    sidecar.write_text(json.dumps(broken))
    assert check() == [1, 1]
    # Slang must reject descriptor-class swaps even when set/binding still match.
    slang = tmp_path / 'probe.slang'
    slang.write_text('[[vk::binding(1, 1)]] RWTexture2D<float4> destination;\n')
    for group in ('textures', 'separate_images', 'separate_samplers', 'samplers'):
        assert any('type mismatch' in e for e in slang_compile.validate_reflection(
            {group: [{'name': 'destination', 'binding': 1, 'set': 1, 'type': 'sampler'}]}, slang, tmp_path))


@pytest.mark.parametrize('group', ['ubos', 'ssbos'])
@pytest.mark.parametrize('type_id', [None, '', '_missing'])
def test_abi_rejects_missing_buffer_type(group, type_id):
    block = dict(name='BrokenBuffer', binding=0, set=0)
    if type_id is not None:
        block['type'] = type_id
    assert slang_compile.validate_abi({group: [block], 'types': {
        '_unrelated': {'name': 'gl_PerVertex', 'members': []}}})
