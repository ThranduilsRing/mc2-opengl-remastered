"""Pinned kit deploy regression cases (no engine process needed)."""
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kit_payload as kit


@pytest.fixture(autouse=True)
def _private_cook_cache(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_COOK_CACHE", str(tmp_path / "cook-cache"))


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


def test_vehicle_payload_has_separate_pin_and_mount(tmp_path, monkeypatch):
    manifest, store = package(tmp_path, monkeypatch)
    vehicle_pin = Path("data/kit/vehicle-kit.json")
    (tmp_path / vehicle_pin).write_text(json.dumps(manifest))
    monkeypatch.setenv("MC2_KIT_STORE", str(tmp_path / "wrong-building-store"))
    actual, items = kit.payload(tmp_path, vehicle_pin, Path("mods/mc2-vehicle-kit"))
    assert actual == manifest
    assert len(items) == 3
    assert all(dst.startswith("mods/mc2-vehicle-kit/") for _, dst, _ in items)


def test_deploy_copies_only_pinned_files_and_rejects_existing_extras(tmp_path, monkeypatch):
    import deploy_payload as dp
    manifest, store = package(tmp_path, monkeypatch)
    for rel in ("content/packaging-r4.json", "content/packaging-r5.json", "content/review.json"):
        path = store / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("{}")
    # The real deploy inventory must not pin art-side bookkeeping either.
    real = json.loads((Path(__file__).resolve().parents[2] / kit.MANIFEST).read_text())
    assert not [r['path'] for r in real['files'] if r['path'].startswith('content/')
                and r['path'].endswith('.json') and not r['path'].endswith('.mc2ir.json')]
    _, items = kit.payload(tmp_path)
    target = tmp_path / 'install'
    monkeypatch.setattr(dp, 'ensure_terrain_materials', lambda *a: {})
    monkeypatch.setattr(dp, 'ensure_engine_normal_cooks', lambda *a: None)
    for _ in range(2):
        dp.deploy(items, target, False)
    installed = target / kit.KIT_REL
    assert {p.relative_to(installed).as_posix() for p in installed.rglob('*') if p.is_file()} == {
        r['path'] for r in manifest['files']}
    extra = installed / 'content/packaging-r4.json'
    extra.parent.mkdir(parents=True)
    extra.write_text('{}')
    with pytest.raises(ValueError) as exc:
        kit.finish(target, manifest, tmp_path / 'tool')
    assert str(extra) in str(exc.value)
    assert 'safe to delete if not in the kit manifest' in str(exc.value)


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


@pytest.mark.parametrize("shared_lods", [False, True])
def test_cooks_then_validates_receipt_and_repairs_corrupt_cache(tmp_path, monkeypatch, shared_lods):
    manifest, store = package(tmp_path, monkeypatch)
    if shared_lods:
        for row in manifest["files"]:
            path = store / row["path"]
            path.write_bytes(b"shared LOD")
            row.update(bytes=path.stat().st_size, sha256=kit.sha(path))
        (tmp_path / kit.MANIFEST).write_text(json.dumps(manifest))
    _, items = kit.payload(tmp_path)
    assert len(items) == 3 and all(i[2] == "kit" for i in items)
    target = tmp_path / "install"
    import shutil
    shutil.copytree(store, target / kit.KIT_REL)
    tool = tmp_path / "importer.exe"
    tool.write_bytes(b"tool")
    calls = []
    def cook(command, **kwargs):
        assert kwargs["env"]["OMP_NUM_THREADS"] == "1"
        if sys.platform == "win32":
            assert kwargs["creationflags"] & kit.subprocess.BELOW_NORMAL_PRIORITY_CLASS
        calls.append(command)
        key = hashlib.sha256(b"shared LOD" if shared_lods else command[1].encode()).hexdigest()[:16]
        cache = target / "cache/content-model"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / f"model-{key}.mci").write_bytes(b"recipe")
        return SimpleNamespace(returncode=0, stdout="", stderr=f"[CONTENT_COOK] key={key} cache=miss")
    kit.finish(target, manifest, tool, cook)
    cooks = 1 if shared_lods else 3
    assert len(calls) == cooks
    receipt_data = json.loads((target / "cache/content-model/industrial-cook.json").read_text())
    assert set(receipt_data["recipes"]) == {r["path"] for r in manifest["files"]}
    assert len(list((target / "cache/content-model").glob("*.mci"))) == (1 if shared_lods else 3)
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == cooks
    next((target / "cache/content-model").glob("*.mci")).write_bytes(b"broken") # same-size hash mismatch
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == cooks  # repair from the independently verified store
    tool.write_bytes(b"rebuilt importer")
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 2 * cooks
    # A retained receipt cannot hide a pruned recipe or an incomplete inventory.
    next((target / "cache/content-model").glob("*.mci")).unlink()
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 2 * cooks
    receipt = target / "cache/content-model/industrial-cook.json"
    partial = json.loads(receipt.read_text())
    partial["files"].pop()
    receipt.write_text(json.dumps(partial))
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 2 * cooks
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
    monkeypatch.setattr(dp, "viewer_payload", lambda *a, **kw: [])
    monkeypatch.setattr(dp, "deploy_inputs", lambda *a: {})
    monkeypatch.delenv("MC2_OPTIONS_OVERRIDE", raising=False)
    (tmp_path / "options.cfg").write_text(f"[MechCommander2]\nb RemasteredBuildings = {'TRUE' if option else 'FALSE'}\n")
    class ReachedCopyPreflight(Exception):
        pass
    monkeypatch.setattr(dp, "check_free_space_for_payload", lambda *a: (_ for _ in ()).throw(ReachedCopyPreflight()))
    args = SimpleNamespace(allow_stale_exe=False, build64_root=None, classic_assets=classic, skip_kit=False)
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


@pytest.fixture
def cook_install(tmp_path, monkeypatch):
    import shutil
    manifest, store = package(tmp_path, monkeypatch)
    target = tmp_path / "install"
    shutil.copytree(store, target / kit.KIT_REL)
    tool = tmp_path / "tool.exe"
    tool.write_bytes(b"tool")
    calls = []
    def cook(command, **kwargs):
        calls.append(command[1])
        key = hashlib.sha256(command[1].encode()).hexdigest()[:16]
        cache = target / "cache/content-model"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / f"model-{key}.mci").write_bytes(command[1].encode())
        return SimpleNamespace(returncode=0, stdout=f"[CONTENT_COOK] key={key}", stderr="")
    return target, manifest, tool, cook, calls


def test_full_receipt_cannot_substitute_an_unrelated_valid_recipe(cook_install):
    target, manifest, tool, cook, calls = cook_install
    kit.finish(target, manifest, tool, cook)
    receipt = target / "cache/content-model/industrial-cook.json"
    previous = json.loads(receipt.read_text())
    original = previous["files"][0]["path"]
    cache = receipt.parent
    unrelated = cache / "model-ffffffffffffffff.mci"
    unrelated.write_bytes(b"unrelated valid recipe")
    previous["files"][0] = dict(path=unrelated.name, bytes=unrelated.stat().st_size, sha256=kit.sha(unrelated))
    receipt.write_text(json.dumps(previous))
    (cache / original).unlink()
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 3  # the original recipe is restored from the shared store
    assert (cache / original).is_file()
    assert unrelated.name not in {r["path"] for r in json.loads(receipt.read_text())["files"]}


@pytest.mark.parametrize("problem", ["absolute", "drive", "rooted", "parent", "dot", "duplicate", "junction"])
def test_kit_copy_rejects_unsafe_destinations(tmp_path, monkeypatch, problem):
    import deploy_payload as dp
    manifest, store = package(tmp_path, monkeypatch)
    rel = manifest["files"][0]["path"]
    bad = {"absolute": str(store / rel), "drive": "C:relative.glb", "rooted": "\\root.glb",
           "parent": "data/../file.glb", "dot": "./file.glb"}
    if problem in bad:
        manifest["files"][0]["path"] = bad[problem]
    elif problem == "duplicate":
        row = dict(manifest["files"][0], path=rel.upper())
        manifest["files"].append(row)
    else:
        import subprocess
        target = tmp_path / "target"
        outside = tmp_path / "outside"
        target.mkdir()
        outside.mkdir()
        # Windows junctions require no symlink privilege.
        result = subprocess.run(["cmd", "/c", "mklink", "/J", str(target / "mods"), str(outside)],
                                capture_output=True, **kit.console_child.child_kwargs(redirected=True))
        assert result.returncode == 0
        monkeypatch.setattr(dp, "ensure_terrain_materials", lambda *a: {})
        monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda *a: None)
        with pytest.raises(ValueError, match="unsafe"):
            dp.deploy([(str(store / rel), (kit.KIT_REL / rel).as_posix(), "kit")], target, False)
        assert list(outside.iterdir()) == []
        return
    with pytest.raises(ValueError, match="unsafe|duplicate"):
        kit.checked_files(store, manifest["files"])


def test_input_mutation_during_cook_prevents_receipt(cook_install):
    target, manifest, tool, cook, calls = cook_install
    def mutate(command, **kwargs):
        result = cook(command, **kwargs)
        (target / kit.KIT_REL / manifest["files"][0]["path"]).write_bytes(b"changed")
        return result
    with pytest.raises(ValueError, match="missing/mismatched"):
        kit.finish(target, manifest, tool, mutate)
    assert not (target / "cache/content-model/industrial-cook.json").exists()


def test_retained_and_cross_install_reuse_isolated_outputs(cook_install):
    import os
    import shutil
    target, manifest, tool, cook, calls = cook_install
    kit.finish(target, manifest, tool, cook)
    count = len(calls)
    def no_cook(*a, **kw):
        pytest.fail("unchanged package cooked")
    kit.finish(target, manifest, tool, no_cook)
    other = target.with_name("other")
    shutil.copytree(target / kit.KIT_REL, other / kit.KIT_REL)
    kit.finish(other, manifest, tool, no_cook)
    recipe = next((other / "cache/content-model").glob("*.mci"))
    original = recipe.read_bytes()
    assert recipe.stat().st_nlink >= 2
    # Runtime publication replaces directory entries; receipts remain private.
    replacement = recipe.with_suffix(".tmp")
    replacement.write_bytes(b"lane write")
    recipe.unlink()
    os.replace(replacement, recipe)
    assert next((target / "cache/content-model").glob(recipe.name)).read_bytes() == original
    assert len(calls) == count
    assert not (other / "cache/content-model/industrial-cook.json").samefile(
        target / "cache/content-model/industrial-cook.json")


def test_changed_package_is_recooked(cook_install):
    target, manifest, tool, cook, calls = cook_install
    kit.finish(target, manifest, tool, cook)
    row = manifest["files"][0]
    path = target / kit.KIT_REL / row["path"]
    path.write_bytes(b"changed asset")
    row.update(bytes=path.stat().st_size, sha256=kit.sha(path))
    kit.finish(target, manifest, tool, cook)
    assert len(calls) == 4  # only the changed model recooks


def test_second_full_payload_deploy_copies_nothing_and_cooks_nothing(cook_install, monkeypatch):
    import deploy_payload as dp
    target, manifest, tool, cook, calls = cook_install
    monkeypatch.setattr(dp, 'ensure_terrain_materials', lambda *a: {})
    monkeypatch.setattr(dp, 'ensure_engine_normal_cooks', lambda *a: None)
    _, items = kit.payload(target.parent)
    dp.deploy(items, str(target), False)
    kit.finish(target, manifest, tool, cook)
    count = len(calls)
    monkeypatch.setattr(dp.shutil, 'copyfile', lambda *a, **kw: pytest.fail('unchanged payload copied'))
    dp.deploy(items, str(target), False)
    kit.finish(target, manifest, tool, lambda *a, **kw: pytest.fail('unchanged payload cooked'))
    assert len(calls) == count


def test_recook_detaches_shared_outputs_before_in_place_writes(cook_install):
    target, manifest, tool, cook, calls = cook_install
    kit.finish(target, manifest, tool, cook)
    recipe = next((target / 'cache/content-model').glob('*.mci'))
    shared = next(kit.cook_cache_root().glob('models/*/*/' + recipe.name))
    recipe.unlink()
    kit.finish(target, manifest, tool, cook)  # restored from the shared cache as a link
    assert recipe.samefile(shared)
    before = shared.read_bytes()
    row = manifest["files"][0]
    path = target / kit.KIT_REL / row["path"]
    path.write_bytes(b"changed")
    row.update(bytes=path.stat().st_size, sha256=kit.sha(path))
    kit.finish(target, manifest, tool, cook)
    assert shared.read_bytes() == before


def test_recipe_evicted_by_a_later_write_in_the_same_cook_is_restored(cook_install, monkeypatch):
    # A full native cache (LRU by mtime, hits never refreshed) can evict a recipe this cook already got (W138b laptop gate).
    target, manifest, tool, cook, calls = cook_install
    class Sequential:
        def __init__(self, **kwargs): pass
        def __enter__(self): return self
        def __exit__(self, *exc): return False
        map = staticmethod(map)
    monkeypatch.setattr(kit, "ThreadPoolExecutor", Sequential)
    cache = target / "cache/content-model"
    def evicting(command, **kwargs):
        result = cook(command, **kwargs)
        if len(calls) == 2:   # the second model's write sweeps the first model's recipe
            (cache / f"model-{hashlib.sha256(calls[0].encode()).hexdigest()[:16]}.mci").unlink()
        return result
    kit.finish(target, manifest, tool, evicting)
    assert len(calls) == 3   # the evicted recipe is relinked from the shared cook cache, not cooked again
    rows = json.loads((cache / "industrial-cook.json").read_text())["files"]
    assert len(rows) == 3 and all((cache / r["path"]).is_file() for r in rows)


def test_stock_receipt_retained_and_corruption_removes_stamp(tmp_path, monkeypatch):
    path = tmp_path / "data/tgl/test.ktx2"
    path.parent.mkdir(parents=True)
    path.write_bytes(b"hello")
    inventory = {"sha256": {}, "cooked_sha256": {"data/tgl/test.ktx2": kit.sha(path)}}
    assert kit.reconcile_stock_textures(tmp_path, inventory) == 1
    stamp = Path(str(path) + ".cookstamp")
    assert stamp.read_text().strip() == "mc2-stock-v1 a430d84680aabd0b"
    before = stamp.stat().st_mtime_ns
    monkeypatch.setattr(kit, "stock_fnv", lambda *a: pytest.fail("retained stock rehashed with FNV"))
    assert kit.reconcile_stock_textures(tmp_path, inventory) == 1
    assert stamp.stat().st_mtime_ns == before
    path.write_bytes(b"wrong")
    assert kit.reconcile_stock_textures(tmp_path, inventory) == 0
    assert not stamp.exists()


def test_ref_root_reroots_only_the_reference_store(monkeypatch):
    monkeypatch.delenv("MC2_REF_ROOT", raising=False)
    assert kit.ref_path("E:/mc2-ref/packages/k") == "E:/mc2-ref/packages/k"
    monkeypatch.setenv("MC2_REF_ROOT", r"C:\mc2gate\ref")
    assert kit.ref_path(r"E:\mc2-ref\packages\k") == "C:/mc2gate/ref/packages/k"
    assert kit.ref_path("E:/mc2-refx/k") == "E:/mc2-refx/k"
    assert kit.ref_path("D:/store/k") == "D:/store/k"


def test_cook_cache_root_follows_the_gate_e_root(monkeypatch):
    # The farm laptop has no E: drive (W138d laptop gate: deploy failed "device is not ready: E:\mc2-cook-cache").
    monkeypatch.delenv("MC2_COOK_CACHE", raising=False)
    monkeypatch.delenv("MC2_GATE_EROOT", raising=False)
    assert kit.cook_cache_root() == Path("E:/mc2-cook-cache")
    monkeypatch.setenv("MC2_GATE_EROOT", "C:/mc2gate/drv/E/")
    assert kit.cook_cache_root() == Path("C:/mc2gate/drv/E/mc2-cook-cache")
    monkeypatch.setenv("MC2_COOK_CACHE", "D:/cc")
    assert kit.cook_cache_root() == Path("D:/cc")
