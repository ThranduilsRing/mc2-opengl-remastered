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


# --- std140 uniform blocks --------------------------------------------------

BLOCK_SRC = """\
// @gpu std140 glsl=Blk block=uB set=1 binding=3 slot=256
struct Blk {
    float m[16];        // @mat4
    int32_t a;
    float   rgb[3];     // @rgb
    float   after;
    float   cascades[32];  // @mat4xN
    float   v[4];       // @vec4
    int32_t _pad[4];    // @cpp_only
};
"""


def _build_block(text=BLOCK_SRC):
    info = g.parse_source(text, "t")
    return info, g.build(info, "t.h", "t.glsl")


def test_block_packs_rgb_scalars_without_vec3_padding():
    info, _ = _build_block()
    offs_, _, _ = g.struct_offsets(info["fields"], "std140")
    by = dict(offs_)
    assert [by["a"], by["rgbR"], by["rgbG"], by["rgbB"], by["after"]] == [64, 68, 72, 76, 80]
    assert by["cascades"] == 96    # mat4 array re-aligns to 16
    assert by["v"] == 96 + 2 * 64


def test_block_emits_uniform_block_and_cpp_asserts():
    _, out = _build_block()
    assert "layout(set = 1, binding = 3, std140) uniform Blk {" in out["glsl"]
    assert "} uB;" in out["glsl"]
    assert "    mat4  cascades[2];  // off 96" in out["glsl"]
    assert "    float rgbG;  // off 72" in out["glsl"]
    assert "_pad" not in out["glsl"]
    lay = out["layout"]
    assert "sizeof(Blk) == 256" in lay
    assert "offsetof(Blk, rgb) == 68" in lay          # C++ name maps to the first scalar
    assert "offsetof(Blk, _pad) == 240" in lay        # tail pad starts at the std140 end
    assert "alignof" not in lay                       # C++ alignment is not the std140 one


@pytest.mark.parametrize("old,new", [
    ("// @gpu std140 glsl=Blk block=uB set=1 binding=3 slot=256",
     "// @gpu std430 glsl=Blk block=uB set=1 binding=3"),                   # block needs std140
    ("// @gpu std140 glsl=Blk block=uB set=1 binding=3 slot=256",
     "// @gpu std140 glsl=Blk block=uB binding=3 slot=256"),                # missing set
    ("// @gpu std140 glsl=Blk block=uB set=1 binding=3 slot=256",
     "// @gpu std140 glsl=Blk block=uB set=1 binding=3 slot=16"),           # slot < std140 size
    ("float   rgb[3];     // @rgb", "float   rgb[4];     // @rgb"),
    ("float   cascades[32];  // @mat4xN", "float   cascades[30];  // @mat4xN"),
    ("int32_t _pad[4];    // @cpp_only",
     "int32_t _pad[4];    // @cpp_only\n    int32_t _pad2[4];   // @cpp_only"),
])
def test_block_errors(old, new):
    assert old in BLOCK_SRC
    with pytest.raises(g.AbiError):
        _build_block(BLOCK_SRC.replace(old, new))


def test_block_annotations_rejected_in_plain_structs():
    text = "// @gpu std430 glsl=S\nstruct S {\n    float c[3];   // @rgb\n};\n"
    with pytest.raises(g.AbiError):
        g.parse_source(text, "t")


def test_terrain_ubo_is_registered_with_the_1024_byte_slot():
    entry = [s for s in g.STRUCTS if s[0].endswith("GpuTerrainFrameUBO.h")]
    assert entry, "TerrainFrameUBO must be a gpu_abi struct"
    with open(os.path.join(ROOT, entry[0][0]), encoding="utf-8") as f:
        info = g.parse_source(f.read(), "terrain")
    _, _, size = g.struct_offsets(info["fields"], "std140")
    assert info["slot"] == 1024 and size <= 1024


@pytest.mark.parametrize("idx", range(len(g.STRUCTS)))
def test_check_detects_drift_in_every_registered_output(tmp_path, idx):
    root = _tmp_root(tmp_path)
    for rel in g.STRUCTS[idx][1:]:
        with open(os.path.join(root, rel), "a", encoding="utf-8", newline="") as f:
            f.write("// tampered\n")
    assert g.main(["--check", "--root", root]) == 1


def test_mech_instance_record_is_64_bytes_with_pinned_offsets():
    entry = [s for s in g.STRUCTS if s[0].endswith("GpuMechInstance.h")]
    assert entry, "GpuMechInstance must be a gpu_abi struct"
    with open(os.path.join(ROOT, entry[0][0]), encoding="utf-8") as f:
        info = g.parse_source(f.read(), "mech")
    o, al, sz = g.struct_offsets(info["fields"], "std430")
    assert (al, sz) == (16, 64)
    assert [x[1] for x in o] == [0, 4, 8, 12, 16, 32, 48, 52, 56, 60]


def test_shader_redeclaring_a_generated_struct_is_rejected(tmp_path):
    root = _tmp_root(tmp_path)
    shader = tmp_path / "shaders" / "shadow_mech.vert"
    shader.parent.mkdir(parents=True, exist_ok=True)
    # The historical bug: a private 48-byte copy of the 64-byte record.
    shader.write_text("struct GpuMechInstance { uint a; vec4 b; vec4 c; };\n", encoding="utf-8")
    assert g.main(["--check", "--root", root]) == 1


def test_shader_including_the_generated_struct_passes(tmp_path):
    root = _tmp_root(tmp_path)
    shader = tmp_path / "shaders" / "shadow_mech.vert"
    shader.parent.mkdir(parents=True, exist_ok=True)
    shader.write_text("#include <include/gpu_abi_mech_instance.gen.hglsl>\n"
                      "// struct GpuMechInstance { stale comment }\n", encoding="utf-8")
    assert g.main(["--check", "--root", root]) == 0
