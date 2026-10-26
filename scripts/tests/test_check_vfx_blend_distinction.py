"""The visible Vulkan VFX path must keep straight-alpha and ONE/ONE distinct."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("vfx_blend", ROOT / "scripts/check-vfx-blend-distinction.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_native_blend_contract_and_one_one_collapse():
    source = (ROOT / "GameOS/gameos/vulkan_vfx_island.cpp").read_text(encoding="utf-8")
    assert not checker.native_blend_issues(source)
    collapsed = source.replace("oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA", "VK_BLEND_FACTOR_SRC_ALPHA")
    assert checker.native_blend_issues(collapsed)


def test_a_comment_cannot_certify_missing_native_blend_state():
    source = (ROOT / "GameOS/gameos/vulkan_vfx_island.cpp").read_text(encoding="utf-8")
    assert checker.native_blend_issues("/*" + source + "*/")


def test_tube_source_one_cannot_inherit_source_alpha():
    source = (ROOT / "GameOS/gameos/vulkan_vfx_island.cpp").read_text(encoding="utf-8")
    assert checker.native_blend_issues(source.replace(
        "tubeMode == 4 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;", ""))
