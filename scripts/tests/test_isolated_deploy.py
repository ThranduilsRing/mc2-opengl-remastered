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
            'data/textures/123.456.ktx2', 'tools/authoring.exe', 'mods/custom/mod.json']
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


def test_lean_seed_preserves_verified_pinned_cooks_with_private_writes(tmp_path):
    iso = lean_tool()
    src, dst = tmp_path / 'source', tmp_path / 'dest'
    lean_fixture(src)
    model = 'mods/mc2-industrial-kit/data/model.glb'
    source_model = src / model
    source_model.parent.mkdir(parents=True)
    source_model.write_bytes(b'kit')
    with (src / '.deployed_manifest.csv').open('a', newline='') as stream:
        csv.writer(stream).writerow([model, kit_payload.sha(source_model), 3, 'abc', '', 0])
    cache = src / 'cache/content-model'
    recipe = cache / 'model-0123456789abcdef.mci'
    recipe.write_bytes(b'cooked')
    receipt = cache / 'industrial-cook.json'
    receipt.write_text(json.dumps({'files': [dict(path=recipe.name, bytes=6, sha256=kit_payload.sha(recipe))]}))
    seed = iso.verified_seed(src)
    links, copies, _, _ = iso.plan(src, iso._load_guard(), seed)
    iso.build(src, dst, links, copies)
    assert recipe.samefile(dst / recipe.relative_to(src))
    assert not receipt.samefile(dst / receipt.relative_to(src))
    assert not source_model.samefile(dst / model)
    (dst / model).write_bytes(b'lane model')
    (dst / receipt.relative_to(src)).write_text('{}')
    assert source_model.read_bytes() == b'kit'
    assert json.loads(receipt.read_text())['files']
    assert not (dst / 'cache/content-model/temp.mci').exists()
