#!/usr/bin/env python3
"""Guard the shared native device plus SDL windows after GL-context retirement."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'GameOS/gameos/'
FILES = [PREFIX + name for name in (
    'gos_render.cpp', 'gos_render.h', 'gameosmain.cpp', 'gameosmain_frame.cpp',
    'gameosmain_internal.h', 'gameos_graphics.cpp', 'gameos_graphics_renderer_internal.h',
    'gos_host_vk.cpp', 'gos_host_vk.h', 'engine_present_vk.cpp', 'engine_present_vk.h',
    'gos_profiler.h', 'gameos_graphics_water.cpp', 'render_host_kind.h', 'CMakeLists.txt')]
FILES += ['editor/EditorGosRender.cpp', 'editor/EditorGameOS.cpp',
          'editor/EditorInterface_internal.h', 'editor/EditorInterface_mouse.cpp',
          'editor/shell/main.cpp', 'editor/shell/PanelProof.cpp',
          'ui_editor/UiEditorMain.cpp', 'ui_editor/UiEditorImageCache.cpp']


def code(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  ' ', source, flags=re.S)


def snapshot(root=ROOT):
    return {path: (root / path).read_text(encoding='utf-8') for path in FILES}


def audit(files):
    failures = []
    for path, source in files.items():
        tokens = code(source)
        if re.search(r'\b(?:gl[A-Z]\w*|glew\w*|wgl\w*|SDL_GL_\w*)\s*\(|'
                     r'\b(?:RenderContextHandle|SDL_GLContext|HGLRC|TracyGpu\w*|SDL_WINDOW_OPENGL)\b', tokens):
            failures.append(path + ': retired GL context/loader/state dependency')
        if re.search(r'^\s*#\s*include\s*[<"](?:GL/|SDL_opengl|imgui_impl_opengl|tracy/TracyOpenGL)', source, re.M):
            failures.append(path + ': retired GL header dependency')
    for path in (PREFIX + 'gos_render.cpp', 'editor/EditorGosRender.cpp',
                 'editor/shell/main.cpp', 'ui_editor/UiEditorMain.cpp'):
        if 'SDL_WINDOW_VULKAN' not in code(files[path]):
            failures.append(path + ': native SDL window flag missing')
    for path in (PREFIX + 'gos_render.cpp', 'editor/EditorGosRender.cpp', PREFIX + 'engine_present_vk.cpp'):
        if 'SDL_GetWindowSizeInPixels' not in code(files[path]):
            failures.append(path + ': native pixel extent query missing')
    if 'SDL_HINT_VIDEO_FOREIGN_WINDOW_VULKAN' not in code(files['editor/EditorGosRender.cpp']):
        failures.append('editor foreign child must wrap a Vulkan window')
    for path in (PREFIX + 'gameosmain.cpp', 'editor/EditorGameOS.cpp'):
        tokens = code(files[path])
        if not re.search(r'\binitVulkanBackend\s*\(', tokens):
            failures.append(path + ': real native device startup missing')
        if tokens.find('shutdownVulkanBackend(') > tokens.find('graphics::destroy_window('):
            failures.append(path + ': GPU owner must shut down before its SDL window')
    native = code(files[PREFIX + 'gos_host_vk.cpp'])
    for call in ('vkGetPhysicalDeviceProperties', 'GetModuleHandleW', 'fatalBackendFailure'):
        if not re.search(r'\b' + call + r'\s*\(', native):
            failures.append('native host query/startup missing: ' + call)
    renderer = code(files[PREFIX + 'gameos_graphics_renderer_internal.h'])
    if not re.search(r'getRenderWindowHandle\s*\([^)]*\)\s*\{\s*return win_h_;', renderer):
        failures.append('renderer must borrow the actual window, with no dummy context wrapper')
    if 'gos_render_context.cpp' in files[PREFIX + 'CMakeLists.txt']:
        failures.append('CMake still compiles retired GL context conventions')
    return failures


def main():
    failures = audit(snapshot())
    for failure in failures:
        print('[native-render-hosts] FAIL: ' + failure, file=sys.stderr)
    if not failures and '--quiet' not in sys.argv:
        print('[native-render-hosts] PASS: real SDL windows plus shared native device; no GL host/context')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
