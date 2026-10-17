"""Install hygiene: pinned terrain materials, complete FSTs, and fatal texture diagnostics."""
import hashlib
import json
from pathlib import Path
import struct
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
from scripts import deploy_payload as dp
from scripts.smoke_lib import gates, logparse
import fst_repack_drop
import residency_slim


@pytest.fixture
def materials(tmp_path, monkeypatch):
    source = tmp_path / 'golden'
    manifest = {}
    paths = ([f'data/textures/mat{i}_normal.tga' for i in range(9)] +
             [f'data/textures/mat{i}_displacement.tga' for i in range(4)] +
             ['data/textures/detail_displacement.tga'])
    for i, rel in enumerate(paths):
        path = source / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes([i]) * 32)
        manifest[rel] = hashlib.sha1(path.read_bytes()).hexdigest()
    pin = tmp_path / 'materials.json'
    pin.write_text(json.dumps(manifest))
    monkeypatch.setattr(dp, 'MATERIAL_MANIFEST', pin)
    monkeypatch.setattr(dp, 'GOLDEN_MATERIAL_ROOT', source)
    return source, manifest


def test_deploy_repairs_missing_and_wrong_materials_then_noop(tmp_path, materials, monkeypatch):
    source, manifest = materials
    target = tmp_path / 'target'
    bad = target / 'data/textures/mat6_normal.tga'
    bad.parent.mkdir(parents=True)
    bad.write_bytes(b'wrong')
    before = {rel: (source / rel).read_bytes() for rel in manifest}
    hashes = dp.deploy([], target, False)
    assert set(hashes) == set(manifest)
    for rel, expected in manifest.items():
        assert hashlib.sha1((target / rel).read_bytes()).hexdigest() == expected
        assert (source / rel).read_bytes() == before[rel]
    stamps = {rel: (target / rel).stat().st_mtime_ns for rel in manifest}
    def no_copy(*args):
        pytest.fail('correct normals must not be rewritten')
    monkeypatch.setattr(dp.os, 'replace', no_copy)
    dp.deploy([], target, False)
    assert stamps == {rel: (target / rel).stat().st_mtime_ns for rel in manifest}


@pytest.mark.parametrize('rel', ['mat0_normal.tga', 'mat0_displacement.tga',
                                  'detail_displacement.tga'])
@pytest.mark.parametrize('corrupt', [False, True])
def test_material_source_failure_refuses_deploy(tmp_path, materials, corrupt, rel):
    source, _ = materials
    path = source / 'data/textures' / rel
    if corrupt:
        path.write_bytes(b'changed golden')
    else:
        path.unlink()
    with pytest.raises(SystemExit):
        dp.deploy([], tmp_path / 'target', False)


def test_correct_target_does_not_need_golden(tmp_path, materials):
    source, manifest = materials
    target = tmp_path / 'target'
    dp.ensure_terrain_materials(target)
    for rel in manifest:
        (source / rel).unlink()
    dp.ensure_terrain_materials(target)


def archive(path, name):
    payload = b'stock bytes'
    path.write_bytes(struct.pack('<II', 0xCADDECAF, 1) +
                     struct.pack('<IIII', 274, len(payload), len(payload), 0) +
                     name.encode().ljust(250, b'\0') + payload)


@pytest.mark.parametrize('dry_run', [False, True])
def test_old_repacker_refuses_removal_before_writing(tmp_path, dry_run):
    src, dst = tmp_path / 'stock.fst', tmp_path / 'out.fst'
    archive(src, 'data/tgl/128/prop.tga')
    before = src.read_bytes()
    dst.write_bytes(b'existing destination')
    with pytest.raises(ValueError, match='FST entry removal is forbidden'):
        fst_repack_drop.repack(src, dst, dry_run)
    assert src.read_bytes() == before
    assert dst.read_bytes() == b'existing destination'


def test_residency_api_refuses_removal(tmp_path):
    with pytest.raises(ValueError, match='FST entry removal is forbidden'):
        residency_slim.repack_fst(tmp_path / 'missing', tmp_path / 'out', {'data/tgl/128/prop.tga'})
    assert not (tmp_path / 'out').exists()


def test_slim_apply_restore_only_loose_files(tmp_path, monkeypatch):
    d128 = tmp_path / 'data/tgl/128'
    d128.mkdir(parents=True)
    (d128 / 'prop.tga').write_bytes(b'loose override')
    (d128 / 'prop.ktx2').write_bytes(b'ktx')
    fst = tmp_path / 'tgl.fst'
    archive(fst, 'data/tgl/128/prop.tga')
    original = fst.read_bytes()
    # A legacy backup must not overwrite today's complete archive on restore.
    backup = tmp_path / 'tgl.fst.preslim'
    backup.write_bytes(b'legacy archive')
    trace = tmp_path / 'trace.log'
    trace.write_text('[TEXLOAD] uniq=0 name=data/tgl/128/prop.tga\n')
    monkeypatch.setattr(sys, 'argv', ['slim', str(tmp_path), '--apply', '--trace', str(trace)])
    residency_slim.main()
    assert not (d128 / 'prop.tga').exists()
    assert fst.read_bytes() == original
    monkeypatch.setattr(sys, 'argv', ['slim', str(tmp_path), '--restore'])
    residency_slim.main()
    assert (d128 / 'prop.tga').read_bytes() == b'loose override'
    assert fst.read_bytes() == original
    assert backup.read_bytes() == b'legacy archive'


@pytest.mark.parametrize('line,bucket', [
    ('[SPLATTING] FAILED to load all material textures', 'splatting_failed'),
    ('[TEXMGR_MISSING] path=data/tgl/128/missing.tga', 'texmgr_missing'),
])
def test_texture_diagnostics_fail_otherwise_passing_smoke(line, bucket):
    clean = '[INSTR v1] enabled: test\n[SMOKE v1] event=summary result=pass\n'
    def evaluate(text):
        return gates.evaluate(logparse.parse_log(text), gates.GateConfig(), exit_code=0, walltime_s=1)
    assert evaluate(clean).passed
    verdict = evaluate(clean + line + '\n')
    assert not verdict.passed
    assert bucket in verdict.buckets


def test_splatting_failure_cannot_be_waived():
    from scripts.smoke_lib import manifest
    entries = manifest.parse_manifest(ROOT / 'tests/smoke/smoke_missions.txt')
    exceptions = {(e.tier, e.stem) for e in entries
                  if e.marker_overrides.get('splatting_failed')}
    assert not exceptions
    assert 'allow_splatting_failed' not in manifest.MARKER_OVERRIDE_KEY_TO_BUCKET
    summary = logparse.parse_log('[INSTR v1] enabled: test\n'
                                 '[SMOKE v1] event=summary result=pass\n'
                                 '[SPLATTING] FAILED to load all material textures\n')
    cfg = gates.GateConfig(marker_overrides={'splatting_failed': True})
    assert not gates.evaluate(summary, cfg, exit_code=0, walltime_s=1).passed
    summary = logparse.parse_log('[INSTR v1] enabled: test\n'
                                 '[SMOKE v1] event=summary result=pass\n'
                                 '[TEXMGR_MISSING] missing.tga\n')
    assert not gates.evaluate(summary, cfg, exit_code=0, walltime_s=1).passed
