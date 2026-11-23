"""Compile the production HDR UI shaders and pin the supported blend inventory."""
from pathlib import Path
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child
import shader_common


@pytest.mark.parametrize("shader", ["present_encode", "hud_color", "hud_tex"])
def test_hdr_ui_real_shader(tmp_path, shader):
    spv = str(tmp_path / (shader + ".spv"))
    for args in ([shader_common.find_tool("glslangValidator"), "-V",
                  str(ROOT / f"shaders/vulkan/{shader}.frag"), "-o", spv],
                 [shader_common.find_tool("spirv-val"), spv]):
        subprocess.run(args, check=True, **console_child.child_kwargs(redirected=True))


def test_hdr_ui_blend_inventory():
    text = (ROOT / "GameOS/gameos/engine_present_vk.cpp").read_text()
    # Reject newly added pipelines until their blend equation is covered.
    calls = re.findall(r"!make(?:Tex)?Pipe\((.*?)\)", text, re.S)
    actual = {}
    for call in calls:
        fields = [f.strip() for f in call.split(",")]
        if fields[0].startswith("VK_PRIMITIVE_TOPOLOGY_"):
            fields.pop(0)
        blend, src, name, *dst = fields
        actual[name] = (blend, src, dst[0] if dst else "VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA")
    expected = {}
    for stem in ("hudColorTri", "hudColorLine", "hudTex"):
        expected[f"s.{stem}Pipe"] = ("true", "VK_BLEND_FACTOR_SRC_ALPHA", "VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA")
        expected[f"s.{stem}OpaquePipe"] = ("false", "VK_BLEND_FACTOR_SRC_ALPHA", "VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA")
    for stem in ("hudColorTri", "hudTex"):
        expected[f"s.{stem}OneInvAlphaPipe"] = ("true", "VK_BLEND_FACTOR_ONE", "VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA")
        expected[f"s.{stem}OneOnePipe"] = ("true", "VK_BLEND_FACTOR_ONE", "VK_BLEND_FACTOR_ONE")
    assert actual == expected
    compact = re.sub(r"\s+", "", text)
    assert compact.count("cba.srcAlphaBlendFactor=(s.hdr&&dstColor==VK_BLEND_FACTOR_ONE)?VK_BLEND_FACTOR_ZERO:VK_BLEND_FACTOR_ONE;") == 2
    assert compact.count("cba.dstAlphaBlendFactor=(dstColor==VK_BLEND_FACTOR_ONE)?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;") == 2
    assert compact.count("constVkBool32hdrOpaque=s.hdr&&!blend;") == 2
    assert compact.count("st[1].pSpecializationInfo=&opaqueSpec;") == 2
    for assignment in ("blendEnable=blend?VK_TRUE:VK_FALSE", "srcColorBlendFactor=srcColor",
                       "dstColorBlendFactor=dstColor", "colorBlendOp=VK_BLEND_OP_ADD",
                       "alphaBlendOp=VK_BLEND_OP_ADD"):
        assert compact.count("cba." + assignment + ";") == 2
    for shader in ("hud_color", "hud_tex"):
        src = (ROOT / f"shaders/vulkan/{shader}.frag").read_text()
        assert "const bool HDR_OPAQUE = false;" in src
        assert "if (HDR_OPAQUE) outColor.a = 1.0;" in src
    imgui = (ROOT / "3rdparty/imgui/backends/imgui_impl_vulkan.cpp").read_text()
    for field, factor in (("srcColor", "SRC_ALPHA"), ("dstColor", "ONE_MINUS_SRC_ALPHA"),
                          ("srcAlpha", "ONE"), ("dstAlpha", "ONE_MINUS_SRC_ALPHA")):
        assert re.findall(rf"color_attachment\[0\]\.{field}BlendFactor = (\w+);", imgui) == ["VK_BLEND_FACTOR_" + factor]
    for field in ("colorBlendOp", "alphaBlendOp"):
        assert re.findall(rf"color_attachment\[0\]\.{field} = (\w+);", imgui) == ["VK_BLEND_OP_ADD"]
    encode = (ROOT / "shaders/vulkan/present_encode.frag").read_text()
    assert encode.count("hdrUiComposite(scene.") == 3
