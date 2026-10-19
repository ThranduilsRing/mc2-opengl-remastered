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
