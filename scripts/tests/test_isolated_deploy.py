"""Fresh installs keep in-place deploy writes private; atomic recipes may share."""
import importlib.util
import csv
import hashlib
import json
import os
import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import deploy_payload as dp
import kit_payload
import pytest


def test_fresh_install_covers_deploy_payload(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "isolated_deploy", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    guard = iso._load_guard()
    # Enumerate real game AND editor destinations, without building/cooking.
    monkeypatch.setattr(dp, "ensure_concrete_detail", lambda root: None)
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    monkeypatch.setattr(dp, "log", lambda message: None)
    rels = set()
    for exe in ("mc2.exe", dp.EDITOR_EXE):
        rels.update(rel for _, rel, _ in dp.enumerate_payload(
            str(ROOT), str(tmp_path / "build"), exe, "mc2.pdb",
            require_build=False))
    manifest = json.loads((ROOT / kit_payload.MANIFEST).read_text())
    rels.update((kit_payload.KIT_REL / row["path"]).as_posix()
                for row in manifest["files"])
    # Outputs written after payload copying, including cooked sidecars/receipts.
    rels.update((".deployed_manifest.csv", "data/textures/engine_normals.cookver",
                 "data/textures/mat0_normal.ktx2", "data/tgl/128/a.tga.cookstamp",
                 "cache/content-model/industrial-cook.json"))
    recipe = "cache/content-model/model-123.mci"
    assert guard.classify(recipe) == "data"
    assert rels
    assert not [rel for rel in rels if guard.classify(rel) != "mutated"]
    assert guard.classify("DATA\\TGL\\128\\A.TGA.COOKSTAMP") == "mutated"
    src, dst = tmp_path / "source", tmp_path / "install"
    for rel in rels | {recipe, "data/terrain/stock.pak", "textures.fst", "data/tgl/128/stock.tga",
                       "mods/custom/data/model.glb"}:
        path = src / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"stock")
    links, copies, _, _ = iso.plan(src, guard)
    iso.build(src, dst, links, copies)
    assert guard.run_check(str(dst), payload_rels=sorted(rels))[0] == "OK"
    assert guard.run_check(str(dst), deep=True)[0] == "OK"
    assert (src / recipe).samefile(dst / recipe)
    assert set(guard.iter_mutated_relpaths(dst)) == set(guard.iter_mutated_relpaths(dst, deep=True))
    assert os.stat(src / "data/terrain/stock.pak").st_ino == os.stat(dst / "data/terrain/stock.pak").st_ino
    assert os.stat(src / "data/tgl/128/stock.tga").st_ino == os.stat(dst / "data/tgl/128/stock.tga").st_ino
    assert os.stat(src / "mods/custom/data/model.glb").st_ino == os.stat(dst / "mods/custom/data/model.glb").st_ino
    items = [(str(src / rel), rel, "support") for rel in sorted(rels)]
    dp.check_hardlink_isolation(items, str(dst))
    hashes = dp.deploy(items, str(dst), allow_stale_pdb=False, full_verify=True)
    assert set(hashes) == rels
    victim = next(iter(sorted(rels)))
    (dst / victim).write_bytes(b"new payload")
    assert (src / victim).read_bytes() == b"stock"


def lean_tool():
    spec = importlib.util.spec_from_file_location('lean_iso', ROOT / 'scripts/make-isolated-deploy.py')
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    return iso


def lean_fixture(src):
    payload = ['mc2.exe', 'runtime.dll', 'shaders_vk/a.spv', 'data/terrain_materials.json', 'mc2.pdb',
               'mods/deployed/mod.json']
    junk = ['debug_state/state.json', 'cache/content-model/temp.mci', 'harness.log',
            'data/textures/_txm_cache_bak/123.456.txm', 'tools/authoring.exe', 'mods/custom/mod.json']
    for rel in payload + junk + ['textures.fst', 'data/missions/a.fit', 'options.cfg', 'system.cfg',
                               'prefs.cfg', 'minprefs.cfg', 'orgprefs.cfg']:
        path = src / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'fixture')
    with (src / '.deployed_manifest.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['manifest_version', 'v1'])
        writer.writerow(['relpath', 'sha256', 'bytes', 'src_commit', 'timestamp', 'mtime_ns'])
        for rel in payload:
            writer.writerow([rel, hashlib.sha256(b'fixture').hexdigest(), 7, 'abc', '', 0])
    return payload, junk


def test_lean_projection_is_private_and_has_no_runtime_cache(tmp_path):
    iso = lean_tool()
    src, dst = tmp_path / 'source', tmp_path / 'dest'
    payload, junk = lean_fixture(src)
    stamp = src / 'data/tgl/128/a.tga.cookstamp'
    stamp.parent.mkdir(parents=True)
    stamp.write_text('mc2-stock-v1 verified')
    seed = iso.verified_seed(src)
    guard = iso._load_guard()
    links, copies, _, copied_bytes = iso.plan(src, guard, seed)
    all_rels = {r for r, _ in links + copies}
    assert set(payload) - {'mc2.pdb', 'mods/deployed/mod.json'} <= all_rels
    assert 'mods/deployed/mod.json' not in all_rels
    assert not set(junk) & all_rels
    assert 'mc2.pdb' not in all_rels
    assert 'textures.fst' in {r for r, _ in links}
    assert {'prefs.cfg', 'minprefs.cfg', 'orgprefs.cfg'} <= {r for r, _ in copies}
    assert 'data/tgl/128/a.tga.cookstamp' in {r for r, _ in copies}
    iso.build(src, dst, links, copies)
    assert (dst / stamp.relative_to(src)).read_bytes() == stamp.read_bytes()
    assert not stamp.samefile(dst / stamp.relative_to(src))
    iso.write_seed_manifest(dst, seed)
    projected = iso.verified_seed(dst)
    assert set(projected) == set(payload) - {'mc2.pdb', 'mods/deployed/mod.json'}
    # Dry-run the release packager's mandatory preference inputs on a fresh seed.
    release = (ROOT / 'scripts/build_release.sh').read_text(encoding='utf-8')
    required = re.search(r'for cfg in ([^;]+); do\s+cp "\$DEPLOY/\$cfg"', release)[1].split()
    assert required == ['minprefs.cfg', 'orgprefs.cfg', 'prefs.cfg', 'system.cfg']
    assert all((dst / cfg).read_bytes() == b'fixture' for cfg in required)
    assert not (src / 'mc2.exe').samefile(dst / 'mc2.exe')
    for rel in ('prefs.cfg', 'minprefs.cfg'):
        assert (dst / rel).read_bytes() == (src / rel).read_bytes()
        assert not (src / rel).samefile(dst / rel)
    assert (src / 'textures.fst').samefile(dst / 'textures.fst')
    assert guard.run_check(str(dst), deep=True)[0] == 'OK'
    assert copied_bytes < iso.plan(src, guard)[3]


def test_seed_rejects_tampered_payload_and_missing_manifest(tmp_path):
    iso = lean_tool()
    lean_fixture(tmp_path)
    (tmp_path / 'runtime.dll').write_bytes(b'tampered')
    with pytest.raises(SystemExit):
        iso.verified_seed(tmp_path)

    (tmp_path / '.deployed_manifest.csv').unlink()
    with pytest.raises(SystemExit):
        iso.verified_seed(tmp_path)


# W138 merge: the W137 pinned-cook seed test is superseded by test_kit_seed_preserves_only_pinned_sources_and_verified_recipes
# (kit_seed_members carries pinned sources + verified recipes; recipes link, the cook receipt is private, writes replace atomically).
def test_verified_cache_rehashes_only_changed_members_and_rejects_corruption(tmp_path, monkeypatch):
    iso = lean_tool()
    lean_fixture(tmp_path)
    iso.verified_seed(tmp_path, use_cache=True)
    calls = []
    sha = iso.file_sha
    monkeypatch.setattr(iso, 'file_sha', lambda p: (calls.append(p), sha(p))[1])
    iso.verified_seed(tmp_path, use_cache=True)
    assert calls == []
    victim = tmp_path / 'runtime.dll'
    stat = victim.stat()
    victim.write_bytes(b'corrupt')  # same length; preserve mtime as well
    os.utime(victim, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(SystemExit):
        iso.verified_seed(tmp_path, use_cache=True)
    assert victim in calls


def test_plan_prunes_excluded_trees_before_descent(tmp_path, monkeypatch):
    iso = lean_tool()
    lean_fixture(tmp_path)
    visited = []
    walk = os.walk

    def recording(*args, **kwargs):
        for row in walk(*args, **kwargs):
            visited.append(Path(row[0]).relative_to(tmp_path).as_posix())
            yield row

    monkeypatch.setattr(iso.os, 'walk', recording)
    iso.plan(tmp_path, iso._load_guard(), iso.verified_seed(tmp_path))
    assert not {'cache', 'mods', 'debug_state'} & set(visited)


def test_link_failure_falls_back_to_guarded_verified_private_copy(tmp_path, monkeypatch):
    iso = lean_tool()
    src, dst = tmp_path / 'src', tmp_path / 'dst'
    src.mkdir()
    (src / 'textures.fst').write_bytes(b'stock')
    guards = []
    monkeypatch.setattr(iso.os, 'link', lambda *args: (_ for _ in ()).throw(OSError('unsupported')))
    monkeypatch.setattr(iso, 'check_space', lambda size, path: guards.append((size, path)))
    assert iso.build(src, dst, [('textures.fst', 5)], []) == (0, 1)
    assert guards and guards[0][0] >= 5
    assert (dst / 'textures.fst').read_bytes() == b'stock'
    assert not (src / 'textures.fst').samefile(dst / 'textures.fst')


def test_copied_file_checked_against_manifest(tmp_path, monkeypatch):
    iso = lean_tool()
    src, dst = tmp_path / 'src', tmp_path / 'dst'
    src.mkdir()
    (src / 'mc2.exe').write_bytes(b'good')
    copy = iso.shutil.copy2

    def corrupt(a, b):
        copy(a, b)
        Path(b).write_bytes(b'evil')

    monkeypatch.setattr(iso.shutil, 'copy2', corrupt)
    with pytest.raises(SystemExit):
        iso.build(src, dst, [], [('mc2.exe', 4)],
                  {'mc2.exe': {'sha256': hashlib.sha256(b'good').hexdigest()}})


def test_parallel_link_fallback_reserves_all_copied_bytes(tmp_path, monkeypatch):
    iso = lean_tool()
    src, dst = tmp_path / 'src', tmp_path / 'dst'
    src.mkdir()
    members = [(f'file-{n}.bin', 7) for n in range(12)]
    for rel, _ in members:
        (src / rel).write_bytes(b'fixture')
    monkeypatch.setattr(iso.os, 'link', lambda *a: (_ for _ in ()).throw(OSError('unsupported')))
    reservations = []
    monkeypatch.setattr(iso, 'check_space', lambda n, p: reservations.append(n))
    assert iso.build(src, dst, members, []) == (0, len(members))
    assert reservations == [int(7 * n * 1.15) for n in range(1, 13)]
    for rel, _ in members:
        assert (dst / rel).read_bytes() == b'fixture'
        assert not (src / rel).samefile(dst / rel)


def test_kit_seed_preserves_only_pinned_sources_and_verified_recipes(tmp_path, monkeypatch):
    iso = lean_tool()
    source, repo = tmp_path / 'src', tmp_path / 'repo'
    monkeypatch.setattr(iso, 'SCRIPTS', repo / 'scripts')
    name = 'mods/mc2-industrial-kit/model.glb'
    path = source / name
    path.parent.mkdir(parents=True)
    path.write_bytes(b'model')
    pinned = repo / 'data/kit/industrial-kit.json'
    pinned.parent.mkdir(parents=True)
    pinned.write_text(json.dumps({'files': [{'path': 'model.glb', 'bytes': 5,
                                           'sha256': iso.file_sha(path)}]}))
    cache = source / 'cache/content-model'
    cache.mkdir(parents=True)
    recipe = cache / 'model-123.mci'
    recipe.write_bytes(b'cooked')
    (cache / 'industrial-cook.json').write_text(json.dumps({'files': [
        {'path': recipe.name, 'bytes': 6, 'sha256': iso.file_sha(recipe)}]}))
    extras = iso.kit_seed_members(source, {}, use_cache=True)
    assert set(extras) == {name, 'cache/content-model/industrial-cook.json',
                           'cache/content-model/model-123.mci'}
    with monkeypatch.context() as warm:
        warm.setattr(kit_payload, 'checked_files', lambda *a: pytest.fail('unchanged kit rehashed'))
        assert iso.kit_seed_members(source, {}, use_cache=True) == extras
    lean_fixture(source)
    guard = lean_tool()._load_guard()
    monkeypatch.setattr(iso, '_load_guard', lambda: guard)
    monkeypatch.setattr(iso, 'check_space', lambda *a: None)
    lane = tmp_path / 'lane'
    assert iso.main(['--from', str(source), '--to', str(lane)]) == 0
    installed = lane / recipe.relative_to(source)
    assert installed.samefile(recipe)
    replacement = installed.with_suffix('.tmp')
    replacement.write_bytes(b'new recipe')
    os.replace(replacement, installed)
    assert recipe.read_bytes() == b'cooked'
    assert installed.read_bytes() == b'new recipe'
    assert not (lane / 'cache/content-model/industrial-cook.json').samefile(cache / 'industrial-cook.json')
    stamp = path.stat()
    path.write_bytes(b'wrong')
    os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    with pytest.raises(ValueError, match='pinned kit'):
        iso.kit_seed_members(source, {}, use_cache=True)


def test_seed_inventory_reuses_and_refreshes_on_manifest_change(tmp_path, monkeypatch):
    iso = lean_tool()
    lean_fixture(tmp_path)
    seed = iso.verified_seed(tmp_path)
    guard = iso._load_guard()
    cold = iso.seed_plan(tmp_path, guard, seed)
    original = iso.plan
    monkeypatch.setattr(iso, 'plan', lambda *a: pytest.fail('unchanged seed walked again'))
    assert iso.seed_plan(tmp_path, guard, seed) == cold
    manifest = tmp_path / '.deployed_manifest.csv'
    manifest.write_text(manifest.read_text().replace('abc', 'next-commit'))
    monkeypatch.setattr(iso, 'plan', original)
    refreshed = iso.seed_plan(tmp_path, guard, iso.verified_seed(tmp_path))
    assert refreshed[0] == cold[0]
    assert refreshed[3] > cold[3]  # the rewritten manifest grew


def test_linked_pinned_kit_safe_only_with_skip_kit(tmp_path):
    iso = lean_tool()
    guard = iso._load_guard()
    src, dst = tmp_path / 'src', tmp_path / 'dst'
    name = 'mods/mc2-industrial-kit/model.glb'
    (src / name).parent.mkdir(parents=True)
    (src / name).write_bytes(b'kit')
    before = iso.content_stamp(src / name)
    iso.build(src, dst, [(name, 3)], [])
    print('NTFS hardlink stamp before/after:', before, iso.content_stamp(src / name))
    assert guard.run_check(str(dst), payload_rels=['mc2.exe'])[0] == 'OK'
    assert guard.run_check(str(dst), payload_rels=[name])[0] == 'SHARED'


@pytest.mark.parametrize('change', ['none', 'source', 'corrupt', 'full'])
def test_fast_install_delta_keeps_unchanged_bytes_and_detaches_changes(tmp_path, monkeypatch, change):
    iso = lean_tool()
    seed, lane, repo = (tmp_path / p for p in ('seed', 'lane', 'repo'))
    lean_fixture(seed)
    monkeypatch.setattr(iso, 'check_space', lambda *a: None)
    assert iso.main(['--from', str(seed), '--to', str(lane)]) == 0
    rels = ['mc2.exe', 'runtime.dll', 'shaders_vk/a.spv']
    for rel in rels:
        path = repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'fixture')
        assert (seed / rel).samefile(lane / rel)
    if change == 'source':
        (repo / 'runtime.dll').write_bytes(b'updated')
    if change == 'corrupt':
        victim = lane / 'runtime.dll'
        old = victim.stat()
        victim.write_bytes(b'corrupt')
        os.utime(victim, ns=(old.st_atime_ns, old.st_mtime_ns))
    items = [(str(repo / rel), rel, 'exe' if rel == 'mc2.exe' else 'support') for rel in rels]
    verified = dp.prepare_seed_delta(items, str(lane), full_verify=change == 'full')
    assert set(verified) == (set() if change == 'full' else set(rels) -
                             ({'runtime.dll'} if change != 'none' else set()))
    dp.check_hardlink_isolation([i for i in items if i[1] not in verified], str(lane))
    monkeypatch.setattr(dp, 'ensure_terrain_materials', lambda *a: {})
    monkeypatch.setattr(dp, 'ensure_engine_normal_cooks', lambda *a: None)
    monkeypatch.setattr(dp, 'retire_unowned', lambda *a, **k: None)
    hashes = dp.deploy(items, str(lane), False, verified=verified)
    assert set(hashes) == set(rels)
    for rel in rels:
        assert (lane / rel).read_bytes() == (repo / rel).read_bytes()
        assert (lane / rel).samefile(seed / rel) == (rel in verified)
    if change == 'source':
        assert (seed / 'runtime.dll').read_bytes() == b'fixture'


def test_normal_deploy_keeps_hardlink_refusal(tmp_path):
    iso = lean_tool()
    seed, lane = tmp_path / 'seed', tmp_path / 'lane'
    seed.mkdir()
    (seed / 'mc2.exe').write_bytes(b'exe')
    iso.build(seed, lane, [('mc2.exe', 3)], [])
    items = [(str(seed / 'mc2.exe'), 'mc2.exe', 'exe')]
    assert dp.prepare_seed_delta(items, str(lane)) == {}
    with pytest.raises(SystemExit):
        dp.check_hardlink_isolation(items, str(lane))


@pytest.mark.parametrize('receipt', [False, True])
def test_mission_write_breaks_seed_links_including_manifest(tmp_path, receipt):
    """The bake-recook-smooth 58-file refusal must never require shared writes."""
    iso = lean_tool()
    seed, lane = tmp_path / 'seed', tmp_path / 'lane'
    rel = 'data/missions/mc2_01.beauty/visual_height_4x.r32'
    for name in (rel, dp.MANIFEST_NAME):
        path = seed / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'original')
    iso.build(seed, lane, [(rel, 8), (dp.MANIFEST_NAME, 8)], [])
    if receipt:
        (lane / '.lane-seed.json').write_text(json.dumps({'shared': {rel: {
            'stamp': iso.content_stamp(lane / rel),
            'sha256': hashlib.sha256(b'original').hexdigest()}}}))
    source = tmp_path / 'changed.r32'
    source.write_bytes(b'changed')
    items = [(str(source), rel, 'data'), (None, dp.MANIFEST_NAME, 'manifest')]
    assert dp.prepare_seed_delta(items, str(lane), allow_legacy=True) == {}
    dp.check_hardlink_isolation(items, str(lane))
    for name in (rel, dp.MANIFEST_NAME):
        assert not (lane / name).samefile(seed / name)
        (lane / name).write_bytes(b'changed')
        assert (seed / name).read_bytes() == b'original'


def test_failed_privatization_preserves_shared_bytes(tmp_path, monkeypatch):
    iso = lean_tool()
    seed, lane = tmp_path / 'seed', tmp_path / 'lane'
    seed.mkdir()
    (seed / 'mc2.exe').write_bytes(b'original')
    iso.build(seed, lane, [('mc2.exe', 8)], [])
    def failed_replace(*args):
        raise OSError('injected replacement failure')
    monkeypatch.setattr(dp.os, 'replace', failed_replace)
    with pytest.raises(OSError, match='injected'):
        dp.prepare_seed_delta([(None, 'mc2.exe', 'exe')], str(lane), allow_legacy=True)
    assert (seed / 'mc2.exe').samefile(lane / 'mc2.exe')
    assert (seed / 'mc2.exe').read_bytes() == b'original'
    assert sorted(p.name for p in lane.iterdir()) == ['mc2.exe']


@pytest.mark.parametrize('receipt', [False, True])
def test_lane_payload_deploy_preserves_seed_and_other_install(tmp_path, monkeypatch, receipt):
    """trees-ship/bake-recook-smooth: run the copy+manifest path after detachment."""
    iso = lean_tool()
    seed, lane, other = (tmp_path / p for p in ('seed', 'lane', 'other'))
    rels = ['data/missions/mc2_01.beauty/visual_height_4x.r32',
            'data/trees/tree.bin']
    for rel in rels + [dp.MANIFEST_NAME]:
        p = seed / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'original')
    for target in (lane, other):
        iso.build(seed, target, [(r, 8) for r in rels + [dp.MANIFEST_NAME]], [])
    if receipt:
        shared = {r: {'stamp': iso.content_stamp(lane / r),
                       'sha256': hashlib.sha256(b'original').hexdigest()} for r in rels}
        (lane / '.lane-seed.json').write_text(json.dumps({'shared': shared}))
    source = tmp_path / 'changed.bin'
    source.write_bytes(b'new lane bytes')
    items = [(str(source), r, 'data') for r in rels]
    writes = items + [(None, dp.MANIFEST_NAME, 'manifest')]
    verified = dp.prepare_seed_delta(writes, str(lane), allow_legacy=True)
    assert verified == {}
    dp.check_hardlink_isolation(writes, str(lane))
    monkeypatch.setattr(dp, 'ensure_terrain_materials', lambda *a: {})
    monkeypatch.setattr(dp, 'ensure_engine_normal_cooks', lambda *a: None)
    monkeypatch.setattr(dp, 'retire_unowned', lambda *a, **k: None)
    hashes = dp.deploy(items, str(lane), False, verified=verified)
    dp.write_manifest(str(lane), hashes, 'lane-tip')
    assert set(dp.read_manifest(str(lane))) == set(rels)
    for rel in rels:
        assert (lane / rel).read_bytes() == b'new lane bytes'
    for rel in rels + [dp.MANIFEST_NAME]:
        assert (seed / rel).read_bytes() == (other / rel).read_bytes() == b'original'
        assert not (lane / rel).samefile(seed / rel)


def test_link_creation_does_not_invalidate_seed_receipts(tmp_path, monkeypatch):
    iso = lean_tool()
    lean_fixture(tmp_path)
    iso.verified_seed(tmp_path, use_cache=True)
    victim = tmp_path / 'runtime.dll'
    other = tmp_path / 'link-of-runtime.dll'
    os.link(victim, other)  # bumps NTFS ChangeTime / link count like an install
    shared = {'runtime.dll': {'stamp': iso.content_stamp(other)}}
    iso.refresh_link_receipts(tmp_path, shared)
    calls = []
    sha = iso.file_sha
    monkeypatch.setattr(iso, 'file_sha', lambda p: (calls.append(p), sha(p))[1])
    iso.verified_seed(tmp_path, use_cache=True)
    assert victim not in calls
    # Same-size corruption with restored mtime is still caught after a refresh.
    stat = victim.stat()
    victim.write_bytes(b'corrupt')
    os.utime(victim, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(SystemExit):
        iso.verified_seed(tmp_path, use_cache=True)



def test_delete_of_linked_install_then_restamp_avoids_rehash(tmp_path, monkeypatch):
    iso = lean_tool()
    lean_fixture(tmp_path)
    iso.verified_seed(tmp_path, use_cache=True)
    victim = tmp_path / 'runtime.dll'
    other = tmp_path / 'install-copy.dll'
    os.link(victim, other)
    other.unlink()  # install delete: link count / ChangeTime change on the seed inode
    calls = []
    sha = iso.file_sha
    monkeypatch.setattr(iso, 'file_sha', lambda p: (calls.append(p), sha(p))[1])
    assert iso.restamp_seed(tmp_path) > 0
    iso.verified_seed(tmp_path, use_cache=True)
    assert victim not in calls
    stat = victim.stat()
    victim.write_bytes(b'corrupt')
    os.utime(victim, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    with pytest.raises(SystemExit):
        iso.verified_seed(tmp_path, use_cache=True)
