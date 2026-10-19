#!/usr/bin/env python3
"""Reject runtime OpenGL shader compilers in owned application sources.

Vendor SDK backends are checked when their host integrations are retired.
Vulkan shader compilation and GLSL source assets are outside this check.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ROOTS = ('GameOS', 'RenderCore', 'RenderWorld', 'GameAdapters', 'mclib',
         'code', 'gui', 'editor', 'text_tool', 'ui_editor', 'tools')
CALL = re.compile(r'\bgl(?:CreateShader(?:Programv)?|CreateProgram|CompileShader|'
                  r'LinkProgram|ShaderSource|ShaderBinary|SpecializeShader)\w*\s*\(')


def code(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: '\n' * m[0].count('\n'), source, flags=re.S)


def audit(files):
    failures = []
    for path, source in files.items():
        for match in CALL.finditer(code(source)):
            line = code(source)[:match.start()].count('\n') + 1
            failures.append(f'{path}:{line}: retired GL compiler: {match[0]}')
    return failures


def snapshot(root=ROOT):
    names = subprocess.check_output(['git', 'ls-files', '--', *ROOTS], cwd=root,
                                    text=True).splitlines()
    return {name: (root / name).read_text(encoding='utf-8') for name in names
            if Path(name).suffix in ('.cpp', '.c', '.h', '.hpp') and (root / name).is_file()}


def main():
    failures = audit(snapshot())
    for failure in failures:
        print('[no-gl-compiler] FAIL: ' + failure, file=sys.stderr)
    if not failures and '--quiet' not in sys.argv:
        print('[no-gl-compiler] PASS: owned runtime GL shader compiler census zero')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
