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


def test_completeness(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "_pe_imports", lambda path: set())  # the fake MZ exe is not a PE
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (target / EXE).write_bytes(b"MZ")
    for d in dp.FFMPEG_DLLS:
        (build / d).write_bytes(b"x")
    # check-only: nothing copied, everything missing is listed
    gaps = dp.check_install_complete(str(target), EXE, str(build), fix=False)
    assert set(dp.FFMPEG_DLLS) <= set(gaps) and {"assets/", "data/", "shaders/"} <= set(gaps)
    assert not (target / dp.FFMPEG_DLLS[0]).exists()
    # fix: DLLs copied from the build dir, only the folders remain
    assert dp.check_install_complete(str(target), EXE, str(build)) == ["assets/", "data/", "shaders/"]
    assert (target / dp.FFMPEG_DLLS[0]).is_file()
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
    monkeypatch.setattr(dp, "FFMPEG_DLLS", ())
    monkeypatch.setattr(dp, "_pe_imports", lambda path: graph.get(Path(path).name, set()))
    gaps = dp.check_install_complete(str(target), EXE, str(build), fix=True)
    assert "b_dep.dll" in gaps and "a_dep.dll" not in gaps


def test_no_exe_anywhere_is_a_gap(tmp_path):
    (tmp_path / "t").mkdir()
    (tmp_path / "b").mkdir()
    gaps = dp.check_install_complete(str(tmp_path / "t"), EXE, str(tmp_path / "b"), fix=False)
    assert any(EXE in g for g in gaps)
