"""sward-ground-unify: ground and grass blades colour from ONE world patch field and ONE palette.

Static pins (no GPU): both shaders include shaders/vulkan/include/world_patch_noise.glsl and call
worldPatchField/worldPatchColor; the vegetation shader carries no palette or patch noise of its own;
the palette stays muted (owner ref 3: olive-tan, less saturated than the grass-ref3/ground-mottle tips).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VK = ROOT / "shaders" / "vulkan"
INCLUDE = VK / "include" / "world_patch_noise.glsl"


def _src(name):
    return (VK / name).read_text(encoding="utf-8")


def _palette():
    text = INCLUDE.read_text(encoding="utf-8")
    out = {}
    for name, body in re.findall(r"const vec3\s+(WPN_\w+)\s*=\s*vec3\(([^)]*)\)", text):
        out[name] = tuple(float(v) for v in body.split(","))
    return out


def test_both_shaders_use_the_shared_field():
    for name in ("terrain_tier1.frag", "vfx_vegetation.frag"):
        src = _src(name)
        assert '#include "include/world_patch_noise.glsl"' in src, name
        assert "worldPatchField(" in src, name
        assert "worldPatchColor(" in src, name


def test_vegetation_has_no_private_patch_palette():
    src = _src("vfx_vegetation.frag")
    for stale in ("worldSwardPatch", "vegPatchNoise", "kOlive", "kYellowGreen", "kStraw"):
        assert stale not in src, stale


def test_palette_is_muted_olive_tan():
    pal = _palette()
    assert set(pal) == {"WPN_OLIVE", "WPN_YELLOW_GREEN", "WPN_STRAW"}
    # Previous tips: ground olive sat 0.48, blade olive (0.27, 0.33, 0.14) sat 0.58.
    for name, (r, g, b) in pal.items():
        sat = (max(r, g, b) - min(r, g, b)) / max(r, g, b)
        assert sat <= 0.45, (name, sat)
        assert g - r <= 0.01, (name, "greener than ref 3")


def test_fine_speckle_octave_present():
    text = INCLUDE.read_text(encoding="utf-8")
    m = re.search(r"const float WPN_SPECK_WU\s*=\s*([\d.]+)", text)
    assert m, "fine in-patch speckle octave missing"
    assert 20.0 <= float(m.group(1)) <= 40.0  # 5-10 m at 4 wu per metre


def test_fringe_roles_preserve_the_capped_interior_survivor_list():
    # Both the frustum radius and projected-height test must treat every field
    # role as the original upright card, even when draw rejects that card.
    role = "(i.atlasFrame & 8u) != 0u ? 0u : (i.atlasFrame >> 4u) & 3u"
    for name in ("include/vegetation_cull_common.glsl", "include/vegetation_structs.glsl"):
        assert role in _src(name), name
    assert "if ((i.atlasFrame & 56u) == 40u) shrink = 0.0;" in _src("vfx_vegetation.vert")
    adapter = (ROOT / "GameAdapters/VegetationAdapter.cpp").read_text(encoding="utf-8")
    assert "inst.atlasFrame |= fieldEdgeRole(" in adapter
    assert "if (hashf(bx+71.0f,by-137.0f)>=edgeProbability) continue;" not in adapter
