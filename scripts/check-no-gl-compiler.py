#!/usr/bin/env python3
"""Reject the retired GL backend, headers, dynamic loader and link dependencies.

The historical script name remains compatible with the compiler-retirement row.
GLSL assets and vendor platform SDKs are outside the owned application census.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import console_child
CALL = re.compile(r'\b(?:TracyGpu\w*|gl[A-Z]\w*|glew\w*|wgl\w*|SDL_GL_\w*|ImGui_ImplOpenGL\w*)\s*\(')
TYPE = re.compile(r'\b(?:SDL_GLContext|SDL_WINDOW_OPENGL|HGLRC|GLuint|GLenum|GLsizei|GLint|GLboolean|GLsync)\b')
HEADER = re.compile(r'^\s*#\s*include\s*[<"](?:GL/|SDL_opengl|imgui_impl_opengl|tracy/TracyOpenGL)', re.M)
LINK = re.compile(r'OpenGL::|GLEW::|find_package\s*\(\s*(?:OpenGL|GLEW)|glew32|opengl32|imgui_impl_opengl')
LOADER = re.compile(r'(?:LoadLibrary(?:Ex)?[AW]?|SDL_LoadObject)\s*\([^;]*(?:opengl32|glew32)|'
                    r'GetProcAddress\s*\([^;]*"(?:gl[A-Z]|glew|wgl)', re.S)



def code(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: '\n' * m[0].count('\n'), source, flags=re.S)


def audit(files):
    failures = []
    for path, source in files.items():
        if path.startswith(('3rdparty/include/GL/', '3rdparty/imgui/backends/imgui_impl_opengl')) or path == '3rdparty/tracy/tracy/TracyOpenGL.hpp':
            failures.append(path + ': retired GL SDK/backend source')
            continue
        if Path(path).parent.as_posix() == 'shaders' and Path(path).suffix in ('.vert', '.frag', '.comp', '.geom', '.tesc', '.tese'):
            failures.append(path + ': retired GL stage entry point')
            continue
        if path.endswith(('CMakeLists.txt', '.cmake')):
            matches = list(LINK.finditer(re.sub(r'#[^\n]*', '', source)))
        else:
            tokens = code(source)
            matches = list(CALL.finditer(tokens)) + list(TYPE.finditer(tokens)) + list(HEADER.finditer(source))
            # Module observation (GetModuleHandle) is allowed; driver loading/resolution is not.
            matches += list(LOADER.finditer(re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)))
        for match in matches:
            failures.append(f'{path}: retired GL dependency: {match[0]}')
    return failures


def snapshot(root=ROOT):
    names = subprocess.check_output(['git', 'ls-files'], cwd=root, text=True,
                                    **console_child.child_kwargs(redirected=True)).splitlines()
    names = [name for name in names if Path(name).parts[0] not in ('3rdparty', 'DirectX8')
             or name == '3rdparty/imgui/CMakeLists.txt'
             or name.startswith(('3rdparty/include/GL/', '3rdparty/imgui/backends/imgui_impl_opengl'))
             or name == '3rdparty/tracy/tracy/TracyOpenGL.hpp']
    return {name: (root / name).read_text(encoding='utf-8', errors='replace') for name in names
            if (Path(name).suffix in ('.cpp', '.c', '.h', '.hpp', '.cmake', '.vert', '.frag', '.comp', '.geom', '.tesc', '.tese') or Path(name).name == 'CMakeLists.txt') and (root / name).is_file()}


def main():
    failures = audit(snapshot())
    for failure in failures:
        print('[no-gl-compiler] FAIL: ' + failure, file=sys.stderr)
    if not failures and '--quiet' not in sys.argv:
        print('[no-gl-compiler] PASS: owned GL calls/headers/loader/link census zero')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
