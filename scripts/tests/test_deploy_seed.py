"""Verified owner seed: no cook, private mutable files, cached stock hashes."""
import csv
import hashlib
import json
import os
from pathlib import Path
import time

import pytest
import kit_payload as kit


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
    (cache / 'industrial-cook.json').write_text(json.dumps({'files': [dict(
        path=recipe.name, bytes=recipe.stat().st_size, sha256=kit.sha(recipe))]}))
    return source


def test_seed_cold_warm_hash_cache_and_private_mutables(tmp_path, monkeypatch):
    source = source_tree(tmp_path); target = tmp_path / 'owner'; cache = tmp_path / 'hashes.json'
    begin = time.monotonic(); kit.seed_install(source, target, cache); cold = time.monotonic() - begin
    assert (target / 'mods/mc2-industrial-kit/model.glb').read_bytes() == b'kit'
    assert (target / 'cache/content-model/model-0123456789abcdef.mci').read_bytes() == b'recipe'
    assert not os.path.samefile(source / 'mc2.exe', target / 'mc2.exe')
    assert not os.path.samefile(source / '.deployed_manifest.csv', target / '.deployed_manifest.csv')
    original = kit.sha; calls = []
    def counted(path):
        calls.append(Path(path)); return original(path)
    monkeypatch.setattr(kit, 'sha', counted)
    begin = time.monotonic(); kit.seed_install(source, target, cache); warm = time.monotonic() - begin
    assert source / 'data/stock.bin' not in calls
    assert source / 'mc2.exe' in calls and target / 'mc2.exe' in calls
    print(f'fixture seed cold={cold:.4f}s warm={warm:.4f}s')
    # A stale cache key must invalidate on changed bytes/mtime, even at the same size.
    (source / 'mc2.exe').write_bytes(b'BAD')
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
