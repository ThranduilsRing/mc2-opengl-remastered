"""Run the real contract entry points on clean and deliberately broken source copies."""
import importlib
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def source(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / 'scripts'))
    shutil.copytree(ROOT / 'shaders', tmp_path / 'shaders')
    for rel in ('GameOS/gameos/vulkan_mech_island.cpp', 'mclib/camera.cpp',
                '3rdparty/smaa/SMAA.hlsl', '3rdparty/fsr1/ffx_a.h',
                '3rdparty/fsr1/ffx_fsr1.h'):
        target = tmp_path / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, target)
    return tmp_path


def run(name, source, monkeypatch):
    checker = importlib.import_module('check-' + name)
    args = ['check-' + name]
    if name == 'lighting-divergence':
        monkeypatch.setattr(checker, 'ROOT', source)
    else:
        args += ['--root', str(source), '--quiet']
        if name == 'vulkan-bindings':
            args += ['--strict']
    monkeypatch.setattr(sys, 'argv', args)
    return checker.main()


def change(source, rel, before, after):
    path = source / rel
    text = path.read_text(encoding='utf-8')
    assert before in text
    path.write_text(text.replace(before, after), encoding='utf-8')


@pytest.mark.parametrize('name', ['lighting-divergence', 'binding-slots', 'vulkan-bindings'])
def test_clean_source_passes(name, source, monkeypatch):
    assert run(name, source, monkeypatch) == 0


@pytest.mark.parametrize('floor', ['mix(0.25, 1.0, shadow)', '(0.50 + 0.50 * NdotL)'])
def test_new_inline_floor_fails(floor, source, monkeypatch):
    path = source / 'shaders/vulkan/terrain_tier1.frag'
    with path.open('a', encoding='utf-8') as out:
        out.write('\nfloat regression = ' + floor + ';\n')
    assert run('lighting-divergence', source, monkeypatch) == 1


def test_cross_stage_collision_fails_on_live_source(source, monkeypatch):
    path = source / 'shaders/vulkan/terrain_tier1.frag'
    with path.open('a', encoding='utf-8') as out:
        out.write('\nlayout(set=0, binding=1) uniform Incompatible { vec4 bad; };\n')
    assert run('vulkan-bindings', source, monkeypatch) == 1


@pytest.mark.parametrize('side', ['cpp', 'glsl', 'set', 'cpp_set', 'type', 'cpp_type'])
def test_native_lockstep_fails(side, source, monkeypatch):
    if side == 'cpp':
        change(source, 'GameOS/gameos/vulkan_mech_island.cpp',
               'b[3].binding = 4;', 'b[3].binding = 42;')
    elif side == 'cpp_set':
        change(source, 'GameOS/gameos/vulkan_mech_island.cpp',
               'layouts[]{s->dsl.get(), s->groundLighting.layout(), ctx.textures().table().layout()}',
               'layouts[]{s->groundLighting.layout(), s->dsl.get(), ctx.textures().table().layout()}')
    elif side == 'cpp_type':
        change(source, 'GameOS/gameos/vulkan_mech_island.cpp',
               'b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC',
               'b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE')
    elif side == 'type':
        change(source, 'shaders/vulkan/mech_tier1.vert',
               'readonly buffer LightsData', 'uniform LightsData')
    else:
        change(source, 'shaders/vulkan/mech_tier1.vert',
               'set = 0, binding = 4', 'set = 0, binding = 42' if side == 'glsl'
               else 'set = 2, binding = 4')
    assert run('binding-slots', source, monkeypatch) == 1


def test_sets_are_distinct_but_descriptor_types_share_one_namespace(source, monkeypatch):
    path = source / 'shaders/vulkan/terrain_tier1.vert'
    with path.open('a', encoding='utf-8') as out:
        out.write('\nlayout(set=2, binding=1) uniform DifferentSet { vec4 fine; };\n')
    assert run('binding-slots', source, monkeypatch) == 0
    change(source, 'shaders/vulkan/terrain_tier1.vert', 'set=2, binding=1', 'set=0, binding=1')
    assert run('binding-slots', source, monkeypatch) == 1


def test_strict_rejects_unmodeled_stage_even_on_single_slot(source, monkeypatch):
    (source / 'shaders/vulkan/new.frag').write_text(
        'layout(set=9, binding=99) uniform Unknown { vec4 v; };\n', encoding='utf-8')
    assert run('vulkan-bindings', source, monkeypatch) == 1


def test_including_stage_set_override_is_preserved(source):
    checker = importlib.import_module('check-binding-slots')
    _, blocks = checker.scan_glsl(source)
    assert {b['set'] for b in blocks if b['block'] == 'TerrainFrameUBO' and
            b['file'] == 'shaders/vulkan/vfx_vegetation.frag'} == {3}


def test_qualified_images_ordered_macros_branches_and_saved_parity(source, monkeypatch, tmp_path):
    scanner = importlib.import_module('check-binding-slots')
    path = source / 'shaders/vulkan/terrain_tier1.vert'
    with path.open('a', encoding='utf-8') as out:
        out.write('''
#define TEST_SLOT 80
#if FEATURE
layout(set=0, binding=TEST_SLOT, rgba8) uniform writeonly highp image2D first;
#undef TEST_SLOT
#define TEST_SLOT 81
#else
layout(set=0, binding=TEST_SLOT, rgba8) readonly uniform highp image2D alternate;
#endif
#undef TEST_SLOT
#define TEST_SLOT 82
layout(set=0, binding=TEST_SLOT) uniform sampler2D last;
''')
    _, blocks = scanner.scan_glsl(source)
    selected = {b['block']: b for b in blocks if b['block'] in ('first', 'alternate', 'last')}
    assert {k: b['binding_token'] for k, b in selected.items()} == {
        'first': '80', 'alternate': '80', 'last': '82'}
    assert selected['last']['line'] <= len(path.read_text().splitlines())
    census = tmp_path / 'census.json'
    def saved():
        monkeypatch.setattr(sys, 'argv', ['check-binding-slots', '--root', str(source),
                                        '--json', str(census), '--quiet'])
        a = scanner.main()
        checker = importlib.import_module('check-vulkan-bindings')
        monkeypatch.setattr(sys, 'argv', ['check-vulkan-bindings', '--json', str(census), '--strict', '--quiet'])
        return a, checker.main()
    assert run('vulkan-bindings', source, monkeypatch) == 0
    assert saved() == (0, 0)
    change(source, 'shaders/vulkan/terrain_tier1.vert', '#define TEST_SLOT 80', '#define TEST_SLOT 0')
    assert run('vulkan-bindings', source, monkeypatch) == 1
    assert saved() == (1, 1)


def test_strict_rejects_descriptor_free_stage_in_live_and_saved_census(source, monkeypatch, tmp_path):
    (source / 'shaders/vulkan/unmodeled.vert').write_text(
        '#version 450\nvoid main() { gl_Position = vec4(0); }\n', encoding='utf-8')
    assert run('vulkan-bindings', source, monkeypatch) == 1
    census = tmp_path / 'census.json'
    monkeypatch.setattr(sys, 'argv', ['check-binding-slots', '--root', str(source),
                                    '--json', str(census), '--quiet'])
    assert importlib.import_module('check-binding-slots').main() == 0
    monkeypatch.setattr(sys, 'argv', ['check-vulkan-bindings', '--json', str(census), '--strict', '--quiet'])
    assert importlib.import_module('check-vulkan-bindings').main() == 1


def test_macro_states_reject_dead_elif_and_commented_guards():
    scanner = importlib.import_module('check-binding-slots')
    text = """#define GUARD 1
#ifdef GUARD // comment
#define LIVE 4
#endif
#ifndef GUARD /* comment */
#define DEAD 8
#endif
#if 0
#define DEAD 9
#elif 0
#define DEAD 10
#elif 1
#define GOOD 11
#else
#define DEAD 12
#endif
"""
    states = scanner.macro_states([(s, Path('probe'), n) for n, s in enumerate(text.splitlines(), 1)])
    assert states[10] is None
    assert states[14] is None
    assert states[16] == {'GUARD': '1', 'LIVE': '4', 'GOOD': '11'}


def test_terrain_specialization_constant_abi():
    import re
    from pathlib import Path
    text = (Path(__file__).resolve().parents[2] / "shaders/vulkan/terrain_tier1.frag").read_text()
    constants = {int(i): int(v) for i, v in re.findall(
        r"layout\(constant_id\s*=\s*(\d+)\)\s*const int \w+\s*=\s*(-?\d+)", text)}
    assert {i: constants[i] for i in (0,10,11,12,13,14)} == {0:0,10:0,11:1,12:1,13:1,14:1}
    assert all(constants[i] == -1 for i in range(20,32))


def test_async_pipeline_diagnostics_use_captured_frame_without_context_wait():
    # The compile completion holds infra.mutex while present may need bind().
    # A context accessor here waits for present and closes that lock cycle.
    infra = (ROOT / "GameOS/gameos/vk_render_infra.cpp").read_text()
    for kind, end in (("graphics", "VkResult compute"), ("compute", "void setSceneViewport")):
        body = infra.split(f"VkResult {kind}(", 1)[1].split(end, 1)[0]
        locked = body.split("std::unique_lock lock(s.mutex)", 1)[1]
        assert "getOrCreateEngineVulkanContext" not in locked
        assert "engineVulkanContextNoWait" not in locked
        assert f'breadcrumb("pipeline","{kind}",r->hash,VK_NULL_HANDLE,frame)' in locked
        assert "mc2::fmt::as_u64(frame)" in locked
    terrain = (ROOT / "GameOS/gameos/vulkan_terrain_island_run.cpp").read_text()
    assert "target != nullptr, false, rctx.tracker().frameNo()" in terrain
    builder = (ROOT / "GameOS/gameos/vulkan_terrain_island_island.cpp").read_text()
    assert "&gp, nullptr, h, r.frame)" in builder
