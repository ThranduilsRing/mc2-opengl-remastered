"""Pinned kit deploy regression cases (no engine process needed)."""
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kit_payload as kit


def package(tmp_path, monkeypatch):
    store = tmp_path / "store"
    store.mkdir()
    paths = [f"data/model_overrides/test/pristine_intact_lod{i}.glb" for i in range(3)]
    rows = []
    for rel in paths:
        p = store / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(rel.encode())
        rows.append(dict(path=rel, bytes=p.stat().st_size, sha256=kit.sha(p)))
    manifest = dict(schema=1, store=str(store), assets=[dict(id="test", source_bake_tag="r2")], files=rows)
    path = tmp_path / kit.MANIFEST
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(manifest))
    monkeypatch.delenv("MC2_KIT_STORE", raising=False)
    return manifest, store


@pytest.mark.parametrize("problem", ["missing", "hash", "asset-list", "unsafe"])
def test_bad_package_fails_before_copy(tmp_path, monkeypatch, problem):
    manifest, store = package(tmp_path, monkeypatch)
    path = store / manifest["files"][0]["path"]
    if problem == "missing":
        path.unlink()
    elif problem == "hash":
        path.write_bytes(b"x" * path.stat().st_size)
    elif problem == "asset-list":
        manifest["assets"][0]["id"] = "wrong"
    else:
        manifest["files"][0]["path"] = "../outside"
    (tmp_path / kit.MANIFEST).write_text(json.dumps(manifest))
    with pytest.raises(ValueError):
        kit.payload(tmp_path)


def test_cooks_then_validates_receipt_and_repairs_corrupt_cache(tmp_path, monkeypatch):
    manifest, store = package(tmp_path, monkeypatch)
    _, items = kit.payload(tmp_path)
    assert len(items) == 3 and all(i[2] == "kit" for i in items)
    target = tmp_path / "install"
    import shutil
    shutil.copytree(store, target / kit.KIT_REL)
    tool = tmp_path / "importer.exe"
    tool.write_bytes(b"tool")
    calls = []
    def cook(command, **kwargs):
        calls.append(command)
        key = hashlib.sha256(command[1].encode()).hexdigest()[:16]
        cache = target / "cache/content-model"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / f"model-{key}.mci").write_bytes(b"recipe")
        return SimpleNamespace(returncode=0, stdout="", stderr=f"[CONTENT_COOK] key={key} cache=miss")
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 3
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 3
    next((target / "cache/content-model").glob("*.mci")).write_bytes(b"bad")
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 6
    tool.write_bytes(b"rebuilt importer")
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 9
    # A retained receipt cannot hide a pruned recipe or an incomplete inventory.
    next((target / "cache/content-model").glob("*.mci")).unlink()
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 12
    receipt = target / "cache/content-model/industrial-cook.json"
    partial = json.loads(receipt.read_text())
    partial["files"].pop()
    receipt.write_text(json.dumps(partial))
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 15
    (target / kit.KIT_REL / "extra.glb").write_bytes(b"unversioned")
    with pytest.raises(ValueError, match="unpinned"):
        kit.finish(target, manifest, tool, cook)


def test_cook_failure_or_missing_output_is_fatal(tmp_path, monkeypatch):
    manifest, store = package(tmp_path, monkeypatch)
    import shutil
    target = tmp_path / "install"
    shutil.copytree(store, target / kit.KIT_REL)
    tool = tmp_path / "tool.exe"
    tool.write_bytes(b"tool")
    for rc in (0, 2):
        with pytest.raises(ValueError):
            kit.finish(target, manifest, tool, lambda *a, **kw: SimpleNamespace(returncode=rc, stdout="", stderr="no cook"))
    assert not (target / "cache/content-model/industrial-cook.json").exists()


@pytest.mark.parametrize("classic,option", [(True, True), (False, False), (False, True)])
def test_deploy_classic_needs_no_package_but_remastered_fails(tmp_path, monkeypatch, classic, option):
    import deploy_payload as dp
    monkeypatch.setattr(dp, "check_exe_fresh", lambda *a, **kw: None)
    monkeypatch.setattr(dp, "git_head", lambda *a: "head")
    monkeypatch.setattr(dp, "require_head_exe", lambda *a: dict(dirty=False))
    monkeypatch.setattr(dp, "enumerate_payload", lambda *a, **kw: [])
    monkeypatch.delenv("MC2_OPTIONS_OVERRIDE", raising=False)
    (tmp_path / "options.cfg").write_text(f"[MechCommander2]\nb RemasteredBuildings = {'TRUE' if option else 'FALSE'}\n")
    class ReachedCopyPreflight(Exception):
        pass
    monkeypatch.setattr(dp, "check_free_space_for_payload", lambda *a: (_ for _ in ()).throw(ReachedCopyPreflight()))
    args = SimpleNamespace(allow_stale_exe=False, build64_root=None, classic_assets=classic)
    expected = ReachedCopyPreflight if classic or not option else SystemExit
    with pytest.raises(expected):
        dp.run_deploy(args, str(tmp_path), str(tmp_path), str(tmp_path), "mc2.exe", "mc2.pdb")


def test_existing_importer_still_runs_ninja_dependency_check(tmp_path, monkeypatch):
    import configure_ninja as cn
    import bootstrap_worktree_build as bootstrap
    tree = tmp_path / "build-cc-tgdump"
    tree.mkdir()
    (tree / "build.ninja").touch()
    (tree / "tg_import_dump.exe").write_bytes(b"stale executable")
    monkeypatch.setattr(bootstrap, "find_cmake", lambda: "cmake")
    commands = []
    monkeypatch.setattr(cn, "under_vcvars", lambda command, env: commands.append(command) or 0)
    assert kit.build_importer(tmp_path).is_file()
    assert len(commands) == 1 and "--build" in commands[0] and "tg_import_dump" in commands[0]
    monkeypatch.setattr(cn, "under_vcvars", lambda *a: 1)
    with pytest.raises(ValueError, match="build failed"):
        kit.build_importer(tmp_path)
