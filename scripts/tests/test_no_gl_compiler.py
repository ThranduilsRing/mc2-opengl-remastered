"""The compiler retirement check must catch new runtime compiler paths."""
import importlib.util
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('no_gl_compiler', ROOT / 'scripts/check-no-gl-compiler.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


@pytest.mark.parametrize('call', ['glCreateShader', 'glCreateProgram', 'glCompileShader',
    'glLinkProgram', 'glShaderSource', 'glShaderBinary', 'glSpecializeShaderARB',
    'glCreateShaderProgramv'])
def test_rejects_runtime_compiler(call):
    assert checker.audit({'host.cpp': f'void bad() {{ {call}(argument); }}'})


def test_ignores_comments_strings_and_native_calls():
    assert not checker.audit({'native.cpp': '// glCompileShader(old);\n'
        'const char* history="glCreateProgram()";\n'
        '/* glLinkProgram(old); */ vkCreateShaderModule(device, info);'})


def test_current_owned_sources_have_no_gl_compiler():
    assert not checker.audit(checker.snapshot())


@pytest.mark.parametrize('source', ['glReadPixels(0);', 'glewInit();', 'wglCreateContext(dc);',
    'SDL_GL_CreateContext(window);', 'ImGui_ImplOpenGL3_RenderDrawData(draw);',
    '#include <GL/glew.h>', '#include "imgui_impl_opengl3.h"',
    'SDL_GLContext context;', 'auto flags = SDL_WINDOW_OPENGL;',
    'LoadLibraryW(L"opengl32.dll");', 'GetProcAddress(module, "glBindTexture");'])
def test_rejects_backend_or_loader(source):
    assert checker.audit({'host.cpp': source})


@pytest.mark.parametrize('source', ['target_link_libraries(app OpenGL::GL)',
    'find_package(GLEW REQUIRED)', 'foreach(dll glew32.dll)',
    'target_sources(imgui PRIVATE backends/imgui_impl_opengl3.cpp)'])
def test_rejects_link_or_build_dependency(source):
    assert checker.audit({'CMakeLists.txt': source})


def test_module_observation_and_native_sdk_are_allowed():
    assert not checker.audit({'host.cpp': '#include <volk.h>\n'
        'GetModuleHandleW(L"opengl32.dll"); vkCreateDevice(device);',
        'CMakeLists.txt': '# OpenGL::GL was retired\ntarget_link_libraries(app Vulkan::Headers)'})
