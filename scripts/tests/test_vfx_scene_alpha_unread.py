"""The VFX premultiplied fold (vfx_particle.frag foldBlend) leaves the scene colour's alpha unequal to the old per-mode blends.
That is only valid while nothing reads scene alpha. This test fails when a destination-alpha consumer appears.

Audit at the fold (consumer, file:line at the time):
  - blend factors: the only DST_ALPHA factor is vulkan_post_screenfx_native.cpp srcAlphaBlendFactor (cloud/shoreline multiply,
    alpha channel result only; its colour factors are DST_COLOR/ZERO).
  - post-process / AA / grade shaders (postprocess.frag, postprocess_visual.frag, aa_*.frag) sample sceneTex as .rgb and write alpha 1.0.
  - HUD, present and capture composite the post-process output, whose alpha is the constant 1.0 above.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALPHA_ONLY_EXEMPT = {"vulkan_post_screenfx_native.cpp"}   # srcAlphaBlendFactor only; asserted below


def _sources():
    for path in (ROOT / "GameOS").rglob("*.cpp"):
        yield path
    for path in (ROOT / "RenderCore").rglob("*.cpp"):
        yield path


def test_no_destination_alpha_colour_factor():
    offenders = []
    for path in _sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if not re.search(r"BLEND_FACTOR_(ONE_MINUS_)?DST_ALPHA", line):
                continue
            alpha_only = re.search(r"\b(src|dst)AlphaBlendFactor\b", line) and "ColorBlendFactor" not in line
            if path.name in ALPHA_ONLY_EXEMPT and alpha_only:
                continue
            offenders.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")
    assert not offenders, "a dst-alpha consumer appeared; the VFX fold must keep strict RGBA again:\n" + "\n".join(offenders)


def test_screenfx_dst_alpha_stays_alpha_channel_only():
    text = (ROOT / "GameOS/gameos/vulkan_post_screenfx_native.cpp").read_text(encoding="utf-8", errors="replace")
    assert "srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR" in text
    assert "srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA" in text


def test_post_shaders_read_scene_rgb_only():
    offenders = []
    for name in ("postprocess.frag", "postprocess_visual.frag", "aa_fxaa.frag", "aa_smaa_blend.frag"):
        text = (ROOT / "shaders/vulkan" / name).read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"texture\(\s*sceneTex", text):
            depth, i = 0, match.start() + len("texture")
            while True:
                depth += text[i] == "("
                depth -= text[i] == ")"
                i += 1
                if depth == 0:
                    break
            if not text.startswith(".rgb", i):
                offenders.append(f"{name}: {text[match.start():i + 6]}")
    assert not offenders, "\n".join(offenders)
