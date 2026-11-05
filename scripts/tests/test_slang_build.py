"""Slang -> SPIR-V path of tools/shader_offline_build/build_variants.py.

Needs slangc + spirv-cross (Vulkan SDK); skipped without. Fixtures live in
scripts/tests/fixtures/slang (NOT part of the shipped shader set)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, "fixtures", "slang")
BUILD = os.path.join(ROOT, "tools", "shader_offline_build", "build_variants.py")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools", "shader_offline_build"))
import console_child  # noqa: E402
import shader_common  # noqa: E402
import slang_compile  # noqa: E402

sdk = pytest.mark.skipif(
    not all(shader_common.find_tool(t) for t in ('slangc', 'spirv-cross', 'glslangValidator')),
    reason='Vulkan SDK tools unavailable')


@pytest.fixture()
def tmp_path():
    with tempfile.TemporaryDirectory(prefix='slang-test-') as td:
        yield Path(td)


def run_build(root, *extra):
    return subprocess.run(
        [sys.executable, "-I", BUILD, "--pilots", os.path.join(root, "pilots.json"),
         "--root", root, *extra], capture_output=True, text=True,
        **console_child.child_kwargs(redirected=True))


@pytest.fixture()
def work(tmp_path):
    if not all(shader_common.find_tool(t) for t in ('slangc', 'spirv-cross', 'glslangValidator')):
        pytest.skip('Vulkan SDK tools unavailable')
    dst = tmp_path / "fx"
    shutil.copytree(FIX, dst)
    return str(dst)


def sidecar(root, stage):
    with open(os.path.join(root, "out", f"slang_probe.{stage}.default.json"),
              encoding="utf-8") as f:
        return json.load(f)


@sdk
def test_version_meets_pin():
    assert slang_compile.check_version(slang_compile.find_slangc()) is None


def test_compiles_and_emits_package(work):
    r = run_build(work)
    assert r.returncode == 0, r.stdout + r.stderr
    out = os.path.join(work, "out")
    for n in ("spirv_index.json", "spirv_package.json"):
        assert os.path.isfile(os.path.join(out, n))
    pkg = json.load(open(os.path.join(out, "spirv_package.json"), encoding="utf-8"))
    assert "slangc" in pkg["tools"]
    idx = {x["key"]: x["artifact"]
           for x in json.load(open(os.path.join(out, "spirv_index.json"),
                                   encoding="utf-8"))["records"]}
    assert set(idx) == {"slang_probe|frag|", "slang_probe|comp|"}
    for art in idx.values():
        with open(os.path.join(out, art), "rb") as f:
            assert f.read(4) == b"\x03\x02\x23\x07"  # SPIR-V magic
    with open(os.path.join(out, idx["slang_probe|frag|"]), "rb") as f:
        assert f.read(8)[4:8] == b"\x00\x05\x01\x00"  # version word 1.5, same as glslang --target-env vulkan1.2
    assert run_build(work, "--check", "--quiet").returncode == 0


def test_frag_reflection_layout(work):
    assert run_build(work).returncode == 0
    b = sidecar(work, "frag")["bindings"]
    assert [(u["binding"], u["set"]) for u in b["ubos"]] == [(0, 0)]
    assert sorted((s["binding"], s["set"], s["type"]) for s in b["samplers"]) == [
        (1, 0, "texture2D"), (2, 0, "sampler")]
    assert sidecar(work, "frag")["language"] == "slang"


def test_comp_reflection_layout(work):
    assert run_build(work).returncode == 0
    ss = sidecar(work, "comp")["bindings"]["ssbos"]
    assert sorted((s["binding"], s["set"]) for s in ss) == [(0, 0), (1, 0)]


def test_broken_shader_fails_with_diagnostic(work):
    cfg = json.load(open(os.path.join(work, "pilots.json"), encoding="utf-8"))
    cfg["pilots"][0]["stages"] = {"frag": "broken.frag.slang"}
    json.dump(cfg, open(os.path.join(work, "pilots.json"), "w", encoding="utf-8"))
    r = run_build(work)
    assert r.returncode == 1
    text = r.stdout + r.stderr
    assert "slangc failed" in text and "undefined_symbol" in text
    assert not [f for f in os.listdir(os.path.join(work, "out")) if f.endswith(".spv")]


def test_artifact_checker_accepts_slang_and_detects_drift(work, tmp_path):
    """scripts/check-spirv-artifacts.py with a Slang pilot: in sync passes, edit fails."""
    with open(os.path.join(work, 'probe.comp.slang'), 'a', encoding='utf-8') as f:
        f.write('\nimport shared;\n')
    Path(work, 'shared.slang').write_text('static const int VALUE = 1;\n')
    assert run_build(work).returncode == 0
    mf = tmp_path / "tools" / "shader_offline_build"
    mf.mkdir(parents=True)
    shutil.copy(os.path.join(work, "pilots.json"), mf / "pilots.json")
    chk = os.path.join(ROOT, "scripts", "check-spirv-artifacts.py")

    def run(*extra):
        # pilots.json lives under <root>/tools/...; sources and out/ under <root>
        for item in os.listdir(work):
            dst = tmp_path / item
            if not dst.exists():
                (shutil.copytree if os.path.isdir(os.path.join(work, item))
                 else shutil.copy)(os.path.join(work, item), dst)
        return subprocess.run([sys.executable, "-I", chk, "--root", str(tmp_path),
                               *extra], capture_output=True, text=True,
                              **console_child.child_kwargs(redirected=True))

    assert run("--quiet").returncode == 0
    with open(tmp_path / "shared.slang", "a", encoding="utf-8") as f:
        f.write("// drift\n")
    r = run()
    assert r.returncode != 0 and "source drift" in (r.stdout + r.stderr)


def test_shared_file_entries_and_define_variants_produce_distinct_spirv(work):
    Path(work, 'multi.comp.slang').write_text('''
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> result;
[shader("compute")] [numthreads(1, 1, 1)]
void first(uint3 id : SV_DispatchThreadID) { result[id.x] = VALUE; }
[shader("compute")] [numthreads(1, 1, 1)]
void second(uint3 id : SV_DispatchThreadID) { result[id.x] = VALUE + 1; }
''')
    pilots = [{'program': name, 'stages': {'comp': 'multi.comp.slang'},
               'entries': {'comp': entry}, 'variants': [
                   {'name': 'one', 'defines': ['VALUE=1']},
                   {'name': 'two', 'defines': ['VALUE=2']}]} for name, entry in (
                       ('first_program', 'first'), ('second_program', 'second'))]
    Path(work, 'pilots.json').write_text(json.dumps({'spv_dir': 'out', 'pilots': pilots}))
    r = run_build(work)
    assert r.returncode == 0, r.stdout + r.stderr
    out = Path(work, 'out')
    meta = [json.loads((out / f'{name}.comp.{variant}.json').read_text())
            for name in ('first_program', 'second_program') for variant in ('one', 'two')]
    assert len({m['spirv_sha256'] for m in meta}) == 4
    assert {m['entry'] for m in meta} == {'first', 'second'}


def test_defines_entries_and_column_major_reach_slangc(tmp_path, monkeypatch):
    src, out = tmp_path / 'probe.slang', tmp_path / 'out.spv'
    out.write_bytes(b'stale')
    commands = []

    def run(cmd, **kwargs):
        assert not out.exists()
        commands.append(cmd)
        out.write_bytes(b'fresh')
        return SimpleNamespace(returncode=0, stdout='', stderr='')
    monkeypatch.setattr(slang_compile.subprocess, 'run', run)
    assert slang_compile.compile_slang('slangc', src, 'frag', out, 'pixel',
                                       defines=['COUNT=4', 'FEATURE'])[0]
    cmd = commands[0]
    assert '-DCOUNT=4' in cmd and '-DFEATURE' in cmd
    assert cmd[cmd.index('-entry') + 1] == 'pixel'
    assert '-matrix-layout-column-major' in cmd

    def timeout(*args, **kwargs):
        out.write_bytes(b'partial')
        raise subprocess.TimeoutExpired('slangc', 60)
    monkeypatch.setattr(slang_compile.subprocess, 'run', timeout)
    ok, diagnostic = slang_compile.compile_slang('slangc', src, 'frag', out)
    assert not ok and 'timed out' in diagnostic and not out.exists()


def test_import_and_include_edits_change_portable_hash(tmp_path):
    src = tmp_path / 'probe.frag.slang'
    src.write_text('import shared;\n#include "params.h"\n')
    (tmp_path / 'shared.slang').write_text('import nested;\n')
    nested = tmp_path / 'nested.slang'
    nested.write_text('import shared;\n')
    (tmp_path / 'params.h').write_text('const int N = 1;\n')
    before = slang_compile.source_bundle(src)
    copy = tmp_path / 'copy'
    copy.mkdir()
    for path in slang_compile.dependencies(src):
        shutil.copy(path, copy / path.name)
    assert slang_compile.source_bundle(copy / src.name) == before
    nested.write_text('import shared;\nconst int X = 2;\n')
    assert slang_compile.source_bundle(src) != before
    nested.unlink()
    with pytest.raises(ValueError, match='unresolved local dependency'):
        slang_compile.source_bundle(src)


def test_bindings_reject_implicit_duplicate_and_unknown_slots(tmp_path):
    src = tmp_path / 'probe.comp.slang'
    src.write_text('ConstantBuffer<P> params;\n')
    assert any('explicit vk::binding' in e for e in slang_compile.validate_reflection({}, src, tmp_path))
    src.write_text('[[vk::binding(8, 0)]] ConstantBuffer<P> params;\n')
    raw = {'ubos': [{'name': 'P', 'binding': 9, 'set': 0}]}
    assert any('no explicit source binding' in e for e in slang_compile.validate_reflection(raw, src, tmp_path))
    raw['separate_samplers'] = raw['ubos']
    assert any('duplicate descriptor' in e for e in slang_compile.validate_reflection(raw, src, tmp_path))
    raw = {'ubos': [{'name': 'P', 'binding': 9, 'set': 1, 'type': '_P'}],
           'types': {'_P': {'name': 'P', 'members': []}}}
    src.write_text('[[vk::binding(9, 1)]] ConstantBuffer<P> params;\n')
    assert slang_compile.validate_reflection(raw, src, tmp_path) == []
    raw = {'ssbos': raw['ubos']}
    assert any('source buffer type mismatch' in e for e in slang_compile.validate_reflection(raw, src, tmp_path))


def test_gpu_abi_rejects_wrong_offset_and_array_stride(tmp_path, monkeypatch):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import gpu_abi_gen as abi
    header = tmp_path / 'Gpu.h'
    header.write_text('// @gpu std430 glsl=Gpu\nstruct Gpu {\nfloat values[2];\n};\n')
    monkeypatch.setattr(abi, 'STRUCTS', [('Gpu.h', '', '')])
    src = tmp_path / 'probe.slang'
    src.write_text('')
    member = {'name': 'values', 'type': 'float', 'offset': 0, 'array': [2], 'array_stride': 4}
    raw = {'types': {'_1': {'name': 'Gpu', 'members': [member]}}}
    assert slang_compile.validate_reflection(raw, src, tmp_path) == []
    member['offset'] = 4
    assert any('offset' in e for e in slang_compile.validate_reflection(raw, src, tmp_path))
    member['offset'], member['array_stride'] = 0, 16
    assert any('stride' in e for e in slang_compile.validate_reflection(raw, src, tmp_path))


def test_binding_index_reports_slang_uses_per_set(tmp_path):
    sys.path.insert(0, os.path.join(ROOT, 'tools/repo_intel'))
    import binding_index
    shaders = tmp_path / 'shaders'
    shaders.mkdir()
    (shaders / 'probe.comp.slang').write_text(
        '[[vk::binding(3, 0)]] RWStructuredBuffer<float> result;\n'
        '[[vk::binding(3, 1)]] ConstantBuffer<P> params;\n')
    index = binding_index._grep_shader_bindings(tmp_path)
    assert index['ssbo', 3][0]['set'] == 0
    assert index['vk_set1_ubo', 3][0]['set'] == 1
    conflicts = binding_index.query_binding(tmp_path, conflicts=True)['conflicts']
    assert {c['namespace'] for c in conflicts} == {'ssbo', 'vk_set1_ubo'}


def test_vk_gate_routes_slang_without_glsl_compiler(tmp_path, monkeypatch, capsys):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import check_vk_shaders as gate
    (tmp_path / 'probe.comp.slang').write_text('')
    calls = []
    monkeypatch.setattr(gate, 'find_compiler', lambda *a: (None, None))
    monkeypatch.setattr(slang_compile, 'bake', lambda src, out: (calls.append(src) is None, ''))
    monkeypatch.setattr(sys, 'argv', ['check_vk_shaders', '--shader-dir', str(tmp_path)])
    with pytest.raises(SystemExit) as exit:
        gate.main()
    assert exit.value.code == 0 and calls == [tmp_path / 'probe.comp.slang']
    monkeypatch.setattr(slang_compile, 'bake', lambda *a: (False, 'bad binding'))
    with pytest.raises(SystemExit) as exit:
        gate.main()
    assert exit.value.code == 1 and 'bad binding' in capsys.readouterr().out


def test_cmake_bakes_slang_with_import_and_validator_dependencies():
    cmake = Path(ROOT, 'GameOS/gameos/CMakeLists.txt').read_text()
    assert '"${MC2_VK_SHADER_SRC_DIR}/*.comp.slang"' in cmake
    assert '"${_src}" -o "${_spv}"' in cmake
    assert 'DEPENDS ${MC2_SLANG_DEPENDS}' in cmake
    assert 'Duplicate GLSL/Slang shader output' in cmake


@sdk
def test_glsl_pilot_is_native_vulkan_and_artifact_checker_agrees(work):
    src = Path(work, 'native.comp')
    src.write_text("""#version 450
layout(local_size_x=1) in;
layout(set=1, binding=0, std430) buffer Out { uint value[]; } result;
void main() { result.value[0] = 42u; }
""", encoding='utf-8')
    cfg = {'spv_dir': 'out', 'pilots': [{'program': 'native_probe',
           'stages': {'comp': 'native.comp'}, 'variants': [{'name': 'default', 'defines': []}]}]}
    Path(work, 'pilots.json').write_text(json.dumps(cfg), encoding='utf-8')
    census = Path(work, 'docs/render-backend-seams/binding-slot-occupancy.json')
    census.parent.mkdir(parents=True)
    site = {'type': 'buffer', 'role': 'Out', 'file': 'native.comp'}
    census.write_text(json.dumps({'occupancy': {'set1:binding0': [site]}}))
    built = run_build(work)
    assert built.returncode == 0, built.stdout + built.stderr
    meta = json.loads(Path(work, 'out/native_probe.comp.default.json').read_text())
    assert meta['language'] == 'glsl' and meta['targetEnv'] == 'vulkan1.3'
    assert [(b['set'], b['binding']) for b in meta['bindings']['ssbos']] == [(1, 0)]
    manifest_dir = Path(work, 'tools/shader_offline_build')
    manifest_dir.mkdir(parents=True)
    (manifest_dir / 'pilots.json').write_text(json.dumps(cfg), encoding='utf-8')
    checked = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'scripts/check-spirv-artifacts.py'),
                              '--root', work, '--quiet'], capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert run_build(work, '--check', '--quiet').returncode == 0
    def check_variants():
        return subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'scripts/check-spirv-reflection-contract.py'),
                               '--root', work], capture_output=True, text=True,
                              **console_child.child_kwargs(redirected=True))
    assert check_variants().returncode == 0
    census.write_text(json.dumps({'occupancy': {'set0:binding0': [site]}}))
    rejected = run_build(work, '--check')
    assert rejected.returncode == 1 and 'binding-slot-occupancy.json (drift)' in rejected.stdout
    assert check_variants().returncode == 1
