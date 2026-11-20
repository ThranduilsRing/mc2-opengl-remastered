"""Provenance enforcement; triggers: scripts/make_playtest_package.py, tools/playtest/."""
import json
from pathlib import Path
import subprocess
import sys
import hashlib
import shutil
import struct
import zlib

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import make_playtest_package as package


@pytest.fixture(autouse=True)
def _isolated_generated(tmp_path, monkeypatch):
    # Tests never read a shared generated inventory (env or TEMP fallback).
    monkeypatch.setattr(package, 'GENERATED', tmp_path / 'generated')
    monkeypatch.setattr(package, 'INVENTORY', tmp_path / 'generated/inventory.json')


@pytest.fixture
def repository(tmp_path):
    root = tmp_path / 'repo'
    root.mkdir()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), '-c', 'core.hooksPath=',
            '-c', 'commit.gpgsign=false', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
            *args], text=True, **package.console_child.child_kwargs(redirected=True)).strip()
    git('init', '-q')
    (root / 'anchor').write_text('committed')
    git('add', 'anchor')
    git('commit', '-qm', 'fixture')
    return root, git


def source_tree(tmp_path):
    source = tmp_path / 'source'
    source.mkdir()
    for rel in package.STOCK_ROOT:
        (source / rel).write_text('stock fixture')
    (source / '.deployed_manifest.csv').write_text(
        'manifest_version,v1\nrelpath,sha256,bytes,src_commit\n')
    inventory = {rel: {'sha256': package.sha256(source / rel)} for rel in package.STOCK_ROOT}
    return source, inventory


def mod(source, name):
    folder = source / 'mods' / name
    folder.mkdir(parents=True)
    (folder / 'mod.json').write_text(json.dumps({'id': name}))
    (folder / 'credits.txt').write_text('Community author')
    return dict(permission_source='Author approval', author='Community author',
                terms='with attribution', credits_file=f'mods/{name}/credits.txt',
                credits_sha256=package.sha256(folder / 'credits.txt'))


def approve(source, inventory, name):
    """Generator-style approval rows (physical path -> sha256) for a fixture mod."""
    for path in (source / 'mods' / name).rglob('*'):
        if path.is_file():
            inventory[path.relative_to(source).as_posix()] = {'sha256': package.sha256(path)}


@pytest.mark.parametrize('rel', ['data/tgl/MW5/atlas.glb', 'data/BT2018/test.bin',
    'assets/bt2018/test.bin', 'assets/HbS/test.bin', 'data/pGi/test.bin',
    'assets/model.UASSET', 'assets/content.PAK', 'data/content.assetbundle',
    'data/mw5-import/model.bin', 'data/bt2018_mechs/model.bin',
    'data/a.uexp', 'data/a.ubulk', 'data/a.umap', 'data/a.utoc', 'data/a.ucas'])
def test_hard_deny_before_hash_or_copy(tmp_path, repository, monkeypatch, rel):
    source, inventory = source_tree(tmp_path)
    record = {'community': mod(source, 'community')}
    path = source / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'forbidden fixture')
    inventory[rel] = '0' * 64  # Neither stock nor explicit permission may override the deny.
    record['community']['files'] = {rel: '0' * 64}
    def unread(*args):
        pytest.fail('denied input must be rejected before hashing')
    monkeypatch.setattr(package, 'sha256', unread)
    out = tmp_path / 'out'
    with pytest.raises(ValueError, match='hard-denied'):
        package.assemble(source, out, record, ['community'], inventory=inventory, repository=repository[0])
    assert not out.exists()


@pytest.mark.parametrize('changed,tree', [(False, 'data'), (True, 'data'), (False, 'Data'), (False, 'Assets')])
def test_unknown_or_changed_stock_refused(tmp_path, repository, changed, tree):
    source, inventory = source_tree(tmp_path)
    path = source / tree / 'stock.txt'
    path.parent.mkdir()
    path.write_text('original')
    if changed:
        inventory[f'{tree}/stock.txt'] = {'sha256': package.sha256(path)}
        path.write_text('changed')
    with pytest.raises(ValueError, match='unapproved stock path/hash'):
        package.assemble(source, tmp_path / 'out', {}, [], inventory=inventory, repository=repository[0])
    assert not (tmp_path / 'out').exists()


def test_permitted_mod_and_explicit_loose_file_pass(tmp_path, repository):
    source, inventory = source_tree(tmp_path)
    record = {'community': mod(source, 'community')}
    path = source / 'mods/community/data/approved.txt'
    path.parent.mkdir()
    path.write_text('approved')
    loose = source / 'assets/approved.txt'
    loose.parent.mkdir()
    loose.write_text('approved loose asset')
    record['community']['files'] = {'assets/approved.txt': package.sha256(loose)}
    inventory['assets/approved.txt'] = {'sha256': package.sha256(loose)}
    approve(source, inventory, 'community')
    result = package.assemble(source, tmp_path / 'out', record, ['community'],
                              inventory=inventory, repository=repository[0])
    assert (tmp_path / 'out/mods/community/data/approved.txt').read_text() == 'approved'
    assert (tmp_path / 'out/assets/approved.txt').read_text() == 'approved loose asset'
    assert result['head'] == repository[1]('rev-parse', 'HEAD')
    assert all(r['source'] in {'permitted-mod', 'stock-inventory'} for r in result['files']
               if r['relpath'].endswith('approved.txt'))


def test_cross_mod_logical_overlap_warns_not_blocks(tmp_path, repository):
    source, inventory = source_tree(tmp_path)
    records = {}
    for name, dest in [('first', 'data/shared.txt'), ('second', 'Data/SHARED.txt')]:
        records[name] = mod(source, name)
        path = source / 'mods' / name / dest
        path.parent.mkdir()
        path.write_text(name)
        approve(source, inventory, name)
    result = package.assemble(source, tmp_path / 'out', records, list(records),
                              inventory=inventory, repository=repository[0])
    assert [o['level'] for o in result['mod_overlaps']] == ['WARN']


@pytest.mark.parametrize('dirty', ['tracked', 'untracked', 'staged'])
def test_receipt_requires_clean_head(tmp_path, repository, dirty):
    root, git = repository
    source, inventory = source_tree(tmp_path)
    result = package.assemble(source, tmp_path / 'clean', {}, [], inventory=inventory, repository=root)
    assert result['head'] == git('rev-parse', 'HEAD')
    with (tmp_path / 'clean/.deployed_manifest.csv').open() as stream:
        assert result['head'] not in stream.read()  # rows keep the source's own src_commit
    (root / ('new' if dirty == 'untracked' else 'anchor')).write_text('dirty')
    if dirty == 'staged':
        git('add', 'anchor')
    with pytest.raises(ValueError, match='clean tree'):
        package.assemble(source, tmp_path / 'dirty', {}, [], inventory=inventory, repository=root)
    assert not (tmp_path / 'dirty').exists()


def test_excluded_kit_cannot_be_requested(tmp_path):
    with pytest.raises(ValueError, match='forbidden mod'):
        package.permitted_mods(tmp_path, ['kit'], {'kit': {'bundle': False}})


@pytest.mark.parametrize('header,allowed', [(b'\xce\xfa\xed\xfe', True),
    (b'PACK', False), (b'', False), (b'\xce\xfa', False)])
def test_packet_header_allowlist(tmp_path, header, allowed):
    path = tmp_path / 'stock.PAK'
    path.write_bytes(header + b'payload')
    if allowed:
        assert package.safe_path(tmp_path, path.name) == path
        package.scan(tmp_path)
    else:
        with pytest.raises(ValueError, match='hard-denied PAK header'):
            package.safe_path(tmp_path, path.name)


def test_denied_prefix_precedes_packet_header_read(tmp_path, monkeypatch):
    folder = tmp_path / 'MW5-content'
    folder.mkdir()
    (folder / 'asset.pak').write_bytes(b'\xce\xfa\xed\xfe')
    def unread(*args, **kwargs):
        pytest.fail('denied prefix must precede header read')
    monkeypatch.setattr(Path, 'open', unread)
    with pytest.raises(ValueError, match='hard-denied path'):
        package.safe_path(tmp_path, 'MW5-content/asset.pak')


@pytest.mark.parametrize('failure', [None, 'missing', 'changed', 'unpinned'])
def test_separate_readonly_credits_source(tmp_path, repository, failure):
    source, inventory = source_tree(tmp_path)
    credits = tmp_path / 'credits-source'
    credits.mkdir()
    record = mod(source, 'community')
    original = source / record['credits_file']
    external = credits / 'installer/author.txt'
    external.parent.mkdir()
    external.write_bytes(original.read_bytes())
    original.unlink()
    record['credits_file'] = 'installer/author.txt'
    if failure == 'missing':
        external.unlink()
    elif failure == 'changed':
        external.write_text('modified')
    elif failure == 'unpinned':
        record.pop('credits_sha256')
    approve(source, inventory, 'community')
    out = tmp_path / 'out'
    if failure:
        with pytest.raises(ValueError, match='missing credits|credits hash mismatch'):
            package.assemble(source, out, {'community': record}, ['community'],
                inventory=inventory, repository=repository[0], credits_source=credits)
        assert not out.exists()
    else:
        package.assemble(source, out, {'community': record}, ['community'],
            inventory=inventory, repository=repository[0], credits_source=credits)
        assert package.sha256(out / 'credits/community.txt') == record['credits_sha256']
        assert not (out / 'installer').exists()


def test_desert_fox_eula_is_pinned_separately(tmp_path):
    record = {'credits_file': 'readme.txt'}
    (tmp_path / 'readme.txt').write_text('Author')
    record['credits_sha256'] = package.sha256(tmp_path / 'readme.txt')
    with pytest.raises(ValueError, match='missing credits'):
        package.verified_credits(tmp_path, 'desert-fox-478ca2f8', record)


@pytest.mark.parametrize('failure', [None, 'altered', 'missing-extract'])
def test_stock_superset_fst_rejects_unverified_member(tmp_path, failure):
    from tools.playtest.generate_inventory import verify_tgl
    extract = tmp_path / 'extract'
    extract.mkdir()
    (extract / 'extra.tga').write_bytes(b'extra')
    members = [('data/tgl/stock.tga', b'changed' if failure == 'altered' else b'stock'),
               ('data/tgl/absent.tga' if failure == 'missing-extract' else 'data/tgl/extra.tga', b'extra')]
    offset = 8 + 266 * len(members)
    data = struct.pack('<II', 0, len(members))
    for name, blob in members:
        data += struct.pack('<IIII250s', offset, len(blob), len(blob), 0, name.encode())
        offset += len(blob)
    data += b''.join(blob for _, blob in members)
    fst = tmp_path / 'tgl.fst'
    fst.write_bytes(data)
    kwargs = dict(manifest={'stock.tga': hashlib.md5(b'stock').hexdigest()},
                  pairs={(len(data), zlib.crc32(data))})
    if failure:
        with pytest.raises(ValueError, match='stock member missing/altered|lacks extract counterpart'):
            verify_tgl(fst, extract, **kwargs)
    else:
        result = verify_tgl(fst, extract, **kwargs)
        assert (result['members'], result['digest_matched'], result['extract_matched']) == (2, 1, 1)


def test_cont6_fixture_rules():
    from tools.playtest import generate_inventory as gen
    groups = dict(G1='polyhaven-trees', G2='polyhaven-trees', G3='repo-tracked',
        G4='cook-receipt', G5='unused-terrain-albedo', G6='transition-ktx2',
        G7='transition-lst', G8='editor-genmap', G9='deploy-stamp')
    rows = (package.PERMISSIONS.parent / 'unmatched_expected.tsv').read_text().splitlines()
    assert len(rows) == 262
    for line in rows:
        group, size, rel = line.split('\t')
        assert gen.matching_rule(rel, gen.tier_rules())['id'] == groups[group]
    with pytest.raises(ValueError, match='hard-denied'):
        gen.matching_rule('data/tgl/mw5/x/cook.json', gen.tier_rules())


@pytest.mark.parametrize('failure', ['tree', 'list-missing', 'list-machine', 'sibling', 'blob'])
def test_cont6_negative_checks(tmp_path, repository, monkeypatch, failure):
    from tools.playtest import generate_inventory as gen
    rules = {r['id']: r for r in gen.tier_rules()}
    rel, key = {
        'tree': ('data/model_overrides/island_tree_01/model.glb', 'polyhaven-trees'),
        'list-missing': ('data/textures/test.lst', 'transition-lst'),
        'list-machine': ('data/textures/test.lst', 'transition-lst'),
        'sibling': ('data/textures/12345678.12345678.ktx2', 'transition-ktx2'),
        'blob': ('anchor', 'repo-tracked')}[failure]
    source = tmp_path / 'source'
    path = source / rel
    path.parent.mkdir(parents=True)
    path.write_bytes(b'A:\\bad.txm\0' if failure == 'list-machine' else b'missing.txm\0')
    monkeypatch.setattr(package, 'CREDITS_SOURCE', source)
    monkeypatch.setattr(package, 'REPOSITORY', repository[0])
    with pytest.raises(ValueError, match='tree intentional|unshipped|machine path|TXM sibling|commit blob'):
        gen.check_rule(rel, dict(sha256=package.sha256(path)), rules[key], source,
            repository[1]('rev-parse', 'HEAD'), {rel: hashlib.sha256(b'original').hexdigest()}, {})


def test_cont6_synthetic_inventory(tmp_path, repository, monkeypatch):
    from tools.playtest import generate_inventory as gen
    source, golden, config = [tmp_path / n for n in ('source', 'golden', 'config')]
    for folder in (source, golden, config):
        folder.mkdir()
    rules = gen.tier_rules()
    # Synthetic stand-ins for what a real run reads from the install: a real play commit (the generator
    # reads intentional-asset-changes.json at it) and the stop-check tuple for tgl.fst.
    baseline = dict(executable_commit=subprocess.check_output(
        ['git', '-C', str(package.REPOSITORY), 'rev-parse', 'HEAD'], text=True,
        **package.console_child.child_kwargs(redirected=True)).strip(),
        tgl=dict(tier='B', members=4667, digest_matched=4212, extract_matched=455, bytes=34295854,
                 crc32='0xaef6a9dd', sha256='18e0f57996b212aac3fa8ddd36c5d73aa7e6fd14099e04934ae704bd6f0b9d14'))
    (config / 'tier_rules.json').write_text(json.dumps(rules))
    (config / 'kit_cc0_sources.json').write_text(json.dumps(
        json.loads((package.PERMISSIONS.parent / 'kit_cc0_sources.json').read_text())[:23]))
    (config / 'bundle_permissions.json').write_text('{}')
    (source / 'CREDITS.txt').write_text('fixture')
    for rel in ('art.fst', 'camera.fst', 'effect.fst', 'insignia.fst', 'misc.fst', 'mission.fst', 'textures.fst'):
        (source / rel).write_bytes(b'fixture')
        (golden / rel).write_bytes(b'fixture')
    tex = source / 'data/textures'
    tex.mkdir(parents=True)
    for name in ('12345678.12345678.txm', '12345678.12345678.ktx2', 'test.lst'):
        (tex / name).write_bytes(b'12345678.12345678.txm\0' if name.endswith('.lst') else b'fixture')
    admitted = {'data/tgl/32/a_cp_island_tree_01_1.tga': 'A',
        'data/textures/87654321.12345678.ktx2': 'B', 'data/textures/golden.lst': 'B'}
    pins = {}
    for rel, tier in admitted.items():
        path = source / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'already admitted')
        pins[rel] = package.sha256(path)
        if tier == 'B':
            reference = golden / rel
            reference.parent.mkdir(parents=True, exist_ok=True)
            reference.write_bytes(path.read_bytes())
    (source / '.deployed_manifest.csv').write_text(
        'manifest_version,v1\nrelpath,sha256,src_commit\nmc2.exe,unused,' + baseline['executable_commit'] + '\n'
        + 'data/tgl/32/a_cp_island_tree_01_1.tga,' + pins['data/tgl/32/a_cp_island_tree_01_1.tga'] + ',\n')
    canonical = tmp_path / 'canonical'
    shutil.copytree(source, canonical)  # a distinct parity reference; the same tree is refused
    monkeypatch.setattr(package, 'PERMISSIONS', config / 'bundle_permissions.json')
    monkeypatch.setattr(package, 'CREDITS_SOURCE', canonical)
    exe_calls = []
    monkeypatch.setattr(package, 'verified_exe_commit', lambda s, commit, *a, **k: exe_calls.append(commit) or commit)
    monkeypatch.setattr(package, 'GENERATED', config)
    monkeypatch.setattr(package, 'INVENTORY', config / 'inventory.json')
    monkeypatch.setattr(gen, 'verify_tgl', lambda *a: baseline['tgl'])
    (canonical / '.deployed_manifest.csv').write_text('manifest_version,v1\nrelpath,sha256,src_commit\n')
    with pytest.raises(ValueError, match='live v0.4 manifest'):
        gen.generate_inventory(source, golden, source)
    shutil.copyfile(source / '.deployed_manifest.csv', canonical / '.deployed_manifest.csv')
    result = gen.generate_inventory(source, golden, source)
    assert exe_calls == [baseline['executable_commit']] and result['parity_basis']['manifest_sha256']
    assert package.INVENTORY.is_file() and json.loads(package.INVENTORY.read_text())['status'] == 'PASS'
    assert result['unmatched'] == 0
    assert result['parity_counts'] == {'D5': 1, 'D6': 1, 'D7': 1}
    for rel, tier in admitted.items():
        assert result['files'][rel]['tier'] == tier and result['files'][rel]['sha256'] == pins[rel]
    classes = {r['id']: r for r in json.loads((config / 'parity_classes.json').read_text())}
    for name, cls in [('12345678.12345678.ktx2', 'D6'), ('test.lst', 'D7')]:
        row = result['files']['data/textures/' + name]
        assert row['tier'] == 'D' and row['parity_class'] == cls and row['evidence']
        assert classes[cls]['count'] == 1 and classes[cls]['action'] == 'ship'
    assert 'mc2-batch-cook' in classes['D6']['evidence']
    (source / 'data/zz').mkdir()
    (source / 'data/zz/unknown.bin').write_bytes(b'unmatched')
    stop = gen.generate_inventory(source, golden, source)
    assert stop['status'] == 'STOP' and not package.INVENTORY.exists()  # no inventory survives a STOP
