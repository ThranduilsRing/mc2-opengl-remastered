"""Regression tests for the tools/ offline provers wired in by D1/D2:
tools/check_ubo_layout.py (std140 parser) and tools/glsl_port_diff.py (rename map +
pinned-debt allowlist). Each test injects the defect the tool exists to catch and
proves it still fires.
"""
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
gpd = _load("glsl_port_diff")


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


# --- glsl_port_diff ---------------------------------------------------------

def _port_files(tmp_path, gl_body, vk_body):
    gl = tmp_path / "gl.frag"
    vk = tmp_path / "vk.frag"
    gl.write_text("//PORT-BEGIN:t\n" + gl_body + "\n//PORT-END:t\n", encoding="utf-8")
    vk.write_text("//PORT-BEGIN:t\n" + vk_body + "\n//PORT-END:t\n", encoding="utf-8")
    return str(gl), str(vk)


def test_rename_map_folds_vk_spelling_onto_gl(tmp_path):
    gl, vk = _port_files(tmp_path, "float x = u_skirtDepth * 2.0;", "float x = uT.skirtDepth * 2.0;")
    assert gpd.diff_block(gpd.Port("t", gl, vk))[0] == 1          # without the map: diverges
    assert gpd.diff_block(gpd.Port("t", gl, vk, rename=[gpd.UT_TO_U]))[0] == 0


def test_rename_map_does_not_hide_arithmetic(tmp_path):
    gl, vk = _port_files(tmp_path, "float x = u_skirtDepth * 2.0;", "float x = uT.skirtDepth * 3.0;")
    assert gpd.diff_block(gpd.Port("t", gl, vk, rename=[gpd.UT_TO_U]))[0] == 1


def test_call_wrapping_is_not_a_divergence(tmp_path):
    gl, vk = _port_files(tmp_path, "f(a,\n  b, c);", "f(a, b,\n  c);")
    assert gpd.diff_block(gpd.Port("t", gl, vk))[0] == 0


def test_missing_markers_exit_2(tmp_path):
    gl = tmp_path / "gl.frag"
    gl.write_text("float x;\n", encoding="utf-8")
    assert gpd.diff_block(gpd.Port("t", str(gl), str(gl)))[0] == 2


def _run_registry_with(monkeypatch, tmp_path, ports, allow_text):
    al = tmp_path / "al.txt"
    al.write_text(allow_text, encoding="utf-8")
    monkeypatch.setattr(gpd, "REGISTRY", ports)
    monkeypatch.setattr(gpd, "ALLOWLIST", str(al))
    return gpd.run_registry(False)


def test_pinned_debt_passes_stale_and_drift_fail(tmp_path, monkeypatch, capsys):
    gl, vk = _port_files(tmp_path, "a();\nb();", "a();")
    port = gpd.Port("t", gl, vk)
    h = gpd.diff_block(port)[2]
    # pinned and unchanged: green
    assert _run_registry_with(monkeypatch, tmp_path, [port], f"t\t{vk}\t{h}\tgap\n") == 0
    # no entry: red
    assert _run_registry_with(monkeypatch, tmp_path, [port], "") == 1
    # entry pins a different diff (the debt moved): red
    assert _run_registry_with(monkeypatch, tmp_path, [port], f"t\t{vk}\t000000000000\tgap\n") == 1
    # port became verbatim but the entry remains: stale, red
    gl2, vk2 = _port_files(tmp_path, "a();", "a();")
    port2 = gpd.Port("t", gl2, vk2)
    assert _run_registry_with(monkeypatch, tmp_path, [port2], f"t\t{vk2}\t{h}\tgap\n") == 1
    assert "STALE-ALLOWLIST" in capsys.readouterr().out


def test_live_registry_is_green():
    cwd = os.getcwd()
    os.chdir(ROOT)
    try:
        assert gpd.run_registry(False) == 0
    finally:
        os.chdir(cwd)
