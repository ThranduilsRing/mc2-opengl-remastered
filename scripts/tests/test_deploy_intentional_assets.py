"""Declared assets deploy from verified content, independent of ignored source files."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import console_child


@pytest.fixture
def assets(tmp_path, monkeypatch):
    source, target, store = (tmp_path / name for name in ("source", "target", "store"))
    for folder in (source, target, store):
        folder.mkdir()
    declared = {"data/model_overrides/fir/tree.glb": hashlib.sha256(b"cooked").hexdigest(),
                "data/tgl/32/fir.tga": hashlib.sha256(b"texture").hexdigest()}
    (source / "intentional-asset-changes.json").write_text(json.dumps(declared))
    for args in (("init",), ("add", "intentional-asset-changes.json"),
                 ("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                  "-c", "core.hooksPath=NUL", "commit", "-m", "declare")):
        subprocess.run(["git", "-C", str(source), *args], check=True, capture_output=True,
                       **console_child.child_kwargs(redirected=True))
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda _: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda _: None)
    return source, target, store, declared


@pytest.mark.parametrize("existing", [None, b"stale", b"cooked"])
def test_store_delivery_and_manifest_hashes(assets, existing):
    source, target, store, declared = assets
    for (rel, sha), content in zip(declared.items(), (b"cooked", b"texture")):
        (store / sha).write_bytes(content)
        if existing is not None:
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(existing)
    items = dp.intentional_payload(source, target, store)
    hashes = dp.deploy(items, str(target), False, full_verify=True)
    assert {rel: row[0] for rel, row in hashes.items()} == declared
    assert {rel: dp.sha256_file(target / rel) for rel in declared} == declared
    assert not (source / "data").exists()


@pytest.mark.parametrize("corrupt", [False, True])
def test_store_miss_or_corruption_fails_before_copy(assets, corrupt, capsys):
    source, target, store, declared = assets
    for sha in declared.values():
        if corrupt:
            (store / sha).write_bytes(b"wrong bytes under right name")
    with pytest.raises(SystemExit):
        dp.intentional_payload(source, target, store)
    assert "store miss or corrupt blob" in capsys.readouterr().err
    assert list(target.iterdir()) == []


def test_seed_and_no_write_cli_audit(assets, tmp_path):
    source, target, store, declared = assets
    install = tmp_path / "seed"
    for rel, content in zip(declared, (b"cooked", b"texture")):
        path = install / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    dp.seed_asset_store(source, install, store)
    assert {p.name: dp.sha256_file(p) for p in store.iterdir()} == {s: s for s in declared.values()}
    # A dirty declaration cannot change what the committed candidate delivers.
    (source / "intentional-asset-changes.json").write_text("{}")
    before = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in tmp_path.rglob("*") if p.is_file()}
    proc = subprocess.run([sys.executable, "-B", dp.__file__, str(target), "--target", "game",
                           "--source-root", str(source), "--asset-store", str(store), "--dry-run"],
                          capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "2 missing, 0 mismatched, 0 unresolved" in proc.stdout
    assert list(target.iterdir()) == []
    assert all((p.stat().st_mtime_ns, p.read_bytes()) == value for p, value in before.items())


def test_stale_source_cannot_override_store_and_preserve_cannot_hide_drift(assets):
    source, target, store, declared = assets
    for (rel, sha), content in zip(declared.items(), (b"cooked", b"texture")):
        (store / sha).write_bytes(content)
        for root in (source, target):
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"stale")
    items = dp.intentional_payload(source, target, store)
    with pytest.raises(SystemExit):
        dp.deploy(items, str(target), False, preserve=list(declared))


@pytest.mark.parametrize("changed", ["source", "copy", "deployed"])
def test_declared_hash_survives_resolution_and_copy(assets, monkeypatch, changed):
    source, target, store, declared = assets
    for sha, content in zip(declared.values(), (b"cooked", b"texture")):
        (store / sha).write_bytes(content)
    items = dp.intentional_payload(source, target, store)
    if changed == "source":
        Path(items[0][0]).write_bytes(b"changed after resolution")
    elif changed == "copy":
        monkeypatch.setattr(dp.shutil, "copyfile", lambda src, dst: Path(dst).write_bytes(b"corrupt copy"))
    else:
        replace = dp.replace_file
        def corrupt(src, dst):
            replace(src, dst)
            Path(dst).write_bytes(b"corrupt deployed bytes")
        monkeypatch.setattr(dp, "replace_file", corrupt)
    with pytest.raises(SystemExit):
        dp.deploy(items, str(target), False)
    if changed != "deployed":
        assert not (target / next(iter(declared))).exists()


def test_unchanged_declarations_reuse_verified_hashes_and_repair_drift(assets, monkeypatch):
    import os
    source, target, store, declared = assets
    for sha, content in zip(declared.values(), (b"cooked", b"texture")):
        (store / sha).write_bytes(content)
    dp.deploy(dp.intentional_payload(source, target, store), str(target), False)
    hashed, original = [], dp.sha256_file
    monkeypatch.setattr(dp, "sha256_file", lambda p: hashed.append(Path(p)) or original(p))
    dp.deploy(dp.intentional_payload(source, target, store), str(target), False)
    assert hashed == []
    dst = target / "data/tgl/32/fir.tga"
    stamp = dst.stat()
    dst.write_bytes(b"changed")  # same size, even if the timestamp is restored
    os.utime(dst, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    dp.deploy(dp.intentional_payload(source, target, store), str(target), False)
    assert dst in hashed and dst.read_bytes() == b"texture"


@pytest.mark.parametrize("prior_manifest", [False, True])
@pytest.mark.parametrize("corrupt", [False, True])
def test_manifest_only_keeps_declared_tgl_without_cooked_source(assets, monkeypatch, prior_manifest, corrupt):
    source, target, store, declared = assets
    for rel, content in zip(declared, (b"cooked", b"texture")):
        dst = target / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(content)
    if prior_manifest:
        dp.write_manifest(str(target), {rel: (sha, (target / rel).stat().st_size, None)
                                      for rel, sha in declared.items()}, "test")
    monkeypatch.setattr(dp, "enumerate_payload", lambda *a, **kw: [])
    if corrupt:
        (target / "data/tgl/32/fir.tga").write_bytes(b"bad texture")
        with pytest.raises(SystemExit):
            dp.write_manifest_only(str(source), str(source), "mc2.exe", "mc2.pdb", str(target))
    else:
        dp.write_manifest_only(str(source), str(source), "mc2.exe", "mc2.pdb", str(target))
        assert {rel: row[0] for rel, row in dp.read_manifest(str(target)).items()} == declared
