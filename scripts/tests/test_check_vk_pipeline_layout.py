"""Tests for scripts/check-vk-pipeline-layout.py (needs the Vulkan SDK; skipped without)."""
import importlib.util
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, "fixtures", "vk_pipeline_layout")

spec = importlib.util.spec_from_file_location("vkpl", os.path.join(ROOT, "scripts", "check-vk-pipeline-layout.py"))
vkpl = importlib.util.module_from_spec(spec)
sys.modules["vkpl"] = vkpl
spec.loader.exec_module(vkpl)

SDK = os.environ.get("VULKAN_SDK") or vkpl.SDK_DEFAULT
if not vkpl.find_tool(SDK, "glslc") or not vkpl.find_tool(SDK, "spirv-cross"):
    pytest.skip("Vulkan SDK (glslc/spirv-cross) not available", allow_module_level=True)


@pytest.fixture(scope="module")
def env():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        res, _ = vkpl.compile_all(os.path.join(ROOT, "shaders", "vulkan"), SDK, tmp)
    shaders, errs = res
    return shaders, vkpl.build_symbols(ROOT, [])


def lint(env, name):
    shaders, sym = env
    path = os.path.join(FIX, name)
    out = []
    vkpl.check_file(path, vkpl.read(path), sym, shaders, out)
    return out


def rules(fs, sev=None):
    return {f.rule for f in fs if sev is None or f.sev == sev}


def test_pre_c26b1eb2a_shadow_stride_is_flagged(env):
    fs = lint(env, "bad_shadow_stride_fallback.cpp")
    assert "VK-STRIDE-FALLBACK" in rules(fs, "ERROR")
    # the resolved fallback (12) also disagrees with the shared VBO stride (40)
    assert "VK-STRIDE-VBO" in rules(fs, "ERROR")
    assert all(f.line > 0 for f in fs)


def test_fixed_shadow_stride_is_clean(env):
    assert lint(env, "good_shadow_stride.cpp") == []


def test_attribute_mismatches(env):
    fs = lint(env, "bad_attr_mismatch.cpp")
    r = rules(fs, "ERROR")
    assert {"VK-INPUT-MISSING", "VK-INPUT-TYPE", "VK-ATTR-OOB"} <= r
    msgs = " ".join(f.msg for f in fs)
    assert "location 3" in msgs and "location 6" in msgs


def test_descriptor_and_push_mismatches(env):
    fs = lint(env, "bad_descriptors_push.cpp")
    r = rules(fs, "ERROR")
    assert {"VK-DESC-TYPE", "VK-DESC-STAGE", "VK-PUSH-SIZE"} <= r


def test_current_tree_has_no_errors():
    assert vkpl.main(["--quiet"]) == 0


@pytest.mark.parametrize("extra", [
    "layout(set=0, binding=31) uniform sampler2D extraBinding;",
    "layout(location=1) in vec2 extraInput;",
    "layout(location=1) out vec4 extraOutput;",
    "layout(push_constant) uniform ExtraPush { vec4 value; } pc;",
])
def test_water_early_interface_mismatch_fails(tmp_path, capsys, extra):
    source = tmp_path / "water_geom.frag"
    source.write_text("#version 450\nlayout(location=0) out vec4 color;\n"
                      "#ifdef WATER_EARLY_TESTS\n" + extra + "\n#endif\n"
                      "void main() { color = vec4(1); }\n", encoding="utf-8")
    cpp = tmp_path / "empty.cpp"
    cpp.write_text("", encoding="utf-8")
    assert vkpl.main(["--shader-dir", str(tmp_path), "--cpp", str(cpp), "--quiet"]) == 1
    assert "WATER_EARLY_TESTS interface/layout differs" in capsys.readouterr().out
    source.write_text("#version 450\nlayout(location=0) out vec4 color;\n"
                      "void main() { color = vec4(1); }\n", encoding="utf-8")
    assert vkpl.main(["--shader-dir", str(tmp_path), "--cpp", str(cpp), "--quiet"]) == 0


def test_resolver_accessor_and_sizeof(env):
    _, sym = env
    assert sym.eval("mc2_static_prop_vertex_stride()", "x") == 40
    assert sym.eval("mc2_mech_vertex_stride()", "x") == 40
    assert sym.eval("sizeof(float) * 3", "x") == 12


def lint_unit(env, dirname):
    shaders, sym = env
    d = os.path.join(FIX, dirname)
    pieces = [os.path.join(d, n) for n in ("unitfix_layout.cpp", "unitfix_pipeline.cpp")]
    assert vkpl.split_unit_key(pieces[0]) == vkpl.split_unit_key(pieces[1]) is not None
    out = []
    vkpl.check_unit(pieces, sym, shaders, out)
    return out


def test_split_unit_resolves_layout_across_pieces(env):
    # The layout is built in one piece and used by the pipeline in the next: no VK-UNRESOLVED, no errors.
    assert lint_unit(env, "split_unit_good") == []


def test_split_unit_errors_are_reported_against_the_piece(env):
    fs = lint_unit(env, "split_unit_bad")
    assert {"VK-DESC-TYPE", "VK-DESC-STAGE", "VK-PUSH-SIZE"} <= rules(fs, "ERROR")
    assert all(os.path.basename(f.path) == "unitfix_layout.cpp" for f in fs if f.rule.startswith(("VK-DESC", "VK-PUSH")))
    assert all(f.line > 0 for f in fs)


def test_pieces_checked_alone_cannot_see_the_layout(env):
    shaders, sym = env
    path = os.path.join(FIX, "split_unit_good", "unitfix_pipeline.cpp")
    out = []
    vkpl.check_file(path, vkpl.read(path), sym, shaders, out)
    assert "VK-PUSH-MISSING" in rules(out, "ERROR")   # the push range and layout sit in the other piece


def test_indexed_vfx_group_layout_and_page_array(tmp_path):
    """Reflect the production variants, including the new 32-byte SSBO record."""
    import json
    import subprocess
    glslc = vkpl.find_tool(SDK, "glslc")
    cross = vkpl.find_tool(SDK, "spirv-cross")
    reflected = {}
    for stage in ("vert", "frag"):
        spv = str(tmp_path / f"vfx_indexed.{stage}.spv")
        source = os.path.join(ROOT, "shaders", "vulkan", f"vfx_particle.{stage}")
        subprocess.run([glslc, "--target-env=vulkan1.2", "-DVFX_INDEXED=1", source, "-o", spv],
                       check=True, capture_output=True, **vkpl.console_child.child_kwargs(redirected=True))
        reflected[stage] = json.loads(subprocess.check_output([cross, spv, "--reflect"],
                                     **vkpl.console_child.child_kwargs(redirected=True)))
    vert, frag = reflected["vert"], reflected["frag"]
    group = next(t for t in vert["types"].values() if t.get("name") == "Group")
    # Member order mirrors VfxIndexedGroup in vulkan_vfx_island.cpp (std430 packs these eight 4-byte words into 32 bytes).
    assert [m["name"] for m in group["members"]] == ["uvOffset", "uvSize", "atlasColumns", "blendMode", "page", "blend"]
    groups = next(t for t in vert["types"].values() if t.get("name") == "Groups")
    assert groups["members"][0]["array_stride"] == 32
    metadata = next(b for b in vert["ssbos"] if b["name"] == "Groups")
    assert (metadata["set"], metadata["binding"]) == (1, 1)
    pages = next(b for b in frag["textures"] if b["name"] == "uPages")
    assert (pages["set"], pages["binding"], pages["array"]) == (1, 0, [64])
