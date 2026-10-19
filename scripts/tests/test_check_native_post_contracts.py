"""Native post blend/mask guards reject real pipeline regressions."""
import importlib.util
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def test_multiply_rejects_wrong_factors_and_comment_canaries():
    checker = load("check-multiply-blend")
    text = (ROOT / "GameOS/gameos/vulkan_post_screenfx_native.cpp").read_text(encoding="utf-8")
    assert not checker.validate_native_multiply(text)
    assignment = "cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;"
    broken = text.replace(assignment, "// " + assignment + "\n cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;")
    assert checker.validate_native_multiply(broken)

def test_color_mask_rejects_missing_channel_and_extra_attachment():
    checker = load("check-colormask-ownership")
    text = (ROOT / "GameOS/gameos/vulkan_post_fog_native.cpp").read_text(encoding="utf-8")
    assert not checker.validate_native_color_mask(text)
    assert checker.validate_native_color_mask(text.replace("VK_COLOR_COMPONENT_B_BIT |", ""))
    assert checker.validate_native_color_mask(text.replace("cb.attachmentCount = 1;", "cb.attachmentCount = 2;"))
