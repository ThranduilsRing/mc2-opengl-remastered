"""Release validation rejects stale runtime files and missing baked shaders."""
from pathlib import Path
import sys

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
