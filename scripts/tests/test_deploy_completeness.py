"""check_install_complete: missing runtime DLLs are copied from the build dir, missing assets/data/shaders are reported."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402

EXE = "mc2.exe"


def test_native_dependencies_and_data_are_required(tmp_path, monkeypatch):
    from types import SimpleNamespace
    (tmp_path / "mc2").write_bytes(b"\x7fELF")
    monkeypatch.setattr(dp.subprocess, "run", lambda *a, **k: SimpleNamespace(
        returncode=0, stdout="libmissing.so => not found\nlibc.so => /lib/libc.so\n", stderr=""))
    assert dp.check_install_complete(str(tmp_path), "mc2", str(tmp_path)) == [
        "assets/", "data/", "libmissing.so", "shaders/"]
    monkeypatch.setattr(dp.subprocess, "run", lambda *a, **k: SimpleNamespace(
        returncode=1, stdout="", stderr="invalid ELF"))
    assert dp.check_install_complete(str(tmp_path), "mc2", str(tmp_path)) == [
        "cannot read ELF dependencies: invalid ELF"]


@pytest.mark.skipif(sys.platform != "win32", reason="Windows case-insensitive runtime DLL filenames")
def test_completeness(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "_pe_imports", lambda path: set())  # the fake MZ exe is not a PE
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (target / EXE).write_bytes(b"MZ")
    for d in dp.RUNTIME_DLLS:
        (build / d.lower()).write_bytes(b"x")
    # check-only: nothing copied, everything missing is listed
    gaps = dp.check_install_complete(str(target), EXE, str(build), fix=False)
    assert {d.lower() for d in dp.RUNTIME_DLLS} <= set(gaps) and {"assets/", "data/", "shaders/"} <= set(gaps)
    assert not (target / dp.RUNTIME_DLLS[0]).exists()
    # fix: DLLs copied from the build dir, only the folders remain
    assert dp.check_install_complete(str(target), EXE, str(build)) == ["assets/", "data/", "shaders/"]
    assert (target / dp.RUNTIME_DLLS[0]).is_file()
    for d in dp.REQUIRED_DIRS:
        (target / d).mkdir()
        (target / d / "f").write_bytes(b"x")
    assert dp.check_install_complete(str(target), EXE, str(build), fix=False) == []


def test_missing_pefile_or_unreadable_exe_is_loud_not_empty(tmp_path, monkeypatch):
    import builtins
    real = builtins.__import__

    def no_pefile(name, *a, **k):
        if name == "pefile":
            raise ImportError(name)
        return real(name, *a, **k)

    exe = tmp_path / EXE
    exe.write_bytes(b"MZ")
    monkeypatch.setattr(builtins, "__import__", no_pefile)
    with pytest.raises(RuntimeError, match="pefile"):
        dp._pe_imports(str(exe))
    gaps = dp.check_install_complete(str(tmp_path), EXE, str(tmp_path), fix=False)
    assert len(gaps) == 1 and "pefile" in gaps[0]
    monkeypatch.setattr(builtins, "__import__", real)
    gaps = dp.check_install_complete(str(tmp_path), EXE, str(tmp_path), fix=False)  # a fake MZ file is not a readable PE
    assert len(gaps) == 1 and "cannot read PE imports" in gaps[0]


def test_transitive_imports_are_walked(tmp_path, monkeypatch):
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (target / EXE).write_bytes(b"MZ")
    (build / "a_dep.dll").write_bytes(b"x")
    graph = {EXE: {"a_dep.dll"}, "a_dep.dll": {"b_dep.dll"}}
    monkeypatch.setattr(dp, "RUNTIME_DLLS", ())
    monkeypatch.setattr(dp, "_pe_imports", lambda path: graph.get(Path(path).name, set()))
    gaps = dp.check_install_complete(str(target), EXE, str(build), fix=True)
    assert "b_dep.dll" in gaps and "a_dep.dll" not in gaps


def test_no_exe_anywhere_is_a_gap(tmp_path):
    (tmp_path / "t").mkdir()
    (tmp_path / "b").mkdir()
    gaps = dp.check_install_complete(str(tmp_path / "t"), EXE, str(tmp_path / "b"), fix=False)
    assert any(EXE in g for g in gaps)


@pytest.mark.parametrize("single", [True, False])
def test_viewer_payload_provenance_runtime_and_shaders(tmp_path, monkeypatch, single):
    import os
    build = tmp_path / "build64-ninja"
    build.mkdir()
    if single:
        (build / "build.ninja").touch()
    output = build if single else build / "RelWithDebInfo"
    output.mkdir(exist_ok=True)
    viewer = build / "out/tools/asset_viewer"
    directory = viewer if single else viewer / "RelWithDebInfo"
    directory.mkdir(parents=True)
    exe = directory / "mc2_asset_viewer.exe"
    exe.write_bytes(b"MZ MC2FP1|" + b"a" * 40 + b"|0|test|2026-10-26T00:00:00|test|FP1END")
    shaders = directory / "shaders_vk"
    shaders.mkdir()
    for name in ("mesh_lit.vert", "mesh_lit.frag", "pbr_sphere.vert", "pbr_sphere.frag"):
        (shaders / (name + ".spv")).write_bytes(b"spv")
    (output / "sdl2.dll").write_bytes(b"sdl")
    graph = {exe.name: {"sdl2.dll"}, "sdl2.dll": {"transitive.dll"}}
    monkeypatch.setattr(dp, "_pe_imports", lambda path: graph.get(Path(path).name, set()))
    with pytest.raises(SystemExit):
        dp.viewer_payload(str(output), None, "b" * 40)
    with pytest.raises(SystemExit):
        dp.viewer_payload(str(output), None, "a" * 40)  # missing transitive DLL
    (output / "transitive.dll").write_bytes(b"dep")
    items = dp.viewer_payload(str(output), None, "a" * 40)
    assert {rel for _, rel, _ in items} >= {exe.name, "sdl2.dll", "transitive.dll", "shaders_vk/mesh_lit.vert.spv"}
    obj = viewer / "CMakeFiles/mc2_tglloader.dir/stale.obj"
    obj.parent.mkdir(parents=True)
    obj.write_bytes(b"obj")
    os.utime(obj, (exe.stat().st_mtime + 10,) * 2)
    with pytest.raises(SystemExit):
        dp.viewer_payload(str(output), None, "a" * 40)


def test_completeness_checks_installed_viewer(tmp_path, monkeypatch):
    (tmp_path / EXE).write_bytes(b"MZ")
    (tmp_path / "mc2_asset_viewer.exe").write_bytes(b"MZ")
    monkeypatch.setattr(dp, "RUNTIME_DLLS", ())
    monkeypatch.setattr(dp, "_pe_imports", lambda path: {"viewer_only.dll"}
                        if Path(path).name == "mc2_asset_viewer.exe" else set())
    assert "viewer_only.dll" in dp.check_install_complete(str(tmp_path), EXE, str(tmp_path), fix=False)
