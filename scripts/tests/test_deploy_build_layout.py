#!/usr/bin/env python3
# scripts/tests/test_deploy_build_layout.py
"""Tests for deploy_payload's VS-vs-Ninja build-tree layout detection.

The VS tree nests outputs under a config dir; the Ninja dev tree (build64-ninja)
is single-config and does not. Deploy must find the exe, launcher and the
freshness-reference .obj files in both without the caller saying which.

Run:
  py -3 -m pytest scripts/tests/test_deploy_build_layout.py -v
"""
from __future__ import annotations

import os
import pytest
from contextlib import ExitStack
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402
from scripts import console_child  # noqa: E402


@pytest.fixture(autouse=True)
def _no_texture_cooks(monkeypatch):
    # enumerate_payload cooks missing generated sidecars (tree upscale takes minutes); these tests only list the payload.
    for name in ("ensure_concrete_detail", "ensure_cliff_rock", "ensure_tree_upscale"):
        monkeypatch.setattr(dp, name, lambda *a, **k: None)


def test_asset_sidecar_deploy_is_identical_across_checkout_roots(monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    rel = "data/tgl/Fixture.mcasset.json"
    tracked = b"".join(b'{"line": %d}\n' % i for i in range(40))
    mixed = b"".join(line.replace(b"\n", b"\r\n") if i % 3 else line
                     for i, line in enumerate(tracked.splitlines(keepends=True)))
    assert mixed != tracked and mixed.replace(b"\r\n", b"\n") == tracked
    outputs = []
    with ExitStack() as cleanup:
        for checkout_bytes in (mixed, tracked):
            root = Path(tempfile.gettempdir()) / f"mcasset-copy-{uuid.uuid4().hex}"
            root.mkdir()
            cleanup.callback(shutil.rmtree, root)
            source = root / "checkout" / rel
            source.parent.mkdir(parents=True)
            source.write_bytes(checkout_bytes)
            work = root / "working-directory"
            work.mkdir()
            with monkeypatch.context() as patch:
                patch.chdir(work)
                hashes = dp.deploy([(str(source), rel, "support")], str(root / "payload"), False)
            output = root / "payload" / rel
            outputs.append(output.read_bytes())
            assert hashes[rel][:2] == (dp.sha256_file(str(output)), len(tracked))
            assert source.read_bytes() == checkout_bytes
    assert outputs == [tracked, tracked]


def _touch(p: Path, mtime=None):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"x")
    if mtime is not None:
        os.utime(p, (mtime, mtime))
    return p


def _vs_tree(tmp_path):
    b = tmp_path / "build64"
    _touch(b / "RelWithDebInfo" / "mc2.exe")
    _touch(b / "mc2.dir" / "RelWithDebInfo" / "a.obj")
    return b


def _ninja_tree(tmp_path):
    b = tmp_path / "build64-ninja"
    _touch(b / "build.ninja")
    _touch(b / "mc2.exe")
    _touch(b / "crashpad_handler.exe")
    for dll in dp.RUNTIME_DLLS:
        _touch(b / dll)
    _touch(b / "shaders/vulkan/fixture.vert.spv")
    _touch(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj")
    return b


def test_vs_layout_not_single_config(tmp_path):
    b = _vs_tree(tmp_path)
    bd = str(b / "RelWithDebInfo")
    root = dp.find_build_root(bd)
    assert root == str(b)
    assert not dp.is_single_config(bd, root)
    assert dp.mc2_obj_dir(root, "RelWithDebInfo", False) == str(b / "mc2.dir" / "RelWithDebInfo")


def test_ninja_layout_detected(tmp_path):
    b = _ninja_tree(tmp_path)
    root = dp.find_build_root(str(b))
    assert root == str(b)
    assert dp.is_single_config(str(b), root)
    assert Path(dp.mc2_obj_dir(root, "build64-ninja", True)).is_dir()


def test_enumerate_payload_needs_only_game(tmp_path):
    b = _ninja_tree(tmp_path)
    items = dp.enumerate_payload(str(ROOT), str(b), "mc2.exe", "mc2.pdb")
    rel = {r for _s, r, _k in items}
    assert "mc2.exe" in rel
    assert "mc2-launcher.exe" not in rel
    assert {"tools/mc2_import/import_tool.py", "tools/mc2x_import/manifest.py",
            "tools/mco_import/build_object2.py", "scripts/console_child.py"} <= rel
    assert not any("__pycache__" in r or r.endswith((".pyc", ".pyo")) for r in rel)


def test_approved_road_art_is_deployed_and_parity_owned(tmp_path):
    # Trigger paths: scripts/deploy_payload.py, data/textures/256Overlays/.
    from scripts import asset_parity
    expected = {f"data/textures/256Overlays/{family}{i:04d}.tga"
                for family in ("runway", "pavedroad", "dirtroad", "2lanedirt") for i in range(15)}
    items = dp.enumerate_payload(str(ROOT), str(_ninja_tree(tmp_path)), "mc2.exe", "mc2.pdb")
    payload = {rel: Path(src) for src, rel, _ in items}
    assert expected <= payload.keys()
    hashes = {rel: dp.sha256_file(payload[rel]) for rel in expected}
    assert asset_parity.owned_paths(ROOT, payload=hashes, declared={}) == {rel.lower() for rel in expected}


def test_missing_baked_shaders_refuses_deploy(tmp_path):
    import pytest
    b = _ninja_tree(tmp_path)
    (b / "shaders/vulkan/fixture.vert.spv").unlink()
    with pytest.raises(SystemExit):
        dp.enumerate_payload(str(ROOT), str(b), "mc2.exe", "mc2.pdb")


def test_dynamic_resource_dll_is_required(tmp_path):
    import pytest
    b = _ninja_tree(tmp_path)
    (b / "mc2res_64.dll").unlink()
    with pytest.raises(SystemExit):
        dp.enumerate_payload(str(ROOT), str(b), "mc2.exe", "mc2.pdb")


def test_native_payload_has_no_windows_runtime(tmp_path):
    b = _ninja_tree(tmp_path)
    _touch(b / "mc2")
    _touch(b / "crashpad_handler")
    items = dp.enumerate_payload(str(ROOT), str(b), "mc2", "mc2.pdb")
    rel = {r for _s, r, _k in items}
    assert "mc2" in rel
    assert "crashpad_handler" in rel
    assert not any(r.endswith((".exe", ".dll", ".pdb", ".bat")) for r in rel)


def test_stale_exe_detected_in_ninja_tree(tmp_path):
    """An .obj newer than the exe means the link did not happen."""
    b = _ninja_tree(tmp_path)
    now = time.time()
    os.utime(b / "mc2.exe", (now - 100, now - 100))
    os.utime(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj", (now, now))
    try:
        dp.check_exe_fresh(str(b / "mc2.exe"), str(b), False)
    except SystemExit:
        return
    raise AssertionError("stale exe not detected in Ninja layout")


def test_fresh_exe_passes_in_ninja_tree(tmp_path):
    b = _ninja_tree(tmp_path)
    now = time.time()
    os.utime(b / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj", (now - 100, now - 100))
    os.utime(b / "mc2.exe", (now, now))
    dp.check_exe_fresh(str(b / "mc2.exe"), str(b), False)


def test_stale_native_object_refuses_deploy(tmp_path):
    import pytest
    b = tmp_path / "build64-ninja"
    _touch(b / "build.ninja")
    _touch(b / "mc2", time.time() - 100)
    _touch(b / "CMakeFiles/mc2.dir/code/main.cpp.o", time.time())
    with pytest.raises(SystemExit):
        dp.check_exe_fresh(str(b / "mc2"), str(b), False)


def test_native_deploy_preserves_execute_permission(tmp_path, monkeypatch):
    import pytest
    if os.name == "nt":
        pytest.skip("POSIX execute permission")
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    source = _touch(tmp_path / "build" / "mc2")
    source.chmod(0o755)
    target = tmp_path / "install"
    dp.deploy([(str(source), "mc2", "exe")], str(target), False)
    assert os.access(target / "mc2", os.X_OK)


def test_missing_vegetation_card_refuses_deploy(tmp_path, monkeypatch, capsys):
    # A clean deploy must carry the native atlas inputs rather than silently
    # enabling vegetation against the old absent private atlas PNG.
    b = _ninja_tree(tmp_path)
    missing = os.path.normcase(os.path.abspath(ROOT / "data/vegetation/cards/sapling_pine.png"))
    isfile = os.path.isfile
    monkeypatch.setattr(dp.os.path, "isfile", lambda p:
        False if os.path.normcase(os.path.abspath(p)) == missing else isfile(p))
    try:
        dp.enumerate_payload(str(ROOT), str(b), "mc2.exe", "mc2.pdb")
    except SystemExit:
        assert "game data payload missing" in capsys.readouterr().err
        return
    raise AssertionError("deploy accepted missing vegetation atlas input")


def test_deploy_prunes_only_unchanged_manifest_files(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    target = tmp_path / "install"
    target.mkdir()
    owned = {}
    for rel in ("retired.spv", "modified.ini", "preserved.ini", "mods/user/keep.fit", "data/savegame/save.fit"):
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"original")
        owned[rel] = (dp.sha256_file(path), path.stat().st_size)
    dp.write_manifest(str(target), owned, "fixture")
    (target / "modified.ini").write_bytes(b"user change")
    (target / "save.fit").write_bytes(b"user save")
    dp.deploy([], str(target), False, preserve=("preserved.ini",), full_verify=True)
    assert not (target / "retired.spv").exists()
    assert (target / "modified.ini").read_bytes() == b"user change"
    assert (target / "preserved.ini").exists()
    assert (target / "save.fit").read_bytes() == b"user save"
    assert (target / "mods/user/keep.fit").exists()
    assert (target / "data/savegame/save.fit").exists()


def test_deploy_rejects_retired_path_outside_target(tmp_path, monkeypatch):
    import pytest
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    target = tmp_path / "install"
    target.mkdir()
    outside = tmp_path / "outside"
    outside.write_bytes(b"keep")
    dp.write_manifest(str(target), {"../outside": (dp.sha256_file(outside), 4)}, "fixture")
    with pytest.raises(SystemExit):
        dp.deploy([], str(target), False)
    assert outside.read_bytes() == b"keep"


def test_aa_payload_ships_notices_only(tmp_path):
    items = dp.enumerate_payload(str(ROOT), str(_ninja_tree(tmp_path)), "mc2.exe", "mc2.pdb")
    aa = [r for _, r, _ in items if r.startswith(("3rdparty/smaa/", "3rdparty/cmaa2/"))]
    assert aa
    assert all(Path(r).name.lower().startswith(("license", "notice", "upstream")) for r in aa)
