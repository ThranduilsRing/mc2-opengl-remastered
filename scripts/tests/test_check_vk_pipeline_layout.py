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


def test_resolver_accessor_and_sizeof(env):
    _, sym = env
    assert sym.eval("mc2_static_prop_vertex_stride()", "x") == 40
    assert sym.eval("mc2_mech_vertex_stride()", "x") == 48
    assert sym.eval("sizeof(float) * 3", "x") == 12
