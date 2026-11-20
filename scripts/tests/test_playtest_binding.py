"""Inventory and exe-commit binding negatives; triggers: scripts/make_playtest_package.py, scripts/smoke_lib/child_registry.py."""
import csv
import json
from pathlib import Path
import subprocess
import sys

import pytest

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import exe_fingerprint
import make_playtest_package as package
from smoke_lib import child_registry


@pytest.fixture(autouse=True)
def _isolated_generated(tmp_path, monkeypatch):
    # Tests never read a shared generated inventory (env or TEMP fallback).
    monkeypatch.setattr(package, 'GENERATED', tmp_path / 'generated')
    monkeypatch.setattr(package, 'INVENTORY', tmp_path / 'generated/inventory.json')


@pytest.fixture
def repo(tmp_path):
    root = tmp_path / 'repo'
    root.mkdir()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), '-c', 'core.hooksPath=',
            '-c', 'commit.gpgsign=false', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
            *args], text=True, **package.console_child.child_kwargs(redirected=True)).strip()
    git('init', '-q', '-b', 'play')
    (root / 'anchor').write_text('a')
    git('add', 'anchor')
    git('commit', '-qm', 'A')
    a = git('rev-parse', 'HEAD')
    git('checkout', '-qb', 'other')
    (root / 'anchor').write_text('b')
    git('commit', '-qam', 'B')
    return root, a, git('rev-parse', 'HEAD'), git


def _fingerprint(monkeypatch, **fp):
    monkeypatch.setattr(exe_fingerprint, 'extract', lambda path: dict(present=True, dirty=False, **fp))


def test_exe_commit_accepts_clean_matching_ancestor(tmp_path, repo, monkeypatch):
    root, a, b, _ = repo
    _fingerprint(monkeypatch, sha=a[:12])
    assert package.verified_exe_commit(tmp_path, a, root, 'play') == a


@pytest.mark.parametrize('case, match', [
    ('non-ancestor', 'not an ancestor'), ('mismatch', 'manifest src_commit'),
    ('dirty', 'dirty'), ('absent', 'unreadable'), ('no-manifest-commit', 'manifest src_commit'),
    ('tilde', 'manifest src_commit'), ('short', 'manifest src_commit')])
def test_exe_commit_negative(tmp_path, repo, monkeypatch, case, match):
    root, a, b, _ = repo
    manifest = {'non-ancestor': b, 'mismatch': b, 'dirty': a, 'absent': a, 'no-manifest-commit': '',
                'tilde': a + '~500', 'short': a[:12]}[case]
    sha = {'non-ancestor': b, 'mismatch': a, 'dirty': a, 'absent': a, 'no-manifest-commit': a,
           'tilde': a, 'short': a}[case]
    if case == 'absent':
        monkeypatch.setattr(exe_fingerprint, 'extract', lambda path: {'present': False, 'reason': 'banner-absent'})
    else:
        _fingerprint(monkeypatch, sha=sha[:12])
        if case == 'dirty':
            monkeypatch.setattr(exe_fingerprint, 'extract',
                                lambda path: dict(present=True, dirty=True, sha=a[:12]))
    with pytest.raises(ValueError, match=match):
        package.verified_exe_commit(tmp_path, manifest, root, 'play')


def _inventory(tmp_path, source, **override):
    refs = override.pop('references', {'credits_source': 'v04'})
    if not (source / '.deployed_manifest.csv').exists():
        (source / '.deployed_manifest.csv').write_text('manifest_version,v1\nrelpath,sha256,bytes,src_commit\n')
    data = dict(status='PASS', source_manifest_sha256=package.sha256(source / '.deployed_manifest.csv'),
                executable_commit='c' * 40, files={}, references=refs)
    data.update(override)
    path = tmp_path / 'inventory.json'
    path.write_text(json.dumps(data))
    return path


def test_inventory_binding(tmp_path):
    source = tmp_path / 'source'
    source.mkdir()
    data, digest = package.bound_inventory(source, 'c' * 40, 'v04', _inventory(tmp_path, source))
    assert data['status'] == 'PASS' and digest == package.sha256(tmp_path / 'inventory.json')
    for override, match in [(dict(status='STOP'), 'not PASS'),
                            (dict(source_manifest_sha256='0' * 64), 'different source manifest'),
                            (dict(executable_commit='d' * 40), 'executable_commit'),
                            (dict(references={'credits_source': 'other'}), 'parity reference')]:
        with pytest.raises(ValueError, match=match):
            package.bound_inventory(source, 'c' * 40, 'v04', _inventory(tmp_path, source, **override))


def test_parity_basis_requires_v04_manifest(tmp_path):
    for name, text in (('source', 'manifest_version,v1\n'), ('v04', 'manifest_version,v1\n'),
                       ('other', 'manifest_version,v1\nx,1,1,,,\n')):
        (tmp_path / name).mkdir()
        (tmp_path / name / '.deployed_manifest.csv').write_text(text)
    basis = package.parity_basis(tmp_path / 'source', tmp_path / 'v04')  # a snapshot of v0.4 is the right source
    assert basis['manifest_sha256'] == package.sha256(tmp_path / 'v04/.deployed_manifest.csv')
    with pytest.raises(ValueError, match='differs from the live v0.4 manifest'):
        package.parity_basis(tmp_path / 'other', tmp_path / 'v04')


def test_assemble_without_inventory_is_bound(tmp_path, repo, monkeypatch):
    root, a, b, git = repo
    git('checkout', '-q', 'play')
    source, v04 = tmp_path / 'source', tmp_path / 'v04'
    for folder in (source, v04):
        folder.mkdir()
        (folder / '.deployed_manifest.csv').write_text(
            f'manifest_version,v1\nrelpath,sha256,bytes,src_commit\nmc2.exe,x,1,{a}\n')
    _fingerprint(monkeypatch, sha=a[:12])
    inventory = _inventory(tmp_path, source, status='STOP', executable_commit=a,
                           references={'credits_source': str(v04)})
    monkeypatch.setattr(package, 'INVENTORY', inventory)
    monkeypatch.setattr(package, 'PLAY_REF', 'play')
    with pytest.raises(ValueError, match='not PASS'):
        package.assemble(source, tmp_path / 'out', {}, [], repository=root, credits_source=v04)
    (v04 / '.deployed_manifest.csv').write_text('manifest_version,v1\n')
    with pytest.raises(ValueError, match='live v0.4 manifest'):
        package.assemble(source, tmp_path / 'out', {}, [], repository=root, credits_source=v04)


def test_manifest_rows_keep_source_commit_not_packaging_head(tmp_path, repo):
    root, a, b, git = repo
    git('checkout', '-q', 'play')
    source = tmp_path / 'source'
    source.mkdir()
    for rel in package.STOCK_ROOT:
        (source / rel).write_text('stock fixture')
    digest = package.sha256(source / 'art.fst')
    (source / '.deployed_manifest.csv').write_text(
        'manifest_version,v1\nrelpath,sha256,bytes,src_commit\n'
        f'art.fst,{digest},13,{"e" * 40}\n')
    inventory = {rel: {'sha256': package.sha256(source / rel)} for rel in package.STOCK_ROOT}
    package.assemble(source, tmp_path / 'out', {}, [], inventory=inventory, repository=root)
    with (tmp_path / 'out/.deployed_manifest.csv').open(newline='') as stream:
        next(csv.reader(stream))
        rows = {r['relpath']: r['src_commit'] for r in csv.DictReader(stream)}
    assert rows['art.fst'] == 'e' * 40
    assert rows['camera.fst'] == '' and a not in rows.values()


@pytest.mark.parametrize('name, data, flagged', [
    ('a.txt', 'see D:\\Games\\x', True), ('b.json', '{"p": "z:/data"}', True),
    ('c.txt', 'path D:\\Games\\x'.encode('utf-16'), True), ('d.txt', 'nothing here', False)])
def test_scan_machine_paths_any_drive_and_utf16(tmp_path, name, data, flagged):
    (tmp_path / name).write_bytes(data if isinstance(data, bytes) else data.encode())
    if flagged:
        with pytest.raises(ValueError, match='machine path'):
            package.scan(tmp_path)
    else:
        package.scan(tmp_path)


def test_scan_skips_pinned_credits_text(tmp_path):
    (tmp_path / 'credits').mkdir()
    (tmp_path / 'credits/readme.txt').write_text('see c:\\MechCommanderOmnitech')
    package.scan(tmp_path)


def test_child_registry_default_dir(tmp_path):
    (tmp_path / 'shipped').mkdir()
    (tmp_path / 'checkout/.git').mkdir(parents=True)
    assert child_registry.default_registry_dir(tmp_path / 'checkout', 'win32') == Path(
        'A:/Games/mc2-opengl/.smoke_children')
    assert child_registry.default_registry_dir(tmp_path / 'shipped', 'win32') == tmp_path / 'shipped/.smoke_children'


def test_bt2018_extractor_list_is_complete_and_flagged():
    listed = [x for x in package.BT2018_LIST.read_text().splitlines() if x]
    assert len(listed) == 111 == len({x.casefold() for x in listed})
    assert all(package.bt2018_extractor_only(rel) for rel in listed)
    assert not package.bt2018_extractor_only('data/tgl/not_in_the_list.tgl')
