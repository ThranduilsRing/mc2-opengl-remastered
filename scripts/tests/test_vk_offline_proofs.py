"""Regression tests for the Vulkan std140 layout checker."""
import importlib.util
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


ubo = _load("check_ubo_layout")


# --- check_ubo_layout -------------------------------------------------------

def _glsl_offsets(tmp_path, body):
    p = tmp_path / "blk.glsl"
    p.write_text("layout(std140) uniform B {\n" + body + "\n} u;\n", encoding="utf-8")
    return dict(ubo.parse_glsl_block(str(p), "B"))


def test_multiple_declarators_per_line_advance_offsets(tmp_path):
    # The terrain_tier1.frag shape that used to read only the first float per line.
    f = _glsl_offsets(tmp_path, "float a; float b; float c;  // off 0\nfloat d;")
    assert f == {"a": 0, "b": 4, "c": 8, "d": 12}


def test_comma_list_declarators(tmp_path):
    f = _glsl_offsets(tmp_path, "float a, b, c;\nvec4 v;")
    assert f == {"a": 0, "b": 4, "c": 8, "v": 16}


def test_comments_and_preprocessor_are_ignored(tmp_path):
    f = _glsl_offsets(tmp_path, "/* float ghost; */\n// float ghost2;\n#if 1\nint a;\n#endif\nint b;")
    assert f == {"a": 0, "b": 4}


def test_unknown_member_type_is_an_error_not_a_skip(tmp_path):
    with pytest.raises(ValueError):
        _glsl_offsets(tmp_path, "float a;\nmat3 m;")


def test_mismatch_exits_nonzero(tmp_path):
    glsl = tmp_path / "s.glsl"
    glsl.write_text("uniform B {\n float a; float b; float c;\n} u;\n", encoding="utf-8")
    cpp = tmp_path / "s.cpp"
    cpp.write_text("struct B {\n float a; // off 0\n float b; // off 4\n float c; // off 12\n};\n",
                   encoding="utf-8")
    code, lines = ubo.check_pair("t", str(cpp), "B", str(glsl), "B")
    assert code == 1 and any("c: GLSL(computed)=8 C++(stated)=12" in l for l in lines)


def test_live_registry_is_clean():
    # Run from the repo root: REGISTRY paths are repo-relative.
    cwd = os.getcwd()
    os.chdir(ROOT)
    try:
        for e in ubo.REGISTRY:
            code, lines = ubo.check_pair(*e)
            assert code == 0, "\n".join(lines)
    finally:
        os.chdir(cwd)
