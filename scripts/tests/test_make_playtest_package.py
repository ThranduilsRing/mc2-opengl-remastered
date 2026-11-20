"""Scoped to scripts/make_playtest_package.py and tools/playtest/."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import make_playtest_package as package


def test_permission_allowlist_and_unlisted_refusal(tmp_path):
    mod = tmp_path / 'mods/community'
    mod.mkdir(parents=True)
    (mod / 'mod.json').write_text(json.dumps({'id': 'community'}))
    (mod / 'credits.txt').write_text('Original author')
    record = {'community': dict(permission_source='Magic, Discord, 2026-11-15',
              terms='with attribution', author='Author', credits_file='mods/community/credits.txt',
              credits_sha256=package.sha256(mod / 'credits.txt'))}
    assert package.permitted_mods(tmp_path, ['community'], record) == record
    with pytest.raises(ValueError, match='unlisted'):
        package.permitted_mods(tmp_path, ['other'], record)
    record['community']['credits_file'] = '../escape.txt'
    with pytest.raises(ValueError, match='unsafe'):
        package.permitted_mods(tmp_path, ['community'], record)


@pytest.mark.parametrize('rel', ['mods/bt2018_mechs/mod.json', 'mods/sample_variant/mod.json',
    'data/tgl/128/chrTxrMech_atlas.ktx2', 'data/tgl/chrPrfMech_atlas.glb',
    'data/tgl/_aoktxtmp/a.tga', 'mc2.pdb', 'system.cfg.bak-precam', 'data/savegame/a.fit',
    'data/tgl/Archer_BHA.glb'])
def test_excluded_content(rel):
    assert package.excluded(rel)


@pytest.mark.parametrize('suffix', ['.cfg', '.json', '.ini', '.md', '.py', '.txt', '.bat'])
def test_scan_rejects_machine_paths_and_preserves_normal_assets(tmp_path, suffix):
    assert not package.excluded('data/tgl/128/atlas.ktx2')
    assert not package.excluded('tools/import_campaign.py')
    assert package.excluded('tools/cook_bc6h_hdri.py')
    config = tmp_path / ('settings' + suffix)
    config.write_text('savePath="data/savegame/"')
    package.scan(tmp_path)
    config.write_text('savePath="C:\\Users\\Owner\\savegame"')
    with pytest.raises(ValueError, match='machine path'):
        package.scan(tmp_path)


def test_clean_staging_bundles_only_permitted_mods(tmp_path, monkeypatch):
    monkeypatch.setattr(package, 'clean_head', lambda *args: '1' * 40)
    source, out = tmp_path / 'source', tmp_path / 'out'
    source.mkdir()
    for rel in package.STOCK_ROOT:
        (source / rel).write_text('stock')
    mod = source / 'mods/community'
    mod.mkdir(parents=True)
    (mod / 'mod.json').write_text('{"id":"community"}')
    (mod / 'credits.txt').write_text('Author')
    other = source / 'mods/unlisted'
    other.mkdir()
    (other / 'mod.json').write_text('{"id":"unlisted"}')
    (source / 'owner-save.ims').write_text('private')
    (source / '.deployed_manifest.csv').write_text(
        'manifest_version,v1\nrelpath,sha256,bytes,src_commit\n')
    record = {'community': dict(permission_source='Magic, Discord', terms='with attribution',
              author='Author', credits_file='mods/community/credits.txt',
              credits_sha256=package.sha256(mod / 'credits.txt'))}
    inventory = {rel: {'sha256': package.sha256(source / rel)} for rel in package.STOCK_ROOT}
    for name in ('mod.json', 'credits.txt'):
        inventory['mods/community/' + name] = {'sha256': package.sha256(mod / name)}
    package.assemble(source, out, record, ['community'], inventory=inventory)
    assert (out / 'mods/community/mod.json').exists()
    assert (out / 'credits/community.txt').read_text() == 'Author'
    assert not (out / 'mods/unlisted').exists()
    assert not (out / 'owner-save.ims').exists()
    assert 'kits included' in (out / 'PLAYTEST-README.txt').read_text()


def _two_mod_source(tmp_path, monkeypatch, bytes_a=b'one', bytes_b=b'two'):
    monkeypatch.setattr(package, 'clean_head', lambda *args: '1' * 40)
    source = tmp_path / 'source'
    source.mkdir()
    for rel in package.STOCK_ROOT:
        (source / rel).write_text('stock')
    (source / '.deployed_manifest.csv').write_text('manifest_version,v1\nrelpath,sha256,bytes,src_commit\n')
    record = {}
    inventory = {rel: {'sha256': package.sha256(source / rel)} for rel in package.STOCK_ROOT}
    for mod, data in (('mod-a', bytes_a), ('mod-b', bytes_b)):
        root = source / 'mods' / mod
        (root / 'data/tgl').mkdir(parents=True)
        (root / 'credits.txt').write_text('Author')
        (root / 'mod.json').write_text('{"id":"%s"}' % mod)
        (root / 'data/tgl/x.tgl').write_bytes(data)
        record[mod] = dict(permission_source='Magic', terms='with attribution', author='A',
                           credits_file=f'mods/{mod}/credits.txt',
                           credits_sha256=package.sha256(root / 'credits.txt'))
        for rel in ('credits.txt', 'mod.json', 'data/tgl/x.tgl'):
            inventory[f'mods/{mod}/{rel}'] = {'sha256': package.sha256(root / rel)}
    return source, record, inventory


@pytest.mark.parametrize('same, level', [(False, 'WARN'), (True, 'INFO')])
def test_cross_mod_logical_overlap_is_reported_not_blocking(tmp_path, monkeypatch, same, level):
    source, record, inventory = _two_mod_source(tmp_path, monkeypatch, b'one', b'one' if same else b'two')
    result = package.assemble(source, tmp_path / 'out', record, ['mod-a', 'mod-b'], inventory=inventory)
    assert [(o['level'], o['path'], o['mods']) for o in result['mod_overlaps']] == [
        (level, 'data/tgl/x.tgl', ['mod-a', 'mod-b'])]
    assert (tmp_path / 'out/mods/mod-a/data/tgl/x.tgl').read_bytes() == b'one'
    assert (tmp_path / 'out/mods/mod-b/data/tgl/x.tgl').exists()


def test_physical_path_claimed_twice_raises(tmp_path, monkeypatch):
    source, record, inventory = _two_mod_source(tmp_path, monkeypatch)
    record['mod-a']['files'] = {'mods/mod-a/data/tgl/x.tgl': 'a', 'mods/mod-a/DATA/tgl/x.tgl': 'a'}
    with pytest.raises(ValueError, match='claimed twice'):
        package.assemble(source, tmp_path / 'out', record, ['mod-a', 'mod-b'], inventory=inventory)


def test_mod_file_without_approval_raises(tmp_path, monkeypatch):
    source, record, inventory = _two_mod_source(tmp_path, monkeypatch)
    del inventory['mods/mod-b/data/tgl/x.tgl']
    with pytest.raises(ValueError, match='without approval'):
        package.assemble(source, tmp_path / 'out', record, ['mod-a', 'mod-b'], inventory=inventory)


def test_mod_file_with_mismatched_digest_raises(tmp_path, monkeypatch):
    source, record, inventory = _two_mod_source(tmp_path, monkeypatch)
    inventory['mods/mod-b/data/tgl/x.tgl'] = {'sha256': '0' * 64}
    with pytest.raises(ValueError, match='digest mismatch'):
        package.assemble(source, tmp_path / 'out', record, ['mod-a', 'mod-b'], inventory=inventory)


@pytest.mark.parametrize('failure', [None, 'folder', 'zip', 'exception', 'write', 'existing'])
def test_main_guards_both_outputs_and_removes_failed_zip(tmp_path, monkeypatch, failure):
    monkeypatch.setattr(package, 'clean_head', lambda *args: '1' * 40)
    out, receipt = tmp_path / 'out', tmp_path / 'receipt.json'
    archive, checksum = out.with_suffix('.zip'), out.with_suffix('.zip.sha256')
    permissions = tmp_path / 'permissions.json'
    permissions.write_text('{}')
    monkeypatch.setattr(sys, 'argv', ['packager', '--source', str(tmp_path / 'source'),
        '--out', str(out), '--receipt', str(receipt), '--permissions', str(permissions)])
    def assemble(*args, **kwargs):
        out.mkdir()
        (out / 'innocent.bin').write_bytes(
            package.release_asset_guard.TOKENS[0] if failure == 'folder' else b'clean')
        return {'head': '1' * 40, 'files': [], 'excluded': {}}
    monkeypatch.setattr(package, 'assemble', assemble)
    scan, calls = package.release_asset_guard.scan, []
    def guarded(path):
        calls.append(path)
        if path == archive:
            if failure == 'exception':
                raise OSError('unreadable zip')
            if failure == 'zip':
                with package.zipfile.ZipFile(archive, 'a') as zf:
                    zf.writestr('renamed.bin', package.release_asset_guard.TOKENS[0])
        return scan(path)
    monkeypatch.setattr(package.release_asset_guard, 'scan', guarded)
    if failure == 'write':
        def broken_write(*args):
            raise OSError('zip write interrupted')
        monkeypatch.setattr(package.zipfile.ZipFile, 'write', broken_write)
    if failure == 'existing':
        archive.write_bytes(b'keep existing package')
    if failure:
        with pytest.raises((ValueError, OSError)):
            package.main()
        assert not receipt.exists() and not checksum.exists()
        assert archive.exists() == (failure == 'existing')
        if failure == 'existing':
            assert archive.read_bytes() == b'keep existing package'
    else:
        package.main()
        assert receipt.exists() and checksum.exists() and archive.exists()
    assert calls == ([] if failure == 'existing' else [out] if failure in {'folder', 'write'} else [out, archive])


def test_bt2018_extractor_only_files_are_never_packaged(tmp_path, monkeypatch):
    source, record, inventory = _two_mod_source(tmp_path, monkeypatch)
    (source / 'data/tgl').mkdir(parents=True)
    (source / 'data/tgl/Zeus.glb').write_bytes(b'bt2018')
    inventory['data/tgl/Zeus.glb'] = {'sha256': package.sha256(source / 'data/tgl/Zeus.glb')}
    monkeypatch.setattr(package, '_BT2018', {'data/tgl/zeus.glb'}, raising=False)
    result = package.assemble(source, tmp_path / 'out', record, ['mod-a'], inventory=inventory)
    assert not (tmp_path / 'out/data/tgl/Zeus.glb').exists()
    assert result['excluded'][package.BT2018_REASON] == 1


def test_committed_bt2018_list_matches_inventory_exclusion():
    listed = [x for x in package.BT2018_LIST.read_text().splitlines() if x]
    inventory = json.loads(package.INVENTORY.read_text())
    assert len(listed) == 111 == inventory['excluded'][package.BT2018_REASON]
    assert not any(package.bt2018_extractor_only(rel) for rel in inventory['files'])
