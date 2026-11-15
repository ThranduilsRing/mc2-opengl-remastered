"""Verified owner seed: no cook, private mutable files, cached stock hashes."""
import csv
import hashlib
import json
import os
from pathlib import Path
import stat
import time

import pytest
import kit_payload as kit
import deploy_payload as deploy


def source_tree(root):
    source = root / 'source'; source.mkdir()
    members = {'mc2.exe': b'EXE', 'shaders/test.frag': b'shader',
               'mods/mc2-industrial-kit/model.glb': b'kit'}
    with (source / '.deployed_manifest.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['manifest_version', 'v1'])
        writer.writerow(['relpath', 'sha256', 'bytes', 'src_commit'])
        for name, data in members.items():
            path = source / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            writer.writerow([name, hashlib.sha256(data).hexdigest(), len(data), 'abc'])
    data = source / 'data/stock.bin'; data.parent.mkdir(); data.write_bytes(b'stock' * 100000)
    cache = source / 'cache/content-model'; cache.mkdir(parents=True)
    recipe = cache / 'model-0123456789abcdef.mci'; recipe.write_bytes(b'recipe')
    for name in ('industrial-cook.json', 'trees-cook.json'):
        (cache / name).write_text(json.dumps({'files': [dict(
            path=recipe.name, bytes=recipe.stat().st_size, sha256=kit.sha(recipe))]}))
    return source


def test_seed_cold_warm_hash_cache_and_private_mutables(tmp_path, monkeypatch):
    source = source_tree(tmp_path); target = tmp_path / 'owner'; cache = tmp_path / 'hashes.json'
    begin = time.monotonic(); kit.seed_install(source, target, cache); cold = time.monotonic() - begin
    assert (target / 'mods/mc2-industrial-kit/model.glb').read_bytes() == b'kit'
    assert (target / 'cache/content-model/model-0123456789abcdef.mci').read_bytes() == b'recipe'
    assert (target / 'cache/content-model/trees-cook.json').is_file()
    assert not os.path.samefile(source / 'mc2.exe', target / 'mc2.exe')
    assert not os.path.samefile(source / '.deployed_manifest.csv', target / '.deployed_manifest.csv')
    original = kit.sha; calls = []
    def counted(path):
        calls.append(Path(path)); return original(path)
    monkeypatch.setattr(kit, 'sha', counted)
    begin = time.monotonic(); kit.seed_install(source, target, cache); warm = time.monotonic() - begin
    assert source / 'data/stock.bin' not in calls
    assert source / 'mc2.exe' in calls and target / 'mc2.exe' in calls
    assert source / 'shaders/test.frag' in calls and target / 'shaders/test.frag' in calls
    print(f'fixture seed cold={cold:.4f}s warm={warm:.4f}s')
    # A stale cache key must invalidate on changed bytes/mtime, even at the same size.
    (source / 'mc2.exe').write_bytes(b'BAD')
    with pytest.raises(ValueError, match='manifest mismatch'):
        kit.seed_install(source, target, cache)


def test_seed_rehashes_manifested_bytes_with_preserved_stamp(tmp_path):
    source = source_tree(tmp_path); target = tmp_path / 'owner'; cache = tmp_path / 'hashes.json'
    kit.seed_install(source, target, cache)
    shader = source / 'shaders/test.frag'
    stamp = shader.stat()
    shader.write_bytes(b'BROKEN')
    os.utime(shader, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    with pytest.raises(ValueError, match='manifest mismatch'):
        kit.seed_install(source, target, cache)


def test_seed_breaks_preexisting_executable_hardlinks(tmp_path):
    source = source_tree(tmp_path); target = tmp_path / 'owner'; target.mkdir()
    os.link(source / 'mc2.exe', target / 'mc2.exe')
    os.link(source / '.deployed_manifest.csv', target / '.deployed_manifest.csv')
    kit.seed_install(source, target, tmp_path / 'hashes.json')
    assert (source / 'mc2.exe').stat().st_nlink == 1
    assert (source / '.deployed_manifest.csv').stat().st_nlink == 1


def test_seed_rejects_manifest_escape(tmp_path):
    source = source_tree(tmp_path)
    with (source / '.deployed_manifest.csv').open('a') as stream:
        stream.write('../escape,wrong,1,abc\n')
    with pytest.raises(ValueError, match='unsafe'):
        kit.seed_install(source, tmp_path / 'owner', tmp_path / 'hashes.json')


def test_seed_refresh_retires_only_unmodified_owned_members(tmp_path):
    source = source_tree(tmp_path); target = tmp_path / 'owner'; cache = tmp_path / 'hashes.json'
    kit.seed_install(source, target, cache)
    name = 'mods/mc2-industrial-kit/model.glb'
    data = (target / name).read_bytes()
    manifest = source / '.deployed_manifest.csv'
    manifest.write_text('\n'.join(line for line in manifest.read_text().splitlines() if not line.startswith(name)) + '\n')
    (target / name).write_bytes(b'user edit')
    with pytest.raises(ValueError, match='modified retired'):
        kit.seed_install(source, target, cache)
    (target / name).write_bytes(data)
    kit.seed_install(source, target, cache)
    assert not (target / name).exists()


@pytest.mark.parametrize('broken', [None, 'exe', 'shader', 'manifest', 'extra', 'fingerprint', 'swapped_nonexe'])
def test_published_identity_checks_head_and_actual_bytes(tmp_path, monkeypatch, broken):
    import exe_fingerprint
    source = source_tree(tmp_path)
    repo = tmp_path / 'repo'
    for rel, data in {'build64-ninja/mc2.exe': b'EXE', 'shaders/test.frag': b'shader',
                      'build64-ninja/shaders/vulkan/test.spv': b'SPV'}.items():
        path = repo / rel; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    path = source / 'shaders/vulkan/test.spv'; path.parent.mkdir(); path.write_bytes(b'SPV')
    manifest = source / '.deployed_manifest.csv'
    manifest.write_text(manifest.read_text().replace('abc', 'a' * 40) +
                        f'shaders/vulkan/test.spv,{kit.sha(path)},3,{"a" * 40}\n')
    monkeypatch.setattr(exe_fingerprint, 'extract', lambda p: dict(present=True, dirty=0,
        sha='b' * 40 if broken == 'fingerprint' and p == source / 'mc2.exe' else 'a' * 40))
    if broken in ('exe', 'shader'):
        (source / ('mc2.exe' if broken == 'exe' else 'shaders/test.frag')).write_bytes(b'BAD')
    if broken == 'swapped_nonexe':  # same size, so the size+mtime fast path would never notice
        (source / 'mods/mc2-industrial-kit/model.glb').write_bytes(b'kiX')
    if broken == 'manifest':
        manifest.write_text(manifest.read_text().replace('a' * 40, 'b' * 40))
    if broken == 'extra':
        (source / 'shaders/extra.frag').write_bytes(b'old shader')
    if broken:
        with pytest.raises(ValueError):
            deploy.verify_published_identity(source, repo, 'a' * 40)
    else:
        deploy.verify_published_identity(source, repo, 'a' * 40)


def test_seed_cli_honors_explicit_source_target_and_repo(tmp_path, monkeypatch):
    import sys
    source, target, repo = (tmp_path / name for name in ('published', 'lane', 'repo'))
    seen = []
    monkeypatch.setattr(sys, 'argv', ['deploy_payload.py', str(target), '--seed-owner-test',
                                    '--seed-source', str(source), '--source-root', str(repo)])
    monkeypatch.setattr(deploy.subprocess, 'check_output', lambda cmd, **kw: 'head\n' if str(repo) in cmd else pytest.fail(str(cmd)))
    monkeypatch.setattr(deploy, 'verify_published_identity', lambda *args: seen.append(('verify', args)))
    monkeypatch.setattr(deploy, 'check_locks', lambda *args: None)
    monkeypatch.setattr(deploy, 'take_folder_lease', lambda *args: False)
    monkeypatch.setattr(kit, 'seed_install', lambda *args: seen.append(('seed', args)))
    deploy.main()
    assert seen == [('verify', (source, str(repo), 'head')), ('seed', (source, target))]


@pytest.mark.parametrize('readonly', [False, True])
def test_deploy_payload_replace_failure_preserves_original(tmp_path, monkeypatch, readonly):
    source = source_tree(tmp_path)
    payload = tmp_path / 'payload'
    payload.mkdir()
    (source / '.deployed_manifest.csv').rename(payload / '.deployed_manifest.csv')
    source.rename(payload / 'files')
    target = tmp_path / 'target'
    target.mkdir()
    dst = target / 'mc2.exe'
    dst.write_bytes(b'original executable')
    if readonly:
        dst.chmod(stat.S_IREAD)
    original_mode = dst.stat().st_mode

    def broken_replace(src, destination):
        assert Path(src).parent == Path(destination).parent
        raise OSError('injected replace failure')

    monkeypatch.setattr(deploy.os, 'replace', broken_replace)
    try:
        with pytest.raises(OSError, match='injected replace failure'):
            deploy.deploy_from_payload(payload, target, tmp_path, 'abc')
        assert dst.read_bytes() == b'original executable'
        assert dst.stat().st_mode == original_mode
        assert list(target.iterdir()) == [dst]
    finally:
        dst.chmod(stat.S_IREAD | stat.S_IWRITE)


@pytest.mark.parametrize('publish', [False, True])
def test_deploy_readonly_hardlink_preserves_peer(tmp_path, monkeypatch, publish):
    import exe_fingerprint
    source = source_tree(tmp_path)
    payload = tmp_path / 'payload'
    payload.mkdir()
    (source / '.deployed_manifest.csv').rename(payload / '.deployed_manifest.csv')
    source.rename(payload / 'files')
    target = tmp_path / 'target'
    target.mkdir()
    peer = tmp_path / 'peer.exe'
    peer.write_bytes(b'original executable')
    peer.chmod(stat.S_IREAD)
    dst = target / 'mc2.exe'
    os.link(peer, dst)
    original_stat = peer.stat()
    monkeypatch.setattr(exe_fingerprint, 'extract', lambda p: dict(present=True, dirty=0, sha='abc'))
    monkeypatch.setattr(exe_fingerprint, 'build_current', lambda *a: True)
    monkeypatch.setattr(deploy, 'ensure_terrain_materials', lambda t: {})
    monkeypatch.setattr(deploy, 'ensure_engine_normal_cooks', lambda t: None)
    try:
        if publish:
            deploy.deploy_from_payload(payload, target, tmp_path, 'abc')
        else:
            deploy.deploy([(str(payload / 'files/mc2.exe'), 'mc2.exe', 'exe')], str(target), False)
        assert dst.read_bytes() == b'EXE'
        assert not os.path.samefile(peer, dst)
        assert peer.read_bytes() == b'original executable'
        assert peer.stat().st_mode == original_stat.st_mode
        if os.name == 'nt':
            assert peer.stat().st_file_attributes == original_stat.st_file_attributes
    finally:
        peer.chmod(stat.S_IREAD | stat.S_IWRITE)
