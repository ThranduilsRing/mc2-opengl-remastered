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
