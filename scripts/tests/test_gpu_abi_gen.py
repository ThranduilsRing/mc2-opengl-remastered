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
    for shader, _, _ in g.MIRRORS:
        dst = tmp_path / shader
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, shader), dst)
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


def test_block_emits_array_length_constants():
    _, out = _build_block()
    assert "#define GPU_ABI_BLK_CASCADES_LEN 2" in out["glsl"]
    assert "GPU_ABI_BLK_M_LEN" not in out["glsl"]   # mat4 / vec4 members are not arrays


def test_terrain_cascade_bound_derives_from_the_generated_array_length():
    path = os.path.join(ROOT, "shaders", "vulkan", "include", "terrain_frame_vk.glsl")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    assert "#define MC2_VK_SHADOW_CSM_MAX GPU_ABI_TERRAINFRAMEUBO_CSMCASCADEMATRICES_LEN" in text


# --- arrays of mat4/vec4, GLSL renames, ivec4 tails, raw-shader mirrors ----------

ARR_SRC = """\
constexpr uint32_t kN = 4;

// @gpu std430 glsl=Lights cpp_align=any
struct Rec {
    float   toWorld[kN][16];   // @mat4xN glsl=to_world
    float   dirs[kN][4];       // @vec4xN glsl=dirs_out; w = type
    int32_t count_;            // @ivec4tail glsl=count
    int32_t pad[3];            // @fold
};
"""


def _build_arr(text=ARR_SRC):
    info = g.parse_source(text, "t")
    return info, g.build(info, "t.h", "t.glsl")


def test_array_members_resolve_named_constants_and_rename_for_glsl():
    info, out = _build_arr()
    offs_, al, sz = g.struct_offsets(info["fields"], "std430")
    assert [n for n, _ in offs_] == ["to_world", "dirs_out", "count"]
    assert [o for _, o in offs_] == [0, 256, 320] and (al, sz) == (16, 336)
    assert "    mat4  to_world[4];  // off 0" in out["glsl"]
    assert "    ivec4 count;  // off 320" in out["glsl"]
    assert "#define GPU_ABI_LIGHTS_TO_WORLD_LEN 4" in out["glsl"]


def test_ivec4_tail_pad_lanes_get_their_own_cpp_offsets():
    _, out = _build_arr()
    lay = out["layout"]
    assert "offsetof(Rec, count_) == 320" in lay
    assert "offsetof(Rec, pad) == 324" in lay          # pad[3] is lanes 1..3 of the ivec4
    assert "sizeof(Rec) == 336" in lay
    assert "alignof" not in lay                         # cpp_align=any


@pytest.mark.parametrize("old,new", [
    ("float   toWorld[kN][16];   // @mat4xN glsl=to_world",
     "float   toWorld[kN][12];   // @mat4xN glsl=to_world"),
    ("float   dirs[kN][4];       // @vec4xN glsl=dirs_out; w = type",
     "float   dirs[kN][4];       // glsl=dirs_out"),                      # 2-D without annotation
    ("float   toWorld[kN][16];", "float   toWorld[kUnknown][16];"),
    ("int32_t pad[3];            // @fold", "int32_t pad[2];            // @fold"),
    ("int32_t count_;            // @ivec4tail glsl=count",
     "float   count_;            // @ivec4tail glsl=count"),
])
def test_array_annotation_errors(old, new):
    assert old in ARR_SRC
    with pytest.raises(g.AbiError):
        _build_arr(ARR_SRC.replace(old, new))


def test_object_lights_record_matches_the_historical_abi():
    entry = [s for s in g.STRUCTS if s[0].endswith("GpuObjectLights.h")]
    assert entry, "GpuObjectLights must be a gpu_abi struct"
    with open(os.path.join(ROOT, entry[0][0]), encoding="utf-8") as f:
        info = g.parse_source(f.read(), "lights")
    o, al, sz = g.struct_offsets(info["fields"], "std430")
    assert sz == 3600                                  # N*112 + 16 at N=32
    assert [x[1] for x in o] == [0, 2048, 2560, 3072, 3584]
    assert [x[0] for x in o] == ["light_to_world", "light_dir", "light_color",
                                 "light_falloff", "numLights"]


def test_raw_shader_mirror_drift_is_rejected(tmp_path):
    root = _tmp_root(tmp_path)
    comp = tmp_path / "shaders" / "lightgrid_build.comp"
    assert g.main(["--check", "--root", root]) == 0
    text = comp.read_text(encoding="utf-8")
    comp.write_text(text.replace("#define LIGHTGRID_MAX_LIGHTS 32", "#define LIGHTGRID_MAX_LIGHTS 16"),
                    encoding="utf-8")
    assert g.main(["--check", "--root", root]) == 1      # stale array length in the mirror


def test_ivec4_annotation_needs_int32_quad():
    src = ("// @gpu std140 glsl=B block=u set=0 binding=0\nstruct B {\n"
           "    int32_t q[4];  // @ivec4\n};\n")
    info = g.parse_source(src, "t")
    assert info["fields"] == [("q", "ivec4")]
    with pytest.raises(g.AbiError):
        g.parse_source(src.replace("int32_t", "float"), "t")


@pytest.mark.parametrize("header,size,block", [
    ("GpuColorFrameUBO.h", 288, "ColorFrameUBO"),   # 256 until the night state, the ambient fill strength, the specular strength and the eye position joined the block
    ("GpuMechFrameUBO.h", 208, "MechFrameUBO"),
    ("GpuLegacyFrameUBO.h", 80, "LegacyFrameUBO"),
])
def test_vk_frame_ubos_are_registered_with_their_historical_sizes(header, size, block):
    entry = [s for s in g.STRUCTS if s[0].endswith(header)]
    assert entry, "%s must be a gpu_abi struct" % block
    with open(os.path.join(ROOT, entry[0][0]), encoding="utf-8") as f:
        info = g.parse_source(f.read(), block)
    _, _, sz = g.struct_offsets(info["fields"], "std140")
    assert sz == size and info["block"] == "uFrame"
