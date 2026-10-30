"""Release validation rejects stale runtime files and missing baked shaders."""
from pathlib import Path
import sys
import zipfile
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import test_release_zip_install as release


def test_dll_allowlist_rejects_unreviewed_files(tmp_path):
    (tmp_path / 'glew32.dll').touch()
    failures = release.verify_tree(tmp_path)
    assert any('unreviewed DLL: glew32.dll' in f for f in failures)
    assert any('SDL2.dll' in f for f in failures)


def test_source_shaders_cannot_replace_baked_spirv(tmp_path):
    shaders = tmp_path / 'shaders/vulkan'
    shaders.mkdir(parents=True)
    (shaders / 'test.vert').write_text('source')
    (shaders / 'test.frag').write_text('source')
    (shaders / 'test.vert.spv').touch()
    assert any('baked .spv' in f for f in release.verify_tree(tmp_path))
    (shaders / 'test.vert.spv').write_bytes(b'\x03\x02\x23\x07')
    assert not any('baked .spv' in f for f in release.verify_tree(tmp_path))


def test_retired_launcher_is_rejected(tmp_path):
    (tmp_path / 'mc2-launcher.exe').touch()
    assert any('retired' in f for f in release.verify_tree(tmp_path))
    assert not any('mods/' in f for f in release.verify_tree(tmp_path))


def test_personal_configs_and_backups_are_rejected(tmp_path):
    for name in ('options.cfg', 'h2prefs.cfg', 'mc2-launcher.exe.bak', 'harness_x.txt'):
        (tmp_path / name).touch()
    failures = release.verify_tree(tmp_path)
    assert all(any(name in f for f in failures) for name in
               ('options.cfg', 'h2prefs.cfg', 'mc2-launcher.exe.bak', 'harness_x.txt'))


def test_each_archive_requires_licenses(tmp_path):
    archive = tmp_path / 'engine.zip'
    with zipfile.ZipFile(archive, 'w') as zf:
        zf.writestr('mc2.exe', b'exe')
    with pytest.raises(ValueError, match='missing license.txt'):
        release.extract_zip(archive, tmp_path / 'install', 'engine')


def test_archive_cannot_escape_empty_install(tmp_path):
    archive = tmp_path / 'engine.zip'
    with zipfile.ZipFile(archive, 'w') as zf:
        for name in ('license.txt', 'EULA.txt', 'LICENSES.md', '../escape.txt'):
            zf.writestr(name, b'content')
    with pytest.raises(ValueError, match='unsafe archive path'):
        release.extract_zip(archive, tmp_path / 'install', 'engine')
    assert not (tmp_path / 'escape.txt').exists()


def test_release_requires_baked_shaders(tmp_path):
    for name in release.REQUIRED_FILES + release.REQUIRED_DLLS:
        (tmp_path / name).touch()
    for name in release.REQUIRED_DIRS:
        (tmp_path / name).mkdir(exist_ok=True)
        (tmp_path / name / "fixture").touch()
    (tmp_path / "data/missions").mkdir()
    (tmp_path / "test.fst").touch()
    (tmp_path / "shaders/test.vert").touch()
    (tmp_path / "shaders/test.frag").touch()
    assert release.verify_tree(tmp_path) == [
        "shaders/vulkan/ contains no non-empty baked .spv files"]
    (tmp_path / "shaders/vulkan").mkdir()
    (tmp_path / "shaders/vulkan/test.vert.spv").write_bytes(b"\x03\x02\x23\x07")
    assert release.verify_tree(tmp_path) == []
    (tmp_path / "glew32.dll").touch()
    assert release.verify_tree(tmp_path) == ["unreviewed DLL: glew32.dll"]
    (tmp_path / "glew32.dll").unlink()
    (tmp_path / "SDL2.dll").unlink()
    assert release.verify_tree(tmp_path) == ["required DLL missing: SDL2.dll"]
    (tmp_path / "SDL2.dll").touch()
    (tmp_path / "prefs.cfg").unlink()
    assert release.verify_tree(tmp_path) == ["required file missing: prefs.cfg"]
