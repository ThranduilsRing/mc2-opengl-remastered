"""Disposable mod staging keeps runtime fonts, stock gaps and mod overrides."""
from pathlib import Path
import pytest
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import mod_smoke


def _write_fst(path, members):
    offset = 8 + 266 * len(members)
    table, blobs = b"", b""
    for name, data in members.items():
        table += struct.pack("<IIII250s", offset + len(blobs), len(data), len(data), 0, name.encode())
        blobs += data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<II", 0xCADDECAF, len(members)) + table + blobs)


def test_stage_preserves_mod_assets_and_supplies_missing_runtime_fonts(tmp_path):
    base, mod, staged = (tmp_path / name for name in ("base", "mod", "staged"))
    for root in (base, mod):
        (root / "assets/graphics").mkdir(parents=True)
    (mod / "data/missions").mkdir(parents=True)
    (mod / "data/missions/first.fit").write_bytes(b"mission")
    (base / "assets/graphics/arial8.bmp").write_bytes(b"base-font")
    (base / "assets/graphics/shared.bmp").write_bytes(b"base-style")
    (mod / "assets/graphics/shared.bmp").write_bytes(b"mod-style")
    (mod / "assets/graphics/custom.bmp").write_bytes(b"mod-only")
    _write_fst(base / "camera.fst", {"data/cameras/cameras.fit": b"stock-camera"})
    _write_fst(mod / "camera.fst", {"data/cameras/custom.fit": b"mod-camera"})
    (base / "system.cfg").write_text(
        '[FastFiles]\nl NumFastFiles=1\nst File0="camera.fst"\n\n[After]\nst Keep="yes"\n',
        encoding="utf-8")
    before = {root: mod_smoke.tree_identity(root) for root in (base, mod)}

    mod_smoke.stage_mod(base, [mod], staged, "first", base)

    assert (staged / "assets/graphics/arial8.bmp").read_bytes() == b"base-font"
    assert (staged / "assets/graphics/shared.bmp").read_bytes() == b"mod-style"
    assert (staged / "assets/graphics/custom.bmp").read_bytes() == b"mod-only"
    assert (base / "assets/graphics/shared.bmp").read_bytes() == b"base-style"
    assert (staged / "data/missions/first.fit").read_bytes() == b"mission"
    assert (staged / "camera.fst").read_bytes() == (mod / "camera.fst").read_bytes()
    assert (staged / "data/cameras/cameras.fit").read_bytes() == b"stock-camera"
    assert not (staged / "data/cameras/custom.fit").exists()
    config = (staged / "system.cfg").read_bytes()
    assert config == (base / "system.cfg").read_bytes()
    mod_smoke.stage_mod(base, [mod], staged, "first", base)
    assert (staged / "system.cfg").read_bytes() == config
    assert {root: mod_smoke.tree_identity(root) for root in (base, mod)} == before


def test_carver_campaign_layers_on_omnitech_without_mutating_sources(tmp_path):
    base, mods, staged = (tmp_path / name for name in ("base", "mods", "staged"))
    _write_fst(base / "camera.fst", {"data/cameras/cameras.fit": b"stock-camera"})
    parent = mods / "MechCommanderOmnitech"
    overlay = mods / "MCO-Carver5"
    for root, stem in ((parent, "a_0_1"), (overlay, "mc2_01")):
        (root / "data/missions").mkdir(parents=True)
        (root / f"data/missions/{stem}.fit").write_bytes(stem.encode())
    (parent / "data/objects.pak").write_bytes(b"omnitech-types")
    (parent / "data/shared.fit").write_bytes(b"parent")
    (overlay / "data/shared.fit").write_bytes(b"campaign-override")
    _write_fst(parent / "camera.fst", {"data/cameras/mcocameras.fit": b"omnitech-camera"})
    before = {root: mod_smoke.tree_identity(root) for root in (base, mods)}
    layers = mod_smoke.mod_layers(mods, "MCO-Carver5", "MechCommanderOmnitech")

    mod_smoke.stage_mod(base, layers, staged, "mc2_01", base)

    assert (staged / "data/objects.pak").read_bytes() == b"omnitech-types"
    assert (staged / "data/shared.fit").read_bytes() == b"campaign-override"
    assert (staged / "data/missions/mc2_01.fit").read_bytes() == b"mc2_01"
    assert (staged / "camera.fst").read_bytes() == (parent / "camera.fst").read_bytes()
    assert (staged / "data/cameras/cameras.fit").read_bytes() == b"stock-camera"
    assert {root: mod_smoke.tree_identity(root) for root in (base, mods)} == before


def test_cached_immutable_overlay_preserves_links_and_private_configs(tmp_path):
    import os
    layer, staged = tmp_path / "layer", tmp_path / "stage"
    (layer / "data").mkdir(parents=True)
    (layer / "data/objects.pak").write_bytes(b"immutable")
    (layer / "data/types.fit").write_bytes(b"private")
    mod_smoke.overlay_mod(layer, staged)
    assert os.path.samefile(layer / "data/objects.pak", staged / "data/objects.pak")
    assert not os.path.samefile(layer / "data/types.fit", staged / "data/types.fit")
    inode = (staged / "data/objects.pak").stat().st_ino
    (staged / "data/types.fit").write_bytes(b"runtime edits")
    mod_smoke.overlay_mod(layer, staged)
    assert (staged / "data/objects.pak").stat().st_ino == inode
    assert (staged / "data/types.fit").read_bytes() == b"private"
    assert (layer / "data/types.fit").read_bytes() == b"private"


@pytest.mark.parametrize("crash", [False, True, "engine"])
def test_battery_isolates_workers_and_cleans_even_on_exception(tmp_path, monkeypatch, crash):
    import os
    import threading
    from types import SimpleNamespace
    mods, base, build = tmp_path / "mods", tmp_path / "base", tmp_path / "build64-ninja"
    for root in (base, build):
        root.mkdir()
    (build / "mc2.exe").write_bytes(b"exe")
    for name in ("parent", "one", "two"):
        folder = mods / name / "data/missions"
        folder.mkdir(parents=True)
        (folder / "first.fit").write_bytes(b"mission")
    (mods / "parent/data/shared.pak").write_bytes(b"packed")
    (mods / "one/data/one-only.fit").write_bytes(b"one")
    monkeypatch.setattr(mod_smoke, "BATTERY", (("one", "one", "parent"), ("two", "two", "parent")))
    monkeypatch.setattr(mod_smoke, "pick_mission", lambda *a: ("first", "fixture"))
    monkeypatch.setattr(mod_smoke.stages, "preflight", lambda *a: None)
    def engine(cache, *a):
        cache.mkdir()
        (cache / "mc2.exe").write_bytes(b"exe")
        if crash == "engine":
            raise OSError("fixture launch failure")
        return {}
    monkeypatch.setattr(mod_smoke, "build_engine_cache", engine)
    monkeypatch.setattr(mod_smoke, "_load_deploy_payload", lambda: SimpleNamespace(ensure_terrain_materials=lambda *a: None))
    runs = []
    rendezvous = threading.Barrier(2)
    warmed = False
    def run(stage, *a):
        runs.append((stage, (stage / "data/shared.pak").stat().st_ino))
        rendezvous.wait(timeout=5)  # both game workers must enter before either finishes
        if stage.name == 'two':
            assert not (stage / "data/one-only.fit").exists()
            assert not (stage / "data/runtime-leak.fit").exists()
            if crash:
                raise OSError("fixture launch failure")
        (stage / "data/runtime-leak.fit").write_bytes(b"runtime")
        if warmed:
            assert (stage / 'cache/content-model/model-one.mci').read_bytes() == b'cooked'
            assert (stage / 'cache/content-model/model-two.mci').read_bytes() == b'cooked'
        recipes = stage / 'cache/content-model'
        recipes.mkdir(parents=True, exist_ok=True)
        (recipes / f'model-{stage.name}.mci').write_bytes(b'cooked')
        (stage / "smoke.log").write_text("fixture")
        return {"exit": 0, "health": {"mech_ok": True, "ai_ok": True,
                 "mech_instances_max": 1, "mlr_mech_draws": 0, "first_brain_s": 0.2, "first_order_s": None}}
    monkeypatch.setattr(mod_smoke, "run_mod", run)
    args = SimpleNamespace(install_root=str(mods), stage_root=str(tmp_path / "stages"), base_install=str(base),
                           exe=str(build / "mc2.exe"), build_dir="build64-ninja", mod="", mission="",
                           keep_stages=False, duration=10, timeout=60, fail_fast=False)
    if crash:
        with pytest.raises(OSError, match="fixture launch failure"):
            mod_smoke.battery(args)
    else:
        assert mod_smoke.battery(args) == 0
    if crash == "engine":
        assert runs == []
    else:
        assert len(runs) == 2 and runs[0][0] != runs[1][0]
        assert runs[0][1] == runs[1][1]  # same immutable parent inode
        assert all(not stage.exists() for stage, _ in runs)
    assert all('_cache' in p.parts for p in (tmp_path / "stages").rglob("*.pak"))
    assert not list((tmp_path / "stages").rglob("mc2.exe"))
    assert (mods / "parent/data/shared.pak").read_bytes() == b"packed"
    if not crash:
        warmed = True
        assert mod_smoke.battery(args) == 0
        assert len(runs) == 4


def test_cross_volume_asset_links_survive_cached_overlay_and_leave_sources_unchanged(tmp_path, monkeypatch):
    import os
    source, cache, staged = (tmp_path / n for n in ("source", "cache", "stage"))
    (source / "data/missions").mkdir(parents=True)
    (source / "data/missions/first.fit").write_bytes(b"mission")
    (source / "data/packed.pak").write_bytes(b"packed")
    (source / "data/config.fit").write_bytes(b"original")
    probe = tmp_path / "link-probe"
    try:
        os.symlink(source / "data/packed.pak", probe)
    except OSError as error:
        if getattr(error, "winerror", None) == 1314:
            pytest.skip("Windows file-link privilege unavailable; copy fallback has separate coverage")
        raise
    finally:
        probe.unlink(missing_ok=True)
    def different_volume(*a, **k):
        raise OSError("fixture: cross-volume hardlink")
    monkeypatch.setattr(os, "link", different_volume)
    mod_smoke.overlay_mod(source, cache)
    assert (cache / "data/packed.pak").is_symlink()
    mod_smoke.overlay_mod(cache, staged, cached_links=True)
    assert (staged / "data/packed.pak").read_bytes() == b"packed"
    assert os.path.samefile(staged / "data/packed.pak", source / "data/packed.pak")
    assert not (staged / "data/config.fit").is_symlink()
    (staged / "data/config.fit").write_bytes(b"runtime edits")
    assert (source / "data/config.fit").read_bytes() == b"original"
    import shutil
    shutil.rmtree(staged)
    shutil.rmtree(cache)
    assert (source / "data/packed.pak").read_bytes() == b"packed"


def test_file_link_failure_falls_back_to_independent_copy(tmp_path, monkeypatch):
    import os
    source, dest = tmp_path / "asset.pak", tmp_path / "copy.pak"
    source.write_bytes(b"asset")
    def denied(*a, **k):
        raise OSError("fixture: linking unavailable")
    monkeypatch.setattr(os, "link", denied)
    monkeypatch.setattr(os, "symlink", denied)
    assert mod_smoke.place_file(source, dest) == "copy"
    assert dest.read_bytes() == b"asset" and not os.path.samefile(source, dest)
