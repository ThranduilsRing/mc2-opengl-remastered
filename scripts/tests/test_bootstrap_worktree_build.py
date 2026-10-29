"""Fresh native worktrees accept SDL dependencies and never restore the GL SDK."""
import importlib.util
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("native_bootstrap", ROOT / "scripts/bootstrap_worktree_build.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


def put(root, rel):
    path = root / "3rdparty" / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"fixture")
    return path


def test_native_cache_arguments_do_not_require_a_gl_sdk():
    args = bootstrap.cache_args("A:/Games/native-lane")
    assert "-DMC2_IMGUI=ON" in args
    assert not any("GLEW" in arg or "OpenGL" in arg for arg in args)


def test_native_cache_arguments_select_the_required_vulkan_backend():
    """G2: Vulkan is the required renderer backend, so the native lane configures it explicitly."""
    args = bootstrap.cache_args("A:/Games/native-lane")
    assert "-DMC2_VULKAN=ON" in args
    assert "-DMC2_VULKAN=OFF" not in args


def test_glew_only_donor_is_not_a_complete_native_sdk(tmp_path):
    donor = tmp_path / "donor"
    put(donor, "lib/x64/glew32.lib")
    with pytest.raises(SystemExit) as error:
        bootstrap.find_donor(str(tmp_path / "lane"), str(donor))
    assert error.value.code == 1


def test_native_donor_copy_filters_retired_gl_but_keeps_sdl_and_math(tmp_path):
    donor, lane = tmp_path / "donor", tmp_path / "lane"
    for rel in ("lib/x64/SDL2.lib", "include/SDL2/SDL.h", "include/glm/mat4x4.hpp",
                "cmake/SDL2/SDL2Config.cmake", "lib/x64/glew32.lib",
                "lib/x64/Glew32.dll", "include/GL/glew.h", "cmake/GLEW/GLEWConfig.cmake"):
        put(donor, rel)
    bootstrap.ensure_deps(str(lane), str(donor))
    for rel in ("lib/x64/SDL2.lib", "include/SDL2/SDL.h", "include/glm/mat4x4.hpp",
                "cmake/SDL2/SDL2Config.cmake"):
        assert (lane / "3rdparty" / rel).read_bytes() == b"fixture"
    assert not any("glew" in p.name.casefold() or p.name == "GL"
                   for p in (lane / "3rdparty").rglob("*"))


def test_native_render_hosts_require_vulkan_without_cache_options():
    """Vulkan and native regions cannot be disabled by stale cache entries."""
    root = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "option(MC2_VULKAN " not in root
    assert "option(MC2_VULKAN_ISLAND " not in root
    assert "set(MC2_VULKAN ON)" in root
    assert "set(MC2_VULKAN_ISLAND ON)" in root
    assert "find_package(Vulkan REQUIRED)" in root


def test_gated_layout_macros_stay_public_on_gameos():
    """G2: MC2_VULKAN_ISLAND changes the gosPostProcess layout, so gameos keeps exporting it."""
    gameos = (ROOT / "GameOS" / "gameos" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "gameos requires MC2_VULKAN=ON" in gameos
    assert "target_compile_definitions(gameos PUBLIC MC2_VULKAN_ISLAND)" in gameos
    assert "target_compile_definitions(gameos PRIVATE MC2_VULKAN)" in gameos
    root = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("gameos_main", "mc2", "mclib", "renderworld", "gameadapters", "gui_runtime"):
        assert f"target_compile_definitions({target} PRIVATE MC2_VULKAN_ISLAND)" in root


def test_populated_native_sdk_does_not_need_a_glew_sentinel(tmp_path, monkeypatch):
    for directory in bootstrap.DEP_DIRS:
        (tmp_path / "3rdparty" / directory).mkdir(parents=True, exist_ok=True)
    put(tmp_path, "lib/x64/SDL2.lib")
    monkeypatch.setattr(bootstrap, "find_donor", lambda *a: pytest.fail("unexpected donor search"))
    bootstrap.ensure_deps(str(tmp_path), None)
