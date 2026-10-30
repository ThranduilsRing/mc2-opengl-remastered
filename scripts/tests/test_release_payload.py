"""Release gates must reject source-only shader payloads."""
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "release_install", Path(__file__).parents[1] / "test_release_zip_install.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


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
        "shaders/vulkan/ contains no baked .spv files"]
    (tmp_path / "shaders/vulkan").mkdir()
    (tmp_path / "shaders/vulkan/test.vert.spv").write_bytes(b"\x03\x02\x23\x07")
    assert release.verify_tree(tmp_path) == []
    (tmp_path / "glew32.dll").touch()
    assert release.verify_tree(tmp_path) == ["unreviewed runtime: glew32.dll"]
    (tmp_path / "glew32.dll").unlink()
    (tmp_path / "SDL2.dll").unlink()
    assert release.verify_tree(tmp_path) == ["required runtime missing: SDL2.dll"]
    (tmp_path / "SDL2.dll").touch()
    (tmp_path / "prefs.cfg").unlink()
    assert release.verify_tree(tmp_path) == ["required file missing: prefs.cfg"]
