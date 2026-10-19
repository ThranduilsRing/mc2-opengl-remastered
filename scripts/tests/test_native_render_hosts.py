"""The host-retirement oracle must reject real GL and ownership regressions."""
import importlib.util
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('native_render_hosts', ROOT / 'scripts/check-native-render-hosts.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_current_native_hosts():
    assert not checker.audit(checker.snapshot())


@pytest.mark.parametrize('path, before, after, expected', [
    ('GameOS/gameos/gos_render.cpp', 'SDL_WINDOW_VULKAN', 'SDL_WINDOW_OPENGL', 'retired GL'),
    ('editor/EditorGosRender.cpp', 'SDL_HINT_VIDEO_FOREIGN_WINDOW_VULKAN', 'old_hint', 'foreign child'),
    ('GameOS/gameos/gos_render.cpp', 'SDL_GetWindowSizeInPixels', 'oldSize', 'pixel extent'),
    ('editor/EditorGameOS.cpp', 'initVulkanBackend(g_editorRenderWindow, w, h);', '', 'device startup'),
])
def test_rejects_host_regressions(path, before, after, expected):
    files = checker.snapshot()
    assert before in files[path]
    files[path] = files[path].replace(before, after)
    assert any(expected in error for error in checker.audit(files))


def test_rejects_driver_call_even_when_no_context_type_is_declared():
    files = checker.snapshot()
    files['GameOS/gameos/gos_host_vk.cpp'] += '\nvoid bad() { glClear(0); }\n'
    assert any('retired GL' in error for error in checker.audit(files))

@pytest.mark.parametrize('path', ['editor/shell/main.cpp', 'editor/shell/PanelProof.cpp',
                                 'ui_editor/UiEditorMain.cpp', 'ui_editor/UiEditorImageCache.cpp'])
def test_tool_driver_calls_are_also_rejected(path):
    files = checker.snapshot()
    files[path] += '\nvoid bad() { glReadPixels(0); }\n'
    assert any('retired GL' in error for error in checker.audit(files))
