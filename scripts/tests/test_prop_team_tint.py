"""The production prop shader uses the same paint slots as stock vehicles/mechs."""
from pathlib import Path
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child
import shader_common


@pytest.fixture(scope="module")
def tint_spirv(tmp_path_factory):
    src = ROOT / "shaders/vulkan/color_prop_tier1.frag"
    spv = tmp_path_factory.mktemp("team-tint") / "prop.spv"
    subprocess.run([shader_common.find_tool("glslangValidator"), "-V",
                    "-I" + str(src.parent), str(src), "-o", str(spv)],
                   check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([shader_common.find_tool("spirv-dis"), str(spv)],
                                   text=True, **console_child.child_kwargs(redirected=True))
    return re.search(r"%propTeamTint_\w+ = OpFunction .*?OpFunctionEnd", text, re.S)[0]


@pytest.mark.parametrize("channel,slot", [(0, "blue"), (1, "red"), (2, "green")])
def test_compiled_team_mask_channel_selects_stock_paint_slot(tint_spirv, channel, slot):
    # Authored R is base; mission_movers passes base as the third argument (psBlue).
    # Follow the compiled weight and paint loads into the multiply, rather than
    # duplicating the shader formula in a CPU model that could pass independently.
    body = tint_spirv
    weight_ptrs = re.findall(r"(%\w+) = OpAccessChain %\w+ %w(?:_\d+)? %uint_" + str(channel) + r"\b", body)
    assert weight_ptrs
    paint = re.search(r"(%\w+) = OpLoad %v3float %" + slot + r"\b", body)[1]
    weights = [re.search(r"(%\w+) = OpLoad %float " + re.escape(ptr) + r"\b", body)[1]
               for ptr in weight_ptrs]
    assert any(re.search(r"OpVectorTimesScalar %v3float " + re.escape(paint) + " " + re.escape(weight) + r"\b", body)
               for weight in weights)


def test_compiled_unmasked_palette_indexes_the_stock_team_slots(tint_spirv):
    # The maskless path must consume the dominant albedo channel's paint;
    # returning the authored blue, or indexing a constant slot, fails this check.
    body = tint_spirv
    pointer, channel = re.search(r"(%\w+) = OpAccessChain %\w+ %team (%\w+)\b", body).groups()
    assert re.search(re.escape(channel) + r" = OpLoad %int %channel\b", body)
    packed = re.search(r"(%\w+) = OpLoad %uint " + re.escape(pointer) + r"\b", body)[1]
    assert re.search(r"OpStore %packed(?:_\d+)? " + re.escape(packed) + r"\b", body)
