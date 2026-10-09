"""Golden std140/std430 layout vectors for tools/gpu_abi_gen.py, plus drift tests.

Vectors are the GLSL spec rules (OpenGL 4.6 sec 7.6.2.2), written out by hand so
the generator's layout engine is checked against something other than itself.
"""
import importlib.util
import os
import shutil
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location(
    "gpu_abi_gen", os.path.join(ROOT, "tools", "gpu_abi_gen.py"))
g = importlib.util.module_from_spec(_spec)
sys.modules["gpu_abi_gen"] = g
_spec.loader.exec_module(g)


def offs(fields, rules):
    o, al, sz = g.struct_offsets(fields, rules)
    return [x[1] for x in o], al, sz


# (id, fields, rules, expected offsets, expected align, expected size)
INNER_F = ("struct", [("a", "float")])
INNER_V2F = ("struct", [("a", "vec2"), ("b", "float")])
INNER_V3F = ("struct", [("v", "vec3"), ("f", "float")])
VECTORS = [
    # vec3 is 12 bytes but 16-aligned; a following float packs at +12 in both.
    ("vec3_then_float", [("v", "vec3"), ("f", "float")], "std430", [0, 12], 16, 16),
    ("vec3_then_float_140", [("v", "vec3"), ("f", "float")], "std140", [0, 12], 16, 16),
    ("vec3_then_vec3", [("a", "vec3"), ("b", "vec3")], "std430", [0, 16], 16, 32),
    ("float_then_vec3", [("f", "float"), ("v", "vec3")], "std430", [0, 16], 16, 32),
    ("float_vec2_pad", [("f", "float"), ("v", "vec2")], "std430", [0, 8], 8, 16),
    # scalar arrays: stride 4 in std430, 16 in std140.
    ("float4_arr_430", [("a", ("arr", "float", 4))], "std430", [0], 4, 16),
    ("float4_arr_140", [("a", ("arr", "float", 4))], "std140", [0], 16, 64),
    ("float_arr_then_float_140", [("a", ("arr", "float", 2)), ("b", "float")], "std140", [0, 32], 16, 48),
    ("vec2_arr_140", [("a", ("arr", "vec2", 3))], "std140", [0], 16, 48),
    ("vec3_arr_430", [("a", ("arr", "vec3", 2))], "std430", [0], 16, 32),
    # matrices are arrays of column vectors.
    ("mat4", [("m", "mat4")], "std140", [0], 16, 64),
    ("mat4_430", [("m", "mat4")], "std430", [0], 16, 64),
    ("mat3_140", [("m", "mat3")], "std140", [0], 16, 48),
    ("mat3_then_float", [("m", "mat3"), ("f", "float")], "std430", [0, 48], 16, 64),
    ("mat2_430", [("m", "mat2")], "std430", [0], 8, 16),
    ("mat2_140", [("m", "mat2")], "std140", [0], 16, 32),
    ("bool_is_4", [("b", "bool"), ("c", "bool")], "std430", [0, 4], 4, 8),
    # nested struct alignment: std140 rounds struct align/size to 16, std430 does not.
    ("nested_140", [("s", INNER_F), ("f", "float")], "std140", [0, 16], 16, 32),
    ("nested_430", [("s", INNER_F), ("f", "float")], "std430", [0, 4], 4, 8),
    ("nested_vec2_430", [("f", "float"), ("s", INNER_V2F)], "std430", [0, 8], 8, 24),
    ("nested_vec2_140", [("f", "float"), ("s", INNER_V2F)], "std140", [0, 16], 16, 32),
    ("struct_arr_430", [("a", ("arr", INNER_V3F, 3))], "std430", [0], 16, 48),
    ("struct_arr_140", [("a", ("arr", INNER_F, 3))], "std140", [0], 16, 48),
    # the production record.
    ("static_prop_instance",
     [("modelMatrix", "mat4"), ("typeID", "uint"), ("firstColorOffset", "uint"),
      ("flags", "uint"), ("lightDataIndex", "uint"), ("aRGBHighlight", "vec4"),
      ("fogRGB", "vec4")], "std430", [0, 64, 68, 72, 76, 80, 96], 16, 112),
]


@pytest.mark.parametrize("vid,fields,rules,eo,ea,es", VECTORS, ids=[v[0] for v in VECTORS])
def test_golden_layout(vid, fields, rules, eo, ea, es):
    assert offs(fields, rules) == (eo, ea, es)


def test_unknown_type_is_an_error():
    with pytest.raises(g.AbiError):
        g.align_size("sampler2D", "std430")


def test_committed_outputs_match_sources():
    assert g.main(["--check"]) == 0


def _tmp_root(tmp_path):
    for src, lay, gl in g.STRUCTS:
        for rel in (src, lay, gl):
            dst = tmp_path / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(os.path.join(ROOT, rel), dst)
    return str(tmp_path)


def test_check_detects_drift_in_generated_file(tmp_path):
    root = _tmp_root(tmp_path)
    gl = os.path.join(root, g.STRUCTS[0][2])
    with open(gl, "a", encoding="utf-8", newline="") as f:
        f.write("// tampered\n")
    assert g.main(["--check", "--root", root]) == 1


def test_check_detects_source_layout_change(tmp_path):
    root = _tmp_root(tmp_path)
    src = os.path.join(root, g.STRUCTS[0][0])
    with open(src, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    # Insert a field: every later offset moves, so committed outputs are stale.
    text = text.replace("    uint32_t typeID;\n", "    uint32_t typeID;\n    uint32_t extra;\n", 1)
    with open(src, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    assert g.main(["--check", "--root", root]) == 1


def test_alignas_mismatch_is_rejected(tmp_path):
    root = _tmp_root(tmp_path)
    src = os.path.join(root, g.STRUCTS[0][0])
    with open(src, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    with open(src, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("alignas(16)", "alignas(8)", 1))
    assert g.main(["--check", "--root", root]) == 2
